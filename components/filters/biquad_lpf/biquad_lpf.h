#pragma once

/**
 * @file  biquad_lpf.h
 * @brief 通用二阶低通滤波器（Butterworth / biquad）
 *
 * 移植自 STM32 工程中的 IMU_FilterPortable.h，去掉了 IMU 相关命名，
 * 任何 float 信号（IMU 轴、气压、温度、电压……）都可以复用：
 *
 *     static biquad_lpf_coeff_t coeff;
 *     static biquad_lpf_state_t state;
 *     biquad_lpf_design(100.0f, 10.0f, &coeff);   // 100Hz 采样，10Hz 截止
 *     biquad_lpf_init(&state, 20);                // 前 20 次直接输出原值
 *     float y = biquad_lpf_process(raw, &state, &coeff);
 *
 * 滤波器本身不依赖 ESP-IDF，可以脱离 RTOS 使用。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 二阶低通滤波器系数 */
typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
} biquad_lpf_coeff_t;

/* 二阶低通滤波器状态（每条信号流需要独立一份） */
typedef struct {
    float x1;               /* 上一次输入 */
    float x2;               /* 上上次输入 */
    float y1;               /* 上一次输出 */
    float y2;               /* 上上次输出 */
    uint16_t warmup_count;  /* 已处理的预热帧数 */
    uint16_t warmup_limit;  /* 预热帧数，期间直接输出原始值 */
} biquad_lpf_state_t;

/**
 * @brief 根据采样率和截止频率设计二阶巴特沃斯低通系数
 *
 * @param sample_hz 采样率，单位 Hz
 * @param cutoff_hz 截止频率，单位 Hz（应小于 sample_hz/2）
 * @param[out] coeff 计算得到的系数
 *
 * @note sample_hz 或 cutoff_hz 非法时，系数退化为直通（b0=1，其余为 0）
 */
void biquad_lpf_design(float sample_hz, float cutoff_hz, biquad_lpf_coeff_t *coeff);

/**
 * @brief 初始化滤波器状态
 *
 * @param state        滤波器状态
 * @param warmup_limit 预热帧数，期间输出原始值，避免初始收敛过程产生跳变，0 表示不预热
 */
void biquad_lpf_init(biquad_lpf_state_t *state, uint16_t warmup_limit);

/**
 * @brief 复位滤波器状态（保留预热帧数设置）
 */
void biquad_lpf_reset(biquad_lpf_state_t *state);

/**
 * @brief 处理一个采样点
 *
 * @param input 当前采样值
 * @param state 滤波器状态
 * @param coeff 滤波器系数
 * @return 滤波后的值
 */
float biquad_lpf_process(float input, biquad_lpf_state_t *state, const biquad_lpf_coeff_t *coeff);

#ifdef __cplusplus
}
#endif
