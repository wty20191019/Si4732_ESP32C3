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
#include "ssb_patch_init.h"

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
#define SI4732_I2C_CLK_HZ           100000  /* Si4732 单独用 100kHz，长导线更稳（OLED 仍 400kHz） */
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
static si4732_ssb_bandwidth_t s_ssb_bw = SI4732_SSB_BW_1K2;

/* 最近一次初始化失败信息（OLED 直接显示，方便没有串口时排查） */
static const char *s_last_stage = "NONE";
static esp_err_t s_last_error = ESP_OK;

/* 引脚只需配置一次；重试初始化时重复 gpio_config 会打印 conflict 警告 */
static bool s_gpio_configured = false;

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
    const esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
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
 */
static esp_err_t si4732_wait_cts_locked(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();

    for (;;) {
        uint8_t status = 0;
        ESP_RETURN_ON_ERROR(si4732_read_locked(&status, 1), TAG, "读取状态字节失败");

        if (status & STATUS_CTS) {
            if (status & STATUS_ERR) {
                ESP_LOGW(TAG, "芯片报告命令错误: STATUS=0x%02X", status);
                return ESP_ERR_INVALID_RESPONSE;
            }
            return ESP_OK;
        }

        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGW(TAG, "等待 CTS 超时（%ums）", (unsigned)timeout_ms);
            return ESP_ERR_TIMEOUT;
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

    esp_err_t err = board_i2c_lock(portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }

    err = si4732_wait_cts_locked(SI4732_CTS_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = si4732_write_locked(cmd, cmd_len);
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

    /* AN332：RST 拉低至少 100us；这里给 1ms，避免模块上 RC 复位电路
     * 让低电平不够长；释放后再等 1ms 才轮询 CTS */
    gpio_set_level(BOARD_SI4732_RST_GPIO, 0);
    esp_rom_delay_us(1000);
    gpio_set_level(BOARD_SI4732_RST_GPIO, 1);
    esp_rom_delay_us(1000);

    esp_err_t err = board_i2c_lock(portMAX_DELAY);
    if (err == ESP_OK) {
        err = si4732_wait_cts_locked(SI4732_CTS_TIMEOUT_MS);
        board_i2c_unlock();
    }

    s_powered = false;
    s_ssb_active = false;
    s_patch_loaded = false;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "复位后等待 CTS 失败: %s（检查 SENB 是否接 GND、VDD 3.3V、SDA/SCL 上拉、RST 接线）",
                 esp_err_to_name(err));
    }
    return err;
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
static esp_err_t si4732_read_tune_status(bool fm, bool *valid,
                                         uint8_t *rssi, uint8_t *snr)
{
    const uint8_t cmd[2] = {
        fm ? CMD_FM_TUNE_STATUS : CMD_AM_TUNE_STATUS,
        TUNE_STATUS_VALID,          /* ARG1 bit0 = INTACK */
    };
    uint8_t resp[8] = {0};

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), resp, sizeof(resp)),
                        TAG, "读取调谐状态失败");

    if (valid) {
        *valid = (resp[1] & TUNE_STATUS_VALID) != 0;
    }
    if (resp[1] & TUNE_STATUS_VALID) {
        uint32_t freq = ((uint32_t)resp[2] << 8) | resp[3];
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

/* 轮询直到调谐/搜台完成（VALID=1）或超时 */
static esp_err_t si4732_wait_tune_done(bool fm, uint8_t *rssi, uint8_t *snr)
{
    const TickType_t start = xTaskGetTickCount();

    for (;;) {
        bool valid = false;
        ESP_RETURN_ON_ERROR(si4732_read_tune_status(fm, &valid, rssi, snr),
                            TAG, "读取调谐状态失败");

        if (valid) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(SI4732_TUNE_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "调谐超时");
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
    size_t cmd_len = 4;

    /* AM_TUNE_FREQ 必须带天线调谐电容参数（AN332 与 PU2CLR 库一致）：
     * 短波（>1800kHz）给 ANTCAPL=1，其余为 0（自动选择） */
    if (!fm) {
        cmd[5] = (freq_khz > 1800) ? 1 : 0;
        cmd_len = 6;

        /* SSB 模式：ARG1 的 bit5:4 选择边带（01=LSB，10=USB） */
        if (s_ssb_active) {
            cmd[1] = (uint8_t)(s_ssb_usb ? 0x02 : 0x01) << 4;
        }
    }

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, cmd_len, NULL, 0), TAG, "TUNE 命令失败");

    s_freq_khz = freq_khz;
    return si4732_wait_tune_done(fm, NULL, NULL);
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

    return si4732_wait_tune_done(fm, NULL, NULL);
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
 * @brief 以“补丁模式”上电，为下载 SSB 补丁做准备
 *
 * AN332 补丁手册要求 POWER_UP 时 PATCH=1、FUNC=1(AM)。
 */
static esp_err_t si4732_patch_power_up(void)
{
    const uint8_t arg1 = (uint8_t)(0x20 |                                  /* PATCH = 1 */
                                   (s_use_crystal ? 0x10 : 0x00) |
                                   0x01);                                  /* FUNC = AM */
    const uint8_t cmd[3] = { CMD_POWER_UP, arg1, 0x05 };

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), NULL, 0),
                        TAG, "POWER_UP(PATCH) 失败");
    vTaskDelay(pdMS_TO_TICKS(SI4732_SSB_PATCH_DELAY_MS));

    /* 上电后属性可能被复位，重新写参考时钟；否则调谐频率会偏 */
    ESP_RETURN_ON_ERROR(si4732_set_property(PROP_REFCLK_FREQ, SI4732_REFCLK_HZ),
                        TAG, "设置 REFCLK_FREQ 失败");
    ESP_RETURN_ON_ERROR(si4732_set_property(PROP_REFCLK_PRESCALE, 1),
                        TAG, "设置 REFCLK_PRESCALE 失败");

    s_powered = true;
    return ESP_OK;
}

/**
 * @brief 把补丁数组按 8 字节一行写入芯片
 *
 * 与普通命令不同，补丁行不需要等待 CTS：PU2CLR/AN332 的做法是
 * 每写一行延时 300us。整段下载期间持有 I2C 锁，避免 OLED 刷新插入。
 */
static esp_err_t si4732_download_patch(void)
{
    const size_t patch_size = sizeof(ssb_patch_content);
    ESP_RETURN_ON_FALSE(patch_size % SI4732_PATCH_LINE_BYTES == 0,
                        ESP_ERR_INVALID_SIZE, TAG, "补丁长度不是 8 的整数倍");

    const TickType_t start = xTaskGetTickCount();

    ESP_RETURN_ON_ERROR(board_i2c_lock(portMAX_DELAY), TAG, "获取 I2C 锁超时");

    for (size_t offset = 0; offset < patch_size; offset += SI4732_PATCH_LINE_BYTES) {
        const esp_err_t err = si4732_write_locked(&ssb_patch_content[offset],
                                                  SI4732_PATCH_LINE_BYTES);
        if (err != ESP_OK) {
            board_i2c_unlock();
            ESP_LOGE(TAG, "补丁下载失败 @%u: %s",
                     (unsigned)offset, esp_err_to_name(err));
            return err;
        }
        esp_rom_delay_us(SI4732_PATCH_DELAY_US);
    }

    board_i2c_unlock();
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "SSB 补丁下载完成: %u 字节，%u 行，耗时 %ums",
             (unsigned)patch_size,
             (unsigned)(patch_size / SI4732_PATCH_LINE_BYTES),
             (unsigned)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
    return ESP_OK;
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
    const uint8_t byte_hi = (uint8_t)(((uint8_t)s_ssb_bw & 0x0F) |
                                       (narrow ? 0x00 : 0x10));   /* SBCUTFLT */
    const uint8_t byte_lo = (uint8_t)(0x00 |                      /* AVC_DIVIDER = 0 */
                                       (0x00 << 4) |              /* AVCEN = 0 */
                                       (0x00 << 5) |              /* SMUTESEL = 0 */
                                       (0x01 << 7));              /* DSP_AFCDIS = 1 */

    return si4732_set_property(PROP_SSB_MODE,
                               (uint16_t)(((uint16_t)byte_hi << 8) | byte_lo));
}

esp_err_t si4732_ssb_enable(bool usb)
{
    ESP_RETURN_ON_FALSE(s_powered, ESP_ERR_INVALID_STATE, TAG, "芯片未上电");
    ESP_RETURN_ON_FALSE(!si4732_band_is_fm(s_band), ESP_ERR_INVALID_STATE,
                        TAG, "SSB 只能在 AM/SW/LW 波段使用");

    s_ssb_usb = usb;

    if (!s_patch_loaded) {
        ESP_LOGI(TAG, "首次进入 SSB，开始下载补丁 ...");
        ESP_RETURN_ON_ERROR(si4732_patch_power_up(), TAG, "补丁模式上电失败");
        ESP_RETURN_ON_ERROR(si4732_download_patch(), TAG, "补丁下载失败");
        s_patch_loaded = true;

        /* 补丁就绪后按正常 AM 模式重新上电（PU2CLR setSSB 流程） */
        ESP_RETURN_ON_ERROR(si4732_power_up(s_band), TAG, "AM 模式上电失败");
        ESP_RETURN_ON_ERROR(si4732_set_mute(false), TAG, "解除静音失败");
        ESP_RETURN_ON_ERROR(si4732_set_volume(s_volume), TAG, "恢复音量失败");
    }

    ESP_RETURN_ON_ERROR(si4732_apply_ssb_mode(), TAG, "配置 SSB_MODE 失败");
    s_ssb_active = true;
    ESP_RETURN_ON_ERROR(si4732_ssb_set_bfo(s_bfo_hz), TAG, "设置 BFO 失败");

    ESP_LOGI(TAG, "进入 SSB %s: %lukHz，BFO %+dHz，带宽 %d",
             usb ? "USB" : "LSB", (unsigned long)s_freq_khz,
             (int)s_bfo_hz, (int)s_ssb_bw);

    /* 重新调谐一次，让 AM_TUNE_FREQ 的 ARG1 带上边带位 */
    return si4732_tune(s_freq_khz);
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

/* ============================ 状态读取 ============================ */

esp_err_t si4732_get_status(si4732_status_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out 不能为空");

    bool valid = false;
    uint8_t rssi = 0, snr = 0;
    ESP_RETURN_ON_ERROR(si4732_read_tune_status(si4732_band_is_fm(s_band),
                                                &valid, &rssi, &snr),
                        TAG, "读取状态失败");

    out->band = s_band;
    out->freq_khz = s_freq_khz;
    out->volume = s_volume;
    out->rssi = rssi;
    out->snr = snr;
    out->stereo = false;    /* TODO: 解析 FM_RSQ_STATUS 的 STEREO 位 */
    out->valid = valid;
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

    /* 复位握手失败不直接放弃：有些模块只有上电后参考时钟才起振，
     * 交给 si4732_power_up() 的时钟模式自动重试处理 */
    s_last_stage = "RESET";
    const esp_err_t reset_err = si4732_reset();
    if (reset_err != ESP_OK) {
        ESP_LOGW(TAG, "复位握手失败(%s)，继续尝试上电",
                 esp_err_to_name(reset_err));
    }

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

    s_last_stage = "OK";
    s_last_error = ESP_OK;

    uint32_t default_khz = 0;
    si4732_band_info(SI4732_BAND_FM, NULL, NULL, NULL, &default_khz);
    s_freq_khz = default_khz;

    si4732_firmware_t fw;
    if (si4732_get_firmware(&fw) == ESP_OK) {
        ESP_LOGI(TAG, "芯片信息: PN=0x%02X FW=%u.%u PATCH=0x%04X COMP=0x%04X REV=0x%02X",
                 (unsigned)fw.part_number, (unsigned)fw.fw_major, (unsigned)fw.fw_minor,
                 (unsigned)fw.patch_id, (unsigned)fw.component_id, (unsigned)fw.chip_rev);
    } else {
        ESP_LOGW(TAG, "读取芯片版本失败（GET_REV 异常）");
    }

    ESP_LOGI(TAG, "初始化完成: I2C 0x%02X，REFCLK %dHz，音量 %u",
             SI4732_I2C_ADDR, SI4732_REFCLK_HZ, (unsigned)s_volume);
    return ESP_OK;
}
