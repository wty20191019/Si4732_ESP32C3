#include "rclk.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "RCLK";

#define RCLK_LEDC_SPEED_MODE    LEDC_LOW_SPEED_MODE
#define RCLK_LEDC_TIMER         LEDC_TIMER_1
#define RCLK_LEDC_CHANNEL       LEDC_CHANNEL_1

/* 32.768 kHz 方波：LEDC 的 1 位分辨率只有 0 和 1 两档，
 * duty = 1 即 50% 占空比。80MHz APB / 32768 / 2 = 1220.703125，
 * 分频器的小数部分可以精确表示，误差为 0。 */
#define RCLK_FREQ_HZ            32768
#define RCLK_DUTY_RESOLUTION    LEDC_TIMER_1_BIT
#define RCLK_DUTY               (1u << RCLK_DUTY_RESOLUTION) / 2

static bool s_running = false;

esp_err_t rclk_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = RCLK_LEDC_SPEED_MODE,
        .duty_resolution = RCLK_DUTY_RESOLUTION,
        .timer_num = RCLK_LEDC_TIMER,
        .freq_hz = RCLK_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "配置 LEDC 定时器失败");

    const ledc_channel_config_t channel_cfg = {
        .gpio_num = BOARD_SI4732_RCLK_GPIO,
        .speed_mode = RCLK_LEDC_SPEED_MODE,
        .channel = RCLK_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = RCLK_LEDC_TIMER,
        .duty = RCLK_DUTY,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "配置 LEDC 通道失败");

    s_running = true;
    ESP_LOGI(TAG, "32.768kHz 参考时钟已启动: GPIO%d", (int)BOARD_SI4732_RCLK_GPIO);
    return ESP_OK;
}

void rclk_stop(void)
{
    if (!s_running) {
        return;
    }
    (void)ledc_stop(RCLK_LEDC_SPEED_MODE, RCLK_LEDC_CHANNEL, 0);
    s_running = false;
    ESP_LOGI(TAG, "32.768kHz 参考时钟已停止");
}

bool rclk_is_running(void)
{
    return s_running;
}
