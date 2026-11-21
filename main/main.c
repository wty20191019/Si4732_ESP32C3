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
#define UI_TASK_PERIOD_MS       50      /* UI 刷新检查周期 */
#define UI_REDRAW_MS            500     /* 主页定期重绘（RSSI 等实时值） */
#define RADIO_POLL_MS           500     /* 状态轮询周期 */
#define SETTINGS_SAVE_DELAY_MS  2000    /* 设置变更后延时保存，减少 NVS 擦写 */
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

/* ============================ UI 状态 ============================ */
typedef enum {
    SCREEN_HOME = 0,    /* 主页：频率 + 信号条 + 音量条 */
    SCREEN_MENU,        /* 长按进入的设置菜单 */
    SCREEN_VOLUME,      /* 音量调整 */
    SCREEN_BFO,         /* SSB BFO 调整 */
} screen_t;

enum {
    MENU_VOLUME = 0,
    MENU_MUTE,
    MENU_SSB,
    MENU_BFO,
    MENU_EXIT,
    MENU_ITEM_COUNT
};

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

static volatile screen_t s_screen = SCREEN_HOME;
static volatile uint8_t s_menu_sel = MENU_VOLUME;
static volatile bool s_muted = false;

/* 设置持久化：改变后 2 秒自动写 NVS */
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

static const char *ssb_name(void)
{
    if (!si4732_ssb_is_active()) {
        return "OFF";
    }
    return si4732_ssb_is_usb() ? "USB" : "LSB";
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
    /* 补丁下载期间总线被长时间占用，不扫描 */
    if (si4732_is_patching()) {
        ESP_LOGW(TAG, "补丁下载中，跳过 I2C 扫描");
        return;
    }

    /* 先看空闲电平：SDA/SCL 被拉低或短路时直接报错，不做全地址扫描 */
    if (!board_i2c_lines_idle()) {
        ESP_LOGE(TAG, "I2C 空闲电平异常（SDA=%d SCL=%d），总线被拉低或与 GND 短路",
                 gpio_get_level(BOARD_I2C_SDA_GPIO),
                 gpio_get_level(BOARD_I2C_SCL_GPIO));
        s_i2c_si4732 = false;
        s_i2c_oled = false;
        return;
    }

    /* 本板只有两个 I2C 设备，直接探测已知地址，不做 0x08~0x77 全扫描 */
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

/* SSB 菜单项循环：OFF -> USB -> LSB -> OFF */
static void radio_cycle_ssb(void)
{
    if (!si4732_ssb_is_active()) {
        if (si4732_get_band() == SI4732_BAND_FM) {
            ESP_LOGI(TAG, "SSB 需要 AM 波段，切换到 SW");
            if (si4732_set_band(SI4732_BAND_SW) != ESP_OK) {
                ESP_LOGE(TAG, "切换到 SW 失败");
                return;
            }
            settings_mark_dirty();
        }
        if (si4732_ssb_enable(true) != ESP_OK) {
            ESP_LOGE(TAG, "进入 SSB USB 失败");
        }
    } else if (si4732_ssb_is_usb()) {
        if (si4732_ssb_enable(false) != ESP_OK) {
            ESP_LOGE(TAG, "切换到 SSB LSB 失败");
        }
    } else {
        if (si4732_ssb_disable() != ESP_OK) {
            ESP_LOGE(TAG, "退出 SSB 失败");
        }
    }
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

    /* 上电初始化，失败则周期性重试（方便接线排查） */
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
                              "3) SDA/SCL 是否有 4.7k 上拉 4) RST 接线 "
                              "5) 模块若是晶振版，RCLK 可不接");
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
        /* ---------- 编码器旋转 ---------- */
        const int32_t count = encoder_get_count();
        const int32_t delta = count - last_count;
        last_count = count;

        if (delta != 0) {
            switch (s_screen) {
            case SCREEN_HOME: {
                uint32_t min_khz = 0, max_khz = 0, step_khz = 0;
                si4732_band_info(si4732_get_band(), &min_khz, &max_khz, &step_khz, NULL);
                if (si4732_ssb_is_active()) {
                    step_khz = 1;   /* SSB 用 1kHz 步进，细调交给 BFO */
                }

                int64_t freq = (int64_t)si4732_get_frequency() +
                               (int64_t)delta * (int64_t)step_khz;
                if (freq < (int64_t)min_khz) {
                    freq = min_khz;
                } else if (freq > (int64_t)max_khz) {
                    freq = max_khz;
                }

                if (si4732_tune((uint32_t)freq) == ESP_OK) {
                    settings_mark_dirty();
                } else {
                    ESP_LOGW(TAG, "调谐到 %lukHz 失败", (unsigned long)freq);
                }
                break;
            }

            case SCREEN_VOLUME: {
                int volume = (int)si4732_get_volume() + (int)delta;
                if (volume < SI4732_VOLUME_MIN) {
                    volume = SI4732_VOLUME_MIN;
                } else if (volume > SI4732_VOLUME_MAX) {
                    volume = SI4732_VOLUME_MAX;
                }
                if (si4732_set_volume((uint8_t)volume) == ESP_OK) {
                    settings_mark_dirty();
                }
                break;
            }

            case SCREEN_BFO: {
                int bfo = (int)si4732_ssb_get_bfo() + (int)delta * 10;
                if (bfo < -16383) {
                    bfo = -16383;
                } else if (bfo > 16383) {
                    bfo = 16383;
                }
                (void)si4732_ssb_set_bfo((int16_t)bfo);
                break;
            }

            case SCREEN_MENU: {
                int sel = (int)s_menu_sel + (delta > 0 ? 1 : -1);
                if (sel < 0) {
                    sel = MENU_ITEM_COUNT - 1;
                } else if (sel >= MENU_ITEM_COUNT) {
                    sel = 0;
                }
                s_menu_sel = (uint8_t)sel;
                break;
            }
            }

            s_status_dirty = true;
            status_refresh();
        }

        /* ---------- 编码器按键 ---------- */
        const uint32_t events = encoder_take_events();

        if (events & ENCODER_EVENT_SW_SHORT) {
            switch (s_screen) {
            case SCREEN_HOME:
                if (si4732_ssb_is_active()) {
                    (void)si4732_ssb_disable();
                }
                radio_next_band();
                settings_mark_dirty();
                break;

            case SCREEN_MENU:
                switch (s_menu_sel) {
                case MENU_VOLUME:
                    s_screen = SCREEN_VOLUME;
                    break;
                case MENU_MUTE:
                    radio_set_mute(!s_muted);
                    break;
                case MENU_SSB:
                    radio_cycle_ssb();
                    break;
                case MENU_BFO:
                    s_screen = SCREEN_BFO;
                    break;
                default:    /* MENU_EXIT */
                    s_screen = SCREEN_HOME;
                    break;
                }
                break;

            case SCREEN_VOLUME:
            case SCREEN_BFO:
                s_screen = SCREEN_MENU;
                break;
            }

            s_status_dirty = true;
            status_refresh();
        }

        if (events & ENCODER_EVENT_SW_LONG) {
            if (s_screen == SCREEN_HOME) {
                s_screen = SCREEN_MENU;
                s_menu_sel = MENU_VOLUME;
            } else {
                s_screen = SCREEN_HOME;
            }
            s_status_dirty = true;
            status_refresh();
        }

        /* ---------- 周期刷新状态 ---------- */
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

/* 画一个带边框的进度条（pct 0~100） */
static void draw_bar(u8g2_t *u, uint8_t x, uint8_t top,
                     uint8_t width, uint8_t height, uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }

    u8g2_DrawFrame(u, x, top, width, height);

    const uint8_t fill = (uint8_t)(((uint16_t)(width - 2) * pct) / 100);
    if (fill > 0) {
        u8g2_DrawBox(u, x + 1, top + 1, fill, height - 2);
    }
}

static void render_not_ready(u8g2_t *u)
{
    const char *stage = "?";
    esp_err_t err = ESP_OK;
    si4732_get_last_error(&stage, &err);

    char line[24];
    u8g2_DrawStr(u, 0, 10, "Si4732 NOT READY");
    snprintf(line, sizeof(line), "SI47:%s RCLK:%s",
             s_i2c_si4732 ? "OK" : "NO",
             rclk_is_running() ? "ON" : "OFF");
    u8g2_DrawStr(u, 0, 24, line);
    snprintf(line, sizeof(line), "ERR %s %s", stage, short_error(err));
    u8g2_DrawStr(u, 0, 38, line);
    u8g2_DrawStr(u, 0, 52, "UART0: diag");
}

static void render_home(u8g2_t *u, const si4732_status_t *st)
{
    char line[24];

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

    /* 信号强度条 + 数值 */
    const uint8_t rssi_pct = (uint8_t)((st->rssi > 60) ? 100 : (st->rssi * 100 / 60));
    snprintf(line, sizeof(line), "%3u", (unsigned)st->rssi);
    u8g2_DrawStr(u, 0, 22, "RSSI");
    draw_bar(u, 28, 16, 70, 7, rssi_pct);
    u8g2_DrawStr(u, 102, 22, line);

    /* 信噪比条 + 数值 */
    const uint8_t snr_pct = (uint8_t)((st->snr > 30) ? 100 : (st->snr * 100 / 30));
    snprintf(line, sizeof(line), "%2u", (unsigned)st->snr);
    u8g2_DrawStr(u, 0, 34, "SNR");
    draw_bar(u, 28, 28, 70, 7, snr_pct);
    u8g2_DrawStr(u, 102, 34, line);

    /* 音量条 + 数值 */
    const uint8_t vol_pct = (uint8_t)(((uint16_t)st->volume * 100) / SI4732_VOLUME_MAX);
    snprintf(line, sizeof(line), "%2u", (unsigned)st->volume);
    u8g2_DrawStr(u, 0, 46, "VOL");
    draw_bar(u, 28, 40, 70, 7, vol_pct);
    u8g2_DrawStr(u, 102, 46, line);

    /* 底部提示 */
    if (s_muted) {
        u8g2_DrawStr(u, 0, 58, "MUTE  SHORT=BAND");
    } else if (!st->tuned) {
        u8g2_DrawStr(u, 0, 58, "TUNING...");
    } else if (!st->valid) {
        u8g2_DrawStr(u, 0, 58, "NO SIGNAL");
    } else {
        u8g2_DrawStr(u, 0, 58, "SHORT=BAND LONG=SET");
    }
}

static void render_menu(u8g2_t *u, const si4732_status_t *st)
{
    static const char *names[MENU_ITEM_COUNT] = {
        "VOL", "MUTE", "SSB", "BFO", "EXIT",
    };

    u8g2_DrawStr(u, 0, 9, "SETTINGS");
    u8g2_DrawHLine(u, 0, 11, OLED_WIDTH);

    for (uint8_t i = 0; i < MENU_ITEM_COUNT; i++) {
        const uint8_t y = (uint8_t)(19 + i * 10);
        char value[8] = "";

        switch (i) {
        case MENU_VOLUME:
            snprintf(value, sizeof(value), "%u", (unsigned)st->volume);
            break;
        case MENU_MUTE:
            snprintf(value, sizeof(value), "%s", s_muted ? "ON" : "OFF");
            break;
        case MENU_SSB:
            snprintf(value, sizeof(value), "%s", ssb_name());
            break;
        case MENU_BFO:
            snprintf(value, sizeof(value), "%+d", (int)st->bfo_hz);
            break;
        default:
            value[0] = '\0';
            break;
        }

        char line[24];
        snprintf(line, sizeof(line), "%c %-4s %s",
                 (i == s_menu_sel) ? '>' : ' ', names[i], value);
        u8g2_DrawStr(u, 0, y, line);

        if (i == s_menu_sel) {
            /* XOR 反白选中行 */
            u8g2_SetDrawColor(u, 2);
            u8g2_DrawBox(u, 0, (uint8_t)(y - 8), OLED_WIDTH, 9);
            u8g2_SetDrawColor(u, 1);
        }
    }
}

static void render_volume(u8g2_t *u, const si4732_status_t *st)
{
    char line[24];

    u8g2_DrawStr(u, 0, 10, "VOLUME");

    const uint8_t pct = (uint8_t)(((uint16_t)st->volume * 100) / SI4732_VOLUME_MAX);
    draw_bar(u, 4, 20, 120, 10, pct);

    snprintf(line, sizeof(line), "%u / %u", (unsigned)st->volume,
             (unsigned)SI4732_VOLUME_MAX);
    u8g2_DrawStr(u, 40, 44, line);
    u8g2_DrawStr(u, 0, 58, "SHORT=BACK LONG=HOME");
}

static void render_bfo(u8g2_t *u, const si4732_status_t *st)
{
    char line[24];

    u8g2_DrawStr(u, 0, 10, "BFO TUNE");
    snprintf(line, sizeof(line), "%+d Hz", (int)st->bfo_hz);
    u8g2_DrawStr(u, 0, 32, line);
    u8g2_DrawStr(u, 0, 58, "SHORT=BACK LONG=HOME");
}

static void render_ui(const si4732_status_t *st)
{
    u8g2_t *u = oled_get_u8g2();
    if (u == NULL) {
        return;
    }

    u8g2_ClearBuffer(u);
    u8g2_SetFont(u, u8g2_font_6x10_tf);

    if (!s_radio_ready) {
        render_not_ready(u);
    } else {
        switch (s_screen) {
        case SCREEN_MENU:   render_menu(u, st); break;
        case SCREEN_VOLUME: render_volume(u, st); break;
        case SCREEN_BFO:    render_bfo(u, st); break;
        default:            render_home(u, st); break;
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

        /* 主页定期刷新 RSSI/SNR；菜单或未就绪时只在状态变化时重绘 */
        const bool periodic = (s_screen == SCREEN_HOME) || !s_radio_ready;
        if (dirty || (periodic && (now - last_render) >= pdMS_TO_TICKS(UI_REDRAW_MS))) {
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
        ESP_LOGI(TAG, "%s %lukHz %s  RSSI %u  SNR %u  VOL %u  BFO %+dHz  %s%s",
                 band_name(st.band), (unsigned long)st.freq_khz,
                 st.ssb_usb ? "USB" : "LSB",
                 (unsigned)st.rssi, (unsigned)st.snr, (unsigned)st.volume,
                 (int)st.bfo_hz,
                 !st.tuned ? "TUNING" : (st.valid ? "LOCK" : "NO SIGNAL"),
                 s_muted ? " MUTE" : "");
    } else {
        ESP_LOGI(TAG, "%s %lu.%03lu MHz  RSSI %u  SNR %u  VOL %u  %s%s",
                 band_name(st.band),
                 (unsigned long)(st.freq_khz / 1000),
                 (unsigned long)(st.freq_khz % 1000),
                 (unsigned)st.rssi, (unsigned)st.snr, (unsigned)st.volume,
                 !st.tuned ? "TUNING" : (st.valid ? "LOCK" : "NO SIGNAL"),
                 s_muted ? " MUTE" : "");
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

    /* 0. NVS：读取上次保存的波段/频率/音量/静音 */
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

    /* 1. 参考时钟必须先于 Si4732 上电 */
    if (rclk_start() != ESP_OK) {
        ESP_LOGE(TAG, "32.768kHz 参考时钟启动失败");
    }

    /* 2. 共用 I2C 总线（Si4732 + OLED） */
    if (board_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败");
    }

    /* 2.1 先复位 Si4732：若上次 SSB 补丁失败让它进入 halt 并拉住 SDA，
     *     复位后再扫描，避免把 0x09~0x77 全部误判成在线设备 */
    (void)si4732_reset();
    vTaskDelay(pdMS_TO_TICKS(10));

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
