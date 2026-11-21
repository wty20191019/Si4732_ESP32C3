#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "u8g2.h"

#include "board.h"
#include "board_i2c.h"
#include "encoder.h"
#include "led.h"
#include "oled_display.h"
#include "rclk.h"
#include "si4732.h"

static const char *TAG = "app";

/* ============================ 可调参数 ============================ */
#define UI_TASK_PERIOD_MS       50      /* UI 刷新检查周期 */
#define RADIO_POLL_MS           500     /* 状态轮询周期 */
#define LED_TASK_PERIOD_MS      500     /* LED 闪烁周期 */
#define RADIO_RETRY_MS          2000    /* 初始化失败后的重试间隔 */
#define OLED_RETRY_MS           2000    /* OLED 初始化失败后的重试间隔 */

#define RADIO_TASK_PRIO         6
#define UI_TASK_PRIO            3
#define CONSOLE_TASK_PRIO       2
#define LED_TASK_PRIO           1

#define RADIO_TASK_STACK        4096
#define UI_TASK_STACK           4096
#define CONSOLE_TASK_STACK      4096
#define LED_TASK_STACK          2048

#define VOLUME_DEFAULT          30

/* ============================ 共享状态 ============================ */
typedef enum {
    ADJ_TUNE = 0,       /* 编码器调节频率 */
    ADJ_VOLUME,         /* 编码器调节音量 */
    ADJ_USB,            /* SSB 上边带，编码器 1kHz 调谐 */
    ADJ_LSB,            /* SSB 下边带，编码器 1kHz 调谐 */
} adjust_mode_t;

static SemaphoreHandle_t s_status_lock = NULL;
static si4732_status_t s_status = {
    .band = SI4732_BAND_FM,
    .freq_khz = 101100,
    .volume = VOLUME_DEFAULT,
};
static volatile bool s_status_dirty = true;
static volatile bool s_radio_ready = false;
static volatile bool s_i2c_si4732 = false;
static volatile bool s_i2c_oled = false;
static volatile adjust_mode_t s_adjust_mode = ADJ_TUNE;

/* ============================ 工具函数 ============================ */

static const char *band_name(si4732_band_t band)
{
    switch (band) {
    case SI4732_BAND_FM: return "FM";
    case SI4732_BAND_AM: return "AM";
    case SI4732_BAND_SW: return "SW";
    case SI4732_BAND_LW: return "LW";
    default:             return "??";
    }
}

static const char *adjust_name(adjust_mode_t mode)
{
    switch (mode) {
    case ADJ_TUNE:   return "TUNE";
    case ADJ_VOLUME: return "VOL";
    case ADJ_USB:    return "USB";
    case ADJ_LSB:    return "LSB";
    default:         return "?";
    }
}

static const char *i2c_device_name(uint8_t addr)
{
    switch (addr) {
    case 0x11:          return "Si4732";
    case 0x63:          return "Si4732(SEN=1)";
    case 0x3C:
    case 0x3D:          return "OLED";
    default:            return "未知";
    }
}

/* 扫描 I2C 总线并按设备名打印，用于上电和 diag 命令排查 */
static void i2c_scan_report(void)
{
    uint8_t addrs[16] = {0};
    const size_t found = board_i2c_scan(addrs, sizeof(addrs));

    s_i2c_si4732 = false;
    s_i2c_oled = false;

    ESP_LOGI(TAG, "I2C 设备: %u 个", (unsigned)found);
    for (size_t i = 0; i < found && i < sizeof(addrs); i++) {
        if (addrs[i] == 0x11 || addrs[i] == 0x63) {
            s_i2c_si4732 = true;
        } else if (addrs[i] == 0x3C || addrs[i] == 0x3D) {
            s_i2c_oled = true;
        }
        ESP_LOGI(TAG, "  0x%02X  %s", addrs[i], i2c_device_name(addrs[i]));
    }
    if (found == 0) {
        ESP_LOGW(TAG, "总线上没有设备，检查供电与 SDA/SCL 接线");
    }
}

/* 进入 SSB：FM 波段先切到 SW，再让芯片加载补丁并进入边带模式 */
static void radio_enter_ssb(bool usb)
{
    if (!s_radio_ready) {
        ESP_LOGW(TAG, "Si4732 尚未就绪");
        return;
    }

    if (si4732_get_band() == SI4732_BAND_FM) {
        ESP_LOGI(TAG, "SSB 需要 AM 波段，先切换到 SW");
        if (si4732_set_band(SI4732_BAND_SW) != ESP_OK) {
            ESP_LOGE(TAG, "切换到 SW 失败");
            return;
        }
    }

    if (si4732_ssb_enable(usb) != ESP_OK) {
        ESP_LOGE(TAG, "进入 SSB %s 失败", usb ? "USB" : "LSB");
    }
}

/* 读取芯片状态并写入共享快照 */
static void status_refresh(void)
{
    if (!s_radio_ready) {
        return;
    }

    si4732_status_t st;
    if (si4732_get_status(&st) != ESP_OK) {
        return;
    }

    if (s_status_lock != NULL &&
        xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status = st;
        s_status_dirty = true;
        xSemaphoreGive(s_status_lock);
    }
}

/* 复制一份共享快照给 UI 使用 */
static bool status_snapshot(si4732_status_t *out, adjust_mode_t *mode)
{
    bool dirty = false;

    if (s_status_lock != NULL &&
        xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_status;
        *mode = s_adjust_mode;
        dirty = s_status_dirty;
        s_status_dirty = false;
        xSemaphoreGive(s_status_lock);
    }
    return dirty;
}

/* ============================ 收音机任务 ============================ */

static void radio_next_band(void)
{
    static const si4732_band_t order[] = {
        SI4732_BAND_FM, SI4732_BAND_AM, SI4732_BAND_SW, SI4732_BAND_LW,
    };
    const size_t count = sizeof(order) / sizeof(order[0]);

    size_t idx = 0;
    for (size_t i = 0; i < count; i++) {
        if (order[i] == si4732_get_band()) {
            idx = i;
            break;
        }
    }

    const si4732_band_t next = order[(idx + 1) % count];
    ESP_LOGI(TAG, "切换波段: %s", band_name(next));
    if (si4732_set_band(next) != ESP_OK) {
        ESP_LOGE(TAG, "切换波段失败");
    }
}

static void radio_task(void *arg)
{
    (void)arg;

    int fail_count = 0;

    /* 上电初始化，失败则周期性重试（方便接线排查） */
    while (!s_radio_ready) {
        if (si4732_init() == ESP_OK) {
            s_radio_ready = true;
            ESP_LOGI(TAG, "Si4732 就绪");
            if (si4732_tune(s_status.freq_khz) != ESP_OK) {
                ESP_LOGW(TAG, "初始调谐失败");
            }
            status_refresh();
        } else {
            fail_count++;
            if (fail_count == 3) {
                ESP_LOGE(TAG, "Si4732 连续初始化失败，请检查："
                              "1) SENB 是否接 GND 2) VDD 是否 3.3V "
                              "3) SDA/SCL 是否有 4.7k 上拉 4) RST 接线 "
                              "5) 模块若是晶振版，RCLK 可不接");
            }
            /* 每 5 次重试（约 10 秒）重新扫描一次总线，更新 OLED 上的在线状态 */
            if ((fail_count % 5) == 0) {
                i2c_scan_report();
            }
            ESP_LOGW(TAG, "Si4732 初始化失败，%dms 后重试", RADIO_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(RADIO_RETRY_MS));
        }
    }

    int32_t last_count = encoder_get_count();
    TickType_t last_poll = xTaskGetTickCount();

    while (1) {
        /* ---------- 编码器旋转 ---------- */
        const int32_t count = encoder_get_count();
        const int32_t delta = count - last_count;
        last_count = count;

        if (delta != 0) {
            const bool ssb_mode = (s_adjust_mode == ADJ_USB || s_adjust_mode == ADJ_LSB);

            if (s_adjust_mode == ADJ_TUNE || ssb_mode) {
                uint32_t min_khz = 0, max_khz = 0, step_khz = 0;
                si4732_band_info(si4732_get_band(), &min_khz, &max_khz, &step_khz, NULL);
                if (ssb_mode) {
                    step_khz = 1;   /* SSB 用 1kHz 步进，细调交给 BFO */
                }

                int64_t freq = (int64_t)si4732_get_frequency() + (int64_t)delta * (int64_t)step_khz;
                if (freq < (int64_t)min_khz) {
                    freq = min_khz;
                } else if (freq > (int64_t)max_khz) {
                    freq = max_khz;
                }

                if (si4732_tune((uint32_t)freq) != ESP_OK) {
                    ESP_LOGW(TAG, "调谐到 %lukHz 失败", (unsigned long)freq);
                }
            } else {
                int volume = (int)si4732_get_volume() + (int)delta;
                if (volume < SI4732_VOLUME_MIN) {
                    volume = SI4732_VOLUME_MIN;
                } else if (volume > SI4732_VOLUME_MAX) {
                    volume = SI4732_VOLUME_MAX;
                }
                if (si4732_set_volume((uint8_t)volume) != ESP_OK) {
                    ESP_LOGW(TAG, "设置音量失败");
                }
            }
            status_refresh();
        }

        /* ---------- 编码器按键 ---------- */
        const uint32_t events = encoder_take_events();

        if (events & ENCODER_EVENT_SW_SHORT) {
            if (si4732_ssb_is_active()) {
                (void)si4732_ssb_disable();
            }
            if (s_adjust_mode == ADJ_USB || s_adjust_mode == ADJ_LSB) {
                s_adjust_mode = ADJ_TUNE;
            }
            radio_next_band();
            status_refresh();
        }
        if (events & ENCODER_EVENT_SW_LONG) {
            /* 长按循环：调谐 -> 音量 -> USB -> LSB -> 调谐 */
            const adjust_mode_t next = (adjust_mode_t)(((int)s_adjust_mode + 1) % 4);
            s_adjust_mode = next;

            if (next == ADJ_USB || next == ADJ_LSB) {
                radio_enter_ssb(next == ADJ_USB);
            } else if (si4732_ssb_is_active()) {
                (void)si4732_ssb_disable();
            }

            s_status_dirty = true;
            status_refresh();
            ESP_LOGI(TAG, "编码器模式: %s", adjust_name(s_adjust_mode));
        }

        /* ---------- 周期刷新状态 ---------- */
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_poll) >= pdMS_TO_TICKS(RADIO_POLL_MS)) {
            last_poll = now;
            status_refresh();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ============================ 显示任务 ============================ */

/* 错误码缩短成 OLED 能放下的文字 */
static const char *short_error(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                   return "OK";
    case ESP_ERR_TIMEOUT:          return "TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "BAD_RESP";
    case ESP_ERR_INVALID_STATE:    return "BAD_STATE";
    case ESP_ERR_INVALID_ARG:      return "BAD_ARG";
    case ESP_ERR_NOT_FOUND:        return "NOT_FOUND";
    case ESP_FAIL:                 return "FAIL";
    default:                       return "ERR";
    }
}

static void render_status(const si4732_status_t *st, adjust_mode_t mode)
{
    u8g2_t *u = oled_get_u8g2();
    if (u == NULL) {
        return;
    }

    char line[24];

    u8g2_ClearBuffer(u);
    u8g2_SetFont(u, u8g2_font_6x10_tf);

    if (!s_radio_ready) {
        const char *stage = "?";
        esp_err_t err = ESP_OK;
        si4732_get_last_error(&stage, &err);

        u8g2_DrawStr(u, 0, 10, "Si4732 NOT READY");
        snprintf(line, sizeof(line), "SI47:%s RCLK:%s",
                 s_i2c_si4732 ? "OK" : "NO",
                 rclk_is_running() ? "ON" : "OFF");
        u8g2_DrawStr(u, 0, 24, line);
        snprintf(line, sizeof(line), "ERR %s %s", stage, short_error(err));
        u8g2_DrawStr(u, 0, 38, line);
        u8g2_DrawStr(u, 0, 52, "UART0: diag");
        oled_refresh();
        return;
    }

    if (st->ssb_active) {
        snprintf(line, sizeof(line), "%s %lukHz %s", band_name(st->band),
                 (unsigned long)st->freq_khz, st->ssb_usb ? "USB" : "LSB");
    } else if (st->band == SI4732_BAND_FM) {
        snprintf(line, sizeof(line), "%s %lu.%02luMHz", band_name(st->band),
                 (unsigned long)(st->freq_khz / 1000),
                 (unsigned long)((st->freq_khz % 1000) / 10));
    } else {
        snprintf(line, sizeof(line), "%s %lukHz", band_name(st->band),
                 (unsigned long)st->freq_khz);
    }
    u8g2_DrawStr(u, 0, 10, line);

    snprintf(line, sizeof(line), "RSSI %3u SNR %2u", (unsigned)st->rssi, (unsigned)st->snr);
    u8g2_DrawStr(u, 0, 24, line);

    snprintf(line, sizeof(line), "VOL %2u  %s", (unsigned)st->volume,
             adjust_name(mode));
    u8g2_DrawStr(u, 0, 38, line);

    if (st->ssb_active) {
        snprintf(line, sizeof(line), "BFO %+dHz", (int)st->bfo_hz);
        u8g2_DrawStr(u, 0, 52, line);
    } else {
        u8g2_DrawStr(u, 0, 52, st->valid ? "SHORT=BAND LONG=MODE" : "TUNING...");
    }

    oled_refresh();
}

static void ui_task(void *arg)
{
    (void)arg;

    TickType_t last_oled_try = 0;
    TickType_t last_diag_render = 0;

    while (1) {
        if (oled_get_u8g2() == NULL) {
            const TickType_t now = xTaskGetTickCount();
            if ((now - last_oled_try) >= pdMS_TO_TICKS(OLED_RETRY_MS)) {
                last_oled_try = now;
                if (oled_init(OLED_I2C_ADDR_DEFAULT) != ESP_OK) {
                    ESP_LOGW(TAG, "OLED 初始化失败，%dms 后重试", OLED_RETRY_MS);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(UI_TASK_PERIOD_MS));
            continue;
        }

        si4732_status_t st;
        adjust_mode_t mode;
        const bool dirty = status_snapshot(&st, &mode);
        const TickType_t now = xTaskGetTickCount();

        /* 未就绪时每 500ms 重绘一次，让 I2C/错误码变化能实时显示 */
        if (dirty || (!s_radio_ready &&
                      (now - last_diag_render) >= pdMS_TO_TICKS(500))) {
            last_diag_render = now;
            render_status(&st, mode);
        }

        vTaskDelay(pdMS_TO_TICKS(UI_TASK_PERIOD_MS));
    }
}

/* ============================ 串口调试命令 ============================ */

static void console_print_status(void)
{
    if (!s_radio_ready) {
        ESP_LOGW(TAG, "Si4732 尚未就绪");
        return;
    }

    si4732_status_t st;
    if (si4732_get_status(&st) != ESP_OK) {
        ESP_LOGE(TAG, "读取状态失败");
        return;
    }

    if (st.ssb_active) {
        ESP_LOGI(TAG, "%s %lukHz %s  RSSI %u  SNR %u  VOL %u  BFO %+dHz  %s",
                 band_name(st.band), (unsigned long)st.freq_khz,
                 st.ssb_usb ? "USB" : "LSB",
                 (unsigned)st.rssi, (unsigned)st.snr, (unsigned)st.volume,
                 (int)st.bfo_hz, st.valid ? "LOCK" : "NO LOCK");
    } else {
        ESP_LOGI(TAG, "%s %lu.%03lu MHz  RSSI %u  SNR %u  VOL %u  %s",
                 band_name(st.band),
                 (unsigned long)(st.freq_khz / 1000),
                 (unsigned long)(st.freq_khz % 1000),
                 (unsigned)st.rssi, (unsigned)st.snr, (unsigned)st.volume,
                 st.valid ? "LOCK" : "NO LOCK");
    }
}

static void console_set_band(si4732_band_t band)
{
    if (!s_radio_ready) {
        ESP_LOGW(TAG, "Si4732 尚未就绪");
        return;
    }
    if (si4732_set_band(band) != ESP_OK) {
        ESP_LOGE(TAG, "切换波段失败");
        return;
    }
    if (s_adjust_mode == ADJ_USB || s_adjust_mode == ADJ_LSB) {
        s_adjust_mode = ADJ_TUNE;
    }
    status_refresh();
    console_print_status();
}

static void console_set_ssb(bool usb)
{
    if (!s_radio_ready) {
        ESP_LOGW(TAG, "Si4732 尚未就绪");
        return;
    }

    if (si4732_get_band() == SI4732_BAND_FM) {
        ESP_LOGI(TAG, "SSB 需要 AM 波段，切换到 SW");
        if (si4732_set_band(SI4732_BAND_SW) != ESP_OK) {
            ESP_LOGE(TAG, "切换到 SW 失败");
            return;
        }
    }

    if (si4732_ssb_enable(usb) != ESP_OK) {
        ESP_LOGE(TAG, "进入 SSB 失败");
        return;
    }

    s_adjust_mode = usb ? ADJ_USB : ADJ_LSB;
    s_status_dirty = true;
    status_refresh();
    console_print_status();
}

static void console_help(void)
{
    ESP_LOGI(TAG, "命令: f101100(FM kHz) a1000(MW) s9700(SW) l200(LW)");
    ESP_LOGI(TAG, "      fm/am/sw/lw 切换波段 | v 状态 | vol40 音量 | +/- 搜台");
    ESP_LOGI(TAG, "      usb/lsb 进入 SSB | ssb0 退出 SSB | bfo150 设置BFO(Hz) | bw0..5 带宽");
    ESP_LOGI(TAG, "      i2c/diag 扫描总线并打印芯片状态");
}

static void console_handle_line(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    if (!strcmp(line, "h") || !strcmp(line, "help")) {
        console_help();
        return;
    }

    if (!strcmp(line, "v")) {
        console_print_status();
        return;
    }

    if (!strcmp(line, "i2c") || !strcmp(line, "diag")) {
        i2c_scan_report();
        ESP_LOGI(TAG, "Si4732: %s，时钟模式: %s",
                 s_radio_ready ? "已就绪" : "未就绪",
                 si4732_using_crystal() ? "板载晶振" : "外部 RCLK");
        if (s_radio_ready) {
            console_print_status();
        }
        return;
    }

    if (!strcmp(line, "fm")) { console_set_band(SI4732_BAND_FM); return; }
    if (!strcmp(line, "am")) { console_set_band(SI4732_BAND_AM); return; }
    if (!strcmp(line, "sw")) { console_set_band(SI4732_BAND_SW); return; }
    if (!strcmp(line, "lw")) { console_set_band(SI4732_BAND_LW); return; }

    if (!strcmp(line, "usb")) { console_set_ssb(true); return; }
    if (!strcmp(line, "lsb")) { console_set_ssb(false); return; }

    if (!strcmp(line, "ssb0") || !strcmp(line, "am0")) {
        if (!s_radio_ready) {
            ESP_LOGW(TAG, "Si4732 尚未就绪");
            return;
        }
        if (si4732_ssb_disable() == ESP_OK) {
            if (s_adjust_mode == ADJ_USB || s_adjust_mode == ADJ_LSB) {
                s_adjust_mode = ADJ_TUNE;
            }
            s_status_dirty = true;
            status_refresh();
            console_print_status();
        }
        return;
    }

    if (!strncmp(line, "bfo", 3)) {
        const int hz = atoi(line + 3);
        if (hz < -16383 || hz > 16383) {
            ESP_LOGW(TAG, "BFO 范围 -16383 ~ 16383 Hz");
            return;
        }
        if (s_radio_ready && si4732_ssb_set_bfo((int16_t)hz) == ESP_OK) {
            ESP_LOGI(TAG, "BFO = %+d Hz", hz);
            status_refresh();
        }
        return;
    }

    if (!strncmp(line, "bw", 2)) {
        const int bw = atoi(line + 2);
        if (bw < 0 || bw > (int)SI4732_SSB_BW_1K0) {
            ESP_LOGW(TAG, "SSB 带宽 0~5");
            return;
        }
        if (si4732_ssb_set_bandwidth((si4732_ssb_bandwidth_t)bw) == ESP_OK) {
            ESP_LOGI(TAG, "SSB 音频带宽索引 = %d", bw);
        }
        return;
    }

    if (!strncmp(line, "vol", 3)) {
        const int volume = atoi(line + 3);
        if (volume < SI4732_VOLUME_MIN || volume > SI4732_VOLUME_MAX) {
            ESP_LOGW(TAG, "音量范围 0~63");
            return;
        }
        if (s_radio_ready &&
            si4732_set_volume((uint8_t)volume) == ESP_OK) {
            ESP_LOGI(TAG, "音量 = %d", volume);
            status_refresh();
        }
        return;
    }

    if (!strcmp(line, "+") || !strcmp(line, "seek+")) {
        if (s_radio_ready) {
            (void)si4732_seek(true);
            status_refresh();
            console_print_status();
        }
        return;
    }
    if (!strcmp(line, "-") || !strcmp(line, "seek-")) {
        if (s_radio_ready) {
            (void)si4732_seek(false);
            status_refresh();
            console_print_status();
        }
        return;
    }

    /* f/a/s/l + 频率（kHz）；FM 允许只写 MHz 值，例如 f101 */
    const char band_char = (char)tolower((unsigned char)line[0]);
    if ((band_char == 'f' || band_char == 'a' ||
         band_char == 's' || band_char == 'l') && isdigit((unsigned char)line[1])) {
        uint32_t freq = (uint32_t)strtoul(line + 1, NULL, 10);
        si4732_band_t band = SI4732_BAND_FM;

        switch (band_char) {
        case 'f': band = SI4732_BAND_FM; break;
        case 'a': band = SI4732_BAND_AM; break;
        case 's': band = SI4732_BAND_SW; break;
        default:  band = SI4732_BAND_LW; break;
        }

        if (band == SI4732_BAND_FM && freq < 1000) {
            freq *= 1000;   /* f101 -> 101000kHz */
        }

        if (!s_radio_ready) {
            ESP_LOGW(TAG, "Si4732 尚未就绪");
            return;
        }
        if (si4732_set_band(band) != ESP_OK || si4732_tune(freq) != ESP_OK) {
            ESP_LOGE(TAG, "调谐失败");
            return;
        }
        if (!si4732_ssb_is_active() &&
            (s_adjust_mode == ADJ_USB || s_adjust_mode == ADJ_LSB)) {
            s_adjust_mode = ADJ_TUNE;
        }
        status_refresh();
        console_print_status();
        return;
    }

    ESP_LOGW(TAG, "未知命令，输入 h 查看帮助");
}

static void console_task(void *arg)
{
    (void)arg;

    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    const esp_err_t err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        (void)uart_param_config(UART_NUM_0, &cfg);
    } else {
        ESP_LOGW(TAG, "UART0 驱动安装失败(%s)，串口命令不可用", esp_err_to_name(err));
    }

    char line[32];
    size_t len = 0;

    while (1) {
        uint8_t ch = 0;
        const int n = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(20));

        if (n <= 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = '\0';
                console_handle_line(line);
                len = 0;
            }
            continue;
        }
        if (ch == '\b' || ch == 127) {
            if (len > 0) {
                len--;
            }
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = (char)ch;
        }
    }
}

/* ============================ LED ============================ */

static void led_task(void *arg)
{
    (void)arg;

    led_init();

    while (1) {
        led_toggle();
        vTaskDelay(pdMS_TO_TICKS(LED_TASK_PERIOD_MS));
    }
}

/* ============================ 入口 ============================ */

void app_main(void)
{
    ESP_LOGI(TAG, "Si4732 收音机启动（ESP32-C3）");
    ESP_LOGI(TAG, "WiFi/BT 已从固件排除（最小构建），射频保持关闭");

    s_status_lock = xSemaphoreCreateMutex();
    if (s_status_lock == NULL) {
        ESP_LOGE(TAG, "创建状态互斥锁失败");
        return;
    }

    /* 1. 参考时钟必须先于 Si4732 上电 */
    if (rclk_start() != ESP_OK) {
        ESP_LOGE(TAG, "32.768kHz 参考时钟启动失败");
    }

    /* 2. 共用 I2C 总线（Si4732 + OLED） */
    if (board_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败");
    }

    /* 3. 上电扫描一次，方便确认接线 */
    i2c_scan_report();

    /* 4. 外设初始化（失败不阻塞启动，各任务内部会重试） */
    if (oled_init(OLED_I2C_ADDR_DEFAULT) != ESP_OK) {
        ESP_LOGW(TAG, "OLED 初始化失败");
    }
    if (encoder_init() != ESP_OK) {
        ESP_LOGE(TAG, "编码器初始化失败");
    }

    /* 5. 启动任务 */
    xTaskCreate(radio_task, "radio", RADIO_TASK_STACK, NULL, RADIO_TASK_PRIO, NULL);
    xTaskCreate(ui_task, "ui", UI_TASK_STACK, NULL, UI_TASK_PRIO, NULL);
    xTaskCreate(console_task, "console", CONSOLE_TASK_STACK, NULL, CONSOLE_TASK_PRIO, NULL);
    xTaskCreate(led_task, "led", LED_TASK_STACK, NULL, LED_TASK_PRIO, NULL);

    ESP_LOGI(TAG, "任务已创建: radio/ui/console/led");
}
