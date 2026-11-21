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

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_I2C_ADDR_DEFAULT   0x3C
#define OLED_WIDTH              128
#define OLED_HEIGHT             64

/* 默认对比度 0~255，128 ≈ 50% 亮度 */
#define OLED_CONTRAST_DEFAULT   128
/* 亮度下限，避免调到全黑看不见 */
#define OLED_CONTRAST_MIN       15

/**
 * @brief 初始化 OLED 并清屏
 *
 * @param addr 7 位 I2C 地址（常见 0x3C，部分模块 0x3D）
 * @return ESP_OK 成功
 */
esp_err_t oled_init(uint8_t addr);

/**
 * @brief 清屏并立即刷新到屏幕
 */
void oled_clear(void);

/**
 * @brief 在缓冲区指定位置绘制字符串（需要调用 oled_refresh() 才会显示）
 *
 * @param x    起始 x 坐标（像素）
 * @param y    文字基线 y 坐标（像素）
 * @param text 以 '\0' 结尾的 ASCII 字符串
 */
void oled_draw_str(uint8_t x, uint8_t y, const char *text);

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

/**
 * @brief 设置屏幕对比度（亮度）
 *
 * @param value 0~255，0 最暗，255 最亮
 * @return ESP_OK 成功
 */
esp_err_t oled_set_contrast(uint8_t value);

/**
 * @brief 读取当前对比度
 *
 * @return 0~255，未初始化时返回 OLED_CONTRAST_DEFAULT
 */
uint8_t oled_get_contrast(void);

/**
 * @brief 设置整屏反色
 *
 * @param enable true 反色，false 正常
 * @return ESP_OK 成功
 */
esp_err_t oled_set_invert(bool enable);

/**
 * @brief 读取当前是否反色
 */
bool oled_get_invert(void);

/**
 * @brief 反初始化（设备被移除时调用），之后可再次 init
 */
void oled_deinit(void);

#ifdef __cplusplus
}
#endif
