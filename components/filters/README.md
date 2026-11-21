# filters 组件（通用数字滤波）

本组件不绑定任何传感器，输入输出全部是 `float`。

## 模块

| 目录 | 内容 |
|---|---|
| `biquad_lpf` | 通用二阶巴特沃斯低通（设计系数 / 初始化 / 逐点处理 / 预热） |

## 用法

```c
#include "biquad_lpf.h"

static biquad_lpf_coeff_t coeff;
static biquad_lpf_state_t state;

biquad_lpf_design(2.0f, 0.4f, &coeff);   // 2Hz 采样 / 0.4Hz 截止
biquad_lpf_init(&state, 5);              // 预热 5 帧，期间直接透传

float rssi_filtered = biquad_lpf_process(rssi_raw, &state, &coeff);
```

每条信号流要独立一份 `biquad_lpf_state_t`；系数可以共用（同一采样率/截止频率时）。

## 在当前工程中的用途

收音机骨架阶段暂未使用；后续可以给 OLED 上的 RSSI/SNR 做平滑，
避免数值跳动（参考采样率 2Hz、截止 0.4Hz）。
