/* Host check for app/midi_router.c (W8): a live note on whose channel only
 * reaches MICRO D tracks (instrument ARP off, not recording) must not start
 * the silent PSG host voices, while MICRO D still gets every note exactly as
 * before and the instrument still gets every note off. The router is compiled
 * into this file; the instrument and MICRO D are fakes that log every call.
 * The fake instrument keeps the notes it holds per channel, so a stuck note
 * shows as a key left held after all keys are up. Exit 0 = all checks passed;
 * otherwise the number of the failing check. */
#include "../app/midi_router.c"
#include <stdio.h>

/* ---- fakes: instrument ---- */
static unsigned role[GROOVE_TRACKS], in_ch[GROOVE_TRACKS], arp[GROOVE_TRACKS], md[GROOVE_TRACKS], recording;
static unsigned posts, note_on_posts, note_off_posts, other_posts;
static uint8_t last_post[3];
static unsigned engine_held[16][128]; /* note ons minus note offs the instrument got */
int fm1_midi_post(const uint8_t *m, size_t n)
{
    ++posts;
    memset(last_post, 0, sizeof last_post); memcpy(last_post, m, n);
    unsigned type = m[0] & 0xf0U;
    if (n == 3 && type == 0x90 && m[2]) { ++note_on_posts; ++engine_held[m[0] & 15U][m[1]]; }
    else if (n == 3 && (type == 0x80 || type == 0x90)) { ++note_off_posts; engine_held[m[0] & 15U][m[1]] = 0; }
    else ++other_posts;
    return 0;
}
unsigned groove_track_role(unsigned t) { return role[t]; }
unsigned groove_midi_input_channel(unsigned t) { return in_ch[t]; }
unsigned groove_arp_enabled(unsigned t) { return arp[t]; }
unsigned groove_edit_recording(void) { return recording; }
unsigned groove_clock_mode(void) { return 0; }
unsigned groove_pad_count(unsigned t) { (void)t; return 0; }
void fm1_engine_pause(void) {}
void fm1_engine_resume(void) {}
void fm1_engine_stop(void) {}
unsigned fm1_midi_output_ports(void) { return 0; }
unsigned fm1_midi_input_ports(void) { return 0; }
unsigned fm1_midi_ready_ports(void) { return 0; }
int fm1_midi_send_port(unsigned p, const uint8_t *m, size_t n) { (void)p; (void)m; (void)n; return 0; }
uint32_t fm1_millis(void) { return 0; }

/* ---- fakes: MICRO D ---- */
static unsigned md_live_calls, md_held[GROOVE_TRACKS][128], md_last_velocity, md_stray; /* stray: note off for a note MICRO D does not hold */
int micro_d_track(unsigned t) { return t < GROOVE_TRACKS && md[t]; }
int micro_d_owns(unsigned t) { return t < GROOVE_TRACKS && md[t]; }
void micro_d_note(unsigned t, unsigned note, unsigned velocity) { (void)t; (void)note; (void)velocity; }
void micro_d_live(unsigned t, unsigned note, unsigned velocity)
{
    ++md_live_calls; md_last_velocity = velocity;
    if (!velocity && !md_held[t][note]) ++md_stray; /* in MONO it would retrigger the held note */
    md_held[t][note] = velocity != 0;
}

/* ---- helpers ---- */
static void setup(void)
{
    groove_midi_init();
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        role[t] = GROOVE_TRACK_NORMAL; in_ch[t] = t; arp[t] = 0; md[t] = 0;
    }
    recording = 0;
    posts = note_on_posts = note_off_posts = other_posts = 0;
    md_live_calls = md_stray = 0;
    memset(engine_held, 0, sizeof engine_held); memset(md_held, 0, sizeof md_held);
}
/* source 3 = the synth's keys (groove_midi_local), else USB / DIN / BLE */
static void send(unsigned source, uint8_t a, uint8_t b, uint8_t c)
{
    const uint8_t m[3] = {a, b, c};
    if (source == 3) (void)groove_midi_local(m, 3);
    else fm1_app_midi_received(source, m, 3);
}
static unsigned engine_stuck(void)
{
    for (unsigned c = 0; c < 16; ++c) for (unsigned n = 0; n < 128; ++n) if (engine_held[c][n]) return 1;
    return 0;
}
#define CHECK(id, cond) do { if (!(cond)) { printf("midi_router: check %d failed: %s\n", id, #cond); return id; } } while (0)

/* 9. Fix 11.11 (MICRO D sounded like PSG): USB / DIN / BLE controllers on a
 *    channel whose every target is a MICRO D track do not reach the silent
 *    PSG host; CC 64 / 120 / 123 and notes do. A mixed channel gets all of
 *    them (its other track needs them). The synth's own path (keys, mixer
 *    pan, LFO: groove_midi_local) is not filtered. */
static unsigned sendn(unsigned source, const uint8_t *m, unsigned n)
{
    posts = 0; memset(last_post, 0, sizeof last_post);
    if (source == 3) (void)groove_midi_local(m, n);
    else fm1_app_midi_received(source, m, n);
    return posts == 1 && !memcmp(last_post, m, n);
}
static int controllers(unsigned source)
{
    static const uint8_t drop[][4] = { /* bytes, message */
        {3, 0xb0, 7, 100}, {3, 0xb0, 10, 0}, {3, 0xb0, 11, 127}, {3, 0xb0, 91, 64}, {3, 0xb0, 0, 1}, {3, 0xb0, 32, 1},
        {3, 0xb0, 72, 90}, {3, 0xb0, 74, 90}, {3, 0xb0, 121, 0}, {3, 0xb0, 1, 127}, {2, 0xc0, 1, 0}, {3, 0xe0, 0, 100},
        {2, 0xd0, 90, 0}, {3, 0xa0, 60, 90}};
    static const uint8_t pass[][4] = {{3, 0xb0, 64, 127}, {3, 0xb0, 64, 0}, {3, 0xb0, 120, 0}, {3, 0xb0, 123, 0}};
    unsigned external = source != 3;
    for (unsigned mode = 0; mode < 3; ++mode) { /* plain; instrument ARP on; recording: the host never wants them */
        setup(); md[0] = 1; arp[0] = mode == 1; recording = mode == 2;
        for (unsigned i = 0; i < sizeof drop / sizeof drop[0]; ++i) {
            unsigned got = sendn(source, drop[i] + 1, drop[i][0]);
            CHECK(80, external ? posts == 0 : got);
        }
        for (unsigned i = 0; i < sizeof pass / sizeof pass[0]; ++i) CHECK(81, sendn(source, pass[i] + 1, pass[i][0]));
        CHECK(82, md_live_calls == 0);
    }
    /* two MICRO D tracks share the channel: still MICRO D only */
    setup(); md[0] = md[1] = 1; in_ch[1] = 0;
    { const uint8_t cc7[3] = {0xb0, 7, 100}, pc[2] = {0xc0, 2};
      unsigned a = sendn(source, cc7, 3); CHECK(83, external ? posts == 0 : a);
      unsigned b = sendn(source, pc, 2); CHECK(84, external ? posts == 0 : b); }
    /* mixed channel: T1 MICRO D + T2 another engine on ch 1 -> everything is posted */
    setup(); md[0] = 1; in_ch[1] = 0;
    for (unsigned i = 0; i < sizeof drop / sizeof drop[0]; ++i) CHECK(85, sendn(source, drop[i] + 1, drop[i][0]));
    for (unsigned i = 0; i < sizeof pass / sizeof pass[0]; ++i) CHECK(86, sendn(source, pass[i] + 1, pass[i][0]));
    /* ... until the other track is switched off (role OFF) */
    role[1] = GROOVE_TRACK_OFF;
    { const uint8_t cc7[3] = {0xb0, 7, 100}; unsigned a = sendn(source, cc7, 3); CHECK(87, external ? posts == 0 : a); }
    /* a channel without MICRO D is never filtered; the MICRO D channel next to it is */
    setup(); md[0] = 1;
    { const uint8_t cc7[3] = {0xb1, 7, 100}, pc[2] = {0xc1, 3}, own[3] = {0xb0, 7, 100};
      CHECK(88, sendn(source, cc7, 3)); CHECK(89, sendn(source, pc, 2));
      unsigned a = sendn(source, own, 3); CHECK(90, external ? posts == 0 : a); }
    /* real-time and system messages always pass */
    if (external) {
        setup(); md[0] = 1;
        static const uint8_t rt[] = {0xf8, 0xfa, 0xfb, 0xfc, 0xfe};
        for (unsigned i = 0; i < sizeof rt; ++i) CHECK(91, sendn(source, &rt[i], 1));
    }
    return 0;
}
static int run(unsigned source)
{
    /* 1. T1 alone on ch 1 is MICRO D: the note on reaches the instrument as a
     *    note off of the same key (no voice), MICRO D gets the note. */
    setup(); md[0] = 1;
    send(source, 0x90, 60, 100);
    CHECK(1, note_on_posts == 0 && note_off_posts == 1 && last_post[0] == 0x80 && last_post[1] == 60 && last_post[2] == 0);
    CHECK(2, md_live_calls == 1 && md_held[0][60] && md_last_velocity == 100);
    send(source, 0x80, 60, 0);
    CHECK(3, note_off_posts == 2 && last_post[0] == 0x80 && md_live_calls == 2 && !md_held[0][60] && !engine_stuck());
    /* note on with velocity 0 is a note off: passes unchanged */
    send(source, 0x90, 61, 100); send(source, 0x90, 61, 0);
    CHECK(4, last_post[0] == 0x90 && last_post[1] == 61 && last_post[2] == 0 && !md_held[0][61]);

    /* 2. Mixed channel: T1 MICRO D + T2 DX7 both on ch 1 -> the note on goes
     *    to the instrument as today, MICRO D still gets it. */
    setup(); md[0] = 1; in_ch[1] = 0;
    send(source, 0x90, 64, 90);
    CHECK(10, note_on_posts == 1 && last_post[0] == 0x90 && last_post[2] == 90 && md_held[0][64]);
    send(source, 0x80, 64, 0);
    CHECK(11, !engine_stuck() && !md_held[0][64]);
    /* the other target switched off (role OFF) -> only MICRO D is left */
    role[1] = GROOVE_TRACK_OFF;
    send(source, 0x90, 65, 90);
    CHECK(12, note_on_posts == 1 && md_held[0][65]);
    send(source, 0x80, 65, 0);

    /* 3. Recording: the note on goes to the instrument (it records the take). */
    setup(); md[0] = 1; recording = 1;
    send(source, 0x90, 67, 80);
    CHECK(20, note_on_posts == 1 && last_post[0] == 0x90 && last_post[2] == 80 && md_held[0][67]);
    /* REC switches off while the key is held: the note off still reaches the
     * instrument (no stuck host voice) and MICRO D. */
    recording = 0;
    send(source, 0x80, 67, 0);
    CHECK(21, !engine_stuck() && note_off_posts == 1 && !md_held[0][67]);
    /* held while REC switches on: the instrument never got the note on, the note
     * off still reaches it (harmless) and MICRO D releases. */
    send(source, 0x90, 68, 80);
    CHECK(22, note_on_posts == 1);
    recording = 1;
    send(source, 0x80, 68, 0);
    CHECK(23, note_off_posts == 3 && last_post[0] == 0x80 && last_post[1] == 68 && !engine_stuck() && !md_held[0][68]);

    /* 4. Instrument ARP on the MICRO D track: the instrument needs the note
     *    (its ARP plays MICRO D through groove_midi_note); MICRO D gets no live
     *    note (unchanged). */
    setup(); md[0] = 1; arp[0] = 1;
    send(source, 0x90, 70, 100);
    CHECK(30, note_on_posts == 1 && last_post[0] == 0x90 && md_live_calls == 0);
    /* ARP switches off while held: the note off still reaches the instrument */
    arp[0] = 0;
    send(source, 0x80, 70, 0);
    CHECK(31, !engine_stuck() && md_live_calls == 0); /* MICRO D never got 70: no note off for it */
    /* held while the ARP switches on: note off still reaches the instrument */
    send(source, 0x90, 71, 100);
    CHECK(32, note_on_posts == 1 && md_held[0][71]);
    arp[0] = 1;
    send(source, 0x80, 71, 0);
    CHECK(33, last_post[0] == 0x80 && last_post[1] == 71 && !engine_stuck());
    CHECK(34, !md_held[0][71]); /* MICRO D got 71 before the ARP came on: it gets its note off too */

    /* 5. The track stops / starts being MICRO D while a key is held. */
    setup(); md[0] = 1;
    send(source, 0x90, 72, 100);
    md[0] = 0; /* MICRO D removed (micro_d_detach frees its slot) */
    send(source, 0x80, 72, 0);
    CHECK(40, last_post[0] == 0x80 && last_post[1] == 72 && !engine_stuck() && md_live_calls == 1);
    md[0] = 1; /* MICRO D again (a fresh slot): a late note off of 72 is not passed on */
    send(source, 0x80, 72, 0);
    CHECK(43, md_live_calls == 1 && md_stray == 0);
    md[0] = 0;
    send(source, 0x90, 73, 100); /* DX7 track now: today's path */
    CHECK(41, note_on_posts == 1 && last_post[0] == 0x90);
    md[0] = 1; /* becomes MICRO D while held: its note off still reaches the instrument */
    send(source, 0x80, 73, 0);
    CHECK(42, last_post[0] == 0x80 && last_post[1] == 73 && !engine_stuck() && md_stray == 0);

    /* 6. From the synth itself (keys path) everything that is not a note passes
     *    unchanged on a MICRO D channel. From USB / DIN / BLE only the sustain
     *    pedal does (fix 11.11; all messages: controllers() below). */
    setup(); md[0] = 1;
    unsigned ext = source != 3;
    send(source, 0xb0, 64, 127);  /* sustain pedal */
    CHECK(50, last_post[0] == 0xb0 && last_post[1] == 64 && last_post[2] == 127);
    send(source, 0xb0, 74, 30);   /* CC */
    CHECK(51, ext ? posts == 1 : last_post[0] == 0xb0 && last_post[1] == 74 && last_post[2] == 30);
    send(source, 0xe0, 0, 80);    /* pitch bend */
    CHECK(52, ext ? posts == 1 : last_post[0] == 0xe0 && last_post[2] == 80);
    send(source, 0xa0, 60, 50);   /* poly pressure */
    CHECK(53, ext ? posts == 1 : last_post[0] == 0xa0);
    CHECK(54, other_posts == (ext ? 1U : 4U) && note_on_posts == 0);

    /* 7. A channel with tracks that are not MICRO D: today's path. */
    setup(); md[0] = 1;
    send(source, 0x91, 60, 100); /* ch 2 = T2 DX7 */
    CHECK(60, note_on_posts == 1 && last_post[0] == 0x91 && md_live_calls == 0);
    send(source, 0x81, 60, 0);
    CHECK(61, !engine_stuck());
    /* a MICRO D track on another channel does not matter; two MICRO D tracks
     * on one channel: suppressed, both get the note */
    md[3] = 1; in_ch[3] = 0;
    send(source, 0x90, 62, 100);
    CHECK(62, note_on_posts == 1 && md_held[0][62] && md_held[3][62]);
    send(source, 0x80, 62, 0);
    CHECK(63, !md_held[0][62] && !md_held[3][62] && !engine_stuck());
    /* one of two MICRO D tracks has the ARP on -> the instrument gets the note */
    arp[3] = 1;
    send(source, 0x90, 63, 100);
    CHECK(64, note_on_posts == 2 && md_held[0][63] && !md_held[3][63]);
    send(source, 0x80, 63, 0);
    CHECK(65, !engine_stuck() && !md_held[0][63]);
    CHECK(66, md_stray == 0);
    /* a note off for a key MICRO D never got (pressed before it became MICRO D) */
    setup(); md[0] = 1;
    send(source, 0x80, 50, 0);
    CHECK(67, md_live_calls == 0 && note_off_posts == 1);
    int r = controllers(source);
    return r;
}

int main(void)
{
    int r = run(3);                   /* keys */
    if (!r) r = run(FM1_MIDI_USB);    /* USB */
    if (!r) r = run(FM1_MIDI_DIN);    /* DIN */
    if (r) return r;
    /* 8. Keys path specifics: LOCAL OFF posts nothing (unchanged); a channel
     *    with no target posts nothing from the keys, USB still posts it. */
    setup(); md[0] = 1;
    struct groove_midi_config c; groove_midi_config_read(&c); c.flags |= LOCAL_OFF; groove_midi_config_apply(&c);
    posts = 0;
    send(3, 0x90, 60, 100);
    CHECK(70, posts == 0 && md_live_calls == 0);
    /* LOCAL OFF switched on while a key is held: MICRO D still gets that key's note off */
    setup(); md[0] = 1;
    send(3, 0x90, 64, 100);
    groove_midi_config_read(&c); c.flags |= LOCAL_OFF; groove_midi_config_apply(&c);
    send(3, 0x80, 64, 0);
    CHECK(74, !md_held[0][64] && md_live_calls == 2 && md_stray == 0);
    send(3, 0x90, 65, 100); /* with LOCAL OFF the keys reach nobody, and their note off neither */
    send(3, 0x80, 65, 0);
    CHECK(75, md_live_calls == 2 && md_stray == 0);
    setup();
    send(3, 0x9f, 60, 100);
    CHECK(71, posts == 0);
    send(FM1_MIDI_USB, 0x9f, 60, 100);
    CHECK(72, note_on_posts == 1 && last_post[0] == 0x9f);
    /* system messages pass unchanged */
    { const uint8_t clock = 0xf8; fm1_app_midi_received(FM1_MIDI_USB, &clock, 1); }
    CHECK(73, last_post[0] == 0xf8);
    printf("midi_router: all checks passed\n");
    return 0;
}
