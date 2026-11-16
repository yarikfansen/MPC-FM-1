/* Host test for the deferred CLOCK switch in app/project.c
 * (groove_settings_clock / clock_poll / groove_settings_clock_pending).
 * Device 11.11: groove_clock_set from the control task reboots the synth while
 * the sequencer plays. Rule under test: a CLOCK change sends STOP the normal
 * way (groove_midi_local 0xFC), groove_clock_set runs from groove_project_poll
 * only on a later poll (never the first poll after the request), only when the
 * transport has been seen stopped for CLOCK_SETTLE_MS, and the transport is
 * left stopped. project.c is linked whole; boot_guard_settling() is faked true
 * so groove_project_poll does only the boot guard call and the clock step.
 * Everything else stays unresolved (lld /force:unresolved), never called.
 * Build/run (Git Bash): CLANG=... sh tests/run_clock_switch.sh
 * Exit code 0 = all passed, otherwise the number of the first failed case. */
#include "extras.h"
#include "instrument.h"
#include "micro_d.h"
#include "binaural.h"
#include "boot_guard.h"

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

/* ---- fakes ---- */
static uint32_t clock_ms;
static unsigned playing;        /* published transport state (audio context) */
static unsigned stop_queued;    /* STOP posted, not yet run by the audio context */
static unsigned stop_lose;      /* the next STOP posts are lost (full queue) */
static unsigned stops, sets, set_mode, engine_mode;
static unsigned set_while_playing, set_too_early, set_before_stop_ran;
static unsigned polls_since_request, stop_ran_since_request, request_was_playing;
uint32_t fm1_millis(void) { return clock_ms; }
void groove_get_view(unsigned track, unsigned step, struct groove_view *view)
{
    (void)track; (void)step;
    view->playing = (uint8_t)playing;
    view->level = 0;
}
int groove_midi_local(const uint8_t *m, size_t n)
{
    if (n != 1 || m[0] != 0xfc) return -1;
    ++stops;
    if (stop_lose) { --stop_lose; return 1; }
    stop_queued = 1;
    return 0;
}
void groove_clock_set(unsigned mode)
{
    ++sets; set_mode = mode;
    if (playing || stop_queued) ++set_while_playing;
    if (polls_since_request < 2) ++set_too_early; /* the request itself or the first poll after it */
    if (request_was_playing && !stop_ran_since_request) ++set_before_stop_ran;
    engine_mode = mode;
}
int micro_d_track(unsigned track) { (void)track; return 0; }
unsigned micro_d_level(unsigned track) { (void)track; return 0; }
unsigned binaural_running(void) { return 0; }
void boot_guard_poll(int ready) { (void)ready; }
int boot_guard_settling(void) { return 1; } /* groove_project_poll returns right after the clock step */
int boot_guard_safe(void) { return 0; }
void groove_midi_defaults(struct groove_midi_config *c) { memset(c, 0, sizeof *c); }

/* The audio context runs a queued STOP between control ticks. */
static void audio_block(void)
{
    if (stop_queued) { stop_queued = 0; playing = 0; stop_ran_since_request = 1; }
}
/* One control tick: the project poll comes first, then the screen (where a
 * CLOCK knob calls groove_settings_clock), as in the engine's control loop. */
static unsigned want;
static void tick(unsigned ms, int knob_mode)
{
    clock_ms += ms;
    ++polls_since_request;
    groove_project_poll();
    if (knob_mode >= 0) {
        if (knob_mode <= GROOVE_CLOCK_MIDI && (unsigned)knob_mode != want) { /* an accepted request */
            want = (unsigned)knob_mode;
            polls_since_request = 0; stop_ran_since_request = 0;
            request_was_playing = playing;
        }
        groove_settings_clock((unsigned)knob_mode);
    }
}
static void reset(unsigned play)
{
    groove_project_init();
    engine_mode = want = GROOVE_CLOCK_INTERNAL;
    clock_ms += 100000U;
    playing = play; stop_queued = stop_lose = 0;
    stops = sets = set_while_playing = set_too_early = set_before_stop_ran = 0;
    polls_since_request = 0; stop_ran_since_request = 0; request_was_playing = 0;
}
static unsigned bad(void) { return set_while_playing || set_too_early || set_before_stop_ran; }

int main(void)
{
    /* 1: playing, knob to MIDI: STOP is sent at once, clock_set is NOT called in the
     * request; not on the first poll after it; not before the STOP has run. */
    reset(1);
    tick(1, GROOVE_CLOCK_MIDI);
    if (stops != 1 || sets || !groove_settings_clock_pending()) return 1;
    tick(1, -1);                       /* first poll after the request: never */
    if (sets) return 1;
    for (unsigned i = 0; i < 60; ++i) tick(1, -1); /* the audio context has not run the STOP: wait */
    if (sets || !groove_settings_clock_pending()) return 1;
    audio_block();
    tick(1, -1);                       /* stopped only just now: settle time first */
    if (sets) return 1;
    for (unsigned i = 0; i < 40 && !sets; ++i) tick(1, -1);
    if (sets != 1 || set_mode != GROOVE_CLOCK_MIDI || bad() || groove_settings_clock_pending()) return 1;
    if (playing) return 1;             /* the transport stays stopped */
    for (unsigned i = 0; i < 300; ++i) tick(1, -1);
    if (sets != 1 || stops > 2) return 1; /* once; at most one repeated STOP while waiting 60 ms */

    /* 2: the usual timing: the STOP runs in the next audio block. clock_set comes
     * >= 2 polls and >= 20 ms after the request, exactly once. */
    reset(1);
    tick(1, GROOVE_CLOCK_MIDI);
    uint32_t asked = clock_ms;
    audio_block();
    unsigned n = 0;
    while (!sets && n < 200) { tick(1, -1); ++n; }
    if (sets != 1 || bad() || n < 2 || clock_ms - asked < 20U || stops != 1) return 2;

    /* 3: transport stopped at the request: no STOP is sent, clock_set still waits
     * for a later poll (not the request, not the first poll after it). */
    reset(0);
    tick(1, GROOVE_CLOCK_MIDI);
    if (stops || sets) return 3;
    tick(30, -1);
    if (sets) return 3;                /* first poll after the request, although 30 ms passed */
    tick(1, -1);
    if (sets != 1 || bad() || set_mode != GROOVE_CLOCK_MIDI) return 3;

    /* 4: slow control ticks (40 ms each): not on the first poll, on the second
     * (the transport has been stopped for more than the settle time by then). */
    reset(1);
    tick(40, GROOVE_CLOCK_MIDI);
    audio_block();
    tick(40, -1);
    if (sets) return 4;
    tick(40, -1);
    if (sets != 1 || bad()) return 4;

    /* 5: the STOP was lost (queue full): it is sent again after 100 ms and the
     * change still waits for the stopped transport. */
    reset(1);
    stop_lose = 1;
    tick(1, GROOVE_CLOCK_MIDI);
    for (unsigned i = 0; i < 90; ++i) { tick(1, -1); audio_block(); }
    if (sets || stops != 1) return 5;
    for (unsigned i = 0; i < 20; ++i) { tick(1, -1); audio_block(); }
    if (stops != 2 || sets) return 5;
    for (unsigned i = 0; i < 40; ++i) { tick(1, -1); audio_block(); }
    if (sets != 1 || bad()) return 5;

    /* 6: the transport is started again while the change waits (PLAY, MIDI
     * start): no clock_set while it plays; a new STOP, then the change. */
    reset(1);
    tick(1, GROOVE_CLOCK_MIDI);
    audio_block();
    tick(1, -1); tick(1, -1);
    playing = 1;                       /* started again */
    for (unsigned i = 0; i < 50; ++i) tick(1, -1);
    if (sets) return 6;
    for (unsigned i = 0; i < 200 && !sets; ++i) { tick(1, -1); audio_block(); }
    if (sets != 1 || bad() || playing) return 6;

    /* 7: the knob goes back before the change ran: one clock_set with the old
     * mode (the engine ignores an unchanged mode), nothing while playing. */
    reset(1);
    tick(1, GROOVE_CLOCK_MIDI);
    tick(1, GROOVE_CLOCK_INTERNAL);
    audio_block();
    for (unsigned i = 0; i < 60; ++i) tick(1, -1);
    if (sets != 1 || set_mode != GROOVE_CLOCK_INTERNAL || bad()) return 7;

    /* 8: same mode again / invalid mode: nothing happens, nothing pending. */
    reset(1);
    tick(1, GROOVE_CLOCK_INTERNAL);
    tick(1, 7);
    for (unsigned i = 0; i < 60; ++i) tick(1, -1);
    if (sets || stops || groove_settings_clock_pending() || !playing) return 8;

    /* 9: pending is reported from the request until clock_set ran (the screen
     * keeps its arp silent for exactly that time). */
    reset(1);
    if (groove_settings_clock_pending()) return 9;
    tick(1, GROOVE_CLOCK_MIDI);
    audio_block();
    for (unsigned i = 0; i < 100; ++i) {
        unsigned before = sets;
        if (!before && !groove_settings_clock_pending()) return 9;
        tick(1, -1);
        if (sets && groove_settings_clock_pending()) return 9;
    }
    if (sets != 1) return 9;

    /* 10: the millisecond counter wraps while waiting. */
    reset(1);
    clock_ms = 0xfffffff0U;
    tick(1, GROOVE_CLOCK_MIDI);
    audio_block();
    for (unsigned i = 0; i < 60; ++i) tick(1, -1);
    if (sets != 1 || bad()) return 10;
    return 0;
}
