#include "encoder.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ENCODER";

/* 正交解码表：索引 = (上一状态 << 2) | 当前状态，
 * 状态 = (A << 1) | B。值为 +1 表示顺时针一步，-1 表示逆时针一步，
 * 0 表示非法跳变（抖动）或状态未变化。 */
static const int8_t s_quad_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

/* EC11 每格（定位感）通常对应 4 次跳变，可按实际手感调整 */
#define ENCODER_TRANSITIONS_PER_DETENT  4

/* 按键消抖：连续 10 次采样一致（2ms x 10 = 20ms）才认为状态稳定 */
#define ENCODER_DEBOUNCE_SAMPLES        10

static portMUX_TYPE s_event_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_events = 0;
static int32_t s_detent_count = 0;

static void encoder_push_event(uint32_t event)
{
    portENTER_CRITICAL(&s_event_lock);
    s_events |= event;
    portEXIT_CRITICAL(&s_event_lock);
}

static uint8_t encoder_read_phase(void)
{
    const int a = gpio_get_level(BOARD_ENC_A_GPIO);
    const int b = gpio_get_level(BOARD_ENC_B_GPIO);
    return (uint8_t)((a << 1) | b);
}

static void encoder_scan_task(void *arg)
{
    (void)arg;

    uint8_t prev_phase = encoder_read_phase();
    int32_t transition_acc = 0;

    bool sw_raw = (gpio_get_level(BOARD_ENC_SW_GPIO) == 0);
    bool sw_stable = sw_raw;
    int sw_debounce = 0;
    TickType_t sw_press_tick = 0;
    bool sw_long_sent = false;

    /* 短按延迟上报：在双击窗口内等第二次按击，区分单击/双击 */
    bool short_pending = false;
    TickType_t short_tick = 0;

    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        /* ---------- 旋转解码 ---------- */
        const uint8_t phase = encoder_read_phase();
        const int8_t step = s_quad_table[(prev_phase << 2) | phase];
        prev_phase = phase;

        if (step != 0) {
            transition_acc += step;
            if (transition_acc >= ENCODER_TRANSITIONS_PER_DETENT) {
                transition_acc -= ENCODER_TRANSITIONS_PER_DETENT;
                s_detent_count++;
                encoder_push_event(ENCODER_EVENT_CW);
            } else if (transition_acc <= -ENCODER_TRANSITIONS_PER_DETENT) {
                transition_acc += ENCODER_TRANSITIONS_PER_DETENT;
                s_detent_count--;
                encoder_push_event(ENCODER_EVENT_CCW);
            }
        }

        /* ---------- 按键消抖与长短按 ---------- */
        const TickType_t now = xTaskGetTickCount();
        const bool pressed = (gpio_get_level(BOARD_ENC_SW_GPIO) == 0);

        if (pressed == sw_stable) {
            sw_debounce = 0;
        } else if (++sw_debounce >= ENCODER_DEBOUNCE_SAMPLES) {
            sw_debounce = 0;
            sw_stable = pressed;

            if (sw_stable) {
                sw_press_tick = now;
                sw_long_sent = false;
            } else if (!sw_long_sent) {
                /* 松开且没有触发过长按：先挂起，等双击窗口结束 */
                if (short_pending &&
                    (now - short_tick) <= pdMS_TO_TICKS(ENCODER_DOUBLE_CLICK_MS)) {
                    short_pending = false;
                    encoder_push_event(ENCODER_EVENT_SW_DOUBLE);
                } else {
                    short_pending = true;
                    short_tick = now;
                }
            }
        }

        if (sw_stable && !sw_long_sent &&
            (now - sw_press_tick) >= pdMS_TO_TICKS(ENCODER_LONG_PRESS_MS)) {
            sw_long_sent = true;
            encoder_push_event(ENCODER_EVENT_SW_LONG);
        }

        /* 双击窗口结束仍没有第二次按击 → 上报单击 */
        if (short_pending &&
            (now - short_tick) > pdMS_TO_TICKS(ENCODER_DOUBLE_CLICK_MS)) {
            short_pending = false;
            encoder_push_event(ENCODER_EVENT_SW_SHORT);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ENCODER_SCAN_PERIOD_MS));
    }
}

esp_err_t encoder_init(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_ENC_A_GPIO) |
                        (1ULL << BOARD_ENC_B_GPIO) |
                        (1ULL << BOARD_ENC_SW_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "编码器引脚配置失败");

    BaseType_t ret = xTaskCreate(encoder_scan_task, "encoder", 3072, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "创建编码器任务失败");

    ESP_LOGI(TAG, "初始化完成: A=GPIO%d B=GPIO%d SW=GPIO%d",
             (int)BOARD_ENC_A_GPIO, (int)BOARD_ENC_B_GPIO, (int)BOARD_ENC_SW_GPIO);
    return ESP_OK;
}

uint32_t encoder_take_events(void)
{
    portENTER_CRITICAL(&s_event_lock);
    const uint32_t events = s_events;
    s_events = 0;
    portEXIT_CRITICAL(&s_event_lock);
    return events;
}

int32_t encoder_get_count(void)
{
    portENTER_CRITICAL(&s_event_lock);
    const int32_t count = s_detent_count;
    portEXIT_CRITICAL(&s_event_lock);
    return count;
}
