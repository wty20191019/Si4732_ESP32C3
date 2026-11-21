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

static const char *TAG = "SI4732";

/* ============================ AN332 命令字 ============================ */
#define CMD_POWER_UP            0x01
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
#define PROP_REFCLK_FREQ        0x0201
#define PROP_REFCLK_PRESCALE    0x0202
#define PROP_RX_VOLUME          0x4000
#define PROP_RX_HARD_MUTE       0x4001

/* ============================ 状态位 ============================ */
#define STATUS_CTS              0x80    /* 命令可以接收 */
#define STATUS_ERR              0x40    /* 上一条命令出错 */
#define TUNE_STATUS_VALID       0x01    /* 调谐/搜台结果有效 */

/* ============================ 时序参数 ============================ */
#define SI4732_XFER_TIMEOUT_MS      50      /* 单次 I2C 传输超时 */
#define SI4732_CTS_TIMEOUT_MS       2000    /* 等待 CTS 的超时（上电约需 110ms） */
#define SI4732_TUNE_TIMEOUT_MS      2000    /* 等待调谐完成超时 */
#define SI4732_POWERUP_DELAY_MS     110     /* AN332 建议的上电稳定时间 */

/* ============================ 驱动内部状态 ============================ */
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_powered = false;
static si4732_band_t s_band = SI4732_BAND_FM;
static uint32_t s_freq_khz = 101100;
static uint8_t s_volume = 30;

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
        .scl_speed_hz = BOARD_I2C_CLK_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
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

esp_err_t si4732_reset(void)
{
    ESP_RETURN_ON_ERROR(si4732_bus_attach(), TAG, "挂载 I2C 设备失败");

    /* AN332：RST 拉低至少 100us，拉高后至少等 30us 才能通信 */
    gpio_set_level(BOARD_SI4732_RST_GPIO, 0);
    esp_rom_delay_us(100);
    gpio_set_level(BOARD_SI4732_RST_GPIO, 1);
    esp_rom_delay_us(30);

    esp_err_t err = board_i2c_lock(portMAX_DELAY);
    if (err == ESP_OK) {
        err = si4732_wait_cts_locked(SI4732_CTS_TIMEOUT_MS);
        board_i2c_unlock();
    }

    s_powered = false;
    return err;
}

static esp_err_t si4732_power_up(si4732_band_t band)
{
    /* FUNC：0 = FM，1 = AM（MW/SW/LW 都使用 AM 模式）
     * ARG1 bit6 = GPO2OEN（RDS 用，这里保留默认值 1）
     * ARG2 = 0x05，模拟音频输出模式 */
    const uint8_t func = si4732_band_is_fm(band) ? 0x00 : 0x01;
    const uint8_t cmd[3] = {
        CMD_POWER_UP,
        (uint8_t)(0x40 | func),
        0x05,
    };

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), NULL, 0), TAG, "POWER_UP 失败");
    vTaskDelay(pdMS_TO_TICKS(SI4732_POWERUP_DELAY_MS));

    /* 外部 32.768kHz 参考时钟：不设置会让调谐频率整体偏移 */
    ESP_RETURN_ON_ERROR(si4732_set_property(PROP_REFCLK_FREQ, SI4732_REFCLK_HZ),
                        TAG, "设置 REFCLK_FREQ 失败");
    ESP_RETURN_ON_ERROR(si4732_set_property(PROP_REFCLK_PRESCALE, 1),
                        TAG, "设置 REFCLK_PRESCALE 失败");

    s_powered = true;
    s_band = band;
    return ESP_OK;
}

esp_err_t si4732_power_down(void)
{
    if (!s_powered) {
        return ESP_OK;
    }

    const uint8_t cmd[1] = { CMD_POWER_DOWN };
    const esp_err_t err = si4732_exec(cmd, sizeof(cmd), NULL, 0);
    s_powered = false;
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

/* ============================ 音量 / 静音 ============================ */

esp_err_t si4732_set_volume(uint8_t volume)
{
    if (volume > SI4732_VOLUME_MAX) {
        volume = SI4732_VOLUME_MAX;
    }

    /* 先解除硬静音，再设置音量 */
    ESP_RETURN_ON_ERROR(si4732_set_property(PROP_RX_HARD_MUTE, 0x0000),
                        TAG, "解除静音失败");
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

    const uint8_t cmd[4] = {
        fm ? CMD_FM_TUNE_FREQ : CMD_AM_TUNE_FREQ,
        0x00,                       /* ARG1：不启用软静音等选项 */
        (uint8_t)(chip_freq >> 8),
        (uint8_t)(chip_freq & 0xFF),
    };

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), NULL, 0), TAG, "TUNE 命令失败");

    s_freq_khz = freq_khz;
    return si4732_wait_tune_done(fm, NULL, NULL);
}

esp_err_t si4732_seek(bool up)
{
    const bool fm = si4732_band_is_fm(s_band);
    const uint8_t cmd[2] = {
        fm ? CMD_FM_SEEK_START : CMD_AM_SEEK_START,
        (uint8_t)(0x04 | (up ? 0x08 : 0x00)),   /* WRAP | SEEKUP */
    };

    ESP_RETURN_ON_ERROR(si4732_exec(cmd, sizeof(cmd), NULL, 0), TAG, "SEEK 命令失败");

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

    uint32_t default_khz = 0;
    si4732_band_info(band, NULL, NULL, NULL, &default_khz);
    return si4732_tune(default_khz);
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
    return ESP_OK;
}

/* ============================ 初始化 ============================ */

esp_err_t si4732_init(void)
{
    /* RST：输出，默认高电平（正常工作） */
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

    ESP_RETURN_ON_ERROR(si4732_bus_attach(), TAG, "挂载 I2C 设备失败");
    ESP_RETURN_ON_ERROR(si4732_reset(), TAG, "硬件复位失败");
    ESP_RETURN_ON_ERROR(si4732_power_up(SI4732_BAND_FM), TAG, "上电失败");
    ESP_RETURN_ON_ERROR(si4732_set_volume(s_volume), TAG, "设置初始音量失败");

    uint32_t default_khz = 0;
    si4732_band_info(SI4732_BAND_FM, NULL, NULL, NULL, &default_khz);
    s_freq_khz = default_khz;

    ESP_LOGI(TAG, "初始化完成: I2C 0x%02X，REFCLK %dHz，音量 %u",
             SI4732_I2C_ADDR, SI4732_REFCLK_HZ, (unsigned)s_volume);
    return ESP_OK;
}
