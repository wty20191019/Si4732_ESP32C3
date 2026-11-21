#pragma once

/**
 * @file  led.h
 * @brief 板载单色 LED（GPIO8）
 */

#include <stdbool.h>

#include "board.h"
#include "driver/gpio.h"

/* 板载 LED 控制引脚：见 board.h 中的 BOARD_LED_GPIO */
#define LED_GPIO_PIN        BOARD_LED_GPIO

// 点亮电平：1 = 高电平点亮，0 = 低电平点亮
// ESP32-C3 Super Mini 的板载 LED 接在 3V3 与 GPIO8 之间，低电平点亮；
// 若你的板子是高电平点亮，把这里改成 1
#define LED_ACTIVE_LEVEL    0

// 初始化 LED 引脚为推挽输出，默认熄灭
void led_init(void);

// 按布尔值控制：true 点亮，false 熄灭
void led_set(bool on);
