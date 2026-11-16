/* Host test for the autosave rest rule in app/project.c:
 * groove_storage_rest() = REST_MS (20 s) since the last groove_storage_touch()
 * and groove_storage_quiet() (transport stopped, 1.5 s of silence).
 * project.c is linked whole; only the functions the rest rule reaches are
 * faked here (clock, track views, MICRO D, BINAURAL). Everything else stays
 * unresolved (lld /force:unresolved) and is never called.
 * Build/run (Git Bash): CLANG=... sh tests/run_autosave.sh
 * Exit code 0 = all passed, otherwise the number of the first failed case. */
#include "extras.h"
#include "instrument.h"
#include "micro_d.h"
#include "binaural.h"

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

/* ---- fakes ---- */
static uint32_t clock_ms;
static unsigned playing, level; /* track 0 view; other tracks silent */
uint32_t fm1_millis(void) { return clock_ms; }
void groove_get_view(unsigned track, unsigned step, struct groove_view *view)
{
    (void)step;
    view->playing = track == 0 ? playing : 0;
    view->level = track == 0 ? level : 0;
}
int micro_d_track(unsigned track) { (void)track; return 0; }
unsigned micro_d_level(unsigned track) { (void)track; return 0; }
unsigned binaural_running(void) { return 0; }

/* Advance the clock 1 ms at a time to `to`, polling rest every ms (as the
 * control task does every tick). Returns the first time rest was true
 * (or `none` when it never was) and checks it stays true once it is. */
static const uint32_t none = 0xdeadbeefU;
static int broke; /* rest went false again without a touch or sound */
static uint32_t run_to(uint32_t to)
{
    uint32_t first = none;
    for (;;) {
        unsigned r = groove_storage_rest();
        if (r && first == none) first = clock_ms;
        if (!r && first != none) broke = 1;
        if (clock_ms == to) break;
        ++clock_ms;
    }
    return first;
}
static uint32_t rest_now(void) { return groove_storage_rest(); }

int main(void)
{
    /* 1: activity at t = 0, quiet all the time: false at 19 999, true at 20 000. */
    clock_ms = 0; groove_storage_touch();
    if (run_to(19999) != none) return 1;
    ++clock_ms; if (!rest_now() || broke) return 1;

    /* 2: a touch at 15 000 ms after the last one moves rest to 35 000 ms. */
    clock_ms = 100000; groove_storage_touch();
    if (run_to(115000) != none) return 2;
    groove_storage_touch();
    if (run_to(134999) != none) return 2;
    ++clock_ms; if (!rest_now() || broke) return 2;

    /* 3: transport playing for 40 s after the touch: never at rest while it
     * plays; rest 1.5 s after it stops at the earliest (within one 50 ms poll). */
    clock_ms = 200000; groove_storage_touch(); playing = 1;
    if (run_to(240000) != none) return 3;
    playing = 0;
    if (run_to(241499) != none) return 3;
    { uint32_t at = run_to(245000); if (at == none || at > 241550U || broke) return 3; }

    /* 4: a tail sounding (meter above the quiet level), the same rule. */
    clock_ms = 300000; groove_storage_touch(); level = 40;
    if (run_to(330000) != none) return 4;
    level = 0;
    if (run_to(331499) != none) return 4;
    { uint32_t at = run_to(335000); if (at == none || at > 331550U || broke) return 4; }

    /* 5: a key held (touch every 10 ms) for 60 s: never at rest; rest 20 s after release. */
    clock_ms = 400000;
    for (unsigned i = 0; i < 6000; ++i) { groove_storage_touch(); if (run_to(clock_ms + 10U) != none) return 5; }
    {
        uint32_t released = clock_ms; groove_storage_touch();
        if (run_to(released + 19999U) != none) return 5;
        ++clock_ms; if (!rest_now() || broke) return 5;
    }

    /* 6: the millisecond counter wraps past 2^32 between the touch and the rest. */
    clock_ms = 0xffffffffU - 10000U; (void)run_to(clock_ms + 2000U); /* quiet before */
    {
        uint32_t touched = clock_ms; groove_storage_touch();
        if (run_to(touched + 19999U) != none) return 6;
        ++clock_ms; if (!rest_now() || broke) return 6;
    }

    /* 7: wrap again, with a sound that ends after the wrap and after 20 s. */
    clock_ms = 0xffffffffU - 5000U; (void)run_to(clock_ms + 2000U);
    {
        uint32_t touched = clock_ms; groove_storage_touch(); playing = 1;
        if (run_to(touched + 25000U) != none) return 7;
        playing = 0;
        if (run_to(touched + 26499U) != none) return 7;
        uint32_t at = run_to(touched + 30000U);
        if (at == none || at - touched > 26550U || broke) return 7;
    }

    /* 8: a rest longer than 2^32 ms (49.7 days, polled now and then) stays a
     * rest: the last touch is not seen as "10 s ago" after the wrap. */
    clock_ms = 600000; groove_storage_touch(); (void)run_to(621000);
    if (!rest_now()) return 8;
    clock_ms += 0x80000000U; if (!rest_now()) return 8;
    clock_ms += 0x80000000U - 10000U; if (!rest_now()) return 8; /* = 611 000 after the wrap */
    return 0;
}
