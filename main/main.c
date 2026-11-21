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
#include "nvs_flash.h"
#include "u8g2.h"

#include "board.h"
#include "board_i2c.h"
#include "encoder.h"
#include "led.h"
#include "oled_display.h"
#include "rclk.h"
#include "settings.h"
#include "si4732.h"

static const char *TAG = "app";

/* ============================ 可调参数 ============================ */
#define UI_TASK_PERIOD_MS       20      /* UI 任务轮询周期 */
#define UI_REDRAW_MS            20      /* 目标刷新间隔（受 I2C 带宽限制） */
#define RADIO_POLL_MS           50      /* 状态轮询周期 */
#define SETTINGS_SAVE_DELAY_MS  2000
#define LED_TASK_PERIOD_MS      500
#define RADIO_RETRY_MS          2000
#define OLED_RETRY_MS           2000

#define RADIO_TASK_PRIO         6
#define UI_TASK_PRIO            3
#define CONSOLE_TASK_PRIO       2
#define LED_TASK_PRIO           1

#define RADIO_TASK_STACK        4096
#define UI_TASK_STACK           4096
#define CONSOLE_TASK_STACK      4096
#define LED_TASK_STACK          2048

#define VOLUME_DEFAULT          30

/* ============================ 主页字段 ============================ */
typedef enum {
    FIELD_FREQ = 0,
    FIELD_BAND,
    FIELD_VOLUME,
    FIELD_BW,
    FIELD_SSB,
    FIELD_BFO,
    FIELD_MUTE,
    FIELD_COUNT
} field_t;

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

static volatile field_t s_field = FIELD_FREQ;
static volatile bool s_editing = false;
static volatile uint8_t s_step_idx = 0;

static volatile bool s_muted = false;

static volatile bool s_settings_dirty = false;
static TickType_t s_settings_last_change = 0;
static radio_settings_t s_boot_settings;

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

static const char *field_name(field_t f)
{
    switch (f) {
    case FIELD_FREQ:   return "FREQ";
    case FIELD_BAND:   return "BAND";
    case FIELD_VOLUME: return "VOL";
    case FIELD_BW:     return "BW";
    case FIELD_SSB:    return "SSB";
    case FIELD_BFO:    return "BFO";
    case FIELD_MUTE:   return "MUTE";
    default:           return "?";
    }
}

static const char *ssb_name(void)
{
    if (!si4732_ssb_is_active()) {
        return "OFF";
    }
    return si4732_ssb_is_usb() ? "USB" : "LSB";
}

static const char *bw_text(uint8_t idx)
{
    static const char *names[6] = { "1.2k", "2.2k", "3k", "4k", "500", "1k" };
    return (idx < 6) ? names[idx] : "?";
}

/* 当前波段/模式可用步进表（Hz） */
static const uint32_t *step_table_current(uint8_t *count)
{
    static const uint32_t fm[] = { 10000000, 1000000, 100000, 10000 };
    static const uint32_t lw[] = { 100000, 10000, 1000 };
    static const uint32_t am[] = { 100000, 10000, 1000 };
    static const uint32_t sw_am[] = { 10000000, 1000000, 100000, 10000, 1000 };
    static const uint32_t sw_ssb[] = { 10000000, 1000000, 100000, 10000,
                                       1000, 100, 10, 1 };

    switch (si4732_get_band()) {
    case SI4732_BAND_FM:
        *count = sizeof(fm) / sizeof(fm[0]);
        return fm;
    case SI4732_BAND_LW:
        *count = sizeof(lw) / sizeof(lw[0]);
        return lw;
    case SI4732_BAND_SW:
        if (si4732_ssb_is_active()) {
            *count = sizeof(sw_ssb) / sizeof(sw_ssb[0]);
            return sw_ssb;
        }
        *count = sizeof(sw_am) / sizeof(sw_am[0]);
        return sw_am;
    default:
        *count = sizeof(am) / sizeof(am[0]);
        return am;
    }
}

static uint8_t step_count_current(void)
{
    uint8_t count = 0;
    (void)step_table_current(&count);
    return count;
}

static uint32_t step_hz_current(void)
{
    uint8_t count = 0;
    const uint32_t *table = step_table_current(&count);

    uint8_t idx = s_step_idx;
    if (idx >= count) {
        idx = count - 1;
    }
    return table[idx];
}

static const char *step_text(uint32_t hz)
{
    switch (hz) {
    case 10000000: return "10M";
    case 1000000:  return "1M";
    case 100000:   return "100k";
    case 10000:    return "10k";
    case 1000:     return "1k";
    case 100:      return "100";
    case 10:       return "10";
    default:       return "1";
    }
}

/* 频率数值（不含波段前缀，供大字显示）：FM/SW 用 MHz，AM/LW 用 kHz */
static void format_freq_value(char *buf, size_t len, const si4732_status_t *st)
{
    if (st->band == SI4732_BAND_FM) {
        snprintf(buf, len, "%lu.%03luMHz",
                 (unsigned long)(st->freq_khz / 1000),
                 (unsigned long)(st->freq_khz % 1000));
    } else if (st->band == SI4732_BAND_SW) {
        int64_t hz = (int64_t)st->freq_khz * 1000;
        if (st->ssb_active) {
            hz += st->bfo_hz;
        }
        if (hz < 0) {
            hz = 0;
        }
        snprintf(buf, len, "%lu.%06luMHz",
                 (unsigned long)(hz / 1000000),
                 (unsigned long)(hz % 1000000));
    } else {
        snprintf(buf, len, "%lukHz", (unsigned long)st->freq_khz);
    }
}

/* 当前步进对应频率字符串中的第几个字符（用于大字频率选位） */
static int freq_digit_index(const si4732_status_t *st, uint32_t step)
{
    char buf[20];
    format_freq_value(buf, sizeof(buf), st);
    const char *dot = strchr(buf, '.');

    if (dot != NULL) {
        /* 10M / 1M 是整数位，位于小数点左侧 */
        if (step >= 1000000) {
            return (int)(dot - buf) - ((step == 10000000) ? 2 : 1);
        }

        int frac = 5;
        switch (step) {
        case 100000: frac = 0; break;
        case 10000:  frac = 1; break;
        case 1000:   frac = 2; break;
        case 100:    frac = 3; break;
        case 10:     frac = 4; break;
        default:     frac = 5; break;
        }
        return (int)(dot - buf) + 1 + frac;
    }

    const int len = (int)strlen(buf);
    int back = 1;
    switch (step) {
    case 100000: back = 3; break;
    case 10000:  back = 2; break;
    default:     back = 1; break;
    }
    return len - 3 - back;   /* "kHz" 占 3 个字符 */
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

static void i2c_scan_report(void)
{
    if (si4732_is_patching()) {
        ESP_LOGW(TAG, "补丁下载中，跳过 I2C 扫描");
        return;
    }

    if (!board_i2c_lines_idle()) {
        ESP_LOGE(TAG, "I2C 空闲电平异常（SDA=%d SCL=%d），总线被拉低或与 GND 短路",
                 gpio_get_level(BOARD_I2C_SDA_GPIO),
                 gpio_get_level(BOARD_I2C_SCL_GPIO));
        s_i2c_si4732 = false;
        s_i2c_oled = false;
        return;
    }

    uint8_t si_addr = 0;
    if (board_i2c_probe_addr(0x11)) {
        si_addr = 0x11;
    } else if (board_i2c_probe_addr(0x63)) {
        si_addr = 0x63;
    }

    uint8_t oled_addr = 0;
    if (board_i2c_probe_addr(0x3C)) {
        oled_addr = 0x3C;
    } else if (board_i2c_probe_addr(0x3D)) {
        oled_addr = 0x3D;
    }

    s_i2c_si4732 = (si_addr != 0);
    s_i2c_oled = (oled_addr != 0);

    if (si_addr != 0) {
        ESP_LOGI(TAG, "I2C 发现 %s: 0x%02X", i2c_device_name(si_addr), si_addr);
    } else {
        ESP_LOGW(TAG, "I2C 未发现 Si4732（0x11/0x63）");
    }

    if (oled_addr != 0) {
        ESP_LOGI(TAG, "I2C 发现 %s: 0x%02X", i2c_device_name(oled_addr), oled_addr);
    } else {
        ESP_LOGW(TAG, "I2C 未发现 OLED（0x3C/0x3D）");
    }
}

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

static void settings_mark_dirty(void)
{
    s_settings_last_change = xTaskGetTickCount();
    s_settings_dirty = true;
}

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

static bool status_snapshot(si4732_status_t *out)
{
    bool dirty = false;

    if (s_status_lock != NULL &&
        xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_status;
        dirty = s_status_dirty;
        s_status_dirty = false;
        xSemaphoreGive(s_status_lock);
    }
    return dirty;
}

/* ============================ 收音机操作 ============================ */

static void radio_set_mute(bool mute)
{
    s_muted = mute;
    (void)si4732_set_mute(mute);
    settings_mark_dirty();
}

static void adjust_frequency(int delta)
{
    const uint32_t step = step_hz_current();

    if (step >= 1000) {
        uint32_t min_khz = 0, max_khz = 0;
        si4732_band_info(si4732_get_band(), &min_khz, &max_khz, NULL, NULL);

        int64_t freq = (int64_t)si4732_get_frequency() +
                       (int64_t)delta * (int64_t)(step / 1000);
        if (freq < (int64_t)min_khz) {
            freq = min_khz;
        } else if (freq > (int64_t)max_khz) {
            freq = max_khz;
        }

        if (si4732_tune((uint32_t)freq) == ESP_OK) {
            settings_mark_dirty();
        }
    } else if (si4732_ssb_is_active()) {
        /* 1kHz 以下用 BFO 做细调 */
        int bfo = (int)si4732_ssb_get_bfo() + delta * (int)step;
        if (bfo < -16383) {
            bfo = -16383;
        } else if (bfo > 16383) {
            bfo = 16383;
        }
        (void)si4732_ssb_set_bfo((int16_t)bfo);
    }
}

static void adjust_volume(int delta)
{
    int volume = (int)si4732_get_volume() + delta;
    if (volume < SI4732_VOLUME_MIN) {
        volume = SI4732_VOLUME_MIN;
    } else if (volume > SI4732_VOLUME_MAX) {
        volume = SI4732_VOLUME_MAX;
    }
    if (si4732_set_volume((uint8_t)volume) == ESP_OK) {
        settings_mark_dirty();
    }
}

static void adjust_band(int delta)
{
    static const si4732_band_t order[] = {
        SI4732_BAND_FM, SI4732_BAND_AM, SI4732_BAND_SW, SI4732_BAND_LW,
    };
    const int count = (int)(sizeof(order) / sizeof(order[0]));

    int idx = 0;
    for (int i = 0; i < count; i++) {
        if (order[i] == si4732_get_band()) {
            idx = i;
            break;
        }
    }

    idx = (idx + ((delta > 0) ? 1 : count - 1)) % count;
    if (si4732_set_band(order[idx]) == ESP_OK) {
        settings_mark_dirty();
    }
    s_step_idx = 0;
}

static void adjust_bw(int delta)
{
    int bw = (int)si4732_ssb_get_bandwidth() + ((delta > 0) ? 1 : -1);
    if (bw < 0) {
        bw = 5;
    } else if (bw > 5) {
        bw = 0;
    }
    (void)si4732_ssb_set_bandwidth((si4732_ssb_bandwidth_t)bw);
}

static void adjust_ssb(int delta)
{
    const bool forward = (delta > 0);

    if (!si4732_ssb_is_active()) {
        if (si4732_get_band() == SI4732_BAND_FM) {
            if (si4732_set_band(SI4732_BAND_SW) != ESP_OK) {
                return;
            }
            settings_mark_dirty();
        }
        if (si4732_ssb_enable(forward) != ESP_OK) {
            ESP_LOGE(TAG, "进入 SSB 失败");
        }
    } else if (si4732_ssb_is_usb()) {
        if (forward) {
            (void)si4732_ssb_disable();
        } else if (si4732_ssb_enable(false) != ESP_OK) {
            ESP_LOGE(TAG, "切换到 LSB 失败");
        }
    } else {
        if (forward) {
            if (si4732_ssb_enable(true) != ESP_OK) {
                ESP_LOGE(TAG, "切换到 USB 失败");
            }
        } else {
            (void)si4732_ssb_disable();
        }
    }

    const uint8_t n = step_count_current();
    if (s_step_idx >= n) {
        s_step_idx = 0;
    }
}

static void adjust_bfo(int delta)
{
    int bfo = (int)si4732_ssb_get_bfo() + delta * 10;
    if (bfo < -16383) {
        bfo = -16383;
    } else if (bfo > 16383) {
        bfo = 16383;
    }
    (void)si4732_ssb_set_bfo((int16_t)bfo);
}

static void radio_apply_boot_settings(void)
{
    if (s_boot_settings.volume > SI4732_VOLUME_MAX) {
        s_boot_settings.volume = VOLUME_DEFAULT;
    }
    (void)si4732_set_volume(s_boot_settings.volume);

    s_muted = s_boot_settings.mute;
    (void)si4732_set_mute(s_muted);

    si4732_band_t band = (si4732_band_t)s_boot_settings.band;
    if (band >= SI4732_BAND_COUNT) {
        band = SI4732_BAND_FM;
    }

    if (si4732_set_band(band) != ESP_OK) {
        ESP_LOGW(TAG, "恢复波段 %s 失败", band_name(band));
    }

    if (si4732_tune(s_boot_settings.freq_khz) != ESP_OK) {
        uint32_t default_khz = 0;
        si4732_band_info(band, NULL, NULL, NULL, &default_khz);
        ESP_LOGW(TAG, "保存的频率 %lukHz 不在 %s 范围，改用 %lukHz",
                 (unsigned long)s_boot_settings.freq_khz,
                 band_name(band), (unsigned long)default_khz);
        (void)si4732_tune(default_khz);
    }

    ESP_LOGI(TAG, "已恢复设置: %s %lukHz VOL %u %s",
             band_name(si4732_get_band()),
             (unsigned long)si4732_get_frequency(),
             (unsigned)si4732_get_volume(),
             s_muted ? "MUTE" : "UNMUTE");
}

static void radio_save_settings(void)
{
    if (!s_radio_ready) {
        return;
    }

    const radio_settings_t s = {
        .band = (uint8_t)si4732_get_band(),
        .freq_khz = si4732_get_frequency(),
        .volume = si4732_get_volume(),
        .mute = s_muted,
    };

    if (s.freq_khz == 0) {
        ESP_LOGW(TAG, "当前频率为 0，跳过本次设置保存");
        s_settings_dirty = false;
        return;
    }

    if (settings_save(&s)) {
        ESP_LOGI(TAG, "设置已保存: %s %lukHz VOL %u %s",
                 band_name(si4732_get_band()),
                 (unsigned long)s.freq_khz,
                 (unsigned)s.volume,
                 s.mute ? "MUTE" : "UNMUTE");
    } else {
        ESP_LOGW(TAG, "设置保存失败");
    }
    s_settings_dirty = false;
}

/* ============================ 收音机任务 ============================ */

static void radio_task(void *arg)
{
    (void)arg;

    int fail_count = 0;

    while (!s_radio_ready) {
        if (si4732_init() == ESP_OK) {
            s_radio_ready = true;
            ESP_LOGI(TAG, "Si4732 就绪");
            radio_apply_boot_settings();
            status_refresh();
        } else {
            fail_count++;
            if (fail_count == 3) {
                ESP_LOGE(TAG, "Si4732 连续初始化失败，请检查："
                              "1) SENB 是否接 GND 2) VDD 是否 3.3V "
                              "3) SDA/SCL 是否 4.7k 上拉 4) RST 接线");
            }
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
        /* ---------- 旋转：选字段 / 改值 ---------- */
        const int32_t count = encoder_get_count();
        const int32_t delta = count - last_count;
        last_count = count;

        if (delta != 0) {
            if (!s_editing) {
                int f = (int)s_field + ((delta > 0) ? 1 : -1);
                if (f < 0) {
                    f = FIELD_COUNT - 1;
                } else if (f >= FIELD_COUNT) {
                    f = 0;
                }
                s_field = (field_t)f;
            } else {
                switch (s_field) {
                case FIELD_FREQ:   adjust_frequency((int)delta); break;
                case FIELD_BAND:   adjust_band((int)delta); break;
                case FIELD_VOLUME: adjust_volume((int)delta); break;
                case FIELD_BW:     adjust_bw((int)delta); break;
                case FIELD_SSB:    adjust_ssb((int)delta); break;
                case FIELD_BFO:    adjust_bfo((int)delta); break;
                case FIELD_MUTE:   radio_set_mute(delta > 0); break;
                default: break;
                }
            }

            s_status_dirty = true;
            status_refresh();
        }

        /* ---------- 按键 ---------- */
        const uint32_t events = encoder_take_events();

        if (events & ENCODER_EVENT_SW_DOUBLE) {
            /* 频率编辑时双击循环步进 */
            if (s_editing && s_field == FIELD_FREQ) {
                const uint8_t n = step_count_current();
                s_step_idx = (uint8_t)((s_step_idx + 1) % (n ? n : 1));
            }
            s_status_dirty = true;
            status_refresh();
        }

        if (events & ENCODER_EVENT_SW_SHORT) {
            if (!s_editing) {
                s_editing = true;
                if (s_field == FIELD_FREQ) {
                    const uint8_t n = step_count_current();
                    if (s_step_idx >= n) {
                        s_step_idx = 0;
                    }
                }
            } else {
                s_editing = false;
            }
            s_status_dirty = true;
            status_refresh();
        }

        if (events & ENCODER_EVENT_SW_LONG) {
            s_editing = false;
            s_field = FIELD_FREQ;
            s_status_dirty = true;
            status_refresh();
        }

        /* ---------- 周期刷新 ---------- */
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_poll) >= pdMS_TO_TICKS(RADIO_POLL_MS)) {
            last_poll = now;
            status_refresh();
        }

        /* ---------- 设置延时保存 ---------- */
        if (s_settings_dirty &&
            (now - s_settings_last_change) >= pdMS_TO_TICKS(SETTINGS_SAVE_DELAY_MS)) {
            radio_save_settings();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ============================ 显示 ============================ */

static void render_not_ready(u8g2_t *u)
{
    const char *stage = "?";
    esp_err_t err = ESP_OK;
    si4732_get_last_error(&stage, &err);

    char line[24];
    u8g2_SetFont(u, u8g2_font_6x10_tf);
    u8g2_DrawStr(u, 0, 10, "Si4732 NOT READY");
    snprintf(line, sizeof(line), "SI47:%s RCLK:%s",
             s_i2c_si4732 ? "OK" : "NO",
             rclk_is_running() ? "ON" : "OFF");
    u8g2_DrawStr(u, 0, 24, line);
    snprintf(line, sizeof(line), "ERR %s %s", stage, short_error(err));
    u8g2_DrawStr(u, 0, 38, line);
    u8g2_DrawStr(u, 0, 52, "UART0: diag");
}

static void render_ui(const si4732_status_t *st)
{
    u8g2_t *u = oled_get_u8g2();
    if (u == NULL) {
        return;
    }

    u8g2_ClearBuffer(u);

    if (!s_radio_ready) {
        render_not_ready(u);
        oled_refresh();
        return;
    }

    char value[20];
    char line[28];

    /* ---------- 上区：大字频率 ---------- */
    u8g2_SetFont(u, u8g2_font_10x20_tf);
    format_freq_value(value, sizeof(value), st);

    const int w = (int)u8g2_GetStrWidth(u, value);
    int x = (OLED_WIDTH - w) / 2;
    if (x < 0) {
        x = 0;
    }
    u8g2_DrawStr(u, (u8g2_uint_t)x, 20, value);

    if (s_field == FIELD_FREQ) {
        u8g2_SetDrawColor(u, 2);
        if (s_editing) {
            /* 选中后：反白当前要调的那一位 */
            const int idx = freq_digit_index(st, step_hz_current());
            int cx = x + idx * 10;
            if (cx < 0) {
                cx = 0;
            } else if (cx > OLED_WIDTH - 10) {
                cx = OLED_WIDTH - 10;
            }
            u8g2_DrawBox(u, (u8g2_uint_t)cx, 2, 10, 18);
        } else {
            /* 选中前：反白第一个字符 */
            u8g2_DrawBox(u, (u8g2_uint_t)x, 2, 10, 18);
        }
        u8g2_SetDrawColor(u, 1);
    }

    /* ---------- 中区：状态行 ---------- */
    u8g2_SetFont(u, u8g2_font_6x10_tf);
    if (s_editing && s_field == FIELD_FREQ) {
        snprintf(line, sizeof(line), "%s %s  STEP %s",
                 band_name(st->band), ssb_name(),
                 step_text(step_hz_current()));
    } else if (s_editing) {
        snprintf(line, sizeof(line), "%s %s  EDIT %s",
                 band_name(st->band), ssb_name(), field_name(s_field));
    } else {
        snprintf(line, sizeof(line), "%s %s%s  R%u S%u",
                 band_name(st->band), ssb_name(),
                 s_muted ? " M" : "",
                 (unsigned)st->rssi, (unsigned)st->snr);
    }
    u8g2_DrawStr(u, 0, 31, line);
    u8g2_DrawHLine(u, 0, 34, OLED_WIDTH);

    /* ---------- 下区：3 行 × 2 列，六个字段同时显示 ---------- */
    static const field_t left_fields[3] = {
        FIELD_BAND, FIELD_BW, FIELD_BFO,
    };
    static const field_t right_fields[3] = {
        FIELD_VOLUME, FIELD_SSB, FIELD_MUTE,
    };

    for (int row = 0; row < 3; row++) {
        const uint8_t y = (uint8_t)(42 + row * 10);

        for (int col = 0; col < 2; col++) {
            const field_t f = (col == 0) ? left_fields[row] : right_fields[row];
            const uint8_t x = (col == 0) ? 0 : 64;
            const bool selected = (s_field == f);
            char cell[16];

            switch (f) {
            case FIELD_BAND:
                snprintf(cell, sizeof(cell), "BAND %s", band_name(st->band));
                break;
            case FIELD_BW:
                snprintf(cell, sizeof(cell), "BW   %s",
                         bw_text((uint8_t)si4732_ssb_get_bandwidth()));
                break;
            case FIELD_BFO:
                snprintf(cell, sizeof(cell), "BFO %+d", (int)st->bfo_hz);
                break;
            case FIELD_VOLUME:
                snprintf(cell, sizeof(cell), "VOL  %u", (unsigned)st->volume);
                break;
            case FIELD_SSB:
                snprintf(cell, sizeof(cell), "SSB  %s", ssb_name());
                break;
            default:
                snprintf(cell, sizeof(cell), "MUTE %s", s_muted ? "ON" : "OFF");
                break;
            }

            u8g2_DrawStr(u, x, y, cell);

            if (selected) {
                u8g2_SetDrawColor(u, 2);
                if (s_editing) {
                    /* 选中后：反白整个半屏单元格 */
                    u8g2_DrawBox(u, x, (uint8_t)(y - 8), 64, 10);
                } else {
                    /* 选中前：反白第一个字符 */
                    u8g2_DrawBox(u, x, (uint8_t)(y - 8), 6, 10);
                }
                u8g2_SetDrawColor(u, 1);
            }
        }
    }

    oled_refresh();
}

static void ui_task(void *arg)
{
    (void)arg;

    TickType_t last_oled_try = 0;
    TickType_t last_render = 0;

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
        const bool dirty = status_snapshot(&st);
        const TickType_t now = xTaskGetTickCount();

        if (dirty || (now - last_render) >= pdMS_TO_TICKS(UI_REDRAW_MS)) {
            last_render = now;
            render_ui(&st);
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
        ESP_LOGI(TAG, "%s %lukHz %s  RSSI %u  SNR %u  VOL %u  BFO %+dHz  BW %s  %s",
                 band_name(st.band), (unsigned long)st.freq_khz,
                 st.ssb_usb ? "USB" : "LSB",
                 (unsigned)st.rssi, (unsigned)st.snr, (unsigned)st.volume,
                 (int)st.bfo_hz,
                 bw_text((uint8_t)si4732_ssb_get_bandwidth()),
                 !st.tuned ? "TUNING" : (st.valid ? "LOCK" : "NO SIGNAL"));
    } else {
        ESP_LOGI(TAG, "%s %lu.%03lu MHz  RSSI %u  SNR %u  VOL %u  %s",
                 band_name(st.band),
                 (unsigned long)(st.freq_khz / 1000),
                 (unsigned long)(st.freq_khz % 1000),
                 (unsigned)st.rssi, (unsigned)st.snr, (unsigned)st.volume,
                 !st.tuned ? "TUNING" : (st.valid ? "LOCK" : "NO SIGNAL"));
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
    settings_mark_dirty();
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
        settings_mark_dirty();
    }

    if (si4732_ssb_enable(usb) != ESP_OK) {
        ESP_LOGE(TAG, "进入 SSB 失败");
        return;
    }

    s_status_dirty = true;
    status_refresh();
    console_print_status();
}

static void console_help(void)
{
    ESP_LOGI(TAG, "命令: f101100(FM kHz) a1000(MW) s9700(SW) l200(LW)");
    ESP_LOGI(TAG, "      fm/am/sw/lw 切换波段 | v 状态 | vol40 音量 | mute/mute0/mute1");
    ESP_LOGI(TAG, "      usb/lsb 进入 SSB | ssb0 退出 SSB | bfo150 设置BFO(Hz) | bw0..5 带宽");
    ESP_LOGI(TAG, "      i2c/diag 扫描总线 | save 立即保存设置");
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

    if (!strcmp(line, "save")) {
        if (s_radio_ready) {
            radio_save_settings();
        }
        return;
    }

    if (!strncmp(line, "mute", 4)) {
        if (!s_radio_ready) {
            ESP_LOGW(TAG, "Si4732 尚未就绪");
            return;
        }
        bool mute = !s_muted;
        if (line[4] == '0') {
            mute = false;
        } else if (line[4] == '1') {
            mute = true;
        }
        radio_set_mute(mute);
        ESP_LOGI(TAG, "静音 = %s", mute ? "ON" : "OFF");
        status_refresh();
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
            status_refresh();
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
            settings_mark_dirty();
            status_refresh();
        }
        return;
    }

    if (!strcmp(line, "+") || !strcmp(line, "seek+")) {
        if (s_radio_ready) {
            (void)si4732_seek(true);
            settings_mark_dirty();
            status_refresh();
            console_print_status();
        }
        return;
    }
    if (!strcmp(line, "-") || !strcmp(line, "seek-")) {
        if (s_radio_ready) {
            (void)si4732_seek(false);
            settings_mark_dirty();
            status_refresh();
            console_print_status();
        }
        return;
    }

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
            freq *= 1000;
        }

        if (!s_radio_ready) {
            ESP_LOGW(TAG, "Si4732 尚未就绪");
            return;
        }
        if (si4732_set_band(band) != ESP_OK || si4732_tune(freq) != ESP_OK) {
            ESP_LOGE(TAG, "调谐失败");
            return;
        }
        settings_mark_dirty();
        s_status_dirty = true;
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

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败，设置不会保存");
    }

    settings_defaults(&s_boot_settings);
    if (!settings_load(&s_boot_settings)) {
        ESP_LOGI(TAG, "使用默认设置");
    }

    if (rclk_start() != ESP_OK) {
        ESP_LOGE(TAG, "32.768kHz 参考时钟启动失败");
    }

    if (board_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败");
    }

    (void)si4732_reset();
    vTaskDelay(pdMS_TO_TICKS(10));

    i2c_scan_report();

    if (oled_init(OLED_I2C_ADDR_DEFAULT) != ESP_OK) {
        ESP_LOGW(TAG, "OLED 初始化失败");
    }
    if (encoder_init() != ESP_OK) {
        ESP_LOGE(TAG, "编码器初始化失败");
    }

    xTaskCreate(radio_task, "radio", RADIO_TASK_STACK, NULL, RADIO_TASK_PRIO, NULL);
    xTaskCreate(ui_task, "ui", UI_TASK_STACK, NULL, UI_TASK_PRIO, NULL);
    xTaskCreate(console_task, "console", CONSOLE_TASK_STACK, NULL, CONSOLE_TASK_PRIO, NULL);
    xTaskCreate(led_task, "led", LED_TASK_STACK, NULL, LED_TASK_PRIO, NULL);

    ESP_LOGI(TAG, "任务已创建: radio/ui/console/led");
}
