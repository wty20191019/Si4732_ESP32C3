#pragma once

/**
 * @file  si4732.h
 * @brief Si4732-A10 收音机驱动（I2C 地址 0x22）
 *
 * 硬件连接（见 components/BSP/board/board.h）：
 *   SDA = GPIO1，SCL = GPIO2，RST = GPIO4，GPO1 = GPIO3，RCLK = GPIO5
 *   SENB 接地，锁定 I2C 地址 0x22
 *
 * 使用顺序（app_main 中实现）：
 *   1. rclk_start()            先输出 32.768 kHz 参考时钟
 *   2. board_i2c_init()        初始化共用 I2C 总线
 *   3. si4732_init()           复位 + 上电 + 设置 REFCLK + 默认音量
 *   4. si4732_tune() / seek / set_volume 等日常操作
 *
 * 时序要点：
 *   - 每条命令发出后必须等 CTS（状态字节 bit7）置位，才能发下一条
 *   - 不设置 REFCLK_FREQ = 32768 会导致调谐频率整体偏移
 *   - FM 与 AM 属于不同工作模式，切换时需要重新 POWER_UP
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C 从机地址（SENB 接地） */
#define SI4732_I2C_ADDR             0x22

/* 音量范围 0~63 */
#define SI4732_VOLUME_MIN           0
#define SI4732_VOLUME_MAX           63

/* 参考时钟频率（由 BSP/rclk 提供） */
#define SI4732_REFCLK_HZ            32768

/**
 * @brief 波段
 *
 * FM 与 AM 是两种芯片工作模式；SW/LW 与 MW 同属 AM 模式，
 * 靠调谐频率区分，切换 SW/LW 时不需要重新上电。
 */
typedef enum {
    SI4732_BAND_FM = 0,     /* 64.0 ~ 108.0 MHz */
    SI4732_BAND_AM,         /* 中波 520 ~ 1710 kHz */
    SI4732_BAND_SW,         /* 短波 2.3 ~ 26.1 MHz */
    SI4732_BAND_LW,         /* 长波 153 ~ 279 kHz */
    SI4732_BAND_COUNT
} si4732_band_t;

/**
 * @brief 当前接收状态（tune/seek 之后刷新）
 */
typedef struct {
    si4732_band_t band;
    uint32_t freq_khz;      /* 当前频率，单位 kHz */
    uint8_t volume;         /* 0~63 */
    uint8_t rssi;           /* 信号强度，FM: dBuV，AM: dBuV */
    uint8_t snr;            /* 信噪比，dB */
    bool stereo;            /* FM 立体声导频有效（当前版本暂未解析） */
    bool valid;             /* 最近一次调谐是否有效 */
    bool ssb_active;        /* 是否处于 SSB 模式 */
    bool ssb_usb;           /* true = USB，false = LSB */
    int16_t bfo_hz;         /* 当前 BFO 偏移（Hz） */
} si4732_status_t;

/**
 * @brief SSB 音频带宽（SSB_MODE.AUDIOBW）
 */
typedef enum {
    SI4732_SSB_BW_1K2 = 0,  /* 1.2kHz（默认） */
    SI4732_SSB_BW_2K2,      /* 2.2kHz */
    SI4732_SSB_BW_3K0,      /* 3.0kHz */
    SI4732_SSB_BW_4K0,      /* 4.0kHz */
    SI4732_SSB_BW_0K5,      /* 500Hz */
    SI4732_SSB_BW_1K0,      /* 1.0kHz */
} si4732_ssb_bandwidth_t;

/**
 * @brief 初始化 Si4732（复位、上电到 FM、设置参考时钟与默认音量）
 *
 * 前置条件：rclk_start() 已调用、board_i2c_init() 已成功。
 *
 * @return ESP_OK 成功
 */
esp_err_t si4732_init(void);

/**
 * @brief 硬件复位（RST 拉低 100us 再拉高），等待芯片 CTS 就绪
 */
esp_err_t si4732_reset(void);

/**
 * @brief 切换到指定波段（必要时重新 POWER_UP）
 */
esp_err_t si4732_set_band(si4732_band_t band);

/**
 * @brief 调谐到指定频率（kHz）
 *
 * FM 会自动换算成芯片要求的 10kHz 单位；AM/SW/LW 直接使用 kHz。
 * 内部等待调谐完成，最长约 2 秒。
 */
esp_err_t si4732_tune(uint32_t freq_khz);

/**
 * @brief 自动搜台（向上/向下），内部等待搜台完成
 */
esp_err_t si4732_seek(bool up);

/**
 * @brief 设置音量（0~63）
 */
esp_err_t si4732_set_volume(uint8_t volume);

/**
 * @brief 读取当前音量
 */
uint8_t si4732_get_volume(void);

/**
 * @brief 读取当前频率（kHz）
 */
uint32_t si4732_get_frequency(void);

/**
 * @brief 读取当前波段
 */
si4732_band_t si4732_get_band(void);

/**
 * @brief 读取当前接收状态（RSSI/SNR 等）
 */
esp_err_t si4732_get_status(si4732_status_t *out);

/**
 * @brief 静音开/关
 */
esp_err_t si4732_set_mute(bool mute);

/**
 * @brief 退出上电状态（切换波段前使用）
 */
esp_err_t si4732_power_down(void);

/**
 * @brief 进入 SSB 模式（USB/LSB）
 *
 * 只能在 AM/SW/LW 波段使用。首次调用会向芯片下载 SSB 补丁
 * （约 10KB，耗时数百毫秒），之后切回普通 AM 不需要重复下载。
 * 芯片掉电或硬件复位后补丁丢失，需要重新下载。
 *
 * @param usb true = USB，false = LSB
 * @return ESP_OK 成功
 */
esp_err_t si4732_ssb_enable(bool usb);

/**
 * @brief 退出 SSB 模式，回到普通 AM 解调
 */
esp_err_t si4732_ssb_disable(void);

/**
 * @brief 查询当前是否处于 SSB 模式
 */
bool si4732_ssb_is_active(void);

/**
 * @brief 查询当前边带（仅 SSB 模式下有意义）
 */
bool si4732_ssb_is_usb(void);

/**
 * @brief 设置 BFO 拍频偏移（单位 Hz，范围 -16383 ~ +16383）
 *
 * 用于 SSB 模式下微调音调/补偿频率误差。
 */
esp_err_t si4732_ssb_set_bfo(int16_t hz);

/**
 * @brief 读取当前 BFO 偏移
 */
int16_t si4732_ssb_get_bfo(void);

/**
 * @brief 设置 SSB 音频带宽
 */
esp_err_t si4732_ssb_set_bandwidth(si4732_ssb_bandwidth_t bw);

/**
 * @brief 通用属性写入（AN332 SET_PROPERTY）
 */
esp_err_t si4732_set_property(uint16_t prop, uint16_t value);

/**
 * @brief 通用属性读取（AN332 GET_PROPERTY）
 */
esp_err_t si4732_get_property(uint16_t prop, uint16_t *value);

/**
 * @brief 获取指定波段的频率范围/步进/默认频率
 *
 * @param band        波段
 * @param min_khz     最小频率（可为 NULL）
 * @param max_khz     最大频率（可为 NULL）
 * @param step_khz    编码器步进（可为 NULL）
 * @param default_khz 默认频率（可为 NULL）
 */
void si4732_band_info(si4732_band_t band,
                      uint32_t *min_khz,
                      uint32_t *max_khz,
                      uint32_t *step_khz,
                      uint32_t *default_khz);

#ifdef __cplusplus
}
#endif
