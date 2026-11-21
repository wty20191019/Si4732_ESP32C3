#pragma once

/**
 * @file  oled_display.h
 * @brief 0.96 寸 OLED（SSD1306/SSD1315，128x64）显示驱动，显示库使用 u8g2
 *
 * SSD1315 与 SSD1306 命令兼容，128x64 面板使用 u8g2 的 ssd1306 128x64 驱动。
 * 显示所用的 I2C 控制器复用 board_i2c 创建的总线，不会重复初始化 I2C。
 */

#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_I2C_ADDR_DEFAULT   0x3C
#define OLED_WIDTH              128
#define OLED_HEIGHT             64

/* 默认对比度 0~255，128 约等于 50% 亮度 */
#define OLED_CONTRAST_DEFAULT   128

/**
 * @brief 初始化 OLED 并清屏
 *
 * @param addr 7 位 I2C 地址（常见 0x3C，部分模块 0x3D）
 * @return ESP_OK 成功
 */
esp_err_t oled_init(uint8_t addr);

/**
 * @brief 把缓冲区内容刷新到屏幕
 */
void oled_refresh(void);

/**
 * @brief 获取底层 u8g2 上下文，便于调用画线、画框、图片等完整 API
 *
 * @return u8g2 上下文指针，未初始化时返回 NULL
 */
u8g2_t *oled_get_u8g2(void);

#ifdef __cplusplus
}
#endif
