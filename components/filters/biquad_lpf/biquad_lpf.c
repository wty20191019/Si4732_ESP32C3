#include "biquad_lpf.h"

#include <math.h>

#define BIQUAD_LPF_PI  3.14159265358979323846f

void biquad_lpf_design(float sample_hz, float cutoff_hz, biquad_lpf_coeff_t *coeff)
{
    if (coeff == 0) {
        return;
    }

    if (cutoff_hz <= 0.0f || sample_hz <= 0.0f) {
        /* 参数非法时退化为直通 */
        coeff->b0 = 1.0f;
        coeff->b1 = 0.0f;
        coeff->b2 = 0.0f;
        coeff->a1 = 0.0f;
        coeff->a2 = 0.0f;
        return;
    }

    {
        const float fr = sample_hz / cutoff_hz;
        const float ohm = tanf(BIQUAD_LPF_PI / fr);
        const float cos_term = cosf(BIQUAD_LPF_PI / 4.0f);   /* Q = 0.7071，二阶巴特沃斯 */
        const float c = 1.0f + 2.0f * cos_term * ohm + ohm * ohm;
        const float b0 = (ohm * ohm) / c;

        coeff->b0 = b0;
        coeff->b1 = 2.0f * b0;
        coeff->b2 = b0;
        coeff->a1 = 2.0f * (ohm * ohm - 1.0f) / c;
        coeff->a2 = (1.0f - 2.0f * cos_term * ohm + ohm * ohm) / c;
    }
}

void biquad_lpf_init(biquad_lpf_state_t *state, uint16_t warmup_limit)
{
    if (state == 0) {
        return;
    }
    biquad_lpf_reset(state);
    state->warmup_limit = warmup_limit;
}

void biquad_lpf_reset(biquad_lpf_state_t *state)
{
    if (state == 0) {
        return;
    }
    state->x1 = 0.0f;
    state->x2 = 0.0f;
    state->y1 = 0.0f;
    state->y2 = 0.0f;
    state->warmup_count = 0U;
}

float biquad_lpf_process(float input, biquad_lpf_state_t *state, const biquad_lpf_coeff_t *coeff)
{
    float y;

    if (state == 0 || coeff == 0) {
        return input;
    }

    if (state->warmup_count < state->warmup_limit) {
        /* 预热阶段直接透传，避免滤波器从 0 收敛造成的数据跳变 */
        y = input;
        state->warmup_count++;
    } else {
        y = coeff->b0 * input
          + coeff->b1 * state->x1
          + coeff->b2 * state->x2
          - coeff->a1 * state->y1
          - coeff->a2 * state->y2;

        /* 异常值保护：出现 NaN/Inf 时用当前输入重置，避免坏值在滤波器里扩散 */
        if (!isfinite(y)) {
            y = input;
        }
    }

    state->x2 = state->x1;
    state->x1 = input;
    state->y2 = state->y1;
    state->y1 = y;

    return y;
}
