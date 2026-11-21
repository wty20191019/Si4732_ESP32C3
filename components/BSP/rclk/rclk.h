#pragma once

/**
 * @file  rclk.h
 * @brief Si4732 参考时钟输出（GPIO5，LEDC 硬件产生 32.768 kHz 方波）
 *
 * 必须在 Si4732 上电前启动本时钟：Si4732-A10 没有板载晶振，
 * 依赖 RCLK 引脚上的 32.768 kHz 参考时钟。上电后还需要把
 * REFCLK_FREQ 属性设为 32768，否则调谐频率会整体偏移。
 *
 * 定时器/通道使用：
 *   LEDC_TIMER_1 / LEDC_CHANNEL_1（避开 LED、蜂鸣器等常用资源）
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 32.768 kHz 参考时钟输出（可重复调用）
 *
 * @return ESP_OK 成功
 */
esp_err_t rclk_start(void);

/**
 * @brief 查询时钟是否在运行
 */
bool rclk_is_running(void);

#ifdef __cplusplus
}
#endif
