/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "micro_d.h"
#include "build_config.h"
#include "lang.h"
#include "fm_patch.h"
#include "version.h"
#include "fm1_engine_control.h"
#include "master_fx.h"
#include "font_aa.h"
#include "lfo.h"
#include "arp.h"
#include "sounds.h"
#include "extras.h"
#if defined(__has_include)
#if __has_include("build_id.h")
#include "build_id.h" /* tools/stamp_build.sh */
#endif
#endif
#ifndef FM1_VERSION_TAG
#define FM1_VERSION_TAG "BETA"
#endif
#ifndef FM1_BUILD_ID
#define FM1_BUILD_ID "DEV"
#endif
#include "boot_guard.h"
#include "blackbox.h"
#include "variation.h"
#include "seq_nav.h"
#include "seq_steps.h"
#include "recorder.h"
#include "binaural.h"
#include "audio_output.h"
#include "keyscale.h"
#include "voice_rules.h"
#include "ui_meter.h"
#include <stdlib.h>
#include <string.h>
enum {
    BACK = 0x0000,
    PANEL = 0x18e3,
    TEXT = 0xf77e, /* v12 white */
    ACCENT = 0x07ff,
    DIM = 0x738e,
    GREEN = 0x07e0,
    AMBER = 0xfd20
};
/* Compose the final pixels before touching the LCD. Sixteen rows cost 7,680
 * bytes; two such buffers let one band be composed while the DMA still sends
 * the other, so a poll outputs several bands. Unchanged bands and unchanged
 * horizontal margins are not transferred. A page change never exposes the
 * temporary background clear on the panel. Keep the 16-row bands: some page
 * parts skip bands by their own row limits, and other heights change pixels. */
enum { PAINT_ROWS = 16, PAINT_BANDS = FM1_SCREEN_HEIGHT / PAINT_ROWS };
_Static_assert(FM1_SCREEN_WIDTH * PAINT_ROWS <= FM1_SCREEN_DMA_PIXELS, "LCD band capacity");
_Static_assert(PAINT_BANDS <= 32, "one priority bit per band");
static uint16_t paint_buffer[2][FM1_SCREEN_WIDTH * PAINT_ROWS], *paint_pixels = paint_buffer[0];
static uint8_t paint_left[PAINT_BANDS], paint_right[PAINT_BANDS];
static unsigned paint_rendering, paint_top, paint_next, paint_x, paint_end;
/* A composed band that waits for the DMA (~0U: none) and the buffer it is in. */
static unsigned paint_held = ~0U, paint_half;
/* What the panel shows: per band and 16-pixel column block, a hash of the
 * block's final pixels and whether it is known. Damage that leaves a block's
 * pixels as they are is not sent again (a knob turn redraws whole windows). */
enum { PAINT_BLOCK = 16, PAINT_BLOCKS = FM1_SCREEN_WIDTH / PAINT_BLOCK };
_Static_assert(PAINT_BLOCKS <= 16, "one known bit per block");
static uint32_t paint_shown[PAINT_BANDS][PAINT_BLOCKS];
static uint16_t paint_known[PAINT_BANDS];
static uint32_t paint_priority;
static struct fm1_ble_status frame_ble;
static struct fm1_bluetooth_status frame_radio;
static unsigned frame_radio_flags, frame_radio_role;
static void paint_mark(unsigned x, unsigned y, unsigned w, unsigned h)
{
    for (unsigned b = y / PAINT_ROWS; b <= (y + h - 1) / PAINT_ROWS; ++b) {
        paint_priority |= 1U << b;
        if (x < paint_left[b]) paint_left[b] = (uint8_t)x;
        if (x + w > paint_right[b]) paint_right[b] = (uint8_t)(x + w);
    }
}
/* Many LCD pages share this bounded fill; keep a single native copy. */
static __attribute__((noinline)) void paint_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
    if (!w || !h || x >= FM1_SCREEN_WIDTH || y >= FM1_SCREEN_HEIGHT) return;
    if (w > FM1_SCREEN_WIDTH - x) w = FM1_SCREEN_WIDTH - x;
    if (h > FM1_SCREEN_HEIGHT - y) h = FM1_SCREEN_HEIGHT - y;
    if (!paint_rendering) { paint_mark(x, y, w, h); return; }
    if (x >= paint_end || x + w <= paint_x) return;
    unsigned right = x + w < paint_end ? x + w : paint_end;
    if (x < paint_x) x = paint_x;
    unsigned first = y < paint_top ? paint_top : y;
    unsigned end = y + h < paint_top + PAINT_ROWS ? y + h : paint_top + PAINT_ROWS;
    for (unsigned row = first; row < end; ++row)
        for (unsigned col = x; col < right; ++col)
            paint_pixels[(row - paint_top) * (paint_end - paint_x) + col - paint_x] = color;
}
#include "font_lcd.h"
#include "font_chord.h"
static void lcd_text(int x, unsigned y, const char *s, const struct lcd_font *f, uint16_t color, uint16_t bg);
/* Legacy text API (6 x 7 cells per scale) drawn with the v6 Barlow font: the
 * cells are still cleared so callers that overwrite text in place keep working,
 * and the capitals start at y like the old 5 x 7 glyphs did. */
static void text(unsigned x, unsigned y, const char *s, unsigned scale, uint16_t color,
                 uint16_t background)
{
    if (!s || !scale || scale > 4 || x >= FM1_SCREEN_WIDTH ||
        y >= FM1_SCREEN_HEIGHT || 7U * scale > FM1_SCREEN_HEIGHT - y) return;
    unsigned w = 6U * scale, h = 7U * scale, n = (unsigned)strlen(s), available = (FM1_SCREEN_WIDTH - x) / w;
    if (n > available) n = available;
    if (!n) return;
    if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + h + scale <= paint_top)) return;
    static const struct lcd_font *const fonts[5] = {0, &lcd11, &lcd22, &lcd30, &lcd30};
    const struct lcd_font *f = fonts[scale];
    char buf[48];
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    memcpy(buf, s, n); buf[n] = 0;
    paint_fill(x, y, n * w, h + scale, background);
    lcd_text((int)x, y >= f->digit_top ? y - f->digit_top : 0U, buf, f, color, background);
}
/* Eight tracks stay visible on HOME. SEQ maps the sixteen white keys to one
 * quarter of the 64-step pattern. All drawing and edits run on app_control. */
/* TRACKS is the home screen: a narrow horizontal mixer of all eight tracks.
 * OVERVIEW is one level down: the selected track. HOME always returns to TRACKS. */
enum { OVERVIEW, STEPS, FM_EDIT, FM_ENV, FM_LFO, TONE_EDIT, MIX /* no screen; keeps the page numbers the web shortcuts use */, BLE, PROJECT, NAME, EFFECTS, SOURCE, ACTIONS, STEP_EDIT, RHYTHM, FM_QUICK, PRESET_BROWSER, ARP_EDIT, STARTUP, TRACKS, MASTER_FX, SCOPE, GLOBAL, LFOS, CHAIN, VARS /* no screen since SEQ rework C1 (its knobs are on SEQ page 2); keeps the numbers */, SMP_REC, SMP_CHOP, SAFE, INFO, FAQ, BINAURAL };
enum { OCT_DOWN, OCT_UP, FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC };
#define BUTTON(b) (1U << (b))
/* ONE palette (v13 mockup HX(hue), 90 % saturation x 60 % lightness: equal brightness for every colour; grey = off).
 * Knob colours, engine colours and page-icon colours all come from here - never write a colour literal for them. */
enum {
    PAL_PINK = 0xf991, PAL_GREEN = 0x37ef, PAL_YELLOW = 0xff26, PAL_TURQ = 0x37fe,
    PAL_BLUE = 0x345f, PAL_PURPLE = 0x919f, PAL_AMBER = 0xfd26, PAL_LIME = 0xafe6, PAL_ORANGE = 0xfb86,
    PAL_ROSE = 0xf996, PAL_OFF = 0x5aab
};
static const uint16_t colors[8] = {PAL_PINK, PAL_GREEN, PAL_YELLOW, PAL_TURQ, PAL_PINK, PAL_GREEN, PAL_YELLOW, PAL_TURQ};
/* Each physical knob keeps one colour on every page: pink, green, yellow, turquoise (v6). */
static const uint16_t knob_colors[4] = {PAL_PINK, PAL_GREEN, PAL_YELLOW, PAL_TURQ};
/* Page-icon colours (icon_row): the four knob colours, then blue and purple; icon i wears page_icon_colors[i % 6]. */
static const uint16_t page_icon_colors[6] = {PAL_PINK, PAL_GREEN, PAL_YELLOW, PAL_TURQ, PAL_BLUE, PAL_PURPLE};
static const uint8_t white_keys[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
/* FM1 EDIT key IDs 15,17,19,22,24,27,29,31,34,36 minus 14. */
static const uint8_t edit_keys[10] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22};
static const struct fm_page {
    const char *title, *label[4];
    uint8_t parameter[4], per_operator;
} fm_pages[] = {
    /* Preserve the FM edit page and knob order. */
    {"TUNE/LVL", {"RATIO", "FINE", "DETUNE", "LEVEL"}, {18, 19, 20, 16}, 1}, /* v6 FM LAB order */
    {"VOICE RATES", {"RATE 1", "RATE 2", "RATE 3", "RATE 4"}, {0, 1, 2, 3}, 1},
    {"VOICE LEVELS", {"LEVEL 1", "LEVEL 2", "LEVEL 3", "LEVEL 4"}, {4, 5, 6, 7}, 1},
    {"SENS/OSC", {"AM SENS", "VELOC", "MODE", ""}, {14, 15, 17, 255}, 1},
    {"SCALING", {"L DEPTH", "R DEPTH", "BREAKPT", ""}, {9, 10, 8, 255}, 1},
    {"CURVES", {"L CURVE", "R CURVE", "RATE SC", ""}, {11, 12, 13, 255}, 1},
    {"PITCH RATES", {"RATE 1", "RATE 2", "RATE 3", "RATE 4"}, {126, 127, 128, 129}, 0},
    {"PITCH LEVELS", {"LEVEL 1", "LEVEL 2", "LEVEL 3", "LEVEL 4"}, {130, 131, 132, 133}, 0},
    {"ALGORITHM", {"ALGO", "FDBACK", "OSC SYNC", "TRANS"}, {134, 135, 136, 144}, 0},
    {"LFO", {"WAVE", "SPEED", "PMD", "AMD"}, {142, 137, 139, 140}, 0},
    {"LFO MOD", {"DELAY", "P SENS", "LFO SYNC", "OSC SYNC"}, {138, 143, 141, 136}, 0}};
/* MICRO D tracks show as engine 6 (app/micro_d.c; the instrument sees a silent PSG). */
enum { ENGINE_MICRO = GROOVE_SOURCES, ENGINES };
static const char *const actions[] = {"STEP DETAILS", "START RECORDING", "COPY STEP", "COPY PAGE", "COPY TRACK",
    "PASTE STEP", "PASTE PAGE", "PASTE TRACK", "CLEAR STEP", "CLEAR PAGE", "CLEAR TRACK",
    "REPEAT TO 32", "REPEAT TO 48", "REPEAT TO 64", "UNDO / REDO", "COPY TIMBRE", "PASTE TIMBRE",
    "DX7 PATCH NAME", "BLUETOOTH / CLOCK", "STOP ALL / REWIND", "MIDI ROUTING", "TRACK ROLE / MIDI IN"};
enum { ACTION_COUNT = sizeof actions / sizeof actions[0] };
static unsigned source_preview, action, action_confirm, tone_clip_ready;
static struct groove_preset tone_clip;
static uint32_t midi_seen[8], midi_until[8];
static uint8_t midi_active[8];
static unsigned track, cursor, selected_note = 60, drum, writing, page, fm_group, op;
static unsigned music_page, subpage_return, project_return, rec_before_press, effects_page;
static unsigned seq_page, arp_page;
/* SEQ pages (rework C1): 1 STEP, 2 VARIATION, 7 TRACK; 4..6 = the GLOBAL MIDI submenu; 0 = the grid. Ids, SELECT order,
 * the tap and the knob mapping live in seq_nav.h (host test tests/t_seq_nav.c). */
static unsigned seq_midi(void) { return seq_page >= SEQ_MIDI_CLOCK && seq_page <= SEQ_MIDI_SYNC; }
/* The pages whose 16 white keys are one row of steps (RHYTHM/REC and TRACK ROLE, as before). */
static unsigned seq_key_steps(void) { return seq_page == SEQ_RHYTHM || seq_page == SEQ_ROLE; }
/* v7: recording mode picked with ALGORITHM on the SEQ grid (0 STEP, 1 LIVE);
 * REC only switches recording on/off in that mode. */
/* v7: the mixer changes engine / sound in place and saves the project by
 * itself 3 s after the last turn (one flash write, not one per detent). */
static uint32_t autosave_at;
static uint8_t track_pan[8], pan_dirty; /* mixer R / L: 0 centre, 1 right, 2 left */
static uint8_t pan_value[8] = {64, 64, 64, 64, 64, 64, 64, 64}; /* pan 0..127 sent as CC 10; track_pan is its side */
/* v11 KEYS LATCH (GLOBAL): released keys keep sounding; the next chord,
 * started with all keys up, replaces them. Off at power on. */
static unsigned keys_latch;
static uint32_t latch_held, latch_phys;
static unsigned latch_track;
/* v11: audio peak of this run and the one saved with the project (LAST RUN) */
static unsigned last_peak = ~0U;
static unsigned project_action;
static int sel_armed = -1; /* v7 SEL: the one lit knob (action column), -1 = none */ /* v7 PROJECT page, KNOB 4: 0 SAVE, 1 LOAD */
static unsigned boot_active, boot_phase;
enum { BOOT_HOLD_MS = 5000 }; /* splash: 5 s (v13: waves on the note slots, then the name, then the love line); any key or encoder skips it */
static uint32_t boot_started;
/* web "SEQ PAGE" names by seq_page (SEQ rework C1: grid, pages 1-3, the GLOBAL MIDI submenu; 3 unused) */
static const char *const seq_pages[] = {"GRID", "1/3 STEP", "2/3 VARIATION", "-", "MIDI CLOCK", "MIDI OUT", "MIDI SYNC", "3/3 TRACK"};
static unsigned rec_down, rec_used;
static uint32_t rec_ms;
static int octave;
/* The editor cursor and the visible playback page are independent. Manual
 * paging/editing pins the view; SEQ resumes follow without moving the cursor. */
static unsigned step_follow = 1, transport_seen;
static const char *notice;
static uint32_t notice_until;
static unsigned step_first(const struct groove_view *v)
{
    return ((step_follow && v->playing ? v->play_step : cursor) % v->sequence_length) & ~15U;
}
static uint8_t operator_page[GROOVE_TRACKS][8], operator_selected[GROOVE_TRACKS],
    lfo_page[GROOVE_TRACKS];
static uint8_t tone_pages[GROOVE_TRACKS][GROOVE_PADS + 1], drum_parts[GROOVE_TRACKS];
static uint32_t operator_time[6], operator_held, operator_long;
static uint8_t key_note[27], key_track[27], retry_reset;
/* The notes held on the current track (latched ones too), ascending and unique, at most 8: the ARP chord plate. */
static unsigned arp_held_notes(uint8_t out[8])
{
    unsigned n = 0;
    for (unsigned k = 0; k < 27; ++k) {
        if (key_note[k] == 0xff || key_track[k] != track) continue;
        uint8_t v = key_note[k];
        unsigned i = n, dup = 0;
        for (unsigned j = 0; j < n; ++j) dup |= out[j] == v;
        if (dup) continue;
        if (n == 8) break;
        while (i && out[i - 1] > v) { out[i] = out[i - 1]; --i; }
        out[i] = v; ++n;
    }
    return n;
}
static uint32_t previous_buttons, previous_keys, blocked_keys, frame_ms;
static uint32_t project_keys; /* Held Play pitches across a project remapping. */
static uint32_t encoder_time[7];
static int8_t encoder_direction[7];
static uint32_t encoder_run[7]; /* start of the current fast spin, 0 = none */
static unsigned ble_device, ble_was_linked, ble_view, ble_action;
static int ble_result, source_result;
static unsigned ble_forget, ble_stage, ble_seconds;
static uint32_t ble_stage_since;
static uint32_t health_output;
static uint32_t health_at, health_underruns, health_dropped, health_adapted, health_dma;
static int project_result;
static unsigned project_reload, project_operation;
static unsigned project_slot, project_factory, name_cursor;
static unsigned preset_slot, preset_bank, preset_loaded;
/* Stored sequences (bit per slot), read again after the project page. */
static unsigned seq_slots_used, seq_slots_valid;
static unsigned preset_current_slot, preset_current_bank;
static uint32_t preset_project_revision;
static unsigned advanced_edit, edit_down, edit_long;
static uint32_t edit_ms;
static unsigned seq_down, seq_tap;
static uint32_t seq_ms;
static unsigned home_down, home_long, home_used, from_global, quick_row; /* quick_row: EDIT window, 0 TONE / 1 ENV */ /* from_global: HOME goes back to GLOBAL */
static uint32_t home_ms;
static unsigned fx_row;
static unsigned arp_down, arp_long;
static uint32_t arp_ms;
static unsigned arp_hold_page, arp_hold_sub, arp_hold_track, arp_hold_was_on; /* before the ARP press: a hold undoes its page step */
static char project_stored[13];
static int project_stored_result;
static uint32_t project_drawn_revision;
/* Slot headers stay in RAM: one header costs three sector reads, a scan of all
 * slots 48. A save (or a load) changes the project revision; then only the
 * current slot, where it went, is read again. Flash errors are not kept. */
static char slot_names[GROOVE_PROJECT_SLOTS][13];
static int8_t slot_results[GROOVE_PROJECT_SLOTS];
static uint32_t slot_known, slot_revision;
static void slot_forget(unsigned slot)
{
    if (slot < GROOVE_PROJECT_SLOTS) slot_known &= ~(1U << slot);
    seq_slots_valid = 0;
}
static void slot_sync(void)
{
    uint32_t revision = groove_project_revision();
    if (revision != slot_revision) { slot_revision = revision; slot_forget(groove_project_slot()); }
}
static int slot_info(unsigned slot, char name[13])
{
    slot_sync();
    if (slot >= GROOVE_PROJECT_SLOTS) return groove_project_info(slot, name);
    if (!(slot_known & (1U << slot))) {
        int r = groove_project_info(slot, slot_names[slot]);
        if (r < 0 && r != GROOVE_PROJECT_INVALID) { /* busy or I/O: read it again next time */
            memcpy(name, slot_names[slot], 13);
            return r;
        }
        slot_results[slot] = (int8_t)r;
        slot_known |= 1U << slot;
    }
    memcpy(name, slot_names[slot], 13);
    return slot_results[slot];
}
/* Stored sequences for the SEQ and SAVE squares. */
static void slot_scan(void)
{
    char name[13];
    slot_sync();
    if (seq_slots_valid) return;
    seq_slots_used = 0; seq_slots_valid = 1;
    for (unsigned i = 0; i < GROOVE_PROJECT_SLOTS; ++i)
        if (slot_info(i, name) == 0) seq_slots_used |= 1U << i;
}
static void project_refresh(void)
{
    if (project_factory) {
        const char *name = groove_factory_name(project_slot);
        memset(project_stored, ' ', 12);
        if (name) memcpy(project_stored, name, strlen(name));
        project_stored[12] = 0;
        project_stored_result = name ? 0 : GROOVE_PROJECT_INVALID;
    } else project_stored_result = slot_info(project_slot, project_stored);
    project_drawn_revision = groove_project_revision();
}
static void preset_refresh(void)
{
    project_slot = preset_slot;
    project_factory = preset_bank;
    project_refresh();
}
static char edit_character(char old, int delta)
{
    static const char alphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
    unsigned at = 0;
    if (old >= 'a' && old <= 'z')
        old = (char)(old - 'a' + 'A');
    for (unsigned i = 0; i < sizeof alphabet - 1; ++i)
        if (alphabet[i] == old)
            at = i;
    int n = (int)sizeof alphabet - 1;
    return alphabet[((int)at + delta % n + n) % n];
}
/* v11 SAVE: KNOB 1 digits, 2 letters A-M, 3 letters N-Z. Turning a knob whose
 * group does not hold the letter starts at the group's first (or last) one. */
static const char *const name_groups[3] = {" 0123456789-", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ"};
static int name_group(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (int g = 0; g < 3; ++g) if (c && strchr(name_groups[g], c)) return g;
    return -1;
}
static char group_character(char old, unsigned g, int delta)
{
    const char *set = name_groups[g];
    int n = (int)strlen(set), at;
    if (old >= 'a' && old <= 'z') old = (char)(old - 'a' + 'A');
    if (name_group(old) != (int)g) at = delta > 0 ? 0 : n - 1;
    else at = (((int)(strchr(set, old) - set) + delta) % n + n) % n;
    return set[at];
}
struct draw_cache {
    unsigned shown_page;
    uint32_t header_cache, title_cache, middle_cache, hint_cache, hold_cache, seq_cache;
    uint32_t foot_cache[4], row_cache[10], middle_shape, cell_cache[8][16], tile_cache[16];
    uint32_t mix_cache[8][5]; /* MIXER per cell: square, name, volume bar, letter, frame + cursor (user 13.11) */
    unsigned meter_cache[8];
    uint32_t top_cache[3]; /* v6 top bar: left, badges, right/centre */
};
static struct draw_cache saved_cache, band_cache, *cache = &saved_cache;
static uint32_t led_cache;
static uint16_t button_led_cache;
static struct groove_view frame_views[GROOVE_TRACKS];
static uint8_t frame_step_notes[16], frame_effects[4];
static unsigned lfo_sel; /* GLOBAL LFO page: 0 = LFO 1, 1 = LFO 2 */
static unsigned bin_page; /* page BINAURAL: 0 = BINAURAL (beats), 1 = 3D SOUND (headphone crossfeed) */
static unsigned green_old = 0xff, play_green = 32; /* play_green: matrix index of the green PLAY LED (default 32, found on the device), 0xff = none */
void fm1_led_raw(int idx, int on); /* app/board/panel.c */
void fm1_led_dim_set(unsigned key, int on); /* app/board/panel.c: soft glow layer */
unsigned groove_screen_play_led_get(void) { return play_green > 43U ? 45U : play_green + 1U; } /* 45 = none */
void groove_screen_play_led_put(unsigned stored) { if (play_green <= 43U) green_old = play_green; play_green = stored >= 1U && stored <= 44U ? stored - 1U : stored == 45U ? 0xffU : 32U; } /* empty / old 0 = default 32 */
int fm1_led_probe(int idx), fm1_led_probe_key(int idx); /* app/board/panel.c */
static unsigned glob_row, battery_adc, battery_shown = ~0U;
/* HOME tap walks back through the pages actually visited; hold goes home. */
enum { BACK_DEPTH = 8 };
static uint8_t back_stack[BACK_DEPTH], back_n, back_going;
static uint8_t key_arp[27], key_seq[27], chain_slot[16], chain_pos, chain_on, chain_cursor, chain_prev_step;
/* v7 CHAIN repeats: field i plays chain_rep[i] + 1 times (x1..x16). */
static uint8_t chain_rep[16], chain_loops;
static uint8_t chain_ahead; /* chain slot (1..16) read ahead into RAM, 0 = none */
static uint32_t chain_mark; /* when the step before the loop's last one started */
/* v7 sampler REC / CHOP (agreed 21d/21e). smp_len in quarter seconds. */
static unsigned smp_pad, smp_src, smp_len = 6, smp_mon = 1, smp_slices, smp_mode, smp_sel, smp_saving, smp_active;
static int smp_db = -36;
static unsigned smp_encoded, smp_done, smp_total;
static struct rec_slice smp_items[8];
static unsigned smp_starts[8];
static unsigned key_counter, arp_track_last = 8;
static struct groove_step frame_cells[GROOVE_STEPS];
static uint8_t frame_patch[156];
static struct groove_fm_controls frame_controls;
static struct fm1_ble_device frame_devices[FM1_BLE_MAX_DEVICES];
static uint32_t frame_time;
/* DX7 voice names for the track rows and the header. groove_fm_copy pauses the
 * engine when its one edit snapshot changes track, and every LCD band redraws
 * its rows: the name is read at most once per frame and track (damage pass),
 * the bands reuse it. The selected track's name comes from the frame's own
 * patch copy when draw_frame made one. */
static char fm_names[GROOVE_TRACKS][8];
static uint32_t fm_name_frame[GROOVE_TRACKS], frame_seq;
static uint8_t fm_name_known, frame_fm_ok;
static const char *fm_track_name(unsigned t)
{
    if (t >= GROOVE_TRACKS) return "";
    if ((fm_name_known & (1U << t)) && (paint_rendering || fm_name_frame[t] == frame_seq)) return fm_names[t];
    uint8_t patch[156];
    const uint8_t *p = t == track && frame_fm_ok ? frame_patch : !groove_fm_copy(t, patch, NULL) ? patch : NULL;
    memset(fm_names[t], 0, sizeof fm_names[t]);
    if (p) memcpy(fm_names[t], p + 145, 7);
    fm_name_known |= (uint8_t)(1U << t);
    fm_name_frame[t] = frame_seq;
    return fm_names[t];
}

/* Share frequently used control helpers instead of repeating their bodies
 * in every editor. These run at panel rate, outside the audio/scan paths. */
static __attribute__((noinline)) int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}
static void move_step(const struct groove_view *view, int delta, int pages)
{
    if (step_follow && view->playing) cursor = view->play_step;
    if (pages) cursor = (unsigned)clamp((int)(cursor / 16) + pages, 0,
        (int)view->sequence_length / 16 - 1) * 16 + (cursor & 15U);
    cursor = (unsigned)clamp((int)cursor + delta, 0, (int)view->sequence_length - 1);
    step_follow = 0;
}
static unsigned navigation_mode(void)
{
    return page == BLE || page == PROJECT || page == PRESET_BROWSER || page == NAME ||
        (page >= SOURCE && page <= RHYTHM) || (page == STEPS && !writing && seq_key_steps());
}
static unsigned encoder_active(const int16_t values[7])
{
    for (unsigned i = 0; i < 7; ++i)
        if (values[i])
            return 1;
    return 0;
}
/* One encoder policy for every continuous parameter: a slow turn moves one
 * unit per detent, however the detents fall into polls (a late poll sums
 * them, it never multiplies them). Only a spin faster than ENC_FAST_MS a
 * detent, without a pause over ENC_GAP_MS, speeds up: after ENC_X2_MS twice
 * the steps for ranges of 64+ values, after ENC_X4_MS four times for 128+.
 * Reversal, a pause or a new control context returns to one step. */
enum { ENC_FAST_MS = 35, ENC_GAP_MS = 60, ENC_X2_MS = 150, ENC_X4_MS = 400 };
static void read_encoders(const struct fm1_controls *controls, uint32_t now, unsigned boundary)
{
    if (boundary) memset(encoder_direction, 0, sizeof encoder_direction);
    for (unsigned i = 0; i < 7; ++i) {
        int value = controls->encoders[i], direction = (value > 0) - (value < 0);
        if (!direction) continue;
        uint32_t gap = now - encoder_time[i], detents = (uint32_t)(value < 0 ? -value : value);
        unsigned fast = direction == encoder_direction[i] && gap <= ENC_GAP_MS && gap <= detents * ENC_FAST_MS;
        if (!fast) encoder_run[i] = 0;
        else if (!encoder_run[i]) encoder_run[i] = encoder_time[i] | 1U;
        encoder_direction[i] = (int8_t)direction;
        encoder_time[i] = now;
    }
}
#ifdef FM1_HOST_TEST
static unsigned web_fine;
void groove_screen_web_fine(unsigned fine) { web_fine = fine; }
#endif
/* Steps for a turn of a parameter with range values; selectors and ranges
 * under 64 always take the detents as they are. */
static int fm_encoder_delta(unsigned encoder, int16_t value, unsigned range)
{
#ifdef FM1_HOST_TEST
    if (web_fine) return value;
#endif
    if (range < 64 || !encoder_run[encoder]) return value;
    uint32_t spin = encoder_time[encoder] - encoder_run[encoder];
    return value * (range >= 128 && spin >= ENC_X4_MS ? 4 : spin >= ENC_X2_MS ? 2 : 1);
}
/* Values of a DX7 voice parameter (VCED order): the rates, levels, scaling
 * depths, output level and fine tune have 100, the rest are small. */
static unsigned fm_range(unsigned parameter)
{
    static const uint8_t op[21] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
                                   4, 4, 8, 4, 8, 100, 2, 32, 100, 15};
    static const uint8_t global[19] = {100, 100, 100, 100, 100, 100, 100, 100, 32, 8,
                                       2, 100, 100, 100, 100, 2, 6, 8, 49};
    return parameter < 126 ? op[parameter % 21] : parameter < 145 ? global[parameter - 126] : 0;
}
/* Values of a shared effect setting: the synced delay has nine divisions,
 * the free delay time and the others are long ranges; 4 is the sync switch. */
static unsigned effects_range(unsigned parameter)
{
    uint8_t values[4];
    if (parameter >= 4) return 0;
    if (parameter != 1) return 128;
    groove_effects_get(values);
    return groove_delay_synced(values[1]) ? GROOVE_DELAY_LAST - GROOVE_DELAY_SYNC + 1U : GROOVE_DELAY_FREE_LAST - 8U;
}
static unsigned parameter_page(unsigned value)
{
    return value == FM_ENV || value == FM_LFO || value == EFFECTS;
}
static void open_subpage(unsigned target)
{
    if (page == target) page = subpage_return;
    else {
        if (!parameter_page(page)) subpage_return = page;
        page = target;
    }
}
static int post(uint8_t command, uint8_t a, uint8_t b, unsigned bytes)
{
    const uint8_t message[3] = {command, a, b};
    if (!groove_midi_local(message, bytes))
        return 1;
    retry_reset = 1;
    return 0;
}
static int var_post(uint8_t status, uint8_t a, uint8_t b) { return post(status, a, b, 3); }
/* User 11.11: REC on, sequencer stopped, our ARP feeding this track: the arp only sounds. The engine's step
 * recording (one note per step from the cursor) is paused while `writing` stays on, and goes on when the
 * sequencer plays; the take then puts every arp note into the nearest step (the engine's live rule).
 * Resuming is a new REC start for the engine: its one-level undo then goes back to that moment. */
static unsigned rec_paused;
static void edit_record(unsigned on, unsigned playing)
{
    rec_paused = arp_rec_pause(on, playing, arp_enabled(track) && !groove_pad_count(track));
    groove_edit_record(on && !rec_paused, track, cursor);
}
/* Reuse the formatter across panel fields instead of duplicating it at every call. */
static __attribute__((noinline)) void decimal(char out[12], int value, unsigned digits)
{
    unsigned n = value < 0 ? (unsigned)-value : (unsigned)value, length;
    /* All displayed values are within -24..300; minimum width is optional. */
    length = 1;
    for (unsigned v = n; v >= 10; v /= 10)
        ++length;
    if (length < digits)
        length = digits;
    unsigned start = value < 0;
    if (start)
        out[0] = '-';
    out[start + length] = 0;
    while (length) {
        out[start + --length] = (char)('0' + n % 10);
        n /= 10;
    }
}
/* Share numeric painting across pages instead of expanding it at every call. */
static __attribute__((noinline)) void number(unsigned x, unsigned y, int value, unsigned digits, unsigned scale,
                   uint16_t foreground, uint16_t background)
{
    char out[12];
    decimal(out, value, digits);
    text(x, y, out, scale, foreground, background);
}
static void note_name(unsigned note, char out[12])
{
    static const char names[] = "CCDDEFFGGAAB";
    static const uint16_t sharps = (1U << 1) | (1U << 3) | (1U << 6) | (1U << 8) | (1U << 10);
    unsigned pitch = note % 12, n = 1;
    out[0] = names[pitch];
    if (sharps & (1U << pitch))
        out[n++] = '#';
    char oct[12];
    decimal(oct, (int)(note / 12) - 1, 1);
    strcpy(out + n, oct);
}
static uint32_t signature(uint32_t seed, const void *bytes, unsigned n)
{
    const uint8_t *p = bytes;
    for (unsigned i = 0; i < n; ++i)
        seed = seed * 33U + p[i];
    return seed;
}
static void frame(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
    paint_fill(x, y, w, 1, color);
    paint_fill(x, y + h - 1, w, 1, color);
    paint_fill(x, y, 1, h, color);
    paint_fill(x + w - 1, y, 1, h, color);
}
static void line(int x, int y, int end_x, int end_y, uint16_t color)
{
    if (paint_rendering && ((y < end_y ? y : end_y) >= (int)(paint_top + PAINT_ROWS) ||
        (y > end_y ? y : end_y) < (int)paint_top)) return;
    /* Algorithm wires are axis-aligned: one LCD window covers the whole wire. */
    if (x == end_x) {
        paint_fill((unsigned)x, (unsigned)(y < end_y ? y : end_y), 1,
                        (unsigned)(y < end_y ? end_y - y + 1 : y - end_y + 1), color);
        return;
    }
    if (y == end_y) {
        paint_fill((unsigned)(x < end_x ? x : end_x), (unsigned)y,
                        (unsigned)(x < end_x ? end_x - x + 1 : x - end_x + 1), 1, color);
        return;
    }
    int dx = end_x > x ? end_x - x : x - end_x, dy = end_y > y ? y - end_y : end_y - y;
    int sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1, error = dx + dy;
    for (;;) {
        paint_fill((unsigned)x, (unsigned)y, 1, 1, color);
        if (x == end_x && y == end_y)
            break;
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x += sx;
        }
        if (twice <= dx) {
            error += dx;
            y += sy;
        }
    }
}
#ifdef FM1_HOST_TEST
static char web_knobs[4][2][16];
#endif
static void abbreviate(const char *in, char *out, unsigned max);
static void aa_text(unsigned x, unsigned y, const char *s, const struct aa_font *f, uint16_t color, uint16_t bg);
static unsigned aa_width(const struct aa_font *f, const char *s);
/* Bottom hint line: smooth small font; ^ = up/down arrows, <> = left/right
 * arrows (white, symmetric), \x7f = green check mark. */
static void hint_text(unsigned x, unsigned y, const char *s, unsigned scale, uint16_t color, uint16_t bg)
{
    (void)scale; (void)y;
    const unsigned top = 227;
    if (paint_rendering && (paint_top >= 240 || paint_top + PAINT_ROWS <= top)) return;
    paint_fill(x, top, 240 - x - 2, 13, bg);
    char word[48]; unsigned n = 0;
    for (const char *c = s;; ++c) {
        unsigned icon = *c == '^' || (*c == '<' && c[1] == '>') || (unsigned char)*c == 127U || !*c;
        if (!icon && n < sizeof word - 1) { word[n++] = *c; continue; }
        if (n) { word[n] = 0; aa_text(x, top, word, &font_small, color, bg); x += aa_width(&font_small, word); n = 0; }
        if (!*c) break;
        unsigned cy = top + 6;
        if (*c == '^') {
            x += 2;
            for (unsigned r = 0; r < 4; ++r) { paint_fill(x + 3 - r, cy - 5 + r, 2 * r + 1, 1, TEXT); paint_fill(x + 3 - r, cy + 5 - r, 2 * r + 1, 1, TEXT); }
            x += 9;
        } else if (*c == '<') {
            x += 2;
            for (unsigned col = 0; col < 4; ++col) { paint_fill(x + col, cy - col, 1, 2 * col + 1, TEXT); paint_fill(x + 10 - col, cy - col, 1, 2 * col + 1, TEXT); }
            x += 13; ++c;
        } else {
            x += 1;
            static const uint8_t check[7][2] = {{0, 3}, {1, 4}, {2, 5}, {3, 4}, {4, 3}, {5, 2}, {6, 1}};
            for (unsigned i = 0; i < 7; ++i) paint_fill(x + check[i][0], cy - 3 + check[i][1], 1, 2, GREEN);
            x += 9;
        }
    }
}
static unsigned knob_tint;
static uint16_t blend565(uint16_t bg, uint16_t fg, unsigned a) /* a: 0..3 */
{
    if (!a) return bg;
    if (a == 3) return fg;
    unsigned r = ((bg >> 11) * (3 - a) + (fg >> 11) * a) / 3, g = (((bg >> 5) & 63U) * (3 - a) + ((fg >> 5) & 63U) * a) / 3,
             b = ((bg & 31U) * (3 - a) + (fg & 31U) * a) / 3;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static unsigned aa_width(const struct aa_font *f, const char *s)
{
    unsigned w = 0;
    for (; *s; ++s) if ((unsigned char)*s >= 32) w += f->width[aa_glyph(*s)];
    return w;
}
/* Anti-aliased text (2-bit glyphs) through the banded painter. */
static void aa_text(unsigned x, unsigned y, const char *s, const struct aa_font *f, uint16_t color, uint16_t bg)
{
    unsigned w = aa_width(f, s), h = f->height;
    if (!w || x >= FM1_SCREEN_WIDTH) return;
    if (!paint_rendering) { paint_mark(x, y, w, h); return; }
    if (y >= paint_top + PAINT_ROWS || y + h <= paint_top) return;
    for (; *s; ++s) {
        if ((unsigned char)*s < 32) continue;
        unsigned g = aa_glyph(*s), gw = f->width[g], bytes = (gw + 3U) / 4U;
        const uint8_t *bits = f->bits + f->offset[g];
        for (unsigned row = 0; row < h; ++row) {
            unsigned py = y + row;
            if (py < paint_top || py >= paint_top + PAINT_ROWS) continue;
            for (unsigned col = 0; col < gw; ++col) {
                unsigned px = x + col;
                if (px < paint_x || px >= paint_end) continue;
                unsigned a = (bits[row * bytes + col / 4U] >> (2U * (col & 3U))) & 3U;
                paint_pixels[(py - paint_top) * (paint_end - paint_x) + px - paint_x] = blend565(bg, color, a);
            }
        }
        x += gw;
    }
}
/* v6 palette (docs/mockups/FM1-v6-screens.html). */
enum { V_DIM = 0x9d11, V_FAINT = 0x5b4b, V_RED = 0xfa49, V_ORANGE = 0xfcc7, V_PINK = 0xc20f,
       V_NAME = 0xbd98, V_DARK = 0x2926, V_NONE = 0x0001 };
static unsigned keep_case; /* >0 while drawing chord names: real lower case (app/font_chord.h), never folded */
static unsigned lcd_glyph(const struct lcd_font *f, char c)
{
    unsigned u = (unsigned char)c;
    if (u >= 'a' && u <= 'z' && !keep_case) u -= 32;
    if (u == '`' && f->count >= 65) return 64; /* middle dot */
    if (u >= 0x80) return u < 0xa0 && f->count >= 97 ? 65 + (u - 0x80) : ~0U; /* Russian capitals */
    u -= f->first;
    return u < f->count ? u : ~0U;
}
static unsigned keep_names; /* >0 while drawing sound / project names: never translated */
static unsigned lcd_width(const struct lcd_font *f, const char *s)
{
    unsigned w = 0;
    if (!keep_names) s = lang_tr(s);
    for (; *s; ++s) { unsigned g = lcd_glyph(f, *s); if (g != ~0U) w += f->adv[g]; }
    return w;
}
/* y is the top of the em box, like canvas textBaseline 'top'. */
static void lcd_text(int x, unsigned y, const char *s, const struct lcd_font *f, uint16_t color, uint16_t bg)
{
    if (!keep_names) s = lang_tr(s);
    unsigned w = lcd_width(f, s), top = y + f->top, h = f->height;
    if (!w || x >= (int)FM1_SCREEN_WIDTH) return;
    if (!paint_rendering) { paint_mark(x > 0 ? (unsigned)x - (x > 1) : 0U, top, w + 2, h); return; }
    if (top >= paint_top + PAINT_ROWS || top + h <= paint_top) return;
    unsigned stride = paint_end - paint_x;
    /* Only this band's rows; the two in-between shades are mixed once. */
    unsigned r0 = paint_top > top ? paint_top - top : 0U, r1 = top + h > paint_top + PAINT_ROWS ? paint_top + PAINT_ROWS - top : h;
    const uint16_t shade_of[4] = {bg, blend565(bg, color, 1), blend565(bg, color, 2), color};
    for (; *s; ++s) {
        unsigned g = lcd_glyph(f, *s);
        if (g == ~0U) continue;
        unsigned gw = f->w[g];
        const uint8_t *bits = f->bits + f->off[g];
        int gx = x + f->x[g];
        x += f->adv[g];
        if (gx >= (int)paint_end || gx + (int)gw <= (int)paint_x) continue;
        for (unsigned row = r0; row < r1; ++row) {
            uint16_t *line = paint_pixels + (top + row - paint_top) * stride;
            for (unsigned col = 0; col < gw; ++col) {
                int px = gx + (int)col;
                unsigned i = row * gw + col, a = (bits[i >> 2] >> (2U * (i & 3U))) & 3U;
                if (!a || px < (int)paint_x || px >= (int)paint_end) continue;
                line[(unsigned)px - paint_x] = shade_of[a];
            }
        }
    }
}
static void lcd_center(int cx, unsigned y, const char *s, const struct lcd_font *f, uint16_t color, uint16_t bg)
{
    lcd_text(cx - (int)(lcd_width(f, s) / 2U), y, s, f, color, bg);
}
/* Fit into width: full text if it fits, else shorten, but never below 4 characters. */
static void fit(const char *in, char out[16], const struct lcd_font *f, unsigned width)
{
    if (!keep_names) in = lang_tr(in);
    strncpy(out, in, 15); out[15] = 0;
    for (unsigned n = (unsigned)strlen(out); lcd_width(f, out) > width && n > 4; --n) {
        char tmp[16]; abbreviate(in, tmp, n - 1); strcpy(out, tmp);
    }
}
/* Canvas roundRect on the LCD: radius 1 = 1.5 px (soft corner pixel),
 * 2 = 2..3 px (corner cut, neighbours soft). V_NONE skips fill or edge. */
static unsigned isqrt_u(unsigned v) { unsigned r = 0; while ((r + 1) * (r + 1) <= v) ++r; return r; }
/* Inset of row k of an h-tall box whose corners are circles of radius R. */
static unsigned rbox_inset(unsigned R, unsigned h, unsigned k)
{
    unsigned d = k < R ? R - 1 - k : k >= h - R ? k - (h - R) : ~0U;
    if (d == ~0U) return 0;
    unsigned dy2 = (2 * d + 1) * (2 * d + 1), rr4 = 4 * R * R;
    return R - (isqrt_u(rr4 > dy2 ? rr4 - dy2 : 0) + 1) / 2;
}
static void rbox(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r, uint16_t fill, uint16_t edge)
{
    /* Organic look: pebble corners on the Fibonacci scale (2 -> 5, 3..4 -> 8). */
    unsigned R = r >= 3 ? 8U : r == 2 ? 5U : 0U;
    if (R * 2 > w) R = w / 2;
    if (R * 2 > h) R = h / 2;
    if (R >= 3) {
        for (unsigned k = 0; k < h; ++k) {
            unsigned in = rbox_inset(R, h, k);
            if (fill != V_NONE && w > 2 * in) paint_fill(x + in, y + k, w - 2 * in, 1, fill);
            if (edge == V_NONE) continue;
            if (k == 0 || k == h - 1) { paint_fill(x + in, y + k, w - 2 * in, 1, edge); continue; }
            /* the outline runs from this row's inset to the outer neighbour's */
            unsigned n = k < R ? rbox_inset(R, h, k - 1) : k >= h - R ? rbox_inset(R, h, k + 1) : in;
            unsigned span = n > in ? n - in : 1;
            paint_fill(x + in, y + k, span, 1, edge);
            paint_fill(x + w - in - span, y + k, span, 1, edge);
        }
        return;
    }
    if (fill != V_NONE) paint_fill(x, y, w, h, fill);
    if (edge != V_NONE) frame(x, y, w, h, edge);
    uint16_t c = edge != V_NONE ? edge : fill;
    for (unsigned k = 0; k < 4; ++k) {
        unsigned cx = k & 1U ? x + w - 1 : x, cy = k & 2U ? y + h - 1 : y;
        int dx = k & 1U ? -1 : 1, dy = k & 2U ? -1 : 1;
        if (r < 2) { paint_fill(cx, cy, 1, 1, blend565(BACK, c, 1)); continue; }
        paint_fill(cx, cy, 1, 1, BACK);
        paint_fill((unsigned)((int)cx + dx), cy, 1, 1, blend565(BACK, c, 2));
        paint_fill(cx, (unsigned)((int)cy + dy), 1, 1, blend565(BACK, c, 2));
        if (fill == V_NONE) paint_fill((unsigned)((int)cx + dx), (unsigned)((int)cy + dy), 1, 1, c);
    }
}
/* v13 glass (docs/mockups/FM1-v13-screens.html: glassCur, glass, gH, gV). Every
 * helper blends over what is already in the band buffer, so it works on any
 * theme and background; circle corners of a real radius, clipped to the band
 * like paint_fill. Alpha is 0..256 inside (256 = opaque), 0..255 (mockup hex
 * byte) in the public arguments; a fraction is 0..256 (1/256 units). */
enum { GLASS_WELL = 0x0862, GLASS_WELL_EDGE = 0x5aad, GLASS_TRACK_EDGE = 0x4a2b, GLASS_WHITE = 0xffff };
static uint16_t glass_mix(uint16_t bg, uint16_t fg, unsigned a)
{
    unsigned r = ((bg >> 11) * (256U - a) + (fg >> 11) * a) >> 8;
    unsigned g = (((bg >> 5) & 63U) * (256U - a) + ((fg >> 5) & 63U) * a) >> 8;
    unsigned b = ((bg & 31U) * (256U - a) + (fg & 31U) * a) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static void glass_mark(int x, int y, unsigned w, unsigned h)
{
    if (x < 0) { w += (unsigned)x; x = 0; }
    if (y < 0) { h += (unsigned)y; y = 0; }
    if ((unsigned)x >= FM1_SCREEN_WIDTH || (unsigned)y >= FM1_SCREEN_HEIGHT || (int)w <= 0 || (int)h <= 0) return;
    if (w > FM1_SCREEN_WIDTH - (unsigned)x) w = FM1_SCREEN_WIDTH - (unsigned)x;
    if (h > FM1_SCREEN_HEIGHT - (unsigned)y) h = FM1_SCREEN_HEIGHT - (unsigned)y;
    paint_mark((unsigned)x, (unsigned)y, w, h);
}
/* Row y, columns [x0, x1): colour c over the band buffer. */
static void glass_span(int y, int x0, int x1, uint16_t c, unsigned a)
{
    if (!a || y < (int)paint_top || y >= (int)(paint_top + PAINT_ROWS)) return;
    if (x0 < (int)paint_x) x0 = (int)paint_x;
    if (x1 > (int)paint_end) x1 = (int)paint_end;
    if (x0 >= x1) return;
    uint16_t *p = paint_pixels + (unsigned)(y - (int)paint_top) * (paint_end - paint_x) + (unsigned)x0 - paint_x;
    for (int n = x1 - x0; n > 0; --n, ++p) *p = a >= 256U ? c : glass_mix(*p, c, a);
}
/* Rounded box of radius r, one colour and alpha. */
static void glass_box(int x, int y, unsigned w, unsigned h, unsigned r, uint16_t c, unsigned a)
{
    unsigned R = r;
    if (R * 2 > w) R = w / 2;
    if (R * 2 > h) R = h / 2;
    int lo = y < (int)paint_top ? (int)paint_top : y;
    int hi = y + (int)h > (int)(paint_top + PAINT_ROWS) ? (int)(paint_top + PAINT_ROWS) : y + (int)h;
    for (int py = lo; py < hi; ++py) {
        int in = (int)rbox_inset(R, h, (unsigned)(py - y));
        glass_span(py, x + in, x + (int)w - in, c, a);
    }
}
/* Dark well with a 1 px outline (the empty part of every glass bar). */
static void glass_well(int x, int y, unsigned w, unsigned h, unsigned r, uint16_t edge)
{
    glass_box(x, y, w, h, r, edge, 256);
    if (w > 2 && h > 2) glass_box(x + 1, y + 1, w - 2, h - 2, r ? r - 1 : 0, GLASS_WELL, 256);
}
/* Coverage 0..10 of the pixel [p, p + 10) (1/10 px) by [lo, hi). */
static unsigned glass_cover(int p, int lo, int hi)
{
    int a = lo > p ? lo : p, b = hi < p + 10 ? hi : p + 10;
    return b > a ? (unsigned)(b - a) : 0U;
}
/* glassCur: the cursor box. Fill fades a_top -> a_bot (mockup bytes: 0x5d -> 0x1c, SEQ red 0x8c -> 0x70) over
 * what is below, 2 px outline in the colour (1 px outside the box, 1 px inside, as the canvas stroke), white
 * highlight strip at (x + 3, y + 1.5, w - 6, min(h * 0.16, 4.5)). */
static void glass_cursor_a(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r, uint16_t colour,
                           unsigned a_top, unsigned a_bot)
{
    if (w < 6 || h < 6) return;
    if (!paint_rendering) { glass_mark((int)x - 1, (int)y - 1, w + 2, h + 2); return; }
    if (y + h + 1 <= paint_top || y >= paint_top + PAINT_ROWS + 1) return;
    unsigned Ro = r + 1, Ri = r ? r - 1 : 0;
    if (Ro * 2 > w + 2) Ro = (w + 2) / 2;
    if (Ro * 2 > h + 2) Ro = (h + 2) / 2;
    if (Ri * 2 > w - 2) Ri = (w - 2) / 2;
    if (Ri * 2 > h - 2) Ri = (h - 2) / 2;
    int hh2 = (int)(h * 32U / 100U); /* highlight height in half pixels: min(h * 0.16, 4.5) * 2 */
    if (hh2 > 9) hh2 = 9;
    unsigned Rh = r > 3 ? r - 2 : 1;
    int hrows = (3 + hh2 + 1) / 2 - 1; /* rows y + 1 .. y + hrows */
    int lo = (int)y - 1 < (int)paint_top ? (int)paint_top : (int)y - 1;
    int hi = (int)(y + h) + 1 > (int)(paint_top + PAINT_ROWS) ? (int)(paint_top + PAINT_ROWS) : (int)(y + h) + 1;
    for (int py = lo; py < hi; ++py) {
        int ol = (int)x - 1 + (int)rbox_inset(Ro, h + 2, (unsigned)(py - (int)y + 1));
        int orr = (int)(x + w) + 1 - (int)rbox_inset(Ro, h + 2, (unsigned)(py - (int)y + 1));
        unsigned k = (unsigned)(py - (int)y);
        if (py <= (int)y || py >= (int)(y + h) - 1) { /* top and bottom edge rows: all outline */
            glass_span(py, ol, orr, colour, 256);
            continue;
        }
        int il = (int)x + 1 + (int)rbox_inset(Ri, h - 2, k - 1);
        int ir = (int)(x + w) - 1 - (int)rbox_inset(Ri, h - 2, k - 1);
        if (il < ol) il = ol;
        if (ir > orr) ir = orr;
        glass_span(py, ol, il, colour, 256);
        glass_span(py, ir, orr, colour, 256);
        unsigned a = (unsigned)((int)a_top + ((int)a_bot - (int)a_top) * (int)(2U * k + 1U) / (int)(2U * h)); /* row centre */
        glass_span(py, il, ir, colour, a + (a >> 7));
        /* highlight rows: coverage of [3, 3 + hh2) half pixels over the row's [2k, 2k + 2) */
        if ((int)k >= 1 && (int)k <= hrows) {
            int c0 = 2 * (int)k > 3 ? 2 * (int)k : 3, c1 = 2 * (int)k + 2 < 3 + hh2 ? 2 * (int)k + 2 : 3 + hh2;
            int cover = c1 > c0 ? c1 - c0 : 0;
            int ins = Rh >= 2 && ((int)k == 1 || (int)k == hrows) ? 1 : 0;
            int gl = (int)x + 3 + ins, gr = (int)(x + w) - 3 - ins; /* never over the outline or past the rounded corner */
            if (gl < il + 1) gl = il + 1;
            if (gr > ir - 1) gr = ir - 1;
            glass_span(py, gl, gr, GLASS_WHITE, (unsigned)(27 * cover));
        }
    }
}
static void glass_cursor(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r, uint16_t colour)
{
    glass_cursor_a(x, y, w, h, r, colour, 0x5d, 0x1c);
}
/* glass: the 28 px wide FX column. Dark well (radius 5) with outline, fill from the bottom (at least 4 px, as
 * drawn) with a horizontal gradient colour 100 % -> 80 % -> 40 %, 4 px highlight, faint overlay, top shine. */
static void glass_column(unsigned x, unsigned top, unsigned h, unsigned fraction, uint16_t colour)
{
    if (h < 12) return;
    if (!paint_rendering) { glass_mark((int)x, (int)top, 28, h); return; }
    if (top >= paint_top + PAINT_ROWS || top + h <= paint_top || x >= paint_end || x + 28 <= paint_x) return;
    if (fraction > 256U) fraction = 256U;
    unsigned fh = ((h - 4U) * fraction + 128U) >> 8;
    if (fh < 4U) fh = 4U;
    int fy = (int)top + (int)h - 2 - (int)fh;
    glass_well((int)x, (int)top, 28, h, 5, GLASS_WELL_EDGE);
    /* fill x + 2 .. x + 26: alpha by pixel centre u = (2 + i + 0.5) / 28, stops 255 @ 0, 204 @ 0.45, 102 @ 1 */
    uint16_t alpha[24];
    for (unsigned i = 0; i < 24; ++i) {
        int p = (int)(2U * (2U + i) + 1U) * 100; /* position in 1/56 x 100; stop 0.45 = 2520 */
        int a = p <= 2520 ? 255 - 51 * p / 2520 : 204 - 102 * (p - 2520) / 3080;
        alpha[i] = (uint16_t)(a + (a >> 7));
    }
    unsigned R = fh < 6U ? fh / 2U : 3U;
    int lo = fy < (int)paint_top ? (int)paint_top : fy;
    int hi = fy + (int)fh > (int)(paint_top + PAINT_ROWS) ? (int)(paint_top + PAINT_ROWS) : fy + (int)fh;
    for (int py = lo; py < hi; ++py) {
        int in = (int)rbox_inset(R, fh, (unsigned)(py - fy));
        for (int px = (int)x + 2 + in; px < (int)x + 26 - in; ++px)
            if (px >= (int)paint_x && px < (int)paint_end) {
                uint16_t *p = paint_pixels + (unsigned)(py - (int)paint_top) * (paint_end - paint_x) + (unsigned)px - paint_x;
                *p = glass_mix(*p, colour, alpha[px - (int)x - 2]);
            }
    }
    if (fh > 8U) glass_box((int)x + 5, fy + 3, 4, fh - 6U, 2, GLASS_WHITE, 102); /* inside the fill (x + 2 .. 26) */
    glass_box((int)x + 2, (int)top + 2, 24, h - 4, 3, GLASS_WHITE, 15);
    /* top shine x + 5 .. 23, y + 3.5 .. 5.5 (alpha .3): half, full, half - well inside the overlay's corners */
    glass_span((int)top + 3, (int)x + 5, (int)x + 23, GLASS_WHITE, 38);
    glass_span((int)top + 4, (int)x + 5, (int)x + 23, GLASS_WHITE, 77);
    glass_span((int)top + 5, (int)x + 5, (int)x + 23, GLASS_WHITE, 38);
}
/* gH: horizontal pill bar. Fill colour 100 % (top) -> 53 % (bottom) from the left, at least h wide. */
static void glass_bar_h(unsigned x, unsigned y, unsigned w, unsigned h, unsigned fraction, uint16_t colour)
{
    if (w < 4 || h < 3) return;
    if (!paint_rendering) { glass_mark((int)x, (int)y, w, h); return; }
    if (y >= paint_top + PAINT_ROWS || y + h <= paint_top || x >= paint_end || x + w <= paint_x) return;
    unsigned R = h / 2U;
    glass_well((int)x, (int)y, w, h, R, GLASS_TRACK_EDGE);
    if (!fraction) return;
    if (fraction > 256U) fraction = 256U;
    unsigned fw = (w * fraction + 128U) >> 8;
    if (fw < h) fw = h;
    int lo = (int)y < (int)paint_top ? (int)paint_top : (int)y;
    int hi = (int)(y + h) > (int)(paint_top + PAINT_ROWS) ? (int)(paint_top + PAINT_ROWS) : (int)(y + h);
    for (int py = lo; py < hi; ++py) {
        unsigned k = (unsigned)(py - (int)y);
        int in = (int)rbox_inset(R, h, k);
        unsigned a = (unsigned)(255 - 119 * (int)(2U * k + 1U) / (int)(2U * h));
        glass_span(py, (int)x + in, (int)(x + fw) - in, colour, a + (a >> 7));
        /* gH highlight: rows [.18 h, .48 h) (coverage in 1/10 px), columns x + h / 2 .. x + fw - h / 2 (inside the pill's ends), only if fw > 1.6 h */
        unsigned cover = glass_cover((int)k * 10, (int)h * 18 / 10, (int)h * 48 / 10);
        if (cover && fw * 10U > h * 16U) glass_span(py, (int)x + (int)(h + 1U) / 2, (int)(x + fw) - (int)(h + 1U) / 2, GLASS_WHITE, 115U * cover / 10U);
    }
}
/* gV: vertical pill bar. Fill colour 100 % (left) -> 53 % (right) from the bottom, at least w high. */
static void glass_bar_v(unsigned x, unsigned y, unsigned w, unsigned h, unsigned fraction, uint16_t colour)
{
    if (w < 3 || h < 4) return;
    if (!paint_rendering) { glass_mark((int)x, (int)y, w, h); return; }
    if (y >= paint_top + PAINT_ROWS || y + h <= paint_top || x >= paint_end || x + w <= paint_x) return;
    unsigned R = w / 2U;
    glass_well((int)x, (int)y, w, h, R, GLASS_TRACK_EDGE);
    if (!fraction) return;
    if (fraction > 256U) fraction = 256U;
    unsigned fh = (h * fraction + 128U) >> 8;
    if (fh < w) fh = w;
    int fy = (int)(y + h) - (int)fh;
    int lo = fy < (int)paint_top ? (int)paint_top : fy;
    int hi = fy + (int)fh > (int)(paint_top + PAINT_ROWS) ? (int)(paint_top + PAINT_ROWS) : fy + (int)fh;
    /* gV highlight: columns [.18 w, .48 w) px (1/10), rows fy + w / 2 .. fy + fh - w / 2 (inside the pill's ends), only if fh > 1.6 w */
    int hl_a = (int)(w + 1U) / 2, hl_b = (int)fh - (int)(w + 1U) / 2;
    if (fh * 10U <= w * 16U) hl_b = hl_a;
    for (int py = lo; py < hi; ++py) {
        int in = (int)rbox_inset(R, fh, (unsigned)(py - fy));
        for (int i = in; i < (int)w - in; ++i) {
            unsigned a = (unsigned)(255 - 119 * (2 * i + 1) / (int)(2U * w));
            glass_span(py, (int)x + i, (int)x + i + 1, colour, a + (a >> 7));
            unsigned cover = py - fy >= hl_a && py - fy < hl_b ? glass_cover(i * 10, (int)w * 18 / 10, (int)w * 48 / 10) : 0U;
            if (cover) glass_span(py, (int)x + i, (int)x + i + 1, GLASS_WHITE, 115U * cover / 10U);
        }
    }
}
/* Shorten for the footer: known words, else initials + last word, else drop
 * inner vowels, then cut to max. */
static void abbreviate(const char *in, char *out, unsigned max)
{
    static const char *const known[][2] = {
        {"VELOCITY", "VEL"}, {"LENGTH", "LEN"}, {"FEEDBK", "FDBK"}, {"RETURN", "RET"}, {"CUTOFF", "CUT"},
        {"SPREAD", "SPRD"}, {"STAGES", "STGS"}, {"CHANCE", "CHNC"}, {"FX SEND", "SEND"}, {"COUNT IN", "CNT"},
        {"BRIGHT", "BRT"}, {"FILTER", "FLT"}, {"LEVEL", "LVL"}, {"REV SEND", "REV"}, {"DLY SEND", "DLY"},
        {"CHORUS", "CHRS"}, {"FLANGE", "FLNG"}, {"SQUARE", "SQR"}, {"1 BAR", "1BAR"}, {"STEPS", "STPS"}};
    char buf[16]; unsigned n = 0;
    for (const char *c = in; *c && n < 15; ++c) buf[n++] = *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c;
    buf[n] = 0;
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; ++i)
        if (!strcmp(buf, known[i][0]) && strlen(known[i][1]) <= max) { strcpy(out, known[i][1]); return; }
    if (n <= max) { strcpy(out, buf); return; }
    const char *last = strrchr(buf, ' ');
    unsigned o = 0;
    if (last) {
        for (unsigned i = 0; buf + i < last && o + 1 < max; ++i)
            if (buf[i] != ' ' && (i == 0 || buf[i - 1] == ' ')) out[o++] = buf[i];
        for (const char *c = last + 1; *c && o < max; ++c) out[o++] = *c;
    } else {
        for (unsigned i = 0; i < n && o < max; ++i)
            if (i == 0 || !strchr("AEIOU", buf[i])) out[o++] = buf[i];
    }
    out[o] = 0;
}
/* v12 knob bar (FM1-v12-screens.html knobs()): one font on every page. Name 16 px (13 px above six
 * letters), white; value 24 px in the knob colour; colour line 48 x 2 with a soft glow, 3 px and a
 * lit panel on the knob that was turned last. */
static unsigned knob_hot(unsigned col)
{
    return col < 4 && encoder_time[2 + col] && frame_time - encoder_time[2 + col] < 1500U; /* knobs are encoders 2..5 */
}
static void knob(unsigned col, const char *label_in, const char *value_in, uint16_t color)
{
    char label[16], value[16];
    const struct lcd_font *lf, *vf;
    lf = strlen(label_in) <= 6 && lcd_width(&lcd16, label_in) <= 56 ? &lcd16 : lcd_width(&lcd13, label_in) <= 56 ? &lcd13 : &lcd12;
    vf = lcd_width(&lcd24, value_in) <= 56 ? &lcd24 : lcd_width(&lcd20, value_in) <= 56 ? &lcd20 : lcd_width(&lcd16, value_in) <= 56 ? &lcd16 : &lcd13;
    fit(label_in, label, lf, 56);
    fit(value_in, value, vf, 56);
#ifdef FM1_HOST_TEST
    if (col < 4) {
        memset(web_knobs[col], 0, sizeof web_knobs[col]);
        unsigned n = (unsigned)strlen(label); if (n > 15) n = 15;
        memcpy(web_knobs[col][0], label, n);
        n = (unsigned)strlen(value); if (n > 15) n = 15;
        memcpy(web_knobs[col][1], value, n);
    }
#endif
    if (paint_rendering && (paint_top >= 240 || paint_top + PAINT_ROWS <= 195)) return;
    unsigned hot = knob_hot(col);
    uint32_t sig =
        signature(signature(color ^ (hot << 20), label, (unsigned)strlen(label)), value, (unsigned)strlen(value));
    if (cache->foot_cache[col] == sig)
        return;
    cache->foot_cache[col] = sig;
    unsigned x = 60 * col;
    paint_fill(x, 195, 60, 45, BACK);
    if (hot) rbox(x + 3, 195, 54, 45, 2, 0x1081, V_NONE);
    if (!*label && !*value) return;
    uint16_t kc = knob_colors[col & 3U];
    paint_fill(x + 6, 196, 48, 1, blend565(hot ? 0x1081 : BACK, kc, 1)); /* glow */
    paint_fill(x + 6, hot ? 200U : 199U, 48, 1, blend565(hot ? 0x1081 : BACK, kc, 1));
    paint_fill(x + 6, 197, 48, hot ? 3U : 2U, kc);
    uint16_t vc = (knob_tint >> col) & 1U ? color : kc;
    lcd_center((int)x + 30, lf == &lcd16 ? 201U : 203U, label, lf, label[0] == '-' && !label[1] ? V_FAINT : TEXT, hot ? 0x1081 : BACK); /* a lone dash = no function */
    lcd_center((int)x + 30, vf == &lcd24 ? 215U : vf == &lcd20 ? 217U : 219U, value, vf, vc, hot ? 0x1081 : BACK);
}
static __attribute__((noinline)) void header(const struct groove_view *v)
{
    if (paint_rendering && paint_top >= 30) return;
    unsigned paused = !v->playing && !groove_sequence_rewound();
    unsigned count = (groove_sequence_count_in() + 3U) / 4U;
    if (count > 4) count = 4;
    uint32_t sig = v->tempo | ((uint32_t)v->play_step << 10) |
        ((uint32_t)v->playing << 16) | (writing << 17) |
        ((uint32_t)v->clock_mode << 18) | ((uint32_t)v->clock_state << 19) | (step_follow << 22) | (arp_enabled(track) << 23) |
        ((uint32_t)(v->sequence_length >> 4) << 24) | (groove_project_dirty() << 27) | (paused << 28) | (count << 29);
    if (sig == cache->header_cache)
        return;
    uint32_t previous = cache->header_cache, changed = sig ^ previous;
    unsigned first = previous == ~0U;
    cache->header_cache = sig;
    if (first) {
        paint_fill(0, 0, 240, 30, BACK);
        text(122, 7, "/", 1, DIM, BACK);
    }
    if (first || (changed & (1U << 27)))
        text(165, 19, groove_project_dirty() ? "*" : " ", 1, AMBER, BACK);
    if (first || (changed & (7U << 24)))
        number(128, 7, v->sequence_length, 2, 1, DIM, BACK);
    /* Fixed-width glyphs replace their own background; a moving playhead
     * only updates its two digits and, at a page boundary, two indicators. */
    if (first || (changed & 1023U)) {
        if (v->clock_mode && !v->tempo)
            text(8, 8, "---", 2, DIM, BACK);
        else
            number(8, 8, v->tempo, 3, 2, TEXT, BACK);
    }
    if (first || (changed & (7U << 18))) {
        paint_fill(45, 15, 32, 7, BACK);
        text(45, 15, !v->clock_mode ? "BPM" : v->clock_state == GROOVE_CLOCK_LOST
            ? "LOST" : v->clock_state == GROOVE_CLOCK_LOCKED ? (groove_midi_setting(track,6) == 1 ? "USB" : groove_midi_setting(track,6) == 2 ? "BLE" : groove_midi_setting(track,6) == 3 ? "DIN" : "EXT") : "WAIT",
            1, v->clock_state == GROOVE_CLOCK_LOST ? AMBER : DIM, BACK);
    }
    if (first || (changed & ((1U << 16) | (1U << 28))))
        text(80, 7, v->playing ? "PLAY " : paused ? "PAUSE" : "STOP ", 1,
             v->playing ? GREEN : paused ? AMBER : DIM, BACK);
    if (first || (changed & (63U << 10)))
        number(110, 7, v->play_step + 1, 2, 1, TEXT, BACK);
    for (unsigned i = 0; i < 4 && 0; ++i) { /* sequence page sticks removed */
        unsigned active = v->playing && i == v->play_step / 16;
        unsigned was_active = (previous & (1U << 16)) && i == ((previous >> 14) & 3U);
        if (first || active != was_active || (changed & (7U << 24)))
            paint_fill(80 + i * 16, 20, 12, 3, active ? GREEN : i * 16 < v->sequence_length ? PANEL : BACK);
    }
    if (first || (changed & (1U << 22)))
        text(142, 7, step_follow ? "FOLLOW" : "EDIT  ", 1, step_follow ? GREEN : AMBER, BACK);
    if (first || (changed & ((1U << 16) | (1U << 17) | (1U << 23) | (7U << 29)))) {
        unsigned arp = arp_enabled(track) != 0;
        uint16_t color = writing ? AMBER : arp ? PANEL : BACK;
        paint_fill(181, 7, 51, 16, color);
        char counting[] = "CNT 4"; counting[4] = (char)('0' + count);
        if (writing || arp)
            text(192, 12, writing ? count ? counting : v->playing ? "REC" : "STEP" : "ARP", 1, writing ? BACK : TEXT, color);
    }
}
/* Battery percent from the raw ADC. 627 = full with the cable in (user, 22.10);
 * 516 = the unit switched itself off at 17 % of the old 493..627 scale (user, 28.10). */
enum { BAT_ADC_FULL = 627, BAT_ADC_EMPTY = 516 };
static unsigned battery_percent(void)
{
    int p = ((int)battery_adc - BAT_ADC_EMPTY) * 100 / (BAT_ADC_FULL - BAT_ADC_EMPTY);
    return (unsigned)clamp(p, 0, 100);
}
static void track_sound_name(const struct groove_view *v, unsigned t, char out[10]);
static const char *const engine_names[ENGINES] = {"DX7", "303", "808", "PSG", "PLUCK", "SAMPLER", "MICRO D"};
/* "N · ENGINE · SOUND"; the engine is left out when the sound name starts with it. */
static void track_title(const struct groove_view *v, unsigned t, char out[24])
{
    char name[10]; track_sound_name(v, t, name);
    const char *engine = v->type < ENGINES ? engine_names[v->type] : "";
    out[0] = (char)('1' + t); out[1] = 0;
    strcat(out, " ` ");
    if (strncmp(name, engine, strlen(engine))) { strcat(out, engine); strcat(out, " ` "); }
    strcat(out, name);
}
/* v6 pixel cat (16 x 16, mockup CAT). Letters index the palette below. */
static const char cat_v6_rows[16][17] = {"................","...o.......o....","..opo.....opo...","..okkooooookko..",
    ".okkdkkkkkdkkko.",".okkkkkkkkkkkko.",".okwgkkkkkwgkko.",".okbgkkkkkbgkko.",".okkkkcpckkkkko.",
    "..okkkccckkkko..","...ookkkkkkoo...","..okdkccccdko...","..okkccccckko.o.","..okdccccckdkook",
    "..okkcckkcckkko.","...oooo..oooo..."};
/* Sleeping cat (mockup CATSLEEP), lying down with closed eyes. */
static const char cat_v6_sleep[8][17] = {"..o.....o.......",".opo...opo......",".okkoookko.oooo.",
    "okkdkkkkdkooddko","okbbkkkbbkkkkkko","okkkcpckkkkdkdko",".okkcccokkkkkkko","..oooooooooooooo"};
/* Coats: ginger, black, white, tabby. Letters: outline, fur, dark fur, cream,
 * white, eye, black, pink. */
static uint16_t cat_v6_color(char ch, unsigned coat)
{
    static const char keys[] = "okdcwgbp";
    static const uint16_t pal[4][8] = {
        {0x3900, 0xfc60, 0xc2c0, 0xff19, 0xffff, 0x47e8, 0x0000, 0xfb55},
        {0x528b, 0x18c3, 0x2946, 0x39c8, 0xffff, 0xfe87, 0x0000, 0xfb55},
        {0x7bd0, 0xf79e, 0xde9b, 0xffff, 0xffff, 0x4d1f, 0x0000, 0xfc77},
        {0x2946, 0x9cd4, 0x4a6a, 0xe71d, 0xffff, 0x7f0b, 0x0000, 0xfb55}};
    const char *k = strchr(keys, ch);
    return k ? pal[coat & 3U][k - keys] : BACK;
}
/* up = paws raised (mockup CATUP), flip = mirrored, sleep = lying down;
 * drawn at scale s. */
static void cat_v6(unsigned x, unsigned y, unsigned s, unsigned up, unsigned flip, unsigned sleep, unsigned coat)
{
    for (unsigned r = 0; r < 16; ++r)
        for (unsigned c = 0; c < 16; ++c) {
            char ch = sleep ? (r >= 7 && r < 15 ? cat_v6_sleep[r - 7][c] : '.') : cat_v6_rows[r][c];
            if (!sleep && up && r == 0) ch = c == 0 || c == 15 ? 'o' : '.';
            if (!sleep && up && r == 1) ch = "ok.o.......o.ko."[c];
            if (!sleep && up && r == 2) ch = "okopo.....opoko."[c];
            if (!sleep && up && r == 9) ch = "..okkkcbckkkko.."[c];
            if (ch != '.') paint_fill(x + (flip ? 15U - c : c) * s, y + r * s, s, s, cat_v6_color(ch, coat));
        }
}
/* "LABEL [ON]" - the lamp centred on the label's ink, its text centred inside. */
static void label_lamp(unsigned x, unsigned y, const char *label, const struct lcd_font *f, uint16_t color, unsigned on)
{
    unsigned lw = lcd_width(f, label), w = on ? 16U : 20U;
    unsigned ly = y + f->digit_top + f->digit_height / 2U - 4U;
    uint16_t c = on ? 0x3f0f : V_RED;
    lcd_text((int)x, y, label, f, color, BACK);
    rbox(x + lw + 3U, ly, w, 9, 2, c, V_NONE);
    lcd_center((int)(x + lw + 3U + w / 2U), ly + 1U, on ? "ON" : "OFF", &lcd8, BACK, c);
}
/* Pages already drawn in the v6 style (top bar instead of the old header). */
static unsigned v6_page(unsigned p)
{
    return p != STARTUP && p != SAFE && p != OVERVIEW;
}
/* v7 SEL: SELECT picks what (STEP / TRACK / SOUND), a knob picks the action, SAVE does it.
 * Numbers index actions[]; 255 = empty. v13: three areas as drawn, the PAGE area (COPY / PASTE / CLEAR PAGE and
 * FILL = REPEAT TO 32/48/64, 200 = SEL_FILL) is no longer offered; their code below stays. */
enum { SEL_ROWS = 3, SEL_FILL = 200 };
static const uint8_t sel_grid[SEL_ROWS][4] = {
    {2, 5, 8, 255}, {4, 7, 10, 14}, {15, 16, 21, 17}}; /* 21 = TRACK ROLE: the direct entry to the SEQ page; STEP > MIDI removed (user 09.11, MIDI lives in GLOBAL) */
static const char *const sel_rows[SEL_ROWS] = {"STEP", "TRACK", "SOUND"};
static const char *const sel_items[SEL_ROWS][4] = {
    {"COPY", "PASTE", "CLEAR", ""}, {"COPY", "PASTE", "CLEAR", "UNDO"}, {"COPY", "PASTE", "ROLE", "NAME"}};
static unsigned sel_row(void)
{
    for (unsigned r = 0; r < SEL_ROWS; ++r) for (unsigned c = 0; c < 4; ++c) if (sel_grid[r][c] == action) return r;
    return 0;
}
static unsigned sel_col(void)
{
    unsigned r = sel_row();
    for (unsigned c = 0; c < 4; ++c) if (sel_grid[r][c] == action) return c;
    return 0;
}
/* v6 top bar. Left: BPM and the F/A/S/L badges (follow, arp, sequencer running,
 * LFO). Right: transport icon, battery percent and a white battery, centred on
 * the digits. Centre: line 1 the section, line 2 what is being worked on. */
static void fill_tri(int ax, int ay, int bx, int by, int cx, int cy, uint16_t c);
static void fill_ellipse(int cx, int cy, int rx, int ry, uint16_t c);
/* Small arrow beside the section title: SELECT can turn this way. The
 * vertical side (7 px) is longer than the two slanted ones (tip 3.5 px out). */
static void select_arrow(int x, int cy, int dir)
{
    fill_tri(x * 10, cy * 10 - 35, x * 10, cy * 10 + 35, x * 10 + dir * 35, cy * 10, V_DIM);
}
/* arrows: bit 0 = SELECT can go left (back), bit 1 = right (forward). */
static void engine_icon(unsigned x, unsigned y, int engine, uint16_t c);
/* v9 header, the same on every page but the mixer:
 * row 1 (y 0..25): BPM (white digits, green label) | the page's name, centred,
 *   20 px | play / record icon and the battery, centred on the digits;
 * row 2 (y 26..61): F A S L as a 2x2 block under the BPM, the engine icon and
 *   the sound's name next to it (centred on the screen when short), an info
 *   window under the battery (top_info_label / top_info_value), or an alert.
 * The mixer keeps F A S L as one row and shows no engine group.
 * top_row2: 0 mixer, 1 full row 2, 2 row 2 with its right side drawn by the page,
 * 3 row 2 = F A S L only, the page draws right of it (MICRO D path), 4 no row 2 (SCOPE). */
static const char *top_info_label, *top_info_value;
static uint16_t top_section_color = 0xf77e; /* the title's colour (TRACK FX: the track's) */
static unsigned top_row2 = 1;
static unsigned top_big_name; /* GLOBAL: the chosen tile's name in 30 px at y 27, the page draws its plate from x 200 */
enum { HDR_FY = 27, HDR_FQ = 16, HDR_FG = 2, CONTENT_TOP = 62 };
/* v12.1: every animation in the central field is MATRIX green (curves, bars, waves, the play cursor) */
enum { MX_G = 0x07e8, MX_H = 0xdfdb, MX_D = 0x0141 };
enum { GLOB_TILE = 0x1083, GLOB_TILE_EDGE = 0x39a8 }; /* the mockup's flat tile #15131a / #3a3540 */
enum { MD_CREAM = 0xf75a, MD_AMBER = 0xfd88, MD_DIMC = 0xa4d0, MD_PANEL = 0x0841, MD_EDGE = 0x6b0a, MD_ORANGE = 0xeba7, MD_BLUE = 0x5d5d };
static float vsin(float c);
static void state_plate(const char *big, int chain, uint16_t numcolor);
static void list_row(unsigned y, unsigned h, unsigned sel, const char *left, const char *right, uint16_t rc, uint16_t lc);
static const uint16_t engine_colors[ENGINES] = {PAL_GREEN, PAL_LIME, PAL_ORANGE, PAL_ROSE, PAL_BLUE, PAL_YELLOW, PAL_AMBER}; /* DX7, 303, 808, PSG, PLUCK, SAMPLER, MICRO D (BINAURAL: PAL_PURPLE) */
/* A track's colour is its engine's colour, on every page (user, 29.10). */
static uint16_t track_color(unsigned t)
{
    unsigned eng = micro_d_track(t) ? (unsigned)ENGINE_MICRO : groove_source_type(t);
    return eng < ENGINES ? engine_colors[eng] : TEXT;
}
/* ---- phase G shared v13 style pieces (G-A). Pages use these, never their own copies. ---- */
#define STYLE_HELPER static __attribute__((unused))
/* Frequency axis, log 20 Hz .. 20 kHz over w pixels (mockup LGF / FXF). Positions are 1/1024 of the width. */
STYLE_HELPER unsigned freq_x(unsigned x, unsigned w, unsigned hz)
{
    static const uint16_t lg[17] = {0, 90, 174, 254, 330, 402, 470, 536, 599, 659, 717, 773, 827, 879, 929, 977, 1024};
    if (hz < 20U) hz = 20U;
    if (hz > 20000U) hz = 20000U;
    unsigned e = 0;
    while ((hz >> e) >= 2U) ++e;
    unsigned m = (hz << 10) >> e, i = (m - 1024U) >> 6, r = (m - 1024U) & 63U; /* hz = 2^e * m / 1024 */
    unsigned l2 = e * 1024U + lg[i] + (lg[i + 1] - lg[i]) * r / 64U; /* log2(hz) in 1/1024 */
    unsigned u = l2 > 4426U ? (l2 - 4426U) * 1024U / 10205U : 0U; /* (log2 hz - log2 20) / log2 1000 */
    return x + (w * u >> 10);
}
/* Inverse: the frequency at pixel column px of the axis that starts at x and is w wide (for curves drawn per column). */
STYLE_HELPER unsigned freq_at(unsigned x, unsigned w, unsigned px)
{
    static const uint16_t ex[17] = {1024, 1069, 1117, 1166, 1218, 1272, 1328, 1387, 1448, 1512, 1579, 1649, 1722, 1798, 1878, 1961, 2048};
    unsigned u = px <= x || !w ? 0U : px - x >= w ? 1024U : (px - x) * 1024U / w;
    unsigned t = u * 10205U / 1024U + 4426U, e = t >> 10, f = t & 1023U, i = f >> 6;
    unsigned m = ex[i] + (ex[i + 1] - ex[i]) * (f & 63U) / 64U;
    return (m << e) >> 10;
}
/* The nine frequency zones (3 x 3 on the log axis, borders 250 Hz and 4 kHz; low red, mid green, high blue; each
 * main zone in three thin hue-shifted zones, alpha 18 / 30 / 42 %), drawn bottom-up: full at the bottom, transparent
 * at the top, plus a faint border line per main border. No labels. Draw the curve on top (use freq_x / freq_at). */
STYLE_HELPER void freq_zones(unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (w < 16 || h < 4) return;
    if (!paint_rendering) { glass_mark((int)x, (int)y, w, h); return; }
    if (y >= paint_top + PAINT_ROWS || y + h <= paint_top || x >= paint_end || x + w <= paint_x) return;
    static const uint16_t zc[9] = {0xf1a8, 0xf266, 0xf3c6, 0x47a6, 0x37aa, 0x37b0, 0x351e, 0x33be, 0x323e}; /* hsl(h, 90 %, 58 %) */
    static const uint16_t zb[10] = {0, 125, 250, 374, 511, 648, 785, 865, 944, 1024}; /* zone borders in 1/1024 of w */
    static const uint8_t za[3] = {46, 77, 107}; /* .18 .30 .42 of 256 */
    int lo = (int)y < (int)paint_top ? (int)paint_top : (int)y;
    int hi = (int)(y + h) > (int)(paint_top + PAINT_ROWS) ? (int)(paint_top + PAINT_ROWS) : (int)(y + h);
    for (int py = lo; py < hi; ++py) {
        unsigned k = (unsigned)(py - (int)y), up = 2U * k + 1U; /* row centre from the top; alpha grows towards the bottom */
        for (unsigned z = 0; z < 9; ++z) {
            unsigned a = za[z % 3U] * up / (2U * h);
            glass_span(py, (int)(x + (w * zb[z] >> 10)), (int)(x + (w * zb[z + 1] >> 10)), zc[z], a);
        }
        unsigned a = 56U * up / (2U * h); /* border lines: white .22 -> 0 */
        glass_span(py, (int)(x + (w * zb[3] >> 10)), (int)(x + (w * zb[3] >> 10)) + 1, GLASS_WHITE, a);
        glass_span(py, (int)(x + (w * zb[6] >> 10)), (int)(x + (w * zb[6] >> 10)) + 1, GLASS_WHITE, a);
    }
}
/* grad_fill_seg: the area under the straight segment (x0, y0) - (x1, y1) down to y_base (exclusive), colour with
 * alpha a_bottom (0..255; the mockup envelopes use 128) at y_base fading linearly to 0 at y_fade (a row above the
 * curve's highest point, e.g. min(y) - 4). Chain one call per segment of a polyline. */
STYLE_HELPER void grad_fill_seg(int x0, int y0, int x1, int y1, int y_base, int y_fade, uint16_t colour, unsigned a_bottom)
{
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y_base <= y_fade) return;
    if (!paint_rendering) {
        int top = y0 < y1 ? y0 : y1;
        if (top < y_base) glass_mark(x0, top, (unsigned)(x1 - x0 + 1), (unsigned)(y_base - top));
        return;
    }
    unsigned a256 = a_bottom + (a_bottom >> 7), span = (unsigned)(y_base - y_fade);
    int cmin = x0 < (int)paint_x ? (int)paint_x : x0, cmax = x1 + 1 > (int)paint_end ? (int)paint_end : x1 + 1;
    for (int px = cmin; px < cmax; ++px) {
        int ys = x1 == x0 ? y0 : y0 + (y1 - y0) * (px - x0) / (x1 - x0);
        int lo = ys < (int)paint_top ? (int)paint_top : ys;
        int hi = y_base > (int)(paint_top + PAINT_ROWS) ? (int)(paint_top + PAINT_ROWS) : y_base;
        for (int py = lo; py < hi; ++py) {
            int d = py - y_fade;
            if (d < 0) continue;
            glass_span(py, px, px + 1, colour, a256 * (2U * (unsigned)d + 1U) / (2U * span));
        }
    }
}
/* grad_fill: a rectangle filled bottom-up (alpha a_bottom at the bottom edge, 0 at the top edge). */
STYLE_HELPER void grad_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t colour, unsigned a_bottom)
{
    if (!w || !h) return;
    if (!paint_rendering) { glass_mark((int)x, (int)y, w, h); return; }
    unsigned a256 = a_bottom + (a_bottom >> 7);
    int lo = (int)y < (int)paint_top ? (int)paint_top : (int)y;
    int hi = (int)(y + h) > (int)(paint_top + PAINT_ROWS) ? (int)(paint_top + PAINT_ROWS) : (int)(y + h);
    for (int py = lo; py < hi; ++py)
        glass_span(py, (int)x, (int)(x + w), colour, a256 * (2U * (unsigned)(py - (int)y) + 1U) / (2U * h));
}
/* Last touched knob (round 4): sound pages show the picture of this knob at rest. knob_enter() on entering a page
 * (K1 shows), knob_touch(k) from the knob handler (k 0..3), knob_shown() when drawing the picture. */
static uint8_t knob_last_touched;
STYLE_HELPER void knob_enter(void) { knob_last_touched = 0; }
STYLE_HELPER void knob_touch(unsigned k) { knob_last_touched = (uint8_t)(k & 3U); }
STYLE_HELPER unsigned knob_shown(void) { return knob_last_touched; }
/* noise_dots: 2 x 2 dots in the box, their number grows with amount (0..256; 256 = 170 dots per 206 x 72 px, mockup
 * pNoise). Dot i has a fixed place, so raising the value only adds dots. */
STYLE_HELPER void noise_dots(unsigned x, unsigned y, unsigned w, unsigned h, unsigned amount, uint16_t colour)
{
    if (w < 4 || h < 4) return;
    if (!paint_rendering) { glass_mark((int)x, (int)y, w, h); return; }
    if (y >= paint_top + PAINT_ROWS || y + h <= paint_top || x >= paint_end || x + w <= paint_x) return;
    if (amount > 256U) amount = 256U;
    unsigned n = amount * 170U * w / 256U * h / 14832U;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t k = (i + 1U) * 0x9E3779B1U;
        k ^= k >> 15; k *= 0x85EBCA6BU; k ^= k >> 13;
        unsigned dy = y + (((k >> 16) * (h - 2U)) >> 16);
        if (dy + 2U <= paint_top || dy >= paint_top + PAINT_ROWS) continue;
        paint_fill(x + (((k & 0xffffU) * (w - 2U)) >> 16), dy, 2, 2, colour);
    }
}
/* icon_row: the page icons in the header row (y 27 .. 61). 1..5 plates are 35 px wide on a 39 px pitch, 6..7 are
 * 23 px on 25.4, 8 are 22 px on 24.4; plate i calls draw[i](cx, cy, colour) with colour page_icon_colors[i % 6] and
 * the icon centre (plate centre, 44); the plate `cur` wears the amber glass cursor (cur >= n: none). The icons are
 * the page workers' own functions (about 18 px, thin line strokes as in the mockup's IC table). */
typedef void (*icon_draw_fn)(int cx, int cy, uint16_t colour);
STYLE_HELPER void icon_row(unsigned n, unsigned cur, const icon_draw_fn *draw)
{
    if (!n || n > 8) return;
    if (paint_rendering && (paint_top >= HDR_FY + 34U || paint_top + PAINT_ROWS <= HDR_FY)) return;
    unsigned w = n >= 8 ? 22U : n >= 6 ? 23U : 35U, pitch10 = n >= 8 ? 244U : n >= 6 ? 254U : 390U, x0 = n >= 6 ? 43U : 44U;
    for (unsigned i = 0; i < n; ++i) {
        unsigned x = x0 + i * pitch10 / 10U;
        rbox(x, HDR_FY, w, 34, 3, 0x10a3, 0x5acb);
        if (draw[i]) draw[i]((int)(x + w / 2U), HDR_FY + 17, page_icon_colors[i % 6U]);
        if (i == cur) glass_cursor(x, HDR_FY, w, 34, 8, MD_AMBER); /* radius 8 = the plate's pebble corner (rbox r 3) */
    }
}
static __attribute__((noinline)) void top_bar(const struct groove_view *v, const char *section, const char *object, uint16_t object_color, unsigned arrows, int engine)
{
    if (paint_rendering && paint_top >= CONTENT_TOP) return;
    unsigned wait = v->clock_mode && !v->tempo;
    unsigned src = groove_midi_setting(track, 6);
    uint32_t sig = (wait ? 0U : v->tempo) | ((uint32_t)v->clock_mode << 10) | ((uint32_t)v->clock_state << 11) | (src << 14);
    if (sig != cache->top_cache[0]) {
        cache->top_cache[0] = sig;
        paint_fill(0, 0, 70, 26, BACK);
        char bpm[12];
        if (wait) strcpy(bpm, "---"); else decimal(bpm, v->tempo, 1);
        lcd_text(5, 3, bpm, &lcd22, TEXT, BACK); /* same size and line as the battery digits */
        const char *label = !v->clock_mode ? "BPM" : v->clock_state == GROOVE_CLOCK_LOST ? "LOST"
            : v->clock_state != GROOVE_CLOCK_LOCKED ? "WAIT" : src == 1 ? "USB" : src == 2 ? "BLE" : src == 3 ? "DIN" : "EXT";
        lcd_text(7 + (int)lcd_width(&lcd22, bpm), 10, label, &lcd13, /* v12: half as big again */
                 v->clock_mode && v->clock_state == GROOVE_CLOCK_LOST ? V_ORANGE : knob_colors[1], BACK);
    }
    unsigned lfo_on = (lfo_get(0, 0) != 0 && lfo_get(0, 2) != 63) || (lfo_get(1, 0) != 0 && lfo_get(1, 2) != 63);
    unsigned badges = step_follow | ((arp_enabled(track) != 0) << 1) | ((v->playing != 0) << 2) | (lfo_on << 3);
    static const char letters[4][2] = {"F", "A", "S", "L"};
    static const uint16_t tint[4] = {TEXT, 0x3f0f, V_RED, 0x2ed9};
    unsigned paused = !v->playing && !groove_sequence_rewound();
    unsigned state = writing ? 3U : v->playing ? 0U : paused ? 1U : 2U, pct = battery_shown > 100U ? battery_percent() : battery_shown;
    uint32_t right = signature(state | (pct << 2) | (keys_latch << 29) | (arrows << 30), section, (unsigned)strlen(section)) ^ top_section_color;
    const int ink2 = 2 * (3 + lcd22.digit_top) + lcd22.digit_height - 1, bx = 228;
    char num[12]; decimal(num, (int)pct, 1);
    int dr = bx - 4, px = dr - (int)lcd_width(&lcd22, num) - 4 - 11, py = (ink2 - 12 + 1) / 2;
    if (right != cache->top_cache[2]) {
        cache->top_cache[2] = right;
        paint_fill(70, 0, 170, 26, BACK);
        const int bt = (ink2 - 14 + 1) / 2 + 2; /* battery body top */
        rbox(bx, (unsigned)bt, 6, 13, 1, V_NONE, TEXT);
        paint_fill(bx + 2, (unsigned)(bt - 2), 2, 2, TEXT);
        unsigned h = (pct * 9U + 50U) / 100U;
        if (h) paint_fill(bx + 2, (unsigned)(bt + 11) - h, 2, h, TEXT);
        uint16_t pc = pct >= 65 ? TEXT : pct >= 30 ? 0xfe87 : pct >= 10 ? V_ORANGE : V_RED;
        lcd_text(dr - (int)lcd_width(&lcd22, num), 3, num, &lcd22, pc, BACK);
        if (state == 0) {
            for (int r = 0; r < 11; ++r) { /* a slightly smaller play triangle, centred on the digits */
                int d = r < 5 ? 5 - r : r - 5, len = (9 * (11 - 2 * d) + 5) / 11;
                paint_fill((unsigned)px + 1, (unsigned)(py + 1 + r), (unsigned)len, 1, 0x3f0f);
            }
        } else if (state == 1) {
            paint_fill((unsigned)px, (unsigned)py, 4, 13, 0xfe87);
            paint_fill((unsigned)px + 7, (unsigned)py, 4, 13, 0xfe87);
        } else if (state == 3) fill_ellipse((px + 5) * 10 + 5, (py + 6) * 10 + 5, 55, 55, V_RED); /* recording */
        int left = 72, rgt = px - 6, cx = 120;
        if (keys_latch) { /* a lit LATCH plate left of PLAY */
            rgt -= 30;
            rbox((unsigned)(px - 34), (unsigned)(py + 1), 28, 11, 2, 0xfe87, V_NONE);
            ++keep_names; lcd_center(px - 20, (unsigned)(py + 2), "LATCH", &lcd8, BACK, 0xfe87); --keep_names;
        }
        unsigned maxw = (unsigned)(2 * (cx - left < rgt - cx ? cx - left : rgt - cx));
        const struct lcd_font *sf = lcd_width(&lcd20, section) <= maxw ? &lcd20 : lcd_width(&lcd17, section) <= maxw ? &lcd17 : &lcd13;
        char line1[16]; fit(section, line1, sf, maxw);
        lcd_center(cx, 3, line1, sf, top_section_color, BACK);
        if (arrows) {
            int half = (int)lcd_width(sf, line1) / 2, ay = 3 + sf->digit_top + sf->digit_height / 2;
            if (arrows & 1U) select_arrow(cx - half - 5, ay, -1);
            if (arrows & 2U) select_arrow(cx + half + 5, ay, 1);
        }
    }
    if (paint_rendering && paint_top + PAINT_ROWS <= 26) return;
    if (top_row2 == 4) return; /* v13 SCOPE: BPM, PLAY and battery only */
    if (!top_row2) { /* mixer: F A S L as one row, v11: the sound name right of it */
        unsigned steps = v->next_length, chain = chain_on != 0;
        uint32_t mix = signature(badges | ((uint32_t)(engine + 1) << 8) | ((uint32_t)object_color << 12) | (chain << 28), object, (unsigned)strlen(object)) + steps * 7919U;
        if (mix != cache->top_cache[1]) {
            cache->top_cache[1] = mix;
            paint_fill(0, 26, 240, 17, BACK);
            for (unsigned i = 0; i < 4; ++i) {
                unsigned x = 5 + i * 15, on = badges >> i & 1U;
                rbox(x, 29, 12, 12, 2, on ? tint[i] : V_NONE, on ? V_NONE : V_FAINT);
                lcd_center((int)x + 6, 29, letters[i], &lcd11, on ? BACK : V_FAINT, on ? tint[i] : BACK);
            }
            if (object[0]) {
                uint16_t ec = engine >= 0 && engine < ENGINES ? engine_colors[engine] : object_color;
                unsigned name = engine >= 0; /* sound names stay as they are, alerts translate */
                char nm[16]; keep_names += name; fit(object, nm, &lcd17, 100); /* under the title, x 70..170 */
                lcd_center(120, 26, nm, &lcd17, ec, BACK); keep_names -= name;
            }
            /* v12: the step count (smaller than on SEQ) and a chain of one and
             * a half links under the battery: yellow on, grey off */
            char num[12]; decimal(num, (int)steps, 1);
            int nw = (int)lcd_width(&lcd14, num);
            lcd_text(222 - nw, 28, num, &lcd14, v->next_length != v->sequence_length ? V_ORANGE : TEXT, BACK);
            uint16_t cc = chain ? 0xfe87 : 0x5aeb;
            rbox(226, 27, 8, 11, 3, V_NONE, cc); rbox(227, 28, 6, 9, 2, V_NONE, cc);
            rbox(228, 35, 8, 8, 3, V_NONE, cc);
        }
        return;
    }
    uint32_t row2 = signature(signature(signature(badges | ((uint32_t)(engine + 1) << 8) | ((uint32_t)object_color << 12) | (top_row2 << 30),
                                                  object, (unsigned)strlen(object)),
                                        top_info_label ? top_info_label : "", top_info_label ? (unsigned)strlen(top_info_label) : 0U),
                              top_info_value ? top_info_value : "", top_info_value ? (unsigned)strlen(top_info_value) : 0U);
    if (row2 == cache->top_cache[1]) return;
    cache->top_cache[1] = row2;
    unsigned edge = top_row2 == 2 ? (top_big_name ? 198U : 168U) : top_row2 == 3 ? 42U : 240U; /* SEQ draws STEPS / CHAIN right of 168 */
    paint_fill(0, 26, edge, CONTENT_TOP - 26, BACK);
    for (unsigned i = 0; i < 4; ++i) {
        unsigned x = 5 + (i % 2U) * (HDR_FQ + HDR_FG), y = HDR_FY + (i / 2U) * (HDR_FQ + HDR_FG), on = badges >> i & 1U;
        rbox(x, y, HDR_FQ, HDR_FQ, 2, on ? tint[i] : V_NONE, on ? V_NONE : V_FAINT);
        lcd_center((int)x + HDR_FQ / 2, y + 1, letters[i], &lcd14, on ? BACK : V_FAINT, on ? tint[i] : BACK);
    }
    int info_left = (int)edge - 2;
    if (top_info_label && top_row2 == 1) {
        unsigned lw = lcd_width(&lcd13, top_info_label), vw = top_info_value ? lcd_width(&lcd22, top_info_value) : 0;
        if (top_info_value) {
            lcd_text(235 - (int)lw, HDR_FY - 1, top_info_label, &lcd13, V_DIM, BACK);
            lcd_text(235 - (int)vw, HDR_FY + 11, top_info_value, &lcd22, TEXT, BACK);
        } else lcd_text(235 - (int)lw, HDR_FY + 8, top_info_label, &lcd13, TEXT, BACK);
        info_left = 235 - (int)(lw > vw ? lw : vw) - 6;
    }
    const int sq = 2 * HDR_FQ + HDR_FG, lo = 5 + sq + 5;
    if (top_row2 == 3) return;
    if (engine >= 0 && object[0]) {
        /* engine icon + the sound's name: centred on the screen when short,
         * pressed to F A S L and cut when long; never over the info window */
        ++keep_names;
        int hi = info_left, avail = hi - (lo + sq + 6);
        char nm[24]; strncpy(nm, object, 23); nm[23] = 0;
        const struct lcd_font *nf = &lcd20;
        if ((int)lcd_width(nf, nm) > avail) nf = &lcd17;
        for (unsigned n = (unsigned)strlen(nm); n > 1 && (int)lcd_width(nf, nm) > avail; ) nm[--n] = 0;
        int gw = sq + 6 + (int)lcd_width(nf, nm), gx = 120 - gw / 2;
        if (gx + gw > hi) gx = hi - gw;
        if (gx < lo) gx = lo;
        uint16_t ec = engine < ENGINES ? engine_colors[engine] : knob_colors[1];
        rbox((unsigned)gx, HDR_FY, (unsigned)sq, (unsigned)sq, 3, V_NONE, ec);
        engine_icon((unsigned)gx + 2, HDR_FY + 4, engine, TEXT); /* the icon is drawn for a 31 px box */
        lcd_text(gx + sq + 6, (unsigned)(HDR_FY + sq / 2 - nf->digit_top - nf->digit_height / 2), nm, nf, ec, BACK);
        --keep_names;
    } else if (object[0]) { /* an alert or a line of its own, between F A S L and the info window */
        /* v11: as large as fits, on the screen centre when it clears F A S L
         * and the info window, else centred between them (FX names) */
        int hi = info_left, cxo = (lo + hi) / 2;
        unsigned maxw = (unsigned)(hi - lo);
        static const struct lcd_font *const sizes[6] = {&lcd30, &lcd24, &lcd20, &lcd17, &lcd13, &lcd11};
        const struct lcd_font *of = sizes[5];
        for (unsigned k = !top_big_name; k < 6; ++k) if (lcd_width(sizes[k], object) <= maxw) { of = sizes[k]; break; }
        char line2[24]; strncpy(line2, object, 23); line2[23] = 0;
        for (unsigned n = (unsigned)strlen(line2); n > 1 && lcd_width(of, line2) > maxw; ) line2[--n] = 0;
        int half = (int)lcd_width(of, line2) / 2;
        if (120 - half >= lo && 120 + half <= hi) cxo = 120;
        lcd_center(cxo, top_big_name ? (unsigned)HDR_FY : (unsigned)(HDR_FY + sq / 2 - of->digit_top - of->digit_height / 2), line2, of, object_color, BACK);
    }
}
static __attribute__((noinline)) void overview(const struct groove_view v[8])
{
    if (paint_rendering && (paint_top >= 190 || paint_top + PAINT_ROWS <= 32)) return;
    unsigned first = step_first(&v[track]);
    char project_name[13]; groove_project_name(project_name);
    uint32_t title = signature(first / 16 | (step_follow << 3) | (cursor << 4) |
        ((unsigned)v[track].sequence_length << 10), project_name, 12);
    if (title != cache->title_cache) {
        cache->title_cache = title;
        paint_fill(0, 32, 240, 13, BACK);
        text(8, 35, project_name, 1, ACCENT, BACK);
        text(86, 35, "OPEN " FM1_RELEASE_VERSION, 1, DIM, BACK);
        text(146, 35, step_follow ? "PAGE" : "STEP", 1, DIM, BACK);
        number(178, 35, step_follow ? first + 1 : cursor + 1, 2, 1, TEXT, BACK);
        text(190, 35, step_follow ? "-" : "/", 1, DIM, BACK);
        number(196, 35, step_follow ? first + 16 : v[track].sequence_length, 2, 1, TEXT, BACK);
    }
    static const char *const chip_names[] = {"PSG YM", "PSG AY", "PSG NOIS", "PSG MIX"};
    for (unsigned t = 0; t < 8; ++t) {
        unsigned y = 48 + 18 * t;
        if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + 16 <= paint_top)) continue;
        uint32_t meta = (v[t].program | ((uint32_t)v[t].volume << 7) | ((t == track) << 14) |
                        ((uint32_t)v[t].modified << 15) | ((uint32_t)v[t].muted << 16) |
                        ((uint32_t)v[t].solo << 17) | ((uint32_t)v[t].ready << 18) |
                        ((uint32_t)v[t].type << 19) | ((uint32_t)midi_active[t] << 22) |
                        ((uint32_t)groove_track_role(t) << 23)) ^ ((uint32_t)sounds_id(t) * 0x9e3779b1U);
        if (meta != cache->row_cache[t]) {
            cache->row_cache[t] = meta;
            paint_fill(5, y, 71, 16, BACK);
            uint16_t bg = t == track ? track_color(t) : PANEL;
            paint_fill(6, y, 18, 16, bg);
            number(12, y + 5, t + 1, 1, 1, t == track ? BACK : track_color(t), bg);
            paint_fill(7, y + 12, 3, 3, midi_active[t] ? GREEN : bg);
            const char *fm_name = v[t].type == GROOVE_DX7 ? fm_track_name(t) : "";
            const char *lib = sounds_name(t);
            ++keep_names; /* sound names stay as they are */
            text(29, y + 1,
                 v[t].type == ENGINE_MICRO    ? micro_d_preset_name(v[t].program)
                 : lib && v[t].ready          ? lib
                 : !v[t].ready                ? "ERROR"
                 : v[t].type == GROOVE_DX7    ? fm_name
                 : v[t].type == GROOVE_303    ? (v[t].program ? "303 SQR" : "303 SAW")
                 : v[t].type == GROOVE_PSG    ? chip_names[v[t].program & 3U]
                 : v[t].type == GROOVE_PLUCK  ? "PLUCK"
                 : v[t].type == GROOVE_SAMPLE ? "SAMPLER"
                                              : "808 KIT",
                 1, track_color(t), BACK);
            --keep_names;
            text(29, y + 9,
                 groove_track_role(t) == GROOVE_TRACK_OFF ? "OFF"
                 : groove_track_role(t) == GROOVE_TRACK_PLAY ? (v[t].muted ? "P MUT" : v[t].solo ? "P SOLO" : "PLAY")
                 : v[t].muted      ? "MUTE"
                 : v[t].solo     ? "SOLO"
                 : v[t].modified ? "EDITED"
                                 : "",
                 1, DIM, BACK);
            number(214, y + 1, v[t].volume, 3, 1, t == track ? TEXT : DIM, BACK);
        }
        for (unsigned i = 0; i < 16; ++i) {
            unsigned step = first + i;
            unsigned enabled = step < v[t].sequence_length;
            unsigned on = enabled && (unsigned)((v[t].active_steps >> step) & 1U);
            unsigned head = v[t].playing && v[t].play_step == step;
            unsigned selected = enabled && t == track && step == cursor;
            uint32_t sig = on | (head << 1) | (selected << 2) | ((uint32_t)v[t].audible << 3) | (enabled << 4);
            if (sig == cache->cell_cache[t][i])
                continue;
            cache->cell_cache[t][i] = sig;
            unsigned x = 80 + i * 8;
            paint_fill(x, y + 3, 6, 11, selected ? TEXT : BACK);
            paint_fill(x + 1, y + 5, 4, 8, on ? (v[t].audible ? track_color(t) : DIM) : enabled ? PANEL : BACK);
            if (head)
                paint_fill(x, y + 3, 6, 2, GREEN);
        }
        unsigned meter = v[t].audible ? (v[t].level * 14 + 63) / 127 : 0;
        unsigned meter_signature = meter | (v[t].level >= 124 ? 16U : 0U);
        if (meter_signature != cache->meter_cache[t]) {
            cache->meter_cache[t] = meter_signature;
            paint_fill(216, y + 11, 14, 3, PANEL);
            if (meter)
                paint_fill(216, y + 11, meter, 3, v[t].level >= 124 ? AMBER : track_color(t));
        }
    }
}
/* Short sound name of a track for the mixer rows (nine characters at most). */
static void track_sound_name(const struct groove_view *v, unsigned t, char out[10])
{
    static const char *const chip_names[] = {"PSG YM", "PSG AY", "PSG NOISE", "PSG MIX"};
    if (v->type == ENGINE_MICRO) { strncpy(out, micro_d_preset_name(v->program), 9); out[9] = 0; return; }
    const char *lib = v->ready ? sounds_name(t) : 0;
    const char *fm_name = !lib && v->ready && v->type == GROOVE_DX7 ? fm_track_name(t) : ""; /* V14: names come from the voice itself */
    const char *name = lib                        ? lib
                       : !v->ready                  ? "ERROR"
                       : v->type == GROOVE_DX7    ? fm_name
                       : v->type == GROOVE_303    ? (v->program ? "303 SQR" : "303 SAW")
                       : v->type == GROOVE_PSG    ? chip_names[v->program & 3U]
                       : v->type == GROOVE_PLUCK  ? "PLUCK"
                       : v->type == GROOVE_SAMPLE ? "SAMPLER"
                                                  : "808 KIT";
    unsigned n = 0;
    while (n < 9 && name[n]) { out[n] = name[n]; ++n; }
    while (n && out[n - 1] == ' ') --n;
    out[n] = 0;
}
/* HOME: eight narrow rows, no notes. Left: track number and sound. Middle: the
 * loop's progress bar with a bright stick at the playing step. Right: M/S flag
 * and level. SELECT moves the highlighted row; KNOB 1 turned right opens it. */
/* Step lamp: a 1 px rounded frame (outer corners cut, inner corners filled),
 * black inside, optional fill rising from the bottom. */
enum { LAMP_GREEN = 0x3f2a, LAMP_GREY = 0x4208, LAMP_BEAT = 0x6b4d, LAMP_BAR = 0x8c71,
       HEAD_ON = 0xf8a3, HEAD_OFF = 0x9000, HEAD_SHADE = 0x79e2 };
/* Mix an RGB565 colour toward white (up) or black, by tenths. */
static uint16_t shade(uint16_t c, unsigned up, unsigned tenths)
{
    unsigned r = c >> 11, g = (c >> 5) & 63U, b = c & 31U;
    if (up) { r += (31U - r) * tenths / 10U; g += (63U - g) * tenths / 10U; b += (31U - b) * tenths / 10U; }
    else { r = r * (10U - tenths) / 10U; g = g * (10U - tenths) / 10U; b = b * (10U - tenths) / 10U; }
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static void lamp(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t edge, uint16_t fill, unsigned fill_h)
{
    paint_fill(x, y, w, h, BACK);
    paint_fill(x + 1, y, w - 2, 1, edge);
    paint_fill(x + 1, y + h - 1, w - 2, 1, edge);
    paint_fill(x, y + 1, 1, h - 2, edge);
    paint_fill(x + w - 1, y + 1, 1, h - 2, edge);
    paint_fill(x + 1, y + 1, 1, 1, edge);
    paint_fill(x + w - 2, y + 1, 1, 1, edge);
    paint_fill(x + 1, y + h - 2, 1, 1, edge);
    paint_fill(x + w - 2, y + h - 2, 1, 1, edge);
    if (fill_h > h - 2) fill_h = h - 2;
    if (!fill_h) return;
    unsigned fx = x + 1, fy = y + h - 1 - fill_h, fw = w - 2, fh = fill_h;
    paint_fill(fx, fy, fw, fh, fill);
    if (fw < 3 || fh < 3) return;
    /* Volume: top and right edges lighter, bottom and left darker, a near-white point top right. */
    uint16_t light = shade(fill, 1, 6), dark = shade(fill, 0, 6);
    paint_fill(fx, fy + fh - 1, fw, 1, dark);
    paint_fill(fx, fy, 1, fh, dark);
    paint_fill(fx, fy, fw, 1, light);
    paint_fill(fx + fw - 1, fy, 1, fh, light);
    paint_fill(fx + fw - 2, fy + 1, 1, 1, shade(fill, 1, 9));
}
/* Running light: red inside, white glint top right, warm brown shade bottom left. */
static void lamp_head(unsigned x, unsigned y, unsigned w, unsigned h, unsigned note)
{
    paint_fill(x + 1, y + 1, w - 2, h - 2, note ? HEAD_ON : HEAD_OFF);
    if (w >= 9) {
        paint_fill(x + w - 5, y + 2, 3, 1, TEXT);
        paint_fill(x + w - 3, y + 3, 1, 2, TEXT);
        paint_fill(x + 2, y + h - 3, 3, 1, HEAD_SHADE);
        paint_fill(x + 2, y + h - 5, 1, 2, HEAD_SHADE);
    } else {
        paint_fill(x + w - 3, y + 2, 2, 1, TEXT);
        paint_fill(x + w - 2, y + 3, 1, 1, TEXT);
        paint_fill(x + 1, y + h - 3, 2, 1, HEAD_SHADE);
        paint_fill(x + 1, y + h - 4, 1, 1, HEAD_SHADE);
    }
}
/* v6 MIXER: one row per track on a common rhythm: number | name | level meter |
 * 16 steps (velocity shading, white playhead) | M/S. Steps sit in beat groups. */
/* v11 ENGINES: turning PRESETS on the mixer shows the engines (the track's
 * engine framed, changing in place as you turn); HOME closes it. */
static uint32_t engines_until;
static unsigned engines_shown(void) { return engines_until != 0; } /* v11: stays until HOME */
/* PRESETS on the mixer: the engine chosen, not built yet (engine_settle), 0xff = none. */
static uint8_t eng_pending = 0xff, eng_track;
static uint32_t eng_at;
/* v13 ENGINES: tile 7 is BINAURAL (an id past the engines); the tiles in drawing order, PRESETS steps through them. */
enum { ENG_BINAURAL = 7 };
static const uint8_t eng_order[8] = {ENGINE_MICRO, GROOVE_DX7, GROOVE_303, GROOVE_808, GROOVE_PSG, GROOVE_PLUCK, GROOVE_SAMPLE, ENG_BINAURAL};
/* A change that crosses drums <-> pitched clears the pattern: 1 while the dialog waits (SAVE = OK, HOME = CANCEL).
 * eng_confirm lets engine_settle() apply that one change. */
static uint8_t eng_warn, eng_confirm, bin_from_eng;
static unsigned eng_is_drum(unsigned id) { return id == GROOVE_808 || id == GROOVE_SAMPLE; }
/* Tiles PRESETS may stop on: MICRO D only while a slot is free (or this track already has it), BINAURAL only when built. */
static unsigned eng_tile_ok(unsigned tile)
{
    unsigned id = eng_order[tile];
    if (id == ENG_BINAURAL) return FM1_BINAURAL;
    if (id == ENGINE_MICRO) return FM1_MICRO_D && (micro_d_track(track) || micro_d_count() < MD_SLOTS);
    return 1;
}
/* PRESETS: `delta` detents from engine `cur` through the tiles in drawing order, both ways, skipping tiles that are not available. */
static unsigned eng_step(unsigned cur, int delta)
{
    unsigned tile = 0;
    for (unsigned i = 0; i < 8; ++i) if (eng_order[i] == cur) tile = i;
    unsigned n = (unsigned)(delta < 0 ? -delta : delta), dir = delta < 0 ? 7U : 1U;
    if (n > 8) n = 8;
    for (; n; --n)
        for (unsigned guard = 8; guard; --guard) {
            tile = (tile + dir) % 8U;
            if (eng_tile_ok(tile)) break;
        }
    return eng_order[tile];
}
static const char *eng_name(unsigned id)
{
    static const char *const names[8] = {"DX7", "303", "808", "PSG", "PLUCK", "SAMPLER", "MICRO D", "BINAURAL"};
    return names[id < 8 ? id : 0];
}
static void engines_draw(const struct groove_view *v)
{
    /* v13: 4 x 2 flat tiles 52 x 56, MICRO D first, BINAURAL last; the glass cursor on the chosen one */
    unsigned cur = eng_pending != 0xff && eng_track == track ? eng_pending : micro_d_track(track) ? (unsigned)ENGINE_MICRO : v->type,
             n = FM1_MICRO_D ? ENGINES : GROOVE_SOURCES;
    if (paint_rendering && (paint_top >= 196 || paint_top + PAINT_ROWS <= 43)) return;
    if (cache->middle_cache == 0xe0000U + cur + ((unsigned)eng_warn << 8)) return;
    cache->middle_cache = 0xe0000U + cur + ((unsigned)eng_warn << 8);
    paint_fill(0, 43, 240, 153, BACK);
    for (unsigned i = 0; i < 8; ++i) {
        unsigned id = eng_order[i], x = 4 + (i % 4U) * 60, y = 70 + (i / 4U) * 60, on = id == cur; /* each tile centred over its knob (centre 30 + 60 col) */
        uint16_t ec = id < ENGINES ? engine_colors[id] : 0xA2DF;
        if (id < 7 && id >= n) { rbox(x, y, 52, 56, 2, V_NONE, 0x2945); continue; }
        rbox(x, y, 52, 56, 2, MD_PANEL, MD_EDGE);
        if (id == ENG_BINAURAL) { /* two waves, a little apart: left and right ear */
            for (unsigned w = 0; w < 2; ++w) {
                int py = 0;
                for (int k = 0; k <= 34; ++k) {
                    int yy = (int)y + 14 + (int)w * 11 - (int)(vsin((float)k / 34.0f * 1.5f - (float)(int)((float)k / 34.0f * 1.5f) + (w ? 0.1f : 0.0f)) * 5.0f);
                    if (k) line((int)x + 8 + k - 1, py, (int)x + 8 + k, yy, w ? knob_colors[3] : knob_colors[0]);
                    py = yy;
                }
            }
        } else engine_icon(x + 11, y + 8, (int)id, ec);
        const char *label = eng_name(id);
        lcd_center((int)x + 26, y + 36, label, lcd_width(&lcd13, label) <= 48 ? &lcd13 : &lcd11, ec, MD_PANEL);
        if (on) glass_cursor(x, y, 52, 56, 5, MD_AMBER); /* the tile is an rbox r = 2: a 5 px corner */
    }
    if (eng_warn && eng_pending != 0xff) { /* modal: the change would clear the pattern */
        unsigned from = micro_d_track(eng_track) ? (unsigned)ENGINE_MICRO : groove_source_type(eng_track);
        char head[24];
        strcpy(head, eng_name(from)); strcat(head, " > "); strcat(head, eng_name(eng_pending));
        rbox(3, 68, 234, 120, 3, 0x0841, V_RED); /* covers the whole tile field (tiles 4..236 here, the cursor one more) */
        rbox(4, 69, 232, 118, 3, V_NONE, V_RED);
        lcd_center(120, 80, head, &lcd24, MD_AMBER, 0x0841);
        lcd_center(120, 108, eng_is_drum(eng_pending) ? "MELODIC TO DRUM" : "DRUM TO MELODIC", &lcd20, TEXT, 0x0841);
        lcd_center(120, 131, "PATTERN WILL BE CLEARED", &lcd17, TEXT, 0x0841);
        lcd_center(58, 160, "SAVE = OK", &lcd17, 0x3f0f, 0x0841);
        lcd_center(170, 160, "HOME = CANCEL", &lcd17, V_RED, 0x0841);
    }
}
enum { MIX_RED = 0xf800, MIX_GREY = 0x6b2e }; /* mockup #ff0000 (saturated), #6b6674 */
static __attribute__((noinline)) void tracks_draw(const struct groove_view v[8])
{
    if (engines_shown()) { engines_draw(&v[track]); return; }
    /* v13 MIXER: all eight tracks, columns 1-4 | 5-8, no frame. Per cell: a glass activity bar, the number
     * box and the name in the engine's colour, a glass volume bar, the state letter (S / M / L / R); the
     * selected cell has the glass cursor. */
    if (!(paint_rendering && (paint_top >= 62 || paint_top + PAINT_ROWS <= 26))) {
        const struct groove_view *sv = &v[track];
        uint32_t key = sv->next_length | ((unsigned)chain_on << 16) | (1U << 30) | ((unsigned)(sv->next_length != sv->sequence_length) << 17);
        if (key != cache->middle_cache) {
            cache->middle_cache = key;
            char num[12]; decimal(num, (int)sv->next_length, 1);
            paint_fill(168, 26, 72, 36, BACK);
            state_plate(num, chain_on != 0, sv->next_length != sv->sequence_length ? V_ORANGE : TEXT);
        }
    }
    if (paint_rendering && (paint_top >= 191 || paint_top + PAINT_ROWS <= 65)) return;
    const unsigned beat_on = v[track].playing && (v[track].play_step & 3U) < 2U;
    for (unsigned t = 0; t < 8; ++t) {
        /* v13: columns 1-4 | 5-8 on the FX window's rect (6, 66, 228 x 124): 108 x 28 cells, 4 px apart */
        unsigned x = 6 + (t >> 2) * 120, y = 66 + (t & 3U) * 32;
        if (paint_rendering && (y + 29 <= paint_top || y - 1 >= paint_top + PAINT_ROWS)) continue;
        unsigned selected = t == track, eng = micro_d_track(t) ? (unsigned)ENGINE_MICRO : v[t].type;
        uint16_t ec = eng < ENGINES ? engine_colors[eng] : TEXT;
        /* user 13.11 (mockup FM1-fx-cleanup s.5, variant A): muted or volume-0 track = whole cell grey, no blink;
         * otherwise the number boxes light all together on every beat while the transport plays (user 13.11, the
         * same beat as the PLAY LED: first half of each quarter note); stopped = dark. Only those squares repaint. */
        unsigned grey = v[t].muted || !v[t].volume;
        unsigned lit = !grey && beat_on;
        uint16_t ic = grey ? MIX_GREY : ec;
        /* user 13.11: each element has its own signature; the check pass marks only the changed element's rectangle
         * (a note repaints the 22 x 22 square, a volume turn the bar). The band pass (cache reset) draws, in the
         * usual order, only the layers that reach the band's rectangle, so overlaps (panel, cursor) stay exact. */
        static const uint8_t er[5][4] = { /* x, y offsets from the cell, w, h (the glyph boxes included) */
            {3, 3, 22, 24}, {28, 1, 62, 19}, {28, 20, 52, 7}, {89, 3, 17, 24}, {0, 0, 0, 0}};
        const char *st = v[t].muted ? "M" : v[t].solo ? "S" : track_pan[t] ? (track_pan[t] == 1 ? "R" : "L") : 0;
        if (!paint_rendering) {
            uint32_t es[5] = {
                (lit | grey << 1 | eng << 2) + 1U,
                signature(v[t].type | (v[t].program << 8) | ((unsigned)sounds_id(t) << 16) | (eng << 24),
                          (const uint8_t[2]){(uint8_t)grey, (uint8_t)v[t].audible}, 2),
                (v[t].volume | grey << 8 | eng << 9) + 1U,
                (v[t].muted | v[t].solo << 1 | (unsigned)track_pan[t] << 2) + 1U,
                selected + 1U};
            for (unsigned k = 0; k < 5; ++k) {
                if (es[k] == cache->mix_cache[t][k]) continue;
                cache->mix_cache[t][k] = es[k];
                if (k == 4) paint_mark(x - 1, y - 1, 110, 30); /* the cursor's outline reaches 1 px outside the cell */
                else paint_mark(x + er[k][0], y + er[k][1], er[k][2], er[k][3]);
            }
            continue;
        }
#define MIX_HIT(X, Y, W, H) ((X) < paint_end && (X) + (W) > paint_x && (Y) < paint_top + PAINT_ROWS && (Y) + (H) > paint_top)
        if (!MIX_HIT(x - 1, y - 1, 110U, 30U)) continue;
        paint_fill(x - 1, y - 1, 110, 30, BACK);
        rbox(x, y, 108, 28, 2, MD_PANEL, MD_EDGE);
        if (MIX_HIT(x + 3, y + 3, 22U, 24U)) {
            /* the number box: square 22 x 22 at x+3, y+3; idle = 2 px frame + digit in the engine colour, sounding = filled */
            rbox(x + 3, y + 3, 22, 22, 2, ic, V_NONE);
            if (!lit) rbox(x + 5, y + 5, 18, 18, 1, MD_PANEL, V_NONE);
            char num[2] = {(char)('1' + t), 0};
            lcd_center((int)x + 14, y + 5, num, &lcd20, lit ? BACK : ic, lit ? ic : MD_PANEL);
        }
        if (MIX_HIT(x + 28, y + 1, 62U, 19U)) {
            char sound[10], name[16]; track_sound_name(&v[t], t, sound);
            ++keep_names; /* sound names stay as they are */
            fit(sound, name, &lcd16, 58);
            lcd_text((int)x + 29, y + 2, name, &lcd16, grey ? V_DIM : !v[t].audible ? V_FAINT : ec, MD_PANEL);
            --keep_names;
        }
        if (MIX_HIT(x + 28, y + 20, 52U, 7U))
            glass_bar_h(x + 29, y + 21, 50, 5, (unsigned)v[t].volume * 256U / 127U, ic); /* volume */
        /* letters in the K4 STATE colours: S yellow, M red, L / R teal */
        if (st && MIX_HIT(x + 89, y + 3, 17U, 24U))
            lcd_center((int)x + 97, y + 5, st, &lcd20, v[t].muted ? MIX_RED : v[t].solo ? PAL_YELLOW : PAL_TURQ, MD_PANEL);
#undef MIX_HIT
        if (selected) glass_cursor(x, y, 108, 28, 5, MIX_RED); /* user 13.11: red cursor; radius 5 = the cell's corner */
    }
}
static void dx7_brightness(unsigned t, int delta);
static int dx7_brightness_value(const uint8_t patch[156]);
static void dx7_algorithm_edit(unsigned t, int delta);
/* Mixer KNOB1: the track's filter. 303 cutoff, pluck brightness, DX7
 * brightness (modulator output levels). Other engines have no filter. */
static void vol_edit(unsigned t, int delta)
{
    if (micro_d_track(t)) micro_d_volume_edit(t, delta);
    else groove_control_edit(t, 0, delta, 0);
}
static void track_filter_edit(unsigned t, int delta)
{
    if (micro_d_track(t)) { micro_d_edit(t, MD_CUTOFF, delta); return; }
    unsigned type = groove_source_type(t);
    if (type == GROOVE_303) groove_tone_edit(t, 0, 0, delta);
    else if (type == GROOVE_PLUCK) groove_tone_edit(t, 0, 1, delta);
    else if (type == GROOVE_DX7) dx7_brightness(t, delta);
    else if (type == GROOVE_808) { /* user 09.11: TONE of the kick and the snare together (part 0 BD, 1 SD; param 1) */
        groove_tone_edit(t, 0, 1, delta);
        groove_tone_edit(t, 1, 1, delta);
    } /* PSG and SAMPLER have no tone control: "--" (user 09.11) */
}
/* v11 KNOB4 on the track page (OVERVIEW; v12: endless ring, see mix_cycle; the mixer has its own, mixer_state_cycle).
 * R / L put the track hard right / left (pan CC 10 on its channel, sent
 * from the control tick); kept with the project. */
static int mix_state(unsigned t)
{
    struct groove_view mv = {0};
    groove_get_view(t, 0, &mv);
    return mv.muted ? 4 : mv.solo ? 1 : track_pan[t] == 1 ? 2 : track_pan[t] == 2 ? 3 : 0;
}
static void mix_cycle(unsigned t, int delta)
{
    struct groove_view mv = {0};
    groove_get_view(t, 0, &mv);
    /* v12: an endless ring ON > SOLO > MUTE > LEFT > RIGHT > ON; right from ON is S then M, left is R then L */
    static const uint8_t ring[5] = {0, 1, 4, 3, 2};
    int state = mix_state(t), at = 0;
    for (int i = 0; i < 5; ++i) if (ring[i] == state) at = i;
    int next = ring[(at + (delta > 0 ? 1 : 4)) % 5];
    if (next == state) return;
    if (mv.muted) groove_mix_toggle(t, 0);
    if (mv.solo) groove_mix_toggle(t, 1);
    if (next == 1) groove_mix_toggle(t, 1);
    if (next == 4) groove_mix_toggle(t, 0);
    unsigned pan = next == 2 ? 1U : next == 3 ? 2U : 0U;
    if (pan != track_pan[t]) { track_pan[t] = (uint8_t)pan; pan_value[t] = pan == 1 ? 127 : pan == 2 ? 0 : 64; pan_dirty |= (uint8_t)(1U << t); }
    /* S / M live in the project, pan in its extras: both kept by autosave. */
    groove_project_touch();
    autosave_at = fm1_millis() + 3000U; if (!autosave_at) autosave_at = 1;
}
/* MIXER KNOB3: smooth pan again (user 10.11: pan works on the device), 0..127 with 64 = centre, sent as
 * CC 10 (the instrument pans the track; MICRO D pans its own output by the same law) and kept in the
 * extras (pan_value); track_pan keeps the side (R / L letter in the cell, the 2-bit side older firmware reads). */
static void pan_edit(unsigned t, int delta)
{
    int v = clamp((int)pan_value[t] + delta, 0, 127);
    if (v == (int)pan_value[t]) return;
    pan_value[t] = (uint8_t)v;
    track_pan[t] = (uint8_t)(v > 64 ? 1 : v < 64 ? 2 : 0);
    pan_dirty |= (uint8_t)(1U << t);
    groove_project_touch();
    autosave_at = fm1_millis() + 3000U; if (!autosave_at) autosave_at = 1;
}
static void pan_text(unsigned t, char out[12])
{
    int v = pan_value[t];
    if (v == 64) { strcpy(out, "C"); return; }
    out[0] = v < 64 ? 'L' : 'R';
    decimal(out + 1, v < 64 ? (64 - v) * 100 / 64 : (v - 64) * 100 / 63, 1);
}
/* v13 MIXER KNOB4: an endless ring ON > SOLO > MUTE > ON (right), left the other way. The pan has KNOB3 now. */
static const char *const mixer_state_names[3] = {"ON", "SOLO", "MUTE"};
static unsigned mixer_state(unsigned t)
{
    struct groove_view mv = {0};
    groove_get_view(t, 0, &mv);
    return mv.muted ? 2U : mv.solo ? 1U : 0U;
}
static void mixer_state_cycle(unsigned t, int delta)
{
    struct groove_view mv = {0};
    groove_get_view(t, 0, &mv);
    unsigned state = mixer_state(t), next = (state + (delta > 0 ? 1U : 2U)) % 3U;
    if (mv.muted) groove_mix_toggle(t, 0);
    if (mv.solo) groove_mix_toggle(t, 1);
    if (next == 1) groove_mix_toggle(t, 1);
    if (next == 2) groove_mix_toggle(t, 0);
    groove_project_touch();
    autosave_at = fm1_millis() + 3000U; if (!autosave_at) autosave_at = 1;
}
/* SAMPLER grid KNOB 3: the pad's LENGTH locked on one step (lock parameter
 * pad * 6 + 2). 255 = no LENGTH lock for this pad on that step. */
static unsigned step_length_lock(unsigned step)
{
    unsigned want = drum * 6U + 2U + 1U; /* setting 5 holds parameter + 1 */
    return groove_step_setting(track, step, 5) == want ? groove_step_setting(track, step, 6) : 255U;
}
static int step_length_edit(unsigned step, int delta)
{
    unsigned want = drum * 6U + 2U + 1U, cur = groove_step_setting(track, step, 5);
    if (cur != want) {
        if (cur) return -2; /* this step already locks another parameter */
        if (groove_step_setting_edit(track, step, 5, (int)want)) return -1; /* all 16 locks in use */
        uint8_t tv[GROOVE_TONE_PARAMS];
        unsigned base = groove_tone_copy(track, drum, tv) ? 127U : tv[2];
        (void)groove_step_setting_edit(track, step, 6, (int)base - (int)groove_step_setting(track, step, 6));
    }
    return groove_step_setting_edit(track, step, 6, delta);
}
static uint8_t fill_have, fill_track, fill_cursor, fill_space = 2, fill_repeat, fill_marked; /* C2 fill state */
/* SEQ page 1 STEP knobs: K1 CHORD; C2: K2 RATCH (the step's split), K3 SPACE, K4 REPEAT (the fill); "--" while the
 * cursor step is empty (drums: the chosen pad not set there). */
static unsigned step_lit_cursor(void);
static unsigned fill_active(void);
static void step_knob(unsigned col, unsigned bank)
{
    static const char *const labels[] = {"CHORD", "RATCH", "SPACE", "REPEAT"}, *const splits[4] = {"OFF", "2X", "3X", "4X"};
    (void)bank;
    char num[12];
    const char *value = "--";
    if (!col) { if (!groove_pad_count(track)) value = groove_sequence_chord_name(groove_step_setting(track, cursor, 3)); }
    else if (step_lit_cursor()) {
        if (col == 1) value = splits[var_split_get(track, cursor)];
        else { decimal(num, col == 2 ? fill_space : fill_active() ? fill_repeat : 0, 1); value = num; }
    }
    knob(col, labels[col & 3U], value, colors[col]);
}
static void break_long_note(unsigned t, unsigned step);
/* A pitch edited on a step inside a long note's tail is a new note there: the long note before it ends here. */
static void cut_tail_before_note(unsigned t, unsigned step)
{
    unsigned len = groove_sequence_length(t);
    if (!len || step >= len) return;
    unsigned prev = (step + len - 1U) % len;
    struct groove_view pv = {0}; groove_get_view(t, prev, &pv);
    if (pv.step.velocity && groove_step_setting(t, prev, 4)) break_long_note(t, step);
}
/* SEQ page 1 STEP: K1 CHORD; C2: K2 RATCH (split of the step), K3 SPACE, K4 REPEAT (fill). The TIE / LOCK / VALUE
 * editors are gone (TIE lives in the grid's K2 LENGTH; locks already in a project keep playing). */
static void split_turn(int delta, uint32_t now);
static void fill_turn(unsigned knob, int delta, uint32_t now);
static void step_adjust(unsigned bank, const int16_t *encoders, uint32_t now)
{
    (void)bank;
    if (encoders[2] && groove_step_setting_edit(track, cursor, 3, encoders[2])) {
        notice = "CHORD/LOCK FULL"; notice_until = now + 2000U;
    }
    if (encoders[3]) split_turn(encoders[3], now);   /* K2 RATCH (C2) */
    if (encoders[4]) fill_turn(2, encoders[4], now); /* K3 SPACE */
    if (encoders[5]) fill_turn(3, encoders[5], now); /* K4 REPEAT */
}
/* SEQ rework C2 (mockup a-d). The step logic is app/seq_steps.c (host test); here only reads, one undo checkpoint
 * per spin or per fill, and the write back. Every rewrite first puts this track's MUTATE / HUMAN original back
 * (var_restore), so a loop's variation is never baked into the pattern. */
static struct groove_step c2_cells[GROOVE_STEPS], fill_snap[GROOVE_STEPS], fill_out[GROOVE_STEPS];
static uint64_t fill_placed;
static int8_t fill_pad;
static uint32_t length_turn_ms, transpose_turn_ms;
static int transpose_shown;
static unsigned transpose_track = ~0U;
void groove_midi_follow(unsigned track, int shift); /* midi_router.c */
static unsigned c2_read(void)
{
    var_restore(track);
    groove_sequence_read(track, 0, c2_cells, GROOVE_STEPS);
    return groove_sequence_length(track);
}
/* One undo per spin: a checkpoint only when this knob rested more than a second. */
static void c2_checkpoint(unsigned op, uint32_t *last, uint32_t now)
{
    if (!*last || now - *last > 1000U) groove_edit_checkpoint(op);
    *last = now | 1U;
}
/* Grid K2 LENGTH: 0..13 gate 1/16..14/16, 14 full step, then one tied tail step per click (stops before a lit step). */
static void seq_length_turn(int delta, uint32_t now)
{
    unsigned len = c2_read(), pos = seq_len_pos(c2_cells, len, cursor);
    int want = clamp((int)pos + delta, 0, SEQ_LEN_FULL + SEQ_LEN_MAX_TAILS);
    if ((unsigned)want == pos) return;
    unsigned got = seq_len_set(c2_cells, len, cursor, (unsigned)want);
    if (got == pos) return;
    c2_checkpoint(0xc2100U + track, &length_turn_ms, now);
    groove_sequence_replace(track, 0, c2_cells, GROOVE_STEPS);
}
/* Page 1: the cursor step is lit (melodic: a note; drums: the chosen pad). */
static unsigned step_lit_cursor(void) { return seq_lit(&frame_cells[cursor % GROOVE_STEPS], groove_pad_count(track) ? (int)drum : -1); }
static unsigned fill_active(void) { return fill_have && fill_track == track && fill_cursor == cursor && fill_pad == (groove_pad_count(track) ? (int)drum : -1); }
/* Page 1 K3 SPACE / K4 REPEAT: copies of the cursor step written as normal steps from a RAM snapshot of the track
 * taken at the first turn, re-applied on every turn (turning down removes copies). The snapshot goes when the cursor,
 * track or pad changes, or when the track no longer holds what the fill wrote (edited elsewhere). One UNDO per fill. */
static void fill_turn(unsigned knob, int delta, uint32_t now)
{
    int pad = groove_pad_count(track) ? (int)drum : -1;
    unsigned len = c2_read();
    (void)now;
    if (groove_track_role(track) != GROOVE_TRACK_NORMAL) return; /* the engine ignores rewrites of PLAY / OFF tracks */
    if (!seq_lit(&c2_cells[cursor], pad)) { notice = "EMPTY STEP"; notice_until = now + 2000U; return; }
    if (fill_active()) {
        (void)seq_fill(fill_snap, fill_out, len, cursor, fill_space, fill_repeat, pad);
        if (memcmp(fill_out, c2_cells, sizeof c2_cells)) fill_have = 0;
    } else fill_have = 0;
    if (!fill_have) {
        memcpy(fill_snap, c2_cells, sizeof c2_cells);
        fill_have = 1; fill_track = (uint8_t)track; fill_cursor = (uint8_t)cursor; fill_pad = (int8_t)pad;
        fill_repeat = 0; fill_placed = 0; fill_marked = 0;
    }
    int top = len > 1 ? (int)len - 1 : 1;
    if (knob == 2) fill_space = (uint8_t)clamp((int)fill_space + delta, 1, top);
    else fill_repeat = (uint8_t)clamp((int)fill_repeat + delta, 0, top);
    uint64_t was = fill_placed;
    fill_placed = seq_fill(fill_snap, fill_out, len, cursor, fill_space, fill_repeat, pad);
    var_split_fill(track, was, fill_placed, var_split_get(track, cursor)); /* the copies carry the split (RAM table) */
    if (!memcmp(fill_out, c2_cells, sizeof c2_cells)) return;
    if (!fill_marked) { groove_edit_checkpoint(0xc2200U + track); fill_marked = 1; } /* UNDO = the track before this fill */
    groove_sequence_replace(track, 0, fill_out, GROOVE_STEPS);
}
/* Page 1 K2 RATCH: the split of the cursor step (OFF / 2X / 3X / 4X), RAM only (variation.c table, decision A: never in
 * the cells), so no UNDO and lost on save / reboot / load; during a fill the copies take it too. */
static void split_turn(int delta, uint32_t now)
{
    if (!step_lit_cursor()) { notice = "EMPTY STEP"; notice_until = now + 2000U; return; }
    unsigned was = var_split_get(track, cursor), next = (unsigned)clamp((int)was + (delta > 0 ? 1 : -1), 0, 3);
    if (next == was) return;
    var_split_set(track, cursor, next);
    if (fill_active()) var_split_fill(track, fill_placed, fill_placed, next);
}
/* Page 2 K3 TRANSPOSE: melodic tracks with role NORMAL; the whole track moves, refused (not clamped) at the ends. */
static unsigned transpose_usable(void) { return !groove_pad_count(track) && groove_track_role(track) == GROOVE_TRACK_NORMAL; }
static void transpose_turn(int delta, uint32_t now)
{
    if (!transpose_usable() || !delta) return;
    struct groove_sequence_options o; groove_sequence_options_copy(&o);
    (void)c2_read();
    if (seq_transpose(c2_cells, GROOVE_STEPS, delta, &o)) { notice = "OUT OF RANGE"; notice_until = now + 2000U; return; }
    c2_checkpoint(0xc2400U + track, &transpose_turn_ms, now);
    groove_sequence_replace(track, 0, c2_cells, GROOVE_STEPS);
    transpose_shown += delta;
}
/* Page 3 K3 FOLLOW: only MICRO D has a live form (the groove_midi_note hook). DX7 / PSG / PLUCK could move only
 * through their sound parameter, which always takes an UNDO step and marks the project changed: "--" there,
 * as on 303 and drums. */
static unsigned follow_usable(unsigned t) { return micro_d_owns(t) != 0; }
/* Each control tick: the latest held (or latched) key gives the shift; FOLLOW tracks that play take it. */
static void follow_update(void)
{
    unsigned found = 0, best = 0, note = 0;
    for (unsigned k = 0; k < 27; ++k) {
        if (key_note[k] == 0xff) continue;
        unsigned sq = (unsigned)(uint8_t)(key_seq[k] - (uint8_t)key_counter - 1U); /* 255 = the latest press */
        if (!found || sq >= best) { best = sq; note = key_note[k]; found = 1; }
    }
    int shift = found ? seq_follow_shift(note) : 0;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        groove_midi_follow(t, var_follow(t) && follow_usable(t) && frame_views[t].playing ? shift : 0);
}
static void sequence_length_adjust(int delta)
{
    if (!delta) return;
    struct groove_view v = {0}; groove_get_view(track, cursor, &v);
    unsigned length = (unsigned)clamp((int)(v.next_length / 16) + delta, 1, 4) * 16U;
    groove_sequence_length_set(track, length);
    cursor %= groove_sequence_length(track);
}
/* v6 SEQUENCER: CHAIN + ON/OFF lamp centred, 16 pattern squares, then all 64
 * steps as four rows of 16 in beat groups. Notes glow (velocity = brightness),
 * a red frame runs over the playing step, the cursor is a white frame. */
/* Drum layers: every pad/instrument has its own colour for active steps. */
static const uint16_t pad_colors[12] = {0xf9a6, 0xfec0, 0x3f2a, 0x3cdf, 0xc99f, 0x07ff, 0xfd20, 0xf81f,
    0x9fe0, 0xfbcf, 0x5d9f, 0xce59};
enum { SEQ_X = 5, SEQ_Y = 93, SEQ_W = 13, SEQ_ROW = 25, SEQ_H = 22 }; /* v12: corner to corner x 5..235, rows end at 190 */
enum { SEQ_EMERALD = 0x564f }; /* v13: lit melodic steps, #50c878 */
static uint16_t mix565(uint16_t bg, uint16_t fg, unsigned a);
/* v7 cursors, the same everywhere. Playing: red, two pixels thick inward.
 * Selected: white, pulsing, each inner ring fainter (a gradient to the centre).
 * `under` is the colour inside the box; pulse_level() steps 10 times a second. */
static unsigned pulse_level(void)
{
    unsigned t = (frame_time / 100U) % 10U;
    return 20U + (t < 5U ? t : 10U - t) * 4U; /* 20..40 of 40 */
}
static void select_cursor(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r, uint16_t under)
{
    static const uint8_t fade[4] = {40, 20, 10, 4};
    unsigned p = pulse_level();
    unsigned bright = ((under >> 11) & 31U) + ((under >> 5) & 63U) / 2U + (under & 31U) > 30U;
    if (bright) { /* on a filled (bright) cell: white pulsing frame, a black line inside to set it off */
        rbox(x, y, w, h, r, V_NONE, mix565(0x4208, TEXT, p));
        frame(x + 1, y + 1, w - 2, h - 2, BACK);
        return;
    }
    rbox(x, y, w, h, r, V_NONE, mix565(under, TEXT, p));
    for (unsigned k = 1; k < 4 && w > 2 * k + 1 && h > 2 * k + 1; ++k)
        frame(x + k, y + k, w - 2 * k, h - 2 * k, mix565(under, TEXT, p * fade[k] / 40U));
}
static unsigned seq_cell_x(unsigned i) { return SEQ_X + (i * 115U + 4U) / 8U; } /* pitch 14.375 */
static unsigned seq_cell_w(unsigned i) { return seq_cell_x(i + 1U) - seq_cell_x(i) - 3U; } /* v13: 3 px gaps, 11.4 px steps */
/* SEQ rework C1: one cell drawing for the grid (steps()) and the page 1 STEP strip (seq_strip()): velocity shade,
 * pad colours, the long note's comet tail, other drum layers, the green play head and the glass cursor. Inputs are the
 * frame copies only (frame_cells, the lanes in `l`): no engine call per cell or per band. `slot` caches the signature;
 * the cell is painted (and its damage marked) only when that changes. `bg` = what lies around the cell. */
struct seq_lanes { uint64_t lane, others, ghost; uint16_t layer; unsigned pads, length, play, marks; }; /* ghost, marks: page 1 only (C2) */
static void seq_play_cursor(unsigned x, unsigned y, unsigned w, unsigned h);
static void seq_lanes_get(const struct groove_view *v, struct seq_lanes *l)
{
    l->pads = groove_pad_count(track) != 0;
    l->lane = l->pads ? groove_drum_steps(track, drum) : v->active_steps;
    l->others = 0;
    if (l->pads) for (unsigned p = 0; p < groove_pad_count(track); ++p) if (p != drum) l->others |= groove_drum_steps(track, p);
    l->layer = l->pads ? pad_colors[drum % 12] : SEQ_EMERALD; /* v13: melodic steps emerald; drum layers keep their colours */
    l->length = v->next_length;
    l->play = v->playing ? v->play_step : ~0U;
    l->ghost = 0; l->marks = 0;
}
static void seq_cell(unsigned step, unsigned x, unsigned y, unsigned cw, const struct seq_lanes *l, uint16_t cursor_col, uint16_t bg, uint32_t *slot)
{
    unsigned i = step % 16, pads = l->pads;
    uint16_t layer = l->layer;
    unsigned used = step < l->length;
    unsigned on = (l->lane >> step) & 1U;
    unsigned head = l->play == step;
    unsigned vel = pads ? 127U : frame_cells[step].velocity & 127U;
    if (on && !vel) vel = 100;
    unsigned other = used && !on && ((l->others >> step) & 1U);
    /* Long notes: a tied step starts a tail; a later step with the same
     * note right after a tied one is its continuation (drawn as the tail). */
    struct groove_step cs = frame_cells[step];
    unsigned tied = !pads && on && (cs.drums >> 12) & 1U, cont = 0, age = 0;
    for (unsigned k = step; !pads && on && k > 0; --k) {
        struct groove_step pv = frame_cells[k - 1], cur = frame_cells[k];
        if (!pv.velocity || !((pv.drums >> 12) & 1U) || pv.note != cur.note) break;
        cont = 1; ++age;
    }
    unsigned thick = (cs.drums & 31U) ? 5U : 3U;
    /* C2, page 1: the current fill's copies dashed (mockup b), a split drawn as marks inside the step */
    unsigned ghost = on && ((l->ghost >> step) & 1U), split = on && l->marks ? var_split_get(track, step) : 0U;
    uint32_t sig = 1U | (used << 1) | (on << 2) | (head << 3) | ((step == cursor) << 4) |
                   (vel << 5) | (track << 12) | (drum << 15) | (other << 20) | (split << 21) | (ghost << 23) |
                   (tied << 26) | (cont << 27) | ((age > 6 ? 6U : age) << 28);
    if (sig == *slot) return;
    *slot = sig;
    if (!paint_rendering) paint_mark(x > 0 ? x - 1 : 0, y - 1, cw + 2, SEQ_H + 2); /* the glass cursor's outline lies 1 px outside the cell */
    paint_fill(x - 1, y - 1, cw + 2, SEQ_H + 2, bg); /* includes the glass outline and the tail's one-pixel bridge */
    /* Beyond the chosen length the steps stay, but nearly transparent. */
    unsigned fade = used ? 0U : 8U, strong = used && i % 4 == 0; /* beyond the length the steps stay faint */
    uint16_t inside = on ? shade(layer, 0, used ? (127U - vel) * 6U / 127U : 8U) : shade(0x18a3, 0, fade);
    if (ghost) { /* a copy of this fill: dim cell, dashed frame in the layer colour */
        rbox(x, y, cw, SEQ_H, 1, shade(layer, 0, 7), V_NONE);
        for (unsigned d = 0; d < cw; d += 4) { paint_fill(x + d, y, d + 2 <= cw ? 2 : 1, 1, layer); paint_fill(x + d, y + SEQ_H - 1, d + 2 <= cw ? 2 : 1, 1, layer); }
        for (unsigned d = 0; d < SEQ_H; d += 4) { paint_fill(x, y + d, 1, 2, layer); paint_fill(x + cw - 1, y + d, 1, 2, layer); }
    } else if (on && cont) { /* the comet's tail: a dark cell, the line through it, fading */
        rbox(x, y, cw, SEQ_H, 1, shade(layer, 0, 8), V_NONE);
        uint16_t tail = shade(layer, 0, age > 6 ? 6U : age);
        paint_fill(x - 1, y + SEQ_H / 2 - thick / 2, cw + 1 + (tied ? 1U : 0U), thick, tail);
    } else if (on) {
        /* user 13.11 (mockup s.10): strong beats 1, 5, 9, 13 lit lighter (#7ae8a2 for emerald) with a white 1 px frame */
        if (strong) rbox(x, y, cw, SEQ_H, 1, inside == SEQ_EMERALD ? 0x7f54 : shade(inside, 1, 3), 0xffff);
        else rbox(x, y, cw, SEQ_H, 1, inside, V_NONE);
        if (tied) paint_fill(x + cw / 2, y + SEQ_H / 2 - thick / 2, cw / 2 + 1, thick, layer);
    }
    else if (strong) rbox(x, y, cw, SEQ_H, 1, 0x2943, 0xde53); /* empty strong beat: lighter frame #d9c99a, fill #2e2819 */
    else rbox(x, y, cw, SEQ_H, 1, inside, shade(i % 4 == 0 ? 0x6b0a : 0x4a26, 0, fade));
    if (other) paint_fill(x + 3, y + SEQ_H - 5, cw - 6, 3, DIM); /* other drum layers */
    for (unsigned k = 0, n = split + 1U; split && k < n; ++k) { /* the split: n hits, dark marks along the bottom */
        unsigned a = x + 2 + k * (cw - 4) / n, b = x + 2 + (k + 1) * (cw - 4) / n;
        if (b > a + 1) paint_fill(a, y + SEQ_H - 8, b - a - 1, 6, 0x0942);
    }
    if (head) seq_play_cursor(x, y, cw, SEQ_H);
    if (step == cursor) glass_cursor_a(x, y, cw, SEQ_H, 3, cursor_col, 0x8c, 0x70); /* glass, the size of a step */
}
/* SEQ page 1 STEP: the 16 steps of the cursor's row (y 100..122) drawn with the grid cell, amber cursor (mockup b).
 * The lanes are read on the damage pass only and kept for the bands (no engine call per band); the cells keep their
 * own signatures, so while the window stands still only the play head's cells repaint. */
enum { STRIP_Y = 100 };
static void seq_strip(const struct groove_view *v)
{
    static struct seq_lanes l;
    if (paint_rendering && (paint_top >= STRIP_Y + SEQ_H + 1 || paint_top + PAINT_ROWS <= STRIP_Y - 1)) return;
    if (!paint_rendering) { seq_lanes_get(v, &l); l.ghost = fill_active() ? fill_placed : 0; l.marks = 1; }
    unsigned base = cursor & ~15U;
    for (unsigned i = 0; i < 16; ++i)
        seq_cell(base + i, seq_cell_x(i), STRIP_Y, seq_cell_w(i), &l, MD_AMBER, MD_PANEL, &cache->cell_cache[4][i]);
}
/* v13 playback cursor: pure green 2 px frame, a fainter glow ring just inside it. */
static void seq_play_cursor(unsigned x, unsigned y, unsigned w, unsigned h)
{
    enum { PLAY_GREEN = 0x07e7 };
    frame(x - 1, y - 1, w + 2, h + 2, mix565(BACK, PLAY_GREEN, 10)); /* glow outside: the gap pixels belong to this cell's redraw */
    rbox(x, y, w, h, 1, V_NONE, PLAY_GREEN);
    frame(x + 1, y + 1, w - 2, h - 2, PLAY_GREEN);
    if (w > 6 && h > 6) frame(x + 2, y + 2, w - 4, h - 4, mix565(BACK, PLAY_GREEN, 14));
}
/* v12 state plate (x 200, 35x34): big number on top, optional chain below (grey off, yellow on). */
static void state_plate(const char *big, int chain, uint16_t numcolor)
{
    paint_fill(200, 27, 35, 34, BACK);
    rbox(200, 27, 35, 34, 3, 0x1081, AMBER);
    int sub = chain >= 0;
    lcd_center(217, sub ? 28U : 33U, big, &lcd22, numcolor, 0x1081);
    if (sub) {
        uint16_t cc = chain ? 0xfe87 : 0x5aeb;
        for (unsigned k = 0; k < 2; ++k) { rbox(205 + k * 10, 50, 14, 9, 2, V_NONE, cc); rbox(206 + k * 10, 51, 12, 7, 2, V_NONE, cc); }
    }
}
/* v12 plate with a caption under the number (CPU, ALGO, ON) instead of the chain. */
static void state_plate_cap(const char *big, const char *cap)
{
    paint_fill(200, 27, 35, 34, BACK);
    rbox(200, 27, 35, 34, 3, 0x1081, AMBER);
    lcd_center(217, 29, big, lcd_width(&lcd22, big) <= 31 ? &lcd22 : &lcd17, TEXT, 0x1081);
    lcd_center(217, 48, cap, &lcd11, V_DIM, 0x1081);
}
/* v12 window 6,66,228x124; its name in lcd30 at y 73 (the page's picture goes below it). */
static void window_draw(const char *title, uint16_t color)
{
    rbox(6, 66, 228, 124, 2, MD_PANEL, MD_EDGE);
    if (title && *title) lcd_center(120, 73, title, &lcd30, color, MD_PANEL);
}
/* ---- v13 SEQ pages 2/8 .. 8/8: the icon row shows where you are (SELECT turns the pages), the window holds the page's picture ---- */
static void sq_line(int x0, int y0, int x1, int y1, uint16_t c) { line(x0, y0, x1, y1, c); line(x0, y0 + 1, x1, y1 + 1, c); }
static void sq_ring(int cx, int cy, int r, uint16_t c)
{
    fill_ellipse(cx * 10 + 5, cy * 10 + 5, r * 10, r * 10, c);
    fill_ellipse(cx * 10 + 5, cy * 10 + 5, (r - 2) * 10, (r - 2) * 10, 0x10a3);
}
static void sq_dot(int cx, int cy, int r10, uint16_t c) { fill_ellipse(cx * 10 + 5, cy * 10 + 5, r10, r10, c); }
static void sqi_grid(int cx, int cy, uint16_t c)
{
    for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k) {
        unsigned x = (unsigned)(cx - 8 + k * 6), y = (unsigned)(cy - 8 + r * 6);
        if ((r + k) % 2 == 0) paint_fill(x, y, 5, 5, c); else frame(x, y, 5, 5, c);
    }
}
static void sqi_step(int cx, int cy, uint16_t c)
{
    sq_dot(cx - 3, cy + 5, 34, c);
    sq_line(cx, cy + 5, cx, cy - 8, c); sq_line(cx, cy - 8, cx + 6, cy - 4, c);
}
static void sqi_rec(int cx, int cy, uint16_t c) { sq_ring(cx, cy, 8, c); sq_dot(cx, cy, 36, c); }
static void sqi_lock(int cx, int cy, uint16_t c)
{
    rbox((unsigned)(cx - 7), (unsigned)(cy - 1), 14, 10, 2, V_NONE, c); rbox((unsigned)(cx - 6), (unsigned)cy, 12, 8, 1, V_NONE, c);
    sq_line(cx - 4, cy - 2, cx - 3, cy - 5, c); sq_line(cx - 3, cy - 5, cx + 3, cy - 5, c); sq_line(cx + 3, cy - 5, cx + 4, cy - 2, c);
}
static void sqi_loop(int cx, int cy, uint16_t c)
{
    rbox((unsigned)(cx - 8), (unsigned)(cy - 5), 16, 10, 5, V_NONE, c); rbox((unsigned)(cx - 7), (unsigned)(cy - 4), 14, 8, 4, V_NONE, c);
    fill_tri(cx + 2, cy - 9, cx + 2, cy - 1, cx + 6, cy - 5, c);
}
static void sqi_din(int cx, int cy, uint16_t c)
{
    static const int8_t d[5][2] = {{-4, -1}, {0, -4}, {4, -1}, {-3, 3}, {3, 3}};
    sq_ring(cx, cy, 8, c);
    for (unsigned i = 0; i < 5; ++i) sq_dot(cx + d[i][0], cy + d[i][1], 13, c);
}
static void sqi_route(int cx, int cy, uint16_t c)
{
    sq_line(cx - 9, cy, cx - 2, cy, c); sq_line(cx - 2, cy, cx + 3, cy - 6, c); sq_line(cx + 3, cy - 6, cx + 9, cy - 6, c);
    sq_line(cx - 2, cy, cx + 3, cy + 6, c); sq_line(cx + 3, cy + 6, cx + 9, cy + 6, c);
}
static void sqi_role(int cx, int cy, uint16_t c)
{
    sq_line(cx - 8, cy - 4, cx + 8, cy - 4, c); sq_line(cx + 8, cy + 4, cx - 8, cy + 4, c);
    fill_tri(cx + 4, cy - 8, cx + 4, cy, cx + 9, cy - 4, c);
}
static void sq_text(int cx, int cy, const char *s, const struct lcd_font *f, uint16_t c, uint16_t bg)
{
    lcd_center(cx, (unsigned)(cy - (int)f->digit_top - (int)f->digit_height / 2), s, f, c, bg);
}
/* equal plates across x0..x1 (gap 6), the chosen one wears the glass cursor */
static void sq_chips(unsigned n, const char *const *lab, unsigned sel, unsigned y, unsigned x0, unsigned x1, unsigned h, const uint16_t *cols)
{
    unsigned w = (x1 - x0 - (n - 1) * 6) / n;
    for (unsigned i = 0; i < n; ++i) {
        unsigned x = x0 + i * (w + 6);
        rbox(x, y, w, h, 3, MD_PANEL, MD_EDGE);
        sq_text((int)(x + w / 2), (int)(y + h / 2), lab[i], lcd_width(&lcd16, lab[i]) + 8 > w ? &lcd13 : &lcd16, cols[i], MD_PANEL);
        if (i == sel) glass_cursor(x, y, w, h, 8, MD_AMBER);
    }
}
static void sq_box(unsigned x, unsigned y, unsigned w, unsigned h, const char *s, const struct lcd_font *f, uint16_t c)
{
    rbox(x, y, w, h, 3, MD_PANEL, c);
    sq_text((int)(x + w / 2), (int)(y + h / 2), s, f, c, MD_PANEL);
}
/* Round 4 chord plate (ARP and SEQ STEP, where the ARP "ON" plate was): x 174..235, the chord about as tall as the plate
 * (40 px, 32 px from 4 letters, 26 px when even that is too wide). Empty name = an empty plate. */
static void chord_plate(const char *name)
{
    paint_fill(174, 27, 61, 34, BACK);
    rbox(174, 27, 61, 34, 3, 0x1081, AMBER);
    if (!name[0]) return;
    ++keep_names; ++keep_case;
    const struct lcd_font *f = strlen(name) <= 3 ? &lcd_chord40 : &lcd_chord32;
    if (lcd_width(f, name) > 57U) f = &lcd_chord32;
    if (lcd_width(f, name) > 57U) f = &lcd_chord26;
    int cy = 44 - (f != &lcd_chord26 && (strchr(name, 'g') || strchr(name, 'j')) ? 2 : 0); /* keep g / j tails inside the plate */
    sq_text(204, cy, name, f, TEXT, 0x1081);
    --keep_case; --keep_names;
}
/* The notes of the cursor step's chord (root first); 0 when the step is empty or a drum step. */
static unsigned step_chord_notes(const struct groove_view *v, uint8_t notes[8])
{
    if (!v->step.velocity || groove_pad_count(track)) return 0;
    struct groove_sequence_options o; groove_sequence_options_copy(&o);
    unsigned id = groove_step_setting(track, cursor, 3), cnt = id && id <= GROOVE_CHORDS && o.chord[id - 1][0] ? o.chord[id - 1][0] : 1U;
    if (cnt > 8) cnt = 8;
    for (unsigned i = 0; i < cnt; ++i) { unsigned n = v->step.note + (i ? o.chord[id - 1][i] : 0U); notes[i] = (uint8_t)(n > 127 ? 127 : n); }
    return cnt;
}
static void mi_clock(int cx, int cy, uint16_t c), mi_out(int cx, int cy, uint16_t c), mi_sync(int cx, int cy, uint16_t c);
/* SEQ pages 1 STEP / 2 VARIATION / 3 TRACK (rework C1, mockup FM1-seq-rework.html b-e), and the GLOBAL MIDI submenu
 * CLOCK / MIDI OUT / MIDI SYNC (mockup round 4). */
static void seq_page_draw(const struct groove_view *v)
{
    if (paint_rendering && (paint_top >= 196 || paint_top + PAINT_ROWS <= 26)) return;
    static const char *const clocks[2] = {"INT", "MIDI"},
        *const srcs[4] = {"AUTO", "USB", "BLE", "DIN"}, *const modes[3] = {"LOCAL", "BOTH", "MIDI"}, *const roles[3] = {"NORMAL", "PLAY", "OFF"};
    unsigned pg = seq_page & 7U, sx[8] = {0};
    char name[24] = "";
    switch (pg) {
    case SEQ_STEP: { strcpy(name, "STEP "); decimal(name + 5, (int)cursor + 1, 1); /* the strip's cells keep their own signatures */
              sx[0] = groove_step_setting(track, cursor, 3); sx[4] = v->step.velocity; sx[5] = v->step.note;
              sx[6] = var_split_get(track, cursor) | step_lit_cursor() << 2; break; } /* C2: the big step */
    case SEQ_RHYTHM: strcpy(name, "VARIATION"); sx[3] = var_get(track, VAR_CHANCE); break;
    case SEQ_MIDI_CLOCK: strcpy(name, "CLOCK"); sx[0] = v->clock_mode != 0; sx[1] = groove_midi_setting(track, 6); break;
    case SEQ_MIDI_OUT: strcpy(name, "MIDI OUT"); sx[0] = groove_midi_setting(track, 0); sx[1] = groove_midi_setting(track, 1); sx[2] = groove_midi_setting(track, 2); sx[3] = groove_midi_setting(track, 4); break;
    case SEQ_MIDI_SYNC: strcpy(name, "MIDI SYNC"); sx[0] = groove_midi_setting(track, 3); sx[1] = groove_midi_setting(track, 5); sx[2] = groove_midi_setting(track, 7); sx[3] = groove_midi_setting(track, 8); break;
    default: strcpy(name, "TRACK"); sx[0] = groove_track_role(track); sx[1] = groove_midi_input_channel(track); break;
    }
    uint32_t key[10] = {pg | (cursor << 4) | (track << 12), sx[0], sx[1], sx[2], sx[3], sx[4], sx[5], sx[6], v->next_length, groove_pad_count(track) | (drum << 8) | (knob_shown() << 12)};
    uint32_t sig = signature(0x5e9bU, key, sizeof key);
    if (pg == SEQ_STEP || pg == SEQ_RHYTHM || pg == SEQ_ROLE) { /* the chord plate (C2: pages 2 and 3 too); a chord rewritten in its own table row keeps its id: follow the notes */
        uint8_t cn[8]; unsigned cc = step_chord_notes(v, cn);
        sig = signature(sig * 33U + cc, cn, cc);
    }
    if (!paint_rendering) {
        if (sig == cache->middle_cache) { if (pg == SEQ_STEP) seq_strip(v); return; } /* only the play head's cells */
        cache->middle_cache = sig;
        memset(cache->cell_cache[4], 0xff, sizeof cache->cell_cache[4]); /* the window repaints: the strip with it */
    }
    static const icon_draw_fn seq_icons[3] = {sqi_step, sqi_rec, sqi_role}, midi_icons[3] = {mi_clock, mi_out, mi_sync};
    unsigned midi = pg >= SEQ_MIDI_CLOCK && pg <= SEQ_MIDI_SYNC;
    paint_fill(42, 26, 198, 36, BACK);
    paint_fill(0, 62, 240, 133, BACK);
    if (midi) icon_row(3, pg - SEQ_MIDI_CLOCK, midi_icons);
    else icon_row(3, seq_nav_icon(pg), seq_icons);
    rbox(6, 66, 228, 124, 2, MD_PANEL, MD_EDGE);
    /* page 1: "STEP 23" in the middle of the band between the window top and the strip (his correction 07c24db) */
    sq_text(120, pg == SEQ_STEP ? 83 : strlen(name) > 14 ? 85 : 88, name, strlen(name) > 14 ? &lcd24 : &lcd30, MD_AMBER, MD_PANEL);
    const uint16_t *k = knob_colors;
    char num[24];
    if (pg == SEQ_STEP) { /* page 1 STEP: the cursor's 16 steps as on the grid, numbers under every 4th, the chord's notes */
        uint8_t notes[8]; char chord[8] = "";
        unsigned cnt = step_chord_notes(v, notes), base = cursor & ~15U;
        seq_strip(v);
        for (unsigned i = 0; i < 16; i += 4) {
            decimal(num, (int)(base + i) + 1, 1);
            sq_text((int)(seq_cell_x(i) + seq_cell_w(i) / 2), 129, num, &lcd12, V_DIM, MD_PANEL);
        }
        if (cnt) {
            unsigned cw = (212U - (cnt - 1U) * 6U) / cnt; /* 38 px as drawn for up to 3 notes, narrower for longer chords */
            if (cw > 38U) cw = 38U;
            for (unsigned i = 0; i < cnt; ++i) {
                unsigned x = 120 - (cnt * (cw + 6U) - 6U) / 2 + i * (cw + 6U);
                note_name(notes[i], num);
                sq_box(x, 138, cw, 26, num, cw >= 36 ? &lcd20 : cw >= 28 ? &lcd16 : &lcd12, k[i % 4U]);
            }
            if (cnt > 1) (void)chord_name(notes, cnt, chord, 0, 0); else note_name(notes[0], chord);
        }
        chord_plate(chord);
        if (sx[6] >> 2) { /* C2: the cursor step large, its split as hits inside one step (mockup b) */
            unsigned n = (sx[6] & 3U) + 1U, w = (94U - (n - 1U) * 4U) / n;
            uint16_t c = groove_pad_count(track) ? pad_colors[drum % 12] : SEQ_EMERALD;
            rbox(70, 172, 100, 16, 3, 0x18a3, 0x4a26);
            for (unsigned i = 0; i < n; ++i) rbox(73 + i * (w + 4U), 175, w, 10, 2, shade(c, 0, 1), V_NONE);
        }
    } else if (pg == SEQ_RHYTHM) { /* page 2 VARIATION: the CHANCE pattern, 16 cells (C3, mockup c; OFF = all 16 lit) */
        for (unsigned i = 0; i < 16; ++i) {
            unsigned x = 14U + i * 214U / 16U, w = 14U + (i + 1U) * 214U / 16U - x - 2U;
            unsigned plays = seq_chance_plays(sx[3], i);
            rbox(x, 110, w, 60, 2, plays ? k[3] : 0x18a1, plays ? V_NONE : 0x39a8);
        }
        char cv[12]; seq_chance_name(sx[3], 0, cv); /* OFF, HOUSE 1 .. DNB 3 */
        sq_text(120, 182, cv, &lcd15, k[3], MD_PANEL);
    } else if (pg == SEQ_MIDI_CLOCK) { /* K1 INT / MIDI, K2 CLK IN */
        const uint16_t c0[2] = {k[0], k[0]}, c1[4] = {k[1], k[1], k[1], k[1]};
        sq_chips(2, clocks, sx[0], 112, 14, 226, 26, c0);
        sq_chips(4, srcs, sx[1] & 3U, 152, 14, 226, 24, c1);
    } else if (pg == SEQ_MIDI_OUT) {
        const uint16_t c0[3] = {k[0], k[0], k[0]};
        sq_chips(3, modes, sx[0] % 3U, 104, 12, 228, 22, c0);
        sq_box(14, 148, 64, 26, "SEQ/ARP", &lcd15, k[0]);
        sq_line(78, 161, 102, 161, k[0]); sq_line(102, 161, 102, 142, k[0]); sq_line(102, 142, 132, 142, k[0]); sq_line(102, 161, 102, 180, k[0]); sq_line(102, 180, 132, 180, k[0]);
        sq_box(132, 132, 92, 22, "ENGINE", &lcd15, k[1]);
        strcpy(num, "MIDI OUT CH "); decimal(num + 12, (int)sx[1] + 1, 1); sq_box(132, 170, 92, 18, num, &lcd13, k[3]);
    } else if (pg == SEQ_MIDI_SYNC) {
        static const char *const ports[8] = {"OFF", "DIN", "USB", "DIN+USB", "BLE", "DIN+BLE", "USB+BLE", "ALL"}, *const ins[3] = {"DIN", "USB", "BLE"};
        char a[20] = "THRU IN ", b[20] = "THRU OUT ";
        strcat(a, ins[sx[2] % 3U]); strcat(b, ports[sx[3] & 7U]);
        unsigned lit = knob_shown(); /* the knob last turned (K1 on entering) wears the cursor */
        sq_box(14, 108, 70, 26, a, &lcd13, k[2]);
        if (lit == 2) glass_cursor(14, 108, 70, 26, 8, MD_AMBER);
        sq_line(84, 121, 154, 121, k[2]); fill_tri(150, 116, 150, 126, 156, 121, k[2]);
        sq_box(156, 108, 70, 26, b, &lcd12, k[3]);
        if (lit == 3) glass_cursor(156, 108, 70, 26, 8, MD_AMBER);
        const char *two[2] = {sx[0] ? "LOCAL ON" : "LOCAL OFF", sx[1] ? "CLK OUT ON" : "CLK OUT OFF"};
        const uint16_t c2[2] = {k[0], k[1]};
        sq_chips(2, two, lit < 2 ? lit : 2U, 150, 14, 226, 26, c2);
    } else {
        const uint16_t cr[3] = {k[1], k[3], k[2]};
        sq_chips(3, roles, sx[0] % 3U, 108, 14, 226, 28, cr);
        sq_text(60, 162, "IN CH", &lcd17, V_DIM, MD_PANEL);
        decimal(num, (int)sx[1] + 1, 1); sq_text(120, 162, num, &lcd30, k[1], MD_PANEL);
    }
    if (pg == SEQ_RHYTHM || pg == SEQ_ROLE) { /* C2: the cursor step's chord plate on pages 2 and 3 too (mockup c, d) */
        uint8_t notes[8]; char chord[8] = "";
        unsigned cnt = step_chord_notes(v, notes);
        if (cnt > 1) (void)chord_name(notes, cnt, chord, 0, 0); else if (cnt) note_name(notes[0], chord);
        chord_plate(chord);
    }
}
static __attribute__((noinline)) void steps(const struct groove_view *v)
{
    {
        static unsigned was_edit;
        unsigned edit = seq_page != 0;
        if (!paint_rendering && edit != was_edit) { /* the window and the grid share the field: repaint it whole */
            was_edit = edit;
            cache->seq_cache = cache->middle_cache = ~0U;
            memset(cache->cell_cache, 0xff, sizeof cache->cell_cache);
            paint_fill(0, 62, 240, 133, BACK);
        }
        if (edit) { seq_page_draw(v); return; }
    }
    if (paint_rendering && (paint_top >= SEQ_Y + 3 * SEQ_ROW + SEQ_H || paint_top + PAINT_ROWS <= 26)) return; /* the state plate starts at y 27 */
    /* v13: the 16 top cells only show which CHAIN slots are filled (the CHAIN page, SEQ hold, edits them). */
    uint32_t strip = 0;
    for (unsigned i = 0; i < 16; ++i) strip |= (uint32_t)(chain_slot[i] != 0) << i;
    if (strip != cache->seq_cache && !(paint_rendering && paint_top >= 86)) {
        cache->seq_cache = strip;
        /* v9: the header's row 2 (F A S L, engine, sound) is left of x 168;
         * here CHAIN under STEPS on the right, the pattern chain below. */
        paint_fill(0, 62, 240, 29, BACK);
        for (unsigned i = 0; i < 16; ++i) {
            unsigned used = chain_slot[i] != 0;
            rbox(seq_cell_x(i), 67, seq_cell_w(i), 18, 1, used ? 0x1081 : V_NONE, used ? MD_AMBER : 0x5acb); /* dark gold + amber frame = filled */
        }
    }
    /* v12: the state plate: step count and the chain lamp */
    if (!(paint_rendering && (paint_top >= 62 || paint_top + PAINT_ROWS <= 26)) && cache->middle_cache != (v->next_length | (unsigned)chain_on << 16)) {
        cache->middle_cache = v->next_length | (unsigned)chain_on << 16;
        char num[12]; decimal(num, v->next_length, 1);
        paint_fill(168, 26, 72, 36, BACK);
        state_plate(num, chain_on != 0, v->next_length != v->sequence_length ? V_ORANGE : TEXT);
    }
    struct seq_lanes l;
    seq_lanes_get(v, &l);
    for (unsigned step = 0; step < GROOVE_STEPS; ++step) {
        unsigned r = step / 16, i = step % 16;
        unsigned x = seq_cell_x(i), y = SEQ_Y + r * SEQ_ROW, cw = seq_cell_w(i);
        if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + SEQ_H <= paint_top)) continue;
        seq_cell(step, x, y, cw, &l, V_RED, BACK, &cache->cell_cache[r][i]); /* v13: red glass cursor */
    }
}
static unsigned fm_parameter(unsigned col)
{
    const struct fm_page *p = &fm_pages[fm_group];
    if (p->parameter[col] == 255)
        return 255;
    return p->parameter[col] + (p->per_operator ? (5 - op) * 21 : 0);
}
static void algorithm_draw(unsigned algorithm, unsigned muted);
static void window_draw(const char *title, uint16_t color);
static void state_plate_cap(const char *big, const char *cap);
static float vsin(float c);
/* ---- v6 FM LAB: the six operators are singing cat heads ---- */
static unsigned isqrt_ui(unsigned v)
{
    unsigned r = 0, b = 1U << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}
/* Anti-aliased drawing for the cats and the FM LAB waves. Coverage 0..40
 * mixes over what is already in the band buffer (shapes drawn in order). */
static uint16_t mix565(uint16_t bg, uint16_t fg, unsigned a)
{
    unsigned r = ((bg >> 11) * (40U - a) + (fg >> 11) * a) / 40U;
    unsigned g = (((bg >> 5) & 63U) * (40U - a) + ((fg >> 5) & 63U) * a) / 40U;
    unsigned b = ((bg & 31U) * (40U - a) + (fg & 31U) * a) / 40U;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static void paint_blend(int x, int y, uint16_t c, unsigned a)
{
    if (!a || x < 0 || y < 0 || x >= (int)FM1_SCREEN_WIDTH || y >= (int)FM1_SCREEN_HEIGHT) return;
    if (a > 40U) a = 40U;
    if (!paint_rendering) { paint_mark((unsigned)x, (unsigned)y, 1, 1); return; }
    if ((unsigned)y < paint_top || (unsigned)y >= paint_top + PAINT_ROWS || (unsigned)x < paint_x || (unsigned)x >= paint_end) return;
    uint16_t *p = paint_pixels + ((unsigned)y - paint_top) * (paint_end - paint_x) + (unsigned)x - paint_x;
    *p = mix565(*p, c, a);
}
static int overlap10(int a0, int a1, int b0, int b1) /* length of [a0,a1) ∩ [b0,b1) */
{
    int lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0;
}
/* One pixel row from four sub-rows: spans [xl, xr) in 1/10 px (xl >= xr = empty). */
static void aa_row(int y, const int xl[4], const int xr[4], uint16_t c)
{
    if (y < 0 || y >= (int)FM1_SCREEN_HEIGHT) return;
    int lo = 1 << 20, hi = -(1 << 20), inl = -(1 << 20), inr = 1 << 20;
    unsigned n = 0;
    for (unsigned k = 0; k < 4; ++k) {
        if (xl[k] >= xr[k]) continue;
        ++n;
        if (xl[k] < lo) lo = xl[k];
        if (xr[k] > hi) hi = xr[k];
        if (xl[k] > inl) inl = xl[k];
        if (xr[k] < inr) inr = xr[k];
    }
    if (!n) return;
    if (lo < 0) lo = 0;
    if (!paint_rendering) { paint_mark((unsigned)(lo / 10), (unsigned)y, (unsigned)((hi + 9) / 10 - lo / 10), 1); return; }
    int fa = 0, fb = 0; /* pixels covered in all four sub-rows */
    if (n == 4) { fa = (inl + 9) / 10; fb = inr / 10; }
    if (fb > fa) paint_fill((unsigned)fa, (unsigned)y, (unsigned)(fb - fa), 1, c);
    for (int x = lo / 10; x * 10 < hi; ++x) {
        if (fb > fa && x >= fa && x < fb) continue;
        unsigned cov = 0;
        for (unsigned k = 0; k < 4; ++k) if (xl[k] < xr[k]) cov += (unsigned)overlap10(x * 10, x * 10 + 10, xl[k], xr[k]);
        paint_blend(x, y, c, cov);
    }
}
/* Filled ellipse; centre and radii in 1/10 px, smooth edges. */
static void fill_ellipse(int cx, int cy, int rx, int ry, uint16_t c)
{
    if (rx < 5 || ry < 5) return;
    int y0 = (cy - ry) / 10 - 1, y1 = (cy + ry) / 10;
    if (paint_rendering) { /* only the rows of the band being painted */
        if (y0 < (int)paint_top) y0 = (int)paint_top;
        if (y1 > (int)paint_top + PAINT_ROWS - 1) y1 = (int)paint_top + PAINT_ROWS - 1;
    }
    for (int y = y0; y <= y1; ++y) {
        int xl[4], xr[4];
        for (unsigned k = 0; k < 4; ++k) {
            int dy = y * 10 + (int)(2U * k + 1U) * 10 / 8 - cy;
            if (dy <= -ry || dy >= ry) { xl[k] = xr[k] = 0; continue; }
            int dx = rx * (int)isqrt_ui((unsigned)(ry * ry - dy * dy)) / ry;
            xl[k] = cx - dx; xr[k] = cx + dx;
        }
        aa_row(y, xl, xr, c);
    }
}
/* Filled triangle; points in 1/10 px, smooth edges. */
static void fill_tri(int ax, int ay, int bx, int by, int cx, int cy, uint16_t c)
{
    int lo = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy), hi = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    const int px[3] = {ax, bx, cx}, py[3] = {ay, by, cy};
    int ylo = lo / 10 - 1, yhi = hi / 10;
    if (paint_rendering) {
        if (ylo < (int)paint_top) ylo = (int)paint_top;
        if (yhi > (int)paint_top + PAINT_ROWS - 1) yhi = (int)paint_top + PAINT_ROWS - 1;
    }
    for (int y = ylo; y <= yhi; ++y) {
        int xl[4], xr[4];
        for (unsigned k = 0; k < 4; ++k) {
            int sy = y * 10 + (int)(2U * k + 1U) * 10 / 8, l = 1 << 20, r = -(1 << 20);
            for (unsigned e = 0; e < 3; ++e) {
                int x0 = px[e], y0 = py[e], x1 = px[(e + 1) % 3], y1 = py[(e + 1) % 3];
                if ((sy < y0) == (sy < y1)) continue;
                int x = x0 + (x1 - x0) * (sy - y0) / (y1 - y0);
                if (x < l) l = x;
                if (x > r) r = x;
            }
            xl[k] = l; xr[k] = r;
        }
        aa_row(y, xl, xr, c);
    }
}
/* Lower half of a thin circle (centre, radius in 1/10 px), smooth. */
static void aa_arc_low(int cx, int cy, int r, uint16_t c)
{
    if (r < 8) r = 8;
    for (int y = cy / 10; y * 10 <= cy + r + 6; ++y)
        for (int x = (cx - r - 6) / 10; x * 10 <= cx + r + 6; ++x) {
            int dx = x * 10 + 5 - cx, dy = y * 10 + 5 - cy;
            if (dy < -2) continue;
            int d = (int)isqrt_ui((unsigned)(dx * dx + dy * dy)) - r;
            if (d < 0) d = -d;
            if (d < 6) paint_blend(x, y, c, (unsigned)(6 - d) * 40U / 6U);
        }
}
/* Smooth thin line, mostly horizontal (whiskers); ends in 1/10 px. */
static void aa_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    for (int x = x0 / 10; x * 10 < x1; ++x) {
        int xc = x * 10 + 5;
        if (xc < x0 || xc > x1) continue;
        int yc = x1 > x0 ? y0 + (y1 - y0) * (xc - x0) / (x1 - x0) : y0;
        for (int y = (yc - 5) / 10; y * 10 <= yc + 5; ++y)
            paint_blend(x, y, c, (unsigned)overlap10(y * 10, y * 10 + 10, yc - 5, yc + 5) * 4U * 3U / 4U);
    }
}
/* Thick smooth stroke from round dots; ends in 1/10 px. */
static void aa_stroke(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 - x0, dy = y1 - y0, len = (int)isqrt_ui((unsigned)(dx * dx + dy * dy)), n = len / 4 + 1;
    for (int i = 0; i <= n; ++i) fill_ellipse(x0 + dx * i / n, y0 + dy * i / n, 9, 9, c);
}
/* v7 engine pictograms in a 30 x 25 box: DX7 FM wave, 303 saw, 808 drum,
 * PSG square, PLUCK decaying string, SAMPLER waveform bars. */
static void engine_icon(unsigned x, unsigned y, int engine, uint16_t c)
{
    const int cx = (int)(x + 15) * 10, cy = (int)(y + 12) * 10 + 5;
    if (engine == GROOVE_DX7 || engine == GROOVE_PLUCK) {
        int px = cx - 110, py = cy;
        for (int k = 1; k <= 22; ++k) {
            float u = (float)k / 22.0f, a = engine == GROOVE_PLUCK ? 70.0f * (1.0f - u) : 70.0f;
            float w = engine == GROOVE_PLUCK ? vsin(u * 3.5f - (float)(int)(u * 3.5f)) : vsin(u - (float)(int)u) * (0.6f + 0.4f * vsin(u * 3.0f - (float)(int)(u * 3.0f)));
            int nx = cx - 110 + k * 10, ny = cy - (int)(a * w);
            aa_stroke(px, py, nx, ny, c); px = nx; py = ny;
        }
    } else if (engine == GROOVE_303) {
        static const int8_t pts[6][2] = {{-11, 7}, {-4, -8}, {-4, 7}, {4, -8}, {4, 7}, {11, -8}};
        for (unsigned i = 0; i + 1 < 6; ++i)
            aa_stroke(cx + pts[i][0] * 10, cy + pts[i][1] * 10, cx + pts[i + 1][0] * 10, cy + pts[i + 1][1] * 10, c);
    } else if (engine == ENGINE_MICRO) {
        for (int k = 0; k < 2; ++k) {
            uint16_t ink = k ? c : shade(c, 0, 5);
            int ox = k ? 15 : -15, oy = k ? 20 : -25;
            for (int i = 0; i < 3; ++i) {
                int x0 = cx - 105 + ox + i * 70;
                aa_stroke(x0, cy + 60 + oy, x0 + 70, cy - 60 + oy, ink);
                aa_stroke(x0 + 70, cy - 60 + oy, x0 + 70, cy + 60 + oy, ink);
            }
        }
    } else if (engine == GROOVE_808) {
        fill_ellipse(cx, cy, 95, 95, c); fill_ellipse(cx, cy, 75, 75, shade(c, 0, 8)); fill_ellipse(cx, cy, 35, 35, c);
    } else if (engine == GROOVE_PSG) {
        static const int8_t pts[8][2] = {{-11, 6}, {-11, -6}, {-4, -6}, {-4, 6}, {4, 6}, {4, -6}, {11, -6}, {11, 6}};
        for (unsigned i = 0; i + 1 < 8; ++i)
            aa_stroke(cx + pts[i][0] * 10, cy + pts[i][1] * 10, cx + pts[i + 1][0] * 10, cy + pts[i + 1][1] * 10, c);
    } else {
        static const uint8_t h[11] = {3, 7, 10, 6, 9, 4, 8, 5, 3, 6, 2};
        for (unsigned i = 0; i < 11; ++i) paint_fill(x + 4 + i * 2, (unsigned)((int)y + 12 - h[i]), 1, (unsigned)(2 * h[i] + 1), c);
    }
}
/* Six different cats: fur, dark, light, eye; mark 0 none, 1 stripes, 2 calico, 3 mask. */
static const uint16_t cat_fur[6][4] = {{0xfcc7, 0xcb43, 0xff17, 0x47e8}, {0x2125, 0x1082, 0x39c8, 0xfe87},
    {0xf79e, 0xce5a, 0xffff, 0x4d1f}, {0x9cd4, 0x52ab, 0xe71d, 0x7f0b}, {0xf77c, 0xcba5, 0xffff, 0x47e8},
    {0xef19, 0x5a06, 0xffbd, 0x4d1f}};
static const uint8_t cat_mark[6] = {1, 0, 0, 1, 2, 3};
/* Head centred at (x, y) px, scale s in tenths (12 = mockup 1.2). mouth 0 = smile, 1..10 open (singing). */
static void cat_head(int x, int y, int s, unsigned v, unsigned mouth, unsigned dim)
{
    uint16_t fur = cat_fur[v][0], dark = cat_fur[v][1], light = cat_fur[v][2], eye = cat_fur[v][3];
    uint16_t pink = 0xfcf8, nose = 0xfb55, lip = 0x3863, whisker = v == 2 || v == 4 || v == 5 ? 0x2104 : 0xffff; /* dark on white, calico, siamese */
    if (dim) { fur = shade(fur, 0, 6); dark = shade(dark, 0, 6); light = shade(light, 0, 6); eye = shade(eye, 0, 6);
               pink = shade(pink, 0, 6); nose = shade(nose, 0, 6); whisker = shade(whisker, 0, 6); }
    const int X = x * 10, Y = y * 10;
#define P(n) ((n) * s)
    for (int sx = -1; sx <= 1; sx += 2) {
        fill_tri(X + sx * P(13), Y - P(3), X + sx * P(11), Y - P(17), X + sx * P(3), Y - P(11), cat_mark[v] == 3 ? dark : fur);
        fill_tri(X + sx * P(21) / 2, Y - P(6), X + sx * P(10), Y - P(27) / 2, X + sx * P(11) / 2, Y - P(10), pink);
    }
    fill_ellipse(X, Y, P(14), P(23) / 2, fur);                       /* round face */
    fill_ellipse(X - P(11), Y + P(4), P(4), P(9) / 2, fur);          /* cheeks */
    fill_ellipse(X + P(11), Y + P(4), P(4), P(9) / 2, fur);
    if (cat_mark[v] == 1) {
        for (int dx = -4; dx <= 4; dx += 4) fill_ellipse(X + P(dx), Y - P(17) / 2, P(1), P(5) / 2, dark);
        fill_ellipse(X - P(12), Y, P(2), P(4) / 5, dark);
        fill_ellipse(X + P(12), Y, P(2), P(4) / 5, dark);
    } else if (cat_mark[v] == 2) {
        fill_ellipse(X - P(7), Y - P(6), P(6), P(5), dark);
        fill_ellipse(X + P(8), Y - P(7), P(5), P(4), 0x2926);
    } else if (cat_mark[v] == 3) fill_ellipse(X, Y + P(3), P(7), P(6), dark);
    fill_ellipse(X - P(3), Y + P(9) / 2, P(4), P(16) / 5, light);    /* muzzle */
    fill_ellipse(X + P(3), Y + P(9) / 2, P(4), P(16) / 5, light);
    for (int sx = -1; sx <= 1; sx += 2) {                            /* eyes: iris, slit pupil, glint */
        int ex = X + sx * P(11) / 2, ey = Y - P(5) / 2;
        fill_ellipse(ex, ey, P(14) / 5, P(3), eye);
        fill_ellipse(ex, ey, P(7) / 10 > 6 ? P(7) / 10 : 6, P(24) / 10, 0x0000); /* slit pupil */
        if (s >= 8) paint_fill((unsigned)((ex + P(6) / 10 + 5) / 10), (unsigned)((ey - P(2) + 5) / 10), 1, 1, 0xffff);
    }
    fill_tri(X - P(8) / 5, Y + P(8) / 5, X + P(8) / 5, Y + P(8) / 5, X, Y + P(16) / 5, nose);
    if (mouth) fill_ellipse(X, Y + P(13) / 2, P(11) / 5, P(1) * 34 * (int)mouth / 100 + 6, lip);
    else {                                                           /* closed: two small arcs, "w" */
        aa_arc_low(X - P(7) / 5, Y + P(21) / 5, P(7) / 5, lip);
        aa_arc_low(X + P(7) / 5, Y + P(21) / 5, P(7) / 5, lip);
    }
    for (int sx = -1; sx <= 1; sx += 2)                              /* whiskers */
        for (int k = -1; k <= 1; ++k)
            aa_line(X + sx * P(6), Y + P(9) / 2 + k * P(6) / 5, X + sx * P(17), Y + P(7) / 2 + k * P(3), whisker);
#undef P
}
/* Golden nimbus over the chosen voice: soft glow, ring, a spark on the right. */
static void voice_nimbus(int x, int y, int s)
{
    const uint16_t gold = 0xfe87;
    int rx = 13 * s, ry = 4 * s;
    fill_ellipse(x * 10, y * 10, rx + 50, ry + 40, shade(gold, 0, 8));
    fill_ellipse(x * 10, y * 10, rx + 25, ry + 22, shade(gold, 0, 5));
    fill_ellipse(x * 10, y * 10, rx + 12, ry + 12, gold);
    fill_ellipse(x * 10, y * 10, rx - 15, ry - 12 > 5 ? ry - 12 : 5, shade(gold, 0, 8));
    paint_fill((unsigned)(x + rx / 10 - 1), (unsigned)(y - 1), 2, 2, 0xffd5);
}
/* One envelope stage, FX style: the line only, no fill (user 13.11, one style). */
static void env_stage(int x0, int y0, int x1, int y1, uint16_t c, unsigned dim)
{
    if (dim) c = shade(c, 0, 6);
    line(x0, y0, x1, y1, c);
    line(x0, y0 - 1, x1, y1 - 1, c);
}
static const uint8_t algorithms[32][12];
/* Mouth of a singing (carrier) voice: 0 when silent, else 2..10 with time. */
/* Positions follow the algorithm: carriers on the bottom row, each modulator
 * above what it feeds; mockup spacing (76 px, rows 142 / 76, scale 1.2),
 * shrinking for wide or tall stacks. The chosen voice wears the nimbus,
 * singing voices open their mouths and get a note. */
/* ---- v13 FM LAB: eleven pages, a page-icon row in the header (icon_row), the page's name large in the window ---- */
/* Round line segment, coordinates in 1/10 px, radius r in 1/10 px (>= 5). */
static void ic_seg(int x0, int y0, int x1, int y1, int r, uint16_t c)
{
    int dx = x1 - x0, dy = y1 - y0, n = (int)isqrt_ui((unsigned)(dx * dx + dy * dy)) / 4 + 1;
    for (int i = 0; i <= n; ++i) fill_ellipse(x0 + dx * i / n, y0 + dy * i / n, r, r, c);
}
/* Polyline of points given in 1/10 px relative to the icon centre. */
static void ic_poly(int cx, int cy, const int8_t (*p)[2], unsigned n, int r, uint16_t c)
{
    for (unsigned i = 0; i + 1 < n; ++i) ic_seg(cx * 10 + p[i][0], cy * 10 + p[i][1], cx * 10 + p[i + 1][0], cy * 10 + p[i + 1][1], r, c);
}
static void ic_dash(int cx, int cy, uint16_t c) /* dotted line through the centre */
{
    for (int x = -9; x < 9; x += 4) ic_seg(cx * 10 + x * 10, cy * 10, cx * 10 + x * 10 + 15, cy * 10, 5, c);
}
static const int8_t ic_e5[5][2] = {{-90, 70}, {-40, -70}, {10, -10}, {40, -10}, {90, 70}};
static void ic_tune(int cx, int cy, uint16_t c)
{
    for (int k = 0; k < 16; ++k) {
        int y0 = (int)(vsin((float)k / 16.0f) * -36.0f), y1 = (int)(vsin((float)(k + 1) / 16.0f) * -36.0f);
        ic_seg(cx * 10 - 80 + k * 10, cy * 10 - 40 + y0, cx * 10 - 70 + k * 10, cy * 10 - 40 + y1, 8, c);
    }
    rbox((unsigned)(cx - 8), (unsigned)(cy + 4), 16, 4, 2, V_NONE, c);
    rbox((unsigned)(cx - 8), (unsigned)(cy + 4), 10, 4, 2, c, V_NONE);
}
static void ic_rates(int cx, int cy, uint16_t c)
{
    ic_poly(cx, cy, ic_e5, 5, 9, c);
    paint_fill((unsigned)(cx - 9), (unsigned)(cy + 9), 18, 1, c);
}
static void ic_levels(int cx, int cy, uint16_t c)
{
    ic_poly(cx, cy, ic_e5, 5, 6, c);
    for (unsigned i = 1; i <= 3; ++i) fill_ellipse(cx * 10 + ic_e5[i][0], cy * 10 + ic_e5[i][1], 21, 21, c);
}
static void ic_prates(int cx, int cy, uint16_t c) { ic_rates(cx, cy, c); ic_dash(cx, cy, c); }
static void ic_plevels(int cx, int cy, uint16_t c) { ic_levels(cx, cy, c); ic_dash(cx, cy, c); }
static void ic_sens(int cx, int cy, uint16_t c)
{
    static const uint8_t h[3] = {4, 7, 11};
    for (int i = 0; i < 3; ++i) paint_fill((unsigned)(cx - 8 + i * 6), (unsigned)(cy + 8 - h[i]), 4, h[i], c);
}
static void ic_scal(int cx, int cy, uint16_t c)
{
    static const int8_t v[3][2] = {{-90, -60}, {0, 40}, {90, -60}};
    ic_poly(cx, cy, v, 3, 9, c);
    fill_ellipse(cx * 10, cy * 10 + 40, 22, 22, c);
}
static void ic_curve(int cx, int cy, uint16_t c)
{
    static const uint8_t y[11] = {0, 1, 4, 10, 19, 30, 45, 64, 86, 111, 140}; /* 14 px * (k / 10)^2.2, 1/10 px */
    for (int k = 0; k < 10; ++k) ic_seg(cx * 10 - 90 + k * 18, cy * 10 + 70 - y[k], cx * 10 - 90 + (k + 1) * 18, cy * 10 + 70 - y[k + 1], 9, c);
}
static void ic_algo(int cx, int cy, uint16_t c)
{
    for (int k = 0; k < 2; ++k) {
        rbox((unsigned)(cx - 6), (unsigned)(cy - 9 + k * 11), 12, 7, 1, V_NONE, c);
        rbox((unsigned)(cx - 5), (unsigned)(cy - 8 + k * 11), 10, 5, 1, V_NONE, c);
    }
    paint_fill((unsigned)cx - 1, (unsigned)(cy - 2), 2, 4, c);
}
static void ic_sine(int cx, int cy, float cycles, int amp, int grow, uint16_t c)
{
    for (int k = 0; k < 18; ++k) {
        int a0 = amp + grow * k / 18, a1 = amp + grow * (k + 1) / 18;
        float u0 = cycles * (float)k / 18.0f, u1 = cycles * (float)(k + 1) / 18.0f;
        ic_seg(cx * 10 - 90 + k * 10, cy * 10 - (int)(vsin(u0 - (float)(int)u0) * (float)a0), cx * 10 - 80 + k * 10,
               cy * 10 - (int)(vsin(u1 - (float)(int)u1) * (float)a1), 9, c);
    }
}
static void ic_lfo(int cx, int cy, uint16_t c) { ic_sine(cx, cy, 1.0f, 55, 0, c); }
static void ic_lfom(int cx, int cy, uint16_t c) { ic_sine(cx, cy, 1.5f, 10, 50, c); }
static const icon_draw_fn fm_icons[11] = {ic_tune, ic_rates, ic_levels, ic_sens, ic_scal, ic_curve, ic_prates, ic_plevels, ic_algo, ic_lfo, ic_lfom};
/* TUNE/LVL, SENS/OSC, SCALING, CURVES: six operator tiles 32 x 72 (muted = dim), the lit operator under the glass cursor. */
static void fm_tiles(const uint8_t patch[156], unsigned muted, unsigned g)
{
    static const char *const curves[4] = {"-LN", "-EX", "+EX", "+LN"};
    for (unsigned i = 0; i < 6; ++i) {
        unsigned x = 12 + i * 36, off = (muted >> i) & 1U, o = (5U - i) * 21U;
        uint16_t k0 = off ? V_FAINT : knob_colors[0], k1 = off ? V_FAINT : knob_colors[1], k2 = off ? V_FAINT : knob_colors[2];
        char n1[2] = {(char)('1' + i), 0}, t[12];
        rbox(x, 112, 32, 72, 4, MD_PANEL, MD_EDGE);
        lcd_center((int)x + 16, 114, n1, &lcd20, off ? V_FAINT : TEXT, MD_PANEL);
        if (g == 0) {
            unsigned level = patch[o + 16];
            glass_bar_v(x + 11, 134, 10, 44, level > 99 ? 256U : level * 256U / 99U, off ? V_FAINT : engine_colors[GROOVE_DX7]);
        } else if (g == 3) {
            glass_bar_v(x + 7, 134, 7, 30, patch[o + 14] > 3 ? 256U : patch[o + 14] * 256U / 3U, k0);
            glass_bar_v(x + 18, 134, 7, 30, patch[o + 15] > 7 ? 256U : patch[o + 15] * 256U / 7U, k1);
            lcd_center((int)x + 16, 168, patch[o + 17] ? "F" : "R", &lcd16, k2, MD_PANEL);
        } else if (g == 4) {
            glass_bar_v(x + 7, 134, 7, 30, patch[o + 9] > 99 ? 256U : patch[o + 9] * 256U / 99U, k0);
            glass_bar_v(x + 18, 134, 7, 30, patch[o + 10] > 99 ? 256U : patch[o + 10] * 256U / 99U, k1);
            note_name(patch[o + 8] + 9U, t);
            lcd_center((int)x + 16, 169, t, &lcd14, k2, MD_PANEL);
        } else {
            lcd_center((int)x + 16, 135, curves[patch[o + 11] & 3U], &lcd14, k0, MD_PANEL);
            lcd_center((int)x + 16, 150, curves[patch[o + 12] & 3U], &lcd14, k1, MD_PANEL);
            glass_bar_h(x + 4, 170, 24, 6, patch[o + 13] > 7 ? 256U : patch[o + 13] * 256U / 7U, k2);
        }
        if (i == op) glass_cursor(x, 112, 32, 72, 11, MD_AMBER);
    }
}
/* RATES / LEVELS (operator and pitch envelope): the curve in the four knob colours, a soft fill under each stage. */
static void fm_env_page(const uint8_t patch[156])
{
    unsigned offset = op == 6 ? 126 : (5 - op) * 21;
    const int yb = 178, hh = 70;
    int px[6], py[6], x = 14;
    px[0] = x; py[0] = yb - patch[offset + 7] * hh / 99;
    for (unsigned i = 0; i < 3; ++i) { x += 10 + (99 - patch[offset + i]) * 34 / 99; px[i + 1] = x; py[i + 1] = yb - patch[offset + 4 + i] * hh / 99; }
    x += 36; px[4] = x; py[4] = py[3];
    x += 10 + (99 - patch[offset + 3]) * 34 / 99; px[5] = x; py[5] = py[0];
    paint_fill(12, (unsigned)yb, 216, 1, 0x2923);
    if (op == 6) for (int d = 12; d < 228; d += 5) paint_fill((unsigned)d, (unsigned)(yb - hh * 50 / 99), 2, 1, 0x4a2b); /* level 50: the pitch does not move */
    /* user 13.11: one green line, no gradient; the node dots keep the knob colours */
    const uint16_t col[5] = {knob_colors[0], knob_colors[1], knob_colors[2], V_FAINT, knob_colors[3]};
    for (unsigned k = 0; k < 5; ++k) ic_seg(px[k] * 10, py[k] * 10, px[k + 1] * 10, py[k + 1] * 10, 13, MX_G);
    static const uint8_t node[4] = {1, 2, 3, 5};
    for (unsigned n = 0; n < 4; ++n) {
        fill_ellipse(px[node[n]] * 10, py[node[n]] * 10, 41, 41, col[node[n] - 1]);
        fill_ellipse(px[node[n]] * 10, py[node[n]] * 10, 27, 27, TEXT);
    }
}
/* DX7 LFO / LFO MOD: the chosen wave, 3.2 cycles; LFO MOD fades it in after DELAY (dotted line = end of the delay). */
static void fm_lfo_page(const uint8_t patch[156], unsigned mod)
{
    static const char *const waves[6] = {"TRIANGLE", "SAW DOWN", "SAW UP", "SQUARE", "SINE", "S/HOLD"};
    static const float sh[6] = {0.6f, -0.4f, 0.9f, -0.8f, 0.2f, -0.1f};
    unsigned wv = patch[142] % 6U;
    const int cy = 142, amp = mod ? 30 : 34;
    float d0 = (float)(patch[138] > 99 ? 99 : patch[138]) * 0.4f / 99.0f, d1 = d0 + 0.24f;
    if (d1 > 1.0f) d1 = 1.0f;
    if (!mod) lcd_text(226 - (int)lcd_width(&lcd20, waves[wv]), 79, waves[wv], &lcd20, knob_colors[0], MD_PANEL);
    paint_fill(14, cy, 212, 1, 0x2923);
    if (mod) for (int d = cy - 38; d < cy + 38; d += 5) paint_fill((unsigned)(14 + (int)(212.0f * d0)), (unsigned)d, 1, 3, knob_colors[0]);
    int lx = 0, ly = 0;
    for (int k = 0; k <= 212; k += 2) {
        float u = (float)k / 212.0f, w = u * 3.2f, f = w - (float)(int)w, v;
        v = wv == 0 ? 1.0f - 4.0f * (f < 0.5f ? 0.5f - f : f - 0.5f) : wv == 1 ? 1.0f - 2.0f * f : wv == 2 ? 2.0f * f - 1.0f
          : wv == 3 ? (f < 0.5f ? 1.0f : -1.0f) : wv == 4 ? vsin(f) : sh[(unsigned)(int)(u * 4.0f) % 6U];
        float a = !mod ? 1.0f : u < d0 ? 0.0f : u < d1 ? (u - d0) / (d1 - d0) : 1.0f;
        int X = (14 + k) * 10, Y = cy * 10 - (int)(a * (float)amp * v * 10.0f);
        if (k) ic_seg(lx, ly, X, Y, 13, knob_colors[0]);
        lx = X; ly = Y;
    }
}
static __attribute__((noinline)) void fm_lab_page(const uint8_t patch[156], const struct groove_fm_controls *controls)
{
    const unsigned g = fm_group > 10 ? 10U : fm_group, muted = controls->operators;
    uint32_t head = g | ((unsigned)(patch[134] & 31U) << 4) | (track << 9);
    if (cache->title_cache != head) {
        cache->title_cache = head;
        paint_fill(42, 26, 198, 36, BACK);
        unsigned first = g < 6 ? 0U : g < 8 ? 6U : g == 8 ? 8U : 9U, n = g < 6 ? 6U : g == 8 ? 1U : 2U;
        icon_row(n, g - first, fm_icons + first);
        char num[12]; decimal(num, (int)(patch[134] & 31U) + 1, 1);
        state_plate_cap(num, "ALGO");
    }
    uint32_t sig = signature(patch[134] | (op << 5) | (muted << 8) | (track << 16) | (g << 20), patch, 145);
    if (cache->middle_shape == sig) return;
    cache->middle_shape = sig;
    paint_fill(0, 62, 240, 133, BACK);
    window_draw(fm_pages[g].title, MD_AMBER);
    if (g == 1 || g == 2 || g == 6 || g == 7) fm_env_page(patch);
    else if (g == 8) algorithm_draw(patch[134], muted);
    else if (g >= 9) fm_lfo_page(patch, g == 10);
    else fm_tiles(patch, muted, g);
}
/* DX7 AMP ENV (page FM_ENV) and the dispatch to the FM LAB pages. */
static __attribute__((noinline, cold, minsize)) void fm_draw(const uint8_t patch[156], const struct groove_fm_controls *controls)
{
    if (paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 26)) return; /* the state plate starts at y 27 */
    if (page == FM_EDIT || page == FM_LFO) { fm_lab_page(patch, controls); return; }
    uint32_t shape = signature(controls->enabled, controls->envelope, 4);
    uint32_t context = track | (fm_group << 3) | (page << 7);
    if (cache->middle_shape != shape || cache->row_cache[0] != context) {
        cache->row_cache[0] = context;
        cache->middle_shape = shape;
        paint_fill(0, 62, 240, 133, BACK);
        /* v6, like the FX curves: dark box, each stage in its knob colour
         * with a soft fill below. Match adsr_config_set's millisecond/level
         * mapping; sustain gets an illustrative 400 ms hold. */
        state_plate_cap(controls->enabled ? "ON" : "OFF", "ADSR");
        unsigned a = 8U * controls->envelope[0], d = 100U + 8U * controls->envelope[1];
        unsigned r = 100U + 19U * controls->envelope[3], total = a + d + 400U + r;
        int x0 = 12, x1 = 12 + (int)(a * 216U / total), x2 = 12 + (int)((a + d) * 216U / total);
        int x3 = 12 + (int)((a + d + 400U) * 216U / total);
        int y = 184 - (int)((100U + 7U * controls->envelope[2]) * 74U / 1000U);
        window_draw("AMP ENV", 0xafb8);
        unsigned dim = !controls->enabled;
        env_stage(x0, 184, x1, 110, MX_G, dim);
        env_stage(x1, 110, x2, y, MX_G, dim);
        env_stage(x2, y, x3, y, MX_G, dim);
        env_stage(x3, y, 228, 184, MX_G, dim);
    }
}
static unsigned tone_part(void) { return groove_pad_count(track) != 0 ? drum_parts[track] : 0; }
static unsigned tone_slot(void)
{
    return groove_pad_count(track) != 0 ? 1U + drum_parts[track] : 0;
}
/* ENV exposes each engine's actual time/shape controls, without changing
 * its synthesis model. A missing knob is blank rather than a dummy ADSR. */
static unsigned tone_parameter(unsigned col)
{
    unsigned type = groove_source_type(track);
    if (page != FM_ENV) return tone_pages[track][tone_slot()] * 4U + col;
    static const uint8_t bass[] = {BASS303_ENVMOD, BASS303_DECAY, BASS303_ACCDEC, BASS303_ACCENT}; /* user 13.11: CUTOFF off K4 */
    static const uint8_t pluck[] = {0, 3, 1, 255};
    static const uint8_t sample[] = {2, 3, 255, 255};
    if (type == GROOVE_303) return bass[col];
    if (type == GROOVE_PSG) return col ? 255 : 3;
    if (type == GROOVE_PLUCK) return pluck[col];
    if (type == GROOVE_SAMPLE) return sample[col];
    const char *name = col == 0 ? "Decay" : col == 1 ? "Attack" : col == 2 ? "Tone" : "Tune";
    for (unsigned i = 0; i < groove_tone_count(track, tone_part()); ++i) {
        const fm1_param_t *p = groove_tone_parameter(track, tone_part(), i);
        if (p && !strcmp(p->name, name)) return i;
    }
    return 255;
}
static void tone_label(unsigned parameter, unsigned value, const fm1_param_t *p, char out[12])
{
    unsigned type = groove_source_type(track);
    if (p->names)
        strcpy(out, p->names[value]);
    else
        decimal(out,
                (int)value - (((type == GROOVE_PSG || type == GROOVE_PLUCK) && parameter == 2) ||
                                      (type == GROOVE_SAMPLE && parameter == 1)
                                  ? 24
                                  : 0),
                1);
}
static float fexp2_ui(float x);
/* ---- v7 sound pages ---- */
static float vlog2(float x);
/* Outline ring: an ellipse with a 1.5 px rim, inside painted bg. 1/10 px. */
static void ring(int cx, int cy, int rx, int ry, uint16_t c, uint16_t bg)
{
    fill_ellipse(cx, cy, rx, ry, c);
    if (rx > 15 && ry > 15) fill_ellipse(cx, cy, rx - 15, ry - 15, bg);
}
/* 808 parts as pictograms in a 34 x 32 tile: KICK, SNARE, LOW/MID/HIGH TOM,
 * RIM, CLAP, COWBELL, CYMBAL, OPEN HAT, CLOSED HAT, then the whole KIT. */
static void drum_icon(unsigned x, unsigned y, unsigned i, uint16_t c, uint16_t bg)
{
    const int cx = (int)(x + 17) * 10, cy = (int)(y + 16) * 10;
    if (i == 0) { ring(cx, cy, 100, 100, c, bg); fill_ellipse(cx, cy, 40, 40, c); }
    else if (i == 1 || (i >= 2 && i <= 4)) {
        int r = i == 1 ? 100 : i == 2 ? 105 : i == 3 ? 85 : 65, top = cy - (i == 1 ? 40 : r / 2);
        int bot = i == 1 ? cy + 50 : cy + r / 2;
        aa_stroke(cx - r, top, cx - r, bot, c); aa_stroke(cx + r, top, cx + r, bot, c);
        aa_stroke(cx - r, bot, cx + r, bot, c);
        ring(cx, top, r, r / 3 > 25 ? r / 3 : 25, c, bg);
        if (i == 1) for (int k = -60; k <= 60; k += 40) aa_stroke(cx + k, bot + 25, cx + k + 20, bot - 5, c);
    } else if (i == 5) { ring(cx, cy + 30, 100, 30, c, bg); aa_stroke(cx - 90, cy - 80, cx + 60, cy + 20, c); }
    else if (i == 6) { ring(cx - 50, cy + 20, 40, 70, c, bg); ring(cx + 50, cy + 20, 40, 70, c, bg); }
    else if (i == 7) {
        aa_stroke(cx - 50, cy - 90, cx + 50, cy - 90, c); aa_stroke(cx + 50, cy - 90, cx + 90, cy + 80, c);
        aa_stroke(cx + 90, cy + 80, cx - 90, cy + 80, c); aa_stroke(cx - 90, cy + 80, cx - 50, cy - 90, c);
    } else if (i == 8) { aa_stroke(cx, cy - 30, cx, cy + 100, c); ring(cx, cy - 30, 120, 30, c, bg); }
    else if (i == 9 || i == 10) {
        aa_stroke(cx, cy - 60, cx, cy + 100, c);
        ring(cx, i == 9 ? cy - 60 : cy - 30, 100, 26, c, bg); ring(cx, cy - 10, 100, 26, c, bg);
    } else { ring(cx - 50, cy + 30, 60, 60, c, bg); ring(cx + 70, cy + 50, 40, 40, c, bg); ring(cx + 50, cy - 70, 70, 22, c, bg); }
}
/* ---- MICRO D pages: a Moog-like panel (black, cream lettering, amber /
 * blue / orange marks, skirted knobs, rocker switches), one picture per
 * page, the turned knob's name on top, the panel section's name below. ---- */
static const uint16_t *const md_ink = knob_colors; /* the knob colours stay the same on every page */
static float md_wave(unsigned w, float u, float width)
{
    float saw = 1.0f - 2.0f * u, tri = 1.0f - 4.0f * (u < 0.5f ? 0.5f - u : u - 0.5f);
    return w == 0 ? saw : w == 1 ? 0.5f * (saw + tri) : w == 2 ? (u < width ? 1.0f : -1.0f) : tri;
}
/* FAQ (INFO > ALGORITHM): how the firmware is played, a page per subject,
 * SELECT turns the pages round. Russian comes from app/lang_ru.h line by line. */
enum { FAQ_PAGES = 10, FAQ_LINES = 7 };
static unsigned faq_page;
/* "KEY|TEXT" lines draw the key as an FM-1 button cap, then what it does. */
static const char *const faq_text[FAQ_PAGES][1 + FAQ_LINES] = {
    {"BUTTONS", "HOME|ONE STEP BACK", "HOME|HOLD: SCOPE", "SEQ|STEPS > PAGES  HOLD: CHAIN", "EDIT|SOUND  HOLD: FM LAB",
     "FX|TRACK FX  HOLD: MASTER", "LFO|LFO 1 AND 2", "ARP|ARPEGGIATOR"},
    {"MORE BUTTONS", "GLO|SETTINGS", "SEL|COPY / PASTE", "SAVE|SAVE THE PROJECT", "PLAY|START / STOP",
     "REC|RECORD", "OCT-|OCTAVE DOWN", "OCT+|OCTAVE UP"},
    {"SELECT AND KNOBS", "SELECT: WHERE (TRACK, STEP,", "ROW, PAGE)", "KNOBS 1-4: WHAT YOU SEE", "COLOURS MATCH THE BARS",
     "ALGORITHM: OPENS A PAGE", "OR MOVES BY ROWS", "PRESETS: ENGINE / LENGTH"},
    {"MIXER", "SELECT: TRACK", "PRESETS: ENGINE", "ALGORITHM: SOUND", "K1 VOLUME  K2 FILTER", "K3 SEND  K4 ON/MUTE/SOLO", "", ""},
    {"SEQUENCER", "SELECT: STEP  ALGORITHM: ROW", "K1 VELOCITY  K2 LENGTH", "K3 CHANCE  K4 NOTE", "KEYS WRITE THE NOTE",
     "REC: LIVE WHILE PLAYING,", "STEP BY STEP WHEN STOPPED", "HOLD A KEY: A LONG NOTE"},
    {"SOUND (EDIT)", "SELECT: PAGES (ROUND)", "DX7: TONE / ENV WINDOWS", "HOLD EDIT: FM LAB", "808 / SAMPLER: SELECT PAD,",
     "ALGORITHM: PAGES", "SAMPLER: REC RECORDS A PAD", ""},
    {"FX AND LFO", "FX: SELECT EFFECT, K1-K4 IT", "MIX 0 = OFF", "LFO: SELECT LFO 1 / 2", "K1 TARGET  K2 RATE",
     "K3 DEPTH  K4 WAVE", "", ""},
    {"SAVE", "SELECT: SLOT", "ALGORITHM: LETTER PLACE", "K1 0-9  K2 A-M  K3 N-Z", "K4 SAVE / LOAD",
     "SAVE BUTTON DOES IT", "CHANGES SAVE THEMSELVES", "3 S AFTER, NOT WHILE PLAYING"},
    {"GLOBAL", "SELECT: THE ROW", "KNOBS: ITS VALUES", "ALGORITHM: OPENS ITS PAGE", "(BLUETOOTH, SCOPE, INFO)",
     "SAVE: SAVES", "", ""},
    {"UPDATE AND SAFE START", "THE UPDATE OFTEN NEEDS", "A SECOND TRY: SAME PAGE,", "CONNECT AND START AGAIN",
     "HOLD OCT- AT POWER ON:", "SAFE START, NO PROJECTS", "", ""},
};
/* The largest of 17 / 15 / 13 px that fits the width. */
static const struct lcd_font *faq_font(const char *s, unsigned width)
{
    return lcd_width(&lcd17, s) <= width ? &lcd17 : lcd_width(&lcd15, s) <= width ? &lcd15 : &lcd13;
}
/* v11: the window takes everything under the header (no knobs here),
 * text 17 px, buttons drawn as key caps. */
static void faq_draw(void)
{
    uint32_t sig = faq_page | (lang_get() << 8) | 0xfa000U;
    if (cache->title_cache == sig) return;
    cache->title_cache = sig;
    paint_fill(42, 26, 198, 36, BACK);
    for (unsigned i = 0; i < FAQ_PAGES; ++i) { /* v13: squares 1..10, 17 x 34 on a 19.2 px pitch */
        unsigned x = 43U + i * 192U / 10U;
        char num[3]; decimal(num, (int)i + 1, 1);
        rbox(x, HDR_FY, 17, 34, 3, 0x10a3, 0x5acb);
        lcd_center((int)x + 8, HDR_FY + 10, num, &lcd13, page_icon_colors[i % 6U], 0x10a3);
        if (i == faq_page) glass_cursor(x, HDR_FY, 17, 34, 8, MD_AMBER);
    }
    paint_fill(0, 62, 240, 178, BACK);
    rbox(4, 66, 232, 170, 3, MD_PANEL, MD_EDGE);
    lcd_center(120, 67, faq_text[faq_page][0], &lcd20, knob_colors[faq_page % 4U], MD_PANEL);
    for (unsigned i = 0; i < FAQ_LINES; ++i) {
        const char *line = faq_text[faq_page][1 + i], *bar = strchr(line, '|');
        unsigned y = 96 + i * 20;
        if (!line[0]) continue;
        if (!bar) { lcd_center(120, y, line, faq_font(line, 222), TEXT, MD_PANEL); continue; }
        char key[8]; unsigned n = (unsigned)(bar - line) < 7U ? (unsigned)(bar - line) : 7U;
        memcpy(key, line, n); key[n] = 0;
        ++keep_names; /* button names stay as printed on the FM-1 */
        rbox(12, y - 1, 46, 17, 3, 0x3186, 0x6b4d);
        lcd_center(35, y + 1, key, &lcd13, TEXT, 0x3186);
        --keep_names;
        lcd_text(66, y, bar + 1, faq_font(bar + 1, 164), TEXT, MD_PANEL);
    }
}
/* INFO: what this firmware is and how the synth is doing, centred, the
 * important things large. RAM = the largest block the heap can still give. */
#ifndef FM1_HOST_TEST
/* Read only, no allocation (a malloc probe briefly took the whole heap from
 * USB/BT). The library heap is dlmalloc 2.8.6 (32-byte chunks) over
 * [HEAP_BEGIN, HEAP_END), grown by sbrk. Its state is a private static: found
 * once by its fingerprint (least_addr = seg.base = HEAP_BEGIN, empty bins
 * point at themselves), then a few words are read. Offsets in words. */
extern char HEAP_BEGIN[], HEAP_END[];
enum { GM_TREEMAP = 1, GM_DVSIZE = 2, GM_TOPSIZE = 3, GM_LEAST = 4, GM_TOP = 6, GM_BINS = 10,
       GM_TREEBINS = 76, GM_SEG_BASE = 113, GM_SEG_SIZE = 114, GM_WORDS = 117 };
static unsigned heap_free(void)
{
    static const volatile uint32_t *gm;
    const uint32_t lo = (uint32_t)(uintptr_t)HEAP_BEGIN, hi = (uint32_t)(uintptr_t)HEAP_END;
#define IN_HEAP(a) ((a) >= lo && (a) < hi)
    for (const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)(lo & ~0xfffffU);
         !gm && p + GM_WORDS <= (const volatile uint32_t *)(uintptr_t)lo; ++p) {
        if (p[GM_LEAST] != lo || p[GM_SEG_BASE] != lo || !IN_HEAP(p[GM_TOP])) continue;
        unsigned ok = 1;
        for (unsigned i = 0; i < 32 && ok; ++i) { /* bin i: fd, bk = itself (empty) or chunks */
            uint32_t self = (uint32_t)(uintptr_t)(p + GM_BINS + 2 * i), fd = p[GM_BINS + 2 * i + 2], bk = p[GM_BINS + 2 * i + 3];
            ok = (fd == self && bk == self) || (IN_HEAP(fd) && IN_HEAP(bk));
        }
        if (ok) gm = p;
    }
    if (!gm) return 0;
    /* top chunk plus the room sbrk has not handed out yet, the designated
     * victim, and the largest tree chunk (highest bin, larger child first) */
    uint32_t seg_end = gm[GM_SEG_BASE] + gm[GM_SEG_SIZE], best = gm[GM_TOPSIZE] + (seg_end < hi ? hi - seg_end : 0U);
    if (gm[GM_DVSIZE] > best && gm[GM_DVSIZE] < hi - lo) best = gm[GM_DVSIZE];
    uint32_t map = gm[GM_TREEMAP];
    if (map) {
        unsigned i = 31; while (!(map >> i)) --i;
        uint32_t t = gm[GM_TREEBINS + i];
        for (unsigned depth = 0; depth < 34 && IN_HEAP(t) && !(t & 7U); ++depth) {
            const volatile uint32_t *c = (const volatile uint32_t *)(uintptr_t)t;
            uint32_t size = c[1] & ~7U;
            if (size > best && size < hi - lo) best = size;
            t = c[5] ? c[5] : c[4]; /* child[1] holds the larger sizes */
        }
    }
#undef IN_HEAP
    return best;
}
#else
static unsigned heap_free(void)
{
    unsigned lo = 0, hi = 256U * 1024U;
    while (hi - lo > 1024U) {
        unsigned mid = (lo + hi) / 2U; void *p = malloc(mid);
        if (p) { free(p); lo = mid; } else hi = mid;
    }
    return lo;
}
#endif
/* Audio load in percent of one block, for the whole audio interrupt: the
 * engine's render plus our mix (MICRO D, master FX, BINAURAL, USB). Now =
 * the highest whole-interrupt block of the last ~0.5 s (the load guard looks
 * at every block, so one sampled block would read low). Peak = the highest
 * whole-interrupt block since start (fm1_audio_stats). Host builds have no
 * audio clock: the engine's figures only. */
static unsigned audio_load(unsigned peak)
{
    struct fm1_render_stats st; groove_audio_stats(&st);
    unsigned dl = st.deadline_ticks ? st.deadline_ticks : 1U;
    uint32_t block = fm1_output_block_ticks();
    if (!peak && block) return (unsigned)((uint64_t)fm1_output_peak_ticks() * 100U / block);
    if (peak) {
        if (!block) return st.peak_ticks * 100U / dl;
        struct fm1_audio_stats a; fm1_audio_stats_read(&a);
        return (unsigned)((uint64_t)a.peak_ticks * 100U / block);
    }
    unsigned pc = st.render_ticks / (dl / 100U + 1U);
    if (block) pc += (unsigned)((uint64_t)fm1_output_mix_ticks() * 100U / block);
    return pc;
}
static unsigned battery_percent(void);
/* v12 list row 220 x h: name left, value right (the chosen row lit amber). */
static void list_row(unsigned y, unsigned h, unsigned sel, const char *left, const char *right, uint16_t rc, uint16_t lc)
{
    uint16_t bg = sel ? 0x1081 : MD_PANEL;
    rbox(10, y, 220, h, 2, bg, sel ? MD_AMBER : MD_EDGE);
    unsigned ty = y + (h - 12U) / 2U - 1U;
    if (left && *left) lcd_text(18, ty, left, &lcd17, sel ? MD_AMBER : lc, bg);
    if (right && *right) lcd_text(222 - (int)lcd_width(&lcd17, right), ty, right, &lcd17, rc, bg);
}
/* The audio load as a state plate (GLOBAL, INFO). */
static void cpu_plate(void)
{
    unsigned now_pc = audio_load(0);
    if (now_pc > 99) now_pc = 99;
    if (cache->middle_cache == now_pc + 1U) return;
    cache->middle_cache = now_pc + 1U;
    char num[12]; decimal(num, (int)now_pc, 1); strcat(num, "%");
    state_plate_cap(num, "CPU");
}
static void info_draw(void)
{
    static unsigned ram_kb, when;
    if (!paint_rendering && (!when || frame_time - when > 2000U)) { ram_kb = heap_free() / 1024U; when = frame_time | 1U; }
    unsigned peak = audio_load(1);
    unsigned pct = battery_shown > 100U ? battery_percent() : battery_shown;
    uint32_t sig = ram_kb | (peak << 12) | (pct << 20) | (lang_get() << 28);
    if (cache->title_cache != sig) {
        cache->title_cache = sig;
        paint_fill(0, 62, 240, 133, BACK);
        char val[8][20], num[12];
        ++keep_names;
        strcpy(val[0], "V1 " FM1_VERSION_TAG);
        strcpy(val[1], FM1_BUILD_ID);
        --keep_names;
        decimal(val[2], (int)ram_kb, 1); strcat(val[2], " KB");
        decimal(val[3], (int)peak, 1); strcat(val[3], " %");
        if (last_peak == ~0U) strcpy(val[4], "--"); else { decimal(val[4], (int)last_peak, 1); strcat(val[4], " %"); }
        decimal(num, (int)pct, 1); strcpy(val[5], num); strcat(val[5], " %");
        strcpy(val[6], "NGVGN");
        strcpy(val[7], "YARIKFANSEN");
        static const char *const names[8] = {"FIRMWARE", "BUILD", "RAM FREE", "CPU PEAK", "LAST RUN PEAK", "BATTERY", "CREATOR", "KO-FI"};
        unsigned bar[8] = {0, 0, 0, peak > 100U ? 100U : peak, last_peak == ~0U ? 0U : last_peak > 100U ? 100U : last_peak, pct, 0, 0};
        /* v13: 8 rows of 14 px on a 15.5 px pitch; CPU PEAK / LAST RUN PEAK / BATTERY carry a glass bar */
        for (unsigned i = 0; i < 8; ++i) {
            unsigned y = 67U + i * 31U / 2U;
            rbox(10, y, 220, 14, 3, MD_PANEL, MD_EDGE);
            ++keep_names;
            lcd_text(16, y + 1, names[i], &lcd12, V_DIM, MD_PANEL);
            lcd_text(224 - (int)lcd_width(&lcd14, val[i]), y, val[i], &lcd14, i == 6 ? PAL_YELLOW : i == 7 ? PAL_PINK : TEXT, MD_PANEL);
            --keep_names;
            if (i >= 3 && i <= 5) glass_bar_h(118, y + 4, 52, 7, bar[i] * 256U / 100U, bar[i] > 70U ? PAL_GREEN : bar[i] > 40U ? PAL_YELLOW : PAL_TURQ);
        }
    }
    cpu_plate();
    knob(0, "FAQ", "ALGO >", colors[0]); knob(1, "", "", colors[1]); knob(2, "", "", colors[2]); knob(3, "", "", colors[3]);
}
static float pic_sin(float cycles) { return vsin(cycles - (float)(int)cycles); }
#include "screen_sound.inc"
#include "screen_sound2.inc"
static void radio_line(unsigned slot, unsigned y, const char *value, uint16_t color)
{
    uint32_t sig = signature(color, value, (unsigned)strlen(value));
    if (cache->tile_cache[slot] == sig) return;
    cache->tile_cache[slot] = sig;
    paint_fill(8, y, 224, 7, BACK);
    text(8, y, value, 1, color, BACK);
}
static __attribute__((noinline)) void draw_ble(const struct fm1_ble_status *status)
{
    static const char *const states[] = {"OFF", "IDLE", "WAITING FOR DEVICE",
        "SCANNING", "CONNECTING", "FINDING MIDI", "CONNECTED", "MIDI READY", "DISCONNECTING"};
    static const char *const errors[] = {
        "", "STACK BUSY", "TIMED OUT", "NOT BLE MIDI", "GATT FAILED", "LINK LOST"};
    unsigned on = (frame_radio_flags & GROOVE_RADIO_ON) != 0;
    unsigned midi = frame_radio_flags & GROOVE_RADIO_MIDI ? 1U + frame_radio_role : 0;
    if (cache->title_cache != ble_view) {
        cache->title_cache = ble_view;
        memset(cache->row_cache, 0xff, sizeof cache->row_cache);
        memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
        paint_fill(0, 44, 240, 196, BACK);
    }
    const char *state = !on ? "BLUETOOTH OFF" : !frame_radio.ready ? "RADIO AVAILABLE ON DEVICE"
        : status->pending ? "APPLYING SETTINGS"
        : !midi ? "MIDI OFF" : status->state == FM1_BLE_OFF ? "STARTING MIDI"
        : status->state <= FM1_BLE_DISCONNECTING ? states[status->state] : "UNKNOWN";
    char progress[40];
    if (on && frame_radio.ready && (status->pending || status->state == FM1_BLE_SCANNING ||
        status->state == FM1_BLE_CONNECTING || status->state == FM1_BLE_DISCOVERING)) {
        unsigned n = (unsigned)strlen(state);
        memcpy(progress, state, n);
        progress[n++] = ' ';
        decimal(progress + n, (int)ble_seconds, 1);
        n = (unsigned)strlen(progress);
        progress[n++] = 'S'; progress[n] = 0;
        state = progress;
    }
    if (!ble_view) {
        radio_line(0, 64, state, on ? GREEN : DIM);
        static const char *const labels[] = {"K1 BLUETOOTH", "K2 BT AUDIO", "K3 BLE ROLE", "K4 CLOCK"};
        unsigned values[] = {on, (frame_radio_flags & GROOVE_RADIO_AUDIO) != 0, midi, groove_clock_mode()};
        const char *const names[] = {on ? "ON" : "OFF", values[1] ? "ON" : "OFF",
            midi == 2 ? "HOST" : midi == 1 ? "DEVICE" : "OFF", values[3] ? "MIDI" : "INT"};
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t sig = values[i] | (on << 8);
            if (cache->row_cache[i] == sig) continue;
            cache->row_cache[i] = sig;
            unsigned y = 76 + i * 26;
            paint_fill(8, y, 224, 20, BACK);
            text(8, y + 4, labels[i], 1, TEXT, BACK);
            text(146, y, names[i], 2, i && i < 3 && !on ? DIM : ACCENT, BACK);
        }
        const char *audio = !on || !values[1] ? "AUDIO OFF" : frame_radio.audio_playing ? "AUDIO PLAYING"
            : frame_radio.audio_connected ? "AUDIO CONNECTED" : "AUDIO WAITING FOR CONNECTION";
        radio_line(1, 184, audio, frame_radio.audio_playing ? GREEN : DIM);
        radio_line(2, 196, ble_action ? "> FORGET MIDI PAIRS" : "> MIDI DEVICE LIST (HOST)", ACCENT);
        const char *error = ble_forget == 1 ? "FORGET MIDI PAIRS? SAVE YES / HOME NO"
            : ble_forget == 2 && !status->pending && !status->error ? "PAIR AGAIN; FORGET FM1 ON PEER TOO"
            : groove_settings_status() ? "SETTINGS NOT SAVED - RETRY"
            : ble_result == -2 ? "BUSY - RETRY" : ble_result == -3 ? "RADIO AVAILABLE ON DEVICE"
            : ble_result ? "COMMAND UNAVAILABLE"
            : status->error <= FM1_BLE_LINK_LOST ? errors[status->error] : "";
        radio_line(3, 213, error, AMBER);
        radio_line(4, 229, "PRESETS ACTION  SAVE OK  HOME EXIT", DIM);
        return;
    }
    /* v9 device list: SELECT a device; KNOB 1 CONNECT, 2 SCAN, 3 DISCONNECT
     * (turn right). The settings are in GLOBAL > BLUETOOTH. */
    { uint32_t ps = 0xb1e00000U | on; if (cache->middle_cache != ps) { cache->middle_cache = ps; state_plate_cap(on ? "ON" : "OFF", "BT"); } }
    knob(0, "CONNECT", "", colors[0]); knob(1, "SCAN", "", colors[1]); knob(2, "DISC", "", colors[2]); knob(3, "", "", colors[3]); /* actions, not pages: no arrows */
    if (paint_rendering && (paint_top >= 195 || paint_top + PAINT_ROWS <= 62)) return;
    unsigned count = status->count > FM1_BLE_MAX_DEVICES ? FM1_BLE_MAX_DEVICES : status->count;
    unsigned first;
    const char *error = groove_settings_status() ? "SETTINGS NOT SAVED - RETRY"
        : ble_result == -2 ? "BUSY - RETRY" : ble_result == -3 ? "RADIO AVAILABLE ON DEVICE"
        : ble_result ? "COMMAND UNAVAILABLE"
        : status->error <= FM1_BLE_LINK_LOST ? errors[status->error] : "";
    uint32_t sig = signature(signature(signature(count | (ble_device << 8) | (ble_action << 16) | (on << 20), state, (unsigned)strlen(state)),
                                       frame_devices, sizeof frame_devices), error, (unsigned)strlen(error));
    if (cache->row_cache[0] == sig) return;
    cache->row_cache[0] = sig;
    paint_fill(0, 62, 240, 133, BACK);
    rbox(6, 66, 228, 124, 3, MD_PANEL, MD_EDGE); /* v13: the window, four rows 220 x 26 on a 29 px pitch, the chosen one glass */
    first = ble_device > 3 ? ble_device - 3 : 0;
    unsigned shown = 0;
    for (unsigned k = 0; k < 4; ++k) {
        unsigned i = first + k, y = 71 + k * 29;
        if (i >= count) break;
        const struct fm1_ble_device *device = &frame_devices[i];
        unsigned sel = i == ble_device, linked = sel && (status->state == FM1_BLE_CONNECTED || status->state == FM1_BLE_READY);
        rbox(10, y, 220, 26, 3, MD_PANEL, MD_EDGE);
        ++keep_names;
        lcd_text(18, y + 2, device->name[0] ? device->name : "UNNAMED MIDI DEVICE", &lcd20, TEXT, MD_PANEL);
        const char *tag = linked ? "CONNECTED" : "FOUND";
        lcd_text(222 - (int)lcd_width(&lcd14, tag), y + 6, tag, &lcd14, linked ? PAL_GREEN : PAL_AMBER, MD_PANEL);
        --keep_names;
        if (sel) glass_cursor(10, y, 220, 26, 8, MD_AMBER);
        ++shown;
    }
    if (shown < 4) { /* the radio's state (and any error) as the next row: SCANNING, NO DEVICES - SCAN ... */
        unsigned y = 71 + shown * 29;
        rbox(10, y, 220, 26, 3, MD_PANEL, MD_EDGE);
        const char *line = error[0] ? error : status->state == FM1_BLE_SCANNING ? "SCANNING" : !count ? "NO DEVICES - SCAN" : state;
        lcd_text(18, y + 2, line, lcd_width(&lcd20, line) <= 204U ? &lcd20 : &lcd14, error[0] ? V_ORANGE : PAL_YELLOW, MD_PANEL);
    }
}
/* v6 SAVE: 16 slot squares (gold = stored, yellow = this project, white frame
 * = chosen), the working name with its letter cursor, the chosen slot's stored
 * name and the state. KNOB 1 slot, KNOB 2 cursor, KNOB 3 letter, SAVE writes. */
static __attribute__((noinline)) void project_draw(void)
{
    if (project_drawn_revision != groove_project_revision())
        project_refresh();
    slot_scan();
    char name[13];
    groove_project_name(name);
    unsigned current = groove_project_slot();
    if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 26))) {
        uint32_t sig = signature(project_slot | (current << 8) | (project_reload << 16), project_stored, 12);
        sig = signature(sig, &seq_slots_used, sizeof seq_slots_used);
        sig = signature(signature(sig, &project_stored_result, sizeof project_stored_result), name, 12) + name_cursor;
        sig = signature(sig, &project_result, sizeof project_result) + project_operation * 7U + project_action * 131U;
        if (cache->title_cache != sig) {
            cache->title_cache = sig;
            /* v10: the important things large and centred: the slot, the
             * name in a big window (its letter under the cursor blinking),
             * then SAVE "to" a tick (or the question / result). */
            paint_fill(0, 62, 240, 130, BACK);
            paint_fill(42, 26, 198, 36, BACK);
            /* v13: the slot in the state plate, a window with the 16 slots */
            { char sn[12]; decimal(sn, (int)project_slot + 1, 1); state_plate_cap(sn, "SLOT"); }
            { /* C3 (mockup f): what the chosen cell holds, large under the SAVE title, left of the slot plate */
                char held[16];
                if (project_stored_result) strcpy(held, project_stored_result < 0 ? "READ ERROR" : "EMPTY");
                else { memcpy(held, project_stored, 12); held[12] = 0; for (unsigned n = 12; n && held[n - 1] == ' '; ) held[--n] = 0; }
                const struct lcd_font *f = project_stored_result ? &lcd26 : &lcd30;
                if (lcd_width(f, held) > 140U) f = &lcd24;
                if (lcd_width(f, held) > 140U) f = &lcd20;
                lcd_center(112, 44U - f->height / 2U, held, f, project_stored_result ? V_DIM : knob_colors[2], BACK);
            }
            rbox(6, 66, 228, 124, 2, MD_PANEL, MD_EDGE);
            for (unsigned i = 0; i < 16; ++i) {
                unsigned x = 14 + (i % 8) * 27, y = 72 + (i / 8) * 19;
                unsigned used = i < GROOVE_PROJECT_SLOTS && (seq_slots_used >> i & 1U), on = i == current;
                uint16_t fill = on ? MD_AMBER : used ? 0x1b47 : MD_PANEL; /* user 13.11: occupied = green (mockup #1d6b3c) */
                rbox(x, y, 24, 16, 2, fill, used || on ? 0x3f0f : MD_EDGE); /* light green edge #3ee07a; no numbers, no stripes */
            }
            for (unsigned i = 0; i < 16; ++i) /* the glass cursor goes over everything */
                if (i == project_slot) glass_cursor(14 + (i % 8) * 27, 72 + (i / 8) * 19, 24, 16, 5, MD_AMBER);
            lcd_center(120, 112, name, &lcd30, TEXT, MD_PANEL);
            char prefix[13]; memcpy(prefix, name, name_cursor); prefix[name_cursor] = 0;
            char letter[2] = {name[name_cursor] && name[name_cursor] != ' ' ? name[name_cursor] : 'W', 0};
            int nx = 120 - (int)lcd_width(&lcd30, name) / 2 + (int)lcd_width(&lcd30, prefix);
            paint_fill((unsigned)nx, 143, lcd_width(&lcd30, letter), 3, knob_colors[1]); /* v13: right under the name (mockup 142) */
            char load_q[24] = "LOAD SLOT ";
            decimal(load_q + 10, (int)project_slot + 1, 1); strcat(load_q, "? SAVE = YES");
            const char *status = project_reload == 3 ? load_q
                         : project_reload ? "OVERWRITE? SAVE = YES, HOME = NO"
                         : project_action ? "SAVE = LOAD"
                         : project_result == GROOVE_PROJECT_EMPTY     ? "NO VALID SAVE"
                         : project_result == GROOVE_PROJECT_TUNING_SPACE ? "REDUCE SAMPLE BANK"
                         : project_result == GROOVE_PROJECT_MEMORY  ? "NOT ENOUGH MEMORY"
                         : project_result == GROOVE_PROJECT_INVALID ? "INVALID PROJECT"
                         : project_result == FM1_FLASH_BUSY         ? "FLASH BUSY - RETRY"
                         : project_result == FM1_FLASH_UNAVAILABLE  ? "FLASH NOT READY"
                         : project_result                           ? "FLASH ERROR - RETRY"
                         : project_operation == 1                   ? "SAVED"
                         : project_operation == 3                   ? "SAVING"
                                                                    : "SAVE = WRITE";
            if (project_reload || project_result || project_operation == 1 || project_operation == 3)
                lcd_center(120, 168, status, &lcd17, project_reload || project_result ? V_ORANGE : knob_colors[1], MD_PANEL);
            else /* v13: what the SAVE button does now; KNOB 4 chooses SAVE or LOAD */
                lcd_center(120, 160, project_action ? "SAVE = LOAD" : "SAVE = SAVE", &lcd24, project_action ? knob_colors[3] : knob_colors[1], MD_PANEL);
        }
    }
    /* each letter knob shows the letter under the cursor when it is in its group */
    int grp = name_group(name[name_cursor]);
    char letter[2] = {name[name_cursor] == ' ' ? '_' : name[name_cursor], 0};
    knob(0, "0-9", grp == 0 ? letter : "-", colors[0]);
    knob(1, "A-M", grp == 1 ? letter : "-", colors[1]);
    knob(2, "N-Z", grp == 2 ? letter : "-", colors[2]);
    knob(3, "ACTION", project_action ? "LOAD" : "SAVE", colors[3]);
}
static __attribute__((noinline)) void name_draw(void)
{
    /* v13: the patch name as ten letter boxes (the glass cursor on the one SELECT moved to), SAVE = OK / HOME = CANCEL in one row */
    const uint8_t *patch = frame_patch;
    char name[11];
    for (unsigned i = 0; i < 10; ++i)
        name[i] = patch[145 + i] >= 32 && patch[145 + i] <= 126 ? (char)patch[145 + i] : '?';
    name[10] = 0;
    char letter[2] = {name[name_cursor] == ' ' ? '_' : name[name_cursor], 0};
    int grp = name_group(name[name_cursor]);
    knob(0, "0-9", grp == 0 ? letter : "", colors[0]);
    knob(1, "A-M", grp == 1 ? letter : "", colors[1]);
    knob(2, "N-Z", grp == 2 ? letter : "", colors[2]);
    knob(3, "", "", colors[3]);
    if (paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 26)) return;
    uint32_t sig = signature(name_cursor | ((unsigned)(patch[134] & 31U) << 8), patch + 145, 10);
    if (cache->middle_cache == sig)
        return;
    cache->middle_cache = sig;
    paint_fill(0, 62, 240, 133, BACK);
    char num[12]; decimal(num, (int)(patch[134] & 31U) + 1, 1);
    state_plate_cap(num, "ALGO");
    rbox(6, 66, 228, 124, 2, MD_PANEL, MD_EDGE);
    for (unsigned i = 0; i < 10; ++i) {
        unsigned x = 14 + i * 21;
        char one[2] = {name[i], 0};
        rbox(x, 92, 19, 32, 2, 0x18a3, MD_EDGE);
        if (name[i] != ' ') lcd_center((int)x + 9, 98, one, &lcd20, TEXT, 0x18a3);
        if (i == name_cursor) glass_cursor(x, 92, 19, 32, 5, MD_AMBER);
    }
    lcd_center(120, 158, "SAVE = OK", &lcd20, knob_colors[1], MD_PANEL); /* HOME leaves as before, unlabelled (user 07.11) */
}
/* Rows 0..6: master inserts (EQ last). Rows 7..8: the engine's delay and
 * reverb, which every track feeds through its own sends. */
enum { FX_ROWS = 10, FX_DELAY = 7, FX_REVERB = 8, FX_COMP = 9, FX_VX = 14, FX_VY = 108, FX_VW = 212, FX_VH = 78 };
/* The effect's caption and name, drawn over its picture (no box behind them). */
static char fx_caption[24];
static const char *fx_title = "";
/* user 13.11 (mockup FM1-fx-cleanup s.3): the plates' order. Page 2 = REVERB, EQ, COMP, REV SEND, DLY SEND;
 * the row numbers (and everything keyed on them) stay. */
static const uint8_t fx_order[FX_ROWS] = {0, 1, 2, 3, 4, 5, 6, FX_COMP, FX_REVERB, FX_DELAY};
static unsigned fx_pos(unsigned row)
{
    unsigned p = 0;
    while (p + 1U < FX_ROWS && fx_order[p] != row) ++p;
    return p;
}
/* Knob -> parameter column. FILTER (user 13.11, mockup s.7): K1 CUTOFF, K2 RESO, K3 TYPE, K4 MIX. */
static unsigned fx_col(unsigned row, unsigned knob_no)
{
    static const uint8_t filter[4] = {1, 2, 0, 3};
    return row == MFX_FILTER ? filter[knob_no & 3U] : knob_no;
}
static const char *fx_label(unsigned row, unsigned col)
{
    static const char *const delay[4] = {"TIME", "FEEDBK", "-", "RETURN"};
    static const char *const reverb[4] = {"-", "DECAY", "-", "RETURN"};
    static const char *const comp[4] = {"THRESH", "RATIO", "SPEED", "MIX"};
    return row < FX_DELAY ? mfx_label(row, col) : row == FX_DELAY ? delay[col] : row == FX_COMP ? comp[col & 3U] : reverb[col];
}
static float fexp2_ui(float x) /* 2^x for display maths */
{
    float r = 1.0f;
    if (!(x > -40.0f)) x = -40.0f; /* also catches NaN: loops below stay bounded */
    if (x > 40.0f) x = 40.0f;
    while (x >= 1.0f) { r *= 2.0f; x -= 1.0f; }
    while (x < 0.0f) { r *= 0.5f; x += 1.0f; }
    return r * (1.0f + x * (0.6931472f + x * (0.2402265f + x * 0.0555041f)));
}
/* 20..999 Hz as digits, then 1.0K..9.9K, then 10K.. */
static void format_hz(char out[12], unsigned hz)
{
    if (hz < 1000U) decimal(out, (int)hz, 1);
    else if (hz < 10000U) {
        unsigned tenths = (hz + 50U) / 100U;
        out[0] = (char)('0' + tenths / 10U); out[1] = '.'; out[2] = (char)('0' + tenths % 10U); out[3] = 'K'; out[4] = 0;
    } else { decimal(out, (int)((hz + 500U) / 1000U), 1); strcat(out, "K"); }
}
/* The shared delay's time as TRACK FX > SHARED FX shows it: the note division when synced, else the applied
 * time in ms (code = the engine's delay value, effects byte 1). Also MASTER FX > DELAY K1 (user 10.11, F6). */
static void delay_time_text(char out[12], unsigned code)
{
    static const char *const division[] = {"1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/8D", "1/4"};
    if (groove_delay_synced(code)) strcpy(out, division[code - GROOVE_DELAY_SYNC]);
    else decimal(out, (int)((groove_delay_ticks() * 80U + 220U) / 441U), 1);
}
static void fx_value(unsigned row, unsigned col, char out[12])
{
    uint8_t e[4]; groove_effects_get(e);
    if (row == MFX_FILTER && col == 1) {
        int hz = mfx_cutoff_hz();
        if (!hz) strcpy(out, "OFF");
        else if (hz < 0) { out[0] = 'H'; format_hz(out + 1, (unsigned)-hz); }
        else if (mfx_value(MFX_FILTER, 0) == 4) { out[0] = 'L'; format_hz(out + 1, (unsigned)hz); } /* DJ */
        else format_hz(out, (unsigned)hz);
        return;
    }
    if (row == MFX_EQ && col) { /* gain in dB, 64 = 0 */
        int db = ((int)mfx_value(row, col) - 64) * 12 / 63;
        decimal(out, db, 1);
        if (db > 0) { memmove(out + 1, out, strlen(out) + 1); out[0] = '+'; }
        strcat(out, "DB"); return;
    }
    if (row == MFX_RING && col == 1) { /* 1 Hz .. 8 kHz, TRACK: ratio to the played pitch */
        float v = (float)mfx_value(row, 1) / 127.0f;
        if (mfx_value(row, 0) == 5) {
            unsigned r100 = (unsigned)(fexp2_ui((v - 0.5f) * 4.0f) * 100.0f + 0.5f);
            out[0] = 'X'; out[1] = (char)('0' + r100 / 100U); out[2] = '.';
            out[3] = (char)('0' + r100 / 10U % 10U); out[4] = (char)('0' + r100 % 10U); out[5] = 0;
        } else format_hz(out, (unsigned)(fexp2_ui(v * 13.0f) + 0.5f));
        return;
    }
    if (row == MFX_VERB && col == 2) { decimal(out, (int)(mfx_value(row, 2) * 100U / 127U), 1); strcat(out, "MS"); return; }
    if (row == FX_COMP) {
        static const char *const ratio[4] = {"2:1", "4:1", "8:1", "20:1"}, *const speed[3] = {"FAST", "MED", "SLOW"};
        unsigned v = mfx_comp_value(col);
        if (!col) { decimal(out, -2 * (int)v, 1); strcat(out, "DB"); }
        else if (col == 1) strcpy(out, ratio[v & 3U]);
        else if (col == 2) strcpy(out, speed[v % 3U]);
        else decimal(out, (int)v, 1);
        return;
    }
    if (row < FX_DELAY) {
        if (!col) { strncpy(out, mfx_type_name(row), 11); out[11] = 0; }
        else decimal(out, (int)mfx_value(row, col), 1);
    } else if (row == FX_DELAY) { /* user 10.11 (F6): K1 the time as SHARED FX shows it, K2 / K4 in % */
        if (col == 2) strcpy(out, "--");
        else if (!col) delay_time_text(out, e[1]);
        else { decimal(out, (int)(e[col == 1 ? 2 : 3] * 100U / 127U), 1); strcat(out, "%"); }
    } else {
        if (!col || col == 2) strcpy(out, "--");
        else decimal(out, e[col == 1 ? 0 : 3], 1);
    }
}
static int fx_raw(unsigned row, unsigned col) /* 0..127 for bars, -1 when unused */
{
    uint8_t e[4]; groove_effects_get(e);
    if (row < FX_DELAY) return (int)mfx_value(row, col);
    if (row == FX_COMP) return col == 3 ? (int)mfx_comp_value(3) : -1;
    if (row == FX_DELAY) return col == 1 ? e[2] : col == 3 ? e[3] : -1;
    return col == 1 ? e[0] : col == 3 ? e[3] : -1;
}
static unsigned fx_active(unsigned row)
{
    uint8_t e[4]; groove_effects_get(e);
    return row < FX_DELAY ? mfx_active(row) : row == FX_COMP ? mfx_comp_value(3) != 0 : e[3] != 0;
}
/* A small pixel cat for the arpeggiator page. k body, w white, b pupil, p pink. */
static const char *const cat_rows[15] = {
    "................",
    "..k..........k..",
    "..kk........kk..",
    "..kpk......kpk..",
    "..kkkkkkkkkkkk..",
    ".kkkkkkkkkkkkkk.",
    ".kkwbkkkkkkwbkk.",
    ".kkwwkkkkkkwwkk.",
    ".kkkkkkppkkkkkk.",
    ".kkkkwkkkkwkkkk.",
    "..kkkkkkkkkkkk..",
    "...kkkkkkkkkk...",
    "..kkkwwwwwwkkk..",
    "..kkwwwwwwwwkk..",
    "..kk.kk..kk.kk.."};
static void cat_draw(unsigned x, unsigned y, unsigned frame, unsigned blink)
{
    const unsigned s = 4;
    int head_dx = frame == 1 ? -1 : frame == 2 ? 1 : 0, dy = frame == 3 ? -1 : 0;
    for (unsigned r = 0; r < 15; ++r)
        for (unsigned c = 0; c < 16; ++c) {
            char ch = cat_rows[r][c];
            if (ch == '.') continue;
            if (blink && (r == 6 || r == 7) && (ch == 'w' || ch == 'b')) ch = r == 7 ? 'b' : 'k';
            uint16_t col = ch == 'k' ? 0xfbe0 : ch == 'w' ? TEXT : ch == 'p' ? 0xfb56 : 0x0000;
            int dx = r <= 10 ? head_dx : 0;
            paint_fill((unsigned)((int)x + ((int)c + dx) * (int)s), (unsigned)((int)y + ((int)r + dy) * (int)s), s, s, col);
        }
    /* paws: raised on the beat */
    unsigned left = frame == 1 || frame == 3, right = frame == 2 || frame == 3;
    for (unsigned side = 0; side < 2; ++side) {
        unsigned up = side ? right : left;
        int cx = side ? 14 : 0;
        if (up) {
            paint_fill((unsigned)((int)x + cx * 4), (unsigned)((int)y + (8 + dy) * 4), 8, 16, 0xfbe0);
            paint_fill((unsigned)((int)x + cx * 4), (unsigned)((int)y + (7 + dy) * 4), 8, 4, TEXT);
        } else {
            paint_fill((unsigned)((int)x + (side ? 13 : 1) * 4), (unsigned)((int)y + (12 + dy) * 4), 4, 8, 0xfbe0);
        }
    }
}
static void music_note(unsigned x, unsigned y, uint16_t c)
{
    paint_fill(x + 6, y, 2, 12, c);           /* stem */
    paint_fill(x + 6, y, 6, 2, c);            /* flag */
    paint_fill(x + 10, y + 2, 2, 3, c);
    paint_fill(x + 1, y + 10, 6, 4, c);       /* head */
    paint_fill(x, y + 11, 1, 2, c);
}
/* ---- live picture of the selected effect ---- */
static float vlog2(float x) /* log2 for display, x > 0 */
{
    int e = 0;
    if (!(x > 1e-9f)) x = 1e-9f; /* also catches NaN */
    if (x > 1e9f) x = 1e9f;
    while (x >= 2.0f && e < 64) { x *= 0.5f; ++e; }
    while (x < 1.0f && e > -64) { x *= 2.0f; --e; }
    float t = x - 1.0f;
    return (float)e + t * (1.4426950f - t * (0.7213475f - t * 0.4808983f));
}
static float vsqrt(float x)
{
    if (x <= 0) return 0;
    float r = x > 1 ? x : 1;
    for (int i = 0; i < 8; ++i) r = 0.5f * (r + x / r);
    return r;
}
static float vsin(float c) /* sin(2*pi*c) */
{
    c -= (float)(int)c;
    if (c < 0) c += 1;
    float a = (c > 0.5f ? c - 1.0f : c) * 6.2831853f;
    if (a > 1.5707963f) a = 3.1415927f - a;
    else if (a < -1.5707963f) a = -3.1415927f - a;
    float q = a * a;
    return a * (1 - q * (0.1666667f - q * (0.0083333f - q * 0.0001984f)));
}
/* Column x of the picture -> value -1..1 (bottom..top). */
static float fx_curve(unsigned row, unsigned x, float t)
{
    float u = (float)x / (float)(FX_VW - 1);
    if (row == FX_COMP) { /* transfer curve, input -48..0 dB across */
        static const float ratio[4] = {2.0f, 4.0f, 8.0f, 20.0f};
        float in = -48.0f + u * 48.0f, thr = -2.0f * (float)mfx_comp_value(0), r = ratio[mfx_comp_value(1) & 3U];
        float outd = in > thr ? thr + (in - thr) / r : in;
        (void)t; return (outd + 48.0f) / 24.0f - 1.0f;
    }
    float p1 = row < FX_DELAY ? mfx_value(row, 1) / 127.0f : 0, p2 = row < FX_DELAY ? mfx_value(row, 2) / 127.0f : 0;
    float mix = row < FX_DELAY ? mfx_value(row, 3) / 127.0f : 0;
    unsigned type = row < FX_DELAY ? mfx_value(row, 0) : 0;
    if (row == MFX_EQ) { /* rough response over 20 Hz..20 kHz, +-12 dB */
        static const float mids[5] = {7.64f, 8.64f, 9.64f, 10.64f, 11.64f}; /* log2 of the mid frequencies */
        float lf = 4.32f + u * 10.0f, d;
        float lo = 1.0f / (1.0f + fexp2_ui(2.0f * (lf - 6.9f)));
        float hi = 1.0f / (1.0f + fexp2_ui(2.0f * (12.97f - lf)));
        d = (lf - mids[type % 5]) * 1.4f;
        float mid = 1.0f / (1.0f + d * d);
        return ((p1 - 0.504f) * lo + (p2 - 0.504f) * mid + ((float)mfx_value(row, 3) / 127.0f - 0.504f) * hi) * 1.9f;
    }
    if (row == MFX_FILTER) { /* magnitude over 20 Hz..20 kHz, -34..+22 dB */
        float f = 20.0f * fexp2_ui(u * 10.0f), mag = 1.0f;
        int hz = mfx_cutoff_hz();
        if (type == 3) { /* COMB */
            float d = 44100.0f / (float)hz, fb = p2 * 0.95f, ph = f * d / 44100.0f;
            mag = 0.6f / vsqrt(1.0f - 2.0f * fb * vsin(ph + 0.25f) + fb * fb);
        } else if (hz) {
            float fc = (float)(hz < 0 ? -hz : hz), w = f / fc, k = 2.0f - 1.9f * p2;
            float den = vsqrt((1 - w * w) * (1 - w * w) + k * k * w * w);
            /* ACID / LADDER: 24 dB low-pass; SEM: 12 dB; DJ: low-pass left of centre, high-pass right */
            unsigned mode = type == 4 ? (hz > 0 ? 0U : 2U) : type == 1 ? 0U : 5U;
            mag = mode == 0 || mode == 5 ? 1.0f / den : mode == 1 ? k * w / den : w * w / den;
            if (mode == 5) mag *= 1.0f / vsqrt((1 - w * w) * (1 - w * w) + 2.0f * w * w);
        }
        mag = (1 - mix) + mix * mag;
        float db = 6.0206f * vlog2(mag > 1e-4f ? mag : 1e-4f);
        return (db + 6.0f) / 28.0f; /* -34..+22 dB: room for the full resonance peak (+20 dB) */
    }
    if (row == MFX_DRIVE) { /* transfer curve: input across, output up */
        float xin = u * 2 - 1, gain = 1 + p1 * p1 * 40, tt = xin * gain, y;
        if (type == 0) y = tt / (1 + (tt < 0 ? -tt : tt));
        else if (type == 1) y = tt < -0.7f ? -0.7f : tt > 0.7f ? 0.7f : tt;
        else if (type == 2) y = vsin(tt * 0.25f) * 0.8f;
        else {
            float lv = fexp2_ui(15 - p1 * 12);
            if (lv > 24) lv = 24;
            y = (float)(int)(xin * lv) / lv;
        }
        if (type != 3) y *= 1.6f / (1 + p1 * 3);
        return xin + mix * (y - xin);
    }
    if (row == MFX_CHORUS || row == MFX_PHASER) { /* LFO sweep, moving */
        float rate = 0.05f * fexp2_ui(p1 * 7.0f), depth = row == MFX_CHORUS ? p2 : 0.4f + 0.6f * p2;
        return depth * (0.15f + 0.85f * mix) * vsin(u * 2.0f - t * rate);
    }
    if (row == MFX_RING) { /* carrier wave */
        float cycles = 1.0f + 11.0f * p1, ph = u * cycles - t * 0.5f, a = 0.2f + 0.8f * mix;
        float fr = ph - (float)(int)ph;
        if (fr < 0) fr += 1;
        return a * (type ? (fr < 0.5f ? 1.0f : -1.0f) : vsin(ph));
    }
    if (row == MFX_VERB) { /* the tail: DECAY = its length, smooth and still (user 13.11) */
        float pre = p2 * 0.25f, decay = 0.06f + 0.94f * p1;
        if (u < pre) return -1;
        return -1 + 2 * (0.45f + 0.55f * mix) * fexp2_ui(-(u - pre) * (type == 0 ? 6.0f : 4.0f) / decay);
    }
    uint8_t e[4];
    groove_effects_get(e);
    float ret = e[3] / 127.0f;
    if (row == FX_DELAY) { /* echo taps */
        float spacing = 0.08f + 0.5f * e[1] / 127.0f, fb = e[2] / 127.0f, pos = u / spacing;
        unsigned n = (unsigned)pos;
        if (pos - (float)n > 0.12f) return -1;
        float a = ret;
        for (unsigned i = 0; i < n; ++i) a *= fb;
        return -1 + 2 * a;
    }
    { /* the shared send reverb's tail, smooth and still like the master reverb's */
        float decay = 0.06f + 0.94f * e[0] / 127.0f;
        return -1 + 2 * (0.3f + 0.7f * ret) * fexp2_ui(-u * 4.0f / decay);
    }
}
/* The curve is computed once per change into a small cache (not again for
 * every LCD band), then drawn from the cache. */
static void fx_visual(void)
{
    static int16_t ys[FX_VW];
    static unsigned done = FX_VW, row_key = ~0U;
    static uint32_t want_sig;
    /* user 13.11: no animation on any effect; the picture is redrawn only when a value changes */
    char v[4][12];
    for (unsigned c = 0; c < 4; ++c) fx_value(fx_row, c, v[c]);
    uint32_t sig = signature(fx_row, v, sizeof v);
    if (!paint_rendering) {
        if (sig != want_sig || row_key != fx_row) { want_sig = sig; row_key = fx_row; done = 0; }
        if (done < FX_VW) {
            const float t = 0.0f; /* a still frame */
            unsigned end = FX_VW;
            for (; done < end; ++done) {
                float val = fx_curve(fx_row, done, t);
                if (!(val > -1)) val = -1; /* NaN-safe */
                if (val > 1) val = 1;
                ys[done] = (int16_t)((int)FX_VY + 6 + (int)((1.0f - val) * 0.5f * (FX_VH - 9))); /* 6 px air above the top */
            }
            if (done == FX_VW) cache->middle_shape = ~sig; /* complete: draw it */
        }
    }
    if (paint_rendering && (paint_top >= FX_VY + FX_VH || paint_top + PAINT_ROWS <= FX_VY)) return;
    if (done < FX_VW || cache->middle_shape == sig) return;
    cache->middle_shape = sig;
    if (!paint_rendering) { paint_mark(FX_VX, FX_VY, FX_VW, FX_VH); return; }
    /* user 13.11 (mockup FM1-fx-cleanup s.11): one style for every picture: one green line 2 px, no fill,
     * no zones or markers, one window background */
    paint_fill(FX_VX, FX_VY, FX_VW, FX_VH, 0x0841);
    int prev = -1;
    for (unsigned x = 0; x < FX_VW; ++x) {
        int y = ys[x];
        int a = prev < 0 ? y : prev, lo = a < y ? a : y, hi = a < y ? y : a;
        paint_fill(FX_VX + x, (unsigned)lo, 1, (unsigned)(hi - lo + 2), MX_G);
        prev = y;
    }
    /* v10: the effect's name is in the header, the curve has the box to itself */
}
/* ---- v7 SAMPLER REC / CHOP ---- */
static unsigned smp_ram, smp_ram_at; /* longest take the RAM allows, measured every 0.5 s */
/* The bank's room for a take on this pad, leaving the chunk padding of the
 * slices CHOP may cut it into (pads smp_pad..8). */
static unsigned smp_bank_frames(void) { return rec_bank_fit(groove_samples_room(smp_pad, 1), 8U - smp_pad); }
/* RAM for a take, from the heap's largest free block read in place (no
 * allocation that could briefly take the heap from USB / Bluetooth); the
 * malloc probe only when the heap cannot be read or looks too full. */
static unsigned smp_ram_now(void)
{
    unsigned big = heap_free(), fit = big ? rec_ram_fit(big) : 0U;
    return fit ? fit : rec_ram_frames();
}
/* The longest take possible now (RAM and bank) and LENGTH's top in quarter seconds. */
static unsigned smp_room(void)
{
    unsigned b = smp_bank_frames();
    return smp_ram < b ? smp_ram : b;
}
static unsigned smp_len_max(void)
{
    unsigned q = (smp_room() + 7999U) / 8000U;
    return q ? q : 1U;
}
/* What a take on this pad can really be: LENGTH, the free RAM and the bank's room. */
static unsigned smp_cap(void)
{
    unsigned c = smp_len * 8000U, b = smp_room();
    return b < c ? b : c;
}
static const char *const smp_sources[REC_SOURCES] = {"USB", "BT", "MIX"};
static unsigned smp_count(void)
{
    unsigned n = 1U << smp_slices;
    if (n > 8U - smp_pad) n = 8U - smp_pad;
    while (n > 1 && rec_frames() / n < 1600U) n >>= 1; /* slices of at least 50 ms */
    return n;
}
/* Slice starts: even, or at the strongest jumps in loudness (TRANSIENT). */
static void smp_cut(void)
{
    unsigned n = smp_count(), total = rec_frames();
    memset(smp_starts, 0, sizeof smp_starts);
    for (unsigned k = 1; k < n; ++k) smp_starts[k] = (unsigned)((uint64_t)total * k / n);
    if (smp_mode && total >= 2000U * n) {
        enum { WIN = 256 };
        static uint16_t score[REC_MAX_FRAMES / WIN];
        unsigned picked = 1, last_e = 0, wins = total / WIN;
        for (unsigned w = 0; w < wins; ++w) {
            unsigned e = 0;
            for (unsigned i = 0; i < WIN; ++i) { int v = rec_sample(w * WIN + i); e += (unsigned)(v < 0 ? -v : v) >> 4; }
            e /= WIN; score[w] = (uint16_t)(e > last_e ? e - last_e : 0); last_e = e;
        }
        while (picked < n) { /* the best remaining onset, 1/16 s from the others and the end */
            unsigned best = 0, bw = 0;
            for (unsigned w = 1; w < wins; ++w) {
                unsigned f = w * WIN, near = 0;
                for (unsigned k = 0; k < picked; ++k) { unsigned d = f > smp_starts[k] ? f - smp_starts[k] : smp_starts[k] - f; near |= d < 2000U; }
                if (!near && total - f >= 2000U && score[w] > best) { best = score[w]; bw = w; }
            }
            if (!best) break;
            smp_starts[picked++] = bw * WIN;
        }
        if (picked == n) {
            for (unsigned a = 1; a < n; ++a)
                for (unsigned b = a + 1; b < n; ++b)
                    if (smp_starts[b] < smp_starts[a]) { unsigned t = smp_starts[a]; smp_starts[a] = smp_starts[b]; smp_starts[b] = t; }
        } else for (unsigned k = 1; k < n; ++k) smp_starts[k] = (unsigned)((uint64_t)total * k / n);
    }
    if (smp_sel >= n) smp_sel = n - 1;
}
/* The take as a filled waveform over columns x0..x0+w-1, centre cy, half height hh. */
static void smp_wave(unsigned x0, unsigned w, unsigned cy, unsigned hh, unsigned span, uint16_t c)
{
    unsigned have = rec_frames();
    if (!have || !span) return;
    if (paint_rendering && (paint_top > cy + hh || paint_top + PAINT_ROWS <= cy - hh)) return; /* band misses the wave */
    for (unsigned x = 0; x < w; ++x) {
        unsigned a = (unsigned)((uint64_t)span * x / w), b = (unsigned)((uint64_t)span * (x + 1) / w), peak = 0;
        if (a >= have) break;
        if (b > have) b = have;
        unsigned step = (b - a) / 32U; /* at most ~32 reads a column, so a 4 s take draws as fast as a short one */
        if (step < 4U) step = 4U;
        for (unsigned i = a; i < b; i += step) { int v = rec_sample(i); unsigned m = (unsigned)(v < 0 ? -v : v); if (m > peak) peak = m; }
        unsigned h = peak * hh / 32768U;
        paint_fill(x0 + x, cy - h, 1, 2 * h + 1, c);
    }
}
/* The eight pads: lit = the target(s), outline = holds a sample, cursor = chosen. */
static void smp_pads(unsigned lit_first, unsigned lit_count, unsigned sel, uint16_t lit_c)
{
    sp_pad_row(lit_first, lit_count, sel, lit_c);
}
static void smp_lamp(unsigned x, unsigned y, const char *label, const char *txt, uint16_t c, unsigned right)
{
    unsigned lw = lcd_width(&lcd13, label), tw = lcd_width(&lcd8, txt) + 8U, w = lw + 3U + tw;
    if (right) x -= w;
    unsigned ly = y + lcd13.digit_top + lcd13.digit_height / 2U - 4U;
    lcd_text((int)x, y, label, &lcd13, TEXT, MD_PANEL);
    rbox(x + lw + 3U, ly, tw, 9, 2, c, V_NONE);
    lcd_center((int)(x + lw + 3U + tw / 2U), ly + 1U, txt, &lcd8, BACK, c);
}
static void smp_draw(unsigned chop)
{
    unsigned st = rec_state(), fr = rec_frames(), lim = rec_limit(), n = smp_count();
    if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 26))) {
        uint32_t sig = signature(chop | (st << 1) | (smp_pad << 4) | (smp_src << 8) | (smp_mon << 10) | (smp_sel << 11) |
                                 (smp_mode << 14) | (smp_slices << 15) | ((uint32_t)(smp_db + 64) << 18), smp_starts, sizeof smp_starts);
        sig = signature(sig, &smp_len, sizeof smp_len);
        sig = signature(sig, &frame_views[track].next_length, sizeof frame_views[track].next_length) + (unsigned)chain_on;
        uint32_t pulse = pulse_level();
        if (sig != cache->row_cache[0] || pulse != cache->row_cache[1]) {
            unsigned full = sig != cache->row_cache[0];
            cache->row_cache[0] = sig; cache->row_cache[1] = pulse;
            paint_fill(44, 26, 154, 36, BACK);
            if (chop) smp_pads(smp_pad, n, smp_pad + smp_sel, knob_colors[2]);
            else smp_pads(smp_pad, 1, smp_pad, st == REC_RUN ? V_RED : knob_colors[0]);
            if (full) { /* v12: the pads are the plates, the state plate on the right, a window below */
                cache->middle_shape = ~0U; cache->middle_cache = ~0U;
                const struct groove_view *sv = &frame_views[track];
                char num[12]; decimal(num, (int)sv->next_length, 1);
                state_plate(num, chain_on != 0, sv->next_length != sv->sequence_length ? V_ORANGE : TEXT);
                paint_fill(0, 62, 240, 133, BACK);
                if (!chop) {
                    const char *nm = st == REC_ARMED ? "ARMED" : st == REC_DONE ? "DONE" : "REC";
                    window_draw(nm, st == REC_RUN ? V_RED : st == REC_ARMED ? 0xfe87 : PAL_AMBER);
                    if (st != REC_ARMED && st != REC_DONE) fill_ellipse((120 + (int)lcd_width(&lcd30, nm) / 2 + 12) * 10, 870, 50, 50, V_RED);
                } else window_draw("CHOP", PAL_AMBER);
            }
        }
        if (!chop) {
            /* the level, as a glass bar under the wave */
            unsigned lv = rec_level();
            uint32_t msig = signature(lv | (st << 28), &smp_ram, sizeof smp_ram) ^ smp_bank_frames();
            if (msig != cache->middle_cache) {
                cache->middle_cache = msig;
                int db = lv ? (int)(20.0f * vlog2((float)lv / 127.0f) / 3.3219f) : -99;
                unsigned on = db > -60 ? (unsigned)(60 + db) : 0;
                glass_bar_h(14, 176, 212, 8, on * 256U / 60U, st == REC_RUN ? V_RED : PAL_GREEN);
            }
            uint32_t wsig = (fr >> 10) | (st << 28) | ((frame_time / 250U & 1U) << 27) | ((smp_cap() < REC_RATE / 4) << 26);
            if (wsig != cache->middle_shape) {
                cache->middle_shape = wsig;
                paint_fill(8, 100, 224, 74, MD_PANEL);
                unsigned th = 2U + (unsigned)((60 + smp_db) * 20 / 60); /* dB scale, as the meter */
                sp_dash_h(14, 226, 146 - (int)th, knob_colors[1], 3, 3); sp_dash_h(14, 226, 146 + (int)th, knob_colors[1], 3, 3);
                if (st == REC_RUN || st == REC_DONE) smp_wave(14, 212, 146, 28, lim, knob_colors[3]);
                else if (smp_cap() < REC_RATE / 4) { /* say why nothing can be recorded here */
                    unsigned bank = smp_bank_frames() < REC_RATE / 4;
                    lcd_center(120, 108, bank ? "SAMPLE MEMORY FULL" : "RAM FULL", &lcd13, V_RED, MD_PANEL);
                    lcd_center(120, 124, bank ? "CHOOSE A PAD WITH A LONGER SOUND" : "USE LIGHTER ENGINES ON TRACKS", &lcd11, V_DIM, MD_PANEL);
                } else if (st != REC_ARMED) lcd_center(120, 114, "REC = START", &lcd11, V_DIM, MD_PANEL);
                if (smp_cap() >= REC_RATE / 4) { /* elapsed / take length, under the name */
                    char tm[24], b2[12];
                    unsigned shown = st == REC_RUN || st == REC_DONE ? fr : 0, cap = st == REC_IDLE ? smp_cap() : lim;
                    decimal(tm, (int)(shown / 32000U), 1); strcat(tm, "."); decimal(b2, (int)(shown % 32000U / 3200U), 1); strcat(tm, b2);
                    strcat(tm, " / "); decimal(b2, (int)(cap / 32000U), 1); strcat(tm, b2); strcat(tm, ".");
                    decimal(b2, (int)(cap % 32000U / 3200U), 1); strcat(tm, b2); strcat(tm, " S");
                    lcd_center(120, 101, tm, &lcd13, st == REC_RUN ? V_RED : V_DIM, MD_PANEL);
                }
            }
        } else {
            unsigned total = fr ? fr : 1;
            uint32_t wsig = signature(sig, &fr, sizeof fr) ^ (smp_encoded ? 0x80000000U | (smp_done << 16) | smp_total : 0U);
            if (wsig != cache->middle_shape && smp_encoded) { /* the PCM is gone: progress only */
                cache->middle_shape = wsig;
                paint_fill(0, 70, 240, 20, BACK);
                rbox(6, 90, 228, 98, 2, 0x0841, 0x2926);
                lcd_center(120, 112, smp_saving ? "SAVING" : "SAVE AGAIN", &lcd22, smp_saving ? knob_colors[2] : V_RED, 0x0841);
                paint_fill(20, 150, 200, 8, 0x18c4);
                if (smp_total) paint_fill(20, 150, smp_done * 200U / smp_total, 8, knob_colors[2]);
            } else if (wsig != cache->middle_shape) {
                cache->middle_shape = wsig;
                paint_fill(8, 100, 224, 84, MD_PANEL);
                smp_wave(14, 212, 132, 22, total, knob_colors[3]);
                for (unsigned k = 0; k < n; ++k) {
                    unsigned x0 = 14 + (unsigned)((uint64_t)212 * smp_starts[k] / total),
                             x1 = k + 1 < n ? 14 + (unsigned)((uint64_t)212 * smp_starts[k + 1] / total) : 226U;
                    if (k) sp_dash_v((int)x0, 108, 158, knob_colors[2], 2, 2);
                    char num[12]; decimal(num, (int)(smp_pad + k + 1), 1);
                    lcd_center((int)(x0 + x1) / 2, 165, num, &lcd17, k == smp_sel ? TEXT : knob_colors[2], MD_PANEL);
                }
            }
        }
    }
    if (!chop) {
        char db[12], len[12];
        decimal(db, smp_db, 1); strcat(db, "DB");
        unsigned room = smp_room();
        if (room >= REC_MIN_FRAMES && smp_len * 8000U >= room) { /* the top of the range: the real maximum, to 1/100 s */
            char num[12];
            decimal(len, (int)(room / 32000U), 1); strcat(len, ".");
            decimal(num, (int)(room % 32000U / 320U), 2); strcat(len, num); strcat(len, "S");
        } else {
            decimal(len, (int)(smp_len / 4U), 1);
            strcat(len, smp_len % 4U == 1 ? ".25S" : smp_len % 4U == 2 ? ".5S" : smp_len % 4U == 3 ? ".75S" : "S");
        }
        knob(0, "SOURCE", smp_sources[smp_src], colors[0]);
        knob(1, "THRESH", db, colors[1]);
        knob(2, "LENGTH", len, colors[2]);
        knob(3, "MONITOR", smp_mon ? "ON" : "OFF", colors[3]);
    } else {
        char start[12], num[12];
        unsigned f = smp_starts[smp_sel];
        decimal(start, (int)(f / 32000U), 1); strcat(start, ".");
        decimal(num, (int)(f % 32000U / 320U), 2); strcat(start, num); strcat(start, "S");
        decimal(num, (int)n, 1);
        knob(0, "START", start, colors[0]);
        knob(1, "SLICES", num, colors[1]);
        knob(2, "MODE", smp_mode ? "TRANSIENT" : "EVEN", colors[2]);
        knob(3, "", "", colors[3]);
    }
}
/* REC / CHOP controls; 1 = handled here. */
static int smp_control(const struct fm1_controls *controls, uint32_t pressed, uint32_t now)
{
    const int16_t *enc = controls->encoders;
    if (!smp_saving && (home_down || (controls->buttons & BUTTON(HOME)))) return 0; /* HOME keeps its meaning */
    if (page == SMP_REC) {
        unsigned st = rec_state();
        if (st == REC_DONE) { page = SMP_CHOP; smp_sel = 0; smp_cut(); cache->shown_page = ~0U; return 1; }
        rec_listen(smp_src, 1, !smp_mon);
        if (pressed & BUTTON(EDIT)) { rec_free(); rec_listen(0, 0, 0); page = TONE_EDIT; cache->shown_page = ~0U; return 1; }
        if (pressed & BUTTON(REC)) {
            if (st == REC_ARMED || st == REC_RUN) rec_stop();
            else {
                smp_ram = smp_ram_now(); smp_ram_at = now | 1U;
                unsigned cap = smp_cap();
                if (cap < REC_RATE / 4) { notice = smp_bank_frames() < REC_RATE / 4 ? "BANK FULL: LONGER PAD" : "RAM FULL: LIGHTER ENGINES"; notice_until = now + 2500U; }
                else if (rec_arm(smp_src, smp_db, cap)) { notice = "RAM FULL: LIGHTER ENGINES"; notice_until = now + 2500U; }
            }
            return 1;
        }
        if (pressed) return 0; /* other buttons keep their usual meaning (and leave) */
        if (st == REC_IDLE && (!smp_ram_at || now - smp_ram_at > 500U)) { smp_ram = smp_ram_now(); smp_ram_at = now | 1U; }
        if (st == REC_IDLE) {
            if (enc[0]) smp_pad = (unsigned)clamp((int)smp_pad + enc[0], 0, 7);
            if (enc[2]) smp_src = (unsigned)clamp((int)smp_src + (enc[2] > 0 ? 1 : -1), 0, REC_SOURCES - 1);
            if (enc[3]) smp_db = clamp(smp_db + (enc[3] > 0 ? 6 : -6), -60, 0);
            if (enc[4]) smp_len = (unsigned)clamp((int)smp_len + (enc[4] > 0 ? 1 : -1), 1, (int)smp_len_max()); /* up to the real maximum */
        }
        if (enc[5]) smp_mon = enc[5] > 0;
        return 1;
    }
    /* CHOP */
    /* SAVE: 1 notice shown -> 2 encode (smp_encoded 1, a few chunks a tick,
     * the first half of the bar) and start the bank job -> 3 one block a tick
     * (the second half). A failure keeps the encoded take (smp_encoded 2), so
     * SAVE simply tries again. */
    if (smp_saving == 1) { smp_saving = 2; return 1; }
    if (smp_saving >= 2) {
        static char msg[40];
        static unsigned job_done, job_total;
        unsigned n = smp_count();
        int r = 0;
        if (smp_saving == 2) {
            if (!smp_encoded) {
                r = rec_encode_begin(smp_starts, n, smp_items);
                for (unsigned k = 0; k < n && !r; ++k) {
                    char num[12]; decimal(num, (int)(smp_pad + k + 1), 1);
                    strcpy(smp_items[k].name, n == 1 ? "REC " : "SLICE "); strcat(smp_items[k].name, num);
                }
                if (!r) { smp_encoded = 1; smp_done = 0; smp_total = 1; }
            }
            if (!r && smp_encoded == 1) {
                /* ~8 ms or 8 chunks a tick: a 4 s take never stalls the control task */
                uint32_t t0 = fm1_millis();
                unsigned d = 0, t = 1, k = 0;
                do r = rec_encode_step(&d, &t); while (r == 1 && ++k < 8U && fm1_millis() - t0 < 8U);
                if (r == 1) {
                    smp_done = d; smp_total = 2U * t;
                    strcpy(msg, "SAVING "); decimal(msg + 7, (int)(smp_done * 100U / smp_total), 1);
                    strcat(msg, "% - POWER ON"); notice = msg; notice_until = now + 3000U;
                    return 1;
                }
                if (!r) smp_encoded = 2;
            }
            if (!r) r = groove_samples_replace_begin(smp_pad, n, smp_items);
            if (!r) { smp_saving = 3; job_done = 0; job_total = 1; smp_done = 1; smp_total = 2; return 1; }
        } else {
            r = groove_samples_replace_step(&job_done, &job_total);
            if (job_total) { smp_done = job_total + job_done; smp_total = 2U * job_total; }
            if (r == 1) {
                strcpy(msg, "SAVING "); decimal(msg + 7, (int)(smp_total ? smp_done * 100U / smp_total : 0U), 1);
                strcat(msg, "% - POWER ON"); notice = msg; notice_until = now + 3000U;
                return 1;
            }
            if (!r) {
                notice = "SAMPLE SAVED"; notice_until = now + 2500U;
                smp_saving = smp_encoded = 0; rec_free(); rec_listen(0, 0, 0);
                page = TONE_EDIT; drum_parts[track] = (uint8_t)smp_pad; cache->shown_page = ~0U;
                return 1;
            }
        }
        if (r == -2) strcpy(msg, "SAMPLE BANK FULL");
        else { strcpy(msg, "SAVE FAILED "); decimal(msg + 12, r, 1); }
        notice = msg; notice_until = now + 8000U; smp_saving = 0;
        return 1;
    }
    if (pressed & BUTTON(EDIT)) { smp_encoded = 0; rec_free(); rec_listen(0, 0, 0); page = TONE_EDIT; cache->shown_page = ~0U; return 1; }
    if (pressed & BUTTON(REC)) { smp_encoded = 0; rec_free(); page = SMP_REC; cache->shown_page = ~0U; return 1; }
    if (pressed & BUTTON(SAVE)) { smp_saving = 1; notice = "SAVING - KEEP POWER ON"; notice_until = now + 10000U; return 1; }
    if (pressed) return 0;
    if (smp_encoded) return 1; /* the take is encoded: only SAVE (again), REC or EDIT */
    unsigned n = smp_count();
    if (enc[0]) smp_sel = (unsigned)clamp((int)smp_sel + enc[0], 0, (int)n - 1);
    if (enc[2] && smp_sel) { /* START: 10 ms a detent, between the neighbours */
        unsigned lo = smp_starts[smp_sel - 1] + 1600U, hi = (smp_sel + 1 < n ? smp_starts[smp_sel + 1] : rec_frames()) - 1600U;
        smp_starts[smp_sel] = (unsigned)clamp((int)smp_starts[smp_sel] + enc[2] * 320, (int)lo, (int)hi);
    }
    if (enc[3]) { smp_slices = (unsigned)clamp((int)smp_slices + (enc[3] > 0 ? 1 : -1), 0, 3); smp_cut(); }
    if (enc[4]) { smp_mode = enc[4] > 0; smp_cut(); }
    return 1;
}
/* v11: the page box sits 4 px lower (66..192), centred between F A S L (61)
 * and the knob stripes (197); everything inside follows. */
enum { BOX_DY = 4 };
/* v7 look shared by every effect-like page (agreed screen 20b): a strip of
 * names at y 48 (chosen: its colour + underline; `lit` bit set = TEXT, else
 * dim), then a dark box y 72..187 with a small caption and the name large. */
/* v9: the strip sits inside the box, at its top (the header owns rows 0..61). */
static void strip20(const char *const *names, unsigned n, unsigned sel, uint16_t c, uint32_t lit)
{
    unsigned cw = n == 2 ? 60U : 228U / n; /* two tabs sit close in the centre */
    for (unsigned i = 0; i < n; ++i) {
        int cx = 120 + (int)(cw * (2U * i + 1U)) / 2 - (int)(cw * n) / 2;
        unsigned w = lcd_width(&lcd13, names[i]);
        lcd_center(cx, 66 + BOX_DY, names[i], &lcd13, i == sel ? c : (lit >> i & 1U) ? TEXT : V_DIM, 0x0841);
        if (i == sel) paint_fill((unsigned)(cx - (int)w / 2), 81 + BOX_DY, w, 2, c);
    }
}
/* v12 effect icons, about 14 x 11 px around (cx, cy). 0..9 the MASTER FX rows
 * (FILTER .. COMP), 10..12 the TRACK FX tabs (TRACK, SHARED, DELAY). */
static int icon_k = 1; /* icon size: 1 normal, 2 large (TRACK FX) */
static void fx_icon(unsigned id, int cx, int cy, uint16_t c)
{
    const int K = icon_k;
    static const int8_t pts[13][7][2] = {
        {{-7, -2}, {-2, -2}, {0, -5}, {2, -2}, {7, 5}, {7, 5}, {7, 5}},       /* filter: resonant low-pass */
        {{-7, 5}, {-4, 4}, {-1, 1}, {1, -1}, {4, -4}, {7, -5}, {7, -5}},      /* drive: soft clip S */
        {{-7, 0}, {-5, -3}, {-2, -3}, {0, 0}, {2, 3}, {5, 3}, {7, 0}},       /* chorus: a wave */
        {{0, 0}, {3, 0}, {3, 3}, {-3, 3}, {-3, -3}, {6, -3}, {6, 5}},         /* phaser: spiral */
        {{-6, 0}, {-3, -3}, {0, 0}, {-3, 3}, {-6, 0}, {-6, 0}, {-6, 0}},      /* ring: two links (second below) */
        {{-7, 5}, {-7, 0}, {-3, -5}, {3, -5}, {7, 0}, {7, 5}, {7, 5}},       /* reverb: a room */
        {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},             /* eq: sliders (below) */
        {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},             /* delay: echo dots (below) */
        {{-7, 0}, {0, 0}, {6, -5}, {0, 0}, {6, 5}, {6, 5}, {6, 5}},          /* sends: a branch */
        {{-7, 0}, {7, 0}, {7, 0}, {7, 0}, {7, 0}, {7, 0}, {7, 0}},            /* comp: threshold line, arrows below */
        {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},             /* track: sliders */
        {{-7, 0}, {0, 0}, {6, -5}, {0, 0}, {6, 5}, {6, 5}, {6, 5}},          /* shared: a branch */
        {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}};            /* delay: echo dots */
    if (id > 12) return;
    for (unsigned k = 0; k + 1 < 7; ++k)
        if (pts[id][k][0] != pts[id][k + 1][0] || pts[id][k][1] != pts[id][k + 1][1])
            line(cx + K * pts[id][k][0], cy + K * pts[id][k][1], cx + K * pts[id][k + 1][0], cy + K * pts[id][k + 1][1], c);
    if (id == 4) { line(cx, cy, cx + K * 3, cy - K * 3, c); line(cx + K * 3, cy - K * 3, cx + K * 6, cy, c); line(cx + K * 6, cy, cx + K * 3, cy + K * 3, c); line(cx + K * 3, cy + K * 3, cx, cy, c); }
    if (id == 6 || id == 10) for (int k = -1; k <= 1; ++k) { /* three sliders */
        line(cx + K * 5 * k, cy - K * 5, cx + K * 5 * k, cy + K * 5, c);
        int y = cy + K * (k == -1 ? 2 : k == 0 ? -3 : 1);
        paint_fill((unsigned)(cx + K * 5 * k - 2 * K), (unsigned)(y - K), 5 * K, 3 * K, c);
    }
    if (id == 7 || id == 12) { paint_fill((unsigned)(cx - K * 7), (unsigned)(cy - K * 2), 4 * K, 4 * K, c); paint_fill((unsigned)(cx - K * 1), (unsigned)(cy - K * 1), 3 * K, 3 * K, c); paint_fill((unsigned)(cx + K * 4), (unsigned)cy, 2 * K, 2 * K, c); }
    if (id == 9) { /* two arrows pressing on the threshold */
        line(cx, cy - K * 6, cx, cy - K * 2, c); line(cx - K * 2, cy - K * 4, cx, cy - K * 2, c); line(cx + K * 2, cy - K * 4, cx, cy - K * 2, c);
        line(cx, cy + K * 6, cx, cy + K * 2, c); line(cx - K * 2, cy + K * 4, cx, cy + K * 2, c); line(cx + K * 2, cy + K * 4, cx, cy + K * 2, c);
    }
    if (id == 8 || id == 11) { line(cx + K * 4, cy - K * 5, cx + K * 6, cy - K * 5, c); line(cx + K * 6, cy - K * 5, cx + K * 6, cy - K * 3, c); line(cx + K * 4, cy + K * 5, cx + K * 6, cy + K * 5, c); line(cx + K * 6, cy + K * 5, cx + K * 6, cy + K * 3, c); }
}
/* The strip of icons on the box's top: chosen = its colour + underline,
 * `lit` bit set = on (TEXT), else dim. */
static void icon_strip(unsigned first, unsigned n, unsigned sel, uint16_t c, uint32_t lit)
{
    unsigned cw = n <= 3 ? 60U : 228U / n;
    for (unsigned i = 0; i < n; ++i) {
        int cx = 120 + (int)(cw * (2U * i + 1U)) / 2 - (int)(cw * n) / 2;
        fx_icon(first + i, cx, 76 + BOX_DY, i == sel ? c : (lit >> i & 1U) ? TEXT : V_DIM);
        if (i == sel) paint_fill((unsigned)(cx - 8), 81 + BOX_DY + 3, 16, 2, c);
    }
}
/* v7 MASTER FX: the nine effects in the 20b strip (white = on, dim = off),
 * the chosen effect's name large, its type or number small above, its curve
 * below in the same box, its controls on the knobs. */
static __attribute__((noinline)) void master_fx_draw(void)
{
    /* user 13.11: short names; row 7 = the shared delay, row 8 = the shared send reverb */
    static const char *const longn[FX_ROWS] = {"FILTER", "DRIVE", "CHORUS", "PHASER", "RING MOD", "REVERB", "EQ", "DLY SEND", "REV SEND", "COMP"};
    for (unsigned c = 0; c < 4; ++c) {
        char kv[12]; fx_value(fx_row, fx_col(fx_row, c), kv);
        knob(c, fx_label(fx_row, fx_col(fx_row, c)), kv, colors[c]);
    }
    if (paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 26)) return;
    uint32_t sig = fx_row;
    for (unsigned row = 0; row < FX_ROWS; ++row) sig |= fx_active(row) << (8 + row);
    char caption[24];
    if (!strcmp(fx_label(fx_row, 0), "TYPE")) fx_value(fx_row, 0, caption);
    else { decimal(caption, (int)fx_pos(fx_row) + 1, 1); strcat(caption, " / 10"); }
    if (!fx_active(fx_row)) strcat(caption, "  OFF");
    strcpy(fx_caption, caption); fx_title = longn[fx_row];
    if (cache->title_cache != sig) {
        cache->title_cache = sig;
        /* v13: 2 pages of 5 plates (white = on, grey = off), the glass cursor marks the chosen one, the window names the effect */
        paint_fill(42, 26, 198, 36, BACK);
        paint_fill(0, 62, 240, 132, BACK);
        unsigned pg = fx_pos(fx_row) / 5U;
        for (unsigned i = 0; i < 5; ++i) {
            unsigned n = fx_order[pg * 5U + i], x = 44 + i * 39, sel = n == fx_row;
            uint16_t ink = fx_active(n) ? TEXT : V_DIM;
            unsigned ic = n == FX_REVERB ? (unsigned)MFX_VERB : n; /* REV SEND: the reverb's icon, DLY SEND: the delay's */
            rbox(x, HDR_FY, 35, 34, 3, 0x10a3, 0x5acb);
            icon_k = 2; fx_icon(ic, (int)x + 17, HDR_FY + 17, ink); fx_icon(ic, (int)x + 18, HDR_FY + 17, ink); icon_k = 1;
            if (n == FX_DELAY || n == FX_REVERB) /* a send: small arrow in the plate's top right corner (mockup s.3) */
                for (unsigned r = 0; r < 6; ++r) paint_fill(x + 26, HDR_FY + 3 + r, (r < 3 ? r : 5 - r) + 1, 1, ink);
            if (sel) glass_cursor(x, HDR_FY, 35, 34, 8, MD_AMBER); /* radius 8 = the plate's pebble corner (rbox r 3) */
        }
        rbox(6, 66, 228, 124, 2, MD_PANEL, MD_EDGE);
        lcd_center(120, 73, longn[fx_row], &lcd30, MD_AMBER, MD_PANEL);
        cache->middle_shape = ~0U; /* the curve sits inside the new window */
    }
    fx_visual();
}
static void master_fx_edit(const int16_t *enc)
{
    for (unsigned k = 0; k < 4; ++k) {
        int d = enc[2 + k];
        if (!d) continue;
        unsigned c = fx_col(fx_row, k); /* knob -> parameter column */
        if (c) d = fm_encoder_delta(2 + k, (int16_t)d, 128); /* the type stays one step */
        if (fx_row < FX_DELAY) mfx_edit(fx_row, c, d);
        else if (fx_row == FX_COMP) mfx_comp_edit(c, d);
        else if (fx_row == FX_DELAY) { if (c != 2) groove_effects_edit(c == 0 ? 1 : c == 1 ? 2 : 3, d); }
        else if (c == 1 || c == 3) groove_effects_edit(c == 1 ? 0 : 3, d);
    }
}
/* ---- v13 TRACK FX (glass pipes, fill = level) and SHARED FX (tail on a time ruler, echoes, return) pictures ---- */
static void fxp_pipe(unsigned x0, unsigned x1, unsigned y, unsigned f, uint16_t c)
{
    glass_bar_h(x0, y - 6, x1 - x0, 12, f, c);
    fill_tri((int)x1 - 1, (int)y - 5, (int)x1 - 1, (int)y + 4, (int)x1 + 5, (int)y, c);
}
static void fxp_rev_icon(int cx, int cy, uint16_t c)
{
    static const uint8_t r[3] = {5, 9, 13};
    for (unsigned k = 0; k < 3; ++k) { /* three arcs opening to the right, centre cx - 8 */
        int px = 0, py = 0;
        for (int a = -2; a <= 2; ++a) {
            int x = cx - 8 + (int)r[k] * (a == 0 ? 10 : (a == 1 || a == -1) ? 9 : 7) / 10, y = cy + (int)r[k] * a * 3 / 10;
            if (a > -2) line(px, py, x, y, c);
            px = x; py = y;
        }
    }
    sq_dot(cx - 8, cy, 22, c);
}
static void fxp_dly_icon(int cx, int cy, uint16_t c)
{
    static const uint8_t h[4] = {20, 13, 8, 5};
    for (unsigned i = 0; i < 4; ++i) rbox((unsigned)(cx - 10 + (int)i * 6), (unsigned)(cy + 10 - (int)h[i]), 3, h[i], 1, c, V_NONE);
}
static void fxp_mix_icon(int cx, int cy, int r, uint16_t c)
{
    sq_ring(cx, cy, r, c);
    sq_line(cx - 5, cy - 1, cx + 5, cy - 1, c); line(cx, cy - 6, cx, cy + 5, c); line(cx + 1, cy - 6, cx + 1, cy + 5, c);
}
static void fxp_draw(unsigned page, const unsigned *f, unsigned ms)
{
    const uint16_t *k = knob_colors;
    paint_fill(8, 100, 224, 88, MD_PANEL);
    if (!page) { /* the track's sends and level on their way to the mix */
        uint16_t ec = frame_views[track].type < ENGINES ? engine_colors[frame_views[track].type] : k[3]; /* the track's engine colour */
        rbox(8, 108, 26, 76, 3, mix565(MD_PANEL, ec, 8), ec);
        char n[4]; n[0] = (char)('1' + track); n[1] = 0;
        sq_text(21, 146, n, &lcd22, ec, mix565(MD_PANEL, ec, 8));
        fxp_pipe(34, 194, 118, f[2], k[2]);
        for (unsigned i = 1; i < 3; ++i) {
            unsigned y = 118 + i * 28;
            fxp_pipe(34, 100, y, f[i - 1], k[i - 1]);
            rbox(102, y - 14, 32, 28, 3, MD_PANEL, k[i - 1]);
            if (i == 1) fxp_rev_icon(118, (int)y, k[0]); else fxp_dly_icon(118, (int)y, k[1]);
            fxp_pipe(134, 194, y, f[3], k[3]);
        }
        rbox(196, 108, 30, 76, 3, 0x1082, TEXT);
        fxp_mix_icon(211, 146, 11, TEXT);
        return;
    }
    /* SHARED FX: reverb tail (length = REV TIME), echoes (spacing = DLY TIME, falling by FEEDBACK), one return to the mix */
    enum { BX = 44 };
    unsigned len = 24U + f[0] * 120U / 256U, sp = 6U + (ms > 500U ? 500U : ms) * 60U / 500U;
    fxp_rev_icon(28, 126, k[0]); fxp_dly_icon(28, 166, k[1]);
    paint_fill(BX, 142, 149, 1, GLASS_TRACK_EDGE); paint_fill(BX, 180, 149, 1, GLASS_TRACK_EDGE);
    for (unsigned x = 0; x <= 144; x += 24) { paint_fill(BX + x, 142, 1, 4, GLASS_TRACK_EDGE); }
    int px = BX, py = 112;
    for (unsigned j = 0; j <= 30; ++j) {
        unsigned e = j / 3U, fr3 = j % 3U; /* 2^-(5 j / 30) in 1/256: a table every 3 points, interpolated */
        static const uint8_t t[11] = {255, 181, 128, 90, 64, 45, 32, 23, 16, 11, 8};
        unsigned tv = e >= 10 ? 8U : (t[e] * (3U - fr3) + t[e + 1] * fr3) / 3U;
        int x = BX + (int)(j * len / 30U), y = 142 - (int)(30U * tv / 256U);
        if (j) sq_line(px, py, x, y, k[0]);
        px = x; py = y;
    }
    paint_fill(BX, 147, len, 2, k[0]); paint_fill(BX, 145, 2, 6, k[0]); paint_fill(BX + len - 2, 145, 2, 6, k[0]);
    unsigned fb = f[2];
    for (unsigned i = 0, h = 28U * 256U; i < 9; ++i, h = h * fb / 256U) {
        unsigned x = BX + i * sp;
        if (x > BX + 144U || h / 256U < 2U) break;
        rbox(x, 180 - h / 256U, 4, h / 256U, 1, k[2], V_NONE);
    }
    paint_fill(BX + 2, 185, sp, 2, k[1]); paint_fill(BX + 2, 183, 2, 6, k[1]); paint_fill(BX + sp, 183, 2, 6, k[1]);
    for (unsigned i = 0; i < 2; ++i) {
        unsigned y = 134 + i * 32;
        sq_line(190, (int)y, 203, (int)y, k[3]); fill_tri(201, (int)y - 5, 201, (int)y + 4, 207, (int)y, k[3]);
    }
    fxp_mix_icon(217, 117, 10, TEXT);
    glass_bar_v(206, 129, 22, 55, f[3], k[3]);
}
static __attribute__((noinline)) void effects_draw(void)
{
    /* v13 TRACK FX / SHARED FX: two tab plates (SELECT turns them, the glass cursor marks the chosen one),
     * then four glass bars, each over its knob in the knob's colour; no words in the window. The delay
     * (time, feedback, return, sync) is one shared effect: SHARED FX has it, a track only sends to it. */
    const uint8_t *values = frame_effects;
    const struct groove_view *tv = &frame_views[track];
    unsigned title = 1U + track + effects_page * 8U + (tv->next_length << 12) + ((unsigned)chain_on << 24);
    if (cache->title_cache != title) {
        cache->title_cache = title;
        static const char *const names[2] = {"TRACK FX", "SHARED FX"};
        paint_fill(42, 26, 198, 36, BACK);
        paint_fill(0, 62, 240, 132, BACK);
        for (unsigned k = 0; k < 2; ++k) { /* two flat tab plates like MICRO D's path; SELECT turns them */
            unsigned x = 44 + k * 39, on = k == effects_page;
            uint16_t ink = on ? TEXT : V_DIM;
            rbox(x, HDR_FY, 35, 34, 3, 0x10a3, 0x5acb);
            icon_k = 2; fx_icon(10 + k, (int)x + 17, HDR_FY + 17, ink); fx_icon(10 + k, (int)x + 18, HDR_FY + 17, ink); icon_k = 1;
            if (on) glass_cursor(x, HDR_FY, 35, 34, 8, MD_AMBER); /* radius 8 = the plate's pebble corner (rbox r 3) */
        }
        char num[12]; decimal(num, (int)tv->next_length, 1);
        state_plate(num, chain_on != 0, tv->next_length != tv->sequence_length ? V_ORANGE : TEXT);
        rbox(6, 66, 228, 124, 2, MD_PANEL, MD_EDGE);
        lcd_center(120, 73, names[effects_page % 2U], &lcd30, MD_AMBER, MD_PANEL);
        memset(cache->row_cache, 0xff, sizeof cache->row_cache);
    }
    static const char *const shared[4] = {"REV TIME", "DLY TIME", "FEEDBACK", "RETURN"};
    static const char *const local[4] = {"REV SEND", "DLY SEND", "TRK VOL", "ALL RET"};
    const char *const *labels = effects_page ? shared : local;
    unsigned fr[4];
    for (unsigned i = 0; i < 4; ++i) {
        unsigned value = values[i], synced = groove_delay_synced(values[1]);
        const char *label = labels[i];
        char display[12];
        if (effects_page && i == 1 && synced) { /* a project saved with the delay on a note division keeps it */
            label = "DIVISION"; delay_time_text(display, values[1]);
        } else if (effects_page && i == 1) delay_time_text(display, values[1]);
        else decimal(display, (int)value, 1);
        /* the bar: 0..1 of the knob's range */
        unsigned full = !effects_page && i < 2 ? 100U : 127U;
        unsigned raw = values[i];
        /* SHARED FX K2 (DLY TIME): codes 128+ are sync divisions / long times, so the
         * bar follows the applied time in ms (square-root scale, 2040 ms = full). */
        if (effects_page == 1 && i == 1) raw = isqrt_u(((groove_delay_ticks() * 80U + 220U) / 441U) * 127U * 127U / 2040U);
        if (raw > full) raw = full;
        fr[i] = raw * 256U / full;
        knob(i, label, display, colors[i]);
    }
    unsigned ms = (groove_delay_ticks() * 80U + 220U) / 441U;
    uint32_t ps = signature(effects_page + 1U, fr, sizeof fr) + (effects_page ? ms : track);
    if (cache->row_cache[0] != ps && !(paint_rendering && (paint_top >= 190 || paint_top + PAINT_ROWS <= 100))) {
        cache->row_cache[0] = ps;
        fxp_draw(effects_page, fr, ms);
    }
}
static void leds(const struct groove_view *v, unsigned visible)
{
    (void)fm1_led_probe(-1);
    uint32_t bits = 0;
    unsigned first = step_first(v), blink = (frame_time / 250U) & 1U;
    if (visible == STEPS && !writing && !seq_page) {
        /* Grid: the keys of the selected step's note (or its drum pads) light up,
         * together with the keys being held. */
        struct groove_step c = frame_cells[cursor % GROOVE_STEPS];
        unsigned pads = groove_pad_count(track) != 0;
        for (unsigned key = 0; key < 27; ++key) {
            unsigned note = groove_key_note(track, key, octave);
            if (key_note[key] != 0xff) bits |= 1U << key;
            if (note > 127 || (!c.velocity && !c.drums)) continue;
            if (!pads && note == c.note) bits |= 1U << key;
        }
    } else if (visible == STEPS && !writing && seq_key_steps()) {
        uint64_t lane =
            groove_pad_count(track) != 0 ? groove_drum_steps(track, drum) : v->active_steps;
        for (unsigned i = 0; i < 16; ++i)
            if ((lane >> (first + i)) & 1U)
                bits |= 1U << white_keys[i];
        if (v->playing && (v->play_step & ~15U) == first)
            bits ^= 1U << white_keys[v->play_step & 15U];
    } else if (visible == PRESET_BROWSER) {
        bits = 1U << white_keys[preset_slot];
    } else
        for (unsigned key = 0; key < 27; ++key)
            if (key_note[key] != 0xff)
                bits |= 1U << key;
    if (visible == FM_EDIT) {
        for (unsigned i = 0; i < 10; ++i) bits &= ~(1U << edit_keys[i]);
        for (unsigned i = 0; i < 6; ++i)
            if (!(frame_controls.operators & (1U << i))) bits |= 1U << edit_keys[i];
        if (op < 8) {
            bits &= ~(1U << edit_keys[op]);
            if (!blink) bits |= 1U << edit_keys[op];
        }
        bits |= 1U << edit_keys[frame_controls.mono ? 8 : 9];
    } else if (visible == TONE_EDIT && v->type == GROOVE_303) {
        bits &= ~((1U << edit_keys[8]) | (1U << edit_keys[9]));
        bits |= 1U << edit_keys[v->mono ? 8 : 9];
    }
    /* Soft key glow (user 09.11, round 4): on SEQ STEP and ARP the chord's keys light (the step's chord, the held
     * keys), the scale of the key glows dim (GLOBAL KEY, or AUTO: the first chord after power-on / a project load). */
    uint32_t dim = 0;
    if (!groove_pad_count(track) && ((visible == STEPS && seq_page == SEQ_STEP) || visible == ARP_EDIT)) {
        if (visible == STEPS && !key_get() && !key_scale_mask()) { /* AUTO, nothing yet: the track's first chord */
            static uint32_t scanned_rev = ~0U;
            static unsigned scanned_track = ~0U;
            uint32_t rev = groove_edit_revision();
            if (rev != scanned_rev || track != scanned_track) {
                scanned_rev = rev; scanned_track = track;
                struct groove_sequence_options o; groove_sequence_options_copy(&o);
                unsigned len = groove_sequence_length(track);
                for (unsigned s = 0; s < len && !key_scale_mask(); ++s) {
                    struct groove_step c; groove_sequence_read(track, s, &c, 1);
                    unsigned id = c.drums & 31U;
                    if (!c.velocity || !id || id > GROOVE_CHORDS || o.chord[id - 1][0] < 2) continue;
                    uint8_t cn[8]; unsigned cc = o.chord[id - 1][0] > 8 ? 8U : o.chord[id - 1][0];
                    for (unsigned i = 0; i < cc; ++i) { unsigned n = c.note + (i ? o.chord[id - 1][i] : 0U); cn[i] = (uint8_t)(n > 127 ? 127 : n); }
                    key_chord_seen(cn, cc); /* a chord without a third (sus, 5) does not fix it: look on */
                }
            }
        }
        uint8_t notes[8];
        unsigned n = visible == ARP_EDIT ? arp_held_notes(notes) : step_chord_notes(v, notes), chord_pc = 0;
        for (unsigned i = 0; i < n; ++i) chord_pc |= 1U << (notes[i] % 12U);
        if (visible == ARP_EDIT && n > 1) key_chord_seen(notes, n); /* AUTO: the first held chord */
        unsigned scale = key_scale_mask();
        for (unsigned key = 0; key < 27; ++key) {
            unsigned note = groove_key_note(track, key, octave);
            if (note > 127) continue;
            if (chord_pc & (1U << (note % 12U))) bits |= 1U << key;
            else if (scale & (1U << (note % 12U))) dim |= 1U << key;
        }
    }
    dim &= ~bits;
    uint32_t changed = bits ^ led_cache;
    for (unsigned key = 0; key < 27; ++key)
        if (changed & (1U << key))
            fm1_led_set(key + 14, (bits >> key) & 1U);
    led_cache = bits;
    {
        static uint32_t dim_cache;
        uint32_t dc = dim ^ dim_cache;
        for (unsigned key = 0; key < 27; ++key)
            if (dc & (1U << key)) fm1_led_dim_set(key + 14, (dim >> key) & 1U);
        dim_cache = dim;
    }
    unsigned step_pages = (visible == STEPS && seq_key_steps()) || visible == ACTIONS;
    bits = ((step_pages ? first > 0 : octave < 0) << OCT_DOWN)
        | ((step_pages ? first + 16 < v->sequence_length : octave > 0) << OCT_UP)
        | ((visible == TRACKS) << HOME) | ((visible == EFFECTS || visible == MASTER_FX) << FX) | ((visible == LFOS) << LFO)
        | ((arp_enabled(track) != 0) << ARP)
        | ((visible == ACTIONS) << SCL)
        | (((visible == STEPS && !seq_midi()) || visible == STEP_EDIT || visible == RHYTHM || visible == CHAIN) << SEQ)
        | ((writing != 0 || (visible == SMP_REC && (rec_state() == REC_RUN || (rec_state() == REC_ARMED && blink)))) << REC)
        | ((v->playing != 0) << PLAY) | ((visible == GLOBAL || (visible == STEPS && seq_midi())) << GLO)
        | ((visible == FM_EDIT || visible == TONE_EDIT || visible == FM_QUICK || visible == STEP_EDIT) << EDIT)
        | ((visible == FM_ENV) << ENV) | ((visible == FM_LFO) << LFO)
        | ((visible == PROJECT || visible == SOURCE || (visible == ACTIONS && action_confirm)) << SAVE);
    /* PLAY LED: with a green LED chosen (PLAY LED page) the red one (PLAY) shows
     * pause only; green blinks on the clock while playing, burns on step 1 when
     * stopped and with red when paused. Without green PLAY alone: blink / steady / fast blink. */
    {
        unsigned beat = (v->play_step & 3U) < 2U, parked = v->play_step == 0, fast = (frame_time / 125U) & 1U;
        unsigned red, green = 0;
        if (play_green <= 43U) { red = !v->playing && !parked; green = v->playing ? beat : 1U; }
        else red = v->playing ? beat : parked ? 1U : fast;
        bits = (bits & ~(1U << PLAY)) | (red << PLAY);
        static unsigned green_cache = 2;
        if (green_old <= 43U) { fm1_led_raw((int)green_old, 0); green_old = 0xffU; green_cache = 2; }
        if (play_green <= 43U && green != green_cache) { fm1_led_raw((int)play_green, (int)green); green_cache = green; }
        else if (play_green > 43U) green_cache = 2;
    }
    changed = bits ^ button_led_cache;
    for (unsigned key = 0; key < 14; ++key)
        if (changed & (1U << key)) fm1_led_set(key, (bits >> key) & 1U);
    button_led_cache = (uint16_t)bits;
}
/* ARP cat: dances on the beat while the arpeggio runs (transport playing or
 * keys held on this track); 0 = sits still, else 1 + quarter-note count. */
static unsigned arp_cat_beat(void)
{
    const struct groove_view *v = &frame_views[track];
    unsigned held = 0;
    for (unsigned k = 0; k < 27; ++k) held |= key_note[k] != 0xff && key_track[k] == track;
    if (!arp_enabled(track) || !(v->playing || held)) return 0;
    unsigned bpm = v->tempo ? v->tempo : 120U;
    return 1U + (unsigned)((uint64_t)frame_time * bpm / 60000U);
}
/* ARP page 1 pattern pills: the held keys' pattern (12 at most), the pill lit by the beat. */
struct arp_pills { unsigned n, pitch, lit, lo, span; uint8_t pat[64]; };
static void arp_pills_get(struct arp_pills *p)
{
    uint8_t held[27]; unsigned hn = 0, hi = 0;
    for (unsigned k = 0; k < 27; ++k) if (key_note[k] != 0xff && key_track[k] == track && hn < 27) held[hn++] = key_note[k];
    p->n = arp_pattern(track, held, hn, p->pat, 64);
    if (p->n > 12) p->n = 12;
    p->lo = 127;
    for (unsigned i = 0; i < p->n; ++i) { if (p->pat[i] < p->lo) p->lo = p->pat[i]; if (p->pat[i] > hi) hi = p->pat[i]; }
    p->span = hi > p->lo ? hi - p->lo : 1;
    unsigned beat = arp_cat_beat();
    p->lit = p->n ? (beat ? beat - 1U : 0U) % p->n : 0U;
    p->pitch = p->n ? 204U / p->n : 35U;
    if (p->pitch > 35U) p->pitch = 35U;
}
static unsigned arp_pill_x(const struct arp_pills *p, unsigned i) { return 120U - p->n * p->pitch / 2U + i * p->pitch + 3U; }
static unsigned arp_pill_y(const struct arp_pills *p, unsigned i) { return 176U - (p->pat[i] - p->lo) * 54U / p->span; }
/* The check pass of ARP_EDIT page 1: held keys changed -> the chord plate and the pill row; only the lit pill
 * moved -> the old and the new pill. `take` = a full repaint follows: only remember the state.
 * row_cache[2] = held keys signature, [3] = lit pill + 1, on page entry ~0. */
static void arp_damage(unsigned take)
{
    uint8_t held[8]; unsigned hn = arp_held_notes(held);
    uint32_t hs = signature(hn + 0xa4000U, held, hn);
    if (arp_page) { /* page 2: only the chord plate shows the held keys */
        if (!take && hs != cache->row_cache[2]) paint_mark(174, 27, 61, 34);
        cache->row_cache[2] = hs;
        return;
    }
    for (unsigned k = 0; k < 27; ++k) if (key_note[k] != 0xff && key_track[k] == track) hs = hs * 33U + key_note[k];
    struct arp_pills ap; arp_pills_get(&ap);
    unsigned was = cache->row_cache[3];
    if (!take && hs != cache->row_cache[2]) {
        paint_mark(174, 27, 61, 34);  /* the chord plate */
        paint_mark(16, 121, 208, 68); /* every pill (x 18..222, y 122..188) */
    } else if (!take && ap.lit + 1U != was && ap.n) {
        unsigned w = ap.pitch - 4U;
        if (was && was - 1U < ap.n) paint_mark(arp_pill_x(&ap, was - 1U) - 1U, arp_pill_y(&ap, was - 1U) - 1U, w, 14);
        paint_mark(arp_pill_x(&ap, ap.lit) - 1U, arp_pill_y(&ap, ap.lit) - 1U, w, 14);
    }
    cache->row_cache[2] = hs; cache->row_cache[3] = ap.lit + 1U;
}
/* DX7 EDIT icons (v13, 48 x 30 boxes): TONE = ALGO / FEEDBACK / OP / LEVEL, ENVELOPE = ATTACK / DECAY / SUSTAIN / RELEASE. */
static void dx_ic_algo(int cx, int cy, uint16_t c)
{
    for (int k = 0; k < 2; ++k) {
        rbox((unsigned)(cx - 12 + k * 14), (unsigned)(cy - 9 + k * 10), 10, 8, 1, V_NONE, c);
        rbox((unsigned)(cx - 11 + k * 14), (unsigned)(cy - 8 + k * 10), 8, 6, 1, V_NONE, c);
    }
    static const int8_t w[3][2] = {{-20, -50}, {70, -50}, {70, 10}};
    ic_poly(cx, cy, w, 3, 9, c);
}
static void dx_ic_fb(int cx, int cy, uint16_t c)
{
    for (int k = 0; k < 12; ++k) {
        float a0 = 0.5f + 0.4f * (float)k, a1 = a0 + 0.4f;
        ic_seg(cx * 10 + (int)(80.0f * vsin(0.25f - a0 * 0.15915494f)), cy * 10 + (int)(80.0f * vsin(a0 * 0.15915494f)),
               cx * 10 + (int)(80.0f * vsin(0.25f - a1 * 0.15915494f)), cy * 10 + (int)(80.0f * vsin(a1 * 0.15915494f)), 10, c);
    }
    fill_tri(cx * 10 + 10, cy * 10 - 130, cx * 10 + 10, cy * 10 - 70, cx * 10 + 70, cy * 10 - 100, c);
}
static void dx_ic_level(int cx, int cy, uint16_t c) { glass_bar_v((unsigned)(cx - 4), (unsigned)(cy - 10), 8, 20, 205, c); }
static void dx_ic_atk(int cx, int cy, uint16_t c) { static const int8_t p[3][2] = {{-120, 70}, {30, -80}, {120, -80}}; ic_poly(cx, cy, p, 3, 11, c); }
static void dx_ic_dec(int cx, int cy, uint16_t c) { static const int8_t p[3][2] = {{-120, -80}, {-20, 30}, {120, 30}}; ic_poly(cx, cy, p, 3, 11, c); }
static void dx_ic_sus(int cx, int cy, uint16_t c)
{
    ic_seg(cx * 10 - 120, cy * 10 - 20, cx * 10 + 120, cy * 10 - 20, 15, c);
    ic_seg(cx * 10 - 120, cy * 10 + 70, cx * 10 + 120, cy * 10 + 70, 6, mix565(0x18a3, c, 16));
}
static void dx_ic_rel(int cx, int cy, uint16_t c) { static const int8_t p[3][2] = {{-120, -60}, {30, 70}, {120, 70}}; ic_poly(cx, cy, p, 3, 11, c); }
static void dx_ic_trans(int cx, int cy, uint16_t c) /* transpose: one arrow up, one down */
{
    for (int k = -1; k <= 1; k += 2) {
        int x = (cx + 6 * k) * 10;
        ic_seg(x, (cy - 7) * 10, x, (cy + 7) * 10, 9, c);
        fill_tri(x - 45, (cy - k * 3) * 10, x + 45, (cy - k * 3) * 10, x, (cy - k * 9) * 10 - (k > 0 ? 0 : 0), c);
    }
}
static const icon_draw_fn dx_icons[2][4] = {{dx_ic_algo, dx_ic_fb, dx_ic_trans, dx_ic_level}, {dx_ic_atk, dx_ic_dec, dx_ic_sus, dx_ic_rel}};
static __attribute__((noinline)) void edit_page_draw(unsigned visible)
{
    if (paint_rendering && paint_top + PAINT_ROWS <= 26) return; /* the state plate starts at y 27 */
    if (!paint_rendering) {
        uint32_t sig = visible * 33U + action + (action_confirm << 8) + ((unsigned)(sel_armed + 1) << 10);
        sig = sig * 33U + cursor;
        sig = sig * 33U + source_preview;
        sig = sig * 33U + (unsigned)source_result;
        for (unsigned i = 0; i < 7; ++i) sig = sig * 33U + groove_step_setting(track, cursor, i);
        for (unsigned i = 0; i < 3; ++i) sig = sig * 33U + groove_sequence_setting(i);
        sig = sig * 33U + groove_sequence_count_in();
        sig = sig * 33U + frame_views[track].volume;
        sig = sig * 33U + frame_views[track].next_length + (unsigned)chain_on * 977U;
        if (notice) sig = signature(sig, notice, (unsigned)strlen(notice));
        if (visible == FM_QUICK) {
            sig = sig * 33U + quick_row; /* SELECT moves the lit window */
            sig = signature(sig, frame_patch, 156);
            sig = sig * 33U + frame_views[track].program;
            sig = sig * 33U + frame_views[track].modified;
            sig = signature(sig, &frame_controls, sizeof frame_controls);
        }
        if (visible == ARP_EDIT) {
            sig = sig * 33U + track + (arp_enabled(track) << 8) + (arp_page << 9) + (keys_latch << 10);
            for (unsigned i = 0; i < ARP_ALL; ++i) sig = sig * 33U + arp_get(track, i);
            sig = sig * 33U + groove_sequence_setting(0);
        }
        /* user 13.11: the held keys and the beat no longer repaint the whole page, only the chord plate, the
         * pattern pills, or the two pills whose light moves (arp_damage) */
        if (cache->middle_cache == sig) { if (visible == ARP_EDIT) arp_damage(0); return; }
        cache->middle_cache = sig;
        if (visible == ARP_EDIT) arp_damage(1); /* the full repaint below covers them: just take the state */
    }
    paint_fill(0, 44, 240, 196, BACK); /* below the v6 top bar */
    cache->top_cache[1] = ~0U; /* the wipe took the header's second row: draw it again */
    memset(cache->foot_cache, 0xff, sizeof cache->foot_cache);
    if (visible == ACTIONS) {
        /* v12 SEL: a list of five rows (the chosen one lit, SELECT turns the drum), the plate with the
         * step count; the knobs show the row's actions, the one turned right lights in its colour. */
        unsigned row = sel_row();
        {
            char num[12]; decimal(num, (int)frame_views[track].next_length, 1);
            state_plate(num, chain_on != 0, frame_views[track].next_length != frame_views[track].sequence_length ? V_ORANGE : TEXT);
        }
        if (action_confirm) { /* v13 REPLACE DATA?: a red-framed window, SAVE = OK, HOME = CANCEL; no knobs */
            rbox(8, 66, 224, 124, 3, MD_PANEL, V_RED); frame(9, 67, 222, 122, V_RED);
            sq_text(120, 90, "REPLACE DATA?", &lcd26, MD_AMBER, MD_PANEL);
            sq_text(120, 118, "THE COPIED DATA WILL", &lcd17, TEXT, MD_PANEL);
            sq_text(120, 138, "OVERWRITE THIS ONE", &lcd17, TEXT, MD_PANEL);
            sq_text(58, 168, "SAVE = OK", &lcd20, knob_colors[1], MD_PANEL);
            sq_text(172, 168, "HOME = CANCEL", &lcd20, V_RED, MD_PANEL);
            return;
        }
        for (unsigned r = 0; r < SEL_ROWS; ++r) { /* v13: three tiles with the name large, the chosen one under an amber glass cursor */
            unsigned y = 66U + r * 43U;
            rbox(6, y, 228, 38, 2, MD_PANEL, MD_EDGE);
            lcd_center(120, y + 19U - lcd30.digit_top - lcd30.digit_height / 2U, sel_rows[r], &lcd30, TEXT, MD_PANEL);
            if (r == row) glass_cursor(6, y, 228, 38, 5, MD_AMBER);
        }
        for (unsigned i = 0; i < 4; ++i) {
            if (sel_grid[row][i] == 255) continue;
            if (sel_armed == (int)i) knob(i, "", sel_items[row][i], colors[i]);
            else knob(i, sel_items[row][i], "", colors[i]);
        }
    } else if (visible == ARP_EDIT) {
        /* v13: the state plate shows ARP ON / OFF. Page 1: the mode's name large in the window, the pattern as
         * bars (the sounding one lit); RATE GATE ARP LATCH on the knobs, the mode on PRESETS, octaves on ALGORITHM.
         * Page 2 (second ARP press): RANDOM with the four polyrhythm variants (CHANCE); MUTATE SWING VAR CHANCE. */
        unsigned on = arp_enabled(track) != 0;
        char kv[4][12];
        (void)on;
        { /* user 09.11 (round 4): the chord of the held keys instead of ON / OFF (ARP on shows as A in F A S L); empty while none */
            char chord[8]; uint8_t held[8]; unsigned hn = arp_held_notes(held);
            if (hn > 1) (void)chord_name(held, hn, chord, 0, 0); else if (hn) note_name(held[0], chord); else chord[0] = 0;
            chord_plate(chord);
        }
        if (!arp_page) {
            struct arp_pills ap; arp_pills_get(&ap);
            char mode[12];
            arp_value_name(track, ARP_MODE, mode);
            window_draw(mode, MD_AMBER);
            for (unsigned i = 0; i < ap.n; ++i) /* v13: coloured glass pills, the sounding one amber */
                glass_bar_h(arp_pill_x(&ap, i), arp_pill_y(&ap, i), ap.pitch - 6U, 12, 256, i == ap.lit ? MD_AMBER : knob_colors[3]);
            arp_value_name(track, ARP_RATE, kv[0]); knob(0, arp_param_name(ARP_RATE), kv[0], colors[0]);
            arp_value_name(track, ARP_GATE, kv[1]); knob(1, arp_param_name(ARP_GATE), kv[1], colors[1]);
            knob(2, "LATCH", keys_latch ? "ON" : "OFF", colors[2]); /* v13: K3 LATCH, K4 ARP (swapped) */
            knob(3, "ARP", on ? "ON" : "OFF", colors[3]);
        } else {
            window_draw("RANDOM", MD_AMBER);
            unsigned chance = arp_get(track, ARP_CHANCE);
            for (unsigned v = 0; v < 4; ++v) {
                uint8_t pat[7];
                unsigned n = arp_poly_pattern(v, pat), x = 10U + v * 55U;
                rbox(x, 112, 51, 72, 2, MD_PANEL, MD_EDGE);
                for (unsigned i = 0; i < n; ++i) {
                    unsigned bx = x + 4U + i * 43U / n, bw = (i + 1U) * 43U / n - i * 43U / n;
                    glass_bar_h(bx, 170U - pat[i] * 14U, bw > 2U ? bw - 1U : bw, 8, 256, knob_colors[v]);
                }
                char count[2] = {(char)('0' + n), 0};
                lcd_center((int)x + 25, 115, count, &lcd13, V_DIM, MD_PANEL);
                if (chance == v + 1U) glass_cursor(x, 112, 51, 72, 4, MD_AMBER);
            }
            arp_value_name(track, ARP_MUTATE, kv[0]); knob(0, "MUTATE", kv[0], colors[0]);
            decimal(kv[1], (int)groove_sequence_setting(0), 1); knob(1, "SWING", kv[1], colors[1]); /* the sequencer's stored swing, as GLOBAL had it */
            arp_value_name(track, ARP_VAR, kv[2]); knob(2, "VAR", kv[2], colors[2]);
            arp_value_name(track, ARP_CHANCE, kv[3]); knob(3, "CHANCE", kv[3], colors[3]);
        }
    } else {
        /* v9 EDIT: two windows, TONE and ENV, filling the box; SELECT picks
         * the window, the knobs change the lit one (no bank, the name is in the header). */
        static const char *const row_names[2] = {"TONE", "ENVELOPE"};
        static const char *const labels[2][4] = {{"ALGO", "FEEDBACK", "TRANS", "LEVEL"}, {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"}};
        int values[2][4] = {{frame_patch[134] + 1, frame_patch[135], (int)frame_patch[144] - 24, frame_views[track].volume},
            {frame_controls.envelope[0], frame_controls.envelope[1], frame_controls.envelope[2], frame_controls.envelope[3]}};
        char text_v[2][4][12];
        for (unsigned r = 0; r < 2; ++r)
            for (unsigned i = 0; i < 4; ++i) {
                if (r == 0 && i == 2 && values[r][i] > 0) { text_v[r][i][0] = '+'; decimal(text_v[r][i] + 1, values[r][i], 1); }
                else decimal(text_v[r][i], values[r][i], 1);
            }
        if (!(paint_rendering && (paint_top >= 62 || paint_top + PAINT_ROWS <= 26))) { /* v13: the algorithm number in the state plate */
            uint32_t psig = (uint32_t)frame_patch[134] | 0x52000U;
            if (psig != cache->row_cache[1]) {
                cache->row_cache[1] = psig;
                char num[12]; decimal(num, (int)(frame_patch[134] & 31U) + 1, 1);
                state_plate_cap(num, "ALGO");
            }
        }
        if (!(paint_rendering && (paint_top >= 195 || paint_top + PAINT_ROWS <= 62))) {
            uint32_t sig = quick_row | 0x51000U;
            if (sig != cache->row_cache[0]) {
                cache->row_cache[0] = sig;
                paint_fill(0, 62, 240, 133, BACK);
                for (unsigned r = 0; r < 2; ++r) { /* v13: two flat windows 228 x 60 at y 66 / 130, the name centred, an icon box over each knob stick */
                    unsigned y = 66 + r * 64, on = r == quick_row;
                    rbox(6, y, 228, 60, 2, MD_PANEL, MD_EDGE);
                    lcd_center(120, y + 3, row_names[r], &lcd17, on ? MD_AMBER : V_DIM, MD_PANEL);
                    for (unsigned i = 0; i < 4; ++i) { /* 48 x 30, the width and place of the knob's coloured stick; the numbers are under the sticks */
                        unsigned bx = 6 + i * 60;
                        rbox(bx, y + 25, 48, 30, 4, 0x18a3, knob_colors[i]);
                        dx_icons[r][i]((int)bx + 24, (int)y + 40, knob_colors[i]);
                    }
                    if (on) glass_cursor(6, y, 228, 60, 5, MD_AMBER);
                }
            }
        }
        for (unsigned i = 0; i < 4; ++i) knob(i, labels[quick_row][i], text_v[quick_row][i], colors[i]);
    }
}
/* ---- v13 splash (docs/mockups/FM1-v13-screens.html, splashFrame): four thin waves appear one after another in the
 * slots of the later start notes (F A C3 F3, no sound now: the chime stays as it is), brightest in the centre and
 * fading to the edges, a note slot flashes its wave. Then the name MPC FM-1 (Righteous) fades in at its place and
 * last the love line (Great Vibes, pink at the bottom to turquoise at the top). The two fonts are pre-rendered
 * 4-bit bitmaps (splash_art.h). boot_phase counts 65 ms; the logo never blocks control or audio, any key skips it. ---- */
#include "splash_art.h"
enum { BOOT_CY = 122, BOOT_W0 = 5, BOOT_W = 230 };
static const uint16_t boot_note_ms[4] = {300, 833, 1900, 2700}; /* wave 2 with chime note 2 (user 10.11, F10: 1/3 of the 800 ms step earlier) */
static int16_t boot_wave_y[4][240]; /* 1/8 px, per column */
static uint8_t boot_flash[4], boot_appear[4]; /* 0..255 each */
static unsigned boot_wave_phase = ~0U;
static unsigned boot_ease(int ms, int len) /* smoothstep 0..256 */
{
    if (ms <= 0) return 0;
    if (ms >= len) return 256;
    int u = ms * 256 / len;
    return (unsigned)((u * u * (3 * 256 - 2 * u)) >> 16);
}
static void boot_waves(unsigned t)
{
    static const uint8_t fl[16] = {255, 205, 165, 133, 107, 86, 69, 56, 45, 36, 29, 23, 19, 15, 12, 10}; /* exp(-dt / 0.3 s), 65 ms steps */
    float ts = (float)t * 0.001f;
    for (unsigned k = 0; k < 4; ++k) {
        boot_appear[k] = 0; boot_flash[k] = 0;
        if (t < boot_note_ms[k]) continue;
        int dt = (int)(t - boot_note_ms[k]);
        boot_appear[k] = (uint8_t)(boot_ease(dt, 280) > 255U ? 255U : boot_ease(dt, 280));
        unsigned fi = (unsigned)dt / 65U;
        boot_flash[k] = fi < 16U ? fl[fi] : 0;
        float ph = ((float)k + ts * (0.9f + 0.15f * (float)k)) * 0.15915494f, amp = (float)(16 + 7 * (int)k) * (1.0f + 0.3f * (float)boot_flash[k] / 255.0f);
        for (int x = 0; x < (int)BOOT_W; ++x) {
            float u = (float)x / (float)BOOT_W;
            float y = (float)BOOT_CY + vsin(u * 3.0f + ph) * amp * vsin(u * 0.5f);
            boot_wave_y[k][BOOT_W0 + x] = (int16_t)(y * 8.0f);
        }
    }
}
/* alpha 0..256 of the horizontal gradient at wave column x (mockup waveGrad: 0 / .35 / .85 / 1 / .85 / .35 / 0 at
 * 0 / .2 / .38 / .5 / .62 / .8 / 1) times the edge shade of the vignette, and the white mix 0..128 of the centre */
static unsigned boot_grad(int x, unsigned *light)
{
    static const uint16_t sx[7] = {0, 46, 87, 115, 143, 184, 230}, sa[7] = {0, 90, 218, 256, 218, 90, 0};
    int u = x - BOOT_W0;
    if (u < 0 || u > (int)BOOT_W) { *light = 0; return 0; }
    unsigned i = 0;
    while (i < 5 && (unsigned)u >= sx[i + 1]) ++i;
    unsigned a = sa[i] + (unsigned)((int)(sa[i + 1] - (int)sa[i]) * (u - (int)sx[i]) / (int)(sx[i + 1] - sx[i]));
    int d = u < 115 ? 115 - u : u - 115;
    *light = d < 28 ? (unsigned)(28 - d) * 128U / 28U : 0U;
    int edge = x < 48 ? 48 - x : x > 192 ? x - 192 : 0; /* vignette: .75 black at the screen edge */
    return a * (unsigned)(256 - edge * 192 / 48) / 256U;
}
/* One wave: a soft glow, a thin core and a white flash line, vertical spans between columns. */
static void boot_wave_draw(unsigned k)
{
    if (!paint_rendering || !boot_appear[k]) return;
    static const int16_t reach[3] = {24, 7, 4}; /* 1/8 px: 3, .85 and .5 px each side */
    unsigned stride = paint_end - paint_x;
    for (int x = BOOT_W0; x < (int)(BOOT_W0 + BOOT_W); ++x) {
        if (x < (int)paint_x || x >= (int)paint_end) continue;
        unsigned light, ga = boot_grad(x, &light);
        if (!ga) continue;
        ga = ga * boot_appear[k] / 255U;
        int y = boot_wave_y[k][x], yp = x > BOOT_W0 ? boot_wave_y[k][x - 1] : y;
        int lo = y < yp ? y : yp, hi = y < yp ? yp : y;
        uint16_t core = light ? glass_mix(knob_colors[k], 0xffff, light) : knob_colors[k];
        for (unsigned layer = 0; layer < 3; ++layer) {
            unsigned la = layer == 0 ? ga * 51U / 256U : layer == 1 ? ga : ga * boot_flash[k] / 512U;
            if (!la) continue;
            int l = lo - reach[layer], h = hi + reach[layer], r0 = l >> 3, r1 = h >> 3;
            if (r1 < (int)paint_top || r0 >= (int)(paint_top + PAINT_ROWS)) continue;
            if (r0 < (int)paint_top) r0 = (int)paint_top;
            if (r1 >= (int)(paint_top + PAINT_ROWS)) r1 = (int)(paint_top + PAINT_ROWS) - 1;
            uint16_t col = layer == 2 ? 0xffff : layer == 1 ? core : knob_colors[k];
            for (int py = r0; py <= r1; ++py) {
                int c0 = py * 8 > l ? py * 8 : l, c1 = py * 8 + 8 < h ? py * 8 + 8 : h;
                if (c1 <= c0) continue;
                unsigned a = (unsigned)(c1 - c0) * la / 8U;
                uint16_t *px = paint_pixels + (unsigned)(py - (int)paint_top) * stride + (unsigned)(x - (int)paint_x);
                *px = glass_mix(*px, col, a > 256U ? 256U : a);
            }
        }
    }
}
/* the four knob colours as a gradient: f 0..256 over three steps */
static uint16_t boot_ramp(unsigned f)
{
    unsigned seg = f * 3U / 257U, rest = f * 3U - seg * 256U;
    return glass_mix(knob_colors[seg], knob_colors[seg + 1U], rest > 256U ? 256U : rest);
}
/* A pre-rendered 4-bit bitmap mixed over the band buffer. title: colour by column (pink - green - yellow - turquoise)
 * with a lighter top; otherwise colour by row, pink at the bottom to turquoise at the top. alpha 0..256. */
static void splash_blit(const struct splash_art *a, unsigned alpha, unsigned title)
{
    if (!paint_rendering) { paint_mark(a->x0, a->y0, a->w, a->h); return; }
    if ((int)a->y0 >= (int)(paint_top + PAINT_ROWS) || (int)(a->y0 + a->h) <= (int)paint_top) return;
    unsigned stride = paint_end - paint_x;
    uint16_t cc[256]; /* title: the colour of each column */
    if (title) for (unsigned c = 0; c < a->w; ++c) cc[c] = boot_ramp(c * 256U / (a->w - 1U));
    for (unsigned r = 0; r < a->h; ++r) {
        unsigned py = a->y0 + r;
        if (py < paint_top || py >= paint_top + PAINT_ROWS) continue;
        uint16_t rowc = title ? 0 : boot_ramp(256U - r * 256U / (a->h - 1U));
        unsigned shine = title && py < 43U ? 41U : 0U;
        for (unsigned c = 0; c < a->w; ++c) {
            unsigned px = a->x0 + c;
            if (px < paint_x || px >= paint_end) continue;
            unsigned i = r * a->w + c, cov = (a->bits[i >> 1] >> ((i & 1U) ? 0 : 4)) & 15U;
            if (!cov) continue;
            uint16_t col = title ? cc[c] : rowc;
            if (shine) col = glass_mix(col, 0xffff, shine);
            uint16_t *p = paint_pixels + (py - paint_top) * stride + (px - paint_x);
            *p = glass_mix(*p, col, cov * alpha / 15U);
        }
    }
}
static __attribute__((noinline, cold, minsize)) void boot_draw(void)
{
    if (cache->middle_cache == boot_phase) return;
    cache->middle_cache = boot_phase;
    unsigned t = boot_phase * 65U;
    if (boot_wave_phase != boot_phase) { boot_wave_phase = boot_phase; boot_waves(t); }
    paint_fill(0, 20, 240, 200, BACK);
    for (unsigned k = 0; k < 4; ++k) boot_wave_draw(k);
    unsigned at = boot_ease((int)t - 3400, 1100);
    if (at) splash_blit(&splash_title, at, 1);
    unsigned al = boot_ease((int)t - 4200, 800);
    if (al) splash_blit(lang_get() ? &splash_love_ru : &splash_love_en, al, 0);
}
static void draw(const struct fm1_ble_status *ble);
/* SCOPE (v13 mockup scopeV13): seven frequency bands as arcs round a pixel cat; band b has base radius 56 + 10 b px,
 * amplitude (20 + 4.3 b) px x level, shape sine -> triangle (sh = b / 6) to the power 1 + 2.2 sh, stroke 4.6 -> 1.3 px.
 * A round mirror ball hangs from the top edge, soft coloured spots drift behind. Master FX copies the output only while
 * this page is open; the band split runs here once per frame (512 samples x 6 two-pole low-passes). */
enum { SCOPE_N = 512, SCOPE_ARC = 96, SCOPE_CX = 120, SCOPE_CY = 236, SCOPE_SPOTS = 18 };
static uint8_t scope_level[7], scope_slow[7], scope_coat, scope_sleep, scope_fresh;
static uint32_t scope_loud_ms, scope_at, scope_slow_at;
static const struct groove_view *scope_view;
static int16_t scope_r[7][SCOPE_ARC + 1]; /* arc radius per angle, 1/10 px */
static float scope_cos[SCOPE_ARC + 1], scope_sin[SCOPE_ARC + 1];
static int16_t scope_spot_x[SCOPE_SPOTS], scope_spot_y[SCOPE_SPOTS];
/* mockup SC[]: red, orange, yellow, green, turquoise, blue, purple */
static const uint16_t scope_colors[7] = {0xf9c6, 0xfc26, PAL_YELLOW, PAL_GREEN, PAL_TURQ, PAL_BLUE, PAL_PURPLE};
static const uint16_t scope_spot_colors[5] = {0xf9b1, 0x1e7f, 0xfe80, 0xffff, 0xb15f}; /* #ff2f8a #10c4ff #ffd000 white #b050ff */
static void scope_frame(void)
{
    static int16_t s[SCOPE_N];
    static const int32_t a[6] = {830, 1991, 4351, 8817, 16233, 24885}; /* 90, 220, 500, 1100, 2400, 5000 Hz at 22 kHz, Q15 */
    static const uint8_t gain[7] = {8, 8, 8, 9, 10, 12, 14}; /* x/8: the highs carry less energy */
    if (!scope_cos[0])
        for (unsigned k = 0; k <= SCOPE_ARC; ++k) { /* upper half, left to right */
            float c = 0.5f + (float)k / (2.0f * SCOPE_ARC);
            scope_cos[k] = pic_sin(c + 0.25f); scope_sin[k] = pic_sin(c);
        }
    mfx_scope_read(s, SCOPE_N);
    int32_t y[6], z[6];
    uint32_t sum[7] = {0};
    for (unsigned k = 0; k < 6; ++k) y[k] = z[k] = (int32_t)s[0] << 4;
    for (unsigned i = 0; i < SCOPE_N; ++i) {
        int32_t x = (int32_t)s[i] << 4, prev = 0;
        for (unsigned k = 0; k < 6; ++k) {
            y[k] += (int32_t)(((int64_t)(x - y[k]) * a[k]) >> 15);
            z[k] += (int32_t)(((int64_t)(y[k] - z[k]) * a[k]) >> 15);
            int32_t band = z[k] - prev; prev = z[k];
            sum[k] += (uint32_t)(band < 0 ? -band : band);
        }
        int32_t hi = x - prev; sum[6] += (uint32_t)(hi < 0 ? -hi : hi);
    }
    unsigned total = 0;
    for (unsigned b = 0; b < 7; ++b) {
        uint32_t lvl = (sum[b] >> 13) * gain[b] / 8U; /* mean |band|, full scale 32768 */
        if (lvl > 20000U) lvl = 20000U;
        unsigned e = (unsigned)((uint64_t)lvl * lvl * 256U / ((uint64_t)lvl * lvl + 1100U * 1100U)); /* reacts to quieter bands */
        if (e > 255U) e = 255U;
        total += e;
        /* fast rise, faster fall: no lag behind the music */
        scope_level[b] = (uint8_t)(e >= scope_level[b] ? e : scope_level[b] - (scope_level[b] - e + 2U) / 3U);
    }
    if (frame_time - scope_slow_at >= 100U) { scope_slow_at = frame_time; memcpy(scope_slow, scope_level, sizeof scope_slow); } /* the ball follows at 10 fps */
    if (total > 12U) scope_loud_ms = frame_time;
    scope_sleep = frame_time - scope_loud_ms > 1500U;
    float t = (float)(frame_time % 600000U) / 1000.0f;
    for (unsigned b = 0; b < 7; ++b) {
        float e = scope_sleep ? 0.0f : (float)scope_level[b] / 255.0f, sh = (float)b / 6.0f, ex = 1.0f + 2.2f * sh;
        int n = (int)ex;
        float f = ex - (float)n, amp = (20.0f + 4.3f * (float)b) * e, speed = t * (0.5f + 0.2f * (float)b);
        for (unsigned k = 0; k <= SCOPE_ARC; ++k) {
            float ph = (float)k / (float)SCOPE_ARC * (1.5f + 0.35f * (float)b) + speed;
            float m = (1.0f + pic_sin(ph)) * 0.5f, q = ph + 0.25f;
            q -= (float)(int)q; if (q < 0) q += 1.0f;
            float u = 2.0f * q - 1.0f, tr = 1.0f - (u < 0 ? -u : u); /* triangle, 1 at the crest */
            float x = (1.0f - sh) * m + sh * tr, w = 1.0f;
            for (int i = 0; i < n; ++i) w *= x;
            w *= 1.0f - f + f * x; /* x^(1 + 2.2 sh), the fraction interpolated */
            scope_r[b][k] = (int16_t)((56.0f + 10.0f * (float)b + w * amp) * 10.0f);
        }
    }
    for (unsigned k = 0; k < SCOPE_SPOTS; ++k) { /* background spots, mockup scopeV13 */
        float an = (t * 0.5f + (float)k * 0.61f) * 0.15915494f, d = (float)(30 + (int)((k * 37U) % 90U));
        scope_spot_x[k] = (int16_t)(120.0f + pic_sin(an + 0.25f) * d * 1.5f);
        scope_spot_y[k] = (int16_t)(110.0f + pic_sin(an * 1.3f + (float)k * 0.159f) * d * 0.6f);
    }
}
/* soft round spot: alpha falls from a40 (0..40) in the centre to 0 at radius 6 */
static void scope_spot(int cx, int cy, uint16_t c, unsigned a40, int top, int bot)
{
    for (int dy = -6; dy <= 6; ++dy) {
        int y = cy + dy;
        if (y < top || y >= bot) continue;
        for (int dx = -6; dx <= 6; ++dx) {
            int d2 = dx * dx + dy * dy;
            if (d2 < 36) paint_blend(cx + dx, y, c, a40 * (unsigned)(36 - d2) / 36U);
        }
    }
}
/* the mirror ball: 11 x 11 facets on a sphere (their width falls with the cosine), lit from the upper left, dark rim,
 * white glint stars, and while music plays coloured sparks of the loud bands */
static void scope_ball(int top, int bot, unsigned mode, float ts)
{
    enum { CX = 120, CY = 38, R = 27 };
    static float ps[11], pc[11], ls[11], lc[11];
    if (!ps[10]) for (int i = 0; i < 11; ++i) {
        ps[i] = pic_sin((float)(i - 5) * 0.27f * 0.15915494f); pc[i] = pic_sin((float)(i - 5) * 0.27f * 0.15915494f + 0.25f);
        ls[i] = pic_sin((float)(i - 5) * 0.3f * 0.15915494f); lc[i] = pic_sin((float)(i - 5) * 0.3f * 0.15915494f + 0.25f);
    }
    if (top >= CY + R + 8) return;
    paint_fill(CX, 0, 1, CY - R + 1, 0x6b4d); /* the string */
    fill_ellipse(CX * 10, CY * 10, R * 10 + 4, R * 10 + 4, 0x4a6b); /* rim line */
    fill_ellipse(CX * 10, CY * 10, R * 10 - 4, R * 10 - 4, 0x2947);
    for (int j = 0; j < 11; ++j)
        for (int i = 0; i < 11; ++i) {
            float fx = (float)R * pc[j] * ls[i], fy = (float)R * ps[j], w = (float)R * 0.246f * pc[j] * lc[i], h = (float)R * 0.2214f * pc[j];
            if (w < 0.5f) continue;
            float ax = (fx < 0 ? -fx : fx) + w * 0.5f, ay = (fy < 0 ? -fy : fy) + h * 0.5f;
            if (ax * ax + ay * ay > (float)((R + 1) * (R + 1))) continue; /* corners stay inside the circle */
            int y0 = (int)(CY + fy - h * 0.5f + 0.5f), y1 = (int)(CY + fy + h * 0.5f + 0.5f);
            if (y1 <= y0) y1 = y0 + 1;
            if (y1 <= top || y0 >= bot) continue;
            int x0 = (int)(CX + fx - w * 0.5f + 0.5f), x1 = (int)(CX + fx + w * 0.5f + 0.5f);
            if (x1 <= x0) x1 = x0 + 1;
            float lit = -ls[i] * pc[j] * 0.55f - ps[j] * 0.6f + pc[j] * lc[i] * 0.6f;
            if (lit < 0) lit = 0;
            float gl = (pic_sin(((float)(i - 5) * 1.9f + (float)(j - 5) * 2.7f + ts * 4.2f) * 0.15915494f) + 1.0f) * 0.5f;
            float v = 40.0f + 110.0f * lit + (mode ? 70.0f : 25.0f) * gl * gl;
            float d2 = (fx * fx + fy * fy) / (float)(R * R);
            if (d2 > 0.3f) v *= 1.0f - 0.6f * (d2 - 0.3f) / 0.7f; /* rim shading */
            unsigned c8 = v > 255.0f ? 255U : (unsigned)v, b8 = c8 + 26U > 255U ? 255U : c8 + 26U;
            paint_fill((unsigned)x0, (unsigned)y0, (unsigned)(x1 - x0), (unsigned)(y1 - y0),
                       (uint16_t)(((c8 >> 3) << 11) | ((c8 >> 2) << 5) | (b8 >> 3)));
        }
    /* glint stars: a cross and a shorter diagonal cross */
    unsigned n = mode ? 3U : 1U, ga = mode ? 34U : 16U;
    int glen = mode ? 7 : 4;
    for (unsigned q = 0; q < n; ++q) {
        float an = (float)q * 2.1f + ts * 0.7f, u = (float)q * 0.43f + ts * 0.15f, rad = (float)R * (0.25f + 0.5f * (u - (float)(int)u));
        int gx = CX + (int)(pic_sin(an * 0.15915494f + 0.25f) * rad), gy = CY + (int)(pic_sin(an * 0.15915494f) * rad * 0.9f);
        for (int d = -glen; d <= glen; ++d) {
            unsigned fa = ga * (unsigned)(glen + 1 - (d < 0 ? -d : d)) / (unsigned)(glen + 1);
            paint_blend(gx + d, gy, 0xffff, fa); paint_blend(gx, gy + d, 0xffff, fa);
            if (d >= -glen * 6 / 10 && d <= glen * 6 / 10) { paint_blend(gx + d, gy + d, 0xffff, fa / 2U); paint_blend(gx + d, gy - d, 0xffff, fa / 2U); }
        }
    }
    if (mode)
        for (unsigned q = 0; q < 8; ++q) {
            unsigned b = (q * 3U + (unsigned)(ts * 1.6f)) % 7U;
            if (scope_slow[b] < 64U) continue;
            float an = (float)q * 2.399f + ts * 0.9f + (float)b, u = (float)q * 0.37f + ts * 0.2f, rad = (float)R * (0.2f + 0.62f * (u - (float)(int)u));
            int sx = CX + (int)(pic_sin(an * 0.15915494f + 0.25f) * rad), sy = CY + (int)(pic_sin(an * 0.15915494f) * rad * 0.95f);
            scope_spot(sx, sy, scope_colors[b], 30, top, bot);
            paint_fill((unsigned)(sx - 1), (unsigned)(sy - 1), 2, 2, mix565(scope_colors[b], 0xffff, 20U));
        }
}
static void scope_draw(const struct groove_view *v)
{
    /* v13: below BPM / PLAY / battery the page is the scope only. Damage is marked once per scope frame
     * (the slow parts, ball and spots, change at 10 fps and the block compare drops the rest). */
    if (!paint_rendering) {
        if (!scope_fresh) return;
        scope_fresh = 0;
        paint_fill(0, 26, 240, 214, BACK);
        paint_mark(88, 0, 64, 26); /* the ball hangs over the empty middle of the header */
        return;
    }
    const int top = (int)paint_top, bot = (int)paint_top + PAINT_ROWS;
    paint_fill(0, 26, 240, 214, BACK);
    float ts = (float)((frame_time / 100U) % 6000U) * 0.1f; /* the slow parts: 100 ms steps */
    unsigned mode = !scope_sleep;
    for (unsigned k = 0; k < SCOPE_SPOTS; ++k) {
        int x = scope_spot_x[k], y = scope_spot_y[k];
        if (y < 64 || y > 190 || x < 2 || x > 236) continue;
        scope_spot(x, y, scope_spot_colors[k % 5U], mode ? 17U : 6U, top, bot);
    }
    /* seven bands: a soft wide glow first, then the stroke, both as chains of round dots between the arc points */
    static const uint8_t wid[7] = {46, 40, 34, 28, 22, 17, 13}; /* stroke width in 1/10 px */
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned b = 0; b < 7; ++b) {
            int rd = (wid[b] + (pass ? 0 : 40)) / 2, sp = rd < 7 ? 7 : rd;
            uint16_t col = pass ? scope_colors[b] : mix565(BACK, scope_colors[b], 6U);
            int px = SCOPE_CX * 10 + (int)(scope_cos[0] * scope_r[b][0]), py = SCOPE_CY * 10 + (int)(scope_sin[0] * scope_r[b][0]);
            for (unsigned k = 1; k <= SCOPE_ARC; ++k) {
                int nx = SCOPE_CX * 10 + (int)(scope_cos[k] * scope_r[b][k]), ny = SCOPE_CY * 10 + (int)(scope_sin[k] * scope_r[b][k]);
                int lo = (py < ny ? py : ny) - rd, hi = (py < ny ? ny : py) + rd;
                if (hi >= top * 10 - 10 && lo < bot * 10 + 10) {
                    int dx = nx - px, dy = ny - py, n = (int)isqrt_ui((unsigned)(dx * dx + dy * dy)) / sp + 1;
                    for (int i = 0; i < n; ++i) fill_ellipse(px + dx * i / n, py + dy * i / n, rd, rd, col);
                }
                px = nx; py = ny;
            }
        }
    scope_ball(top, bot, mode, ts);
    /* sleeping: Z z beside the cat (mockup: 148,176 and 157,168), drifting up */
    if (scope_sleep) {
        unsigned z = (frame_time / 80U) % 12U;
        lcd_text(148, 176U - z / 2U, "Z", &lcd10, TEXT, BACK);
        lcd_text(157, 168U - (z + 6U) % 12U / 2U, "z", &lcd9, V_DIM, BACK);
    }
    /* the cat dances on the beat: paws up every other beat, turns every two */
    if (bot > 186) {
        unsigned bpm = v->tempo ? v->tempo : 120U, beat = (unsigned)((uint64_t)frame_time * bpm / 60000U);
        if (scope_sleep) cat_v6(SCOPE_CX - 24, SCOPE_CY - 48, 3, 0, 0, 1, scope_coat);
        else cat_v6(SCOPE_CX - 24, SCOPE_CY - 50, 3, beat & 1U, (beat >> 1) & 1U, 0, scope_coat);
    }
}
static void paint_flush(void);
/* ---- SAFE mode (app/boot_guard.c): nothing user-made is loaded ---- */
static const char *safe_note;
static uint32_t safe_save_ms, safe_rom_ms;
static __attribute__((noinline, cold, minsize)) void safe_draw(void)
{
    if (cache->title_cache != 0) {
        cache->title_cache = 0;
        char why[40];
        if (boot_guard_reason() == BOOT_GUARD_KEYS) strcpy(why, "OCT- HELD AT POWER ON");
        else { decimal(why, (int)boot_guard_failures(), 1); strcat(why, " FAILED STARTS IN A ROW"); }
        /* v13: what happened, then three tiles with a coloured edge: the way back and the two holds. */
        lcd_center(120, 6, "SAFE MODE", &lcd30, PAL_AMBER, BACK);
        lcd_center(120, 42, why, &lcd16, TEXT, BACK);
        static const char *const head[3] = {"POWER OFF AND ON", "HOLD SAVE 2 S", "HOLD OCT- AND OCT+ 3 S"};
        static const char *const sub[3] = {"= NORMAL START", "FORGET FAILED STARTS", "USB DOWNLOAD MODE"};
        static const uint16_t tint[3] = {PAL_GREEN, PAL_TURQ, PAL_YELLOW};
        for (unsigned k = 0; k < 3; ++k) {
            unsigned y = 84U + k * 50U;
            rbox(10, y, 220, 44, 3, MD_PANEL, MD_EDGE);
            rbox(10, y, 5, 44, 1, tint[k], V_NONE);
            lcd_text(26, y + 2, head[k], &lcd20, tint[k], MD_PANEL);
            lcd_text(26, y + 25, sub[k], &lcd14, V_DIM, MD_PANEL);
        }
    }
    uint32_t sig = safe_note ? signature(9U, safe_note, (unsigned)strlen(safe_note)) : 1U;
    if (sig == cache->middle_cache) return;
    cache->middle_cache = sig;
    paint_fill(0, 60, 240, 22, BACK); /* the line under the title: the note, or what is not loaded */
    if (safe_note) lcd_center(120, 62, safe_note, &lcd16, PAL_AMBER, BACK);
    else lcd_center(120, 62, "SONGS AND SETTINGS NOT LOADED", &lcd13, V_DIM, BACK);
}
static void safe_control(const struct fm1_controls *c, uint32_t now)
{
    const uint32_t save = BUTTON(SAVE), octs = BUTTON(OCT_DOWN) | BUTTON(OCT_UP);
    if ((c->buttons & save) == save && !(c->buttons & ~save)) {
        if (!safe_save_ms) safe_save_ms = now | 1U;
        else if (safe_save_ms != ~0U && now - safe_save_ms >= 2000U) {
            safe_note = boot_guard_clear() ? "COULD NOT CLEAR - TRY AGAIN" : "CLEARED. POWER OFF AND ON.";
            safe_save_ms = ~0U; /* once per hold */
        }
    } else safe_save_ms = 0;
    if ((c->buttons & octs) == octs && !(c->buttons & ~octs)) {
        if (!safe_rom_ms) safe_rom_ms = now | 1U;
        else if (now - safe_rom_ms >= 3000U) {
            safe_note = "USB DOWNLOAD MODE...";
            cache->middle_cache = ~0U;
            draw(&frame_ble);
            paint_flush();
            boot_guard_enter_rom();
        }
    } else safe_rom_ms = 0;
}
#if FM1_BINAURAL
/* BINAURAL and 3D SOUND icons (v11 knob glyphs), smooth strokes like the
 * engine pictograms. Unit grid about +-10 x +-8; k = tenths of a pixel per
 * unit (10 = 1 px); the stroke grows a little with k. */
enum { BI_BRAIN, BI_EAR, BI_TIMER, BI_VOL, BI_PHONES, BI_WIDTH, BI_FEED, BI_MIX, BI_OFF,
       BI_DELTA, BI_THETA, BI_ALPHA, BI_BETA, BI_FOCUS, BI_CUSTOM, BI_END, BI_PINK, BI_RAIN };
static int bi_r = 10; /* stroke radius, 1/10 px */
/* Plain pixel strokes, 2 px (3 px for the large signs), like the MICRO D path
 * icons. A chain of smooth dots looked softer but cost several ms per 16-row
 * band (every band is drawn again from scratch), so a knob change on the
 * device took tens of ms and the cells were seen half drawn or late. */
static void bi_seg(int x0, int y0, int x1, int y1, uint16_t c)
{
    int t = bi_r >= 14 ? 3 : 2, ax = (x0 + 5) / 10, ay = (y0 + 5) / 10, bx = (x1 + 5) / 10, by = (y1 + 5) / 10;
    int adx = bx > ax ? bx - ax : ax - bx, ady = by > ay ? by - ay : ay - by;
    for (int o = 0; o < t; ++o) {
        int ox = ady > adx ? o - t / 2 : 0, oy = ady > adx ? 0 : o - t / 2;
        line(ax + ox, ay + oy, bx + ox, by + oy, c);
    }
}
static void bi_poly(int cx, int cy, int k, const int8_t (*p)[2], unsigned n, uint16_t c) /* points in half units */
{
    for (unsigned i = 0; i + 1 < n; ++i) bi_seg(cx + p[i][0] * k / 2, cy + p[i][1] * k / 2, cx + p[i + 1][0] * k / 2, cy + p[i + 1][1] * k / 2, c);
}
/* part of an ellipse (centre ox, oy, radii rx, ry in units), from turn t0 to t1; t 0.25 = top */
static void bi_arc(int cx, int cy, int k, float ox, float oy, float rx, float ry, float t0, float t1, uint16_t c)
{
    int px = 0, py = 0;
    for (int i = 0; i <= 16; ++i) {
        float t = t0 + (t1 - t0) * (float)i / 16.0f;
        int x = cx + (int)((ox + rx * vsin(t + 0.25f)) * (float)k), y = cy + (int)((oy - ry * vsin(t)) * (float)k);
        if (i) bi_seg(px, py, x, y, c);
        px = x; py = y;
    }
}
static void bi_wave(int cx, int cy, int k, float x0, float w, float amp, float cycles, uint16_t c)
{
    int px = 0, py = 0;
    for (int i = 0; i <= 20; ++i) {
        float t = (float)i / 20.0f;
        float y = amp > 0 ? amp * vsin(cycles * t) : 3.0f * vsin(1.3f * t) + 1.5f * vsin(3.7f * t + 0.2f); /* amp < 0: pink */
        int x = cx + (int)((x0 + w * t) * (float)k), yy = cy - (int)(y * (float)k);
        if (i) bi_seg(px, py, x, yy, c);
        px = x; py = yy;
    }
}
static void bin_icon(unsigned id, int cx, int cy, int k, uint16_t c, uint16_t bg)
{
    bi_r = 8 + k / 3;
    switch (id) {
    case BI_BRAIN: /* a head with a brain wave in it */
        bi_arc(cx, cy, k, 0, 0, 9, 7.4f, 0.0f, 1.0f, c);
        bi_wave(cx, cy, k, -6.5f, 13, 3, 2, c);
        break;
    case BI_EAR: { /* the carrier: what the ear hears */
        bi_arc(cx, cy, k, 0, -1, 5.4f, 5.4f, 0.5f, 0.0f, c);
        static const int8_t p[4][2] = {{11, -2}, {10, 6}, {7, 13}, {0, 16}};
        bi_poly(cx, cy, k, p, 4, c);
        bi_arc(cx, cy, k, 0, -0.5f, 1.8f, 1.8f, 0.5f, -0.2f, c);
        break;
    }
    case BI_TIMER: { /* a stopwatch */
        bi_arc(cx, cy, k, 0, 1, 6.6f, 6.6f, 0.0f, 1.0f, c);
        static const int8_t p[3][2] = {{0, -8}, {0, 2}, {6, 5}}, q[2][2] = {{-4, -15}, {4, -15}};
        bi_poly(cx, cy, k, p, 3, c); bi_poly(cx, cy, k, q, 2, c);
        break;
    }
    case BI_VOL: { /* a speaker: the noise level */
        static const int8_t p[7][2] = {{-18, -5}, {-8, -5}, {1, -14}, {1, 14}, {-8, 5}, {-18, 5}, {-18, -5}};
        bi_poly(cx, cy, k, p, 7, c);
        bi_arc(cx, cy, k, 0.5f, 0, 4.5f, 4.5f, -0.14f, 0.14f, c);
        bi_arc(cx, cy, k, 0.5f, 0, 8.5f, 8.5f, -0.14f, 0.14f, c);
        break;
    }
    case BI_PHONES: /* headphones: the band and two cups */
        bi_arc(cx, cy, k, 0, 1, 7.6f, 7.6f, 0.5f, 0.0f, c);
        fill_ellipse(cx - 76 * k / 10, cy + 46 * k / 10, 2 * k + 5, 36 * k / 10, c);
        fill_ellipse(cx + 76 * k / 10, cy + 46 * k / 10, 2 * k + 5, 36 * k / 10, c);
        break;
    case BI_WIDTH: { /* <-> */
        static const int8_t p[2][2] = {{-20, 0}, {20, 0}}, a[3][2] = {{-10, -9}, {-20, 0}, {-10, 9}}, b[3][2] = {{10, -9}, {20, 0}, {10, 9}};
        bi_poly(cx, cy, k, p, 2, c); bi_poly(cx, cy, k, a, 3, c); bi_poly(cx, cy, k, b, 3, c);
        break;
    }
    case BI_FEED: /* each ear gets some of the other: two crossing arrows */
        bi_seg(cx - 8 * k, cy - 4 * k, cx + 6 * k, cy + 3 * k, c);
        bi_seg(cx + 8 * k, cy - 4 * k, cx - 6 * k, cy + 3 * k, c);
        fill_tri(cx + 10 * k, cy + 5 * k, cx + 4 * k, cy + 5 * k, cx + 7 * k, cy, c);
        fill_tri(cx - 10 * k, cy + 5 * k, cx - 4 * k, cy + 5 * k, cx - 7 * k, cy, c);
        break;
    case BI_MIX: /* two circles: dry and wet */
        bi_arc(cx, cy, k, -3.8f, 0, 6.2f, 6.2f, 0.0f, 1.0f, c);
        bi_arc(cx, cy, k, 3.8f, 0, 6.2f, 6.2f, 0.0f, 1.0f, c);
        break;
    case BI_OFF: /* power sign */
        bi_arc(cx, cy, k, 0, 0.5f, 5, 5, 0.33f, 1.17f, c);
        bi_seg(cx, cy - 6 * k, cx, cy - 1 * k, c);
        break;
    case BI_DELTA: { /* the Greek letters of the brain waves */
        bi_arc(cx, cy, k, 0, 2.5f, 3.5f, 3.5f, 0.0f, 1.0f, c);
        static const int8_t p[4][2] = {{6, -12}, {-4, -12}, {-4, -6}, {0, -2}};
        bi_poly(cx, cy, k, p, 4, c);
        break;
    }
    case BI_THETA:
        bi_arc(cx, cy, k, 0, 0, 3.5f, 6, 0.0f, 1.0f, c);
        bi_seg(cx - 3 * k, cy, cx + 3 * k, cy, c);
        break;
    case BI_ALPHA:
        bi_arc(cx, cy, k, -1.5f, 1, 3.5f, 3.5f, 0.0f, 1.0f, c);
        bi_seg(cx + 2 * k, cy - 3 * k, cx + 5 * k, cy + 5 * k, c);
        break;
    case BI_BETA: {
        static const int8_t p[11][2] = {{-6, 14}, {-6, -12}, {0, -14}, {6, -10}, {0, -2}, {-6, -2}, {2, -2}, {8, 4}, {4, 10}, {-2, 10}, {-6, 8}};
        bi_poly(cx, cy, k, p, 11, c);
        break;
    }
    case BI_FOCUS: /* a target */
        bi_arc(cx, cy, k, 0, 0, 5.5f, 5.5f, 0.0f, 1.0f, c);
        fill_ellipse(cx, cy, 2 * k, 2 * k, c);
        break;
    case BI_CUSTOM: /* a slider: tuned by hand */
        bi_seg(cx - 7 * k, cy, cx + 7 * k, cy, c);
        fill_ellipse(cx + 3 * k, cy, 3 * k, 3 * k, c);
        break;
    case BI_END: /* stop: the timer ran out */
        fill_tri(cx - 4 * k, cy - 4 * k, cx + 4 * k, cy - 4 * k, cx + 4 * k, cy + 4 * k, c);
        fill_tri(cx - 4 * k, cy - 4 * k, cx - 4 * k, cy + 4 * k, cx + 4 * k, cy + 4 * k, c);
        break;
    case BI_PINK: bi_wave(cx, cy, k, -8, 16, -1, 1, c); break; /* soft, slow noise */
    case BI_RAIN: /* three drops */
        for (int i = -1; i <= 1; ++i) {
            int x = cx + i * 7 * k, y = cy + (i ? -1 : 2) * k;
            fill_ellipse(x, y + 2 * k, 2 * k + 3, 2 * k + 3, c);
            fill_tri(x - 2 * k, y + 2 * k, x + 2 * k, y + 2 * k, x, y - 4 * k, c);
        }
        break;
    }
    (void)bg;
}
/* A v11 knob cell: the knob's glyph in its colour, the value large below it
 * (digits, or a sign followed by digits). */
static unsigned bin_value_icon(unsigned param) /* PRESET / NOISE type / MODE as a sign */
{
    if (param == BIN_PRESET) {
        static const uint8_t ic[BIN_CUSTOM + 1] = {BI_OFF, BI_DELTA, BI_THETA, BI_ALPHA, BI_BETA, BI_FOCUS, BI_CUSTOM};
        char t[12]; binaural_text(BIN_PRESET, t);
        return !strcmp(t, "END") ? BI_END : ic[binaural_preset() % (BIN_CUSTOM + 1U)];
    }
    return !binaural_noise() ? BI_OFF : binaural_noise() > 10U ? BI_RAIN : BI_PINK;
}
static void beat_text(char out[12]) /* 10, 10.5, 0.5: no ".0" */
{
    unsigned b = binaural_beat10();
    decimal(out, (int)(b / 10U), 1);
    if (b % 10U) { char n[12]; strcat(out, "."); decimal(n, (int)(b % 10U), 1); strcat(out, n); }
}
/* v11 BINAURAL (GLOBAL > BINAURAL >, mockup FM1-v11-proposals.html no. 10):
 * the rhythm's sign and the beat large, the L / R waves drifting apart at the
 * beat (slowed ten times), the session's progress, the two carriers. Knobs:
 * brain = rhythm (preset), ear = carrier, stopwatch = timer, speaker = noise
 * (sign = pink / rain, then its level). SELECT tunes the beat, ALGORITHM picks
 * the noise type. The timer counts in the header's info window.
 * v11 3D SOUND (no. 12, bin_page 1): the mode on top, the head from above
 * with the two virtual speakers (+-30 deg) crossfeed puts in front, WIDTH and
 * MIX at the sides, FEED under the head (instead of AZIMUTH). */
static __attribute__((noinline)) void binaural_draw(void)
{
    char v[4][12], num[12];
    unsigned on = binaural_running(), xf = binaural_xfeed();
    if (!bin_page) {
        beat_text(v[0]);
        decimal(v[1], (int)binaural_carrier(), 1);
        if (binaural_timer_min()) { decimal(v[2], (int)binaural_timer_min(), 1); strcat(v[2], "m"); } else strcpy(v[2], "OFF");
        if (binaural_noise()) { v[3][0] = binaural_noise() > 10U ? 'R' : 'P'; decimal(v[3] + 1, (int)((binaural_noise() - 1U) % 10U + 1U), 1); } else strcpy(v[3], "OFF"); /* P3 = pink 3, R3 = rain 3 */
        for (char *c = v[2]; *c; ++c) if (*c == 'm') *c = 'M';
        knob(0, "RHYTHM", binaural_preset() ? v[0] : "OFF", knob_colors[0]);
        knob(1, "CARRIER", v[1], knob_colors[1]);
        knob(2, "TIMER", v[2], knob_colors[2]);
        knob(3, "NOISE", v[3], knob_colors[3]);
    } else {
        decimal(v[1], (int)binaural_width(), 1); decimal(v[2], (int)binaural_feed(), 1); decimal(v[3], (int)binaural_mix(), 1);
        knob(0, "MODE", xf ? "X" : "OFF", knob_colors[0]);
        knob(1, "WIDTH", v[1], knob_colors[1]);
        knob(2, "FEED", v[2], knob_colors[2]);
        knob(3, "MIX", v[3], knob_colors[3]);
    }
    if (paint_rendering && (paint_top >= 195 || paint_top + PAINT_ROWS <= 26)) return;
    const struct groove_view *bv = &frame_views[track];
    uint32_t fsig = bin_page | (bv->next_length << 4) | ((unsigned)chain_on << 16) | 0xb1000000U;
    if (fsig != cache->title_cache) {
        cache->title_cache = fsig;
        /* v12: two plates (BINAURAL, 3D SOUND; SELECT flips), the state plate, a MICRO D window */
        paint_fill(42, 26, 198, 36, BACK);
        paint_fill(0, 62, 240, 133, BACK);
        for (unsigned k = 0; k < 2; ++k) {
            unsigned x = 44 + k * 39, on = k == bin_page;
            uint16_t ink = on ? TEXT : V_DIM;
            rbox(x, HDR_FY, 35, 34, 3, 0x10a3, 0x5acb);
            bi_r = 12;
            int cx = (int)x * 10 + 175, cy = (HDR_FY + 17) * 10;
            bi_arc(cx, cy, 10, 0, 0, 11, 11, 0.0f, 1.0f, ink);
            if (!k) bi_seg(cx, cy - 110, cx, cy + 110, ink);
            else { bi_seg(cx - 130, cy + 30, cx - 90, cy + 30, ink); bi_seg(cx + 90, cy + 30, cx + 130, cy + 30, ink); }
            if (on) glass_cursor(x, HDR_FY, 35, 34, 8, MD_AMBER); /* radius 8 = the plate's pebble corner (rbox r 3) */
        }
        char num[12]; decimal(num, (int)bv->next_length, 1);
        state_plate(num, chain_on != 0, bv->next_length != bv->sequence_length ? V_ORANGE : TEXT);
        rbox(6, 66, 228, 124, 2, MD_PANEL, MD_EDGE);
        cache->row_cache[0] = cache->row_cache[1] = cache->middle_shape = ~0U;
    }
    if (paint_rendering && paint_top + PAINT_ROWS <= 66) return;
    if (bin_page) {
        uint32_t sig = signature(0x3d000000U | xf | binaural_width() << 1 | binaural_feed() << 8 | binaural_mix() << 15, "", 0);
        if (sig == cache->row_cache[0]) return;
        cache->row_cache[0] = sig;
        const int cx = 120, cy = 152;
        paint_fill(8, 68, 224, 120, MD_PANEL);
        lcd_center(cx, 73, xf ? "XFEED" : "OFF", &lcd30, xf ? knob_colors[0] : V_DIM, MD_PANEL);
        for (unsigned a = 0; a < 36; ++a) { /* the orbit, dotted */
            float t = (float)a / 36.0f;
            fill_ellipse(cx * 10 + (int)(260.0f * vsin(t + 0.25f)), cy * 10 - (int)(260.0f * vsin(t)), 7, 7, 0x39a8);
        }
        if (xf) for (int s = -1; s <= 1; s += 2) { /* the two speakers crossfeed puts in front */
            int sx = cx * 10 + s * 130, sy = cy * 10 - 230;
            fill_ellipse(sx, sy, 45, 45, MX_G);
        }
        bi_r = 10; bi_arc(cx * 10, cy * 10, 10, 0, 0, 10, 10, 0.0f, 1.0f, MD_CREAM); /* the head from above */
        bi_r = 10; bi_seg(cx * 10 - 120, cy * 10 + 30, cx * 10 - 80, cy * 10 + 30, MD_CREAM); bi_seg(cx * 10 + 80, cy * 10 + 30, cx * 10 + 120, cy * 10 + 30, MD_CREAM);
        return;
    }
    /* the rhythm's sign and the beat, large */
    uint32_t sig = signature(on | bin_value_icon(BIN_PRESET) << 1 | binaural_carrier() << 6, v[0], (unsigned)strlen(v[0]));
    if (sig != cache->row_cache[0]) {
        cache->row_cache[0] = sig;
        paint_fill(8, 68, 224, 40, MD_PANEL);
        paint_fill(8, 160, 224, 28, MD_PANEL);
        unsigned w = lcd_width(&lcd30, v[0]), hz = lcd_width(&lcd20, "Hz");
        int gx = 120 - (int)(w + 3U + hz) / 2;
        lcd_text(gx, 73, v[0], &lcd30, on ? knob_colors[2] : V_DIM, MD_PANEL);
        lcd_text(gx + (int)w + 3, 79, "Hz", &lcd20, TEXT, MD_PANEL);
        char line[32]; unsigned car = binaural_carrier(), beat = binaural_beat10();
        strcpy(line, "L "); decimal(num, (int)car, 1); strcat(line, num); strcat(line, " Hz");
        lcd_text(14, 164, line, &lcd20, on ? knob_colors[0] : V_DIM, MD_PANEL);
        strcpy(line, "R "); decimal(num, (int)(car + beat / 10U), 1); strcat(line, num);
        if (beat % 10U) { strcat(line, "."); decimal(num, (int)(beat % 10U), 1); strcat(line, num); }
        strcat(line, " Hz");
        lcd_text(226 - (int)lcd_width(&lcd20, line), 164, line, &lcd20, on ? knob_colors[3] : V_DIM, MD_PANEL);
        cache->middle_shape = cache->row_cache[1] = ~0U;
    }
    /* the session's progress (no timer: the empty track) */
    unsigned total = binaural_timer_min() * 60U, left = binaural_remaining();
    unsigned fill = total && on ? (total - (left < total ? left : total)) * 212U / total : 0U;
    if (fill != cache->row_cache[1]) {
        cache->row_cache[1] = fill;
        paint_fill(14, 182, 212, 4, 0x1082);
        if (fill) paint_fill(14, 182, fill, 4, knob_colors[2]);
    }
    /* the waves (user 13.11: only what moves is repainted): L stands still, R drifts against it by the beat
     * (the RHYTHM knob), at most 10 frames/s; a drift step repaints R's rows only (y 136..161) */
    unsigned step = on ? (unsigned)((uint64_t)(frame_time / 100U * 100U) * binaural_beat10() * 32U / 100000U) & 31U : 8U;
    uint32_t shape = 1U | step << 1 | on << 12;
    if (!paint_rendering) {
        if (shape == cache->middle_shape) return;
        unsigned whole = cache->middle_shape == ~0U || ((shape ^ cache->middle_shape) & (1U << 12));
        cache->middle_shape = shape;
        if (whole) paint_mark(8, 108, 224, 54); /* first draw or ON / OFF: both waves (their colour) */
        else paint_mark(8, 136, 224, 26);
        return;
    }
    paint_fill(8, 108, 224, 50, MD_PANEL);
    for (unsigned w = 0; w < 2; ++w) {
        unsigned cy = 122 + w * 26;
        uint16_t c = !on ? V_FAINT : MX_G;
        float ph = w ? (float)step / 32.0f : 0.0f;
        int prev = -1;
        for (unsigned x = 0; x < 212; ++x) {
            int yy = (int)cy - (int)(vsin((float)x / 53.0f + ph) * 10.0f);
            int a2 = prev < 0 ? yy : prev, lo2 = a2 < yy ? a2 : yy, hi2 = a2 < yy ? yy : a2;
            paint_fill(14 + x, (unsigned)lo2, 1, (unsigned)(hi2 - lo2 + 2), c);
            prev = yy;
        }
    }
}
#endif
/* GLOBAL (v13): a 4 x 4 grid of icon tiles by topic (SOUND / TIME / LINK / SYSTEM). SELECT moves the glass
 * cursor tile by tile; the knobs change the chosen tile's values right here (up to four), no sub-pages;
 * ALGORITHM (turned right) opens the pages that need a screen (BLUETOOTH device list, SCOPE, INFO,
 * BINAURAL, 3D SOUND, the MIDI submenu). User 09.11 (round 4): CLOCK, MIDI OUT and MIDI SYNC moved into one MIDI tile
 * (its submenu: CLOCK / MIDI OUT / MIDI SYNC), TRACK ROLE left GLOBAL (it stays a SEQ page), new KEY tile.
 * F11 (user 10.11, "убрать"): the DISPLAY tile (BRIGHT) is gone; its LINK slot is empty. */
/* BINAURAL and 3D SOUND (FM1_BINAURAL) follow SCOPE; without it the tiles are not there. */
enum { GR_BIN_ROWS = 2 * FM1_BINAURAL, GLOB_ROWS = 11 + GR_BIN_ROWS, GR_OUTPUT = 0, GR_BPM = 1, GR_CLICK = 2, GR_KEY = 3, GR_LOWCUT = 4,
       GR_BT = 5, GR_MIDI = 6, GR_SCOPE = 7,
       GR_BINAURAL = FM1_BINAURAL ? 8 : 99, GR_3D = FM1_BINAURAL ? 9 : 98,
       GR_LANG = 8 + GR_BIN_ROWS, GR_THEME = 9 + GR_BIN_ROWS, GR_INFO = 10 + GR_BIN_ROWS,
       GI_CLOCK = 120, GI_MIDI_SYNC = 121 /* icons only: the MIDI submenu's pages */ };
static const char *const glob_rows[GLOB_ROWS] = {"OUTPUT", "BPM", "CLICK", "KEY", "LOW CUT", "BLUETOOTH",
    "MIDI", "SCOPE",
#if FM1_BINAURAL
    "BINAURAL", "3D SOUND",
#endif
    "LANGUAGE", "THEME", "INFO"};
/* The grid, row by row: SOUND, TIME, LINK, SYSTEM; 255 = empty slot (row ids >= GLOB_ROWS are not built in). */
static const uint8_t glob_slots[16] = {GR_OUTPUT, GR_LOWCUT, GR_BINAURAL, GR_3D, GR_BPM, GR_CLICK, GR_KEY, 255,
                                       GR_BT, GR_MIDI, 255, 255, GR_SCOPE, GR_THEME, GR_LANG, GR_INFO};
static unsigned glob_slot_ok(unsigned slot) { return glob_slots[slot & 15U] < GLOB_ROWS; }
/* SELECT: n detents along the grid, skipping the empty slots, round. */
static void glob_move(int n)
{
    unsigned slot = 0;
    for (unsigned i = 0; i < 16; ++i) if (glob_slots[i] == glob_row) slot = i;
    for (int k = n < 0 ? -n % GLOB_ROWS : n % GLOB_ROWS; k > 0; --k)
        do slot = (slot + (n < 0 ? 15U : 1U)) & 15U; while (!glob_slot_ok(slot));
    glob_row = glob_slots[slot];
}
/* The knobs of one GLOBAL row: label and value for each of the four (empty = unused). */
static void glob_knobs(unsigned row, const char *label[4], char value[4][12])
{
    static const char *const clicks[] = {"OFF", "REC", "ON"};
    const struct groove_view *v = &frame_views[track];
    for (unsigned i = 0; i < 4; ++i) { label[i] = ""; value[i][0] = 0; }
    label[0] = glob_rows[row];
    switch (row) {
    case GR_OUTPUT: if (mfx_output()) { value[0][0] = '+'; decimal(value[0] + 1, (int)mfx_output() * 3, 1); strcat(value[0], "DB"); } else strcpy(value[0], "0DB"); break;
    case GR_BPM: if (v->clock_mode && !v->tempo) strcpy(value[0], "WAIT"); else decimal(value[0], v->tempo, 1); break;
    case GR_CLICK: /* K1 the click, K2 the count-in (its own tile in v12) */
        label[1] = "COUNT IN";
        strcpy(value[0], clicks[groove_sequence_setting(1) % 3]); strcpy(value[1], groove_sequence_setting(2) ? "1 BAR" : "OFF"); break;
    case GR_KEY: { /* K1 KEY AUTO / C..B, K2 MODE (-- while AUTO) */
        static const char *const keys[13] = {"AUTO", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        label[1] = "MODE";
        strcpy(value[0], keys[key_get() % 13U]); strcpy(value[1], !key_get() ? "--" : key_mode_get() ? "MIN" : "MAJ"); break;
    }
    case GR_LOWCUT: strcpy(value[0], mfx_lowcut() ? "100HZ" : "OFF"); break;
    case GR_BT: {
        unsigned flags = frame_radio_flags, role = frame_radio_role;
        unsigned on = (flags & GROOVE_RADIO_ON) != 0, midi = !on || !(flags & GROOVE_RADIO_MIDI) ? 0U : role == FM1_BLE_CENTRAL ? 2U : 1U;
        label[0] = "BLUETOOTH"; label[1] = "BT AUDIO"; label[2] = "BLE MIDI"; label[3] = "CLOCK";
        strcpy(value[0], on ? "ON" : "OFF"); strcpy(value[1], flags & GROOVE_RADIO_AUDIO ? "ON" : "OFF");
        strcpy(value[2], midi == 2 ? "HOST" : midi == 1 ? "DEVICE" : "OFF"); strcpy(value[3], v->clock_mode ? "MIDI" : "INT");
        break;
    }
    case GR_MIDI: label[0] = ""; break; /* the mockup: no knobs; ALGORITHM opens the submenu */
    case GR_SCOPE: case GR_INFO: strcpy(value[0], ">"); break;
#if FM1_BINAURAL
    case GR_BINAURAL: strcpy(value[0], binaural_running() ? "ON" : "OFF"); break;
    case GR_3D: strcpy(value[0], binaural_xfeed() ? "XFEED" : "OFF"); break;
#endif
    case GR_LANG: strcpy(value[0], lang_get() ? "RUSSIAN" : "ENGLISH"); break;
    case GR_THEME: { static const char *const themes[3] = {"ORGANIC", "CLASSIC", "MATRIX"}; strcpy(value[0], themes[ui_theme_get()]); break; }
    }
}
/* What the row shows in the list: its first value. */
static void glob_summary(unsigned row, char out[24])
{
    const char *label[4]; char value[4][12];
    glob_knobs(row, label, value);
    strcpy(out, glob_rows[row]); strcat(out, "  ");
    if (row == GR_BT || row == GR_BINAURAL || row == GR_3D || row == GR_MIDI) strcat(value[0], " >"); /* the page behind it */
    strcat(out, value[0]);
}
/* v13 GLOBAL icons, 1.8 px strokes around (cx, cy), as gIcon() in the mockup. */
static void gl_seg(int cx, int cy, int x0, int y0, int x1, int y1, uint16_t c)
{
    aa_stroke((cx + x0) * 10, (cy + y0) * 10, (cx + x1) * 10, (cy + y1) * 10, c);
}
static void gl_poly(int cx, int cy, const int8_t *p, unsigned n, uint16_t c)
{
    for (unsigned i = 0; i + 1 < n; ++i) gl_seg(cx, cy, p[2 * i], p[2 * i + 1], p[2 * i + 2], p[2 * i + 3], c);
}
/* Outlined circle of radius r (px): the tile's colour inside is the tile fill. */
static void gl_ring(int cx, int cy, int r, uint16_t c)
{
    ring(cx * 10, cy * 10, r * 10 + 9, r * 10 + 9, c, GLOB_TILE);
}
static void gl_dot(int cx, int cy, int dx, int dy, uint16_t c)
{
    fill_ellipse((cx + dx) * 10, (cy + dy) * 10, 12, 12, c);
}
static void glob_icon(unsigned row, int x, int y, uint16_t c)
{
    static const int8_t output[] = {-3, -3, 3, -8, 3, 8, -3, 3}, bpm[] = {-6, 8, -2, -8, 2, -8, 6, 8, -6, 8},
        lowcut[] = {-10, 8, -2, 8, 3, -6, 10, -6}, bt[] = {-5, -5, 5, 5, 0, 9, 0, -9, 5, -5, -5, 5};
    switch (row) {
    case GR_OUTPUT: paint_fill((unsigned)(x - 9), (unsigned)(y - 3), 6, 6, c); gl_poly(x, y, output, 4, c); break;
    case GR_BPM: gl_poly(x, y, bpm, 5, c); gl_seg(x, y, 0, 4, 5, -6, c); break;
    case GR_CLICK: fill_ellipse(x * 10, (y + 3) * 10, 40, 40, c); gl_seg(x, y, -8, -6, -5, -3, c); gl_seg(x, y, 8, -6, 5, -3, c); gl_seg(x, y, 0, -9, 0, -5, c); break;
    case GI_CLOCK: gl_ring(x, y, 9, c); gl_seg(x, y, 0, -6, 0, 0, c); gl_seg(x, y, 0, 0, 5, 2, c); break;
    case GR_KEY: /* "C#": the key letter with a small sharp, as the mockup */
        ++keep_names; sq_text(x - 6, y, "C", &lcd17, c, GLOB_TILE); sq_text(x + 6, y - 3, "#", &lcd12, c, GLOB_TILE); --keep_names;
        break;
    case GR_LOWCUT: gl_poly(x, y, lowcut, 4, c); break;
    case GR_BT: gl_poly(x, y, bt, 6, c); break;
    case GR_MIDI: {
        static const int8_t dots[5][2] = {{-5, 2}, {-3, -3}, {0, -5}, {3, -3}, {5, 2}};
        gl_ring(x, y, 9, c);
        for (unsigned i = 0; i < 5; ++i) gl_dot(x, y, dots[i][0], dots[i][1], c);
        break;
    }
    case GI_MIDI_SYNC: /* two half circles chasing each other, an arrow head on each */
        for (int h = 0; h < 2; ++h) {
            int px = 0, py = 0;
            for (int k = 0; k <= 10; ++k) {
                float a = ((h ? 3.4416f : 0.3f) + (float)k * 0.29416f) * 0.15915494f; /* turns */
                int qx = (int)(80.0f * pic_sin(a + 0.25f)), qy = (int)(80.0f * pic_sin(a));
                if (k) aa_stroke(x * 10 + px, y * 10 + py, x * 10 + qx, y * 10 + qy, c);
                px = qx; py = qy;
            }
        }
        fill_tri((x + 5) * 10, (y - 9) * 10, (x + 9) * 10, (y - 7) * 10, (x + 5) * 10, (y - 5) * 10, c);
        fill_tri((x - 9) * 10, (y + 7) * 10, (x - 5) * 10, (y + 9) * 10, (x - 9) * 10, (y + 11) * 10, c);
        break;
    case GR_SCOPE: {
        static const uint8_t h[5] = {5, 10, 7, 12, 4};
        for (unsigned i = 0; i < 5; ++i) paint_fill((unsigned)(x - 10 + (int)(i * 44U) / 10), (unsigned)(y + 7 - (int)h[i]), 3, h[i], c);
        break;
    }
#if FM1_BINAURAL
    case GR_BINAURAL: gl_ring(x, y, 11, c); gl_seg(x, y, 0, -11, 0, 11, c); break;
    case GR_3D: gl_ring(x, y - 2, 9, c); gl_seg(x, y, -13, 3, -9, 3, c); gl_seg(x, y, 9, 3, 13, 3, c); break;
#endif
    case GR_THEME: /* a ring with its right half filled */
        gl_ring(x, y, 8, c);
        for (int dy = -8; dy <= 8; ++dy) paint_fill((unsigned)x, (unsigned)(y + dy), (unsigned)((int)isqrt_ui((unsigned)(64 - dy * dy)) + (dy * dy < 64)), 1, c);
        break;
    case GR_LANG:
        rbox((unsigned)(x - 9), (unsigned)(y - 7), 18, 14, 2, V_NONE, c); rbox((unsigned)(x - 8), (unsigned)(y - 6), 16, 12, 1, V_NONE, c);
        ++keep_names; lcd_center(x, (unsigned)(y - 6), "A", &lcd13, c, GLOB_TILE); --keep_names;
        break;
    case GR_INFO: gl_ring(x, y, 9, c); gl_seg(x, y, 0, -1, 0, 5, c); gl_dot(x, y, 0, -5, c); break;
    }
}
/* The MIDI submenu's icon row (CLOCK, MIDI OUT, MIDI SYNC: the old GLOBAL tiles' icons). */
static void mi_clock(int cx, int cy, uint16_t c) { glob_icon(GI_CLOCK, cx, cy, c); }
static void mi_out(int cx, int cy, uint16_t c) { glob_icon(GR_MIDI, cx, cy, c); }
static void mi_sync(int cx, int cy, uint16_t c) { glob_icon(GI_MIDI_SYNC, cx, cy, c); }
/* GLOBAL grid: 4 x 4 tiles 54 x 28 at (6 + 58 c, 66 + 32 r), the row's colour for its icons, the glass cursor on the
 * chosen tile; its name is the header's big line (top_big_name). */
static __attribute__((noinline)) void global_draw(void)
{
    if (paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 62)) return;
    uint32_t sig = glob_row | 0x6c000000U;
    if (sig == cache->row_cache[0]) return;
    cache->row_cache[0] = sig;
    paint_fill(0, 62, 240, 130, BACK);
    for (unsigned i = 0; i < 16; ++i) {
        if (!glob_slot_ok(i)) continue;
        unsigned x = 6 + (i & 3U) * 58, y = 66 + (i >> 2) * 32;
        rbox(x, y, 54, 28, 2, GLOB_TILE, GLOB_TILE_EDGE);
        glob_icon(glob_slots[i], (int)x + 27, (int)y + 14, knob_colors[i >> 2]);
        if (glob_slots[i] == glob_row) glass_cursor(x, y, 54, 28, 5, MD_AMBER);
    }
}
static const struct groove_view *hdr_v;
static char hdr_section[24], hdr_object[24];
static uint16_t hdr_color;
static unsigned hdr_arrows, hdr_pending;
static int hdr_engine;
static void draw_page_body(const struct fm1_ble_status *ble)
{
    unsigned visible = boot_active ? STARTUP : page;
    if (visible != cache->shown_page) {
        cache->shown_page = visible;
        if (!paint_rendering && (visible == TONE_EDIT || visible == FM_ENV || visible == FM_LFO)) knob_enter(); /* a sound page starts on KNOB 1 */
        paint_fill(0, 0, 240, 240, BACK);
        if (visible != STARTUP && !v6_page(visible)) paint_fill(0, 29, 240, 1, PANEL);
        cache->header_cache = cache->title_cache = cache->middle_cache = cache->hint_cache = cache->hold_cache = cache->seq_cache = ~0U;
        memset(cache->foot_cache, 0xff, sizeof cache->foot_cache);
        memset(cache->row_cache, 0xff, sizeof cache->row_cache);
        memset(cache->mix_cache, 0xff, sizeof cache->mix_cache);
        memset(cache->cell_cache, 0xff, sizeof cache->cell_cache);
        memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
        memset(cache->meter_cache, 0xff, sizeof cache->meter_cache);
        memset(cache->top_cache, 0xff, sizeof cache->top_cache);
    }
    if (visible == STARTUP) { boot_draw(); return; }
    if (visible == SAFE) { safe_draw(); return; }
    const struct groove_view *views = frame_views, *v = &views[track];
    if (!paint_rendering) leds(v, visible);
    if (0) {
        return;
    }
    if (v6_page(visible)) {
        /* v6 top bar; alerts take the second line for a while. */
        static const char *const sections[] = {[TRACKS] = "MIXER", [STEPS] = "SEQ", [GLOBAL] = "GLOBAL",
            [ACTIONS] = "SEL", [ARP_EDIT] = "ARP", [SOURCE] = "ENGINE", [FM_QUICK] = "EDIT", [MASTER_FX] = "MASTER FX", [FM_EDIT] = "FM LAB", [FM_ENV] = "EDIT", [FM_LFO] = "DX7 LFO", [CHAIN] = "CHAIN", [SMP_REC] = "SAMPLER", [SMP_CHOP] = "SAMPLER CHOP", [LFOS] = "LFO", [SCOPE] = "SCOPE", [INFO] = "INFO", [FAQ] = "FAQ", [BINAURAL] = "BINAURAL",
            [TONE_EDIT] = "SOUND", [BLE] = "BLUETOOTH", [PROJECT] = "SAVE", [NAME] = "NAME",
            [EFFECTS] = "TRACK FX", [STEP_EDIT] = "SEQ", [RHYTHM] = "SEQ", [PRESET_BROWSER] = "PRESETS"};
        char object[24];
        uint16_t object_color = TEXT;
        track_title(v, track, object);
        char title0[24]; strcpy(title0, object);
        int engine = -1; /* v7: engine icon + patch name instead of "N · ENGINE · NAME" */
        if (visible == GLOBAL || visible == SCOPE || visible == INFO || visible == FAQ) object[0] = 0; /* not about a track */
        if (visible == GLOBAL) { strncpy(object, glob_rows[glob_row], 23); object[23] = 0; object_color = MD_AMBER; } /* v13: the chosen tile's name, big, between F A S L and the CPU plate */
        if (visible == MASTER_FX) object[0] = 0; /* v12: the window names the effect */
        if (visible == LFOS) object[0] = 0;
        if (visible == BINAURAL) object[0] = 0;
        if (visible == CHAIN) { strcpy(object, chain_on ? "PATTERN CHAIN ON" : "PATTERN CHAIN OFF"); object_color = chain_on ? knob_colors[1] : V_DIM; }
        const char *section = sections[visible];
        if (visible == FM_EDIT || (visible == FM_LFO && v->type == GROOVE_DX7)) section = "FM LAB"; /* v13: always FM LAB; the page icons say which page */
        if (visible == TRACKS && engines_shown()) section = "ENGINES";
        if (visible == STEPS && seq_midi()) section = "MIDI"; /* the GLOBAL MIDI submenu */
        char tone_title[16];
        if (v->type != GROOVE_DX7 && (visible == FM_ENV || visible == FM_LFO)) section = "EDIT";
        if (visible == TONE_EDIT && micro_d_track(track)) {
            unsigned pg = tone_pages[track][tone_slot()] % MD_PAGES;
            char num[12];
            (void)pg; (void)num; strcpy(tone_title, "MICRO D"); /* where you are shows on the path below */
            section = tone_title;
        } else if (visible == TONE_EDIT) {
            strcpy(tone_title, v->type == GROOVE_303 ? "303 ACID" : "SOUND"); /* v13: the pages show on the icon row / pads */
            section = tone_title;
        }
        
        /* C3 (mockup f): the title stays SAVE; what the chosen cell holds is drawn large under it (project_draw) */
        char fx_head[24];
        if (visible == EFFECTS) { /* v12: the track's number and engine in the title, the window names the tab */
            fx_head[0] = (char)('1' + track); fx_head[1] = ' '; fx_head[2] = 0;
            if (v->type < ENGINES) strcat(fx_head, engine_names[v->type]);
            section = fx_head;
        }
        if (visible == BLE || visible == PROJECT || (visible == EFFECTS && effects_page) || (visible == FM_QUICK && quick_row)) object[0] = 0; /* DX7 EDIT page 2 (ENV): no track name */
        if ((visible == FM_EDIT || visible == FM_ENV || visible == FM_LFO) && v->type == GROOVE_DX7) {
            /* icon + "VOICE NAME  CHOIR n" (a DX7 algorithm = how the voices sing together) */
            char name[11], num[12]; memcpy(name, frame_patch + 145, 10); name[10] = 0;
            unsigned k = 10; while (k && name[k - 1] == ' ') name[--k] = 0;
            strcpy(object, name);
            (void)num; /* the CHOIR (algorithm) number shows in the info window */
            engine = GROOVE_DX7; strcpy(title0, object);
        }

        /* v13: the SEQ pages show their name in the icon row's window, the title stays SEQ */
        if (notice && (int32_t)(frame_time - notice_until) >= 0) notice = NULL;
        const char *alert = notice ? notice : source_result ? "SOURCE MEMORY FULL"
            : visible == GLOBAL || visible == MASTER_FX || visible == LFOS || visible == SCOPE || visible == BINAURAL ? NULL
            : v->clock_mode && v->clock_state != GROOVE_CLOCK_LOCKED ? "WAIT MIDI CLOCK"
            : writing && groove_sequence_count_in() ? "COUNT IN"
            : visible == STEPS && v->sequence_length != v->next_length ? "NEXT LENGTH" : NULL;
        if (!strcmp(object, title0) && object[0]) { /* still the track title: draw it as icon + sound name */
            char sound[10]; track_sound_name(v, track, sound);
            if (engine < 0) { engine = v->type; strcpy(object, sound); }
        }
        if (alert) {
            engine = -1;
            strncpy(object, alert, 23); object[23] = 0;
            object_color = writing ? V_RED : V_ORANGE;
        }
        top_big_name = visible == GLOBAL && !alert;
        /* Pages SELECT turns: arrows only where it can still go. */
        unsigned arrows = 0;
        if (visible == TONE_EDIT && micro_d_track(track)) arrows = 3; /* the pages go round */
        else if (visible == TONE_EDIT && v->type != GROOVE_303) {
            unsigned pg = tone_pages[track][tone_slot()], max = (groove_tone_count(track, tone_part()) - 1) / 4;
            arrows = (pg > 0) | ((pg < max) << 1);
        }
        /* row 2 of the header: the mixer keeps F A S L as a row; the step grid
         * draws STEPS / CHAIN on the right itself; some pages fill the info window */
        static char info_num[12];
        top_row2 = visible == TRACKS ? 2U : visible == FM_EDIT ? 3U : visible == FM_ENV || visible == FM_QUICK ? 2U : visible == FM_LFO && v->type == GROOVE_DX7 ? 3U : visible == STEPS ? (seq_page ? 3U : 2U) : visible == SMP_REC || visible == SMP_CHOP || visible == FAQ || (visible == TONE_EDIT && (v->type == GROOVE_303 || v->type == GROOVE_SAMPLE)) ? 3U : visible == FM_LFO ? 2U : visible == ACTIONS || visible == CHAIN || visible == NAME || visible == SOURCE ? 2U : visible == PROJECT ? 3U : visible == ARP_EDIT || visible == GLOBAL || visible == INFO || visible == BLE || (visible == TONE_EDIT && !micro_d_track(track)) ? 2U : (visible == TONE_EDIT && micro_d_track(track)) || visible == EFFECTS || visible == LFOS || visible == BINAURAL || visible == MASTER_FX ? 3U : 1U;
        if (visible == SCOPE) { top_row2 = 4U; section = ""; } /* v13: no SCOPE label, no track, no F A S L */
        top_info_label = top_info_value = NULL;
        top_section_color = visible == EFFECTS ? track_color(track) : visible == TONE_EDIT && v->type == GROOVE_303 ? engine_colors[GROOVE_303] : TEXT;
        if (visible == ACTIONS && action_confirm) { section = "REPLACE DATA?"; top_section_color = MD_AMBER; } /* v13: the bar names it, row 2 stays */
        if (visible == FAQ) { top_info_label = "PAGE"; decimal(info_num, (int)faq_page + 1, 1); strcat(info_num, "/10"); top_info_value = info_num; }
#if FM1_BINAURAL
#endif
        else if (0) { }
        else if (0) { top_info_label = "ALGO"; decimal(info_num, frame_patch[134] + 1, 1); top_info_value = info_num; }
        /* drawn after the page body (draw_page), so the header always wins rows 0..61 */
        hdr_v = v; strncpy(hdr_section, section, sizeof hdr_section - 1); strncpy(hdr_object, object, sizeof hdr_object - 1);
        hdr_color = object_color; hdr_arrows = arrows; hdr_engine = engine; hdr_pending = 1;
    } else header(v);
    if (visible == PROJECT) {
        project_draw();
        return;
    }
    if (visible == BLE) {
        draw_ble(ble);
        return;
    }
    if (visible == TRACKS) {
        tracks_draw(views);
        char value[12];
        decimal(value, v->volume, 1);
        knob(0, "VOLUME", value, colors[0]);
        {
            unsigned type = v->type;
            int f = type == GROOVE_303 ? frame_patch[0] : type == GROOVE_PLUCK || type == GROOVE_808 ? frame_patch[1] /* 808: BD tone */
                  : type == GROOVE_DX7 ? dx7_brightness_value(frame_patch) : type == ENGINE_MICRO ? 0 : -1;
            if (type == ENGINE_MICRO) micro_d_text(track, MD_CUTOFF, value);
            else if (f < 0) strcpy(value, "--");
            else if (type == GROOVE_303) format_hz(value, (unsigned)(314.0f * fexp2_ui(2.9305886f * (float)f / 127.0f)));
            else if (type == GROOVE_PLUCK) { decimal(value, (f * 100 + 63) / 127, 1); strcat(value, "%"); }
            else decimal(value, f, 1);
            knob(1, type == GROOVE_DX7 ? "BRIGHT" : type == GROOVE_808 ? "TONE" : "FILTER", value, colors[1]);
        }
        pan_text(track, value);
        knob(2, "PAN", value, colors[2]);
        { /* user 13.11: the state's value in its colour: ON teal, SOLO yellow, MUTE red */
            static const uint16_t state_ink[3] = {PAL_TURQ, PAL_YELLOW, MIX_RED};
            unsigned s = mixer_state(track) % 3U;
            knob_tint = 8U; knob(3, "STATE", mixer_state_names[s], state_ink[s]); knob_tint = 0;
        }
        return;
    }
    if (visible == SCOPE) { scope_view = v; return; } /* drawn by draw_page after the header: the ball hangs over it */
    if (visible == INFO) { info_draw(); return; }
    if (visible == FAQ) { faq_draw(); return; }
#if FM1_BINAURAL
    if (visible == BINAURAL) { binaural_draw(); return; }
#endif
    if (visible == LFOS) {
        /* v7 (agreed 21a): LFO 1 | LFO 2 in the 20b strip, PRESETS switches.
         * In the box: rate and wave small, the target large, a bipolar depth
         * bar, then the live wave with the yellow four-point star. */
        unsigned l = lfo_sel;
        char name[12], rate[12];
        lfo_target_name(lfo_get(l, 0), name); lfo_rate_name(lfo_get(l, 1), rate);
        char depth[12]; int d = (int)lfo_get(l, 2) - 63;
        decimal(depth, d * 100 / 63, 1); if (d > 0) { memmove(depth + 1, depth, strlen(depth) + 1); depth[0] = '+'; }
        if (!(paint_rendering && paint_top >= 192)) {
            const struct groove_view *lv = &frame_views[track];
            uint8_t key[8] = {(uint8_t)lfo_get(l, 0), (uint8_t)lfo_get(l, 1), (uint8_t)lfo_get(l, 2), (uint8_t)lfo_get(l, 3), (uint8_t)l};
            uint32_t sig = signature(lv->next_length | ((unsigned)chain_on << 16), key, sizeof key);
            if (sig != cache->row_cache[0]) {
                cache->row_cache[0] = sig;
                /* v12: MICRO D window; two LFO plates (the chosen one amber), LFO 1 / LFO 2 large on top */
                paint_fill(42, 26, 198, 36, BACK);
                paint_fill(0, 62, 240, 132, BACK);
                for (unsigned k = 0; k < 2; ++k) {
                    unsigned x = 44 + k * 39, on = k == l;
                    uint16_t ink = on ? TEXT : V_DIM; /* v13: both plates dark, the chosen one white with the amber glass cursor */
                    rbox(x, HDR_FY, 35, 34, 3, 0x10a3, 0x5acb);
                    int px = (int)x + 4, py = 0;
                    for (int k2 = 0; k2 <= 27; ++k2) {
                        int yy = HDR_FY + 17 - (int)(vsin((float)k2 / 27.0f) * 8.0f);
                        if (k2) { line(px, py, (int)x + 4 + k2, yy, ink); line(px, py + 1, (int)x + 4 + k2, yy + 1, ink); }
                        px = (int)x + 4 + k2; py = yy;
                    }
                }
                glass_cursor(44 + l * 39, HDR_FY, 35, 34, 3, MD_AMBER);
                char num[12]; decimal(num, (int)lv->next_length, 1);
                state_plate(num, chain_on != 0, lv->next_length != lv->sequence_length ? V_ORANGE : TEXT);
                rbox(6, 66, 228, 124, 2, MD_PANEL, MD_EDGE);
                char tab[20]; lfo_target_title(lfo_get(l, 0), tab); /* user 13.11: the target's name, not "LFO 1" */
                lcd_center(120, 73, tab, lcd_width(&lcd30, tab) <= 216 ? &lcd30 : &lcd24, MD_AMBER, MD_PANEL);
                cache->middle_shape = ~0U;
            }
            sig = lfo_get(l, 3) | (lfo_get(l, 2) << 4) | (l << 12) | ((frame_time / 100U) << 14); /* 10 frames/s */
            if (sig != cache->middle_shape) {
                uint32_t before = cache->middle_shape;
                cache->middle_shape = sig;
                const unsigned cy = 148;
                float depth_f = (float)(d < 0 ? -d : d) / 63.0f, amp = 6.0f + 26.0f * depth_f;
                /* The dot's place is taken once per frame; the bands draw it there. When only the time
                 * moved (same LFO, wave and depth) the damage is the dot's old and new place, not the
                 * whole picture: same pixels, 1-2 narrow bands instead of five full ones 10 times a second. */
                static int dot_x = 14, dot_y = 148;
                if (!paint_rendering) {
                    int old_x = dot_x, old_y = dot_y;
                    dot_x = 14 + (int)(lfo_phase(l) * 212.0f); /* one period over the width: the dot runs to the end */
                    dot_y = (int)cy - (int)(lfo_output(l) * amp);
                    if (before != ~0U && !((before ^ sig) & 0x3fffU)) {
                        for (unsigned k = 0; k < 2; ++k) {
                            int mx = (k ? dot_x : old_x) - 6, my = (k ? dot_y : old_y) - 6;
                            if (mx < 8) mx = 8;
                            if (mx > 232 - 13) mx = 232 - 13;
                            if (my < 110) my = 110;
                            if (my > 188 - 13) my = 188 - 13;
                            paint_mark((unsigned)mx, (unsigned)my, 13, 13);
                        }
                        goto lfo_picture_done;
                    }
                }
                paint_fill(8, 110, 224, 78, MD_PANEL);
                paint_fill(14, cy, 212, 1, MD_EDGE);
                int px = 14, py = 0;
                unsigned w = lfo_get(l, 3);
                for (unsigned x = 0; x < 213; ++x) {
                    int yy = (int)cy - (int)(lfo_shape(w, (float)x / 212.0f) * amp);
                    if (x) { line(px, py, 14 + (int)x, yy, MX_G); line(px, py + 1, 14 + (int)x, yy + 1, MX_G); }
                    px = 14 + (int)x; py = yy;
                }
                fill_ellipse(dot_x * 10 + 5, dot_y * 10 + 5, 25, 25, MIX_RED); /* user 13.11: smaller, red */
            lfo_picture_done:;
            }
        }
        knob(0, "TARGET", name, colors[0]);
        knob(1, "RATE", rate, colors[1]);
        knob(2, "DEPTH", depth, colors[2]);
        knob(3, "WAVE", lfo_wave_name(lfo_get(l, 3)), colors[3]);
        return;
    }
    if (visible == CHAIN) {
        /* v13: 16 cells, amber glass = chosen, red frame = playing, number = the project (SAVE slot); the plate shows the slot. */
        if (!(paint_rendering && (paint_top >= 62 || paint_top + PAINT_ROWS <= 26)) && cache->middle_cache != (chain_cursor | 0x77000U)) {
            cache->middle_cache = chain_cursor | 0x77000U;
            char num[12]; decimal(num, (int)chain_cursor + 1, 1);
            state_plate_cap(num, "SLOT");
        }
        if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44))) {
            for (unsigned i = 0; i < 16; ++i) {
                unsigned x = 8 + (i % 4) * 57, y = 64 + (i / 4) * 32;
                uint32_t sig = 1U | ((uint32_t)chain_slot[i] << 1) | ((i == chain_cursor) << 6) |
                               ((chain_on && i == chain_pos) << 7) | ((uint32_t)chain_rep[i] << 14);
                if (sig == cache->tile_cache[i]) continue;
                cache->tile_cache[i] = sig;
                unsigned playing = chain_on && i == chain_pos;
                paint_fill(x - 2, y - 2, 56, 32, BACK);
                rbox(x, y, 52, 28, 2, chain_slot[i] ? 0x18c4 : V_NONE, 0x39a8);
                if (playing) { rbox(x, y, 52, 28, 2, V_NONE, V_RED); frame(x + 1, y + 1, 50, 26, V_RED); } /* v13: red frame = playing */
                if (i == chain_cursor) glass_cursor(x, y, 52, 28, 5, MD_AMBER); /* v13: amber glass = the chosen cell */
                char num[12];
                if (chain_slot[i]) decimal(num, chain_slot[i], 2); else strcpy(num, "-");
                lcd_text((int)x + 6, y + 3, num, &lcd22, chain_slot[i] ? (playing ? V_RED : TEXT) : V_FAINT,
                         chain_slot[i] ? 0x18c4 : BACK);
                if (chain_slot[i]) { /* repeats, bottom right: x1 dim */
                    num[0] = 'X'; decimal(num + 1, chain_rep[i] + 1, 1);
                    lcd_text((int)x + 47 - (int)lcd_width(&lcd12, num), y + 14, num, &lcd12, chain_rep[i] ? 0xfe87 : V_FAINT, 0x18c4);
                }
            }
        }
        char slot[12], seqv[12];
        decimal(slot, chain_cursor + 1, 1);
        if (chain_slot[chain_cursor]) decimal(seqv, chain_slot[chain_cursor], 2); else strcpy(seqv, "----");
        knob(0, "FIELD", slot, colors[0]);
        knob(1, "PROJECT", seqv, colors[1]);
        { char rp[12]; rp[0] = 'X'; decimal(rp + 1, chain_rep[chain_cursor] + 1, 1); knob(2, "REPEAT", rp, colors[2]); }
        knob(3, "CHAIN", chain_on ? "ON" : "OFF", colors[3]);
        return;
    }
    if (visible == SMP_REC || visible == SMP_CHOP) { smp_draw(visible == SMP_CHOP); return; }
    if (visible == GLOBAL) {
        global_draw();
        cpu_plate();
        const char *klabel[4]; char kvalue[4][12];
        glob_knobs(glob_row, klabel, kvalue);
        for (unsigned i = 0; i < 4; ++i) {
            /* a knob without a function shows a dim dash (v13) */
            if (!*klabel[i] && !kvalue[i][0]) klabel[i] = "-";
            knob(i, klabel[i], kvalue[i], colors[i]);
        }
        return;
    }
    if (visible == MASTER_FX) {
        master_fx_draw();
        return;
    }
    if (visible >= SOURCE) {
        edit_page_draw(visible);
        return;
    }
    if (visible == NAME) {
        name_draw();
        return;
    }
    if (visible == EFFECTS) {
        effects_draw();
        return;
    }
    if (visible == FM_LFO && v->type != GROOVE_DX7) {
        track_lfo_draw();
        return;
    }
    const uint8_t *patch = frame_patch;
    const struct groove_fm_controls controls = frame_controls;
    if (visible == OVERVIEW)
        overview(views);
    else if (visible == STEPS)
        steps(v);
    else if (visible == TONE_EDIT || (visible == FM_ENV && v->type != GROOVE_DX7)) {
        tone_draw(patch);
    } else if (visible == FM_ENV) {
        amp_env_draw(&controls);
    } else {
        fm_draw(patch, &controls);
    }
    /* Lower controls cannot intersect an upper LCD band. Avoid formatting
     * their labels and querying the engine once for every unrelated band. */
    if (paint_rendering && paint_top + PAINT_ROWS <= 184) return;
    for (unsigned col = 0; col < 4; ++col) {
        char value[12];
        const char *label;
        uint16_t color = colors[col];
        if (visible == STEPS && seq_page == SEQ_STEP) {
            step_knob(col, 0); continue;
        } else if (visible == STEPS && !seq_page) {
            static const char *const labels[] = {"VELOCITY", "LENGTH", "PROB", "NOTE"};
            struct groove_step c = frame_cells[cursor % GROOVE_STEPS];
            unsigned has = c.velocity != 0, pads = groove_pad_count(track) != 0;
            label = labels[col];
            if (!col) { if (has || pads) decimal(value, has ? groove_step_setting(track, cursor, 0) : 0, 1); else strcpy(value, "--"); }
            else if (col == 2) {
                unsigned pv = groove_step_setting(track, cursor, 2);
                if (!has && !pads) strcpy(value, "--");
                else { decimal(value, pv ? (int)((pv * 100U + 4U) / 8U) : 100, 1); strcat(value, "%"); }
            } else if (col == 1) {
                unsigned g = groove_step_setting(track, cursor, 1);
                if (pads && groove_source_type(track) == GROOVE_SAMPLE) {
                    /* SAMPLER: this pad's LENGTH on this step (a parameter lock), else the pad's own */
                    label = "LENGTH";
                    unsigned v = step_length_lock(cursor);
                    if (v == 255U) { uint8_t tv[GROOVE_TONE_PARAMS]; v = groove_tone_copy(track, drum, tv) ? 127U : tv[2]; }
                    decimal(value, (int)((v * 100U + 63U) / 127U), 1); strcat(value, "%");
                    if (step_length_lock(cursor) == 255U) color = V_DIM;
                }
                else if (!has || pads) strcpy(value, "--");
                else if (groove_step_setting(track, cursor, 4)) { /* a long note: how many steps it lasts from here */
                    unsigned n = 1, len = groove_sequence_length(track), st = cursor;
                    while (n < len && groove_step_setting(track, st, 4)) { st = (st + 1U) % len; ++n; } /* C3: up to the sequence length */
                    decimal(value, (int)n, 1); strcat(value, " STEPS");
                }
                else { decimal(value, g ? g == 15 ? 100 : (g * 100 + 8) / 16 : 50, 1); strcat(value, "%"); }
            } else if (pads) {
                label = "LAYER"; strncpy(value, groove_pad_name(track, drum), 11); value[11] = 0;
                color = pad_colors[drum % 12];
            }
            else note_name(has ? c.note : selected_note, value);
        } else if (visible == STEPS && seq_page == SEQ_RHYTHM) { /* page 2 VARIATION (rework C1); K3 TRANSPOSE inactive until C2 */
            static const char *const labels[] = {"MUTATE", "HUMAN", "TRANSPOSE", "CHANCE"};
            label = labels[col];
            if (col == 2) { /* TRANSPOSE (C2): the offset since the page opened; drums and PLAY / OFF tracks "--" */
                if (!transpose_usable()) strcpy(value, "--");
                else { value[0] = transpose_shown > 0 ? '+' : 0; decimal(value + (transpose_shown > 0), transpose_shown, 1); }
            } else var_value(track, seq_nav_var_knob(col), value);
        } else if (visible == STEPS && seq_page == SEQ_MIDI_CLOCK) { /* round 4: K1 CLOCK, K2 CLK IN; BPM is GLOBAL, LENGTH on the grid */
            static const char *const sources[] = {"AUTO", "USB", "BLE", "DIN"};
            label = col == 0 ? "CLOCK" : col == 1 ? "CLK IN" : "";
            if (!col) strcpy(value, v->clock_mode ? "MIDI" : "INT");
            else if (col == 1) strcpy(value, sources[groove_midi_setting(track,6) & 3U]);
            else value[0] = 0;
        } else if (visible == STEPS && seq_page == SEQ_ROLE) {
            static const char *const roles[] = {"NORMAL", "PLAY", "OFF"};
            label = col == 0 ? "ROLE" : col == 1 ? "IN CH" : col == 2 ? "FOLLOW" : ""; /* K3 FOLLOW (C2): MICRO D only */
            if (!col) strcpy(value,roles[groove_track_role(track)]);
            else if (col == 1) decimal(value,groove_midi_input_channel(track)+1,1);
            else if (col == 2) strcpy(value, !follow_usable(track) ? "--" : var_follow(track) ? "ON" : "OFF");
            else value[0] = 0;
        } else if (visible == STEPS && (seq_page == SEQ_MIDI_OUT || seq_page == SEQ_MIDI_SYNC)) {
            static const uint8_t parameters[2][4] = {{0,1,2,4},{3,5,7,8}};
            static const char *const labels[2][4] = {{"SEQ/ARP", "OUT CH", "OUT PORT", "KEY OUT"}, {"LOCAL", "CLK OUT", "THRU IN", "THRU OUT"}};
            static const char *const modes[] = {"LOCAL", "BOTH", "MIDI"};
            static const char *const ports[] = {"OFF", "DIN", "USB", "DIN+USB", "BLE", "DIN+BLE", "USB+BLE", "ALL"};
            static const char *const sources[] = {"DIN", "USB", "BLE"};
            unsigned parameter = parameters[seq_page - 5][col], setting = groove_midi_setting(track,parameter);
            label = labels[seq_page - 5][col];
            if (!parameter) strcpy(value,modes[setting]);
            else if (parameter == 1) decimal(value,setting + 1,1);
            else if (parameter == 2 || parameter == 8) strcpy(value,ports[setting]);
            else if (parameter == 7) strcpy(value,sources[setting]);
            else strcpy(value,setting ? "ON" : "OFF");
        } else if (visible == TONE_EDIT && micro_d_track(track)) {
            unsigned parameter = (tone_pages[track][tone_slot()] % MD_PAGES) * 4U + col;
            label = micro_d_label(parameter);
            micro_d_text(track, parameter, value);
            color = md_ink[col];
        } else if (visible == TONE_EDIT || (visible == FM_ENV && v->type != GROOVE_DX7)) {
            unsigned parameter = tone_parameter(col);
            const fm1_param_t *p = groove_tone_parameter(track, tone_part(), parameter);
            label = p ? p->name : "";
            if (p && visible == FM_ENV && v->type == GROOVE_303 && parameter == BASS303_ACCDEC) label = "ACC DECAY";
            if (!p)
                value[0] = 0;
            else
                tone_label(parameter, patch[parameter], p, value);
        } else if (visible == FM_ENV) {
            static const char *const labels[] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
            label = labels[col];
            decimal(value, controls.envelope[col], 1);
            strcat(value, "%");
        } else if (visible == FM_EDIT || visible == FM_LFO) {
            unsigned parameter = fm_parameter(col);
            label = fm_pages[fm_group].label[col];
            if (parameter == 255)
                value[0] = 0;
            else if (parameter < 126 && parameter % 21 == 17)
                strcpy(value, patch[parameter] ? "FIXED" : "RATIO");
            else if (parameter == 136 || parameter == 141)
                strcpy(value, patch[parameter] ? "ON" : "OFF");
            else if (parameter < 126 && parameter % 21 == 20)
                decimal(value, (int)patch[parameter] - 7, 1);
            else if (parameter < 126 && parameter % 21 == 8)
                note_name(patch[parameter] + 9, value);
            else if (parameter < 126 && (parameter % 21 == 11 || parameter % 21 == 12)) {
                static const char *const curves[] = {"-LN", "-EX", "+EX", "+LN"};
                strcpy(value, curves[patch[parameter] & 3]);
            } else if (parameter == 142) {
                static const char *const waves[] = {"TRI", "SAWDN", "SAWUP", "SQR", "SINE", "S/H"};
                strcpy(value, waves[patch[parameter] % 6]);
            } else
                decimal(value,
                        patch[parameter] + (parameter == 134   ? 1
                                            : parameter == 144 ? -24
                                                               : 0),
                        1);
        } else {
            static const char *const labels[4] = {"LEVEL", "STEP", "NOTE", "BPM"};
            label = labels[col];
            if (col == 0 && visible == OVERVIEW) {
                label = "TRACKS";
                strcpy(value, "<<");
            } else if (col == 0)
                decimal(value, v->volume, 1);
            else if (col == 1 && visible == OVERVIEW) {
                label = "LEVEL";
                decimal(value, v->volume, 1);
            } else if (visible == OVERVIEW && col == 2) {
                uint8_t sends[2]; groove_sends_get(track, sends);
                label = "FX SEND";
                decimal(value, (int)((sends[0] * 100U + 7U) / 15U), 0);
            } else if (visible == OVERVIEW) {
                label = v->solo ? "SOLO" : v->muted ? "MUTE" : "TRACK";
                strcpy(value, v->solo ? "S" : v->muted ? "M" : "ON");
            } else if (col == 1U) {
                label = visible != OVERVIEW && step_follow && v->playing ? "PLAY" : "STEP";
                decimal(value, step_follow && v->playing ? v->play_step + 1U : cursor + 1U, 2);
            } else if (col == 2 && groove_pad_count(track) != 0) {
                label = v->type == GROOVE_SAMPLE ? "SLOT" : "DRUM";
                decimal(value, drum + 1, 2);
            } else if (col == 2)
                note_name(selected_note, value);
            else if (v->clock_mode) {
                label = "MIDI";
                if (!v->tempo)
                    strcpy(value, v->clock_state == GROOVE_CLOCK_LOST ? "LOST" : "WAIT");
                else
                    decimal(value, v->tempo, 1);
            } else
                decimal(value, v->tempo, 1);
        }
        knob_tint = visible == STEPS && !seq_page && col == 3 && groove_pad_count(track) ? 8U : 0U;
        knob(col, label, value, color);
        knob_tint = 0;
    }
    if (v6_page(visible)) return; /* v6: no hint line, alerts go to the top bar */
    if (notice && (int32_t)(frame_time - notice_until) >= 0) notice = NULL;
    const char *hint = notice ? notice : source_result ? "SOURCE MEMORY FULL - RETRY"
                       : v->clock_mode && v->clock_state != GROOVE_CLOCK_LOCKED
                           ? "WAIT MIDI CLOCK - SEQ SYNC: INT/MIDI"
                       : writing && groove_sequence_count_in() ? "COUNT IN - RECORD ON NEXT BAR"
                       : writing ? v->playing ? "LIVE REC - PLAY KEYS  REC TO FINISH"
                                                             : "STEP REC - PLAY KEYS  REC TO FINISH"
                       : visible == OVERVIEW && (previous_buttons & BUTTON(HOME))
                           ? "PRESETS SOURCE  EDIT PARAMETERS"
                       : visible == OVERVIEW ? "PRESETS SOUND  ALGO ENGINE  KNOB1 BACK"
                       : visible == STEPS && !seq_page ? "SELECT<> STEP  ALGO^ ROW  SEQ PAGES"
                       : visible == STEPS && seq_page == SEQ_STEP ? "SEQ GRID  SELECT PAGE  KEYS CHORD"
                       : visible == STEPS && seq_midi() ? "SELECT PAGE  HOME GLOBAL"
                       : visible == STEPS    ? "SELECT PAGE  ALGO STEP  PRESETS ROW"
                       : visible == FM_EDIT  ? "BLACK VOICE  SELECT PAGE  EDIT BACK"
                       : visible == FM_ENV && v->type != GROOVE_DX7 ? "ENV BACK  SELECT TRACK"
                       : visible == FM_ENV   ? "PRESETS ENV ON/OFF  HOME TRACKS"
                       : visible == GLOBAL && glob_row == GR_BT ? "ALGORITHM OPENS THE DEVICE LIST" /* C3, mockup g */
                       : visible == TONE_EDIT && !advanced_edit
                           ? "HOLD EDIT MORE  SELECT TRACK"
                           : "SEL PAGE  HOME TRACKS";
    uint32_t hint_sig = signature(0, hint, (unsigned)strlen(hint));
    if (hint_sig != cache->hint_cache) {
        cache->hint_cache = hint_sig;
        paint_fill(6, 231, 228, 7, BACK);
        hint_text(6, 231, hint, 1, DIM, BACK);
    }
}
/* v11 MIXER: a light runs round the screen edge while playing, one lap a
 * bar: 64 places (16 a side), four between two sequencer steps. Drawn over
 * everything; the mark pass repaints the place it left. */
static unsigned edge_at = ~0U;
static void edge_rect(unsigned p, unsigned r[4])
{
    unsigned k = (p % 16U) * 15U, side = p / 16U;
    r[2] = side & 1U ? 3U : 4U; r[3] = side & 1U ? 4U : 3U;
    r[0] = side == 0 ? k : side == 1 ? 237U : side == 2 ? 236U - k : 0U;
    r[1] = side == 0 ? 0U : side == 1 ? k : side == 2 ? 237U : 236U - k;
}
static void edge_light(void)
{
    const struct groove_view *sv = &frame_views[track];
    static unsigned last_step = ~0U; static uint32_t step_ms;
    unsigned p = ~0U;
    if (sv->playing && sv->tempo) {
        unsigned step = sv->play_step;
        if (step != last_step) { last_step = step; step_ms = frame_time; }
        unsigned len = 15000U / sv->tempo, sub = (frame_time - step_ms) * 4U / (len ? len : 1U);
        p = (step % 16U) * 4U + (sub > 3U ? 3U : sub);
    } else last_step = ~0U;
    unsigned r[4];
    if (!paint_rendering) {
        if (p == edge_at) return;
        if (edge_at != ~0U) { edge_rect(edge_at, r); paint_mark(r[0], r[1], r[2], r[3]); }
        edge_at = p;
        if (p != ~0U) { edge_rect(p, r); paint_mark(r[0], r[1], r[2], r[3]); }
        return;
    }
    if (edge_at == ~0U) return;
    edge_rect(edge_at, r);
    if (r[1] >= paint_top + PAINT_ROWS || r[1] + r[3] <= paint_top) return;
    paint_fill(r[0], r[1], r[2], r[3], MX_G);
}
static void draw_page(const struct fm1_ble_status *ble)
{
    hdr_pending = 0;
    draw_page_body(ble);
    if (hdr_pending) top_bar(hdr_v, hdr_section, hdr_object, hdr_color, hdr_arrows, hdr_engine);
    if (scope_view) { scope_draw(scope_view); scope_view = NULL; }
    if (page == TRACKS) edge_light();
    if (boot_active || page == SAFE || page == SCOPE || page == FAQ || (paint_rendering && (paint_top > 194 || paint_top + PAINT_ROWS <= 193))) return; /* no hold bar across the scope's rainbow */
    uint32_t since = 0;
    unsigned active = 1;
    if (edit_down && !edit_long) since = edit_ms;
    else if (arp_down && !arp_long) since = arp_ms;
    else if (seq_down) since = seq_ms;
    else active = 0;
    unsigned width = active ? (unsigned)clamp((int)(frame_time - since), 0, 600) * 232U / 600U : 0;
    if (cache->hold_cache == width) return;
    cache->hold_cache = width;
    /* v6 hold bar just above the knob bars. */
    paint_fill(4, 193, 232, 2, width ? 0x39a8 : BACK);
    if (width) paint_fill(4, 193, width, 2, TEXT);
}
/* F8 hook (ui_meter.h): draw, paint_compose and paint_flush are timed around their bodies; nothing else changes. */
static void draw_frame(const struct fm1_ble_status *ble);
static void draw(const struct fm1_ble_status *ble)
{
    uint32_t t0 = fm1_output_timer(), ms = fm1_millis();
    draw_frame(ble);
    ui_meter_add(UI_DRAW, fm1_output_timer_since(t0), fm1_millis() - ms);
}
static void draw_frame(const struct fm1_ble_status *ble)
{
    unsigned visible = page;
    frame_time = fm1_millis();
    if (page != TRACKS) engines_until = 0; /* leaving the mixer closes ENGINES */
    if (eng_warn && (page != TRACKS || !engines_shown())) { eng_warn = 0; eng_pending = 0xff; } /* the dialog goes with the page */
    { static unsigned was; unsigned is = engines_shown(); if (is != was) { was = is; saved_cache.shown_page = ~0U; } }
    mfx_scope_enable(visible == SCOPE);
    if (visible == SCOPE && frame_time - scope_at >= 45U) { scope_at = frame_time; scope_frame(); scope_fresh = 1; } /* 20 fps: the LCD link cannot carry more */
    if (boot_active) {
        boot_phase = (frame_time - boot_started) / 65U;
        if (boot_phase > 77) boot_phase = 77;
    }
    if (notice && (int32_t)(frame_time - notice_until) >= 0) notice = NULL;
    for (unsigned t = 0; t < 8; ++t) {
        unsigned n = groove_midi_activity(t);
        if (n != midi_seen[t]) { midi_seen[t] = n; midi_until[t] = frame_time + 120U; }
        midi_active[t] = (int32_t)(midi_until[t] - frame_time) > 0;
    }
    frame_ble = *ble;
    if (visible == GLOBAL) { /* the BLUETOOTH row shows the settings as they are now */
        frame_radio_flags = groove_settings_radio_flags();
        frame_radio_role = groove_settings_role_value();
    }
    if (visible == BLE) {
        fm1_bluetooth_get_status(&frame_radio);
        frame_radio_flags = groove_settings_radio_flags();
        frame_radio_role = groove_settings_role_value();
        memset(frame_devices, 0, sizeof frame_devices);
        for (unsigned i = 0; i < ble->count && i < FM1_BLE_MAX_DEVICES; ++i)
            fm1_ble_get_device(i, &frame_devices[i]);
    }
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        groove_get_view(t, cursor, &frame_views[t]);
        if (micro_d_track(t)) {
            micro_d_audible(t, frame_views[t].audible);
            frame_views[t].type = ENGINE_MICRO; frame_views[t].program = (uint8_t)micro_d_preset(t);
            frame_views[t].volume = (uint8_t)micro_d_volume(t); frame_views[t].level = (uint8_t)micro_d_level(t);
        }
    }
    if (visible == STEPS) {
        groove_sequence_read(track, 0, frame_cells, GROOVE_STEPS);
        unsigned first = step_first(&frame_views[track]);
        for (unsigned i = 0; i < 16; ++i) {
            struct groove_view cell = {0};
            groove_get_view(track, first + i, &cell);
            frame_step_notes[i] = cell.step.note;
        }
    }
    if (visible == EFFECTS) {
        groove_effects_get(frame_effects);
        if (!effects_page) {
            uint8_t sends[2]; groove_sends_get(track, sends);
            frame_effects[0] = (uint8_t)((sends[0] * 100U + 7U) / 15U); /* REV SEND */
            frame_effects[1] = (uint8_t)((sends[1] * 100U + 7U) / 15U); /* DLY SEND */
            frame_effects[2] = micro_d_track(track) ? (uint8_t)micro_d_volume(track) : frame_views[track].volume; /* TRK VOL; [3] ALL RET */
        }
    }
    memset(frame_patch, 0, sizeof frame_patch);
    memset(&frame_controls, 0, sizeof frame_controls);
    ++frame_seq;
    frame_fm_ok = 0;
    if (visible == TONE_EDIT || (visible == FM_ENV && groove_source_type(track) != GROOVE_DX7))
        groove_tone_copy(track, tone_part(), frame_patch);
    else if (visible == FM_LFO && groove_source_type(track) != GROOVE_DX7)
        groove_lfo_copy(track, frame_patch);
    else if ((visible >= FM_EDIT && visible <= FM_LFO) || visible == NAME || visible == FM_QUICK ||
             (visible == TRACKS && groove_source_type(track) == GROOVE_DX7))
        frame_fm_ok = !groove_fm_copy(track, frame_patch, &frame_controls);
    else if (visible == TRACKS)
        groove_tone_copy(track, 0, frame_patch);
    /* Merge new damage with unfinished bands. Input can replace a pending
     * frame; no stale queue of old encoder positions needs to be displayed. */
    paint_priority = 0;
    cache = &saved_cache;
    draw_page(&frame_ble);
}
/* Compose one band of the current frame into paint_pixels (the half the DMA
 * is not reading), clipped to columns left..left+width. */
static void paint_compose_band(unsigned band, unsigned left, unsigned width);
static void paint_compose(unsigned band, unsigned left, unsigned width)
{
    uint32_t t0 = fm1_output_timer(), ms = fm1_millis();
    paint_compose_band(band, left, width);
    ui_meter_add(UI_COMPOSE, fm1_output_timer_since(t0), fm1_millis() - ms);
}
static void paint_compose_band(unsigned band, unsigned left, unsigned width)
{
    paint_top = band * PAINT_ROWS;
    paint_x = left;
    paint_end = left + width;
    memset(cache, 0xff, sizeof *cache);
    draw_page(&frame_ble);
    /* Organic look: near-black pixels (background, panels, dark marks)
     * take a moss ramp, from #1a211c up to #3b4c40, by their green level. */
    static const uint16_t moss[16] = {0x1903,0x1923,0x1924,0x2144,0x2164,0x2165,0x2185,0x29a5,
                                      0x29c5,0x29c6,0x31e6,0x3206,0x3207,0x3227,0x3a47,0x3a68};
    unsigned theme = ui_theme_get();
    for (unsigned i = 0, n = width * PAINT_ROWS; theme != 1U && i < n; ++i) {
        uint16_t c = paint_pixels[i];
        if (theme == 0U) { if (!(c & 0xc618)) paint_pixels[i] = moss[(c >> 5) & 15U]; continue; }
        /* MATRIX: everything in green phosphor by brightness; red (play head, record) stays red */
        unsigned r = c >> 11, g = (c >> 5) & 63U, b = c & 31U;
        if (r > 22U && g < 24U) continue;
        unsigned l = (r * 2U + g + b) * 63U / 187U; /* 0..63 */
        paint_pixels[i] = (uint16_t)(((l >> 4) << 11) | (l << 5) | (l >> 3));
    }
}
/* New bands start during the first PAINT_START_MS of a poll; waiting for the
 * DMA ends at PAINT_WAIT_MS. A full-width band is 7,680 bytes, about 4 ms of
 * SPI at the estimated 16 MHz, so a poll outputs at least two bands (one
 * before) and leaves the control task idle for at least 4 ms of its tick. */
enum { PAINT_START_MS = 4, PAINT_WAIT_MS = 6 };
/* Narrow a composed band to the columns whose pixels differ from the panel and
 * pack them tightly. Returns 0 when the panel already shows the whole band. */
static unsigned paint_changed(unsigned band, unsigned *left, unsigned *width)
{
    unsigned x0 = *left, w = *width, lo = x0 + w, hi = x0;
    for (unsigned k = x0 / PAINT_BLOCK; k * PAINT_BLOCK < x0 + w; ++k) {
        unsigned a = k * PAINT_BLOCK, b = a + PAINT_BLOCK, bit = 1U << k;
        if (a < x0 || b > x0 + w) { /* partly composed: send that part, the block is no longer known */
            paint_known[band] &= (uint16_t)~bit;
            if (a < x0) a = x0;
            if (b > x0 + w) b = x0 + w;
        } else {
            uint32_t h = 2166136261U;
            for (unsigned r = 0; r < PAINT_ROWS; ++r)
                for (unsigned c = a; c < b; ++c) h = (h ^ paint_pixels[r * w + c - x0]) * 16777619U;
            if ((paint_known[band] & bit) && paint_shown[band][k] == h) continue;
            paint_shown[band][k] = h;
            paint_known[band] |= (uint16_t)bit;
        }
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    if (hi <= lo) return 0;
    if (hi - lo != w)
        for (unsigned r = 0; r < PAINT_ROWS; ++r)
            memmove(paint_pixels + r * (hi - lo), paint_pixels + r * w + lo - x0, (hi - lo) * sizeof *paint_pixels);
    *left = lo;
    *width = hi - lo;
    return 1;
}
static void paint_flush_bands(void);
static void paint_flush(void)
{
    uint32_t t0 = fm1_output_timer(), ms = fm1_millis();
    paint_flush_bands();
    ui_meter_add(UI_FLUSH, fm1_output_timer_since(t0), fm1_millis() - ms);
}
static void paint_flush_bands(void)
{
    uint32_t started = fm1_millis();
    static unsigned held_left, held_width;
    paint_rendering = 1;
    cache = &band_cache;
    for (;;) {
        if (paint_held == ~0U) {
            if (fm1_millis() - started >= PAINT_START_MS) break;
            /* Fresh knob/key feedback goes ahead of the rest of a page repaint.
             * Rotate within that set so continuous motion cannot starve its text.
             * Every other band is taken in plain order, so bands that animation
             * keeps out of the priority set (scope edges, a knob after the turn)
             * still get painted. */
            static unsigned fair;
            unsigned band = PAINT_BANDS;
            fair ^= 1U;
            for (unsigned i = 0; paint_priority && fair && i < PAINT_BANDS; ++i) {
                unsigned candidate = (paint_next + i) % PAINT_BANDS;
                if (paint_priority & (1U << candidate)) { band = candidate; break; }
            }
            for (unsigned i = 0; band == PAINT_BANDS && i < PAINT_BANDS; ++i) {
                unsigned candidate = (paint_next + i) % PAINT_BANDS;
                if (paint_left[candidate] < paint_right[candidate]) band = candidate;
            }
            if (band == PAINT_BANDS) break; /* the panel shows the whole frame */
            paint_next = (band + 1U) % PAINT_BANDS;
            held_left = paint_left[band];
            held_width = paint_right[band] - held_left;
            /* Later damage marks the band again and repaints it after this one. */
            paint_left[band] = FM1_SCREEN_WIDTH;
            paint_right[band] = 0;
            paint_priority &= ~(1U << band);
            paint_compose(band, held_left, held_width);
            if (!paint_changed(band, &held_left, &held_width)) continue;
            paint_held = band;
        }
        /* The DMA borrows the other buffer until it reports completion. */
        int pending;
        while ((pending = fm1_screen_pending()) > 0 && fm1_millis() - started < PAINT_WAIT_MS) {}
        if (pending > 0) break; /* this band goes out first on the next poll */
        /* Rasterize directly into the tightly packed DMA rectangle. */
        if (pending < 0 || fm1_screen_submit(held_left, paint_held * PAINT_ROWS, held_width, PAINT_ROWS,
                                             paint_pixels, held_width * PAINT_ROWS)) {
            saved_cache.shown_page = ~0U; /* Retry the whole page after a transport error. */
            memset(paint_known, 0, sizeof paint_known);
            paint_held = ~0U;
            break;
        }
        paint_held = ~0U;
        paint_half ^= 1U;
        paint_pixels = paint_buffer[paint_half];
    }
    paint_rendering = 0;
    cache = &saved_cache;
}
static int release_keys_except(uint32_t keep)
{
    for (unsigned key = 0; key < 27; ++key)
        if (key_note[key] != 0xff && !(keep & (1U << key))) {
            if (!post((uint8_t)(0x80 + groove_midi_input_channel(key_track[key])), key_note[key], 0, 3))
                return 0;
            key_note[key] = 0xff;
            project_keys &= ~(1U << key);
        }
    return 1;
}
static int release_keys(void) { return release_keys_except(0); }
/* The ENGINES cursor follows every PRESETS detent; the engine itself is built
 * once the knob has rested ENGINE_SETTLE_MS (or before ALGORITHM edits the
 * sound), so a fast turn does not build every engine on the way. Autosave is
 * armed once per change. A key release that cannot be posted retries later. */
enum { ENGINE_SETTLE_MS = 200 };
static void engine_settle(uint32_t now, unsigned force)
{
    if (eng_pending == 0xff || (eng_warn && !eng_confirm) || (!force && now - eng_at < ENGINE_SETTLE_MS)) return;
    unsigned t = eng_track, next = eng_pending;
#if FM1_BINAURAL
    if (next == ENG_BINAURAL) { /* the BINAURAL tile opens its page: no engine, no track in the mixer */
        eng_pending = 0xff; eng_warn = 0;
        page = BINAURAL; bin_page = 0; bin_from_eng = 1; from_global = 0; cache->shown_page = ~0U;
        return;
    }
#endif
    unsigned cur = micro_d_track(t) ? (unsigned)ENGINE_MICRO : groove_source_type(t);
    if (next != cur && !eng_confirm && eng_is_drum(next) != (groove_pad_count(t) != 0)) {
        eng_warn = 1; /* drums <-> pitched clears the pattern: wait for SAVE (OK) or HOME (CANCEL) */
        return;
    }
    if (next != cur && !release_keys()) return;
    eng_pending = 0xff; eng_warn = 0;
    cache->shown_page = ~0U; /* the cursor shows the real engine again */
    if (next == cur) return;
    var_split_clear(t); /* C2: a new engine, no splits */
    if (cur == ENGINE_MICRO) micro_d_detach(t);
    if (next == ENGINE_MICRO) {
        source_result = micro_d_attach(t);
        if (!source_result) micro_d_preset_load(t, micro_d_preset(t));
    } else source_result = groove_source_set(t, next);
    if (!source_result) {
        sounds_forget(t);
        memset(tone_pages[t], 0, sizeof tone_pages[t]);
        drum_parts[t] = 0;
        if (t == track) { drum = 0; selected_note = groove_pad_count(t) ? groove_pad_note(t, 0) : 60; }
    } else { notice = "ENGINE MEMORY FULL"; notice_until = now + 2000U; }
    autosave_at = now + 3000U; if (!autosave_at) autosave_at = 1;
}
/* HOME and the list load immediately, including over unsaved edits.
 * Selecting a preset replaces the workspace without writing a project slot. */
static int preset_load(unsigned start, uint32_t now)
{
    var_forget(); /* the variation originals belong to the old pattern */
    chain_loops = 0;
    project_result = preset_bank ? groove_factory_load(preset_slot) : groove_project_load_slot(preset_slot);
    if (project_result) {
        notice = project_result == GROOVE_PROJECT_EMPTY ? "EMPTY PROJECT SLOT" : "PROJECT LOAD FAILED";
        notice_until = now + 2000U;
        return project_result;
    }
    key_auto_reset(); /* KEY AUTO: the new project's first chord sets the key */
    unsigned channels = 0; uint32_t keep = 0;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        if (groove_track_role(t) == GROOVE_TRACK_PLAY) channels |= 1U << groove_midi_input_channel(t);
    for (unsigned key = 0; key < 27; ++key)
        if (key_note[key] != 0xff && (channels & (1U << groove_midi_input_channel(key_track[key]))))
            keep |= 1U << key;
    /* A physical key can feed Normal and Play together. The project has
     * retired Normal voices; retain its original pitch until key release. */
    if (!release_keys_except(keep)) return -1;
    project_keys = keep;
    blocked_keys |= previous_keys & ~keep;
    preset_loaded = 1;
    preset_project_revision = groove_project_revision();
    preset_current_slot = preset_slot; preset_current_bank = preset_bank;
    writing = cursor = drum = advanced_edit = 0; step_follow = 1;
    memset(operator_page, 0, sizeof operator_page);
    memset(operator_selected, 0, sizeof operator_selected);
    memset(lfo_page, 0, sizeof lfo_page);
    memset(tone_pages, 0, sizeof tone_pages);
    memset(drum_parts, 0, sizeof drum_parts);
    selected_note = groove_pad_count(track) ? groove_pad_note(track, 0) : 60;
    page = music_page = TRACKS;
    notice = start ? "PROJECT PLAYING" : "PROJECT LOADED";
    notice_until = now + 2000U;
    if (start) {
        struct groove_view view = {0}; groove_get_view(track, cursor, &view);
        /* A live Play session keeps its transport during project replacement.
         * Sending Start again would rewind that clock and restart its ARP. */
        if (!view.playing && !post(0xfa, 0, 0, 1)) return -1;
    }
    return 0;
}
/* Long notes, recorded live: the instrument writes a note when its key goes
 * down but not how long it was held. Here the hold is measured and written
 * into the steps: shorter than a step -> GATE in sixteenths; longer -> the
 * first step full length + TIE, the same note (chord) tied over the next
 * steps, the last one untied with the remainder as its GATE (16 steps at most). */
static uint8_t long_count[GROOVE_TRACKS], long_step[GROOVE_TRACKS] = {255, 255, 255, 255, 255, 255, 255, 255};
static uint32_t long_ms[GROOVE_TRACKS];
static void step_set(unsigned t, unsigned step, unsigned setting, unsigned value)
{
    int cur = (int)groove_step_setting(t, step, setting);
    if ((int)value != cur) (void)groove_step_setting_edit(t, step, setting, (int)value - cur);
}
static void long_note_start(unsigned t, uint32_t now)
{
    if (!long_count[t]++) { long_ms[t] = now; long_step[t] = (uint8_t)groove_sequence_record_position(t); }
}
/* While a key is held in live REC, its step plays at once; a 50 % gate
 * would send the note-off of the same pitch and cut the held key. So the
 * step is tied (full gate) as soon as it is written; long_note_end then
 * sets the real length on release. */
static void long_note_hold(void)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        unsigned s = long_step[t];
        if (!long_count[t] || s >= GROOVE_STEPS || groove_step_setting(t, s, 4)) continue;
        struct groove_view v = {0}; groove_get_view(t, s, &v);
        if (!v.step.velocity) continue;
        step_set(t, s, 1, 15); step_set(t, s, 4, 1);
    }
}
/* Editing a step inside a long note (a chain of tied steps): the note
 * before ends at this step (its tie is cut), and the rest of the chain from
 * here on, the old note's tail, is cleared, so the new note sounds at once. */
static void break_long_note(unsigned t, unsigned step)
{
    unsigned len = groove_sequence_length(t);
    if (!len || step >= len) return;
    unsigned prev = (step + len - 1U) % len;
    struct groove_view pv = {0}; groove_get_view(t, prev, &pv);
    unsigned tied_in = pv.step.velocity && groove_step_setting(t, prev, 4);
    if (tied_in) step_set(t, prev, 4, 0);
    if (!tied_in && !groove_step_setting(t, step, 4)) return;
    struct groove_view cv = {0}; groove_get_view(t, step, &cv);
    unsigned old = tied_in ? pv.step.note : cv.step.note;
    for (unsigned n = 0, st = step; n < len; ++n) { /* C3: a long note may span the whole sequence */
        unsigned more = groove_step_setting(t, st, 4);
        step_set(t, st, 4, 0);
        if (st != step) {
            struct groove_view sv = {0}; groove_get_view(t, st, &sv);
            if (!sv.step.velocity || sv.step.note != old) break;
            groove_sequence_edit(t, st, old, 0); /* the old note's tail */
        }
        if (!more) break;
        st = (st + 1U) % len;
    }
}
static void long_note_end(unsigned t, uint32_t now)
{
    if (!long_count[t] || --long_count[t] || long_step[t] >= GROOVE_STEPS) return;
    unsigned s = long_step[t], tempo = frame_views[t].tempo, length = groove_sequence_length(t);
    long_step[t] = 255;
    if (!tempo || !length) return;
    struct groove_view v = {0}; groove_get_view(t, s, &v);
    if (!v.step.velocity) return;
    unsigned step_ms = 15000U / tempo, len16 = (now - long_ms[t]) * 16U / (step_ms ? step_ms : 1U);
    if (len16 <= 16U) { step_set(t, s, 1, len16 < 1U ? 1U : len16 > 15U ? 15U : len16); step_set(t, s, 4, 0); return; }
    unsigned n = seq_rec_steps(len16, length), chord = groove_step_setting(t, s, 3); /* C3: up to the sequence length (was 16) */
    step_set(t, s, 1, 15); step_set(t, s, 4, 1);
    for (unsigned j = 1; j < n; ++j) {
        unsigned st = (s + j) % length, last = j == n - 1, rest = len16 - 16U * (n - 1U);
        groove_sequence_edit(t, st, v.step.note, v.step.velocity);
        step_set(t, st, 3, chord);
        step_set(t, st, 4, !last);
        step_set(t, st, 1, last ? (rest < 1U ? 1U : rest > 15U ? 15U : rest) : 15U);
    }
}
static void toggle_step(unsigned step)
{
    if (!groove_pad_count(track)) break_long_note(track, step); /* a note on a long note's tail ends it here (same as the grid entry) */
    struct groove_view v = {0};
    groove_get_view(track, step, &v);
    unsigned enabled =
        groove_pad_count(track) != 0 ? (v.step.drums & (1U << drum)) != 0 : v.step.velocity != 0;
    if (!groove_pad_count(track) && enabled) selected_note = v.step.note;
    groove_sequence_edit(track, step, selected_note, enabled ? 0 : 100);
    if (enabled) { struct groove_view after = {0}; groove_get_view(track, step, &after); if (!(groove_pad_count(track) ? after.step.drums : after.step.velocity)) var_split_set(track, step, 0); } /* C2: a cleared step loses its split */
    cursor = step;
    step_follow = 0;
    if (page != TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
}
void groove_screen_init(void)
{
    track = cursor = drum = writing = fm_group = op = 0;
    page = music_page = subpage_return = project_return = rec_before_press = TRACKS;
    lfo_init();
    rec_down = rec_used = 0;
    selected_note = 60;
    step_follow = 1;
    transport_seen = 0;
    notice = NULL;
    octave = 0;
    source_preview = action = action_confirm = tone_clip_ready = 0;
    seq_page = arp_page = 0;
    memset(midi_seen, 0, sizeof midi_seen);
    memset(midi_until, 0, sizeof midi_until);
    previous_buttons = previous_keys = blocked_keys = retry_reset = 0;
    ble_device = ble_was_linked = ble_view = ble_action = 0;
    ble_result = source_result = 0;
    ble_forget = ble_stage = ble_seconds = 0;
    ble_stage_since = fm1_millis();
    struct fm1_engine_stats health;
    fm1_engine_stats_read(&health);
    health_output = groove_midi_dropped();
    health_at = fm1_millis(); health_underruns = health.underruns; health_dropped = health.midi_dropped;
    struct fm1_audio_stats dma;
    fm1_audio_stats_read(&dma);
    health_dma = dma.overruns; health_adapted = 0;
    project_result = 0;
    project_reload = project_operation = 0;
    project_slot = project_factory = name_cursor = 0;
    preset_slot = preset_loaded = 0;
    preset_bank = 1;
    preset_current_slot = 0; preset_current_bank = 1;
    advanced_edit = edit_down = edit_long = 0;
    seq_down = 0;
    arp_down = arp_long = 0;
    memset(project_stored, 0, sizeof project_stored);
    cache->shown_page = ~0U;
    led_cache = button_led_cache = 0;
    memset(key_note, 0xff, sizeof key_note); project_keys = 0;
    memset(operator_page, 0, sizeof operator_page);
    memset(operator_selected, 0, sizeof operator_selected);
    memset(lfo_page, 0, sizeof lfo_page);
    memset(tone_pages, 0, sizeof tone_pages);
    memset(drum_parts, 0, sizeof drum_parts);
    operator_held = operator_long = 0;
    memset(paint_left, FM1_SCREEN_WIDTH, sizeof paint_left);
    memset(paint_right, 0, sizeof paint_right);
    paint_next = paint_rendering = paint_priority = 0;
    paint_held = ~0U;
    memset(paint_known, 0, sizeof paint_known); /* fm1_screen_init cleared the panel */
    memset(encoder_direction, 0, sizeof encoder_direction);
    memset(encoder_run, 0, sizeof encoder_run);
    fm1_controls_init();
#ifndef FM1_HOST_TEST
    {   /* Power-on keys (OCT- = SAFE, OCT- with OCT+ = USB download mode).
         * The scan needs a few ms; the wait is bounded even if time stood still. */
        uint32_t t0 = fm1_millis();
        for (unsigned i = 0; fm1_millis() - t0 < 30U && i < 2000000U; ++i) {}
        struct fm1_controls first;
        fm1_controls_read(&first);
        boot_guard_keys(first.buttons);
    }
#endif
    for (unsigned key = 0; key < 41; ++key) fm1_led_set(key, 0);
    fm1_screen_init();
    boot_started = fm1_millis();
    boot_active = !boot_guard_safe();
    (void)bb_start(fm1_millis(), boot_guard_safe()); /* reads what the last session left, before anything else runs */
    if (boot_guard_safe()) page = SAFE;
    struct fm1_ble_status ble = {0};
    fm1_ble_get_status(&ble);
    draw(&ble);
    paint_flush();
    frame_ms = fm1_millis();
}
static void select_track(int delta)
{
    unsigned before = track;
    track = (unsigned)clamp((int)track + delta, 0, 7);
    if (page == FM_QUICK && groove_source_type(track) != GROOVE_DX7) page = TONE_EDIT;
    if (page == TONE_EDIT && !advanced_edit && groove_source_type(track) == GROOVE_DX7) page = FM_QUICK;
    /* ENV exists only on DX7 and 303 (user 13.11): never show it for another engine (CHAIN / MIDI swap keeps the page) */
    if (page == FM_ENV && groove_source_type(track) != GROOVE_DX7 && groove_source_type(track) != GROOVE_303) page = TONE_EDIT;
    if ((groove_source_type(track) != GROOVE_DX7 && page == FM_EDIT) ||
        (groove_source_type(track) == GROOVE_DX7 && page == TONE_EDIT)) page = OVERVIEW;
    if (before != track) {
        cursor %= groove_sequence_length(track);
        cache->middle_cache = ~0U; source_result = 0;
        drum = drum_parts[track] < groove_pad_count(track) ? drum_parts[track] : 0;
        if (!groove_pad_count(track) && groove_pad_count(before)) selected_note = 60;
    }
    if (groove_pad_count(track)) {
        drum %= groove_pad_count(track);
        selected_note = groove_pad_note(track, drum);
    }
}
void groove_screen_tap(uint32_t now)
{
    int bpm = groove_tempo_tap(now);
    notice = bpm < 0 ? "TAP: EXT CLOCK" : bpm ? "TAP TEMPO APPLIED" : "TAP AGAIN";
    notice_until = now + 1500U;
}
static int apply_controls(const struct fm1_controls *controls, uint32_t now,
                          const struct fm1_ble_status *ble)
{
    uint32_t pressed = controls->buttons & ~previous_buttons;
    uint32_t old_keys = previous_keys, key_down = controls->keys & ~old_keys;
    unsigned play_handled = 0;
    static unsigned play_down;
    static uint32_t play_ms;
    previous_buttons = controls->buttons;
    previous_keys = controls->keys;
    blocked_keys &= controls->keys;
    if (pressed || encoder_active(controls->encoders)) notice = NULL;
    if (retry_reset) {
        blocked_keys = controls->keys;
        if (!post(0xff, 0, 0, 1))
            return 0;
        retry_reset = 0;
        memset(key_note, 0xff, sizeof key_note); project_keys = 0;
    }
    unsigned old_page = page, old_mode = navigation_mode();
    if (!(controls->buttons & BUTTON(EDIT))) edit_down = edit_long = 0;
    if (!(controls->buttons & BUTTON(ARP))) arp_down = arp_long = 0;
    if (seq_tap && !(controls->buttons & BUTTON(SEQ))) { /* SEQ rework C1: SEQ tapped on the grid = page 1 STEP */
        seq_tap = 0;
        if (page == STEPS && !seq_page) { seq_page = seq_nav_tap(seq_page); step_follow = 0; knob_enter(); cache->shown_page = ~0U; goto controls_done; }
    }
    if (!(controls->buttons & BUTTON(SEQ))) seq_down = 0;
    if (pressed & BUTTON(SEQ)) {
        seq_down = 1; seq_ms = now;
    }
    if (seq_down && now - seq_ms >= 600U) { /* hold SEQ: chain of sequences */
        seq_down = seq_tap = 0; page = CHAIN; cache->shown_page = ~0U;
        goto controls_done;
    }
    if (page == OVERVIEW || page == STEPS || page == TRACKS) music_page = page;
    if (smp_active && page != SMP_REC && page != SMP_CHOP) { rec_free(); rec_listen(0, 0, 0); smp_active = smp_saving = smp_encoded = 0; }
    if ((pressed & BUTTON(REC)) && page == TONE_EDIT && groove_source_type(track) == GROOVE_SAMPLE && !writing) {
        /* v7: REC in the sampler editor = record a sample into the chosen pad */
        page = SMP_REC; smp_pad = tone_part(); smp_active = 1; rec_free(); smp_ram_at = 0; cache->shown_page = ~0U;
        goto controls_done;
    }
    if ((page == SMP_REC || page == SMP_CHOP) && smp_control(controls, pressed, now)) goto controls_done;
    if (pressed & BUTTON(REC)) rec_before_press = writing;
    /* HOME defers REC until release so REC+SELECT never starts a take.
     * A held gesture preserves the previous recording state. */
    if ((pressed & BUTTON(REC)) && page == OVERVIEW &&
        !(controls->buttons & ~BUTTON(REC))) {
        rec_down = 1; rec_used = 0; rec_ms = now;
    }
    if (rec_down) {
        if (page != OVERVIEW || (pressed & ~BUTTON(REC))) rec_down = 0;
        else if (!(controls->buttons & BUTTON(REC))) {
            if (!rec_used && now - rec_ms < 600U) writing ^= 1U;
            rec_down = 0;
        } else {
            pressed &= ~BUTTON(REC);
            if (controls->encoders[0]) {
                rec_used = 1;
                if (now - rec_ms >= 600U) {
                    cursor = (unsigned)clamp((int)cursor + controls->encoders[0], 0,
                        (int)groove_sequence_length(track) - 1);
                    groove_edit_seek(track, cursor);
                    step_follow = 0;
                }
                goto controls_done;
            }
        }
    }
    /* HOME: tap = one level up (see below); hold = home (mixer).
     * Any other control used while HOME is held makes it a combination. */
    if (pressed & BUTTON(HOME)) { home_down = 1; home_long = home_used = 0; home_ms = now; }
    if (home_down) {
        if ((controls->buttons & ~BUTTON(HOME)) || encoder_active(controls->encoders) || controls->keys)
            home_used = 1;
        if (!(controls->buttons & BUTTON(HOME))) {
            home_down = 0;
            if (!home_long && !home_used) {
                if (eng_warn) { eng_warn = 0; eng_pending = 0xff; } /* ENGINES dialog: HOME = CANCEL, the cursor goes back to the real engine */
                else if (page == TRACKS && engines_shown()) { engines_until = 0; cache->shown_page = ~0U; }
                else if (page == BINAURAL && bin_from_eng) { bin_from_eng = 0; page = TRACKS; engines_until = 1; cache->shown_page = ~0U; } /* back to ENGINES */
                else if (page == ACTIONS && action_confirm) action_confirm = 0;
                else if (page == BLE && ble_forget == 1) ble_forget = 0;
                else if ((from_global && (page == SCOPE || page == INFO || page == BLE || page == BINAURAL)) || (page == STEPS && seq_midi())) {
                    from_global = 0; seq_page = 0; page = GLOBAL; cache->shown_page = ~0U;
                }
                else if (page == STEPS && seq_page) { seq_page = 0; cache->shown_page = ~0U; }
                else {
                    /* v7: one level up, never back through every page visited:
                     * a sub page goes to its main page, a main page to the mixer. */
                    unsigned sound = groove_source_type(track) == GROOVE_DX7 ? FM_QUICK : TONE_EDIT;
                    unsigned up = page == CHAIN || page == STEP_EDIT || page == RHYTHM ? STEPS
                                : page == FM_EDIT || page == FM_ENV || page == FM_LFO || page == SMP_REC || page == SMP_CHOP || page == NAME ? sound
                                : page == FAQ ? INFO : page == BLE || page == INFO || page == BINAURAL ? GLOBAL : TRACKS;
                    if (page == SMP_REC || page == SMP_CHOP) { rec_free(); rec_listen(0, 0, 0); }
                    if (up == page) up = TRACKS;
                    seq_page = 0; page = up; back_n = 0; cache->shown_page = ~0U;
                }
                goto controls_done;
            }
        } else if (!home_long && !home_used && now - home_ms >= 600U) {
            home_long = 1; back_n = 0;
            page = page == SCOPE ? TRACKS : SCOPE; cache->shown_page = ~0U; /* hold HOME: the oscilloscope (again: mixer) */
            goto controls_done;
        }
    }
    /* FX: tap = the track's effects (TRACK FX); each next tap goes on: TRACK FX / SHARED FX -> MASTER FX -> close;
     * hold 600 ms = MASTER FX straight away. */
    {
        static unsigned fx_down, fx_long;
        static uint32_t fx_ms;
        if (pressed & BUTTON(FX)) { fx_down = 1; fx_long = 0; fx_ms = now; goto controls_done; }
        if (fx_down) {
            if (!fx_long && now - fx_ms >= 600U) { fx_long = 1; page = MASTER_FX; cache->shown_page = ~0U; }
            if (!(controls->buttons & BUTTON(FX))) {
                fx_down = 0;
                if (!fx_long) {
                    if (page == EFFECTS) page = MASTER_FX;
                    else if (page == MASTER_FX) page = music_page == page ? TRACKS : music_page;
                    else { page = EFFECTS; effects_page = 0; }
                    cache->shown_page = ~0U;
                }
            }
            goto controls_done;
        }
    }
    if (page == INFO && controls->encoders[1] > 0) { page = FAQ; faq_page = 0; cache->shown_page = ~0U; goto controls_done; }
    if (page == FAQ) {
        if (controls->encoders[0]) faq_page = (unsigned)(((int)faq_page + (controls->encoders[0] > 0 ? 1 : -1) + FAQ_PAGES) % FAQ_PAGES);
        goto controls_done;
    }
    if (page == FM_QUICK && controls->encoders[0]) quick_row = controls->encoders[0] > 0 ? 1U : 0U; /* EDIT: TONE / ENV window */
    if (page == GLOBAL && !(pressed & ~BUTTON(SAVE))) {
        if (controls->encoders[0]) glob_move(controls->encoders[0]); /* SELECT: tile by tile, round */
        int d = controls->encoders[2];
        unsigned touched = 0;
        for (unsigned i = 0; i < 4; ++i) touched |= controls->encoders[2 + i] != 0;
        if (glob_row == GR_BT) { /* KNOB 1 Bluetooth, 2 audio, 3 BLE MIDI role, 4 clock */
            unsigned flags = groove_settings_radio_flags(), role = groove_settings_role_value();
            for (unsigned i = 0; i < 2; ++i) {
                int delta = controls->encoders[2 + i];
                if (delta > 0) flags |= 1U << i; else if (delta < 0) flags &= ~(1U << i);
            }
            if (controls->encoders[4]) role = (unsigned)clamp((int)role + controls->encoders[4], FM1_BLE_PERIPHERAL, FM1_BLE_CENTRAL);
            if (flags != groove_settings_radio_flags() || role != groove_settings_role_value()) ble_result = groove_settings_radio(flags, role);
            if (controls->encoders[5]) groove_settings_clock(controls->encoders[5] > 0 ? GROOVE_CLOCK_MIDI : GROOVE_CLOCK_INTERNAL);
        } else if (glob_row == GR_KEY) { /* K1 KEY AUTO / C..B, K2 MODE MAJ / MIN */
            if (controls->encoders[2]) key_edit(0, controls->encoders[2] > 0 ? 1 : -1);
            if (controls->encoders[3]) key_edit(1, controls->encoders[3]);
        } else if (glob_row == GR_CLICK) { /* K1 CLICK, K2 COUNT IN */
            if (controls->encoders[2]) groove_sequence_setting_edit(1, controls->encoders[2]);
            if (controls->encoders[3]) groove_sequence_setting_edit(2, controls->encoders[3]);
        } else if (d) {
            if (glob_row == GR_OUTPUT) mfx_output_set((unsigned)clamp((int)mfx_output() + (d > 0 ? 1 : -1), 0, 4));
            else if (glob_row == GR_BPM) groove_tempo_edit(d);
            else if (glob_row == GR_LOWCUT) mfx_lowcut_set(d > 0);
            else if (glob_row == GR_LANG && lang_get() != (d > 0)) lang_set(d > 0);
            else if (glob_row == GR_THEME) ui_theme_set((ui_theme_get() + (d > 0 ? 1U : 2U)) % 3U);
            if (glob_row == GR_LANG || glob_row == GR_THEME) cache->shown_page = ~0U; /* every page redraws */
        }
        if (touched) { autosave_at = now + 3000U; if (!autosave_at) autosave_at = 1; } /* SAVE keeps them too */
        /* ALGORITHM (turned right) opens the page behind a row: BLUETOOTH's
         * device list, SCOPE, INFO. SAVE saves. */
        unsigned opens = glob_row == GR_SCOPE || glob_row == GR_INFO || glob_row == GR_BT || glob_row == GR_BINAURAL || glob_row == GR_3D || glob_row == GR_MIDI;
        if (opens && controls->encoders[1] > 0) {
            from_global = 1; bin_from_eng = 0;
            if (glob_row == GR_SCOPE) page = SCOPE;
            else if (glob_row == GR_MIDI) { page = STEPS; seq_page = SEQ_MIDI_CLOCK; knob_enter(); step_follow = 0; cache->shown_page = ~0U; } /* MIDI submenu, CLOCK first */
            else if (glob_row == GR_BINAURAL || glob_row == GR_3D) { page = BINAURAL; bin_page = glob_row == GR_3D; cache->shown_page = ~0U; }
            else if (glob_row == GR_INFO) { page = INFO; cache->shown_page = ~0U; }
            else if (glob_row == GR_BT) { page = BLE; ble_view = 1; ble_action = frame_ble.count ? 0 : 1; ble_forget = 0; ble_result = 0;
                                          if (frame_ble.state == FM1_BLE_IDLE) ble_result = fm1_ble_scan(); }
        }
        if (pressed & BUTTON(SAVE)) { if (groove_project_save_slot(groove_project_slot()) == 0) { notice = "SAVED"; notice_until = now + 1200U; } }
        goto controls_done;
    }
    if (page == SCOPE && !pressed) {
        for (unsigned i = 0; i < 7; ++i)
            if (controls->encoders[i]) scope_coat = (uint8_t)((scope_coat + (controls->encoders[i] > 0 ? 1U : 3U)) % 4U);
        goto controls_done;
    }
#if FM1_BINAURAL
    if (page == BINAURAL && bin_from_eng && controls->encoders[6] && !(pressed & ~BUTTON(SAVE))) {
        /* opened from ENGINES: PRESETS goes on through the tiles (the cursor leaves BINAURAL) and shows ENGINES again */
        page = TRACKS; engines_until = 1; bin_from_eng = 0;
        blocked_keys |= controls->keys;
        eng_pending = (uint8_t)eng_step(ENG_BINAURAL, controls->encoders[6]); eng_track = (uint8_t)track; eng_at = now;
        cache->shown_page = ~0U;
        goto controls_done;
    }
    if (page == BINAURAL && !(pressed & ~BUTTON(SAVE))) {
        /* v11 BINAURAL: knobs rhythm / carrier / timer / noise level, SELECT the
         * beat (fine), ALGORITHM the noise type. 3D SOUND: mode / width / feed / mix. */
        static const uint8_t map[2][6] = {{BIN_BEAT, BIN_NTYPE, BIN_PRESET, BIN_CARRIER, BIN_TIMER, BIN_NLEVEL},
                                          {BIN_PARAMS, BIN_PARAMS, BIN_MODE, BIN_WIDTH, BIN_FEED, BIN_MIX}};
        unsigned touched = 0;
        for (unsigned i = 0; i < 6; ++i) {
            int e = controls->encoders[i], n = e < 0 ? -e : e;
            unsigned param = map[bin_page][i];
            if (param == BIN_PARAMS) continue;
            if (n > 8) n = 8;
            for (int k = 0; k < n; ++k) binaural_edit(param, e);
            touched |= n != 0;
        }
        if (touched) { autosave_at = now + 3000U; if (!autosave_at) autosave_at = 1; }
        if (pressed & BUTTON(SAVE)) { if (groove_project_save_slot(groove_project_slot()) == 0) { notice = "SAVED"; notice_until = now + 1200U; } }
        goto controls_done;
    }
#endif
    if (page == LFOS && !pressed) {
        if (controls->encoders[0]) { /* user 10.11 (F7): SELECT right = LFO 2, left = LFO 1; the LFO button cycle stays */
            unsigned want = controls->encoders[0] > 0;
            if (want != lfo_sel) { lfo_sel = want; cache->shown_page = ~0U; }
        }
        for (unsigned i = 0; i < 4; ++i) lfo_edit(lfo_sel, i, fm_encoder_delta(2 + i, controls->encoders[2 + i], i == 1 ? LFO_RATES : i == 2 ? 127U : 0U));
        goto controls_done;
    }
    if (page == CHAIN && !(pressed & ~BUTTON(SAVE))) {
        int row = (int)chain_cursor / 4 + controls->encoders[2], col = (int)chain_cursor % 4 + controls->encoders[0];
        row = clamp(row, 0, 3); col = clamp(col, 0, 3);
        chain_cursor = (uint8_t)(row * 4 + col);
        if (controls->encoders[3]) chain_slot[chain_cursor] = (uint8_t)clamp((int)chain_slot[chain_cursor] + controls->encoders[3], 0, GROOVE_PROJECT_SLOTS);
        if (controls->encoders[4]) chain_rep[chain_cursor] = (uint8_t)clamp((int)chain_rep[chain_cursor] + (controls->encoders[4] > 0 ? 1 : -1), 0, 15);
        if (controls->encoders[5]) chain_on = controls->encoders[5] > 0;
        if (pressed & BUTTON(SAVE) && chain_slot[chain_cursor]) { /* jump here now */
            chain_pos = chain_cursor; chain_on = 1; chain_loops = 0;
            struct groove_view tv = {0}; groove_get_view(0, 0, &tv);
            preset_slot = chain_slot[chain_cursor] - 1U; preset_bank = preset_current_bank; preset_refresh();
            preset_load(tv.playing, now);
            page = CHAIN; cache->shown_page = ~0U;
        }
        goto controls_done;
    }
    if (page == MASTER_FX && !pressed) {
        if (controls->encoders[0]) {
            fx_row = fx_order[clamp((int)fx_pos(fx_row) + controls->encoders[0], 0, FX_ROWS - 1)];
        }
        master_fx_edit(controls->encoders);
        goto controls_done;
    }
    /* Emergency stop has the same meaning in every modal page. */
    if ((controls->buttons & (BUTTON(HOME) | BUTTON(PLAY))) ==
            (BUTTON(HOME) | BUTTON(PLAY)) && (pressed & (BUTTON(HOME) | BUTTON(PLAY)))) {
        blocked_keys |= controls->keys;
        if (!post(0xff, 0, 0, 1)) return 0;
        memset(key_note, 0xff, sizeof key_note); project_keys = 0;
        page = music_page = TRACKS;
        writing = project_reload = edit_down = edit_long = 0;
        arp_down = arp_long = seq_down = rec_down = 0;
        edit_record(0, 0);
        return 1;
    }
    /* A project received over MIDI can replace the current instrument while
     * its editor is open. Leave editors that cannot apply to the new source. */
    if (page == NAME && groove_source_type(track) != GROOVE_DX7)
        page = OVERVIEW;
    if ((controls->buttons & (BUTTON(HOME) | BUTTON(SAVE))) == (BUTTON(HOME) | BUTTON(SAVE)) &&
        (pressed & (BUTTON(HOME) | BUTTON(SAVE)))) {
        int r = groove_edit_undo();
        notice = r ? "NOTHING TO UNDO" : "UNDO / REDO APPLIED";
        notice_until = now + 2000U;
        page = music_page;
        goto controls_done;
    }
    if ((pressed & BUTTON(SEQ)) && (controls->buttons & BUTTON(HOME))) {
        groove_screen_tap(now);
        seq_down = edit_down = 0;
        if (controls->buttons & BUTTON(HOME)) page = music_page;
        goto controls_done;
    }
    if ((controls->buttons & (BUTTON(HOME) | BUTTON(GLO))) == (BUTTON(HOME) | BUTTON(GLO)) &&
        (pressed & (BUTTON(HOME) | BUTTON(GLO)))) {
        page = BLE; ble_view = ble_forget = ble_action = 0; ble_result = 0;
        goto controls_done;
    }
    if ((controls->buttons & (BUTTON(SEQ) | BUTTON(EDIT))) == (BUTTON(SEQ) | BUTTON(EDIT)) &&
        (pressed & (BUTTON(SEQ) | BUTTON(EDIT)))) {
        if (!release_keys()) return 0;
        blocked_keys |= controls->keys;
        page = ACTIONS; action_confirm = 0; edit_down = seq_down = 0;
        goto controls_done;
    }
    /* Panel shortcuts precede modal editors. A menu must not swallow a
     * navigation key, transport control or the release of a held gesture. */
    if ((controls->buttons & (BUTTON(EDIT) | BUTTON(REC))) == (BUTTON(EDIT) | BUTTON(REC)) &&
        (pressed & (BUTTON(EDIT) | BUTTON(REC))) && groove_source_type(track) == GROOVE_DX7) {
        writing = rec_before_press;
        edit_down = 0;
        page = NAME; name_cursor = 0;
        goto controls_done;
    }
    if (pressed & (BUTTON(HOME) | BUTTON(SEQ) | BUTTON(FX) | BUTTON(SCL) |
                   BUTTON(EDIT) | BUTTON(ENV) | BUTTON(LFO))) {
        if (pressed & BUTTON(HOME)) {
            /* Acts on release (short) or after 600 ms (hold); see home_down. */
        } else if (pressed & BUTTON(SEQ)) {
            /* SEQ rework C1: SEQ tapped on the grid opens page 1 STEP (on release, so a
             * hold still means CHAIN); SEQ anywhere else, sub-pages too, returns to the grid. */
            seq_tap = page == STEPS && !seq_page;
            seq_page = 0;
            page = STEPS; step_follow = 1; cache->shown_page = ~0U;
        } else if (pressed & BUTTON(FX)) {
            /* Acts on release (tap: track sends) or after 600 ms (hold: master FX). */
        } else if (pressed & BUTTON(SCL)) {
            /* SEL: the track's operations menu (copy/paste/clear, undo...). */
            if (page == ACTIONS) page = music_page;
            else {
                if (!release_keys()) return 0;
                blocked_keys |= controls->keys;
                page = ACTIONS; action_confirm = 0; edit_down = seq_down = 0;
            }
        } else if (pressed & BUTTON(EDIT)) { /* user 12.11: on the SEQ grid and pages too EDIT = the track's sound, as from the mixer */
            unsigned edit = groove_source_type(track) == GROOVE_DX7 ? FM_QUICK : TONE_EDIT;
            edit_down = 1; edit_long = 0; edit_ms = now;
            page = page == FM_QUICK || page == FM_EDIT || page == edit ? music_page : edit;
            advanced_edit = 0;
            if (page == TONE_EDIT) tone_pages[track][tone_slot()] = 0;
            if (page == TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
        } else if (pressed & BUTTON(ENV)) {
            /* DX7 and (user 13.11) 303 have an ENV page; elsewhere ENV is inactive. */
            if (groove_source_type(track) == GROOVE_DX7 || groove_source_type(track) == GROOVE_303) open_subpage(FM_ENV);
        } else { /* v13 LFO: the first press opens LFO 1, the second LFO 2, the third leaves */
            if (page != LFOS) { lfo_sel = 0; page = LFOS; }
            else if (!lfo_sel) lfo_sel = 1;
            else { lfo_sel = 0; page = music_page == LFOS ? TRACKS : music_page; }
            cache->shown_page = ~0U;
        }
        goto controls_done;
    }
    if (edit_down && !edit_long && now - edit_ms >= 600U &&
        !(controls->buttons & (BUTTON(SEQ) | BUTTON(REC) | BUTTON(HOME)))) {
        edit_long = advanced_edit = 1;
        page = groove_source_type(track) == GROOVE_DX7 ? FM_EDIT : TONE_EDIT;
        goto controls_done;
    }
    if (pressed & BUTTON(ARP)) {
        /* v13: one press from any page opens ARP and turns it ON; the next press opens the randomisation
         * page, the third leaves (like LFO). User 10.11 (F1): an arp that was off starts with LATCH OFF,
         * one already on keeps its LATCH; leaving the page with LATCH off stops it (groove_screen_control). */
        if (!release_keys()) return 0;
        blocked_keys |= controls->keys;
        /* User 11.11: holding ARP switches the track's arp on / off; the hold undoes this press's page step. */
        arp_down = 1; arp_long = 0; arp_ms = now;
        arp_hold_page = page; arp_hold_sub = arp_page; arp_hold_track = track; arp_hold_was_on = arp_enabled(track) != 0;
        if (page == ARP_EDIT) {
            if (!arp_page) arp_page = 1; /* the second press: the randomisation page; the third leaves, ARP stays on */
            else { arp_page = 0; page = music_page; cache->shown_page = ~0U; }
        } else {
            arp_page = 0; page = ARP_EDIT; cache->shown_page = ~0U;
            groove_arp_set(track, 0); /* the engine arpeggiator stays off; ours runs instead */
            unsigned was_on = arp_enabled(track) != 0;
            if (!was_on) arp_set(track, 1);
            keys_latch = arp_press_latch(was_on, keys_latch);
            if (!keys_latch) latch_held = 0;
            arp_edit(track, ARP_HOLD, keys_latch ? 1 : -1); /* HOLD follows LATCH, as on the LATCH knob */
        }
        goto controls_done;
    }
    if (arp_down && !arp_long && now - arp_ms >= 600U && track == arp_hold_track &&
        !(controls->buttons & (BUTTON(SEQ) | BUTTON(REC) | BUTTON(HOME)))) {
        /* Hold ARP (as EDIT / SEQ: 600 ms, the same hold bar): back to the page before the press, the arp
         * switched to the opposite of its state before the press. On: LATCH by F1 (an arp switched on starts
         * with LATCH off, HOLD follows), already so if the press switched it on. Off: LATCH stays. */
        arp_long = 1;
        if (page != arp_hold_page) {
            back_going = 1; /* not a new level: drop the level the press pushed */
            if (back_n && back_stack[back_n - 1] == arp_hold_page) --back_n;
        }
        page = arp_hold_page; arp_page = arp_hold_sub; cache->shown_page = ~0U;
        if (!arp_hold_toggle(arp_hold_was_on)) arp_set(track, 0);
        else if (!arp_enabled(track)) {
            groove_arp_set(track, 0);
            arp_set(track, 1);
            keys_latch = arp_press_latch(0, keys_latch);
            latch_held = 0;
            arp_edit(track, ARP_HOLD, -1);
        }
        goto controls_done;
    }
    if (pressed & BUTTON(REC)) {
        writing ^= 1U;
        /* v8: no mode to choose: REC while playing records live, while stopped step by step. */
        if (writing && (page == BLE || page == PROJECT || page == PRESET_BROWSER ||
                       page == NAME || (page >= SOURCE && page <= RHYTHM) ||
                       (page == STEPS && (seq_page == 1 || seq_page == 3)))) {
            page = STEPS; seq_page = 0; step_follow = 1;
        }
        goto controls_done;
    }
    if (page != ACTIONS && !(page == STEPS && seq_key_steps()))
        octave = clamp(octave + !!(pressed & BUTTON(OCT_UP)) - !!(pressed & BUTTON(OCT_DOWN)), -2, 2);
    if ((pressed & BUTTON(SAVE)) && eng_warn && page == TRACKS) { /* ENGINES dialog: SAVE = OK */
        eng_confirm = 1; engine_settle(now, 1); eng_confirm = 0;
        goto controls_done;
    }
    if ((pressed & BUTTON(SAVE)) && page != PROJECT && page != BLE &&
        page != SOURCE && page != ACTIONS) {
        project_return = page; page = PROJECT; project_result = 0;
        project_reload = project_operation = 0;
        project_slot = groove_project_slot(); project_factory = name_cursor = 0;
        project_refresh();
        goto controls_done;
    }
    if (pressed & BUTTON(GLO)) { /* GLO: global settings */
        page = page == GLOBAL ? (music_page == GLOBAL ? TRACKS : music_page) : GLOBAL;
        cache->shown_page = ~0U;
        goto controls_done;
    }
    if (page == TRACKS && (controls->encoders[6] || controls->encoders[1])) {
        /* v7: PRESETS = engine, ALGORITHM = sound, in place; autosave later. */
        /* The pattern warning: SAVE = OK, HOME = CANCEL; ALGORITHM waits for the answer. */
        if (eng_warn && !controls->encoders[6]) goto controls_done;
        if (controls->encoders[6]) {
            blocked_keys |= controls->keys;
            if (!engines_shown()) {
                /* User 10.11: the first detent, either way, only opens ENGINES with the cursor on the
                 * track's own engine; nothing changes until the knob is turned again. */
                cache->shown_page = ~0U;
                engines_until = 1;
            } else {
                /* User 10.11: a turn during the warning closes it and walks on past that engine (it was
                 * not built), so an engine that would clear the pattern can be passed by. */
                eng_warn = 0;
                if (eng_pending != 0xff && eng_track != track) engine_settle(now, 1);
                unsigned cur = eng_pending != 0xff ? eng_pending : micro_d_track(track) ? (unsigned)ENGINE_MICRO : groove_source_type(track);
                unsigned next = eng_step(cur, controls->encoders[6]); /* v13: tile order, both ways; two MICRO D tracks at most */
                if (eng_pending == 0xff || eng_track == track) { eng_pending = (uint8_t)next; eng_track = (uint8_t)track; eng_at = now; }
            }
        }
        if (controls->encoders[1]) {
            engine_settle(now, 1); /* the sound belongs to the chosen engine */
            if (page != TRACKS || eng_warn) goto controls_done; /* BINAURAL opened, or the dialog came up */
            if (micro_d_track(track)) {
                int n = (int)micro_d_preset_count();
                micro_d_preset_load(track, (unsigned)(((int)micro_d_preset(track) + controls->encoders[1]) % n + n) % (unsigned)n);
            } else if (sounds_step(track, controls->encoders[1], now)) groove_control_edit(track, controls->encoders[1], 0, 0);
            autosave_at = now + 3000U; if (!autosave_at) autosave_at = 1;
        }
        goto controls_done;
    }
    if (page == SOURCE) {
        int delta = controls->encoders[0] + controls->encoders[1] + controls->encoders[3] + controls->encoders[6];
        source_preview = (unsigned)((int)source_preview + delta % GROOVE_SOURCES + GROOVE_SOURCES) % GROOVE_SOURCES;
        if (pressed & BUTTON(SAVE)) {
            source_result = groove_source_set(track, source_preview); var_split_clear(track);
            if (!source_result) sounds_forget(track);
            if (!source_result) {
                memset(tone_pages[track], 0, sizeof tone_pages[track]);
                drum_parts[track] = drum = 0;
                selected_note = groove_pad_count(track) ? groove_pad_note(track, 0) : 60;
                page = music_page;
            }
        }
        goto controls_done;
    }
    if (page == ACTIONS) {
        /* Keep the selection on the v7 grid (FILL is stored as SEL_FILL). */
        if (action >= 11 && action <= 13) action = SEL_FILL;
        else {
            unsigned found = 0;
            for (unsigned r = 0; r < SEL_ROWS; ++r) for (unsigned c = 0; c < 4; ++c) found |= sel_grid[r][c] == action;
            if (!found) action = sel_grid[0][0];
        }
        int move = controls->encoders[6];
        /* v7: turning a knob right lights its action (only one at a time),
         * left puts it out; SAVE then does the lit action. */
        for (unsigned i = 0; i < 4; ++i) {
            if (!controls->encoders[2 + i] || sel_grid[sel_row()][i] == 255) continue;
            if (controls->encoders[2 + i] > 0) { sel_armed = (int)i; action = sel_grid[sel_row()][i]; }
            else if (sel_armed == (int)i) sel_armed = -1;
        }
        int paging = !!(pressed & BUTTON(OCT_UP)) - !!(pressed & BUTTON(OCT_DOWN));
        if (move || paging) {
            cursor = (unsigned)clamp((int)cursor + move + paging * 16, 0,
                                     (int)groove_sequence_length(track) - 1);
            step_follow = action_confirm = 0;
        }
        for (unsigned i = 0; i < 16; ++i)
            if ((key_down & ~blocked_keys) & (1U << white_keys[i])) {
                cursor = (cursor & ~15U) + i;
                step_follow = action_confirm = 0;
            }
        if (controls->encoders[0]) { /* SELECT: row; keep the column where it exists */
            unsigned r = (unsigned)(((int)sel_row() + controls->encoders[0] % SEL_ROWS + SEL_ROWS) % SEL_ROWS), c = sel_col(); /* the drum turns round */
            action = sel_grid[r][c] != 255 ? sel_grid[r][c] : sel_grid[r][0];
            action_confirm = 0; sel_armed = -1;
        }
        if (pressed & BUTTON(SAVE)) {
            int r = 0;
            if (sel_armed < 0) { notice = "TURN A KNOB RIGHT"; notice_until = now + 2000U; goto controls_done; }
            if (((action >= 7 && action <= 13) || action == 16) && !action_confirm) { action_confirm = 1; goto controls_done; } /* v13: REPLACE DATA? first, SAVE again = OK, HOME = cancel */
            sel_armed = -1;
            action_confirm = 1;
            if (action == SEL_FILL) { /* fill = repeat the page up to the track length */
                unsigned length = groove_sequence_length(track);
                if (length <= 16) { notice = "16 STEPS: NO FILL"; notice_until = now + 2000U; goto controls_done; }
                action = length == 32 ? 11U : length == 48 ? 12U : 13U;
            }
            unsigned operation = action;
            unsigned destructive = (action >= 7 && action <= 13) || action == 16;
            if (destructive && !action_confirm) { action_confirm = 1; goto controls_done; }
            action_confirm = 0;
            if (action == 0) { page = STEPS; seq_page = 1; step_follow = 0; cache->shown_page = ~0U; }
            else if (action == 1) { page = STEPS; seq_page = 0; writing = 1; step_follow = 1; cache->shown_page = ~0U; }
            else if (action <= 4) {
                r = groove_edit_copy(track, cursor, action - 2);
                if (!r) action += 3; /* Select the matching paste command. */
            }
            else if (action <= 7) {
                unsigned wanted = action == 5 ? 1U : action == 6 ? 16U : 64U;
                r = groove_edit_clipboard_count() == wanted ? groove_edit_paste(track, cursor, action - 5) : -3;
            }
            else if (action <= 10) r = groove_edit_clear(track, cursor, action - 8);
            else if (action <= 13) r = groove_edit_repeat(track, (action - 9) * 16);
            else if (action == 14) r = groove_edit_undo();
            else if (action == 15) { r = groove_timbre_export(track, (uint8_t *)&tone_clip, sizeof tone_clip); tone_clip_ready = !r; }
            else if (action == 16) r = tone_clip_ready ? groove_timbre_import(track, (uint8_t *)&tone_clip, sizeof tone_clip) : -1;
            else if (action == 17) {
                if (groove_source_type(track) == GROOVE_DX7) { page = NAME; name_cursor = 0; }
                else r = -1;
            } else if (action == 18) { page = BLE; ble_view = ble_forget = ble_action = 0; ble_result = 0; }
            else if (action == 21) { /* V14 setting: track role (the SEQ page) */
                page = STEPS; seq_page = SEQ_ROLE; knob_enter(); step_follow = 0; cache->shown_page = ~0U;
            } else {
                if (!post(0xff, 0, 0, 1)) return 0;
                memset(key_note, 0xff, sizeof key_note); project_keys = 0; writing = 0; page = OVERVIEW;
            }
            notice = r == -3 ? "COPY SAME SIZE FIRST"
                : r == -2 ? "CHORD/LOCK FULL" : r ? "NOT POSSIBLE HERE"
                : operation == 2 ? "STEP COPIED"
                : operation == 3 ? "PAGE COPIED"
                : operation == 4 ? "TRACK COPIED"
                : operation == 5 ? "STEP PASTED"
                : operation == 6 ? "PAGE PASTED" : "DONE";
            notice_until = now + 2000U;
            cache->middle_cache = ~0U;
        }
        goto controls_done;
    }
    if (page == PROJECT) {
        seq_slots_valid = 0; /* re-read the SEQ squares after storing */
        if (old_page == PROJECT) {
            /* v11: SELECT (or PRESETS) slot, ALGORITHM cursor, KNOB 1 digits,
             * KNOB 2 A-M, KNOB 3 N-Z, KNOB 4 action; the SAVE button does it. */
            int slot_move = controls->encoders[0] + controls->encoders[6];
            if (slot_move) {
                project_slot = (unsigned)clamp((int)project_slot + slot_move, 0, GROOVE_PROJECT_SLOTS - 1);
                project_reload = project_operation = 0;
                project_refresh();
            }
            name_cursor = (unsigned)clamp((int)name_cursor + controls->encoders[1], 0, 11);
            if (controls->encoders[5]) { project_action = controls->encoders[5] > 0; project_reload = project_operation = 0; }
            if ((pressed & BUTTON(SAVE)) && project_action) { /* LOAD: the second SAVE confirms */
                if (!(seq_slots_used >> project_slot & 1U)) { notice = "EMPTY SLOT"; notice_until = now + 2000U; }
                else if (project_reload != 3) project_reload = 3;
                else {
                    struct groove_view lv = {0}; groove_get_view(track, cursor, &lv);
                    project_reload = 0; preset_slot = project_slot; preset_bank = 0;
                    (void)preset_load(lv.playing, now);
                }
                goto controls_done;
            }
            for (unsigned g = 0; g < 3; ++g)
                if (controls->encoders[2 + g] && !project_factory) {
                    char name[13];
                    groove_project_name(name);
                    groove_project_rename(name_cursor, group_character(name[name_cursor], g, controls->encoders[2 + g]));
                    project_reload = 0;
                }
            if (pressed & BUTTON(SAVE)) {
                unsigned overwrite = project_slot != groove_project_slot() && !project_stored_result;
                project_reload = overwrite && !project_reload ? 2 : 0;
                project_result = 0;
                if (!project_reload) {
                    project_operation = 3;
                    draw(ble);
                    paint_next = 180U / PAINT_ROWS;
                    paint_flush();
                    project_result = groove_project_save_slot(project_slot);
                    slot_forget(project_slot); /* even a failed save may have changed it */
                    project_operation = 1;
                    project_refresh();
                }
            }
        }
        if (!project_result && !project_reload && project_operation == 1) {
            page = project_return;
            if (page == PRESET_BROWSER) preset_refresh();
            notice = "PROJECT SAVED";
            notice_until = now + 2000U;
            project_operation = 0;
        }
    } else if (page == NAME) {
        name_cursor = (unsigned)clamp((int)name_cursor + controls->encoders[0], 0, 9);
        for (unsigned g = 0; g < 3; ++g) /* as on SAVE: K1 0-9 (and space), K2 A-M, K3 N-Z act on the character under the cursor */
            if (controls->encoders[2 + g]) {
                uint8_t patch[156];
                if (!groove_fm_copy(track, patch, NULL))
                    groove_fm_name_edit(track, name_cursor,
                        group_character((char)patch[145 + name_cursor], g, controls->encoders[2 + g]));
            }
    } else if (page == BLE) {
        if (encoder_active(controls->encoders)) ble_forget = 0;
        if (!ble_view) ble_action = (unsigned)clamp((int)ble_action + controls->encoders[6], 0, 1);
        if (!ble_view) {
            unsigned flags = groove_settings_radio_flags(), role = groove_settings_role_value();
            for (unsigned i = 0; i < 2; ++i) {
                int delta = controls->encoders[2 + i];
                if (delta > 0) flags |= 1U << i;
                else if (delta < 0) flags &= ~(1U << i);
            }
            if (controls->encoders[4]) {
                role = (unsigned)clamp((int)role + controls->encoders[4],
                                       FM1_BLE_PERIPHERAL, FM1_BLE_CENTRAL);
            }
            if (flags != groove_settings_radio_flags() || role != groove_settings_role_value())
                ble_result = groove_settings_radio(flags, role);
            if (controls->encoders[5])
                groove_settings_clock(controls->encoders[5] > 0 ? GROOVE_CLOCK_MIDI : GROOVE_CLOCK_INTERNAL);
            if (pressed & BUTTON(SAVE)) {
                if (ble_action) {
                    if (ble_forget != 1) ble_forget = 1;
                    else { ble_result = fm1_ble_forget_bonds(); ble_forget = ble_result ? 0 : 2; }
                } else if ((flags & (GROOVE_RADIO_ON | GROOVE_RADIO_MIDI)) ==
                    (GROOVE_RADIO_ON | GROOVE_RADIO_MIDI) && role == FM1_BLE_CENTRAL) {
                    ble_view = 1; ble_action = ble->count ? 0 : 1; ble_forget = 0;
                    if (ble->state == FM1_BLE_IDLE) ble_result = fm1_ble_scan();
                } else ble_result = -1;
            }
        } else {
            unsigned count = ble->count > FM1_BLE_MAX_DEVICES ? FM1_BLE_MAX_DEVICES : ble->count;
            ble_device = (unsigned)clamp((int)ble_device + controls->encoders[0], 0,
                                         count ? (int)count - 1 : 0);
            /* the actions sit on the knobs: KNOB 1 CONNECT, 2 SCAN, 3 DISCONNECT (turned right) */
            int act = -1;
            for (unsigned i = 0; i < 3; ++i) if (controls->encoders[2 + i] > 0) act = (int)i;
            if (act >= 0) {
                ble_action = (unsigned)act;
                if (!ble_action) ble_result = fm1_ble_connect(ble_device);
                else if (ble_action == 1) {
                    ble_result = fm1_ble_scan();
                    if (!ble_result) ble_device = 0;
                } else if (ble_action == 2) ble_result = fm1_ble_disconnect();
                else { ble_action = ble_forget = 0; ble_result = 0; }
            }
        }
    } else {
        if (page == STEPS && !seq_page) {
            /* v8: SELECT moves along the row, ALGORITHM between rows (clockwise =
             * down); KNOB1 velocity, KNOB2 gate (SAMPLER: the pad's length),
             * KNOB3 probability, KNOB4 note. PRESETS sets the length
             * (16/32/48/64); EDIT opens the track's sound (user 12.11), SEQ page 1. */
            struct groove_view sv = {0};
            groove_get_view(track, cursor, &sv);
            int cols = controls->encoders[0];
            int rows = controls->encoders[1];
            /* User 09.11: moving the cursor keeps REC on; the step-record cursor follows it. */
            if (rows || cols) {
                move_step(&sv, cols, rows);
                if (writing) groove_edit_seek(track, cursor);
                groove_get_view(track, cursor, &sv);
            }
            /* PRESETS = this track's length (16/32/48/64). REC records live while
             * playing and step by step while stopped (no separate mode knob). */
            sequence_length_adjust(controls->encoders[6]);
            /* The engine is chosen only on the mixer (it resets the track sound). */
            if (controls->encoders[2] && groove_step_setting_edit(track, cursor, 0, controls->encoders[2])) {
                notice = "EMPTY STEP"; notice_until = now + 2000U;
            }
            if (controls->encoders[4]) { /* PROB in eighths: 0 = always (shown 100%), else n/8; clockwise = more often */
                int v = (int)groove_step_setting(track, cursor, 2), c = v ? v : 8;
                int nc = clamp(c + (controls->encoders[4] > 0 ? 1 : -1), 1, 8), nv = nc == 8 ? 0 : nc;
                if (nv != v && groove_step_setting_edit(track, cursor, 2, nv - v)) { notice = "EMPTY STEP"; notice_until = now + 2000U; }
            }
            if (controls->encoders[3] && groove_source_type(track) == GROOVE_SAMPLE) {
                /* SAMPLER: KNOB 3 = this pad's LENGTH on the chosen step (parameter lock) */
                if (!(groove_drum_steps(track, drum) >> cursor & 1U)) { notice = "EMPTY STEP"; notice_until = now + 2000U; }
                else {
                    int r = step_length_edit(cursor, fm_encoder_delta(3, controls->encoders[3], 128));
                    if (r == -1) { notice = "ALL 16 LOCKS IN USE"; notice_until = now + 2000U; }
                    else if (r == -2) { notice = "STEP LOCKS ANOTHER PARAM"; notice_until = now + 2000U; }
                }
            }
            if (controls->encoders[3] && !groove_pad_count(track) && sv.step.velocity)
                seq_length_turn(controls->encoders[3], now); /* LENGTH: gate 1/16 .. full, then tied tail steps (C2) */
            if (controls->encoders[5] && groove_pad_count(track)) {
                drum = (unsigned)clamp((int)drum + controls->encoders[5], 0, (int)groove_pad_count(track) - 1);
                selected_note = groove_pad_note(track, drum);
            } else if (controls->encoders[5]) {
                if (sv.step.velocity) {
                    cut_tail_before_note(track, cursor);
                    groove_sequence_edit(track, cursor, (unsigned)clamp((int)sv.step.note + controls->encoders[5], 0, 127),
                                         sv.step.velocity);
                    groove_get_view(track, cursor, &sv);
                    selected_note = sv.step.note;
                } else selected_note = (unsigned)clamp((int)selected_note + controls->encoders[5], 0, 127);
            }
            goto controls_done;
        }
        if (page != BLE) {
            /* v8: the track changes only in the mixer; elsewhere SELECT moves
             * along the page (step, pad, effect) or turns its pages. */
            unsigned tracks = (page == TRACKS && !engines_shown()) || page == OVERVIEW; /* v13: SELECT does nothing on ENGINES */
            select_track(tracks ? controls->encoders[0] : 0);
            {
                if (page == OVERVIEW && controls->encoders[1]) {
                    /* ALGORITHM picks the engine; PRESETS picks the sound. Finish old
                     * note-offs before replacing the instance; held keys wait for release. */
                    blocked_keys |= controls->keys;
                    if (!release_keys())
                        return 0;
                    int type = (int)groove_source_type(track);
                    type = (type + controls->encoders[1] % GROOVE_SOURCES + GROOVE_SOURCES) %
                           GROOVE_SOURCES;
                    source_preview = (unsigned)type;
                    source_result = 0;
                    page = SOURCE;
                    cache->shown_page = ~0U;
                    operator_held = operator_long = 0;
                    goto controls_done;
                }
                int oct_delta = (int)((pressed >> OCT_UP) & 1U) - (int)((pressed >> OCT_DOWN) & 1U);
                if (page == STEPS) {
                    unsigned next = seq_page;
                    struct groove_view v = {0}; groove_get_view(track, cursor, &v);
                    /* User 09.11: on RHYTHM/REC and TRACK ROLE the 16 keys are one row of the grid; ALGORITHM picks
                     * the step, PRESETS (or OCT -/+) the row of 16 (1-16, 17-32 ...) when the track is longer. On STEP
                     * the keys play (OCT = octave) and write the chord into the step. SELECT turns the pages:
                     * STEP / RHYTHM / ROLE (round 4: no grid in the row, SELECT left on STEP stops; EDIT = the track's sound, back here),
                     * in the MIDI submenu CLOCK / MIDI OUT / MIDI SYNC; ALGORITHM does nothing there. */
                    int move = !seq_page ? controls->encoders[0] : seq_midi() ? 0 : controls->encoders[1];
                    int rows = !seq_page ? oct_delta : seq_midi() ? 0 : controls->encoders[6] + (seq_key_steps() ? oct_delta : 0);
                    if (seq_page && controls->encoders[0]) next = seq_nav_select(seq_page, controls->encoders[0]);
                    if (next != seq_page || move || rows) {
                        move_step(&v, move, rows);
                        if (move) writing = 0;
                    }
                    if (next != seq_page) { seq_page = next; knob_enter(); cache->shown_page = ~0U; }
                    if (seq_page == SEQ_RHYTHM) { /* TRANSPOSE shows the offset since page 2 was opened (or the track changed) */
                        static uint32_t seen;
                        if (track != transpose_track || now - seen > 500U) { transpose_shown = 0; transpose_track = track; }
                        seen = now;
                    }
                    if (seq_page == SEQ_STEP) step_adjust(0, controls->encoders, now);
                    else if (seq_page == SEQ_RHYTHM) { /* page 2 VARIATION: K1 MUTATE, K2 HUMAN, K3 TRANSPOSE (C2), K4 CHANCE */
                        for (unsigned i = 0; i < 4; ++i)
                            if (controls->encoders[2 + i] && seq_nav_var_knob(i) != SEQ_KNOB_NONE) var_edit(track, seq_nav_var_knob(i), controls->encoders[2 + i]);
                        if (controls->encoders[4]) transpose_turn(controls->encoders[4], now);
                    } else if (seq_page == SEQ_MIDI_CLOCK) { /* MIDI > CLOCK: K1 INT / MIDI, K2 CLK IN (round 4) */
                        if (controls->encoders[2]) groove_settings_clock(controls->encoders[2] > 0
                            ? GROOVE_CLOCK_MIDI : GROOVE_CLOCK_INTERNAL);
                        groove_settings_midi(track,6,controls->encoders[3]);
                    } else if (seq_page == SEQ_ROLE) { /* page 3 TRACK: K1 ROLE, K2 IN CH, K3 FOLLOW (C2, MICRO D only) */
                        if (controls->encoders[4] && follow_usable(track)) var_follow_set(track, controls->encoders[4] > 0);
                        if (controls->encoders[2] || controls->encoders[3]) { blocked_keys |= controls->keys; if (!release_keys()) return 0; }
                        for (unsigned i=0;i<2;++i) if (controls->encoders[2+i]) {
                            int r=groove_settings_track(track,i,controls->encoders[2+i]);
                            notice=r ? "TRACK SETTING FAILED" : "TRACK SET, NOTES OFF";
                            notice_until=now+1500U;
                        }
                    } else if (seq_page == SEQ_MIDI_OUT || seq_page == SEQ_MIDI_SYNC) {
                        static const uint8_t parameters[2][4] = {{0,1,2,4},{3,5,7,8}};
                        for (unsigned i = 0; i < 4; ++i) if (controls->encoders[2+i]) {
                            groove_settings_midi(track,parameters[seq_page-5][i],controls->encoders[2+i]);
                            knob_touch(i);
                            notice = "MIDI SET, NOTES OFF"; notice_until = now + 1500U;
                        }
                    }
                }
                if (page == ARP_EDIT) {
                    /* v13 page 1: K1 rate, K2 gate, K3 LATCH, K4 ARP on / off; page 2: K1 MUTATE, K2 SWING, K3 VAR,
                     * K4 CHANCE. PRESETS = mode, ALGORITHM = octaves on both. */
                    if (!arp_page) {
                        arp_edit(track, ARP_RATE, fm_encoder_delta(2, controls->encoders[2], 0U));
                        arp_edit(track, ARP_GATE, fm_encoder_delta(3, controls->encoders[3], 0U));
                        if (controls->encoders[5]) {
                            unsigned want = controls->encoders[5] > 0;
                            groove_arp_set(track, 0);
                            if (want != (arp_enabled(track) != 0)) arp_set(track, want);
                            notice = NULL;
                        }
                        if (controls->encoders[4]) { /* LATCH: the former GLOBAL KEYS LATCH; HOLD of this arpeggiator follows it */
                            keys_latch = controls->encoders[4] > 0;
                            if (!keys_latch) latch_held = 0;
                            arp_edit(track, ARP_HOLD, keys_latch ? 1 : -1);
                        }
                    } else {
                        arp_edit(track, ARP_MUTATE, fm_encoder_delta(2, controls->encoders[2], 101U));
                        groove_sequence_setting_edit(0, controls->encoders[3]); /* SWING: the stored setting GLOBAL had */
                        arp_edit(track, ARP_VAR, controls->encoders[4]);
                        arp_edit(track, ARP_CHANCE, controls->encoders[5]);
                    }
                    arp_edit(track, ARP_MODE, controls->encoders[6]);
                    arp_edit(track, ARP_OCT, controls->encoders[1]);
                } else if (page == NAME) {
                    name_cursor = (unsigned)clamp((int)name_cursor + controls->encoders[0], 0, 9);
                    for (unsigned g = 0; g < 3; ++g)
                        if (controls->encoders[2 + g]) {
                            uint8_t name_patch[156];
                            if (!groove_fm_copy(track, name_patch, NULL))
                                groove_fm_name_edit(track, name_cursor,
                                    group_character((char)name_patch[145 + name_cursor], g, controls->encoders[2 + g]));
                        }
                } else if (page == FM_QUICK) {
                    /* SELECT only picks the TONE / ENV window and never edits a value: while it turns (and for 150 ms
                     * after) the knobs are ignored, so a stray detent on KNOB 1 cannot ride along with it. */
                    static uint32_t quick_select_at;
                    static unsigned quick_select_seen;
                    if (controls->encoders[0]) { quick_select_at = now; quick_select_seen = 1; }
                    if (quick_select_seen && now - quick_select_at < 150U) {
                        /* ignore the knobs */
                    } else if (quick_row) {
                        for (unsigned i = 0; i < 4; ++i)
                            if (controls->encoders[2 + i]) groove_adsr_edit(track, i, fm_encoder_delta(2 + i, controls->encoders[2 + i], 101));
                    } else {
                        static const uint8_t parameters[] = {134, 135, 144};
                        dx7_algorithm_edit(track, controls->encoders[2]);
                        for (unsigned i = 1; i < 3; ++i) groove_fm_edit(track, parameters[i], controls->encoders[2 + i]);
                        vol_edit(track, fm_encoder_delta(5, controls->encoders[5], 128));
                    }
                } else if (page == EFFECTS) {
                    unsigned next = (unsigned)clamp((int)effects_page + controls->encoders[0], 0, 1); /* SELECT turns the two tabs */
                    if (next != effects_page) { effects_page = next; cache->shown_page = ~0U; }
                    if (effects_page) {
                        for (unsigned i = 0; i < 4; ++i)
                            groove_effects_edit(i, fm_encoder_delta(2 + i, controls->encoders[2 + i], effects_range(i)));
                    } else {
                        for (unsigned i = 0; i < 2; ++i)
                            groove_sends_edit(track, i, controls->encoders[2 + i]);
                        vol_edit(track, fm_encoder_delta(4, controls->encoders[4], 128));
                        groove_effects_edit(3, fm_encoder_delta(5, controls->encoders[5], 128));
                    }
                } else if (page == FM_LFO && groove_source_type(track) != GROOVE_DX7) {
                    for (unsigned i = 0; i < 4; ++i)
                        groove_lfo_edit(track, i, fm_encoder_delta(2 + i, controls->encoders[2 + i], i < 2 ? 101U : 0U));
                } else if (page == TONE_EDIT && micro_d_track(track)) {
                    /* MICRO D: SELECT or ALGORITHM turn the five pages round; KNOB 1-4 the values. */
                    int turn = controls->encoders[0] + controls->encoders[1];
                    unsigned slot = tone_slot();
                    if (turn) {
                        tone_pages[track][slot] = (uint8_t)(((int)(tone_pages[track][slot] % MD_PAGES) + (turn > 0 ? 1 : -1) + MD_PAGES) % MD_PAGES);
                        knob_enter();
                    }
                    unsigned pg = tone_pages[track][slot] % MD_PAGES;
                    for (unsigned col = 0; col < 4; ++col)
                        if (controls->encoders[2 + col]) {
                            unsigned parameter = pg * 4U + col;
                            knob_touch(col);
                            micro_d_edit(track, parameter, fm_encoder_delta(2 + col, controls->encoders[2 + col], micro_d_max(parameter) + 1U));
                            autosave_at = now + 3000U; if (!autosave_at) autosave_at = 1;
                        }
                } else if (page == TONE_EDIT || (page == FM_ENV && groove_source_type(track) != GROOVE_DX7)) {
                    if (page == TONE_EDIT && groove_source_type(track) == GROOVE_303)
                        for (unsigned i = 8; i < 10; ++i)
                            if ((key_down & ~blocked_keys) & (1U << edit_keys[i]))
                                groove_mono_set(track, 1); /* 303 = MONO always (user 10.11): POLY does nothing */
                    /* v7: on pad engines (808, SAMPLER) SELECT picks the pad/drum and
                     * ALGORITHM turns the pages; elsewhere SELECT turns the pages. */
                    unsigned pad_engine = groove_pad_count(track) != 0;
                    int part_turn = pad_engine ? controls->encoders[0] : controls->encoders[6];
                    int page_turn = pad_engine ? controls->encoders[1] : controls->encoders[0];
                    if (groove_pad_count(track) != 0) {
                        drum_parts[track] = (uint8_t)clamp(
                            (int)drum_parts[track] + part_turn, 0,
                            groove_source_type(track) == GROOVE_808 ? GROOVE_DRUMS - (page == FM_ENV)
                                                                    : groove_pad_count(track) - 1);
                        for (unsigned key = 0; key < 27; ++key)
                            if ((key_down & ~blocked_keys) & (1U << key)) {
                                int pad =
                                    groove_pad_index(track, groove_key_note(track, key, octave));
                                if (pad >= 0)
                                    drum_parts[track] = (uint8_t)pad;
                            }
                        if (drum_parts[track] < groove_pad_count(track)) {
                            drum = drum_parts[track];
                            selected_note = groove_pad_note(track, drum);
                        }
                    }
                    unsigned slot = tone_slot(), count = groove_tone_count(track, tone_part());
                    int direction = page == TONE_EDIT ? (page_turn > 0) - (page_turn < 0) : 0;
                    tone_pages[track][slot] = (uint8_t)clamp(
                        (int)tone_pages[track][slot] + direction, 0, (int)((count - 1) / 4));
                    if (part_turn || direction) knob_enter(); /* new part / page: start from KNOB 1 */
                    for (unsigned col = 0; col < 4; ++col)
                        if (controls->encoders[2 + col]) {
                            knob_touch(col);
                            unsigned parameter = tone_parameter(col);
                            const fm1_param_t *p =
                                groove_tone_parameter(track, tone_part(), parameter);
                            if (p)
                                groove_tone_edit(
                                    track, tone_part(), parameter,
                                    fm_encoder_delta(2 + col, controls->encoders[2 + col], p->names ? 0U : p->max + 1U));
                        }
                } else if (page >= FM_EDIT && page <= FM_LFO) {
                    if (page == FM_EDIT) {
                        for (unsigned i = 0; i < 10; ++i) {
                            uint32_t bit = 1U << edit_keys[i];
                            if ((key_down & ~blocked_keys) & bit) {
                                if (i < 8)
                                    operator_selected[track] = (uint8_t)i;
                                else
                                    groove_mono_set(track, i == 8);
                                if (i < 6) {
                                    operator_time[i] = now;
                                    operator_held |= 1U << i;
                                    operator_long &= ~(1U << i);
                                }
                            }
                            if (i < 6) {
                                if (!(controls->keys & bit) || (blocked_keys & bit))
                                    operator_held &= ~(1U << i);
                                if ((operator_held & ~operator_long & (1U << i)) &&
                                    now - operator_time[i] >= 500U) {
                                    groove_operator_toggle(track, i);
                                    operator_long |= 1U << i;
                                }
                            }
                        }
                        op = operator_selected[track];
                        unsigned limit = op < 6 ? 5 : op == 6 ? 1 : 0;
                        int direction = (controls->encoders[0] > 0) - (controls->encoders[0] < 0);
                        operator_page[track][op] = (uint8_t)clamp(
                            (int)operator_page[track][op] + direction, 0, (int)limit);
                        fm_group = op < 6    ? operator_page[track][op]
                                   : op == 6 ? 6 + operator_page[track][op]
                                             : 8;
                    } else if (page == FM_LFO) {
                        lfo_page[track] =
                            (uint8_t)clamp((int)lfo_page[track] + controls->encoders[0], 0, 1);
                        fm_group = 9 + lfo_page[track];
                    }
                    for (unsigned col = 0; col < 4; ++col)
                        if (controls->encoders[2 + col]) {
                            if (page == FM_ENV)
                                groove_adsr_edit(track, col,
                                                 fm_encoder_delta(2 + col, controls->encoders[2 + col], 101));
                            else
                                groove_fm_edit(track, fm_parameter(col),
                                               fm_encoder_delta(2 + col, controls->encoders[2 + col], fm_range(fm_parameter(col))));
                        }
                } else if (page == TRACKS) {
                    /* v13 order: KNOB1 volume; KNOB2 brightness/filter; KNOB3 smooth pan L-R (the FX send lives on
                     * TRACK FX); KNOB4 the ON > SOLO > MUTE ring; ALGORITHM is the tempo. */
                    if (controls->encoders[2]) vol_edit(track, fm_encoder_delta(2, controls->encoders[2], 128));
                    if (controls->encoders[3]) track_filter_edit(track, fm_encoder_delta(3, controls->encoders[3], 128));
                    if (controls->encoders[4]) pan_edit(track, fm_encoder_delta(4, controls->encoders[4], 128));
                    if (controls->encoders[5]) mixer_state_cycle(track, controls->encoders[5]);
                } else if (page != STEPS || !seq_page) {
                    if (page == OVERVIEW) {
                        /* Same knobs as the mixer: KNOB3 FX send, KNOB4 ON/MUTE/SOLO. */
                        if (controls->encoders[4]) {
                            groove_sends_edit(track, 0, controls->encoders[4]);
                            groove_sends_edit(track, 1, controls->encoders[4]);
                        }
                    } else if (groove_pad_count(track) != 0) {
                        drum = (unsigned)clamp((int)drum + controls->encoders[4], 0,
                                               (int)groove_pad_count(track) - 1);
                        selected_note = groove_pad_note(track, drum);
                    } else if (page == STEPS) {
                        struct groove_view selected = {0};
                        groove_get_view(track, cursor, &selected);
                        if (controls->encoders[4] && step_follow && selected.playing) {
                            cursor = selected.play_step;
                            groove_get_view(track, cursor, &selected);
                        }
                        /* STEP selects the existing cell; NOTE edits its pitch
                         * without replacing velocity, chord, gate or locks.
                         * On an empty cell it sets the next inserted note. */
                        if (selected.step.velocity) selected_note = selected.step.note;
                        if (controls->encoders[4]) {
                            step_follow = 0;
                            selected_note = (unsigned)clamp((int)selected_note + controls->encoders[4], 0, 127);
                            if (selected.step.velocity) {
                                cut_tail_before_note(track, cursor);
                                groove_sequence_edit(track, cursor, selected_note, selected.step.velocity);
                                groove_get_view(track, cursor, &selected);
                                selected_note = selected.step.note; /* Respect chord pitch limits. */
                            }
                        }
                    }
                    if (page == OVERVIEW) {
                        /* KNOB 1 is navigation: left goes back to the track list. */
                        if (controls->encoders[2] < 0) { page = TRACKS; cache->shown_page = ~0U; }
                        if (controls->encoders[3])
                            vol_edit(track, fm_encoder_delta(3, controls->encoders[3], 128));
                    } else if (controls->encoders[2])
                        vol_edit(track, fm_encoder_delta(2, controls->encoders[2], 128));
                    if (page == OVERVIEW && controls->encoders[5]) mix_cycle(track, controls->encoders[5]);
                    else if (controls->encoders[5]) {
                        groove_tempo_edit(fm_encoder_delta(5, controls->encoders[5], 271));
                        if (groove_clock_mode() == GROOVE_CLOCK_MIDI) {
                            notice = "BPM SET BY EXT CLOCK";
                            notice_until = now + 2000U;
                        }
                    }
                }
                if (page == FM_ENV && groove_source_type(track) == GROOVE_DX7 && controls->encoders[6])
                    groove_adsr_enable(track, controls->encoders[6] > 0);
                /* v7: sounds and engines change only on the mixer; ALGORITHM does
                 * nothing in the sound editors (it duplicated knobs there). */
            }
        }
    }
controls_done:
    engine_settle(now, 0);
    if (sounds_tick(now)) { autosave_at = now + 3000U; if (!autosave_at) autosave_at = 1; }
    /* Autosave writes Flash, which briefly silences audio: only at rest (20 s
     * without controls or MIDI notes, transport stopped and quiet). */
    if (autosave_at && (int32_t)(now - autosave_at) >= 0 && groove_storage_rest()) {
        autosave_at = 0;
        unsigned slot = groove_project_slot();
        if (slot < GROOVE_PROJECT_SLOTS && groove_project_save_slot(slot) == 0) seq_slots_valid = 0;
    }
    if (page != old_page) {
        if (back_going) back_going = 0;
        else if (page == TRACKS) back_n = 0;
        else if (old_page != STARTUP && old_page != SOURCE) {
            if (back_n == BACK_DEPTH) { memmove(back_stack, back_stack + 1, BACK_DEPTH - 1); --back_n; }
            back_stack[back_n++] = (uint8_t)old_page;
        }
        action_confirm = ble_forget = project_reload = 0;
        if (!(pressed & BUTTON(EDIT))) edit_down = 0;
        if (page != STEPS) seq_down = 0;
    }
    if (page != OVERVIEW) rec_down = 0;
    if (page != old_page && page != ARP_EDIT && !(pressed & BUTTON(ARP))) arp_down = arp_long = 0; /* the ARP press itself may leave the page */
    /* Manual selection must not be overwritten by the recording cursor on
     * the next poll. Entering sequence editing finishes the recording take. */
    if (page == ACTIONS || page == STEP_EDIT || (page == STEPS && (seq_page == 1 || seq_page == 3))) writing = 0;
    {   /* playing in this tick: a PLAY press below starts or stops the sequencer before any arp note of this tick */
        struct groove_view pv = {0};
        groove_get_view(track, cursor, &pv);
        edit_record(writing, pv.playing != ((pressed & BUTTON(PLAY)) && !play_handled));
    }
    if ((pressed & BUTTON(PLAY)) && !play_handled) {
        struct groove_view view = {0};
        groove_get_view(track, cursor, &view);
        uint8_t command = view.playing ? 0xfc : (writing && groove_sequence_setting(2) && groove_sequence_rewound() ? 0xfa : 0xfb);
        if (!post(command, 0, 0, 1)) return 0;
        play_down = 1; play_ms = now;
    }
    /* Hold PLAY 500 ms: restart from the first step (Start, 0xfa). */
    if (play_down) {
        if (!(controls->buttons & BUTTON(PLAY))) play_down = 0;
        else if (now - play_ms >= 500U) {
            play_down = 0;
            /* the playhead starts where the edit cursor is: put it on step 1 of the pattern, whatever line is shown */
            cursor = 0; step_follow = 1; groove_edit_seek(track, 0);
            if (!post(0xfa, 0, 0, 1)) return 0;
        }
    }
    unsigned mode = navigation_mode();
    if (page != FM_EDIT)
        operator_held = operator_long = 0;
    if (mode != old_mode || page != old_page) {
        blocked_keys |= old_keys;
        if (!release_keys())
            return 0;
    }
    if (retry_reset) {
        blocked_keys |= controls->keys;
        if (!post(0xff, 0, 0, 1))
            return 0;
        retry_reset = 0;
        memset(key_note, 0xff, sizeof key_note); project_keys = 0;
    }
    uint32_t keys = controls->keys & ~blocked_keys;
    /* EDIT's labelled black keys select OP1..6 / Pitch / Global / Mono / Poly;
     * they must never also generate keyboard MIDI, including on page changes. */
    if (page == FM_EDIT)
        for (unsigned i = 0; i < 10; ++i)
            keys &= ~(1U << edit_keys[i]);
    if (page == TONE_EDIT && groove_source_type(track) == GROOVE_303)
        keys &= ~((1U << edit_keys[8]) | (1U << edit_keys[9]));
    if (page == STEPS && !writing) {
        struct groove_view view = {0};
        groove_get_view(track, cursor, &view);
        unsigned first = step_first(&view);
        if (!seq_page) {
            /* Grid: a key writes its note into the selected step; the same
             * note again clears it. Drum keys toggle their pad on that step.
             * User 11.11: with our ARP on (melodic track) nothing is written: the keys only feed the arp. */
            for (unsigned key = 0; key < 27 && arp_grid_writes(arp_enabled(track) != 0, groove_pad_count(track)); ++key) {
                if (!((key_down & keys) & (1U << key))) continue;
                unsigned note = groove_key_note(track, key, octave);
                if (note > 127) continue;
                struct groove_view cell = {0};
                groove_get_view(track, cursor, &cell);
                if (groove_pad_count(track)) {
                    unsigned c = cursor; toggle_step(c); /* current layer, whatever key */
                } else {
                    break_long_note(track, cursor); /* a note on a long note's tail ends it here */
                    if (cell.step.velocity && cell.step.note == note) groove_sequence_edit(track, cursor, note, 0);
                    else {
                        groove_sequence_edit(track, cursor, note, cell.step.velocity ? cell.step.velocity : 100);
                        selected_note = note;
                    }
                }
                step_follow = 0;
            }
        } else if (seq_page == SEQ_STEP) {
            /* User 09.11 (round 4): the keys sound and write the chord they hold into the step (melodic tracks);
             * a key added while others are held grows the chord, a new hand (all keys up) starts a new one. */
            static uint32_t hand;
            static unsigned hand_step = ~0U, hand_track = ~0U;
            if (!(keys & 0x7ffffffU) || hand_step != cursor || hand_track != track) { hand = 0; hand_step = cursor; hand_track = track; }
            uint32_t added = key_down & keys & 0x7ffffffU;
            if (added && !groove_pad_count(track) && groove_track_role(track) == GROOVE_TRACK_NORMAL) {
                hand |= added;
                uint8_t notes[8];
                unsigned n = 0;
                for (unsigned key = 0; key < 27 && n <= 8; ++key) {
                    unsigned note = groove_key_note(track, key, octave), i, dup = 0;
                    if (!(hand & (1U << key)) || note > 127) continue;
                    for (i = 0; i < n && i < 8; ++i) dup |= notes[i] == note;
                    if (dup) continue;
                    if (n == 8) { n = 9; break; }
                    for (i = n; i && notes[i - 1] > note; --i) notes[i] = notes[i - 1];
                    notes[i] = (uint8_t)note; ++n;
                }
                if (n > 8) { notice = "8 NOTES AT MOST"; notice_until = now + 2000U; }
                else if (n) {
                    struct groove_view cell = {0};
                    groove_get_view(track, cursor, &cell);
                    break_long_note(track, cursor); /* a chord on a long note's tail ends it here */
                    int r = groove_sequence_chord(track, cursor, notes, n, cell.step.velocity ? cell.step.velocity : 100);
                    if (r == -2) { notice = "CHORD/LOCK FULL"; notice_until = now + 2000U; }
                    else if (!r) { selected_note = notes[0]; key_chord_seen(notes, n); }
                }
                step_follow = 0;
            }
        } else if (seq_key_steps()) {
            for (unsigned i = 0; i < 16; ++i)
                if ((key_down & keys) & (1U << white_keys[i])) toggle_step(first + i);
        }
    }
    unsigned layer_keys = page == STEPS && !seq_page && groove_pad_count(track) != 0;
    if (keys_latch && !layer_keys && !mode) {
        if (latch_track != track) { latch_held = 0; latch_track = track; }
        if ((keys & ~latch_phys) && !latch_phys) latch_held = 0; /* a new chord */
        latch_phys = keys;
        latch_held |= keys;
        keys = latch_held;
    } else latch_phys = 0;
    for (unsigned key = 0; key < 27; ++key) {
        unsigned note = layer_keys ? groove_pad_note(track, drum) : groove_key_note(track, key, octave);
        if (key_note[key] != 0xff &&
            (mode || !(keys & (1U << key)) ||
             (key_note[key] != note && !(project_keys & (1U << key))) || key_track[key] != track)) {
            if (!key_arp[key] && !post((uint8_t)(0x80 + groove_midi_input_channel(key_track[key])), key_note[key], 0, 3))
                return 0;
            if (!key_arp[key]) long_note_end(key_track[key], now);
            key_note[key] = 0xff; key_arp[key] = 0;
            project_keys &= ~(1U << key);
        }
        if (!mode && note < 128 && (keys & (1U << key)) && key_note[key] == 0xff) {
            /* With our arpeggiator on, held keys feed it instead of sounding. */
            key_arp[key] = arp_enabled(track) && !groove_pad_count(track);
            key_seq[key] = (uint8_t)(++key_counter);
            if (!key_arp[key] && writing && !groove_pad_count(track) && groove_track_role(track) == GROOVE_TRACK_NORMAL) {
                /* REC (live or step by step): the step the instrument is about to write may lie inside a long note's tail */
                unsigned at = groove_sequence_record_position(track);
                if (at == 255U) at = cursor; /* stopped: the recording cursor */
                if (at < GROOVE_STEPS) break_long_note(track, at); /* 254 = count-in: nothing is written */
            }
            if (!key_arp[key] && !post((uint8_t)(0x90 + groove_midi_input_channel(track)), (uint8_t)note, 100, 3))
                return 0;
            /* LOCAL OFF (GLOBAL > MIDI SYNC, K1) sends the keys and the ARP to MIDI only: say why they are silent. */
            if (!groove_midi_setting(track, 3) && !notice) { notice = "LOCAL OFF: MIDI SYNC K1"; notice_until = now + 2500U; }
            if (!key_arp[key] && writing && frame_views[track].playing && !groove_pad_count(track)) long_note_start(track, now);
            key_note[key] = (uint8_t)note;
            key_track[key] = (uint8_t)track;
            selected_note = note;
            if (groove_pad_count(track) != 0)
                drum = (unsigned)groove_pad_index(track, note);

        }
    }
    if (page != TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
    long_note_hold();
    {
        /* LFOs, arpeggiator and sequence chain run here, every control tick. */
        static uint32_t last_tick;
        unsigned dt = last_tick ? now - last_tick : 0;
        last_tick = now;
        struct groove_view tv = {0};
        groove_get_view(0, 0, &tv);
        struct lfo_cc cc[8];
        unsigned n = lfo_tick(dt > 200U ? 200U : dt, tv.tempo, tv.playing, !writing, cc, 8);
        for (unsigned t = 0; t < 8 && pan_dirty; ++t)
            if (pan_dirty & (1U << t)) {
                if (!post((uint8_t)(0xb0 | groove_midi_input_channel(t)), 10, pan_value[t], 3)) break;
                micro_d_pan(t, pan_value[t]); /* MICRO D is ours: it pans its own output */
                pan_dirty &= (uint8_t)~(1U << t);
            }
        for (unsigned i = 0; i < n; ++i) (void)post((uint8_t)(0xb0 | cc[i].channel), cc[i].cc, cc[i].value, 3);
        struct arp_event ev[6];
        if (arp_track_last < 8 && arp_track_last != track) {
            unsigned m = arp_stop(arp_track_last, groove_midi_input_channel(arp_track_last), ev, 6);
            for (unsigned i = 0; i < m; ++i) (void)post(ev[i].status, ev[i].note, ev[i].velocity, 3);
        }
        arp_track_last = track;
        uint8_t held[27]; unsigned hn = 0;
        for (unsigned pass = 0; pass < 27 && hn < 27; ++pass) { /* press order */
            unsigned best = 27; unsigned best_seq = 0xffffffffU;
            for (unsigned k = 0; k < 27; ++k)
                if (key_arp[k] && key_note[k] != 0xff && key_track[k] == track) {
                    unsigned sq = (unsigned)(uint8_t)(key_seq[k] - (uint8_t)key_counter - 1U);
                    unsigned used = 0;
                    for (unsigned j = 0; j < hn; ++j) used |= held[j] == key_note[k];
                    if (!used && sq < best_seq) { best_seq = sq; best = k; }
                }
            if (best == 27) break;
            held[hn++] = key_note[best];
        }
        arp_swing_set(groove_sequence_setting(0)); /* the SWING knob on ARP page 2 edits this stored value */
        arp_keep_note(writing != 0);
        if (groove_settings_clock_pending()) {
            /* CLOCK knob turned (all sites end in groove_settings_clock): the transport is being stopped and the
             * mode changes on a later poll. Until then our arps end their sounding note and play nothing, else
             * a latched arp would post new notes into the switch. They go on by themselves afterwards.
             * Only this track's arp can sound (the one of the track left before was stopped above). */
            unsigned m = arp_stop(track, groove_midi_input_channel(track), ev, 6);
            for (unsigned i = 0; i < m; ++i) (void)post(ev[i].status, ev[i].note, ev[i].velocity, 3);
        } else {
            unsigned m = arp_tick(track, groove_midi_input_channel(track), now, tv.tempo, tv.playing, held, hn, ev, 6);
            for (unsigned i = 0; i < m; ++i) (void)post(ev[i].status, ev[i].note, ev[i].velocity, 3);
        }
        var_tick(now, var_post);
        follow_update(); /* C2 FOLLOW: the held key's shift to MICRO D tracks that follow */
        /* Chain: track 1's loop plays its repeats, then the next sequence. That
         * one is read into RAM during the loop (no Flash at the switch). On the
         * last repeat it is loaded on the control tick nearest to the loop's end,
         * timed from the start of the step before the last one (swing moves only
         * odd steps); with MIDI clock, or if that tick was missed, at the wrap. */
        if (chain_on && tv.playing) {
            unsigned stepnow = tv.play_step, len = tv.sequence_length ? tv.sequence_length : 64U;
            unsigned next = chain_pos;
            for (unsigned i = 1; i <= 16; ++i) { unsigned c = (chain_pos + i) % 16U; if (chain_slot[c]) { next = c; break; } }
            unsigned moves = chain_slot[next] && next != chain_pos, last = chain_loops >= chain_rep[chain_pos];
            if (moves && !preset_current_bank && chain_ahead != chain_slot[next]) {
                chain_ahead = chain_slot[next]; /* one read per pattern, even if it fails */
                (void)groove_project_preload(chain_slot[next] - 1U);
            }
            if (stepnow != chain_prev_step && stepnow == len - 2U) chain_mark = now;
            unsigned step_ms = tv.tempo ? 15000U / tv.tempo : 0U, wrapped = stepnow < chain_prev_step;
            unsigned early = last && step_ms && tv.clock_mode != GROOVE_CLOCK_MIDI && stepnow >= len - 2U &&
                             now - chain_mark <= 3U * step_ms && (int32_t)(now + dt - (chain_mark + 2U * step_ms)) >= 0;
            if (wrapped && !last) ++chain_loops;
            else if (moves && (early || wrapped)) {
                unsigned keep = page, keep_music = music_page;
                chain_pos = (uint8_t)next;
                preset_slot = chain_slot[next] - 1U; preset_bank = preset_current_bank;
                preset_load(1, now); /* also zeroes chain_loops */
                preset_refresh();
                page = keep; music_page = keep_music; cache->shown_page = ~0U;
                stepnow = 0;
            } else if (wrapped) chain_loops = 0;
            chain_prev_step = (uint8_t)stepnow;
        } else {
            chain_prev_step = 0;
            if (!chain_on && chain_ahead) { chain_ahead = 0; groove_project_preload_drop(); }
        }
    }
    return 1;
}

void groove_screen_control(void)
{
    struct fm1_controls controls;
    fm1_controls_read(&controls);
    uint32_t now = fm1_millis(), key_down = controls.keys & ~previous_keys;
    if (boot_guard_settling() && !boot_guard_safe()) { /* OCT- during the first 1.5 s = SAFE mode */
        boot_guard_keys(controls.buttons);
        if (boot_guard_safe()) { page = SAFE; boot_active = 0; cache->shown_page = ~0U; }
    }
    unsigned master_changed = groove_master_update(controls.knob); /* v11: no MASTER text over the page */
    {   /* Black box: page, last control and audio load, kept in RAM for the start after a crash. */
        static uint32_t bb_at;
        uint32_t fresh = controls.buttons & ~previous_buttons;
        for (unsigned i = 0; i < 7; ++i) if (controls.encoders[i]) bb_control(i, now);
        if (fresh) bb_control(16U + (unsigned)__builtin_ctz(fresh), now);
        if (now - bb_at >= BB_UPDATE_MS) {
            bb_at = now;
            struct fm1_audio_stats bb_dma;
            fm1_audio_stats_read(&bb_dma);
            bb_poll(now, page, audio_load(0), bb_dma.overruns, fm1_output_fx_shed_count(),
                    (boot_guard_safe() ? BB_FLAG_SAFE : 0U) | (frame_views[track].playing ? BB_FLAG_PLAYING : 0U));
        }
    }
    if (now - health_at >= 500U) {
        struct fm1_engine_stats health;
        fm1_engine_stats_read(&health);
        struct fm1_render_stats audio;
        groove_audio_stats(&audio);
        struct fm1_audio_stats dma;
        fm1_audio_stats_read(&dma);
        unsigned adapted = audio.adapted;
        /* v11: technical states (overload, MIDI overflow, voices cut) are
         * not shown over the page; GLOBAL's service line counts them. */
        health_output = groove_midi_dropped();
        health_adapted = adapted;
        health_dma = dma.overruns;
        health_at = now; health_underruns = health.underruns; health_dropped = health.midi_dropped;
    }
    unsigned input_changed = master_changed || controls.buttons != previous_buttons || controls.keys != previous_keys ||
                             encoder_active(controls.encoders);
    {   /* Autosave rest: anything held or moved, or a MIDI note on any track (keys and external).
         * Not the MASTER pot: its ADC noise could keep the synth from ever being at rest. */
        static unsigned midi_notes_seen;
        unsigned midi_notes = 0;
        for (unsigned t = 0; t < 8; ++t) midi_notes += groove_midi_activity(t);
        if (controls.keys || controls.buttons || encoder_active(controls.encoders) ||
            midi_notes != midi_notes_seen)
            groove_storage_touch();
        midi_notes_seen = midi_notes;
    }
    /* The logo never blocks control/audio or consumes the first gesture.
     * Ignore the initial MASTER ADC pickup when deciding whether to skip. */
    { /* boot chime once, its first note with the first splash wave (boot_note_ms[0]), never in SAFE */
        static uint8_t chime_done;
        if (!chime_done && page != SAFE && !boot_guard_safe() && now - boot_started >= boot_note_ms[0]) { chime_done = 1; mfx_chime_start(); }
    }
    if (boot_active && (now - boot_started >= BOOT_HOLD_MS || controls.buttons || controls.keys ||
        encoder_active(controls.encoders))) { /* not the MASTER pot: its ADC noise skipped the logo */
        boot_active = 0;
        input_changed = 1;
    }
    struct fm1_ble_status ble = {0};
    fm1_ble_get_status(&ble);
    if (page == SAFE) {
        safe_control(&controls, now);
        previous_buttons = controls.buttons;
        previous_keys = controls.keys;
        if (now - frame_ms >= 100U || input_changed || saved_cache.shown_page == ~0U) {
            frame_ms = now;
            draw(&ble);
        }
        paint_flush();
        return;
    }
    unsigned stage = ble.state | (ble.pending << 8);
    if (stage != ble_stage) { ble_stage = stage; ble_stage_since = now; }
    ble_seconds = (now - ble_stage_since) / 1000U;
    if (ble_seconds > 999) ble_seconds = 999;
    unsigned linked = ble.state == FM1_BLE_CONNECTED || ble.state == FM1_BLE_READY ||
                      ble.state == FM1_BLE_DISCOVERING;
    if (ble_was_linked && !linked)
        retry_reset = 1;
    ble_was_linked = linked;
    writing = groove_edit_recording() || (rec_paused && writing); /* paused by edit_record: REC stays on */
    if (!writing) rec_paused = 0;
    if (writing) cursor = groove_edit_cursor();
    unsigned error = groove_edit_error();
    if (error && !notice) { notice = error == 1 ? "REC STOP: MIDI FULL" : "CHORD LIMIT REACHED"; notice_until = now + 2000U; }
    static uint32_t fx_shed_seen; /* the audio load guard bypassed the master FX for ~1 s */
    uint32_t fx_shed = fm1_output_fx_shed_count();
    if (fx_shed != fx_shed_seen) { fx_shed_seen = fx_shed; if (!notice) { notice = "CPU FULL: MASTER FX OFF"; notice_until = now + 2000U; } }
    struct groove_view transport = {0};
    groove_get_view(track, cursor, &transport);
    cursor %= transport.sequence_length;
    if (transport.playing && !transport_seen && !(page == STEPS && seq_page) && page != STEP_EDIT && page != ACTIONS) step_follow = 1;
    if (!transport.playing && transport_seen && step_follow && !writing)
        cursor = transport.play_step;
    transport_seen = transport.playing;
    unsigned old_page = page;
    unsigned boundary =
        controls.buttons != previous_buttons || controls.encoders[0] || controls.encoders[6];
    if (page == FM_EDIT) {
        for (unsigned i = 0; i < 10; ++i)
            boundary |= (key_down & (1U << edit_keys[i])) != 0;
    } else if (page == TONE_EDIT && groove_source_type(track) == GROOVE_303) {
        boundary |= (key_down & ((1U << edit_keys[8]) | (1U << edit_keys[9]))) != 0;
    } else if (page == TRACKS || page == OVERVIEW || page == STEPS ||
               ((page == TONE_EDIT || page == FM_ENV) && groove_pad_count(track) != 0))
        boundary |= key_down != 0;
    if (boundary) {
        if ((rec_down && !(controls.buttons & BUTTON(REC)))) {
            /* The encoder count includes motion before this release. Route
             * that motion through the held layer before closing it. */
            struct fm1_controls prior = controls;
            prior.keys = previous_keys;
            prior.buttons = previous_buttons;
            for (unsigned i = 0; i < 7; ++i) {
                controls.encoders[i] = 0;
            }
            if (encoder_active(prior.encoders) && !apply_controls(&prior, now, &ble))
                return;
        }
    }
    read_encoders(&controls, now, boundary);
    { /* Battery: smooth the noisy ADC and change the shown percent at most
       * every 10 s, and only by 2 % or more (no shaking top bar, less redraw). */
        static uint32_t battery_avg, battery_at;
        if (!battery_avg) battery_avg = (uint32_t)controls.battery << 6;
        battery_avg += (uint32_t)(((int32_t)((uint32_t)controls.battery << 6) - (int32_t)battery_avg) / 64);
        if (!battery_at || now - battery_at >= 10000U) {
            battery_at = now ? now : 1U;
            unsigned adc = battery_avg >> 6;
            battery_adc = adc;
            unsigned pct = battery_percent();
            if (battery_shown > 100U || pct + 2U <= battery_shown || pct >= battery_shown + 2U || pct == 100U || pct == 0U) battery_shown = pct;
        }
    }
    if (!apply_controls(&controls, now, &ble))
        return;
    {   /* User 10.11 F1 leave rule removed 11.11 (the user: leaving ARP for SEQ must not stop it).
         * F2: when REC ends (any way: REC button, OVERVIEW tap, MMC / transfer stop, emergency stop) the arps
         * that were on during the take go off, with LATCH, so they do not play over what they recorded.
         * Checked here, after every path that changes the page or REC; an MMC stop is seen on the next tick
         * (writing follows groove_edit_recording above). The sounding note ends in the next arp_tick. */
        unsigned arp_mask = 0;
        for (unsigned t = 0; t < 8; ++t) arp_mask |= (arp_enabled(t) != 0) << t;
        unsigned arp_off = arp_rec_follow(writing != 0, arp_mask);
        if (arp_off) {
            for (unsigned t = 0; t < 8; ++t)
                if (arp_off & (1U << t)) { arp_set(t, 0); arp_edit(t, ARP_HOLD, -1); }
            keys_latch = 0; latch_held = 0;
        }
    }
    {   /* User 10.11 (stable sound): the 303 plays MONO always (voice_rules.h). frame_views is at most one frame
         * old; the copy is set at once so the engine call is not repeated until the next frame reads it. */
        uint8_t type[GROOVE_TRACKS], mono[GROOVE_TRACKS];
        for (unsigned t = 0; t < GROOVE_TRACKS; ++t) { type[t] = (uint8_t)groove_source_type(t); mono[t] = frame_views[t].mono; }
        unsigned poly = voice_303_poly_mask(type, mono, GROOVE_TRACKS);
        for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
            if (poly & (1U << t)) { groove_mono_set(t, 1); frame_views[t].mono = 1; }
    }
    /* Input changes do not wait for the animation timer. LCD work is bounded
     * separately and continues on later polls, even between animation frames. */
    if (now - frame_ms >= 33U || page != old_page || input_changed || saved_cache.shown_page == ~0U) {
        frame_ms = now;
        draw(&ble);
    }
    paint_flush();
}

void groove_screen_mmc(unsigned command)
{
    unsigned transport = command == 1 || command == 9 ? 0xfc :
                         command == 2 || command == 3 || command == 6 ? 0xfb : 0;
    if (!transport && command != 7) return;
    writing = command == 6;
    rec_down = 0;
    edit_record(writing, transport == 0xfb); /* command 6 starts the sequencer */
    if (transport) (void)post((uint8_t)transport, 0, 0, 1);
}
#ifdef FM1_HOST_TEST
int groove_screen_preset(unsigned slot)
{
    if (slot >= GROOVE_PROJECT_SLOTS) return GROOVE_PROJECT_INVALID;
    struct groove_view view = {0}; groove_get_view(track, cursor, &view);
    preset_slot = slot; preset_bank = 1; preset_refresh();
    return preset_load(view.playing, fm1_millis());
}
unsigned groove_screen_preset_position(void)
{
    unsigned current = preset_loaded && preset_project_revision == groove_project_revision();
    return preset_current_slot | (preset_current_bank << 8) | (current << 9);
}
/* Web shortcuts enter the same editor and use the same parameter handlers. */
int groove_screen_open(unsigned target)
{
    if (target > ARP_EDIT || target == PROJECT || target == PRESET_BROWSER || target == SOURCE || page == SAFE) return -1; /* no preset browser and no engine-select page (user 07.11) */
    boot_active = 0;
    unsigned quick = target == FM_QUICK;
    if (quick && groove_source_type(track) != GROOVE_DX7) target = TONE_EDIT;
    if ((target == FM_EDIT || target == NAME) &&
        groove_source_type(track) != GROOVE_DX7) {
        if (target == FM_EDIT) target = TONE_EDIT;
        else return -1;
    }
    if (!release_keys()) return -1;
    blocked_keys |= previous_keys;
    edit_down = seq_down = arp_down = arp_long = rec_down = 0;
    if (parameter_page(target)) {
        if (!parameter_page(page)) subpage_return = page;
    } else advanced_edit = !quick && (target == FM_EDIT || target == TONE_EDIT);
    page = target == OVERVIEW ? TRACKS : target;
    if (target == STEP_EDIT || target == RHYTHM) { page = STEPS; seq_page = target == STEP_EDIT ? 1 : 0; step_follow = 0; }
    else if (page == STEPS) seq_page = 0;
    if (page == ARP_EDIT) arp_page = 0;
    if (page == ACTIONS || (page == STEPS && seq_page == 1)) {
        writing = 0;
        edit_record(0, 0);
    }
    if (quick && page == TONE_EDIT) tone_pages[track][tone_slot()] = 0;
    if (page == FM_ENV && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
    action_confirm = name_cursor = 0;
    if (page == OVERVIEW || page == STEPS || page == TRACKS) music_page = page;
    if (page == SOURCE) { source_preview = groove_source_type(track); source_result = 0; }
    if (page == PRESET_BROWSER) { project_result = 0; preset_refresh(); }
    if (page == BLE) { ble_view = 1; ble_forget = ble_action = 0; ble_result = 0; } /* v11: only the device list; its settings are GLOBAL > BLUETOOTH */
    cache->shown_page = ~0U;
    return 0;
}
/* Browser hit targets use the same cursor, record and step-edit handlers. */
/* Browser-only targets describe the real display, not an independent copy of
 * the synthesizer. Stale messages are rejected after a control-context change. */
struct web_target { int32_t x, y, w, h, action, index, owner, raw; char label[16], value[16]; };
static struct web_target web_targets[32];
static unsigned web_target_count;
unsigned groove_screen_web_context(void)
{
    unsigned context[] = {page, track, groove_source_type(track), op, fm_group,
        tone_part(), tone_pages[track][tone_slot()], effects_page,
        ble_view, advanced_edit, 0U,
        (page == STEP_EDIT || (page == STEPS && seq_page)) ? cursor : 0, seq_page, arp_page};
    return signature(2166136261U, context, sizeof context);
}
unsigned groove_screen_web_mask(void)
{
    unsigned mask = 0;
    if (page == TRACKS || page == OVERVIEW || page == STEPS || page == FM_EDIT ||
        page == FM_ENV || page == FM_LFO || page == TONE_EDIT || page == EFFECTS ||
        page == STEP_EDIT || page == RHYTHM || page == FM_QUICK || page == ARP_EDIT) {
        for (unsigned i = 0; i < 4; ++i)
            if (web_knobs[i][0][0] && strcmp(web_knobs[i][0], "-") &&
                web_knobs[i][1][0] && strcmp(web_knobs[i][1], "--")) mask |= 1U << i;
    }
    if ((page == TRACKS || page == OVERVIEW || (page == STEPS && !seq_page)) && groove_clock_mode()) mask &= ~8U;
    if (page == STEPS && seq_page == 4 && groove_clock_mode()) mask &= ~2U;
    if (page == RHYTHM) mask &= 7U; /* Fixed 1/16 quantization is a readout. */
    return mask;
}
static void web_target(unsigned x, unsigned y, unsigned w, unsigned h, unsigned action,
                       unsigned index, unsigned owner, const char *label, int value, const char *display)
{
    if (web_target_count >= 32) return;
    struct web_target *t = &web_targets[web_target_count++];
    memset(t, 0, sizeof *t);
    t->x = x; t->y = y; t->w = w; t->h = h; t->action = action;
    t->index = index; t->owner = owner; t->raw = value;
    unsigned n = (unsigned)strlen(label); if (n > 15) n = 15;
    memcpy(t->label, label, n);
    if (display) { n = (unsigned)strlen(display); if (n > 15) n = 15; memcpy(t->value, display, n); }
    else decimal(t->value, value, 1);
}
static void web_alias(unsigned x, unsigned y, unsigned w, unsigned h, unsigned col)
{
    if (groove_screen_web_mask() & (1U << col))
        web_target(x, y, w, h, 0, col, track, web_knobs[col][0], 0, web_knobs[col][1]);
}
const void *groove_screen_web_targets(void)
{
    /* Browser navigation can change page before the next 33 ms paint tick.
     * Refresh the shared draw metadata before exposing its edit targets. */
    struct fm1_ble_status status = {0}; fm1_ble_get_status(&status); draw(&status);
    web_target_count = 0;
    struct groove_view v; groove_get_view(track, cursor, &v);
    unsigned type = groove_source_type(track);
    if (page != BLE && page != PROJECT && page != PRESET_BROWSER && !v.clock_mode)
        web_target(6, 4, 65, 23, 1, 0, track, "BPM", v.tempo, NULL);
    if (page == TRACKS) {
        for (unsigned t = 0; t < 8; ++t) {
            struct groove_view row; groove_get_view(t, cursor, &row);
            char label[16] = "T1 LEVEL"; label[1] += t;
            web_target(10 + (t & 1U) * 111 + 31, 70 + (t >> 1) * 30 + 19, 52, 7, 2, 0, t, label, row.volume, NULL);
        }
    } else if (page == STEPS) {
        web_target(180, 42, 56, 14, 8, 0, track, "LOOP LENGTH", v.next_length, NULL);
        web_target(64, 15, 110, 15, 6, 1, track, "SEQ PAGE", seq_page + 1, seq_pages[seq_page]);
    } else if (page == ARP_EDIT) {
        web_target(6, 176, 222, 16, 6, 1, track, "ARP PAGE", arp_page + 1, arp_page ? "RANDOM" : "NOTES");
    } else if (page == FM_EDIT || ((page == FM_ENV || page == FM_LFO) && type == GROOVE_DX7)) {
        uint8_t patch[156]; struct groove_fm_controls controls;
        groove_fm_copy(track, patch, &controls);
        web_target(6, 178, 47, 15, 3, 134, track, "CHOIR", patch[134] + 1, NULL);
        web_target(58, 178, 37, 15, 3, 135, track, "FEEDBACK", patch[135], NULL);
        web_target(101, 178, 56, 15, 4, 0, track, "VOICE MODE", controls.mono, controls.mono ? "MONO" : "POLY");
        if (page == FM_EDIT && op < 7)
            web_target(198, 31, 34, 17, 6, 0, track, "PARAMETER PAGE", operator_page[track][op] + 1, NULL);
        if (page == FM_ENV)
            web_target(6, 76, 65, 17, 5, 0, track, "ADSR", controls.enabled, controls.enabled ? "ON" : "OFF");
        if (page == FM_LFO) {
            static const char *const waves[] = {"TRI", "SAW DOWN", "SAW UP", "SQUARE", "SINE", "S/HOLD"};
            web_target(6, 106, 180, 22, 3, 142, track, "LFO WAVE", patch[142], waves[patch[142] % 6]);
            web_target(6, 142, 112, 17, 3, 141, track, "KEY SYNC", patch[141], patch[141] ? "ON" : "OFF");
            web_target(122, 142, 67, 17, 3, 143, track, "PM SENS", patch[143], NULL);
        }
    } else if (page == TONE_EDIT || (page == FM_ENV && type != GROOVE_DX7)) {
        if (!groove_pad_count(track))
            for (unsigned col = 0; col < 4; ++col) web_alias(6, 74 + col * 24, 228, 19, col);
        if (page == TONE_EDIT && advanced_edit && groove_tone_count(track, tone_part()) > 4)
            web_target(188, 33, 40, 17, 6, 0, track, "PARAMETER PAGE", tone_pages[track][tone_slot()] + 1, NULL);
    } else if (page == EFFECTS) {
        for (unsigned col = 0; col < 4; ++col) web_alias(6, 86 + col * 22, 204, 20, col);
        web_target(6, 59, 228, 18, 6, 6, track, "FX PAGE", effects_page, effects_page ? "SHARED" : "TRACK");
    }
    return web_targets;
}
unsigned groove_screen_web_target_count(void) { return web_target_count; }
int groove_screen_web_adjust(unsigned context, unsigned action, unsigned index, unsigned owner, int delta)
{
    if (context != groove_screen_web_context()) return 0;
    if (!delta || delta < -127 || delta > 127) return -1;
    unsigned valid = action == 0 && owner == track && index < 4 &&
        (groove_screen_web_mask() & (1U << index));
    groove_screen_web_targets();
    for (unsigned i = 0; i < web_target_count; ++i) {
        struct web_target *t = &web_targets[i];
        if (t->action == (int)action && t->index == (int)index && t->owner == (int)owner) valid = 1;
    }
    if (!valid) return -1;
    if (!action) return (int)index + 3; /* Encoder index + 1. */
    if (action == 6) return (int)index + 1;
    if (action == 1) groove_tempo_edit(delta);
    else if (action == 2) vol_edit(owner, delta);
    else if (action == 3) groove_fm_edit(owner, index, delta);
    else if (action == 4) groove_mono_set(owner, delta > 0);
    else if (action == 5) groove_adsr_enable(owner, delta > 0);
    else if (action == 7) {
        struct groove_view v; groove_get_view(owner, cursor, &v);
        if ((index ? v.solo : v.muted) != (unsigned)(delta > 0)) groove_mix_toggle(owner, index);
    }
    else if (action == 8) sequence_length_adjust(delta);
    return 0;
}
const char *groove_screen_web_knobs(void) { return &web_knobs[0][0][0]; }
unsigned groove_screen_web_first(void)
{
    struct groove_view v; groove_get_view(track, cursor, &v); return step_first(&v);
}
int groove_screen_web_step(unsigned step, unsigned toggle)
{
    struct groove_view v; groove_get_view(track, cursor, &v);
    if ((page != OVERVIEW && page != STEPS) || step >= v.sequence_length || toggle > 1) return -1;
    if (!release_keys()) return -1;
    blocked_keys |= previous_keys;
    writing = 0; step_follow = 0; cursor = step;
    if (page == OVERVIEW) seq_page = 0;
    page = music_page = STEPS;
    edit_record(0, 0);
    if (toggle && seq_page != 1 && seq_page != 3) toggle_step(cursor);
    cache->shown_page = ~0U;
    return 0;
}
/* Read-only panel state for the browser simulator. */
void groove_screen_snapshot(uint8_t state[8])
{
    state[0] = (uint8_t)track;
    state[1] = (uint8_t)page;
    state[2] = (uint8_t)cursor;
    state[3] = (uint8_t)drum;
    state[4] = (uint8_t)writing;
    state[5] = (uint8_t)op;
    state[6] = (uint8_t)fm_group;
    state[7] = (uint8_t)(octave + 24);
}
#endif

/* FM1 algorithm layout drawn with bounded RGB565 primitives. */
static const uint8_t algorithms[32][12] = {
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x01}, /* 1 */
    {0x20, 0x00, 0x21, 0x01, 0x30, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00}, /* 2 */
    {0x20, 0x00, 0x21, 0x00, 0x22, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x01}, /* 3 */
    {0x20, 0x00, 0x21, 0x00, 0x22, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x03}, /* 4 */
    {0x10, 0x00, 0x11, 0x00, 0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x01}, /* 5 */
    {0x10, 0x00, 0x11, 0x00, 0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x02}, /* 6 */
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10, 0x42, 0x01}, /* 7 */
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x01, 0x41, 0x10, 0x42, 0x00}, /* 8 */
    {0x20, 0x00, 0x21, 0x01, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10, 0x42, 0x00}, /* 9 */
    {0x20, 0x00, 0x21, 0x00, 0x22, 0x01, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10}, /* 10 */
    {0x20, 0x00, 0x21, 0x00, 0x22, 0x00, 0x30, 0x00, 0x31, 0x00, 0x41, 0x11}, /* 11 */
    {0x10, 0x00, 0x11, 0x01, 0x30, 0x00, 0x21, 0x20, 0x31, 0x00, 0x41, 0x10}, /* 12 */
    {0x10, 0x00, 0x11, 0x00, 0x30, 0x00, 0x21, 0x20, 0x31, 0x00, 0x41, 0x11}, /* 13 */
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x00, 0x42, 0x11}, /* 14 */
    {0x20, 0x00, 0x21, 0x01, 0x30, 0x00, 0x31, 0x00, 0x32, 0x00, 0x42, 0x10}, /* 15 */
    {0x20, 0x00, 0x11, 0x20, 0x21, 0x00, 0x22, 0x00, 0x31, 0x10, 0x32, 0x01}, /* 16 */
    {0x20, 0x00, 0x11, 0x21, 0x21, 0x00, 0x22, 0x00, 0x31, 0x10, 0x32, 0x00}, /* 17 */
    {0x20, 0x00, 0x11, 0x20, 0x21, 0x01, 0x31, 0x10, 0x32, 0x00, 0x33, 0x00}, /* 18 */
    {0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x20, 0x00, 0x30, 0x00, 0x21, 0x31}, /* 19 */
    {0x10, 0x00, 0x20, 0x00, 0x11, 0x31, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10}, /* 20 */
    {0x10, 0x00, 0x20, 0x00, 0x11, 0x31, 0x30, 0x00, 0x40, 0x00, 0x31, 0x30}, /* 21 */
    {0x10, 0x00, 0x11, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x21, 0x41}, /* 22 */
    {0x10, 0x00, 0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x40, 0x00, 0x31, 0x31}, /* 23 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x50, 0x00, 0x31, 0x41}, /* 24 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x50, 0x00, 0x41, 0x31}, /* 25 */
    {0x10, 0x00, 0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x41, 0x11}, /* 26 */
    {0x10, 0x00, 0x20, 0x00, 0x21, 0x01, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10}, /* 27 */
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x01, 0x40, 0x10}, /* 28 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x31, 0x00, 0x40, 0x00, 0x41, 0x01}, /* 29 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x01, 0x40, 0x00}, /* 30 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x50, 0x00, 0x51, 0x01}, /* 31 */
    {0x00, 0x00, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x50, 0x01}, /* 32 */
};
/* Operators off the bottom row are modulators: their output level is brightness. */
static unsigned dx7_modulators(const uint8_t patch[156])
{
    const uint8_t *layout = algorithms[patch[134] & 31U];
    unsigned mask = 0;
    for (unsigned i = 0; i < 6; ++i) if (layout[i * 2] & 15U) mask |= 1U << i;
    return mask;
}
static void dx7_brightness(unsigned t, int delta)
{
    uint8_t patch[156]; struct groove_fm_controls c;
    if (groove_fm_copy(t, patch, &c)) return;
    unsigned mods = dx7_modulators(patch);
    for (unsigned i = 0; i < 6; ++i) if (mods & (1U << i)) groove_fm_edit(t, (5 - i) * 21 + 16, delta);
}
/* Changing the algorithm keeps the sound musical: an operator that becomes a
 * carrier takes the old carriers' level (so it is heard, not a whisper), one
 * that becomes a modulator takes the old modulators' level (so it does not
 * turn into harsh noise). Each algorithm then sounds clearly different. */
static void dx7_algorithm_edit(unsigned t, int delta)
{
    uint8_t patch[156]; struct groove_fm_controls c;
    if (!delta || groove_fm_copy(t, patch, &c)) return;
    unsigned before = dx7_modulators(patch), carriers = 0, cs = 0, mods = 0, ms = 0;
    for (unsigned i = 0; i < 6; ++i) {
        unsigned level = patch[(5 - i) * 21 + 16];
        if (before & (1U << i)) { ms += level; ++mods; } else { cs += level; ++carriers; }
    }
    unsigned carrier_level = carriers ? cs / carriers : 90, mod_level = mods ? ms / mods : 70;
    if (carrier_level < 80) carrier_level = 80;
    if (mod_level > 85) mod_level = 85;
    groove_fm_edit(t, 134, delta);
    if (groove_fm_copy(t, patch, &c)) return;
    unsigned after = dx7_modulators(patch);
    for (unsigned i = 0; i < 6; ++i) {
        unsigned bit = 1U << i, at = (5 - i) * 21 + 16;
        if ((before ^ after) & bit)
            groove_fm_edit(t, at, (int)((after & bit) ? mod_level : carrier_level) - (int)patch[at]);
    }
}
static int dx7_brightness_value(const uint8_t patch[156])
{
    unsigned mods = dx7_modulators(patch), n = 0, sum = 0;
    for (unsigned i = 0; i < 6; ++i) if (mods & (1U << i)) { sum += patch[(5 - i) * 21 + 16]; ++n; }
    return n ? (int)(sum / n) : -1;
}
/* A wire 2 px thick (the drawing's 1.5 px line). */
static void algo_wire(int x0, int y0, int x1, int y1, uint16_t c)
{
    line(x0, y0, x1, y1, c);
    if (x0 == x1) line(x0 + 1, y0, x1 + 1, y1, c); else line(x0, y0 + 1, x1, y1 + 1, c);
}
static void algorithm_draw(unsigned algorithm, unsigned muted)
{
    /* v13: blocks as large as the stack allows, centred in the window (y 106..188); carriers have a green frame and
     * wire, modulators a turquoise one, feedback is pink, the chosen operator wears the glass cursor. */
    const uint8_t *layout = algorithms[algorithm & 31U];
    unsigned cmin = 15, cmax = 0, rmax = 0, left = 999, right = 0;
    for (unsigned i = 0; i < 6; ++i) {
        unsigned c = layout[i * 2] >> 4, r = layout[i * 2] & 15U;
        if (c < cmin) cmin = c;
        if (c > cmax) cmax = c;
        if (r > rmax) rmax = r;
    }
    const int nc = (int)(cmax - cmin) + 1, nr = (int)rmax + 1;
    const int colp = nc > 1 ? (190 / (nc - 1) > 60 ? 60 : 190 / (nc - 1)) : 36;
    const int bw = nc > 1 ? (colp - 10 > 44 ? 44 : colp - 10) : 44;
    const int pitch = 80 / nr > 42 ? 42 : 80 / nr;
    const int bh = pitch * 2 / 3 > 26 ? 26 : pitch * 2 / 3, bottom0 = 183;
    const uint16_t green = knob_colors[1], turq = knob_colors[3];
    for (unsigned i = 0; i < 6; ++i) {
        unsigned column = layout[i * 2] >> 4, row = layout[i * 2] & 15;
        int x = 120 + ((int)column * 2 - (int)(cmin + cmax)) * colp / 2, y = bottom0 - 9 - bh - (int)row * pitch;
        unsigned branch = layout[i * 2 + 1] >> 4, feedback = layout[i * 2 + 1] & 15;
        int bottom = row ? y + pitch : bottom0;
        const uint16_t wire = row ? turq : green;
        if (!row) { if (x < (int)left) left = (unsigned)x; if (x > (int)right) right = (unsigned)x; }
        if (branch == 0)
            algo_wire(x, y + bh, x, bottom, wire);
        else {
            algo_wire(x, y + bh, x, y + pitch - 4, wire);
            int first = branch == 1 ? x - colp : branch == 2 ? x + colp : x;
            int last = branch == 4 ? x + 2 * colp : branch >= 2 ? x + colp : first;
            algo_wire(first < x ? first : x, y + pitch - 4, last > x ? last : x, y + pitch - 4, wire);
            for (int dest = first; dest <= last; dest += colp)
                algo_wire(dest, y + pitch - 4, dest, bottom, wire);
        }
        if (row) { /* an arrowhead where each modulator wire enters its target */
            int first = branch == 1 ? x - colp : branch == 2 ? x + colp : x;
            int last = branch == 4 ? x + 2 * colp : branch >= 2 ? x + colp : first;
            if (!branch) first = last = x;
            for (int dest = first; dest <= last; dest += colp)
                fill_tri(dest * 10 - 35, (y + pitch) * 10 - 65, dest * 10 + 35, (y + pitch) * 10 - 65, dest * 10, (y + pitch) * 10 - 5, turq);
        }
        if (feedback) {
            int end_y = y + (int)(feedback - 1) * pitch + bh, fx = x + bw / 2 + 5;
            algo_wire(x, end_y, fx, end_y, knob_colors[0]);
            algo_wire(fx, end_y, fx, y - 5, knob_colors[0]);
            algo_wire(fx, y - 5, x, y - 5, knob_colors[0]);
            algo_wire(x, y - 5, x, y, knob_colors[0]);
            if (fx + 16 < 236) lcd_text(fx + 3, (unsigned)(y + bh / 2 - 7), "FB", &lcd12, knob_colors[0], MD_PANEL);
        }
    }
    algo_wire((int)left, bottom0, (int)right, bottom0, green);
    lcd_text((int)right + bw / 2 + 6, (unsigned)(bottom0 - 7), "OUT", &lcd12, green, MD_PANEL);
    for (unsigned i = 0; i < 6; ++i) {
        unsigned column = layout[i * 2] >> 4, row = layout[i * 2] & 15;
        int cx = 120 + ((int)column * 2 - (int)(cmin + cmax)) * colp / 2, y = bottom0 - 9 - bh - (int)row * pitch;
        unsigned off = (muted >> i) & 1U;
        rbox((unsigned)(cx - bw / 2), (unsigned)y, (unsigned)bw, (unsigned)bh, 4, MD_PANEL, off ? V_FAINT : row ? turq : green);
        char num[2] = {(char)('1' + i), 0};
        lcd_center(cx, (unsigned)(y + bh / 2 - 6), num, bh >= 20 ? &lcd17 : &lcd11, off ? V_DIM : TEXT, MD_PANEL);
        if (i == op) glass_cursor((unsigned)(cx - bw / 2), (unsigned)y, (unsigned)bw, (unsigned)bh, 11, MD_AMBER);
    }
}
#ifdef FM1_HOST_TEST
unsigned groove_screen_test_page(void) { return page; }
#endif

/* Pattern extras owned by the screen: CHAIN (16 + on) and library sounds (8). */
void groove_screen_extras_get(uint8_t out[26])
{
    memset(out, 0xff, 26);
    memcpy(out, chain_slot, 16); out[16] = chain_on;
    sounds_state_get(out + 17);
}
void groove_screen_extras_put(const uint8_t in[26])
{
    /* While a chain plays it loads the patterns itself; keep the running chain. */
    if (!chain_on) {
        for (unsigned i = 0; i < 16; ++i) chain_slot[i] = in[i] <= GROOVE_PROJECT_SLOTS ? in[i] : 0;
        chain_on = in[16] == 1; chain_pos = 0; chain_prev_step = 0;
    }
    sounds_state_put(in + 17);
}
/* Record version 2: CHAIN repeats (16), SEQ variations (32), 6 spare. */
void groove_screen_extras2_get(uint8_t out[54])
{
    memset(out, 0xff, 54);
    memcpy(out, chain_rep, 16);
    var_state_get(out + 16);
    for (unsigned t = 0; t < 8; ++t) out[48 + t / 4U] = (uint8_t)((out[48 + t / 4U] & ~(3U << (2U * (t % 4U)))) | (track_pan[t] << (2U * (t % 4U)))); /* 48..49: R / L */
}
void groove_screen_extras2_put(const uint8_t in[54])
{
    if (!chain_on) for (unsigned i = 0; i < 16; ++i) chain_rep[i] = in[i] <= 15 ? in[i] : 0;
    var_state_put(in + 16); /* 48..49 (pan side): groove_screen_pan_put */
}
void groove_screen_pan_get(uint8_t value[8]) { memcpy(value, pan_value, 8); }
void groove_screen_pan_put(const uint8_t side[2], const uint8_t value[8])
{
    for (unsigned t = 0; t < 8; ++t) {
        unsigned p = side[t / 4U] == 0xff ? 0U : (side[t / 4U] >> (2U * (t % 4U))) & 3U;
        unsigned v = value[t] <= 127 ? value[t] : p == 1 ? 127U : p == 2 ? 0U : 64U;
        /* A load resets the engines' pan: send CC 10 again where it is not centre. */
        if (v != pan_value[t] || v != 64) pan_dirty |= (uint8_t)(1U << t);
        pan_value[t] = (uint8_t)v;
        track_pan[t] = (uint8_t)(v > 64 ? 1 : v < 64 ? 2 : 0);
    }
}
#ifdef FM1_HOST_TEST
/* Tests: jump straight to the master effects page. */
void groove_screen_test_fx(unsigned row) { page = MASTER_FX; fx_row = row % FX_ROWS; cache->shown_page = ~0U; }
#endif
unsigned groove_screen_peak(void) { return audio_load(1); }
void groove_screen_last_peak_set(unsigned percent) { last_peak = percent; }
