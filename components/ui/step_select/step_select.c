#include "step_select.h"

#include <stdio.h>

void step_select_init(step_select_t *sel, const float *values, uint8_t count, uint8_t index)
{
    if (sel == NULL) {
        return;
    }
    sel->values = values;
    sel->count = count;
    sel->index = (count > 0 && index < count) ? index : 0;
}

float step_select_get(const step_select_t *sel)
{
    if (sel == NULL || sel->values == NULL || sel->count == 0) {
        return 1.0f;
    }
    return sel->values[sel->index];
}

float step_select_next(step_select_t *sel)
{
    if (sel == NULL || sel->count == 0) {
        return 1.0f;
    }
    sel->index = (uint8_t)((sel->index + 1) % sel->count);
    return sel->values[sel->index];
}

float step_select_prev(step_select_t *sel)
{
    if (sel == NULL || sel->count == 0) {
        return 1.0f;
    }
    sel->index = (uint8_t)((sel->index + sel->count - 1) % sel->count);
    return sel->values[sel->index];
}

void step_select_text(const step_select_t *sel, char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return;
    }
    snprintf(buf, size, "%g", (double)step_select_get(sel));
}
