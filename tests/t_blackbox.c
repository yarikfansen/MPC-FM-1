/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Host check of app/blackbox.c: cold start, restart without a power cut,
 * damaged record, a reset in the middle of an update, the load log. */
#include "blackbox.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned restart(uint32_t now, unsigned safe) { bb_test_reset(); return bb_start(now, safe); }
static void power_cut(void) { memset(bb_test_ram(), 0xa5, sizeof(struct bb_record)); }

int main(void)
{
    /* 1. Cold start: RAM garbage is not a record. */
    power_cut();
    CHECK(!restart(1000, 0), "garbage read as a record");
    CHECK(!bb_last(), "bb_last after a cold start");
    CHECK(!bb_event(0), "event after a cold start");

    /* 2. A session: page, control, load, drops; then a restart reads it back. */
    uint32_t now = 1000;
    for (unsigned i = 0; i < 300; ++i) { /* 3 s of 10 ms polls */
        now += 10;
        if (i == 150) bb_control(5, now);
        bb_poll(now, 20, i < 200 ? 47 : 62, i > 250 ? 3 : 0, i > 100 ? 1 : 0, BB_FLAG_PLAYING);
    }
    CHECK(restart(500, 0), "restart did not find the record");
    const struct bb_record *r = bb_last();
    CHECK(r && r->page == 20 && r->control == 5, "page %u control %u", r ? r->page : 0, r ? r->control : 0);
    CHECK(r && r->uptime_ms >= 2900 && r->uptime_ms <= 3000, "uptime %u", r ? (unsigned)r->uptime_ms : 0);
    CHECK(r && r->control_ms == 1510, "control_ms %u", r ? (unsigned)r->control_ms : 0);
    CHECK(r && r->cpu == 62 && r->cpu_peak == 62, "cpu %u peak %u", r ? r->cpu : 0, r ? r->cpu_peak : 0);
    CHECK(r && r->overruns == 3 && r->sheds == 1, "overruns %u sheds %u", r ? r->overruns : 0, r ? r->sheds : 0);
    CHECK(r && r->restarts == 0 && (r->flags & BB_FLAG_PLAYING) && !(r->flags & BB_FLAG_SAFE), "restarts / flags");

    /* 3. Restarts in a row are counted; SAFE is kept in the flags. */
    now = 500;
    for (unsigned i = 0; i < 20; ++i) { now += 10; bb_poll(now, 28, 5, 0, 0, 0); }
    CHECK(restart(100, 1), "second restart");
    r = bb_last();
    CHECK(r && r->restarts == 1 && r->page == 28, "restarts %u page %u", r ? r->restarts : 0, r ? r->page : 0);
    now = 100;
    for (unsigned i = 0; i < 20; ++i) { now += 10; bb_poll(now, 28, 5, 0, 0, BB_FLAG_SAFE | BB_FLAG_USB); }
    CHECK(restart(100, 0), "third restart");
    r = bb_last();
    CHECK(r && r->restarts == 2 && (r->flags & BB_FLAG_SAFE) && (r->flags & BB_FLAG_USB), "restarts %u flags %u", r ? r->restarts : 0, r ? r->flags : 0);

    /* 4. Any damaged byte = cold start. */
    for (unsigned at = 0; at < sizeof(struct bb_record); ++at) {
        bb_test_reset(); (void)bb_start(0, 0); bb_poll(200, 3, 10, 0, 0, 0);
        ((uint8_t *)bb_test_ram())[at] ^= 0x10;
        CHECK(!restart(0, 0), "damaged byte %u accepted", at);
    }
    /* 5. A reset in the middle of an update (magic already cleared) = cold start, never a mixed record. */
    bb_test_reset(); (void)bb_start(0, 0); bb_poll(200, 3, 10, 0, 0, 0);
    bb_test_ram()->magic = 0;
    bb_test_ram()->page = 9;
    CHECK(!restart(0, 0), "half-written record accepted");

    /* 6. Load log: drops, guard trips, one entry per climb over the line; newest first; at most BB_EVENTS. */
    power_cut(); (void)restart(0, 0);
    now = 0;
    now += 100; bb_poll(now, 20, 50, 0, 0, 0);
    CHECK(!bb_event(0), "event without a cause");
    bb_control(16 + 2, now);
    now += 100; bb_poll(now, 20, 90, 0, 0, 0);
    now += 100; bb_poll(now, 20, 95, 0, 0, 0);
    const struct bb_event *e = bb_event(0);
    CHECK(e && e->kind == BB_EVENT_HIGH && e->cpu == 90 && e->page == 20 && e->control == 18 && !bb_event(1), "high-load entry");
    now += 100; bb_poll(now, 20, 80, 0, 0, 0); /* still inside the 10 % band */
    now += 100; bb_poll(now, 20, 90, 0, 0, 0);
    CHECK(!bb_event(1), "second entry without leaving the band");
    now += 100; bb_poll(now, 20, 60, 4, 0, 0);
    e = bb_event(0);
    CHECK(e && e->kind == BB_EVENT_DROP && e->count == 4 && e->uptime_ms == 600, "drop entry");
    now += 100; bb_poll(now, 17, 60, 4, 2, 0);
    e = bb_event(0);
    CHECK(e && e->kind == BB_EVENT_SHED && e->count == 2 && e->page == 17, "guard entry");
    for (unsigned i = 0; i < 20; ++i) { now += 100; bb_poll(now, 17, 60, 5 + i, 2, 0); }
    CHECK(bb_event(BB_EVENTS - 1) && !bb_event(BB_EVENTS), "log length");
    e = bb_event(0);
    CHECK(e && e->kind == BB_EVENT_DROP && e->count == 1 && e->uptime_ms == now, "newest entry first");
    CHECK(bb_event(1) && bb_event(1)->uptime_ms == now - 100, "order");

    /* 7. Faster polls than BB_UPDATE_MS do not rewrite the record; counters stop at their limits. */
    power_cut(); (void)restart(0, 0);
    bb_poll(100, 1, 300, 70000, 0, 0);
    bb_poll(150, 2, 10, 70000, 0, 0);
    CHECK(restart(0, 0) && bb_last()->page == 1 && bb_last()->cpu == 255 && bb_last()->overruns == 65535, "limits");

    /* 8. USB readout (transfer command 24): exact layout of the three parts. */
    {
        uint8_t o[BB_READ_BYTES + 4];
        memset(o, 0x5a, sizeof o);
        bb_test_reset(); /* before bb_start: header only, no stale log */
        CHECK(bb_read(0, o) == 44 && o[0] == 1 && o[1] == 0 && o[2] == 0 && o[3] == 0 && o[4] == 0 && o[43] == 0 && o[44] == 0x5a,
              "read before start");
        power_cut(); (void)restart(0, 0);
        bb_control(3, 50);
        bb_poll(100, 7, 40, 0, 0, BB_FLAG_PLAYING);   /* uptime 100 */
        bb_poll(200, 7, 91, 2, 0, BB_FLAG_PLAYING);   /* uptime 200: drop x2, then high */
        CHECK(restart(1000, 0), "restart before readout");
        bb_control(16 + 4, 1234);
        bb_poll(1100, 9, 30, 0, 0, BB_FLAG_USB);       /* uptime 100 */
        bb_poll(1300, 12, 88, 70000, 1, BB_FLAG_USB);  /* uptime 300: drop (stops at 65535), guard x1, high */
        memset(o, 0x5a, sizeof o);
        CHECK(bb_read(0, o) == BB_READ_BYTES, "part 0 length");
        static const uint8_t want0[44] = {1, 0, 3, 3,
            /* this session: uptime 300, control_ms 234, restarts 1, overruns 65535, sheds 1, page 12, control 20, cpu 88, peak 88, flags 4, version 1 */
            0x2c, 0x01, 0, 0, 0xea, 0, 0, 0, 1, 0, 0xff, 0xff, 1, 0, 12, 20, 88, 88, 4, 1,
            /* previous: uptime 200, control_ms 50, restarts 0, overruns 2, sheds 0, page 7, control 3, cpu 91, peak 91, flags 2, version 1 */
            0xc8, 0, 0, 0, 0x32, 0, 0, 0, 0, 0, 2, 0, 0, 0, 7, 3, 91, 91, 2, 1};
        for (unsigned i = 0; i < 44; ++i) CHECK(o[i] == want0[i], "part 0 byte %u = %u, want %u", i, o[i], want0[i]);
        CHECK(o[44] == 0x5a, "part 0 wrote past its length");
        /* newest first: the poll logs DROP, then SHED, then HIGH */
        CHECK(bb_read(1, o) == BB_READ_BYTES, "part 1 length");
        static const uint8_t want1[44] = {1, 1, 3, 3,
            0x2c, 0x01, 0, 0, BB_EVENT_HIGH, 12, 20, 88, 1, 0,
            0x2c, 0x01, 0, 0, BB_EVENT_SHED, 12, 20, 88, 1, 0,
            0x2c, 0x01, 0, 0, BB_EVENT_DROP, 12, 20, 88, 0xff, 0xff,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        for (unsigned i = 0; i < 44; ++i) CHECK(o[i] == want1[i], "part 1 byte %u = %u, want %u", i, o[i], want1[i]);
        CHECK(bb_read(2, o) == BB_READ_BYTES && o[0] == 1 && o[1] == 2 && o[3] == 3, "part 2 header");
        for (unsigned i = 4; i < 44; ++i) CHECK(o[i] == 0, "part 2 byte %u not empty", i);
        CHECK(bb_read(3, o) == 0 && bb_read(255, o) == 0, "unknown part accepted");
        /* a full log: part 2 holds entries 4..7, the oldest last */
        for (unsigned i = 0; i < 10; ++i) bb_poll(1400 + 100 * i, 12, 50, 70001 + i, 1, 0);
        CHECK(bb_read(2, o) == BB_READ_BYTES && o[3] == 8, "full log count %u", o[3]);
        const struct bb_event *old = bb_event(7);
        CHECK(old && o[34] == (uint8_t)old->uptime_ms && o[35] == (uint8_t)(old->uptime_ms >> 8) && o[38] == old->kind &&
              o[42] == 1 && o[43] == 0, "part 2 entry 7");
        bb_test_reset(); /* restarted, bb_start not yet called: the old log is not shown */
        CHECK(bb_read(1, o) == BB_READ_BYTES && o[2] == 0 && o[3] == 0 && o[4] == 0 && o[8] == 0, "stale log before start");
        /* cold start: no previous record, its 20 bytes are zero */
        power_cut(); (void)restart(0, 0);
        CHECK(bb_read(0, o) == BB_READ_BYTES && o[2] == 1 && o[3] == 0 && o[23] == 1, "cold start header");
        for (unsigned i = 24; i < 44; ++i) CHECK(o[i] == 0, "cold start previous byte %u", i);
    }

    if (failures) { printf("blackbox: %u check(s) FAILED\n", failures); return 1; }
    printf("blackbox: all checks passed\n");
    return 0;
}
