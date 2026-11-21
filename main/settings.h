#pragma once

/**
 * @file  settings.h
 * @brief 收音机设置持久化（NVS，断电不丢失）
 *
 * 保存范围（基础版）：
 *   - 当前波段（FM/AM/SW/LW）
 *   - 当前频率（kHz）
 *   - 音量（0~63）
 *   - 静音状态
 *
 * 写入由调用方做 2 秒防抖，避免连续拧编码器时频繁擦写 NVS。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_NVS_NAMESPACE  "radio"

typedef struct {
    uint8_t  band;       /* si4732_band_t：0=FM 1=AM 2=SW 3=LW */
    uint32_t freq_khz;   /* 当前频率，单位 kHz */
    uint8_t  volume;     /* 0~63 */
    bool     mute;       /* true = 静音 */
} radio_settings_t;

/**
 * @brief 填充默认设置（FM 101.1MHz / 音量 30 / 不静音）
 */
void settings_defaults(radio_settings_t *s);

/**
 * @brief 从 NVS 读取设置（不存在或读取失败时保持传入值不变）
 *
 * @return true 至少读到一个保存过的字段
 */
bool settings_load(radio_settings_t *s);

/**
 * @brief 写入设置到 NVS
 *
 * @return true 成功
 */
bool settings_save(const radio_settings_t *s);

#ifdef __cplusplus
}
#endif
