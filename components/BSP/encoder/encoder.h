#pragma once

/**
 * @file  encoder.h
 * @brief EC11 旋转编码器驱动（A/B 相 + 按键）
 *
 * 接线（见 board.h）：
 *   A  = GPIO6，B = GPIO7，按键 = GPIO9（低电平按下，内部上拉）
 *
 * 驱动内部创建一个 2ms 周期的扫描任务：
 *   - A/B 相使用正交解码状态表，累计 4 次跳变算一格（EC11 常见规格）
 *   - 按键 20ms 软件消抖；短按在松开时产生，长按在按住满阈值时产生
 *
 * 事件通过位掩码一次性取走，任务忙时不会丢事件（位会被合并）。
 */

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 长按判定阈值（毫秒） */
#define ENCODER_LONG_PRESS_MS   1000

/* 双击判定窗口（毫秒） */
#define ENCODER_DOUBLE_CLICK_MS 350

/* 扫描周期（毫秒） */
#define ENCODER_SCAN_PERIOD_MS  2

/* 事件位定义 */
#define ENCODER_EVENT_CW        (1u << 0)   /* 顺时针转一格 */
#define ENCODER_EVENT_CCW       (1u << 1)   /* 逆时针转一格 */
#define ENCODER_EVENT_SW_SHORT  (1u << 2)   /* 按键短按（双击时不会产生） */
#define ENCODER_EVENT_SW_LONG   (1u << 3)   /* 按键长按 */
#define ENCODER_EVENT_SW_DOUBLE (1u << 4)   /* 快速双击 */

/**
 * @brief 初始化编码器引脚并启动扫描任务
 *
 * @return ESP_OK 成功
 */
esp_err_t encoder_init(void);

/**
 * @brief 取出所有待处理事件（非阻塞，取完即清空）
 *
 * @return ENCODER_EVENT_xxx 的位或，无事件返回 0
 */
uint32_t encoder_take_events(void);

/**
 * @brief 读取累计旋转格数（顺时针为正）
 */
int32_t encoder_get_count(void);

/**
 * @brief 清零累计旋转格数
 */
void encoder_reset_count(void);

#ifdef __cplusplus
}
#endif
