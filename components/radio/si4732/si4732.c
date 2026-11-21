#include "si4732.h"

#include <string.h>

#include "board.h"
#include "board_i2c.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssb_patch_full.h"

static const char *TAG = "SI4732";

/* ============================ AN332 命令字 ============================ */
#define CMD_POWER_UP            0x01
#define CMD_GET_REV             0x10
#define CMD_POWER_DOWN          0x11
#define CMD_SET_PROPERTY        0x12
#define CMD_GET_PROPERTY        0x13
#define CMD_GET_INT_STATUS      0x14

#define CMD_FM_TUNE_FREQ        0x20
#define CMD_FM_SEEK_START       0x21
#define CMD_FM_TUNE_STATUS      0x22

#define CMD_AM_TUNE_FREQ        0x40
#define CMD_AM_SEEK_START       0x41
#define CMD_AM_TUNE_STATUS      0x42

/* ============================ 属性号 ============================ */
#define PROP_SSB_BFO            0x0100
#define PROP_SSB_MODE           0x0101
#define PROP_SSB_SOFT_MUTE_MAX_ATTENUATION 0x3302
#define PROP_REFCLK_FREQ        0x0201
#define PROP_REFCLK_PRESCALE    0x0202
#define PROP_FM_SEEK_BAND_BOTTOM 0x1400
#define PROP_FM_SEEK_BAND_TOP    0x1401
#define PROP_FM_SEEK_FREQ_SPACING 0x1402
#define PROP_AM_SEEK_BAND_BOTTOM 0x3400
#define PROP_AM_SEEK_BAND_TOP    0x3401
#define PROP_AM_SEEK_FREQ_SPACING 0x3402
#define PROP_RX_VOLUME          0x4000
#define PROP_RX_HARD_MUTE       0x4001

/* FM 波段属性使用 10kHz 单位，AM 使用 kHz 单位 */
#define FM_PROP_UNIT_KHZ        10

/* ============================ 状态位 ============================ */
#define STATUS_CTS              0x80    /* 命令可以接收 */
#define STATUS_ERR              0x40    /* 上一条命令出错 */
#define TUNE_STATUS_VALID       0x01    /* 调谐/搜台结果有效 */

/* ============================ 时序参数 ============================ */
#define SI4732_I2C_CLK_HZ           400000  /* 与总线一致，400kHz（需 4.7k 上拉） */
#define SI4732_XFER_TIMEOUT_MS      50      /* 单次 I2C 传输超时 */
#define SI4732_CTS_TIMEOUT_MS       1000    /* 等待 CTS 的超时（上电约需 110ms） */
#define SI4732_TUNE_TIMEOUT_MS      2000    /* 等待调谐完成超时 */
#define SI4732_POWERUP_DELAY_MS     110     /* AN332 建议的上电稳定时间 */

/* SSB 补丁参数 */
#define SI4732_PATCH_LINE_BYTES     8       /* 每行：命令字 + 7 字节数据 */
#define SI4732_PATCH_DELAY_US       300     /* 每行之间的最小间隔（PU2CLR 取值） */
#define SI4732_SSB_PATCH_DELAY_MS   50      /* 补丁 POWER_UP 后的稳定时间 */

/* 模块是否自带 32.768kHz 晶振：
 *   0 = 无晶振，由 ESP32 的 GPIO5 输出 RCLK（本工程硬件）
 *   1 = 模块自带晶振，POWER_UP 使用 XOSCEN=1
 * 无晶振时不要打开自动切换，否则每次失败都会白白多试一轮。 */
#define SI4732_BOARD_HAS_CRYSTAL    0

/* 时钟模式：false = 外部 RCLK（XOSCEN=0），true = 板载晶振（XOSCEN=1） */
static bool s_use_crystal = (SI4732_BOARD_HAS_CRYSTAL != 0);

/* ============================ 驱动内部状态 ============================ */
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_powered = false;
static si4732_band_t s_band = SI4732_BAND_FM;
static uint32_t s_freq_khz = 101100;
static uint8_t s_volume = 30;

/* SSB 状态：补丁下载后一直有效，直到芯片复位/掉电 */
static bool s_patch_loaded = false;
static bool s_ssb_active = false;
static bool s_ssb_usb = false;
static int16_t s_bfo_hz = 0;
/* 默认 3kHz，与 Pocket-FT8 / SI4735 Touchscreen 的
 * setSSBConfig(2, 1, 0, 1, 0, 1) 一致；1.2kHz 会切掉 FT8 的高音频音调 */
static si4732_ssb_bandwidth_t s_ssb_bw = SI4732_SSB_BW_3K0;

/* 调谐完成状态：STC 是一次性中断标志，读状态时可能已被清零，
 * 所以单独保存“最近一次 tune/seek 是否成功完成”。 */
static bool s_tuned = false;

/* 补丁加载/下载期间为 true，I2C 扫描等操作应跳过 */
static volatile bool s_patching = false;

/* 最近一次成功加载的补丁 ID，0 表示未加载/已丢失 */
static uint16_t s_patch_id = 0;

/* 最近一次初始化失败信息（OLED 直接显示，方便没有串口时排查） */
static const char *s_last_stage = "NONE";
static esp_err_t s_last_error = ESP_OK;

/* 引脚只需配置一次；重试初始化时重复 gpio_config 会打印 conflict 警告 */
static bool s_gpio_configured = false;

/* RST 引脚是否已配置为输出（重置流程也可能在 si4732_init 之前被调用） */
static bool s_rst_gpio_ready = false;

/* ============================ 底层 I2C ============================ */

/* 把 Si4732 挂到 board_i2c 管理的共享总线上 */
static esp_err_t si4732_bus_attach(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(board_i2c_get_bus(&bus), TAG, "获取 I2C 总线失败");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SI4732_I2C_ADDR,
        .scl_speed_hz = SI4732_I2C_CLK_HZ,
    };

    /* 挂设备也会改总线配置，和 OLED/扫描 共用同一把锁 */
    ESP_RETURN_ON_ERROR(board_i2c_lock(portMAX_DELAY), TAG, "获取 I2C 锁超时");
    const esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    board_i2c_unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "已挂载 I2C 设备 0x%02X @ %dkHz",
                 SI4732_I2C_ADDR, SI4732_I2C_CLK_HZ / 1000);
    }
    return err;
}

/* 写一段命令（调用前必须持有 I2C 锁） */
static esp_err_t si4732_write_locked(const uint8_t *data, size_t len)
{
    return i2c_master_transmit(s_dev, data, len, SI4732_XFER_TIMEOUT_MS);
}

/* 读若干字节（调用前必须持有 I2C 锁） */
static esp_err_t si4732_read_locked(uint8_t *data, size_t len)
{
    return i2c_master_receive(s_dev, data, len, SI4732_XFER_TIMEOUT_MS);
}

/**
 * @brief 轮询状态字节直到 CTS 置位（调用前必须持有 I2C 锁）
 *
 * Si4732 的每个响应第一个字节都是 STATUS：
 *   bit7 CTS = 1 表示可以接收下一条命令
 *   bit6 ERR = 1 表示上一条命令执行出错
 *
 * 注意：芯片在 powerdown（刚复位）时可能直接 NACK 状态读，
 * 不能把一次 NACK 当成致命错误，要继续轮询直到超时。
 */
static esp_err_t si4732_wait_cts_locked(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    esp_err_t last_err = ESP_ERR_TIMEOUT;

    for (;;) {
        uint8_t status = 0;
        const esp_err_t err = si4732_read_locked(&status, 1);

        if (err == ESP_OK) {
            if (status & STATUS_CTS) {
                if (status & STATUS_ERR) {
                    ESP_LOGW(TAG, "芯片报告命令错误: STATUS=0x%02X", status);
                    return ESP_ERR_INVALID_RESPONSE;
                }
                return ESP_OK;
            }
            last_err = ESP_ERR_TIMEOUT;
        } else {
            /* powerdown 或芯片尚未就绪，NACK 后继续等 */
            last_err = err;
        }

        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGW(TAG, "等待 CTS 超时（%ums，最后错误 %s）",
                     (unsigned)timeout_ms, esp_err_to_name(last_err));
            return last_err;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/**
 * @brief 发送一条命令并可选读取响应
 *
 * 整个过程都在 I2C 互斥锁内完成，保证与 OLED 刷新不会互相打断。
 *
 * @param cmd      命令缓冲区（首字节为命令字）
 * @param cmd_len  命令长度
 * @param resp     响应缓冲区，可为 NULL；首字节为 STATUS
 * @param resp_len 响应长度
 */
static esp_err_t si4732_exec(const uint8_t *cmd, size_t cmd_len,
                             uint8_t *resp, size_t resp_len)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "请在 si4732_init() 之后调用");

    /* POWER_UP 是唤醒命令：芯片在 powerdown 状态下会 NACK 状态读，
     * 所以不能先等 CTS，必须直接写命令（与 AN332/PU2CLR 行为一致）。 */
    const bool is_power_up = (cmd[0] == CMD_POWER_UP);

    esp_err_t err = board_i2c_lock(portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }

    if (!is_power_up) {
        err = si4732_wait_cts_locked(SI4732_CTS_TIMEOUT_MS);
    }

    if (err == ESP_OK) {
        err = si4732_write_locked(cmd, cmd_len);

        /* 刚复位时芯片可能还没准备好，POWER_UP 允许重试几次 */
        for (int retry = 0; is_power_up && err != ESP_OK && retry < 2; retry++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            err = si4732_write_locked(cmd, cmd_len);
        }
    }

    if (err == ESP_OK) {
        err = si4732_wait_cts_locked(SI4732_CTS_TIMEOUT_MS);
    }
    if (err == ESP_OK && resp != NULL && resp_len > 0) {
        err = si4732_read_locked(resp, resp_len);
    }

    board_i2c_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "命令 0x%02X 执行失败: %s", cmd[0], esp_err_to_name(err));
    }
    return err;
}

/* ============================ 波段信息 ============================ */

static bool si4732_band_is_fm(si4732_band_t band)
{
    return band == SI4732_BAND_FM;
}

void si4732_band_info(si4732_band_t band,
                      uint32_t *min_khz, uint32_t *max_khz,
                      uint32_t *step_khz, uint32_t *default_khz)
{
    uint32_t min_v = 0, max_v = 0, step_v = 0, def_v = 0;

    switch (band) {
    case SI4732_BAND_FM:
        min_v = 64000; max_v = 108000; step_v = 100; def_v = 101100;
        break;
    case SI4732_BAND_AM:
        min_v = 520; max_v = 1710; step_v = 10; def_v = 1000;
        break;
    case SI4732_BAND_SW:
        min_v = 2300; max_v = 26100; step_v = 5; def_v = 9700;
        break;
    case SI4732_BAND_LW:
        min_v = 153; max_v = 279; step_v = 3; def_v = 200;
        break;
    default:
        break;
    }

    if (min_khz) { *min_khz = min_v; }
    if (max_khz) { *max_khz = max_v; }
    if (step_khz) { *step_khz = step_v; }
    if (default_khz) { *default_khz = def_v; }
}

/* ============================ 上电 / 复位 ============================ */

/**
 * @brief 写入当前波段的搜台范围与步进
 *
 * 参考 PU2CLR SI4735 库：setSeekFmLimits / setSeekFmSpacing /
 * setSeekAmLimits / setSeekAmSpacing。不写这些属性时，芯片使用
 * 默认的 MW 520~1710kHz，短波/长波搜台会跑回中波范围。
 */
static esp_err_t si4732_apply_band_config(si4732_band_t band)
{
    uint32_t min_khz = 0, max_khz = 0, step_khz = 0;
    si4732_band_info(band, &min_khz, &max_khz, &step_khz, NULL);

    if (si4732_band_is_fm(band)) {
        ESP_RETURN_ON_ERROR(si4732_set_property(PROP_FM_SEEK_BAND_BOTTOM,
                                                (uint16_t)(min_khz / FM_PROP_UNIT_KHZ)),
                            TAG, "设置 FM 搜台下限失败");
        ESP_RETURN_ON_ERROR(si4732_set_property(PROP_FM_SEEK_BAND_TOP,
                                                (uint16_t)(max_khz / FM_PROP_UNIT_KHZ)),
                            TAG, "设置 FM 搜台上限失败");
        /* FM 步进属性单位为 10kHz，10 = 100kHz */
        ESP_RETURN_ON_ERROR(si4732_set_property(PROP_FM_SEEK_FREQ_SPACING,
                                                (uint16_t)(step_khz / FM_PROP_UNIT_KHZ)),
                            TAG, "设置 FM 搜台步进失败");
    } else {
        ESP_RETURN_ON_ERROR(si4732_set_property(PROP_AM_SEEK_BAND_BOTTOM,
                                                (uint16_t)min_khz),
                            TAG, "设置 AM 搜台下限失败");
        ESP_RETURN_ON_ERROR(si4732_set_property(PROP_AM_SEEK_BAND_TOP,
                                                (uint16_t)max_khz),
                            TAG, "设置 AM 搜台上限失败");
        ESP_RETURN_ON_ERROR(si4732_set_property(PROP_AM_SEEK_FREQ_SPACING,
                                                (uint16_t)step_khz),
                            TAG, "设置 AM 搜台步进失败");
    }

    ESP_LOGI(TAG, "%s 搜台范围 %lukHz~%lukHz，步进 %lukHz",
             band == SI4732_BAND_FM ? "FM" :
             band == SI4732_BAND_AM ? "AM" :
             band == SI4732_BAND_SW ? "SW" : "LW",
             (unsigned long)min_khz, (unsigned long)max_khz,
             (unsigned long)step_khz);
    return ESP_OK;
}


esp_err_t si4732_reset(void)
{
    ESP_RETURN_ON_ERROR(si4732_bus_attach(), TAG, "挂载 I2C 设备失败");

    /* 允许在 si4732_init() 之前单独调用（用于开机先释放被拉低的 SDA） */
    if (!s_rst_gpio_ready) {
        const gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << BOARD_SI4732_RST_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "配置 RST 引脚失败");
        s_rst_gpio_ready = true;
    }
    gpio_set_level(BOARD_SI4732_RST_GPIO, 1);

    /* AN332 第 6 节：芯片在 RST 上升沿采样 GPO1 / GPO2 选择总线模式：
     *   2-wire: GPO1 = 1, GPO2 = 0
     *   3-wire: GPO1 = 0, GPO2 = 0
     * GPO1 内部只有 1MΩ 上拉，不能只依赖它——这里把 GPIO3 推挽强驱动为高，
     * 否则芯片可能进入 3-wire 模式，I2C 永远 NACK。 */
    const gpio_config_t gpo_force_high = {
        .pin_bit_mask = 1ULL << BOARD_SI4732_GPO1_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpo_force_high), TAG, "配置 GPO1 失败");
    gpio_set_level(BOARD_SI4732_GPO1_GPIO, 1);
    esp_rom_delay_us(100);     /* AN332 要求 >=30ns，这里给 100us */

    /* AN332：RST 拉低至少 100us；这里给 1ms，避免模块上 RC 复位电路
     * 让低电平不够长 */
    gpio_set_level(BOARD_SI4732_RST_GPIO, 0);
    esp_rom_delay_us(1000);
    gpio_set_level(BOARD_SI4732_RST_GPIO, 1);   /* 上升沿采样总线模式 */
    esp_rom_delay_us(50);

    /* 采样完成后释放 GPO1，切回输入上拉（用于后续 STC 中断） */
    const gpio_config_t gpo_input = {
        .pin_bit_mask = 1ULL << BOARD_SI4732_GPO1_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&gpo_input);

    esp_rom_delay_us(1000);

    /* 复位后芯片进入 powerdown，此时读状态会 NACK，这是正常现象，
     * 不能再把 NACK 当失败——真正的握手由随后的 POWER_UP 完成。 */
    s_powered = false;
    s_ssb_active = false;
    s_tuned = false;
    s_patch_loaded = false;
    s_patch_id = 0;
    return ESP_OK;
}

static esp_err_t si4732_power_up_once(si4732_band_t band, bool crystal)
{
    /* FUNC：0 = FM，1 = AM（MW/SW/LW 都使用 AM 模式）
     * ARG1 bit6 = GPO2OEN（RDS 用，这里保留默认值 1）
     * ARG2 = 0x05，模拟音频输出模式 */
    const uint8_t func = si4732_band_is_fm(band) ? 0x00 : 0x01;
    const uint8_t cmd[3] = {
        CMD_POWER_UP,
        (uint8_t)(0x40 | (crystal ? 0x10 : 0x00) | func),
        0x05,
    };

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), NULL, 0), TAG,
                        "POWER_UP 失败 (FUNC=%u, XOSCEN=%u)",
                        (unsigned)func, crystal ? 1u : 0u);
    vTaskDelay(pdMS_TO_TICKS(SI4732_POWERUP_DELAY_MS));

    /* 参考时钟：不设置会让调谐频率整体偏移 */
    ESP_RETURN_ON_ERROR(si4732_set_property(PROP_REFCLK_FREQ, SI4732_REFCLK_HZ),
                        TAG, "设置 REFCLK_FREQ 失败");
    ESP_RETURN_ON_ERROR(si4732_set_property(PROP_REFCLK_PRESCALE, 1),
                        TAG, "设置 REFCLK_PRESCALE 失败");

    /* 回读确认芯片内部真的接受 32768Hz 参考时钟，同时校验通信是否正常 */
    uint16_t refclk_read = 0;
    const esp_err_t refclk_err = si4732_get_property(PROP_REFCLK_FREQ, &refclk_read);
    if (refclk_err != ESP_OK) {
        ESP_LOGE(TAG, "读取 REFCLK_FREQ 失败: %s", esp_err_to_name(refclk_err));
        return refclk_err;
    }
    if (refclk_read != SI4732_REFCLK_HZ) {
        ESP_LOGE(TAG, "REFCLK_FREQ 回读 = %u Hz（期望 %d Hz），判定芯片通信异常",
                 (unsigned)refclk_read, SI4732_REFCLK_HZ);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "REFCLK_FREQ 回读 = %u Hz", (unsigned)refclk_read);

    /* 搜台范围必须跟着波段走，否则 SW/LW 会跑回 MW 默认范围 */
    ESP_RETURN_ON_ERROR(si4732_apply_band_config(band), TAG, "配置波段搜台参数失败");

    s_powered = true;
    s_band = band;
    return ESP_OK;
}

/**
 * @brief 上电，并在外部 RCLK / 板载晶振之间自动重试
 *
 * 很多 Si4732-A10 模块自带 32.768kHz 晶振，此时 RCLK 不能接外部时钟，
 * POWER_UP 必须带 XOSCEN=1；本板方案则是外部 RCLK + XOSCEN=0。
 * 两种模式都试一次，成功后记住，避免每次切换波段都失败重试。
 */
static esp_err_t si4732_power_up(si4732_band_t band)
{
    esp_err_t err = si4732_power_up_once(band, s_use_crystal);

    if (err != ESP_OK && SI4732_BOARD_HAS_CRYSTAL) {
        ESP_LOGW(TAG, "按%s时钟模式上电失败，改用%s模式重试",
                 s_use_crystal ? "板载晶振" : "外部 RCLK",
                 s_use_crystal ? "外部 RCLK" : "板载晶振");

        /* 先复位，清掉上一次失败留下的状态 */
        (void)si4732_reset();

        err = si4732_power_up_once(band, !s_use_crystal);
        if (err == ESP_OK) {
            s_use_crystal = !s_use_crystal;
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "时钟模式: %s",
                 s_use_crystal ? "板载晶振 XOSCEN=1" : "外部 RCLK 32.768kHz XOSCEN=0");
    } else {
        ESP_LOGE(TAG, "上电失败：芯片没有应答。请确认 GPIO5 已接到模块 RCLK、"
                      "SENB 接 GND、VDD=3.3V、SDA/SCL 接线正确");
    }
    return err;
}

esp_err_t si4732_power_down(void)
{
    if (!s_powered) {
        return ESP_OK;
    }

    const uint8_t cmd[1] = { CMD_POWER_DOWN };
    const esp_err_t err = si4732_exec(cmd, sizeof(cmd), NULL, 0);
    s_powered = false;
    s_ssb_active = false;
    s_tuned = false;
    return err;
}

/* ============================ 属性读写 ============================ */

esp_err_t si4732_set_property(uint16_t prop, uint16_t value)
{
    const uint8_t cmd[6] = {
        CMD_SET_PROPERTY,
        0x00,                       /* ARG1 保留 */
        (uint8_t)(prop >> 8),
        (uint8_t)(prop & 0xFF),
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };
    return si4732_exec(cmd, sizeof(cmd), NULL, 0);
}

esp_err_t si4732_get_property(uint16_t prop, uint16_t *value)
{
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "value 不能为空");

    const uint8_t cmd[4] = {
        CMD_GET_PROPERTY,
        0x00,
        (uint8_t)(prop >> 8),
        (uint8_t)(prop & 0xFF),
    };
    uint8_t resp[4] = {0};

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), resp, sizeof(resp)),
                        TAG, "GET_PROPERTY 失败");

    *value = (uint16_t)(((uint16_t)resp[2] << 8) | resp[3]);
    return ESP_OK;
}

esp_err_t si4732_get_firmware(si4732_firmware_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out 不能为空");

    const uint8_t cmd[1] = { CMD_GET_REV };
    uint8_t resp[9] = {0};

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), resp, sizeof(resp)),
                        TAG, "GET_REV 失败");

    out->part_number = resp[1];
    out->fw_major = resp[2];
    out->fw_minor = resp[3];
    out->patch_id = (uint16_t)(((uint16_t)resp[4] << 8) | resp[5]);
    out->component_id = (uint16_t)(((uint16_t)resp[6] << 8) | resp[7]);
    out->chip_rev = resp[8];
    return ESP_OK;
}

bool si4732_using_crystal(void)
{
    return s_use_crystal;
}

void si4732_get_last_error(const char **stage, esp_err_t *err)
{
    if (stage != NULL) {
        *stage = s_last_stage;
    }
    if (err != NULL) {
        *err = s_last_error;
    }
}

/* ============================ 音量 / 静音 ============================ */

esp_err_t si4732_set_volume(uint8_t volume)
{
    if (volume > SI4732_VOLUME_MAX) {
        volume = SI4732_VOLUME_MAX;
    }

    /* 与 PU2CLR 库一致：只写 RX_VOLUME，静音由 si4732_set_mute 单独控制 */
    ESP_RETURN_ON_ERROR(si4732_set_property(PROP_RX_VOLUME, volume),
                        TAG, "设置音量失败");

    s_volume = volume;
    return ESP_OK;
}

uint8_t si4732_get_volume(void)
{
    return s_volume;
}

esp_err_t si4732_set_mute(bool mute)
{
    return si4732_set_property(PROP_RX_HARD_MUTE, mute ? 0x0003 : 0x0000);
}

/* ============================ 调谐 / 搜台 ============================ */

uint32_t si4732_get_frequency(void)
{
    return s_freq_khz;
}

si4732_band_t si4732_get_band(void)
{
    return s_band;
}

/**
 * @brief 读取 TUNE_STATUS 并解析频率/RSSI/SNR
 *
 * 响应格式（AN332）：STATUS, ARG1, READFREQ(2), RSSI, SNR, MULT, ...
 * FM 的 READFREQ 单位是 10kHz，AM 是 kHz。
 */
static esp_err_t si4732_read_tune_status(bool fm, bool intack, bool *stc,
                                         bool *valid, uint8_t *rssi, uint8_t *snr)
{
    const uint8_t cmd[2] = {
        fm ? CMD_FM_TUNE_STATUS : CMD_AM_TUNE_STATUS,
        (uint8_t)(intack ? 0x01 : 0x00),   /* ARG1 bit0 = INTACK */
    };
    uint8_t resp[7] = {0};

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), resp, sizeof(resp)),
                        TAG, "读取调谐状态失败");

    /* STATUS 的 bit0 是 STCINT：调谐/搜台完成标志 */
    if (stc) {
        *stc = (resp[0] & 0x01) != 0;
    }
    if (valid) {
        *valid = (resp[1] & TUNE_STATUS_VALID) != 0;   /* VALID = 信号是否有效 */
    }

    /* READFREQ 无论 VALID 是否为 1 都是当前实际频率；0 视为无效数据 */
    const uint32_t freq = ((uint32_t)resp[2] << 8) | resp[3];
    if (freq != 0) {
        s_freq_khz = fm ? freq * 10 : freq;
    }

    if (rssi) {
        *rssi = resp[4];
    }
    if (snr) {
        *snr = resp[5];
    }
    return ESP_OK;
}

/**
 * @brief 轮询直到调谐/搜台完成（STC=1）或超时
 *
 * 注意不能等 VALID：VALID 表示“信号达到搜台有效门限”，
 * 没接天线或信号弱时调谐已经完成，但 VALID 仍然是 0。
 */
static esp_err_t si4732_wait_tune_done(bool fm, uint8_t *rssi, uint8_t *snr)
{
    const TickType_t start = xTaskGetTickCount();

    for (;;) {
        bool stc = false;
        bool valid = false;
        ESP_RETURN_ON_ERROR(si4732_read_tune_status(fm, false, &stc, &valid, rssi, snr),
                            TAG, "读取调谐状态失败");

        if (stc) {
            /* 再读一次并清除 STCINT，拿到最终 RSSI/SNR/VALID */
            ESP_RETURN_ON_ERROR(si4732_read_tune_status(fm, true, NULL, &valid, rssi, snr),
                                TAG, "读取调谐状态失败");
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(SI4732_TUNE_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "调谐超时（STC 未置位）");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t si4732_tune(uint32_t freq_khz)
{
    uint32_t min_khz = 0, max_khz = 0;
    si4732_band_info(s_band, &min_khz, &max_khz, NULL, NULL);

    ESP_RETURN_ON_FALSE(freq_khz >= min_khz && freq_khz <= max_khz,
                        ESP_ERR_INVALID_ARG, TAG,
                        "频率 %lukHz 超出当前波段范围 %lu~%lu",
                        (unsigned long)freq_khz,
                        (unsigned long)min_khz,
                        (unsigned long)max_khz);

    const bool fm = si4732_band_is_fm(s_band);
    const uint32_t chip_freq = fm ? (freq_khz / 10) : freq_khz;

    uint8_t cmd[6] = {
        fm ? CMD_FM_TUNE_FREQ : CMD_AM_TUNE_FREQ,
        0x00,                       /* ARG1：不启用软静音等选项 */
        (uint8_t)(chip_freq >> 8),
        (uint8_t)(chip_freq & 0xFF),
        0x00,                       /* ANTCAPH */
        0x00,                       /* ANTCAPL */
    };
    size_t cmd_len = 5;     /* FM: CMD + ARG1 + FREQH + FREQL + ANTCAP */

    /* AM_TUNE_FREQ 必须带天线调谐电容参数（AN332 与 PU2CLR 库一致）：
     * 短波（>1800kHz）给 ANTCAPL=1，其余为 0（自动选择） */
    if (!fm) {
        cmd[5] = (freq_khz > 1800) ? 1 : 0;
        cmd_len = 6;

        /* SSB 模式：AM_TUNE_FREQ ARG1 的 D7:D6 = USBLSB
         * （10 = USB，01 = LSB），与 PU2CLR 库、参考工程一致。
         * 注意不是 bit5:4——写错位置芯片会忽略，仍按普通 AM 解调。 */
        if (s_ssb_active) {
            cmd[1] = s_ssb_usb ? 0x80 : 0x40;
        }
    }

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, cmd_len, NULL, 0), TAG, "TUNE 命令失败");

    s_freq_khz = freq_khz;
    s_tuned = false;
    const esp_err_t err = si4732_wait_tune_done(fm, NULL, NULL);
    if (err == ESP_OK) {
        s_tuned = true;
    }
    return err;
}

esp_err_t si4732_seek(bool up)
{
    /* SSB 模式没有硬件搜台，由上层决定怎么调 */
    ESP_RETURN_ON_FALSE(!s_ssb_active, ESP_ERR_NOT_SUPPORTED, TAG, "SSB 模式不支持搜台");

    const bool fm = si4732_band_is_fm(s_band);

    uint8_t cmd[6] = {
        fm ? CMD_FM_SEEK_START : CMD_AM_SEEK_START,
        (uint8_t)(0x04 | (up ? 0x08 : 0x00)),   /* WRAP | SEEKUP */
        0x00, 0x00,                             /* AM 专用参数 */
        0x00,                                   /* ANTCAPH */
        0x00,                                   /* ANTCAPL */
    };
    size_t cmd_len = 2;

    /* AM_SEEK_START 同样需要 4 个附加参数，否则芯片可能拒绝命令 */
    if (!fm) {
        cmd[5] = (s_freq_khz > 1800) ? 1 : 0;
        cmd_len = 6;
    }

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, cmd_len, NULL, 0), TAG, "SEEK 命令失败");

    s_tuned = false;
    const esp_err_t err = si4732_wait_tune_done(fm, NULL, NULL);
    if (err == ESP_OK) {
        s_tuned = true;
    }
    return err;
}

/* ============================ 波段切换 ============================ */

esp_err_t si4732_set_band(si4732_band_t band)
{
    ESP_RETURN_ON_FALSE(band < SI4732_BAND_COUNT, ESP_ERR_INVALID_ARG,
                        TAG, "无效波段 %d", (int)band);

    if (s_powered && band == s_band) {
        return ESP_OK;
    }

    /* 同属 AM 模式（MW/SW/LW）时不必重新上电，直接改频率即可 */
    const bool need_power_cycle =
        !s_powered || (si4732_band_is_fm(band) != si4732_band_is_fm(s_band));

    if (need_power_cycle) {
        ESP_RETURN_ON_ERROR(si4732_power_down(), TAG, "退出上电失败");
        ESP_RETURN_ON_ERROR(si4732_power_up(band), TAG, "上电失败");
        ESP_RETURN_ON_ERROR(si4732_set_volume(s_volume), TAG, "恢复音量失败");
    }

    s_band = band;

    /* 切换波段后退出 SSB，需要时由上层重新启用 */
    s_ssb_active = false;

    /* AM 模式内部切换（MW/SW/LW）时只需要更新搜台范围 */
    if (!need_power_cycle) {
        ESP_RETURN_ON_ERROR(si4732_apply_band_config(band), TAG, "配置波段搜台参数失败");
    }

    uint32_t default_khz = 0;
    si4732_band_info(band, NULL, NULL, NULL, &default_khz);
    return si4732_tune(default_khz);
}

/* ============================ SSB 模式 ============================ */

/**
 * @brief POWER_UP(FUNC=15) 查询 library ID
 *
 * AN332 7.2.2：进入补丁模式前必须先查询 library，确认补丁兼容；
 * 查询完成后芯片自动回到 powerdown，随后才能发 PATCH=1 的 POWER_UP。
 */
static esp_err_t si4732_query_library(bool xoscen)
{
    const uint8_t cmd[3] = {
        CMD_POWER_UP,
        (uint8_t)(0x0F | (xoscen ? 0x10 : 0x00)),   /* FUNC=15, XOSCEN */
        0x05,
    };
    uint8_t resp[8] = {0};

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), resp, sizeof(resp)),
                        TAG, "POWER_UP(QUERY) 失败");

    ESP_LOGI(TAG, "Library 查询响应: %02X %02X %02X %02X %02X %02X %02X %02X",
             resp[0], resp[1], resp[2], resp[3],
             resp[4], resp[5], resp[6], resp[7]);

    /* RESP7 是 library ID：R11 = 0x0B，与 patch_full(0x9D29) 匹配 */
    ESP_LOGI(TAG, "Library ID=0x%02X%s", resp[7],
             (resp[7] == 0x0B) ? "（R11，与补丁匹配）" : "（注意：补丁要求 R11）");
    if (resp[7] != 0x0B) {
        ESP_LOGW(TAG, "库版本不是 R11，补丁可能不兼容");
    }

    vTaskDelay(pdMS_TO_TICKS(5));
    return ESP_OK;
}

/**
 * @brief 以“补丁模式”上电（PATCH=1、FUNC=AM）
 *
 * PU2CLR 的 patchPowerUp 固定用 XOSCEN=1（它的板子带晶振）；
 * 本板是外部 RCLK，应使用 XOSCEN=0，这里由调用者按已确认的时钟模式传入。
 */
static esp_err_t si4732_patch_power_up(bool xoscen)
{
    const uint8_t cmd[3] = {
        CMD_POWER_UP,
        (uint8_t)(0x20 | (xoscen ? 0x10 : 0x00) | 0x01),   /* PATCH + XOSCEN + FUNC=AM */
        0x05,
    };

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), NULL, 0),
                        TAG, "POWER_UP(PATCH, XOSCEN=%d) 失败", xoscen ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(SI4732_SSB_PATCH_DELAY_MS));

    s_powered = true;
    return ESP_OK;
}

/**
 * @brief 把补丁数组按 8 字节一行写入芯片
 *
 * AN332 要求每写一行后等 CTS 再写下一行；ERR 置位说明补丁校验失败，
 * 芯片会 halt，必须硬件复位才能恢复。
 */
static esp_err_t si4732_download_patch(void)
{
    const size_t patch_size = sizeof(ssb_patch_content);
    ESP_RETURN_ON_FALSE(patch_size % SI4732_PATCH_LINE_BYTES == 0,
                        ESP_ERR_INVALID_SIZE, TAG, "补丁长度不是 8 的整数倍");

    const TickType_t start = xTaskGetTickCount();
    esp_err_t err = board_i2c_lock(portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t offset = 0; offset < patch_size; offset += SI4732_PATCH_LINE_BYTES) {
        err = si4732_write_locked(&ssb_patch_content[offset],
                                  SI4732_PATCH_LINE_BYTES);
        if (err == ESP_OK) {
            err = si4732_wait_cts_locked(SI4732_CTS_TIMEOUT_MS);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "补丁下载失败 @%u: %s",
                     (unsigned)offset, esp_err_to_name(err));
            break;
        }
    }

    board_i2c_unlock();

    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "SSB 补丁下载完成: %u 字节，%u 行，耗时 %ums",
             (unsigned)patch_size,
             (unsigned)(patch_size / SI4732_PATCH_LINE_BYTES),
             (unsigned)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
    return ESP_OK;
}

/**
 * @brief 完整执行一次补丁加载流程：查询 library -> 补丁上电 -> 下载
 */
static esp_err_t si4732_ssb_load_patch_once(bool xoscen)
{
    (void)si4732_power_down();
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(si4732_query_library(xoscen), TAG, "Library 查询失败");
    ESP_RETURN_ON_ERROR(si4732_patch_power_up(xoscen), TAG, "补丁模式上电失败");
    return si4732_download_patch();
}

/**
 * @brief 写入 SSB_MODE 属性（带宽、边带滤波器、AVC、软静音、AFC）
 *
 * 字节布局（AN332 REV 0.8）：
 *   byte0: AUDIOBW[3:0] | SBCUTFLT[7:4]
 *   byte1: AVC_DIVIDER[3:0] | AVCEN[4] | SMUTESEL[5] | DSP_AFCDIS[7]
 * 与 PU2CLR loadPatch 保持一致：SBCUTFLT 窄带时为 0，AVCEN=0，DSP_AFCDIS=1。
 */
static esp_err_t si4732_apply_ssb_mode(void)
{
    const bool narrow = (s_ssb_bw == SI4732_SSB_BW_1K2 ||
                         s_ssb_bw == SI4732_SSB_BW_0K5 ||
                         s_ssb_bw == SI4732_SSB_BW_1K0);

    /* AN332 / PU2CLR 的字节顺序：
     *   属性值高字节 = raw[1] = AVC_DIVIDER | AVCEN | SMUTESEL | DSP_AFCDIS
     *   属性值低字节 = raw[0] = AUDIOBW | SBCUTFLT
     * 之前写反了，芯片会收到错误的带宽/滤波器配置。 */
    const uint8_t audio_byte = (uint8_t)(((uint8_t)s_ssb_bw & 0x0F) |
                                         (narrow ? 0x00 : 0x10));   /* SBCUTFLT */
    /* 参考 Pocket-FT8 / SI4735 Touchscreen：AVCEN=1 打开 SSB 自动音量，
     * 否则 AVCEN=0 时 SSB 音频会非常小甚至听不到。 */
    const uint8_t avc_byte = (uint8_t)(0x00 |                      /* AVC_DIVIDER = 0 */
                                       (0x01 << 4) |              /* AVCEN = 1 */
                                       (0x00 << 5) |              /* SMUTESEL = 0 */
                                       (0x01 << 7));              /* DSP_AFCDIS = 1 */

    const esp_err_t err = si4732_set_property(PROP_SSB_MODE,
                                              (uint16_t)(((uint16_t)avc_byte << 8) | audio_byte));
    if (err != ESP_OK) {
        return err;
    }

    /* 关闭 SSB 软静音，排除 SNR=0 时被软静音压掉音频 */
    return si4732_set_property(PROP_SSB_SOFT_MUTE_MAX_ATTENUATION, 0);
}

esp_err_t si4732_ssb_enable(bool usb)
{
    ESP_RETURN_ON_FALSE(s_powered, ESP_ERR_INVALID_STATE, TAG, "芯片未上电");
    ESP_RETURN_ON_FALSE(!si4732_band_is_fm(s_band), ESP_ERR_INVALID_STATE,
                        TAG, "SSB 只能在 AM/SW/LW 波段使用");

    s_ssb_usb = usb;

    /* 频率无效时先回到当前波段默认频率，避免补丁后用 0Hz 调谐 */
    uint32_t min_khz = 0, max_khz = 0, default_khz = 0;
    si4732_band_info(s_band, &min_khz, &max_khz, NULL, &default_khz);
    if (s_freq_khz < min_khz || s_freq_khz > max_khz) {
        s_freq_khz = default_khz;
    }

    if (!s_patch_loaded) {
        ESP_LOGI(TAG, "首次进入 SSB，开始加载补丁 ...");
        s_patching = true;   /* 下载期间禁止扫描总线 */

        /* AN332：XOSCEN=0 表示使用外部 RCLK，XOSCEN=1 表示使用晶振。
         * 先用本板已经在用的时钟模式（正常上电时已确认可用），
         * 失败再复位换另一种模式重试。 */
        esp_err_t err = si4732_ssb_load_patch_once(s_use_crystal);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "补丁流程(%s)失败: %s，复位后改用%s重试",
                     s_use_crystal ? "XOSCEN=1" : "XOSCEN=0",
                     esp_err_to_name(err),
                     s_use_crystal ? "XOSCEN=0" : "XOSCEN=1");
            (void)si4732_reset();
            err = si4732_ssb_load_patch_once(!s_use_crystal);
            if (err == ESP_OK) {
                s_use_crystal = !s_use_crystal;
            }
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SSB 补丁加载失败，复位并恢复 AM 模式");
            (void)si4732_reset();
            if (si4732_power_up(s_band) == ESP_OK) {
                (void)si4732_set_mute(false);
                (void)si4732_set_volume(s_volume);
            }
            s_patch_loaded = false;
            s_ssb_active = false;
            s_patching = false;
            return err;
        }

        s_patch_loaded = true;

        /* AN332 7.2.2：最后一行补丁 CTS 置高后芯片已经完成启动、处于 powerup，
         * 此时不能再发 POWER_UP（会返回 ERR），直接写属性/音量即可。 */
        s_powered = true;
        err = si4732_set_property(PROP_REFCLK_FREQ, SI4732_REFCLK_HZ);
        if (err == ESP_OK) {
            err = si4732_set_property(PROP_REFCLK_PRESCALE, 1);
        }
        if (err == ESP_OK) {
            err = si4732_apply_band_config(s_band);
        }
        if (err == ESP_OK) {
            err = si4732_set_mute(false);
        }
        if (err == ESP_OK) {
            err = si4732_set_volume(s_volume);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "补丁后配置失败: %s", esp_err_to_name(err));
            (void)si4732_reset();
            s_patch_loaded = false;
            s_patching = false;
            return err;
        }

        s_patching = false;
        ESP_LOGI(TAG, "SSB 补丁加载完成");

        /* 补丁是否真正生效：GET_REV 的 PATCHID 应该非 0 */
        si4732_firmware_t fw;
        if (si4732_get_firmware(&fw) == ESP_OK) {
            s_patch_id = fw.patch_id;
            ESP_LOGI(TAG, "补丁后版本: PN=0x%02X FW=%u.%u PATCH=0x%04X COMP=0x%04X REV=0x%02X",
                     (unsigned)fw.part_number, (unsigned)fw.fw_major,
                     (unsigned)fw.fw_minor, (unsigned)fw.patch_id,
                     (unsigned)fw.component_id, (unsigned)fw.chip_rev);
            if (fw.patch_id == 0) {
                ESP_LOGW(TAG, "PATCH=0x0000，补丁可能没有真正运行");
            }
        } else {
            s_patch_id = 0;
            ESP_LOGW(TAG, "补丁后读取版本失败");
        }
    }

    ESP_RETURN_ON_ERROR(si4732_apply_ssb_mode(), TAG, "配置 SSB_MODE 失败");
    s_ssb_active = true;

    ESP_LOGI(TAG, "进入 SSB %s: %lukHz，BFO %+dHz，带宽 %d，TUNE ARG1=0x%02X",
             usb ? "USB" : "LSB", (unsigned long)s_freq_khz,
             (int)s_bfo_hz, (int)s_ssb_bw, usb ? 0x80 : 0x40);

    /* 先调谐（AM_TUNE_FREQ 的 ARG1 D7:D6 带边带位），再写 BFO，
     * 与 PU2CLR / 参考工程的 setSSB -> setFrequency -> setSSBBfo 顺序一致 */
    ESP_RETURN_ON_ERROR(si4732_tune(s_freq_khz), TAG, "SSB 调谐失败");
    return si4732_ssb_set_bfo(s_bfo_hz);
}

esp_err_t si4732_ssb_disable(void)
{
    if (!s_ssb_active) {
        return ESP_OK;
    }

    s_ssb_active = false;
    ESP_LOGI(TAG, "退出 SSB，回到普通 AM");
    return si4732_tune(s_freq_khz);
}

bool si4732_ssb_is_active(void)
{
    return s_ssb_active;
}

bool si4732_is_patching(void)
{
    return s_patching;
}

uint16_t si4732_get_patch_id(void)
{
    return s_patch_id;
}

bool si4732_ssb_is_usb(void)
{
    return s_ssb_usb;
}

esp_err_t si4732_ssb_set_bfo(int16_t hz)
{
    if (hz < -16383) {
        hz = -16383;
    } else if (hz > 16383) {
        hz = 16383;
    }

    if (s_ssb_active) {
        /* 16 位有符号数以补码写入属性 */
        ESP_RETURN_ON_ERROR(si4732_set_property(PROP_SSB_BFO, (uint16_t)hz),
                            TAG, "设置 BFO 失败");
    }

    s_bfo_hz = hz;
    return ESP_OK;
}

int16_t si4732_ssb_get_bfo(void)
{
    return s_bfo_hz;
}

esp_err_t si4732_ssb_set_bandwidth(si4732_ssb_bandwidth_t bw)
{
    ESP_RETURN_ON_FALSE(bw <= SI4732_SSB_BW_1K0, ESP_ERR_INVALID_ARG,
                        TAG, "无效带宽 %d", (int)bw);

    s_ssb_bw = bw;
    if (s_ssb_active) {
        return si4732_apply_ssb_mode();
    }
    return ESP_OK;
}

si4732_ssb_bandwidth_t si4732_ssb_get_bandwidth(void)
{
    return s_ssb_bw;
}

/* ============================ 状态读取 ============================ */

esp_err_t si4732_get_status(si4732_status_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out 不能为空");

    bool valid = false;
    uint8_t rssi = 0, snr = 0;
    ESP_RETURN_ON_ERROR(si4732_read_tune_status(si4732_band_is_fm(s_band), false,
                                                NULL, &valid, &rssi, &snr),
                        TAG, "读取状态失败");

    out->band = s_band;
    out->freq_khz = s_freq_khz;
    out->volume = s_volume;
    out->rssi = rssi;
    out->snr = snr;
    out->stereo = false;    /* TODO: 解析 FM_RSQ_STATUS 的 STEREO 位 */
    out->valid = valid;
    out->tuned = s_tuned;
    out->ssb_active = s_ssb_active;
    out->ssb_usb = s_ssb_usb;
    out->bfo_hz = s_bfo_hz;
    return ESP_OK;
}

/* ============================ 初始化 ============================ */

esp_err_t si4732_init(void)
{
    s_last_stage = "GPIO";
    s_last_error = ESP_OK;
    /* RST：输出，默认高电平（正常工作）
     * 只在第一次初始化时配置，重试时重复 gpio_config 会触发
     * "conflict found for GPIO[4]" 警告（只是提示，不影响功能） */
    if (!s_gpio_configured) {
        const gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << BOARD_SI4732_RST_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "配置 RST 引脚失败");
        s_rst_gpio_ready = true;
        gpio_set_level(BOARD_SI4732_RST_GPIO, 1);

        /* GPO1：调谐完成中断输入，内部上拉
         * TODO: 改为下降沿中断 + 信号量，替代 TUNE_STATUS 轮询 */
        const gpio_config_t gpo_cfg = {
            .pin_bit_mask = 1ULL << BOARD_SI4732_GPO1_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&gpo_cfg), TAG, "配置 GPO1 引脚失败");

        s_gpio_configured = true;
    }

    s_last_stage = "ATTACH";
    const esp_err_t attach_err = si4732_bus_attach();
    if (attach_err != ESP_OK) {
        s_last_error = attach_err;
        ESP_LOGE(TAG, "挂载 I2C 设备失败: %s", esp_err_to_name(attach_err));
        return attach_err;
    }

    s_last_stage = "RESET";
    ESP_RETURN_ON_ERROR(si4732_reset(), TAG, "硬件复位失败");

    s_last_stage = "POWER";
    esp_err_t err = si4732_power_up(SI4732_BAND_FM);
    if (err != ESP_OK) {
        s_last_error = err;
        return err;
    }

    s_last_stage = "VOL";
    err = si4732_set_mute(false);
    if (err == ESP_OK) {
        err = si4732_set_volume(s_volume);
    }
    if (err != ESP_OK) {
        s_last_error = err;
        return err;
    }

    s_last_stage = "VERIFY";
    s_last_error = ESP_OK;

    uint32_t default_khz = 0;
    si4732_band_info(SI4732_BAND_FM, NULL, NULL, NULL, &default_khz);
    s_freq_khz = default_khz;

    si4732_firmware_t fw;
    const esp_err_t fw_err = si4732_get_firmware(&fw);
    if (fw_err != ESP_OK || fw.part_number != 0x20) {
        s_last_error = (fw_err != ESP_OK) ? fw_err : ESP_ERR_INVALID_RESPONSE;
        ESP_LOGE(TAG, "芯片校验失败: PN=0x%02X（期望 0x20，Si4732）",
                 (unsigned)fw.part_number);
        return s_last_error;
    }
    ESP_LOGI(TAG, "芯片信息: PN=0x%02X FW=%u.%u PATCH=0x%04X COMP=0x%04X REV=0x%02X",
             (unsigned)fw.part_number, (unsigned)fw.fw_major, (unsigned)fw.fw_minor,
             (unsigned)fw.patch_id, (unsigned)fw.component_id, (unsigned)fw.chip_rev);

    s_last_stage = "OK";
    ESP_LOGI(TAG, "初始化完成: I2C 0x%02X，REFCLK %dHz，音量 %u",
             SI4732_I2C_ADDR, SI4732_REFCLK_HZ, (unsigned)s_volume);
    return ESP_OK;
}
