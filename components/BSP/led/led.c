#include "led.h"

void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO_PIN,   // 只配置 LED 所在引脚
        .mode = GPIO_MODE_OUTPUT,               // 推挽输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,      // 不启用内部上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // 不启用内部下拉
        .intr_type = GPIO_INTR_DISABLE,         // 不使用中断
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));     // 应用引脚配置
    led_set(false);                             // 初始化后默认熄灭
}

void led_set(bool on)
{
    // LED_ACTIVE_LEVEL 为 1 时高电平点亮，为 0 时低电平点亮
    int level = on ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL;
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO_PIN, level));
}
