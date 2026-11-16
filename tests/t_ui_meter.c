/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Host check of app/ui_meter.c (F8 hook, transfer command 25): the exact reply
 * layout, max / average / count per kind, the restart at each read, limits. */
#include "ui_meter.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int main(void)
{
    uint8_t o[UI_METER_BYTES + 4];
    /* 1. Nothing measured: header only, every kind zero. */
    memset(o, 0x5a, sizeof o);
    CHECK(ui_meter_read(o, 928419, 0xffffffffU) == 51 && UI_METER_BYTES == 51, "length");
    CHECK(o[0] == 1 && u32(o + 1) == 928419 && u32(o + 5) == 0xffffffffU, "header");
    for (unsigned i = 9; i < 51; ++i) CHECK(o[i] == 0, "empty byte %u = %u", i, o[i]);
    CHECK(o[51] == 0x5a, "wrote past the reply");

    /* 2. Known calls: exact bytes. draw 3 calls 100 / 400 / 250 (avg 250), compose 2 calls 70000 / 70001
     * (avg 70000), flush 1 call 0x01020304 ticks, 6 ms. */
    ui_meter_add(UI_DRAW, 100, 0); ui_meter_add(UI_DRAW, 400, 1); ui_meter_add(UI_DRAW, 250, 0);
    ui_meter_add(UI_COMPOSE, 70000, 0); ui_meter_add(UI_COMPOSE, 70001, 2);
    ui_meter_add(UI_FLUSH, 0x01020304U, 6);
    ui_meter_add(UI_KINDS, 999, 9); /* unknown kind: ignored */
    CHECK(ui_meter_read(o, 0x00012345U, 0) == 51, "length 2");
    static const uint8_t want[51] = {1, 0x45, 0x23, 0x01, 0, 0, 0, 0, 0,
        3, 0, 0, 0, 0x90, 0x01, 0, 0, 0xfa, 0, 0, 0, 1, 0,                 /* draw: 3, max 400, avg 250, 1 ms */
        2, 0, 0, 0, 0x71, 0x11, 0x01, 0, 0x70, 0x11, 0x01, 0, 2, 0,        /* compose: 2, max 70001, avg 70000, 2 ms */
        1, 0, 0, 0, 4, 3, 2, 1, 4, 3, 2, 1, 6, 0};                         /* flush: 1, max = avg, 6 ms */
    for (unsigned i = 0; i < 51; ++i) CHECK(o[i] == want[i], "byte %u = %u, want %u", i, o[i], want[i]);

    /* 3. A read starts the figures again. */
    CHECK(ui_meter_read(o, 1, 2) == 51, "length 3");
    for (unsigned i = 9; i < 51; ++i) CHECK(o[i] == 0, "after read byte %u = %u", i, o[i]);

    /* 4. Many long calls: the average does not overflow (64-bit sum), ms stops at 65535, rounding down. */
    for (unsigned i = 0; i < 100000; ++i) ui_meter_add(UI_FLUSH, 0xfffffff0U + (i & 1U), i == 5 ? 70000U : 3U);
    ui_meter_read(o, 0, 0);
    CHECK(u32(o + 37) == 100000 && u32(o + 41) == 0xfffffff1U && u32(o + 45) == 0xfffffff0U, "big average %u %u %u",
          (unsigned)u32(o + 37), (unsigned)u32(o + 41), (unsigned)u32(o + 45));
    CHECK(o[49] == 0xff && o[50] == 0xff, "ms limit");
    CHECK(u32(o + 9) == 0 && u32(o + 23) == 0, "other kinds untouched");

    if (failures) { printf("ui_meter: %u check(s) FAILED\n", failures); return 1; }
    printf("ui_meter: all checks passed\n");
    return 0;
}
