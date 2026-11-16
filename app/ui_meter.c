/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "ui_meter.h"

/* Per kind since the last read: a sum, a maximum and a count (a few adds per call). */
static struct { uint64_t sum; uint32_t count, max; uint16_t max_ms; } meter[UI_KINDS];

void ui_meter_add(unsigned kind, uint32_t ticks, uint32_t ms)
{
    if (kind >= UI_KINDS) return;
    meter[kind].sum += ticks;
    if (meter[kind].count != 0xffffffffU) ++meter[kind].count;
    if (ticks > meter[kind].max) meter[kind].max = ticks;
    if (ms > meter[kind].max_ms) meter[kind].max_ms = (uint16_t)(ms > 65535U ? 65535U : ms);
}
static uint8_t *put(uint8_t *p, uint32_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) *p++ = (uint8_t)(v >> (8U * i));
    return p;
}
unsigned ui_meter_read(uint8_t out[UI_METER_BYTES], uint32_t block_ticks, uint32_t timer_period)
{
    uint8_t *p = out;
    *p++ = UI_METER_VERSION;
    p = put(p, block_ticks, 4);
    p = put(p, timer_period, 4);
    for (unsigned k = 0; k < UI_KINDS; ++k) {
        uint32_t n = meter[k].count;
        p = put(p, n, 4);
        p = put(p, meter[k].max, 4);
        p = put(p, n ? (uint32_t)(meter[k].sum / n) : 0U, 4);
        p = put(p, meter[k].max_ms, 2);
        meter[k].sum = 0; meter[k].count = meter[k].max = 0; meter[k].max_ms = 0;
    }
    return (unsigned)(p - out);
}
