#pragma once

/**
 * @file  step_select.h
 * @brief 通用"调整步进/位数"选择器
 *
 * 用于两个按键的界面里逐级切换调整量：
 *   - 环境量：0.01 → 0.1 → 1 → 10 → 100
 *   - 屏幕亮度：1 → 5 → 10 → 25 → 50
 *
 * 纯数据结构 + 纯函数，不依赖 ESP-IDF / RTOS，可单独复用。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const float *values;    /* 可选的步进值列表（调用方保证生命周期） */
    uint8_t count;          /* 列表长度 */
    uint8_t index;          /* 当前下标 */
} step_select_t;

/**
 * @brief 初始化选择器
 *
 * @param sel     选择器
 * @param values  步进值数组
 * @param count   数组长度
 * @param index   初始下标（越界会被夹到 0）
 */
void step_select_init(step_select_t *sel, const float *values, uint8_t count, uint8_t index);

/**
 * @brief 取当前步进值
 */
float step_select_get(const step_select_t *sel);

/**
 * @brief 切到下一个步进（循环）
 *
 * @return 切换后的步进值
 */
float step_select_next(step_select_t *sel);

/**
 * @brief 切到上一个步进（循环）
 *
 * @return 切换后的步进值
 */
float step_select_prev(step_select_t *sel);

/**
 * @brief 把当前步进格式化成字符串，例如 "0.01"、"0.1"、"100"
 *
 * @param buf  输出缓冲区
 * @param size 缓冲区大小
 */
void step_select_text(const step_select_t *sel, char *buf, size_t size);

#ifdef __cplusplus
}
#endif
