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

// 点亮电平：1 = 高电平点亮，0 = 低电平点亮（按实际硬件接法修改这里）
#define LED_ACTIVE_LEVEL    1

// 初始化 LED 引脚为推挽输出，默认熄灭
void led_init(void);

// 点亮 / 熄灭 LED
void led_on(void);
void led_off(void);

// 按布尔值控制：true 点亮，false 熄灭
void led_set(bool on);

// 翻转当前状态
void led_toggle(void);
