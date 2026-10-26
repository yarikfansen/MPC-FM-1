/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "fm_patch.h"
#include "version.h"
#include "fm1_engine_control.h"
#include "master_fx.h"
#include "font_aa.h"
#include "lfo.h"
#include "arp.h"
#include "sounds.h"
#include "extras.h"
#include "boot_guard.h"
#include "variation.h"
#include "recorder.h"
#include <string.h>
enum {
    BACK = 0x0000,
    PANEL = 0x18e3,
    TEXT = 0xf77e,
    ACCENT = 0x07ff,
    DIM = 0x738e,
    GREEN = 0x07e0,
    AMBER = 0xfd20
};
/* Compose the final pixels before touching the LCD. Sixteen rows cost 7,680
 * bytes; unchanged bands and unchanged horizontal margins are not transferred.
 * A page change never exposes the temporary background clear on the panel. */
enum { PAINT_ROWS = 16, PAINT_BANDS = FM1_SCREEN_HEIGHT / PAINT_ROWS };
_Static_assert(FM1_SCREEN_WIDTH * PAINT_ROWS <= FM1_SCREEN_DMA_PIXELS, "LCD band capacity");
static uint16_t paint_pixels[FM1_SCREEN_WIDTH * PAINT_ROWS];
static uint8_t paint_left[PAINT_BANDS], paint_right[PAINT_BANDS];
static unsigned paint_rendering, paint_top, paint_next, paint_x, paint_end;
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
enum { OVERVIEW, STEPS, FM_EDIT, FM_ENV, FM_LFO, TONE_EDIT, MIX, BLE, PROJECT, NAME, EFFECTS, SOURCE, ACTIONS, STEP_EDIT, RHYTHM, FM_QUICK, PRESET_BROWSER, ARP_EDIT, STARTUP, TRACKS, MASTER_FX, SCOPE, GLOBAL, LFOS, CHAIN, VARS, SMP_REC, SMP_CHOP, SAFE };
enum { OCT_DOWN, OCT_UP, FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC };
#define BUTTON(b) (1U << (b))
static const uint16_t colors[8] = {0xfbf6, 0xfd87, 0xff09, 0x7f0b, 0x3eb8, 0x4d1f, 0xac5f, 0xfb7b}; /* v6 track colours */
/* Each physical knob keeps one colour on every page: pink, green, yellow, turquoise (v6). */
static const uint16_t knob_colors[4] = {0xfbf6, 0x3f0f, 0xfe87, 0x2ed9};
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
    {"COMMON", {"CHOIR", "FDBACK", "OSC SYNC", "TRANS"}, {134, 135, 136, 144}, 0},
    {"LFO", {"WAVE", "SPEED", "PMD", "AMD"}, {142, 137, 139, 140}, 0},
    {"LFO MOD", {"DELAY", "P SENS", "LFO SYNC", "OSC SYNC"}, {138, 143, 141, 136}, 0}};
static const char *const source_names[] = {"DX7 FM", "BASS 303", "DRUM 808", "PSG", "PLUCK", "SAMPLER"};
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
/* v7: recording mode picked with ALGORITHM on the SEQ grid (0 STEP, 1 LIVE);
 * REC only switches recording on/off in that mode. */
static unsigned rec_mode;
/* v7: the mixer changes engine / sound in place and saves the project by
 * itself 3 s after the last turn (one flash write, not one per detent). */
static uint32_t autosave_at;
static unsigned project_action;
static int sel_armed = -1; /* v7 SEL: the one lit knob (action column), -1 = none */ /* v7 PROJECT page, KNOB 4: 0 SAVE, 1 LOAD */
static unsigned boot_active, boot_phase;
static uint32_t boot_started;
static const char *const seq_pages[] = {"1/8 BASIC", "2/8 STEP DETAILS", "3/8 RHYTHM/REC", "4/8 LOCKS", "5/8 SYNC / LOOP", "6/8 MIDI TRACK", "7/8 MIDI ROUTING", "8/8 TRACK ROLE"};
static unsigned rec_down, rec_used;
static uint32_t rec_ms;
static int octave;
/* The editor cursor and the visible playback page are independent. Manual
 * paging/editing pins the view; SEQ resumes follow without moving the cursor. */
static unsigned step_follow = 1, transport_seen;
static const char *notice;
static uint32_t notice_until;
static char master_notice[24];
static unsigned step_first(const struct groove_view *v)
{
    return ((step_follow && v->playing ? v->play_step : cursor) % v->sequence_length) & ~15U;
}
static uint8_t operator_page[GROOVE_TRACKS][8], operator_selected[GROOVE_TRACKS],
    lfo_page[GROOVE_TRACKS];
static uint8_t tone_pages[GROOVE_TRACKS][GROOVE_PADS + 1], drum_parts[GROOVE_TRACKS];
static uint32_t operator_time[6], operator_held, operator_long;
static uint8_t key_note[27], key_track[27], retry_reset;
static uint32_t previous_buttons, previous_keys, blocked_keys, frame_ms, glo_ms;
static uint32_t project_keys; /* Held Play pitches across a project remapping. */
static uint32_t encoder_time[7];
static int8_t encoder_direction[7];
static uint8_t encoder_speed[7];
static unsigned glo_down, glo_used, ble_device, ble_was_linked, ble_view, ble_action;
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
static unsigned home_down, home_long, home_used;
static uint32_t home_ms;
static unsigned fx_row;
static unsigned arp_down, arp_long;
static uint32_t arp_ms;
static char project_stored[13];
static int project_stored_result;
static uint32_t project_drawn_revision;
static void project_refresh(void)
{
    if (project_factory) {
        const char *name = groove_factory_name(project_slot);
        memset(project_stored, ' ', 12);
        if (name) memcpy(project_stored, name, strlen(name));
        project_stored[12] = 0;
        project_stored_result = name ? 0 : GROOVE_PROJECT_INVALID;
    } else project_stored_result = groove_project_info(project_slot, project_stored);
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
struct draw_cache {
    unsigned shown_page;
    uint32_t header_cache, title_cache, middle_cache, hint_cache, hold_cache, seq_cache;
    uint32_t foot_cache[4], row_cache[10], middle_shape, cell_cache[8][16], tile_cache[16];
    unsigned meter_cache[8];
    uint32_t top_cache[3]; /* v6 top bar: left, badges, right/centre */
};
static struct draw_cache saved_cache, band_cache, *cache = &saved_cache;
static uint32_t led_cache;
static uint16_t button_led_cache;
static struct groove_view frame_views[GROOVE_TRACKS];
static uint8_t frame_step_notes[16], frame_effects[4];
static unsigned lfo_sel; /* GLOBAL LFO page: 0 = LFO 1, 1 = LFO 2 */
static unsigned glob_row, battery_adc, battery_shown = ~0U;
/* HOME tap walks back through the pages actually visited; hold goes home. */
enum { BACK_DEPTH = 8 };
static uint8_t back_stack[BACK_DEPTH], back_n, back_going;
static uint8_t key_arp[27], key_seq[27], chain_slot[16], chain_pos, chain_on, chain_cursor, chain_prev_step;
/* v7 CHAIN repeats: field i plays chain_rep[i] + 1 times (x1..x16); var_sel = the lit VARIATION knob. */
static uint8_t chain_rep[16], chain_loops, var_sel;
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
    return page == BLE || page == PROJECT || page == PRESET_BROWSER || page == MIX || page == NAME ||
        (page >= SOURCE && page <= RHYTHM) || (page == STEPS && !writing && seq_page) || glo_down;
}
static unsigned encoder_active(const int16_t values[7])
{
    for (unsigned i = 0; i < 7; ++i)
        if (values[i])
            return 1;
    return 0;
}
static void read_encoders(const struct fm1_controls *controls, uint32_t now, unsigned boundary)
{
    /* Apply the first detent immediately. A continuing fast turn accelerates
     * only continuous parameters; page/option selectors retain raw detents.
     * Reversal, a pause or a new control context returns to fine adjustment. */
    if (boundary) memset(encoder_direction, 0, sizeof encoder_direction);
    for (unsigned i = 0; i < 7; ++i) {
        int direction = (controls->encoders[i] > 0) - (controls->encoders[i] < 0);
        if (!direction) continue;
        encoder_speed[i] = direction == encoder_direction[i] && now - encoder_time[i] <= 40U
            ? (uint8_t)clamp(encoder_speed[i] + 1, 1, 4) : 1;
        encoder_direction[i] = (int8_t)direction;
        encoder_time[i] = now;
    }
}
#ifdef FM1_HOST_TEST
static unsigned web_fine;
void groove_screen_web_fine(unsigned fine) { web_fine = fine; }
#endif
static int fm_encoder_delta(unsigned encoder, int16_t value)
{
#ifdef FM1_HOST_TEST
    if (web_fine) return value;
#endif
    int amount = value < 0 ? -(int)value : value;
    /* Slow: one step per detent. Fast (several detents in a row): seven. */
    int gain = amount < 2 ? 1 : amount < 4 ? 2 : 7;
    int speed = encoder_speed[encoder] >= 3 ? 7 : encoder_speed[encoder];
    if (gain < speed) gain = speed;
    return value * gain;
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
enum { V_DIM = 0x8c32, V_FAINT = 0x4a2a, V_RED = 0xfa49, V_ORANGE = 0xfcc7, V_PINK = 0xc20f,
       V_NAME = 0xbd98, V_DARK = 0x2926, V_NONE = 0x0001 };
static unsigned lcd_glyph(const struct lcd_font *f, char c)
{
    unsigned u = (unsigned char)c;
    if (u >= 'a' && u <= 'z') u -= 32;
    if (u == '`' && f->count == 65) return 64; /* middle dot */
    u -= f->first;
    return u < f->count ? u : ~0U;
}
static unsigned lcd_width(const struct lcd_font *f, const char *s)
{
    unsigned w = 0;
    for (; *s; ++s) { unsigned g = lcd_glyph(f, *s); if (g != ~0U) w += f->adv[g]; }
    return w;
}
/* y is the top of the em box, like canvas textBaseline 'top'. */
static void lcd_text(int x, unsigned y, const char *s, const struct lcd_font *f, uint16_t color, uint16_t bg)
{
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
    strncpy(out, in, 15); out[15] = 0;
    for (unsigned n = (unsigned)strlen(out); lcd_width(f, out) > width && n > 4; --n) {
        char tmp[16]; abbreviate(in, tmp, n - 1); strcpy(out, tmp);
    }
}
/* Canvas roundRect on the LCD: radius 1 = 1.5 px (soft corner pixel),
 * 2 = 2..3 px (corner cut, neighbours soft). V_NONE skips fill or edge. */
static void rbox(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r, uint16_t fill, uint16_t edge)
{
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
/* v6 knob bar: coloured line, label, value in the knob colour. */
static void knob(unsigned col, const char *label_in, const char *value_in, uint16_t color)
{
    char label[16], value[16];
    const struct lcd_font *lf = lcd_width(&lcd14, label_in) <= 56 ? &lcd14 : &lcd12;
    fit(label_in, label, lf, 56);
    /* Values: 20 px when they fit, else 16, 13 (bigger text where there is room). */
    const struct lcd_font *vf = lcd_width(&lcd20, value_in) <= 56 ? &lcd20 : lcd_width(&lcd16, value_in) <= 56 ? &lcd16 : &lcd13;
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
    if (paint_rendering && (paint_top >= 233 || paint_top + PAINT_ROWS <= 195)) return;
    uint32_t sig =
        signature(signature(color, label, (unsigned)strlen(label)), value, (unsigned)strlen(value));
    if (cache->foot_cache[col] == sig)
        return;
    cache->foot_cache[col] = sig;
    unsigned x = 60 * col;
    paint_fill(x, 195, 60, 38, BACK);
    if (!*label && !*value) return;
    paint_fill(x + 6, 197, 48, 2, knob_colors[col & 3U]);
    uint16_t vc = (knob_tint >> col) & 1U ? color : knob_colors[col & 3U];
    lcd_center((int)x + 30, lf == &lcd14 ? 200U : 201U, label, lf, TEXT, BACK);
    lcd_center((int)x + 30, vf == &lcd20 ? 215U : vf == &lcd16 ? 217U : 219U, value, vf, vc, BACK);
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
 * EMPTY is a guess (3.3 V of 4.2 V) until the second measurement. */
enum { BAT_ADC_FULL = 627, BAT_ADC_EMPTY = 493 };
static unsigned battery_percent(void)
{
    int p = ((int)battery_adc - BAT_ADC_EMPTY) * 100 / (BAT_ADC_FULL - BAT_ADC_EMPTY);
    return (unsigned)clamp(p, 0, 100);
}
static void track_sound_name(const struct groove_view *v, unsigned t, char out[10]);
static const char *const engine_names[GROOVE_SOURCES] = {"DX7", "303", "808", "PSG", "PLUCK", "SAMPLER"};
/* "N · ENGINE · SOUND"; the engine is left out when the sound name starts with it. */
static void track_title(const struct groove_view *v, unsigned t, char out[24])
{
    char name[10]; track_sound_name(v, t, name);
    const char *engine = v->type < GROOVE_SOURCES ? engine_names[v->type] : "";
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
/* v7 SEL: SELECT picks what (STEP / PAGE / TRACK / SOUND), a knob picks the
 * action, SAVE does it. Numbers index actions[]; 200 = fill the page through
 * the whole length (REPEAT TO 32/48/64); 255 = empty. */
enum { SEL_ROWS = 4, SEL_FILL = 200 };
static const uint8_t sel_grid[SEL_ROWS][4] = {
    {2, 5, 8, 255}, {3, 6, 9, SEL_FILL}, {4, 7, 10, 14}, {15, 16, 255, 17}};
static const char *const sel_rows[SEL_ROWS] = {"STEP", "PAGE", "TRACK", "SOUND"};
static const char *const sel_items[SEL_ROWS][4] = {
    {"COPY", "PASTE", "CLEAR", ""}, {"COPY", "PASTE", "CLEAR", "FILL"}, {"COPY", "PASTE", "CLEAR", "UNDO"}, {"COPY", "PASTE", "", "NAME"}};
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
/* Small arrow beside the section title: SELECT can turn this way. The
 * vertical side (7 px) is longer than the two slanted ones (tip 3.5 px out). */
static void select_arrow(int x, int cy, int dir)
{
    fill_tri(x * 10, cy * 10 - 35, x * 10, cy * 10 + 35, x * 10 + dir * 35, cy * 10, V_DIM);
}
/* arrows: bit 0 = SELECT can go left (back), bit 1 = right (forward). */
static void engine_icon(unsigned x, unsigned y, int engine, uint16_t c);
static __attribute__((noinline)) void top_bar(const struct groove_view *v, const char *section, const char *object, uint16_t object_color, unsigned arrows, int engine)
{
    if (paint_rendering && paint_top >= 42) return;
    unsigned wait = v->clock_mode && !v->tempo;
    unsigned src = groove_midi_setting(track, 6);
    uint32_t sig = (wait ? 0U : v->tempo) | ((uint32_t)v->clock_mode << 10) | ((uint32_t)v->clock_state << 11) | (src << 14);
    if (sig != cache->top_cache[0]) {
        cache->top_cache[0] = sig;
        paint_fill(0, 0, 62, 28, BACK);
        char bpm[12];
        if (wait) strcpy(bpm, "---"); else decimal(bpm, v->tempo, 1);
        lcd_text(5, 3, bpm, &lcd22, TEXT, BACK); /* same size and line as the battery digits */
        const char *label = !v->clock_mode ? "BPM" : v->clock_state == GROOVE_CLOCK_LOST ? "LOST"
            : v->clock_state != GROOVE_CLOCK_LOCKED ? "WAIT" : src == 1 ? "USB" : src == 2 ? "BLE" : src == 3 ? "DIN" : "EXT";
        lcd_text(8 + (int)lcd_width(&lcd22, bpm), 13, label, &lcd9,
                 v->clock_mode && v->clock_state == GROOVE_CLOCK_LOST ? V_ORANGE : V_DIM, BACK);
    }
    unsigned lfo_on = (lfo_get(0, 0) != 0 && lfo_get(0, 2) != 63) || (lfo_get(1, 0) != 0 && lfo_get(1, 2) != 63);
    unsigned badges = step_follow | ((arp_enabled(track) != 0) << 1) | ((v->playing != 0) << 2) | (lfo_on << 3);
    if (badges != cache->top_cache[1]) {
        cache->top_cache[1] = badges;
        static const char letters[4][2] = {"F", "A", "S", "L"};
        static const uint16_t tint[4] = {TEXT, 0x3f0f, V_RED, 0x2ed9};
        paint_fill(4, 28, 60, 14, BACK);
        for (unsigned i = 0; i < 4; ++i) {
            unsigned x = 5 + i * 15, on = badges >> i & 1U;
            rbox(x, 29, 12, 12, 2, on ? tint[i] : V_NONE, on ? V_NONE : V_FAINT);
            lcd_center((int)x + 6, 29, letters[i], &lcd11, on ? BACK : V_FAINT, on ? tint[i] : BACK);
        }
    }
    unsigned paused = !v->playing && !groove_sequence_rewound();
    unsigned state = v->playing ? 0U : paused ? 1U : 2U, pct = battery_shown > 100U ? battery_percent() : battery_shown;
    uint32_t right = signature(signature(state | (pct << 2) | ((uint32_t)object_color << 9) | (arrows << 30) | ((uint32_t)(engine + 1) << 25), section, (unsigned)strlen(section)),
                               object, (unsigned)strlen(object));
    if (right == cache->top_cache[2]) return;
    cache->top_cache[2] = right;
    paint_fill(62, 0, 178, 28, BACK);
    paint_fill(64, 28, 126, 14, BACK); /* the icon reaches y 41; STEPS (x >= 190) is drawn by SEQ */
    /* Ink centre of the 22 px digits drawn at y 3, nudged up 1 px. */
    /* Play/pause (13 rows) and the battery (2-row cap + 13-row body) are
     * centred on the ink of the 22 px digits drawn at y 3. */
    const int ink2 = 2 * (3 + lcd22.digit_top) + lcd22.digit_height - 1, bx = 228;
    const int bt = (ink2 - 14 + 1) / 2 + 2; /* battery body top */
    rbox(bx, (unsigned)bt, 6, 13, 1, V_NONE, TEXT);
    paint_fill(bx + 2, (unsigned)(bt - 2), 2, 2, TEXT);
    unsigned h = (pct * 9U + 50U) / 100U;
    if (h) paint_fill(bx + 2, (unsigned)(bt + 11) - h, 2, h, TEXT);
    char num[12]; decimal(num, (int)pct, 1);
    uint16_t pc = pct >= 65 ? 0x3f0f : pct >= 30 ? 0xfe87 : pct >= 10 ? V_ORANGE : V_RED;
    int dr = bx - 4, px = dr - (int)lcd_width(&lcd22, num) - 4 - 11, py = (ink2 - 12 + 1) / 2;
    lcd_text(dr - (int)lcd_width(&lcd22, num), 3, num, &lcd22, pc, BACK);
    if (state == 0) {
        for (int r = 0; r < 13; ++r) {
            int d = r < 6 ? 6 - r : r - 6, len = (11 * (13 - 2 * d) + 6) / 13;
            paint_fill((unsigned)px, (unsigned)(py + r), (unsigned)len, 1, 0x3f0f);
        }
        line(px + 2, py + 9, px + 6, py + 7, 0x1c69);
    } else if (state == 1) {
        paint_fill((unsigned)px, (unsigned)py, 4, 13, 0xfe87);
        paint_fill((unsigned)px + 7, (unsigned)py, 4, 13, 0xfe87);
    } /* stopped: no icon */
    int left = 64, rgt = px - 6, cx = (left + rgt) / 2;
    unsigned maxw = (unsigned)(rgt - left);
    const struct lcd_font *sf = lcd_width(&lcd13, section) <= maxw ? &lcd13 : &lcd11;
    char line1[16]; fit(section, line1, sf, maxw);
    lcd_center(cx, 3, line1, sf, V_DIM, BACK);
    if (arrows) {
        int half = (int)lcd_width(sf, line1) / 2, ay = 3 + sf->digit_top + sf->digit_height / 2;
        if (arrows & 1U) select_arrow(cx - half - 5, ay, -1);
        if (arrows & 2U) select_arrow(cx + half + 5, ay, 1);
    }
    if (engine >= 0) {
        /* v7: engine icon (y 17..41) + sound name centred on it, one group
         * centred between the F/A/S/L badges and the play icon. */
        static const struct lcd_font *const nsz[3] = {&lcd15, &lcd13, &lcd12};
        unsigned avail = (unsigned)(rgt - 66), k = 0;
        while (k < 2 && 36U + lcd_width(nsz[k], object) > avail) ++k;
        char nm[24]; strncpy(nm, object, 23); nm[23] = 0;
        for (unsigned n = (unsigned)strlen(nm); n > 1 && 36U + lcd_width(nsz[k], nm) > avail; ) nm[--n] = 0;
        int gw = 36 + (int)lcd_width(nsz[k], nm), gx = (66 + rgt) / 2 - gw / 2;
        if (gx < 66) gx = 66;
        engine_icon((unsigned)gx, 17, engine, 0x3f0f);
        lcd_text(gx + 36, (unsigned)(29 - nsz[k]->digit_top - nsz[k]->digit_height / 2), nm, nsz[k], object_color, BACK);
        return;
    }
    static const struct lcd_font *const sizes[3] = {&lcd13, &lcd12, &lcd11};
    unsigned k = 0;
    while (k < 2 && lcd_width(sizes[k], object) > maxw) ++k;
    char line2[24]; strncpy(line2, object, 23); line2[23] = 0;
    for (unsigned n = (unsigned)strlen(line2); n > 1 && lcd_width(sizes[k], line2) > maxw; ) line2[--n] = 0;
    lcd_center(cx, k ? 18U : 17U, line2, sizes[k], object_color, BACK);
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
            uint16_t bg = t == track ? colors[t] : PANEL;
            paint_fill(6, y, 18, 16, bg);
            number(12, y + 5, t + 1, 1, 1, t == track ? BACK : colors[t], bg);
            paint_fill(7, y + 12, 3, 3, midi_active[t] ? GREEN : bg);
            char fm_name[8]={0}; uint8_t patch[156];
            if (v[t].type==GROOVE_DX7 && !groove_fm_copy(t,patch,NULL)) memcpy(fm_name,patch+145,7);
            const char *lib = sounds_name(t);
            text(29, y + 1,
                 lib && v[t].ready            ? lib
                 : !v[t].ready                ? "ERROR"
                 : v[t].type == GROOVE_DX7    ? fm_name
                 : v[t].type == GROOVE_303    ? (v[t].program ? "303 SQR" : "303 SAW")
                 : v[t].type == GROOVE_PSG    ? chip_names[v[t].program & 3U]
                 : v[t].type == GROOVE_PLUCK  ? "PLUCK"
                 : v[t].type == GROOVE_SAMPLE ? "SAMPLER"
                                              : "808 KIT",
                 1, colors[t], BACK);
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
            paint_fill(x + 1, y + 5, 4, 8, on ? (v[t].audible ? colors[t] : DIM) : enabled ? PANEL : BACK);
            if (head)
                paint_fill(x, y + 3, 6, 2, GREEN);
        }
        unsigned meter = v[t].audible ? (v[t].level * 14 + 63) / 127 : 0;
        unsigned meter_signature = meter | (v[t].level >= 124 ? 16U : 0U);
        if (meter_signature != cache->meter_cache[t]) {
            cache->meter_cache[t] = meter_signature;
            paint_fill(216, y + 11, 14, 3, PANEL);
            if (meter)
                paint_fill(216, y + 11, meter, 3, v[t].level >= 124 ? AMBER : colors[t]);
        }
    }
}
/* Short sound name of a track for the mixer rows (nine characters at most). */
static void track_sound_name(const struct groove_view *v, unsigned t, char out[10])
{
    static const char *const chip_names[] = {"PSG YM", "PSG AY", "PSG NOISE", "PSG MIX"};
    const char *lib = v->ready ? sounds_name(t) : 0;
    char fm_name[8] = {0}; uint8_t patch[156]; /* V14: names come from the voice itself */
    if (!lib && v->ready && v->type == GROOVE_DX7 && !groove_fm_copy(t, patch, NULL)) memcpy(fm_name, patch + 145, 7);
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
enum { TRK_Y = 50, TRK_PITCH = 17, TRK_H = 17 };
static uint8_t frame_mix_vel[GROOVE_TRACKS][16];
static unsigned mix_step_x(unsigned i) { return 84 + i * 8 + (i / 4) * 2; }
/* Each row shows its own 16-step window: the one that is playing. */
static unsigned mix_first(const struct groove_view *v, unsigned t)
{
    unsigned length = v->sequence_length ? v->sequence_length : 16U;
    return (v->playing ? v->play_step % length : t == track ? cursor % length : 0U) & ~15U;
}
static __attribute__((noinline)) void tracks_draw(const struct groove_view v[8])
{
    /* One shared playhead: a short white bar above the step grid (the
     * selected track's step), instead of a moving frame in every row. */
    if (!(paint_rendering && (paint_top >= 50 || paint_top + PAINT_ROWS <= 43))) {
        const struct groove_view *sv = &v[track];
        unsigned len = sv->sequence_length ? sv->sequence_length : 16U;
        unsigned head = sv->playing ? (sv->play_step % len) - mix_first(sv, track) : 16U;
        if (head != cache->middle_cache) {
            cache->middle_cache = head;
            paint_fill(80, 43, 140, 6, BACK);
            if (head < 16) paint_fill(mix_step_x(head), 45, 6, 3, V_RED);
        }
    }
    if (paint_rendering && (paint_top >= TRK_Y + 8 * TRK_PITCH || paint_top + PAINT_ROWS <= TRK_Y)) return;
    for (unsigned t = 0; t < 8; ++t) {
        unsigned y = TRK_Y + TRK_PITCH * t;
        if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + TRK_H <= paint_top)) continue;
        unsigned length = v[t].sequence_length ? v[t].sequence_length : 16U;
        unsigned selected = t == track, first = mix_first(&v[t], t);
        unsigned lamps = (unsigned)((v[t].active_steps >> first) & 0xffffU);
        unsigned head = v[t].playing ? (v[t].play_step % length) - first : 16U;
        unsigned lv = v[t].audible ? v[t].level : 0U, pulse = 0;
        while (pulse < 7 && lv >= (1U << pulse)) ++pulse; /* log meter: 1,2,4..64 */
        if (lv >= 124) pulse = 8;
        uint32_t shades = 0; /* 2 bits per step: velocity in four steps of brightness */
        for (unsigned i = 0; i < 16; ++i) {
            unsigned vel = frame_mix_vel[t][i];
            shades |= (uint32_t)(vel >= 112 ? 3U : vel >= 80 ? 2U : vel >= 48 ? 1U : 0U) << (2 * i);
        }
        /* The row's labels and meter, and each step cell, have their own
         * damage: a moving playhead repaints two small cells, not the row. */
        uint8_t key[8] = {(uint8_t)(selected | (v[t].muted << 1) | (v[t].solo << 2) | (v[t].audible << 3) | (v[t].ready << 4)),
                          v[t].type, v[t].program, (uint8_t)pulse, (uint8_t)sounds_id(t), 0, 0, 0};
        uint32_t sig = signature(7U, key, sizeof key);
        if (sig != cache->row_cache[t]) {
            cache->row_cache[t] = sig;
            paint_fill(0, y, 82, TRK_H, BACK);
            paint_fill(220, y, 20, TRK_H, BACK);
            if (selected) paint_fill(3, y + 2, 1, 12, V_PINK);
            char num[2] = {(char)('1' + t), 0};
            lcd_center(9, y + 1, num, &lcd13, selected ? TEXT : V_DIM, BACK);
            char sound[10], name[16]; track_sound_name(&v[t], t, sound);
            const struct lcd_font *nf = lcd_width(&lcd12, sound) <= 46 ? &lcd12 : &lcd11;
            fit(sound, name, nf, 46);
            lcd_text(20, nf == &lcd12 ? y + 2 : y + 3, name, nf, !v[t].audible ? V_FAINT : selected ? TEXT : V_NAME, BACK);
            paint_fill(72, y + 3, 4, 11, V_DARK);
            if (pulse) {
                unsigned h = (pulse * 11U + 4U) / 8U;
                paint_fill(72, y + 14 - h, 4, h, pulse == 8 ? AMBER : colors[t]);
            }
            if (v[t].muted) lcd_center(230, y + 1, "M", &lcd13, 0xfe87, BACK);
            else if (v[t].solo) lcd_center(230, y + 1, "S", &lcd13, 0x2ed9, BACK);
        }
        unsigned lit = (v[t].volume * 16U + 63U) / 127U; /* volume = frames lit in the track colour */
        for (unsigned i = 0; i < 16; ++i) {
            unsigned x = mix_step_x(i), used = first + i < length, on = (lamps >> i) & 1U, q = (shades >> (2 * i)) & 3U;
            uint32_t cell = 1U | (used << 1) | (on << 2) | (q << 3) | ((i < lit) << 5) | (v[t].audible << 7) | (t << 8);
            if (cell == cache->cell_cache[t][i]) continue;
            cache->cell_cache[t][i] = cell;
            paint_fill(x, y + 3, 6, 11, BACK);
            if (!used) continue;
            if (on) rbox(x, y + 3, 6, 11, 1, shade(colors[t], 0, (3U - q) * 2U), V_NONE);
            else rbox(x, y + 3, 6, 11, 1, 0x1083, i < lit ? shade(colors[t], 0, v[t].audible ? 3U : 7U)
                                                 : i % 4 == 0 ? 0x39a8 : 0x2105);
        }
        (void)head;
    }
}
static void dx7_brightness(unsigned t, int delta);
static int dx7_brightness_value(const uint8_t patch[156]);
static void dx7_algorithm_edit(unsigned t, int delta);
/* Mixer KNOB1: the track's filter. 303 cutoff, pluck brightness, DX7
 * brightness (modulator output levels). Other engines have no filter. */
static void track_filter_edit(unsigned t, int delta)
{
    unsigned type = groove_source_type(t);
    if (type == GROOVE_303) groove_tone_edit(t, 0, 0, delta);
    else if (type == GROOVE_PLUCK) groove_tone_edit(t, 0, 1, delta);
    else if (type == GROOVE_DX7) dx7_brightness(t, delta);
}
/* KNOB4 on the mixer and the track page: ON > MUTE > SOLO. */
static void mix_cycle(unsigned t, int delta)
{
    struct groove_view mv = {0};
    groove_get_view(t, 0, &mv);
    int state = mv.solo ? 2 : mv.muted ? 1 : 0;
    int next = clamp(state + (delta > 0 ? 1 : -1), 0, 2);
    if (next == state) return;
    if (state == 1) groove_mix_toggle(t, 0);
    if (state == 2) groove_mix_toggle(t, 1);
    if (next == 1) groove_mix_toggle(t, 0);
    if (next == 2) groove_mix_toggle(t, 1);
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
static void step_knob(unsigned col, unsigned bank)
{
    static const char *const labels[] = {"VELOCITY", "GATE", "PROB", "CHORD", "TIE", "LOCK", "VALUE", "NOTE"};
    unsigned setting = bank * 4 + col, v = groove_step_setting(track, cursor, setting);
    char value[12];
    if (setting == 1) { decimal(value, v ? v == 15 ? 100 : (v * 100 + 8) / 16 : 50, 1); strcat(value, "%"); }
    else if (setting == 2) { decimal(value, v ? (v * 100 + 4) / 8 : 100, 1); strcat(value, "%"); }
    else if (setting == 3) strcpy(value, groove_sequence_chord_name(v));
    else if (setting == 4) strcpy(value, v ? "ON" : "OFF");
    else if (setting == 5 && !v) strcpy(value, "OFF");
    else if (setting < 7) decimal(value, v, 1);
    else note_name(frame_views[track].step.note, value);
    if (groove_pad_count(track) && (setting == 1 || setting == 3 || setting == 4 || setting == 7)) strcpy(value, "--");
    knob(col, labels[setting], value, colors[col]);
}
static void step_adjust(unsigned bank, const int16_t *encoders, uint32_t now)
{
    for (unsigned i = 0; i < 4; ++i) if (encoders[2 + i]) {
        unsigned setting = bank * 4 + i;
        if (setting < 7) {
            if (groove_step_setting_edit(track, cursor, setting, encoders[2 + i])) {
                notice = "CHORD/LOCK FULL"; notice_until = now + 2000U;
            }
        } else if (!groove_pad_count(track)) {
            struct groove_view v = {0}; groove_get_view(track, cursor, &v);
            groove_sequence_edit(track, cursor, (unsigned)clamp((int)v.step.note + encoders[2 + i], 0, 127), v.step.velocity);
        }
    }
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
enum { SEQ_X = 5, SEQ_Y = 74, SEQ_PITCH = 14, SEQ_W = 11, SEQ_ROW = 31, SEQ_H = 24 };
static uint16_t mix565(uint16_t bg, uint16_t fg, unsigned a);
/* v7 cursors, the same everywhere. Playing: red, two pixels thick inward.
 * Selected: white, pulsing, each inner ring fainter (a gradient to the centre).
 * `under` is the colour inside the box; pulse_level() steps 10 times a second. */
static unsigned pulse_level(void)
{
    unsigned t = (frame_time / 100U) % 10U;
    return 20U + (t < 5U ? t : 10U - t) * 4U; /* 20..40 of 40 */
}
static void play_cursor(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r)
{
    rbox(x, y, w, h, r, V_NONE, V_RED);
    frame(x + 1, y + 1, w - 2, h - 2, V_RED);
}
static void select_cursor(unsigned x, unsigned y, unsigned w, unsigned h, unsigned r, uint16_t under)
{
    static const uint8_t fade[4] = {40, 20, 10, 4};
    unsigned p = pulse_level();
    rbox(x, y, w, h, r, V_NONE, mix565(under, TEXT, p));
    for (unsigned k = 1; k < 4 && w > 2 * k + 1 && h > 2 * k + 1; ++k)
        frame(x + k, y + k, w - 2 * k, h - 2 * k, mix565(under, TEXT, p * fade[k] / 40U));
}
static unsigned seq_cell_x(unsigned i) { return SEQ_X + i * SEQ_PITCH + (i / 4) * 3; }
static __attribute__((noinline)) void steps(const struct groove_view *v)
{
    if (paint_rendering && (paint_top >= SEQ_Y + 3 * SEQ_ROW + SEQ_H || paint_top + PAINT_ROWS <= 42)) return;
    unsigned pads = groove_pad_count(track) != 0;
    if (!seq_slots_valid) {
        char name[13]; seq_slots_used = 0; seq_slots_valid = 1;
        for (unsigned i = 0; i < GROOVE_PROJECT_SLOTS; ++i)
            if (groove_project_info(i, name) == 0) seq_slots_used |= 1U << i;
    }
    uint32_t strip = chain_on | (preset_slot << 1) | ((preset_loaded ? preset_current_slot + 1U : 0U) << 6) | (rec_mode << 12) | (writing << 13);
    strip = signature(strip, &seq_slots_used, sizeof seq_slots_used);
    if (strip != cache->seq_cache && !(paint_rendering && paint_top >= 68)) {
        cache->seq_cache = strip;
        paint_fill(0, 42, 240, 26, BACK);
        /* v7: REC + mode lamp on the left (ALGORITHM picks STEP / LIVE; red
         * while recording), CHAIN + lamp on the right edge. */
        {
            const char *mode = rec_mode ? "LIVE" : "STEP";
            unsigned lw = lcd_width(&lcd13, "REC"), mw = lcd_width(&lcd8, mode) + 8U;
            unsigned ly = 43 + lcd13.digit_top + lcd13.digit_height / 2U - 4U;
            uint16_t c = writing ? V_RED : rec_mode ? V_ORANGE : 0xfe87;
            lcd_text(8, 43, "REC", &lcd13, writing ? V_RED : TEXT, BACK);
            rbox(8 + lw + 3U, ly, mw, 9, 2, c, V_NONE);
            lcd_center((int)(8 + lw + 3U + mw / 2U), ly + 1U, mode, &lcd8, BACK, c);
        }
        unsigned cw = lcd_width(&lcd13, "CHAIN"), lw = chain_on ? 16U : 20U;
        label_lamp(232U - (cw + 3U + lw), 43, "CHAIN", &lcd13, 0xfe87, chain_on);
        /* Patterns: yellow = playing now, dark gold = stored, outline = empty, white frame = selected. */
        for (unsigned i = 0; i < 16; ++i) {
            unsigned used = (seq_slots_used >> i & 1U) && i < GROOVE_PROJECT_SLOTS;
            unsigned cur = preset_loaded && i == preset_current_slot, sel = i == preset_slot;
            rbox(10 + i * 14, 59, 10, 8, 2, cur ? 0xfe87 : used ? 0x7b45 : V_NONE,
                 sel ? TEXT : cur || used ? V_NONE : 0x5a64);
        }
    }
    /* STEPS n: under the battery, on the badge row, right edge = battery
     * edge. Label and number are right-aligned one after the other. */
    if (!(paint_rendering && (paint_top >= 44 || paint_top + PAINT_ROWS <= 28)) && cache->middle_cache != v->next_length) {
        cache->middle_cache = v->next_length;
        char num[12]; decimal(num, v->next_length, 1);
        const int mid = 35; /* centre of the F/A/S/L badges */
        int nw = (int)lcd_width(&lcd13, num), lw = (int)lcd_width(&lcd9, "STEPS");
        paint_fill(234 - (unsigned)nw - 4 - (unsigned)lw - 2, 29, (unsigned)(nw + 4 + lw + 2), 13, BACK);
        lcd_text(234 - nw, (unsigned)(mid - lcd13.digit_top - lcd13.digit_height / 2), num, &lcd13,
                 v->next_length != v->sequence_length ? V_ORANGE : TEXT, BACK);
        lcd_text(234 - nw - 4 - lw, (unsigned)(mid - lcd9.digit_top - lcd9.digit_height / 2), "STEPS", &lcd9, V_DIM, BACK);
    }
    uint64_t lane = pads ? groove_drum_steps(track, drum) : v->active_steps, others = 0;
    if (pads) for (unsigned p = 0; p < groove_pad_count(track); ++p) if (p != drum) others |= groove_drum_steps(track, p);
    uint16_t layer = pads ? pad_colors[drum % 12] : 0x3f0f;
    for (unsigned step = 0; step < GROOVE_STEPS; ++step) {
        unsigned r = step / 16, i = step % 16;
        unsigned x = seq_cell_x(i), y = SEQ_Y + r * SEQ_ROW;
        if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + SEQ_H <= paint_top)) continue;
        unsigned used = step < v->next_length;
        unsigned on = (lane >> step) & 1U;
        unsigned head = v->playing && v->play_step == step;
        unsigned vel = pads ? 127U : frame_cells[step].velocity & 127U;
        if (on && !vel) vel = 100;
        unsigned other = used && !on && ((others >> step) & 1U);
        uint32_t sig = 1U | (used << 1) | (on << 2) | (head << 3) | ((step == cursor) << 4) |
                       (vel << 5) | (track << 12) | (drum << 15) | (other << 20) |
                       ((step == cursor ? pulse_level() : 0U) << 21);
        if (sig == cache->cell_cache[r][i]) continue;
        cache->cell_cache[r][i] = sig;
        paint_fill(x, y, SEQ_W, SEQ_H, BACK);
        /* Beyond the chosen length the steps stay, but nearly transparent. */
        unsigned fade = used ? 0U : 8U;
        uint16_t inside = on ? shade(layer, 0, used ? (127U - vel) * 6U / 127U : 8U) : shade(0x1082, 0, fade);
        if (on) rbox(x, y, SEQ_W, SEQ_H, 2, inside, V_NONE);
        else rbox(x, y, SEQ_W, SEQ_H, 2, inside, shade(i % 4 == 0 ? 0x4209 : 0x2926, 0, fade));
        if (other) paint_fill(x + 3, y + SEQ_H - 5, SEQ_W - 6, 3, DIM); /* other drum layers */
        if (head) play_cursor(x, y, SEQ_W, SEQ_H, 2);
        if (step == cursor) select_cursor(x, y, SEQ_W, SEQ_H, 2, inside);
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
    rbox(x, y, 30, 25, 2, shade(c, 0, 8), c);
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
/* One envelope stage, FX style: the line in its colour, a dim fill down to y 184. */
static void env_stage(int x0, int y0, int x1, int y1, uint16_t c, unsigned dim)
{
    if (dim) c = shade(c, 0, 6);
    uint16_t f = shade(c, 0, 7);
    for (int x = x0; x <= x1; ++x) {
        int y = x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y1;
        if (y < 184) paint_fill((unsigned)x, (unsigned)y, 1, (unsigned)(184 - y), f);
    }
    line(x0, y0, x1, y1, c);
    line(x0, y0 - 1, x1, y1 - 1, c);
}
static const uint8_t algorithms[32][12];
/* Mouth of a singing (carrier) voice: 0 when silent, else 2..10 with time. */
static unsigned fm_lab_mouth(unsigned voice, unsigned level)
{
    if (!frame_views[track].audible || !frame_views[track].level || !level) return 0;
    float t = (float)(frame_time % 60000U) / 1000.0f * 5.0f / 6.2831853f + (float)voice * 0.16f;
    float sn = vsin(t - (float)(int)t);
    if (sn < 0) sn = -sn;
    return 2U + (unsigned)((0.3f + 0.7f * sn) * (float)level * 8.0f / 99.0f);
}
/* Positions follow the algorithm: carriers on the bottom row, each modulator
 * above what it feeds; mockup spacing (76 px, rows 142 / 76, scale 1.2),
 * shrinking for wide or tall stacks. The chosen voice wears the nimbus,
 * singing voices open their mouths and get a note. */
static __attribute__((noinline)) void fm_lab_draw(const uint8_t patch[156], unsigned muted)
{
    if (paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44)) return;
    const uint8_t *layout = algorithms[patch[134] & 31U];
    /* The table's columns start anywhere and may skip: count only the used
     * ones and number them left to right. */
    unsigned used_cols = 0, nrows = 1, rank[16];
    for (unsigned i = 0; i < 6; ++i) {
        used_cols |= 1U << (layout[i * 2] >> 4);
        if ((layout[i * 2] & 15U) + 1U > nrows) nrows = (layout[i * 2] & 15U) + 1U;
    }
    unsigned ncols = 0;
    for (unsigned c = 0; c < 16; ++c) rank[c] = used_cols >> c & 1U ? ncols++ : 0U;
    int px = 228 / (int)ncols; if (px > 76) px = 76;
    int py = nrows > 2 ? 84 / ((int)nrows - 1) : 66;
    int s = 12;
    if (px * 12 / 76 < s) s = px * 12 / 76;
    if (py * 12 / 66 < s) s = py * 12 / 66;
    int cx[6], cy[6];
    unsigned mouth[6], moved = 0;
    for (unsigned i = 0; i < 6; ++i) {
        cx[i] = 120 + ((int)rank[layout[i * 2] >> 4] * 2 - ((int)ncols - 1)) * px / 2;
        cy[i] = 142 - (int)(layout[i * 2] & 15U) * py;
        mouth[i] = (layout[i * 2] & 15U) || (muted >> i & 1U) ? 0U : fm_lab_mouth(i, patch[(5 - i) * 21 + 16]);
        if (mouth[i] != cache->cell_cache[0][i]) { cache->cell_cache[0][i] = mouth[i]; moved |= 1U << i; }
    }
    uint32_t sig = signature(patch[134] | (op << 5) | (muted << 8), patch, 126);
    if (cache->middle_shape != sig) {
        cache->middle_shape = sig;
        moved = 0;
        paint_fill(0, 44, 240, 148, BACK);
    } else if (!paint_rendering) {
        /* Only the singing mouths (and their notes) change: mark them. */
        for (unsigned i = 0; i < 6; ++i) if (moved >> i & 1U)
        {
            paint_mark((unsigned)(cx[i] - 4 * s / 10), (unsigned)(cy[i] + 3 * s / 10), (unsigned)(8 * s / 10 + 1), (unsigned)(8 * s / 10 + 2));
            paint_mark((unsigned)(cx[i] + 17 * s / 12), (unsigned)(cy[i] - 19 * s / 12), 7, 9);
        }
        return;
    }
    if (op < 6) voice_nimbus(cx[op], cy[op] - 26 * s / 12, s);
    for (unsigned i = 0; i < 6; ++i) {
        cat_head(cx[i], cy[i], s, i, mouth[i], (muted >> i) & 1U);
        if (layout[i * 2 + 1] & 15U) { /* feedback: a small open ring by the ear */
            int fx = cx[i] + 19 * s / 12, fy = cy[i] - 10 * s / 12;
            for (int a = 0; a < 16; ++a) {
                float c = (float)a / 20.0f;
                int px2 = fx + (int)(4.0f * vsin(c + 0.25f)), py2 = fy + (int)(4.0f * vsin(c));
                paint_fill((unsigned)px2, (unsigned)py2, 1, 1, 0x8bf2);
            }
        }
        if (mouth[i]) { /* a small eighth note by the singer */
            unsigned nx = (unsigned)(cx[i] + 17 * s / 12), ny = (unsigned)(cy[i] - 19 * s / 12);
            uint16_t c = knob_colors[2];
            paint_fill(nx + 3, ny, 1, 7, c);
            paint_fill(nx + 4, ny + 1, 1, 2, c); paint_fill(nx + 5, ny + 2, 1, 2, c);
            paint_fill(nx + 1, ny + 5, 2, 1, c); paint_fill(nx, ny + 6, 4, 2, c);
        }
        if (s >= 9) {
            char num[2] = {(char)('1' + i), 0};
            lcd_center(cx[i], (unsigned)(cy[i] + 25 * s / 12), num, &lcd11, i == op ? TEXT : V_DIM, BACK);
        }
    }
}
/* FM LAB knobs: each value as a small wave in its knob colour. */
static void wave_knob(unsigned col, const char *label, const char *value, float cycles, float amp, float shift)
{
    if (paint_rendering && (paint_top >= 233 || paint_top + PAINT_ROWS <= 190)) return;
    uint32_t sig = signature(signature(0x5741u + col, label, (unsigned)strlen(label)), value, (unsigned)strlen(value));
    if (cache->foot_cache[col] == sig) return;
    cache->foot_cache[col] = sig;
    unsigned x = col * 60 + 4, y = 196;
    uint16_t c = knob_colors[col];
    paint_fill(col * 60, 190, 60, 43, BACK);
    paint_fill(x + 2, 191, 48, 2, c);
    rbox(x, y, 52, 24, 2, 0x0841, V_NONE);
    for (unsigned pass = 0; pass < (shift != 0.0f ? 2U : 1U); ++pass) {
        /* Smooth curve: each column covers y(k-0.5)..y(k+0.5) plus 0.6 px each side. */
        uint16_t ink = pass ? shade(c, 0, 5) : c;
        for (unsigned k = 1; k < 51; ++k) {
            int yv[2];
            for (unsigned h = 0; h < 2; ++h) {
                float ph = ((float)k - 0.5f + (float)h) / 52.0f * cycles - (pass ? shift : 0.0f);
                ph -= (float)(int)ph; if (ph < 0) ph += 1.0f;
                yv[h] = (int)((float)(y + 12) * 10.0f + 5.0f - vsin(ph) * amp * 10.0f);
            }
            int lo = (yv[0] < yv[1] ? yv[0] : yv[1]) - 6, hi = (yv[0] > yv[1] ? yv[0] : yv[1]) + 6;
            for (int r = lo / 10; r * 10 < hi; ++r)
                paint_blend((int)(x + k), r, ink, (unsigned)overlap10(r * 10, r * 10 + 10, lo, hi) * 4U);
        }
    }
    lcd_text((int)x + 2, y + 25, label, &lcd10, V_DIM, BACK);
    lcd_text((int)(x + 51 - lcd_width(&lcd10, value)), y + 25, value, &lcd10, c, BACK);
}
static __attribute__((noinline, cold, minsize)) void fm_draw(const uint8_t patch[156], const struct groove_fm_controls *controls)
{
    if (paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44)) return;
    if (page == FM_EDIT && !(fm_group == 1 || fm_group == 2 || fm_group == 6 || fm_group == 7)) {
        fm_lab_draw(patch, controls->operators);
        return;
    }
    const struct fm_page *p = &fm_pages[fm_group];
    /* Titles, diagrams and the summary row have independent damage. Editing
     * a number never retransmits an unchanged operator diagram. */
    uint32_t title = track | (op << 3) | (fm_group << 6) |
                     ((uint32_t)controls->operators << 10);
    uint32_t shape = patch[134] | (op << 5) | ((uint32_t)controls->operators << 8);
    if (page == FM_ENV)
        shape = signature(controls->enabled, controls->envelope, 4);
    else if (page == FM_EDIT &&
             (fm_group == 1 || fm_group == 2 || fm_group == 6 || fm_group == 7)) {
        unsigned offset = op == 6 ? 126 : (5 - op) * 21;
        shape = signature(op, patch + offset, 8);
    } else if (page == FM_LFO)
        memcpy(&shape, patch + 141, 3);
    if (cache->title_cache != title) {
        cache->title_cache = title;
        paint_fill(0, 44, 240, 36, BACK); /* titles live in the v6 top bar */
        (void)p;
        /* VOICES (rates / levels pages only: the cats pick the voice there;
         * AMP ENV is one envelope for the whole track, so no cats). The word
         * spans from x 8 to where the ADSR OFF lamp ends on AMP ENV. */
        if (page == FM_ENV) goto voices_done;
        {
            const unsigned end = 8 + lcd_width(&lcd13, "ADSR") + 3 + 20;
            const struct lcd_font *vf = lcd_width(&lcd22, "VOICES") <= end - 8 ? &lcd22 : lcd_width(&lcd17, "VOICES") <= end - 8 ? &lcd17 : &lcd13;
            lcd_text(8, (unsigned)(66 - (int)vf->digit_top - (int)vf->digit_height / 2), "VOICES", vf, V_DIM, BACK);
        }
        for (unsigned i = 0; i < 6; ++i) {
            int x = 72 + (int)i * 30;
            if (page == FM_EDIT && i == op) voice_nimbus(x, 54, 5);
            cat_head(x, 66, 6, i, 0, (controls->operators >> i) & 1U);
        }
    voices_done:;
    }
    uint32_t context = track | (fm_group << 3) | (page << 7) | ((op == 6U) << 11);
    if (cache->middle_shape != shape || cache->row_cache[0] != context) {
        cache->row_cache[0] = context;
        cache->middle_shape = shape;
        paint_fill(0, page == FM_ENV ? 44U : 80U, 240, page == FM_ENV ? 148U : 112U, BACK);
        if (page == FM_ENV) {
            /* v6, like the FX curves: dark box, each stage in its knob colour
             * with a soft fill below. Match adsr_config_set's millisecond/level
             * mapping; sustain gets an illustrative 400 ms hold. */
            label_lamp(8, 50, "ADSR", &lcd13, TEXT, controls->enabled);
            unsigned a = 8U * controls->envelope[0], d = 100U + 8U * controls->envelope[1];
            unsigned r = 100U + 19U * controls->envelope[3], total = a + d + 400U + r;
            int x0 = 12, x1 = 12 + (int)(a * 216U / total), x2 = 12 + (int)((a + d) * 216U / total);
            int x3 = 12 + (int)((a + d + 400U) * 216U / total);
            int y = 184 - (int)((100U + 7U * controls->envelope[2]) * 104U / 1000U);
            rbox(6, 70, 228, 118, 2, 0x0841, 0x2926);
            unsigned dim = !controls->enabled;
            env_stage(x0, 184, x1, 78, knob_colors[0], dim);
            env_stage(x1, 78, x2, y, knob_colors[1], dim);
            env_stage(x2, y, x3, y, knob_colors[2], dim);
            env_stage(x3, y, 228, 184, knob_colors[3], dim);
        } else if (page == FM_EDIT &&
                   (fm_group == 1 || fm_group == 2 || fm_group == 6 || fm_group == 7)) {
            /* Rate / level preview: higher RATE = shorter stage, L3 holds. */
            unsigned offset = op == 6 ? 126 : (5 - op) * 21;
            rbox(6, 86, 228, 102, 2, 0x0841, 0x2926);
            if (op == 6) paint_fill(8, 134, 224, 1, 0x18c4);
            int x = 14, last_y = 180 - patch[offset + 7] * 84 / 99;
            for (unsigned i = 0; i < 4; ++i) {
                if (i == 3) { env_stage(x, last_y, x + 36, last_y, knob_colors[2], 0); x += 36; }
                int y = 180 - patch[offset + 4 + i] * 84 / 99;
                int next = x + 10 + (99 - patch[offset + i]) * 34 / 99;
                env_stage(x, last_y, next, y, knob_colors[i], 0);
                paint_fill((unsigned)next - 1, (unsigned)y - 1, 3, 3, TEXT);
                x = next;
                last_y = y;
            }
        } else if (page == FM_LFO) {
            static const char *const waves[6] = {"TRIANGLE", "SAW DOWN", "SAW UP",
                                                 "SQUARE",   "SINE",     "S/HOLD"};
            text(8, 91, "LFO WAVE", 1, DIM, BACK);
            text(8, 110, waves[patch[142] % 6], 2, colors[track], BACK);
            text(8, 146, patch[141] ? "KEY SYNC ON" : "KEY SYNC OFF", 1, TEXT, BACK);
            text(124, 146, "PM SENS", 1, DIM, BACK);
            number(178, 146, patch[143], 1, 1, TEXT, BACK);
        } else
            algorithm_draw(patch[134], controls->operators);
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
    static const uint8_t bass[] = {BASS303_ENVMOD, BASS303_DECAY, BASS303_ACCDEC, BASS303_CUTOFF};
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
static __attribute__((noinline)) void track_lfo_draw(void)
{
    const uint8_t *v = frame_patch;
    uint32_t sig = signature(track | (frame_views[track].type << 8), v, 4);
    if (cache->title_cache == sig) return;
    cache->title_cache = sig;
    paint_fill(0, 44, 240, 151, BACK);
    text(8, 62, "T", 1, colors[track], BACK);
    number(14, 62, track + 1, 1, 1, colors[track], BACK);
    text(32, 62, source_names[frame_views[track].type], 1, TEXT, BACK);
    text(8, 80, v[1] ? "MODULATION ON" : "DEPTH 0 = OFF", 1, v[1] ? GREEN : DIM, BACK);
    text(8, 100, v[2] ? "STEREO PAN" : "VOLUME TREMOLO", 2, TEXT, BACK);
    text(8, 140, "K1 RATE    K2 DEPTH", 1, DIM, BACK);
    text(8, 159, "K3 TARGET  K4 WAVE", 1, DIM, BACK);
    text(8, 181, "KEYS PLAY  SELECT TRACK", 1, DIM, BACK);
    static const char *const labels[] = {"RATE Hz", "DEPTH", "TARGET", "WAVE"};
    static const char *const waves[] = {"TRI", "SQUARE", "SAW UP", "SAW DOWN"};
    for (unsigned i = 0; i < 4; ++i) {
        char value[12];
        if (!i) {
            unsigned rate = groove_lfo_rate(v[0]);
            char fraction[12]; decimal(value, rate / 100, 1); decimal(fraction, rate % 100, 2);
            strcat(value, "."); strcat(value, fraction);
        } else if (i == 1) { decimal(value, v[i], 1); strcat(value, "%"); }
        else strcpy(value, i == 2 ? (v[i] ? "PAN" : "VOLUME") : waves[v[i]]);
        knob(i, labels[i], value, colors[i]);
    }
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
static unsigned tone_active; /* last knob turned on a sound page (0..3) */
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
/* ---- v7 sound pages: the turned knob's parameter, alone and large ---- */
enum { PIC_X = 10, PIC_W = 220, PIC_TOP = 66, PIC_BOT = 184 };
static int16_t pic_y[PIC_W];
/* Draw pic_y[] as a 2 px line in c, optionally with a dim fill down to PIC_BOT. */
static void pic_poly(uint16_t c, unsigned fill)
{
    int last = pic_y[0];
    for (int x = 0; x < PIC_W; ++x) {
        int y = pic_y[x];
        if (y < PIC_TOP) y = PIC_TOP;
        if (y > PIC_BOT) y = PIC_BOT;
        if (fill && y < PIC_BOT) paint_fill((unsigned)(PIC_X + x), (unsigned)y, 1, (unsigned)(PIC_BOT - y), shade(c, 0, 7));
        int lo = last < y ? last : y, hi = last < y ? y : last;
        paint_fill((unsigned)(PIC_X + x), (unsigned)lo, 1, (unsigned)(hi - lo + 2), c);
        last = y;
    }
}
static float pic_sin(float cycles) { return vsin(cycles - (float)(int)cycles); }
/* Filter response for the 303 pictures: fc 0..1, q 0..1. */
static void pic_filter(float fc, float q)
{
    float damp = 1.2f - 1.05f * q, f0 = 0.1f + 0.75f * fc;
    for (int x = 0; x < PIC_W; ++x) {
        float u = (float)x / PIC_W, w = fexp2_ui((u - f0) * 9.0f), d = (1.0f - w * w) * (1.0f - w * w) + w * damp * w * damp;
        float db = -10.0f * vlog2(d < 1e-6f ? 1e-6f : d) / 3.3219f;
        pic_y[x] = (int16_t)(150 - (int)(db * 1.7f));
    }
}
/* A tuning ruler with a needle: pos -1..1 around the centre. */
static void pic_needle(float pos, uint16_t c)
{
    for (int i = -12; i <= 12; ++i) {
        unsigned x = (unsigned)(120 + i * 8), h = i == 0 ? 24U : i % 4 == 0 ? 14U : 8U;
        paint_fill(x, 150 - h / 2U, 1, h, i == 0 ? TEXT : V_DIM);
    }
    int nx = 120 + (int)(pos * 96.0f);
    fill_tri(nx * 10 - 70, 1060, nx * 10 + 70, 1060, nx * 10, 1300, c);
    paint_fill((unsigned)nx, 128, 2, 44, c);
}
/* Envelope with attack 0, peak at top, exponential fall over `fall` of the width. */
static void pic_fall(float start, float height, float fall)
{
    for (int x = 0; x < PIC_W; ++x) {
        float u = (float)x / PIC_W;
        float e = u < start ? 0.0f : fexp2_ui(-(u - start) / (fall < 0.02f ? 0.02f : fall) * 3.0f);
        pic_y[x] = (int16_t)(PIC_BOT - 4 - (int)(height * 108.0f * e));
    }
}
/* v7: one filter picture for all four 303 FILTER knobs (nv = their values
 * 0..1). The curve with its cutoff dot and resonance peak, the ENV MOD ghost
 * (dashed: where the envelope pushes the cutoff) with an arrow out, and the
 * DECAY arrow back. The turned knob's part is lit, the rest is grey. */
static void pic_arrow(int x0, int x1, int y, uint16_t c, unsigned dashed)
{
    int a = x0 < x1 ? x0 : x1, b = x0 < x1 ? x1 : x0, dir = x1 > x0 ? 1 : -1;
    for (int x = a; x < b; ++x)
        if (!dashed || (x - a) % 6 < 3) paint_fill((unsigned)x, (unsigned)y, 1, 2, c);
    fill_tri(x1 * 10 + dir * 20, y * 10 + 10, (x1 - dir * 7) * 10, y * 10 - 40, (x1 - dir * 7) * 10, y * 10 + 60, c);
}
static void filter_picture(const float *nv, unsigned col, uint16_t c)
{
    float fc = nv[0], q = nv[1], ef = fc + 0.6f * nv[2];
    if (ef > 1.0f) ef = 1.0f;
    /* ENVMOD / DECAY: a note every 1.2 s throws the cutoff up to the ghost,
     * DECAY sets how fast it falls back (live). */
    float now = fc;
    if (col >= 2) {
        float ph = (float)(frame_time % 1200U) / 1200.0f, decay = 0.04f + 0.6f * nv[3];
        now = fc + (ef - fc) * fexp2_ui(-ph * 1.2f / decay * 1.44f);
    }
    pic_filter(ef, q);
    uint16_t ghost = col == 2 ? c : V_FAINT;
    for (int x = 0; x < PIC_W; ++x) {
        int y = pic_y[x];
        if (y >= PIC_TOP && y < PIC_BOT && x % 6 < 3) paint_fill((unsigned)(PIC_X + x), (unsigned)y, 1, 2, ghost);
    }
    pic_filter(now, q);
    pic_poly(col < 2 ? c : 0xce79, 1);
    int cx = PIC_X + (int)((0.1f + 0.75f * fc) * PIC_W), ex = PIC_X + (int)((0.1f + 0.75f * ef) * PIC_W);
    int px = 0;
    for (int x = 1; x < PIC_W; ++x) if (pic_y[x] < pic_y[px]) px = x;
    int py = pic_y[px] < PIC_TOP + 4 ? PIC_TOP + 4 : pic_y[px];
    for (int y = py + 8; y < PIC_BOT - 4; y += 4) paint_fill((unsigned)cx, (unsigned)y, 1, 2, col == 0 ? c : V_DIM);
    fill_ellipse(cx * 10 + 5, (PIC_BOT - 4) * 10, col == 0 ? 55 : 35, col == 0 ? 55 : 35, col == 0 ? c : V_DIM);
    fill_ellipse((PIC_X + px) * 10 + 5, py * 10, col == 1 ? 55 : 35, col == 1 ? 55 : 35, col == 1 ? c : V_DIM);
    if (ex > cx + 12) {
        pic_arrow(cx, ex, PIC_TOP + 8, col == 2 ? c : V_DIM, 0);
        pic_arrow(ex, cx, PIC_TOP + 20, col == 3 ? c : V_DIM, 1);
    }
}
/* type: engine, pg: page, col: knob 0..3, n: value 0..1, raw: raw value. */
static void sound_picture(unsigned type, unsigned pg, unsigned col, float n, unsigned raw, uint16_t c)
{
    unsigned id = type == GROOVE_303 ? pg * 4U + col : type == GROOVE_PSG ? 12U + col : 16U + col;
    switch (id) {
    /* 0..3: the 303 FILTER page draws filter_picture() instead. */
    case 4: /* 303 ACCENT: plain notes and the accented one */
        for (unsigned i = 0; i < 4; ++i) {
            unsigned h = i == 3 ? 30U + (unsigned)(n * 86.0f) : 30U, x = 26U + i * 52U; /* accent: 30..116 px */
            rbox(x, PIC_BOT - h, 32, h, 2, i == 3 ? c : shade(c, 0, 6), V_NONE);
        }
        break;
    case 5: /* 303 WAVE: saw or square */
        for (int x = 0; x < PIC_W; ++x) {
            float u = (float)x / PIC_W * 2.0f, ph = u - (float)(int)u;
            pic_y[x] = (int16_t)(125 - (int)(raw ? (ph < 0.5f ? 45 : -45) : 45.0f - 90.0f * ph));
        }
        pic_poly(c, 0); break;
    case 6: case 14: case 18: /* TUNE: needle on a ruler */
        pic_needle(n * 2.0f - 1.0f, c); break;
    case 7: /* 303 VOLUME: the wave's height against full scale */
        for (int x = 0; x < PIC_W; ++x) pic_y[x] = (int16_t)(125 - (int)(55.0f * pic_sin((float)x / PIC_W * 3.0f)));
        pic_poly(V_FAINT, 0);
        for (int x = 0; x < PIC_W; ++x) pic_y[x] = (int16_t)(125 - (int)(55.0f * n * pic_sin((float)x / PIC_W * 3.0f)));
        pic_poly(c, 0); break;
    case 8: /* 303 DRIVE: the wave clipped */
        for (int x = 0; x < PIC_W; ++x) pic_y[x] = (int16_t)(125 - (int)(55.0f * pic_sin((float)x / PIC_W * 2.0f)));
        pic_poly(V_FAINT, 0);
        for (int x = 0; x < PIC_W; ++x) {
            float v = pic_sin((float)x / PIC_W * 2.0f) * (1.0f + 3.0f * n);
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pic_y[x] = (int16_t)(125 - (int)(55.0f * v));
        }
        pic_poly(c, 0); break;
    case 9: /* 303 DRIVE TYPE: OFF / SOFT / RAT curves, the chosen one lit */
        for (unsigned t = 0; t < 3; ++t) {
            unsigned x0 = 16U + t * 72U, on = t == raw;
            rbox(x0, 80, 64, 90, 2, V_NONE, on ? c : 0x2926);
            for (int k = 0; k < 60; ++k) {
                float in = (float)k / 30.0f - 1.0f, out = t == 0 ? in : t == 1 ? in / (1.0f + (in < 0 ? -in : in)) * 1.6f : (in > 0.35f ? 0.35f : in < -0.35f ? -0.35f : in) * 2.5f;
                if (out > 0.9f) out = 0.9f;
                if (out < -0.9f) out = -0.9f;
                paint_fill(x0 + 2U + (unsigned)k, (unsigned)(125 - (int)(out * 40.0f)), 1, 2, on ? c : V_DIM);
            }
        }
        break;
    case 10: /* 303 SLIDE: the glide from one note to the next */
        for (int x = 0; x < PIC_W; ++x) {
            float u = (float)x / PIC_W, g = 0.03f + 0.4f * n, k = u < 0.45f ? 0.0f : u > 0.45f + g ? 1.0f : (u - 0.45f) / g;
            pic_y[x] = (int16_t)(160 - (int)(70.0f * k));
        }
        pic_poly(c, 0); break;
    case 11: /* 303 ACC DEC: the accent bump fading */
        pic_fall(0.15f, 0.85f, 0.03f + 0.7f * n); pic_poly(c, 1); break;
    case 12: /* PSG MODE: YM, AY square, NOISE, MIX */
        for (int x = 0; x < PIC_W; ++x) {
            float u = (float)x / PIC_W * 3.0f, sq = u - (float)(int)u < 0.5f ? 1.0f : -1.0f;
            float nz = (float)(((unsigned)x * 2654435761U >> 27) & 15U) / 7.5f - 1.0f;
            float v = raw == 2 ? nz : raw == 3 ? sq * 0.7f + nz * 0.3f : sq * (raw ? 0.7f : 1.0f);
            pic_y[x] = (int16_t)(125 - (int)(45.0f * v));
        }
        pic_poly(c, 0); break;
    case 13: /* PSG NOISE: amount */
        for (int x = 0; x < PIC_W; x += 3) {
            float nz = (float)(((unsigned)x * 2654435761U >> 27) & 15U) / 7.5f - 1.0f;
            int h = (int)(nz * 55.0f * n);
            paint_fill((unsigned)(PIC_X + x), (unsigned)(h < 0 ? 125 : 125 - h), 2, (unsigned)(h < 0 ? -h : h) + 1U, c);
        }
        break;
    case 15: case 19: /* RELEASE: the tail after the key goes up (dotted line) */
        for (int x = 0; x < PIC_W; ++x) {
            float u = (float)x / PIC_W, r = 0.02f + 0.5f * n;
            float e = u < 0.4f ? 1.0f : fexp2_ui(-(u - 0.4f) / r * 3.0f);
            pic_y[x] = (int16_t)(PIC_BOT - 4 - (int)(95.0f * e));
        }
        pic_poly(c, 1);
        for (int y = PIC_TOP; y < PIC_BOT; y += 4) paint_fill(PIC_X + 88, (unsigned)y, 1, 2, TEXT);
        break;
    case 16: /* PLUCK DECAY: the string ringing out */
        for (int x = 0; x < PIC_W; ++x) {
            float u = (float)x / PIC_W, e = fexp2_ui(-u / (0.05f + 0.5f * n) * 2.0f);
            pic_y[x] = (int16_t)(125 - (int)(55.0f * e * pic_sin(u * 14.0f)));
        }
        pic_poly(c, 0); break;
    case 17: /* PLUCK BRIGHT: harmonics */
        for (unsigned i = 0; i < 10; ++i) {
            float a = fexp2_ui(-(float)i * (1.6f - 1.4f * n));
            unsigned h = (unsigned)(a * 110.0f) + 2U;
            paint_fill(18U + i * 21U, PIC_BOT - h, 14, h, i ? c : TEXT);
        }
        break;
    default: break;
    }
}
static __attribute__((noinline)) void tone_draw(const uint8_t values[GROOVE_TONE_PARAMS])
{
    if (paint_rendering && (paint_top >= 195 || paint_top + PAINT_ROWS <= 32)) return;
    unsigned type = groove_source_type(track), part = tone_part(),
             pg = page == FM_ENV ? 0 : tone_pages[track][tone_slot()];
    const struct groove_view view = frame_views[track];
    uint32_t title = track | (part << 3) | (pg << 7) | (type << 12) | (view.mono << 16);
    if (cache->title_cache != title) {
        cache->title_cache = title;
        paint_fill(0, 44, 240, 151, BACK); /* titles live in the v6 top bar */
        memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
        cache->row_cache[0] = ~0U;
    }
    if (type == GROOVE_808 || !groove_pad_count(track)) {
        /* 808: the part's name big under the title, 11 drums + KIT as icons,
         * the hit below. Melodic engines: the turned knob big, the picture. */
        uint32_t sig = signature(part | (pg << 4) | (tone_active << 8), values, GROOVE_TONE_PARAMS);
        /* 303 FILTER, ENVMOD / DECAY turned: the curve plays a note every 1.2 s (10 frames/s). */
        if (type == GROOVE_303 && pg == 0 && (tone_active & 3U) >= 2U) sig ^= (frame_time / 100U + 1U) << 20;
        if (sig != cache->row_cache[0]) {
            cache->row_cache[0] = sig;
            paint_fill(0, 44, 240, 150, BACK);
            if (type == GROOVE_808) {
                static const char *const long_names[12] = {"BASS DRUM", "SNARE DRUM", "LOW TOM", "MID TOM", "HIGH TOM", "RIM SHOT",
                    "HAND CLAP", "COWBELL", "CYMBAL", "OPEN HAT", "CLOSED HAT", "KIT"};
                lcd_center(120, 44, long_names[part % 12U], &lcd17, TEXT, BACK);
                unsigned count = GROOVE_DRUMS + (page != FM_ENV);
                for (unsigned i = 0; i < count; ++i) {
                    unsigned x = 6 + (i % 6) * 38, y = 66 + (i / 6) * 36, on = i == part, kit = i == GROOVE_DRUMS;
                    uint16_t bg = on ? 0x28c4 : 0x1083, ink = on ? knob_colors[0] : kit ? 0xfe87 : TEXT;
                    rbox(x, y, 34, 32, 2, bg, on ? knob_colors[0] : kit ? 0x5a64 : 0x2105);
                    drum_icon(x, y, i, ink, bg);
                }
                /* the hit: a fast ring-down, longer with DECAY when the part has it */
                rbox(6, 142, 228, 46, 2, 0x0841, 0x2926);
                int prev = 183; /* keeps 3+ px inside the box (y 142..187) */
                for (int x = 0; x < 220; ++x) {
                    float u = (float)x / 220.0f, e = fexp2_ui(-u * 7.0f), w = vsin(u * 30.0f * (1.0f - u * 0.5f) - (float)(int)(u * 30.0f * (1.0f - u * 0.5f)));
                    int y = 183 - (int)(34.0f * e * (w < 0 ? -w : w));
                    line(10 + x, prev, 11 + x, y, knob_colors[2]); prev = y;
                }
            } else {
                /* v7: only the turned knob's parameter, large; its name above the
                 * box in the knob colour; no numbers (the value is under the knob). */
                unsigned col = tone_active & 3U, parameter = tone_parameter(col);
                const fm1_param_t *ap = groove_tone_parameter(track, 0, parameter);
                rbox(6, 62, 228, 126, 2, 0x0841, 0x2926);
                if (ap) {
                    lcd_center(120, 46, ap->name, &lcd13, knob_colors[col], BACK);
                    float n = ap->max ? (float)values[parameter] / (float)ap->max : 0.0f;
                    if (type == GROOVE_303 && pg == 0) {
                        float nv[4];
                        for (unsigned k = 0; k < 4; ++k) {
                            unsigned pk = tone_parameter(k);
                            const fm1_param_t *kp = groove_tone_parameter(track, 0, pk);
                            nv[k] = kp && kp->max ? (float)values[pk] / (float)kp->max : 0.0f;
                        }
                        filter_picture(nv, col, knob_colors[col]);
                    } else sound_picture(type, pg, col, n, values[parameter], knob_colors[col]);
                }
            }
        }
    } else {
        /* v7 SAMPLER: the pad's name large, the 8 pads as tiles with a tiny
         * waveform (chosen = pink), then the turned knob's name and only its
         * picture in the box (no numbers). */
        uint32_t sig = signature(part | (pg << 4) | (tone_active << 8) | 0x5000U, values, GROOVE_TONE_PARAMS);
        if (sig != cache->row_cache[0]) {
            cache->row_cache[0] = sig;
            paint_fill(0, 44, 240, 150, BACK);
            lcd_center(120, 44, groove_pad_name(track, part), &lcd17, TEXT, BACK);
            unsigned count = groove_pad_count(track) < 8 ? groove_pad_count(track) : 8U;
            for (unsigned i = 0; i < count; ++i) {
                unsigned x = 6 + i * 29, on = i == part;
                uint16_t bg = on ? 0x28c4 : 0x1083, ink = on ? knob_colors[0] : V_DIM;
                rbox(x, 62, 26, 24, 2, bg, on ? knob_colors[0] : 0x2105);
                for (unsigned k = 0; k < 9; ++k) { /* tiny waveform */
                    unsigned h = 2U + ((i * 7U + k * 5U) % 9U) * (9U - k) / 9U;
                    paint_fill(x + 4 + k * 2, 74 - h, 1, 2 * h, ink);
                }
            }
            unsigned col = tone_active & 3U, parameter = tone_parameter(col);
            const fm1_param_t *ap = groove_tone_parameter(track, part, parameter);
            rbox(6, 106, 228, 82, 2, 0x0841, 0x2926);
            if (ap) {
                uint16_t c = knob_colors[col];
                float n = ap->max ? (float)values[parameter] / (float)ap->max : 0.0f;
                lcd_center(120, 92, ap->name, &lcd13, c, BACK);
                const int mid = 147;
                unsigned id = pg * 4U + col;
                if (id == 0 || id == 2) { /* LEVEL: height vs full scale; LENGTH: the cut */
                    int cut = id == 2 ? 12 + (int)(n * 212.0f) : 232;
                    float amp = id == 0 ? n : 1.0f;
                    for (int x = 12; x < 228; ++x) {
                        float u = (float)(x - 12) / 216.0f, e = fexp2_ui(-u * 3.0f);
                        int h = (int)(34.0f * e * amp * (0.5f + 0.5f * (float)(((unsigned)x * 2654435761U >> 28) & 7U) / 7.0f));
                        if (id == 0) paint_fill((unsigned)x, (unsigned)(mid - (int)(34.0f * e)), 1, 1, V_FAINT);
                        paint_fill((unsigned)x, (unsigned)(mid - h), 1, (unsigned)(2 * h + 1), x < cut ? c : shade(c, 0, 7));
                    }
                    if (id == 2) paint_fill((unsigned)cut, 110, 2, 74, TEXT);
                } else if (id == 1) { /* TUNE: needle */
                    for (int i = -12; i <= 12; ++i) {
                        unsigned x = (unsigned)(120 + i * 8), h = i == 0 ? 20U : i % 4 == 0 ? 12U : 6U;
                        paint_fill(x, (unsigned)(mid + 8) - h / 2U, 1, h, i == 0 ? TEXT : V_DIM);
                    }
                    int nx = 120 + (int)((n * 2.0f - 1.0f) * 96.0f);
                    fill_tri(nx * 10 - 60, 1150, nx * 10 + 60, 1150, nx * 10, 1350, c);
                    paint_fill((unsigned)nx, 134, 2, 36, c);
                } else if (id == 3) { /* CHOKE: groups 1..4, OFF = none */
                    unsigned g = values[parameter];
                    for (unsigned i = 0; i < 4; ++i) {
                        unsigned x = 30 + i * 50, on = g == i + 1U;
                        rbox(x, 122, 34, 34, 2, on ? shade(c, 0, 5) : V_NONE, on ? c : 0x2926);
                        if (on) { aa_stroke((int)(x + 8) * 10, 1300, (int)(x + 26) * 10, 1480, c); aa_stroke((int)(x + 26) * 10, 1300, (int)(x + 8) * 10, 1480, c); }
                    }
                } else if (id == 4) { /* VARIATION: 4 hits, the chosen one lit */
                    for (unsigned i = 0; i < 4; ++i) {
                        unsigned on = values[parameter] == i, x0 = 16 + i * 54 + i * 2;
                        for (unsigned k = 0; k < 40; ++k) { /* each hit: its own height and length */
                            float e = fexp2_ui(-(float)k / (8.0f + (float)i * 3.0f));
                            unsigned h = (unsigned)(e * (float)(34 - (int)i * 5)) + 1U;
                            paint_fill(x0 + k, (unsigned)mid - h, 1, 2 * h, on ? c : V_FAINT);
                        }
                    }
                } else if (id == 5) { /* VEL LPF: a soft hit duller than a loud one */
                    for (unsigned side = 0; side < 2; ++side)
                        for (unsigned k = 0; k < 96; ++k) {
                            float e = fexp2_ui(-(float)k / 18.0f), spike = side ? (float)((k * 2654435761U >> 29) & 3U) / 3.0f : 0.0f;
                            float amp = side ? 30.0f : 30.0f * (0.35f + 0.65f * (1.0f - n));
                            unsigned h = (unsigned)(e * amp * (0.7f + 0.3f * spike)) + 1U;
                            paint_fill(16 + side * 112 + k, (unsigned)mid - h, 1, 2 * h, side ? c : shade(c, 0, 5));
                        }
                }
            }
        }
    }
}
static __attribute__((noinline)) void mix_draw(const struct groove_view v[8])
{
    if (paint_rendering && (paint_top >= 184 || paint_top + PAINT_ROWS <= 32)) return;
    unsigned bank = track / 4;
    if (cache->title_cache != bank) {
        cache->title_cache = bank;
        paint_fill(0, 44, 240, 21, BACK);
        text(8, 51, "KEYS 1-8 MUTE  9-16 SOLO", 1, DIM, BACK);
    }
    for (unsigned i = 0; i < 16; ++i) {
        unsigned t = i & 7U, on = i < 8 ? v[t].muted : v[t].solo;
        uint32_t sig = on | ((t == track) << 1);
        if (sig == cache->tile_cache[i])
            continue;
        cache->tile_cache[i] = sig;
        unsigned x = 8 + (i % 4) * 57, y = 72 + (i / 4) * 29;
        uint16_t bg = on ? colors[t] : PANEL;
        paint_fill(x, y, 53, 25, bg);
        if (t == track)
            frame(x, y, 53, 25, TEXT);
        text(x + 7, y + 5, i < 8 ? "MUTE" : "SOLO", 1, on ? BACK : DIM, bg);
        number(x + 37, y + 5, t + 1, 1, 1, on ? BACK : colors[t], bg);
    }
}
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
    radio_line(0, 47, state, on ? GREEN : DIM);
    if (!ble_view) {
        static const char *const labels[] = {"K1 BLUETOOTH", "K2 BT AUDIO", "K3 BLE ROLE", "K4 CLOCK"};
        unsigned values[] = {on, (frame_radio_flags & GROOVE_RADIO_AUDIO) != 0, midi, groove_clock_mode()};
        const char *const names[] = {on ? "ON" : "OFF", values[1] ? "ON" : "OFF",
            midi == 2 ? "HOST" : midi == 1 ? "DEVICE" : "OFF", values[3] ? "MIDI" : "INT"};
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t sig = values[i] | (on << 8);
            if (cache->row_cache[i] == sig) continue;
            cache->row_cache[i] = sig;
            unsigned y = 61 + i * 30;
            paint_fill(8, y, 224, 20, BACK);
            text(8, y + 4, labels[i], 1, TEXT, BACK);
            text(146, y, names[i], 2, i && i < 3 && !on ? DIM : ACCENT, BACK);
        }
        const char *audio = !on || !values[1] ? "AUDIO OFF" : frame_radio.audio_playing ? "AUDIO PLAYING"
            : frame_radio.audio_connected ? "AUDIO CONNECTED" : "AUDIO WAITING FOR CONNECTION";
        radio_line(1, 184, audio, frame_radio.audio_playing ? GREEN : DIM);
        radio_line(2, 196, ble_action ? "> FORGET MIDI PAIRS" : "> MIDI DEVICE LIST (HOST)", ACCENT);
    } else {
        for (unsigned i = 0; i < FM1_BLE_MAX_DEVICES; ++i) {
            const struct fm1_ble_device *device = &frame_devices[i];
            uint32_t sig = signature(i == ble_device, device, sizeof *device);
            sig = sig * 33U + (i < status->count);
            if (!status->count) sig = sig * 33U + status->state;
            if (cache->row_cache[i] == sig) continue;
            cache->row_cache[i] = sig;
            unsigned y = 60 + i * 17;
            uint16_t bg = i < status->count && i == ble_device ? PANEL : BACK;
            paint_fill(8, y, 224, 15, bg);
            if (i < status->count) {
                number(12, y + 4, i + 1, 1, 1, ACCENT, bg);
                text(27, y + 4, device->name[0] ? device->name : "UNNAMED MIDI DEVICE", 1, TEXT, bg);
                char signal[12];
                decimal(signal, device->rssi, 1);
                text(204, y + 4, signal, 1, DIM, bg);
            } else if (!i)
                text(12, y + 4, status->state == FM1_BLE_SCANNING ? "LOOKING FOR MIDI DEVICES..."
                    : "NO DEVICES - SELECT SCAN BELOW", 1, DIM, bg);
        }
        static const char *const actions[] = {"> CONNECT SELECTED", "> SCAN", "> DISCONNECT", "> SETTINGS"};
        radio_line(1, 200, actions[ble_action], ACCENT);
    }
    const char *error = ble_forget == 1 ? "FORGET MIDI PAIRS? SAVE YES / HOME NO"
        : ble_forget == 2 && !status->pending && !status->error ? "PAIR AGAIN; FORGET FM1 ON PEER TOO"
        : groove_settings_status() ? "SETTINGS NOT SAVED - RETRY"
        : ble_result == -2 ? "BUSY - RETRY" : ble_result == -3 ? "RADIO AVAILABLE ON DEVICE"
        : ble_result ? "COMMAND UNAVAILABLE"
        : status->error <= FM1_BLE_LINK_LOST ? errors[status->error] : "";
    radio_line(3, 213, error, AMBER);
    radio_line(4, 229, "PRESETS ACTION  SAVE OK  HOME EXIT", DIM);
}
/* v6 SAVE: 16 slot squares (gold = stored, yellow = this project, white frame
 * = chosen), the working name with its letter cursor, the chosen slot's stored
 * name and the state. KNOB 1 slot, KNOB 2 cursor, KNOB 3 letter, SAVE writes. */
static __attribute__((noinline)) void project_draw(void)
{
    if (project_drawn_revision != groove_project_revision())
        project_refresh();
    if (!seq_slots_valid) {
        char tmp[13]; seq_slots_used = 0; seq_slots_valid = 1;
        for (unsigned i = 0; i < GROOVE_PROJECT_SLOTS; ++i)
            if (groove_project_info(i, tmp) == 0) seq_slots_used |= 1U << i;
    }
    char name[13];
    groove_project_name(name);
    unsigned current = groove_project_slot();
    if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44))) {
        uint32_t sig = signature(project_slot | (current << 8) | (project_reload << 16), project_stored, 12);
        sig = signature(sig, &seq_slots_used, sizeof seq_slots_used);
        sig = signature(signature(sig, &project_stored_result, sizeof project_stored_result), name, 12) + name_cursor;
        sig = signature(sig, &project_result, sizeof project_result) + project_operation * 7U + project_action * 131U;
        if (cache->title_cache != sig) {
            cache->title_cache = sig;
            paint_fill(0, 44, 240, 148, BACK);
            for (unsigned i = 0; i < 16; ++i) {
                unsigned x = 10 + (i % 8) * 28, y = 48 + (i / 8) * 26;
                unsigned used = i < GROOVE_PROJECT_SLOTS && (seq_slots_used >> i & 1U), cur = i == current;
                uint16_t fill = cur ? 0xfe87 : used ? 0x7b45 : V_NONE;
                rbox(x, y, 24, 22, 2, fill, i == project_slot ? TEXT : fill == V_NONE ? 0x5a64 : V_NONE);
                if (i == project_slot && fill != V_NONE) rbox(x + 1, y + 1, 22, 20, 2, V_NONE, TEXT);
                char num[12]; decimal(num, (int)i + 1, 1);
                lcd_center((int)x + 12, y + 5, num, &lcd12, fill == V_NONE ? V_DIM : BACK, fill == V_NONE ? BACK : fill);
            }
            lcd_text(10, 102, "NAME", &lcd11, V_DIM, BACK);
            lcd_text(10, 114, name, &lcd24, TEXT, BACK);
            char prefix[13]; memcpy(prefix, name, name_cursor); prefix[name_cursor] = 0;
            char letter[2] = {name[name_cursor] && name[name_cursor] != ' ' ? name[name_cursor] : 'W', 0};
            paint_fill(10 + lcd_width(&lcd24, prefix), 138, lcd_width(&lcd24, letter), 2, knob_colors[2]);
            char line[24];
            strcpy(line, "SLOT "); decimal(line + 5, (int)project_slot + 1, 1); strcat(line, ": ");
            strcat(line, project_stored_result < 0 ? "READ ERROR" : project_stored_result ? "EMPTY" : project_stored);
            lcd_text(10, 148, line, &lcd13, V_NAME, BACK);
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
            lcd_text(10, 170, status, &lcd13, project_reload || project_result ? V_ORANGE : 0x3f0f, BACK);
        }
    }
    char slot[12], cur[12], letter[2] = {name[name_cursor] && name[name_cursor] != ' ' ? name[name_cursor] : '_', 0};
    decimal(slot, (int)project_slot + 1, 1); decimal(cur, (int)name_cursor + 1, 1);
    knob(0, "SLOT", slot, colors[0]);
    knob(1, "CURSOR", cur, colors[1]);
    knob(2, "LETTER", letter, colors[2]);
    knob(3, "ACTION", project_action ? "LOAD" : "SAVE", colors[3]);
}
static __attribute__((noinline)) void name_draw(void)
{
    const uint8_t *patch = frame_patch;
    uint32_t sig = signature(name_cursor, patch + 145, 10);
    if (cache->middle_cache == sig)
        return;
    cache->middle_cache = sig;
    if (cache->title_cache != 1U) {
        cache->title_cache = 1U;
        text(8, 120, "SELECT CURSOR  K1 LETTER", 1, TEXT, BACK);
        text(8, 145, "SAVE STORES WITH PROJECT", 1, DIM, BACK);
        text(8, 171, "WEB IMPORT / EXPORT .SYX", 1, DIM, BACK);
        text(8, 225, "HOME EXIT  EDIT SOUND", 1, DIM, BACK);
    }
    paint_fill(8, 76, 120, 19, BACK);
    char name[11];
    for (unsigned i = 0; i < 10; ++i)
        name[i] = patch[145 + i] >= 32 && patch[145 + i] <= 126 ? (char)patch[145 + i] : '?';
    name[10] = 0;
    text(8, 76, name, 2, ACCENT, BACK);
    paint_fill(8 + name_cursor * 12, 93, 10, 2, ACCENT);
}
/* Rows 0..6: master inserts (EQ last). Rows 7..8: the engine's delay and
 * reverb, which every track feeds through its own sends. */
enum { FX_ROWS = 9, FX_DELAY = 7, FX_REVERB = 8, FX_VX = 7, FX_VY = 74, FX_VW = 226, FX_VH = 112 };
/* The effect's caption and name, drawn over its picture (no box behind them). */
static char fx_caption[24];
static const char *fx_title = "";
/* v6: curve colour per effect (knob palette index). */
static const uint8_t fx_ink[FX_ROWS] = {1, 2, 3, 3, 0, 1, 3, 2, 1};
static const char *fx_label(unsigned row, unsigned col)
{
    static const char *const delay[4] = {"TIME", "FEEDBK", "-", "RETURN"};
    static const char *const reverb[4] = {"-", "DECAY", "-", "RETURN"};
    return row < FX_DELAY ? mfx_label(row, col) : row == FX_DELAY ? delay[col] : reverb[col];
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
    if (row < FX_DELAY) {
        if (!col) { strncpy(out, mfx_type_name(row), 11); out[11] = 0; }
        else decimal(out, (int)mfx_value(row, col), 1);
    } else if (row == FX_DELAY) {
        if (col == 2) strcpy(out, "--");
        else decimal(out, e[col == 0 ? 1 : col == 1 ? 2 : 3], 1);
    } else {
        if (!col || col == 2) strcpy(out, "--");
        else decimal(out, e[col == 1 ? 0 : 3], 1);
    }
}
static int fx_raw(unsigned row, unsigned col) /* 0..127 for bars, -1 when unused */
{
    uint8_t e[4]; groove_effects_get(e);
    if (row < FX_DELAY) return (int)mfx_value(row, col);
    if (row == FX_DELAY) return col == 1 ? e[2] : col == 3 ? e[3] : -1;
    return col == 1 ? e[0] : col == 3 ? e[3] : -1;
}
static unsigned fx_active(unsigned row)
{
    uint8_t e[4]; groove_effects_get(e);
    return row < FX_DELAY ? mfx_active(row) : e[3] != 0;
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
    if (row == MFX_VERB) { /* the tail: DECAY = its length, alive (moving grain) */
        float pre = p2 * 0.25f, decay = 0.06f + 0.94f * p1;
        if (u < pre) return -1;
        float grain = 0.75f + 0.25f * vsin(u * 37.0f - t * 1.7f) * vsin(u * 11.0f + t * 0.9f);
        return -1 + 2 * (0.45f + 0.55f * mix) * grain * fexp2_ui(-(u - pre) * (type == 0 ? 6.0f : 4.0f) / decay);
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
    { /* reverb tail, alive like the master reverb's */
        float decay = 0.06f + 0.94f * e[0] / 127.0f;
        float grain = 0.75f + 0.25f * vsin(u * 37.0f - t * 1.7f) * vsin(u * 11.0f + t * 0.9f);
        return -1 + 2 * (0.3f + 0.7f * ret) * grain * fexp2_ui(-u * 4.0f / decay);
    }
}
/* The curve is computed once per change into a small cache (not again for
 * every LCD band), then drawn from the cache. */
static void fx_visual(void)
{
    static int16_t ys[FX_VW];
    static unsigned done = FX_VW, row_key = ~0U;
    static uint32_t want_sig;
    unsigned animated = fx_row == MFX_CHORUS || fx_row == MFX_PHASER || fx_row == MFX_RING || fx_row == MFX_VERB || fx_row == FX_REVERB;
    char v[4][12];
    for (unsigned c = 0; c < 4; ++c) fx_value(fx_row, c, v[c]);
    uint32_t sig = signature(fx_row | (animated ? (frame_time / 100U) << 4 : 0U), v, sizeof v);
    if (!paint_rendering) {
        if (sig != want_sig || row_key != fx_row) { want_sig = sig; row_key = fx_row; done = 0; }
        if (done < FX_VW) {
            float t = (float)(frame_time % 100000U) / 1000.0f;
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
    uint16_t ink = knob_colors[fx_ink[fx_row]], fillc = shade(ink, 0, 7);
    unsigned bars = fx_row == MFX_FILTER || (fx_row >= MFX_VERB && fx_row != MFX_EQ);
    paint_fill(FX_VX, FX_VY, FX_VW, FX_VH, 0x0841);
    paint_fill(FX_VX, FX_VY + FX_VH / 2, FX_VW, 1, 0x18c4);
    int prev = -1;
    for (unsigned x = 0; x < FX_VW; ++x) {
        int y = ys[x];
        if (bars) paint_fill(FX_VX + x, (unsigned)y, 1, FX_VY + FX_VH - (unsigned)y, fillc);
        int a = prev < 0 ? y : prev, lo = a < y ? a : y, hi = a < y ? y : a;
        paint_fill(FX_VX + x, (unsigned)lo, 1, (unsigned)(hi - lo + 1), ink);
        prev = y;
    }
    lcd_center(120, 78, fx_caption, &lcd11, V_DIM, 0x0841);
    lcd_center(120, 92, fx_title, lcd_width(&lcd26, fx_title) <= 220 ? &lcd26 : &lcd22, knob_colors[0], 0x0841);
}
/* ---- v7 SAMPLER REC / CHOP ---- */
static unsigned smp_ram, smp_ram_at; /* longest take the RAM allows, measured every 0.5 s */
static unsigned smp_bank_frames(void) { return groove_samples_room(smp_pad, 1) / 228U * 320U; }
/* What a take on this pad can really be: LENGTH, the free RAM and the bank's room. */
static unsigned smp_cap(void)
{
    unsigned c = smp_len * 8000U, b = smp_bank_frames();
    if (smp_ram < c) c = smp_ram;
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
    for (unsigned x = 0; x < w; ++x) {
        unsigned a = (unsigned)((uint64_t)span * x / w), b = (unsigned)((uint64_t)span * (x + 1) / w), peak = 0;
        if (a >= have) break;
        if (b > have) b = have;
        for (unsigned i = a; i < b; i += 4) { int v = rec_sample(i); unsigned m = (unsigned)(v < 0 ? -v : v); if (m > peak) peak = m; }
        unsigned h = peak * hh / 32768U;
        paint_fill(x0 + x, cy - h, 1, 2 * h + 1, c);
    }
}
/* The eight pads: lit = the target(s), outline = holds a sample, cursor = chosen. */
static void smp_pads(unsigned lit_first, unsigned lit_count, unsigned sel, uint16_t lit_c)
{
    for (unsigned i = 0; i < 8; ++i) {
        unsigned x = 6 + i * 29, lit = i >= lit_first && i < lit_first + lit_count, has = groove_samples_pad_frames(i) != 0;
        uint16_t bg = lit ? 0x3863 : 0x1083;
        rbox(x, 44, 26, 24, 3, bg, lit ? lit_c : has ? 0x5a64 : 0x2926);
        char num[4] = {(char)('1' + i), 0};
        lcd_center((int)x + 13, 49, num, &lcd13, lit ? lit_c : has ? TEXT : V_DIM, bg);
        if (i == sel) select_cursor(x, 44, 26, 24, 3, bg);
    }
}
static void smp_lamp(unsigned x, unsigned y, const char *label, const char *txt, uint16_t c, unsigned right)
{
    unsigned lw = lcd_width(&lcd13, label), tw = lcd_width(&lcd8, txt) + 8U, w = lw + 3U + tw;
    if (right) x -= w;
    unsigned ly = y + lcd13.digit_top + lcd13.digit_height / 2U - 4U;
    lcd_text((int)x, y, label, &lcd13, TEXT, BACK);
    rbox(x + lw + 3U, ly, tw, 9, 2, c, V_NONE);
    lcd_center((int)(x + lw + 3U + tw / 2U), ly + 1U, txt, &lcd8, BACK, c);
}
static void smp_draw(unsigned chop)
{
    unsigned st = rec_state(), fr = rec_frames(), lim = rec_limit(), n = smp_count();
    if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44))) {
        uint32_t sig = signature(chop | (st << 1) | (smp_pad << 4) | (smp_src << 8) | (smp_mon << 10) | (smp_sel << 11) |
                                 (smp_mode << 14) | (smp_slices << 15) | ((uint32_t)(smp_db + 64) << 18), smp_starts, sizeof smp_starts);
        sig = signature(sig, &smp_len, sizeof smp_len);
        uint32_t pulse = pulse_level();
        if (sig != cache->row_cache[0] || pulse != cache->row_cache[1]) {
            if (sig != cache->row_cache[0]) { cache->middle_shape = ~0U; cache->middle_cache = ~0U; }
            cache->row_cache[0] = sig; cache->row_cache[1] = pulse;
            paint_fill(0, 44, 240, 26, BACK);
            if (chop) smp_pads(smp_pad, n, smp_pad + smp_sel, knob_colors[2]);
            else smp_pads(smp_pad, 1, smp_pad, st == REC_RUN ? V_RED : knob_colors[0]);
        }
        if (!chop) {
            /* meter with the threshold mark, SRC / time / MON, the time bar */
            unsigned lv = rec_level(), mark = (unsigned)(38 + smp_db * 38 / 60); /* -60..0 dB over 38 segments */
            uint32_t msig = signature(lv | ((fr / 320U) << 8) | (st << 28), &smp_ram, sizeof smp_ram) ^ smp_bank_frames();
            if (msig != cache->middle_cache) {
                cache->middle_cache = msig;
                paint_fill(0, 70, 240, 38, BACK);
                int db = lv ? (int)(20.0f * vlog2((float)lv / 127.0f) / 3.3219f) : -99;
                unsigned on = db > -60 ? (unsigned)(38 + db * 38 / 60) : 0;
                for (unsigned i = 0; i < 38; ++i)
                    paint_fill(6 + i * 6, 72, 5, 8, i < on ? (i < 26 ? 0x3f0f : i < 33 ? 0xfe87 : V_RED) : 0x1082);
                paint_fill(6 + mark * 6 - 1, 69, 2, 14, knob_colors[1]);
                smp_lamp(6, 85, "SRC", smp_sources[smp_src], knob_colors[0], 0);
                smp_lamp(234, 85, "MON", smp_mon ? "ON" : "OFF", smp_mon ? knob_colors[3] : V_DIM, 1);
                char tm[24], b2[12];
                unsigned shown = st == REC_RUN || st == REC_DONE ? fr : 0, cap = st == REC_IDLE ? smp_cap() : lim;
                decimal(tm, (int)(shown / 32000U), 1); strcat(tm, "."); decimal(b2, (int)(shown % 32000U / 3200U), 1); strcat(tm, b2);
                strcat(tm, " / "); decimal(b2, (int)(cap / 32000U), 1); strcat(tm, b2); strcat(tm, ".");
                decimal(b2, (int)(cap % 32000U / 3200U), 1); strcat(tm, b2); strcat(tm, " S");
                lcd_center(120, 86, tm, &lcd13, st == REC_RUN ? V_RED : V_DIM, BACK);
                paint_fill(6, 100, 228, 4, 0x18c4);
                if (cap && shown) paint_fill(6, 100, (unsigned)((uint64_t)228 * shown / cap), 4, V_RED);
            }
            uint32_t wsig = (fr >> 10) | (st << 28) | ((frame_time / 250U & 1U) << 27) | ((smp_cap() < REC_RATE / 4) << 26);
            if (wsig != cache->middle_shape) {
                cache->middle_shape = wsig;
                rbox(6, 108, 228, 80, 2, 0x0841, 0x2926);
                paint_fill(10, 148, 220, 1, 0x18c4);
                if (st == REC_RUN || st == REC_DONE) {
                    smp_wave(10, 220, 148, 34, lim, V_RED);
                    fill_ellipse(160, 1190, 35, 35, V_RED);
                    lcd_text(24, 113, "REC", &lcd11, V_RED, 0x0841);
                } else {
                    unsigned th = 2U + (unsigned)((60 + smp_db) * 32 / 60); /* dB scale, as the meter */
                    for (unsigned x = 10; x < 230; x += 7) { paint_fill(x, 148 - th, 4, 1, knob_colors[1]); paint_fill(x, 148 + th, 4, 1, knob_colors[1]); }
                    if (st == REC_ARMED) {
                        if (!(frame_time / 250U & 1U)) fill_ellipse(160, 1190, 35, 35, 0xfe87);
                        lcd_text(24, 113, "ARMED", &lcd11, 0xfe87, 0x0841);
                    } else if (smp_cap() < REC_RATE / 4) { /* say why nothing can be recorded here */
                        unsigned bank = smp_bank_frames() < REC_RATE / 4;
                        lcd_center(120, 150, bank ? "SAMPLE MEMORY FULL" : "RAM FULL", &lcd13, V_RED, 0x0841);
                        lcd_center(120, 166, bank ? "CHOOSE A PAD WITH A LONGER SOUND" : "USE LIGHTER ENGINES ON TRACKS", &lcd11, V_DIM, 0x0841);
                    } else lcd_center(120, 160, "REC = START", &lcd13, V_DIM, 0x0841);
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
                paint_fill(0, 70, 240, 20, BACK);
                lcd_center(120, 72, "START", &lcd13, knob_colors[0], BACK);
                rbox(6, 90, 228, 98, 2, 0x0841, 0x2926);
                unsigned a = smp_starts[smp_sel], b = smp_sel + 1 < n ? smp_starts[smp_sel + 1] : total;
                unsigned xa = 10 + (unsigned)((uint64_t)220 * a / total), xb = 10 + (unsigned)((uint64_t)220 * b / total);
                paint_fill(xa, 92, xb - xa, 94, 0x2924);
                smp_wave(10, 220, 142, 40, total, knob_colors[0]);
                for (unsigned k = 0; k < n; ++k) {
                    unsigned x = 10 + (unsigned)((uint64_t)220 * smp_starts[k] / total), on = k == smp_sel;
                    paint_fill(x, 92, on ? 3 : 1, 94, on ? knob_colors[2] : TEXT);
                    char num[12]; decimal(num, (int)(smp_pad + k + 1), 1);
                    lcd_text((int)x + 4, 93, num, &lcd11, on ? knob_colors[2] : TEXT, on ? 0x2924 : 0x0841);
                }
            }
        }
    }
    if (!chop) {
        char db[12], len[12];
        decimal(db, smp_db, 1); strcat(db, "DB");
        decimal(len, (int)(smp_len / 4U), 1);
        strcat(len, smp_len % 4U == 1 ? ".25S" : smp_len % 4U == 2 ? ".5S" : smp_len % 4U == 3 ? ".75S" : "S");
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
                smp_ram = rec_ram_frames(); smp_ram_at = now | 1U;
                unsigned cap = smp_cap();
                if (cap < REC_RATE / 4) { notice = smp_bank_frames() < REC_RATE / 4 ? "BANK FULL: LONGER PAD" : "RAM FULL: LIGHTER ENGINES"; notice_until = now + 2500U; }
                else if (rec_arm(smp_src, smp_db, cap)) { notice = "RAM FULL: LIGHTER ENGINES"; notice_until = now + 2500U; }
            }
            return 1;
        }
        if (pressed) return 0; /* other buttons keep their usual meaning (and leave) */
        if (st == REC_IDLE && (!smp_ram_at || now - smp_ram_at > 500U)) { smp_ram = rec_ram_frames(); smp_ram_at = now | 1U; }
        if (st == REC_IDLE) {
            if (enc[0]) smp_pad = (unsigned)clamp((int)smp_pad + enc[0], 0, 7);
            if (enc[2]) smp_src = (unsigned)clamp((int)smp_src + (enc[2] > 0 ? 1 : -1), 0, REC_SOURCES - 1);
            if (enc[3]) smp_db = clamp(smp_db + (enc[3] > 0 ? 6 : -6), -60, 0);
            if (enc[4]) smp_len = (unsigned)clamp((int)smp_len + (enc[4] > 0 ? 1 : -1), 1, REC_MAX_FRAMES / 8000);
        }
        if (enc[5]) smp_mon = enc[5] > 0;
        return 1;
    }
    /* CHOP */
    /* SAVE: 1 notice shown -> 2 encode + start the bank job -> 3 one block a
     * tick. A failure keeps the encoded take, so SAVE simply tries again. */
    if (smp_saving == 1) { smp_saving = 2; return 1; }
    if (smp_saving >= 2) {
        static char msg[40];
        unsigned n = smp_count();
        int r = 0;
        if (smp_saving == 2) {
            if (!smp_encoded) {
                r = rec_encode(smp_starts, n, smp_items);
                for (unsigned k = 0; k < n && !r; ++k) {
                    char num[12]; decimal(num, (int)(smp_pad + k + 1), 1);
                    strcpy(smp_items[k].name, n == 1 ? "REC " : "SLICE "); strcat(smp_items[k].name, num);
                }
                if (!r) smp_encoded = 1;
            }
            if (!r) r = groove_samples_replace_begin(smp_pad, n, smp_items);
            if (!r) { smp_saving = 3; smp_done = 0; smp_total = 1; return 1; }
        } else {
            r = groove_samples_replace_step(&smp_done, &smp_total);
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
/* VARIATION pictures inside the 20b box (y 122..184): a fixed little
 * melody; the share of steps touched follows the value. */
static void var_picture(unsigned which, unsigned value, uint16_t c)
{
    static const uint8_t pitch[16] = {3, 3, 5, 2, 6, 3, 1, 4, 5, 2, 3, 6, 4, 2, 5, 3};
    uint16_t calm = 0x2cea;
    if (which == 2) { /* RATCHET: 1, 2, 3, 4 hits; the chosen count lit */
        for (unsigned i = 0; i < 4; ++i) {
            unsigned x = 16 + i * 56, on = i == value, n = i + 1, w = (36 - (n - 1) * 3) / n;
            rbox(x, 126, 48, 56, 3, on ? 0x2924 : 0x1083, on ? c : 0x2926);
            for (unsigned h = 0; h < n; ++h) rbox(x + 6 + h * (w + 3), 136, w, 36, 2, on ? c : calm, V_NONE);
        }
        return;
    }
    for (unsigned s = 0; s < 16; ++s) {
        unsigned x = 10 + s * 14, hit = (s * 37U + 11U) % 100U, y = 176 - pitch[s] * 7U;
        if (which == 0) { /* MUTATE: moved notes, the old place as a frame */
            if (hit < value) {
                unsigned ny = s & 1U ? y + 14U : y - 14U;
                frame(x, y, 10, 6, V_FAINT);
                paint_fill(x + 4, (ny < y ? ny : y) + 6, 2, (ny < y ? y - ny : ny - y) - 6 + 1, c);
                rbox(x, ny, 10, 6, 2, c, V_NONE);
            } else rbox(x, y, 10, 6, 2, calm, V_NONE);
        } else if (which == 1) { /* HUMAN: velocity bars wander */
            int j = (int)((s * 53U + 7U) % 41U) - 20, h = 30 + j * (int)value / 100;
            rbox(x, (unsigned)(182 - h), 10, (unsigned)h, 2, value && j ? c : calm, V_NONE);
        } else { /* CHANCE: value n/8 of the notes play (0 = all) */
            unsigned plays = !value || (s * 5U + 3U) % 8U < value;
            if (plays) rbox(x, 128, 10, 54, 2, calm, V_NONE);
            else rbox(x, 128, 10, 54, 2, 0x10a2, c);
        }
    }
}
/* v7 look shared by every effect-like page (agreed screen 20b): a strip of
 * names at y 48 (chosen: its colour + underline; `lit` bit set = TEXT, else
 * dim), then a dark box y 72..187 with a small caption and the name large. */
static void strip20(const char *const *names, unsigned n, unsigned sel, uint16_t c, uint32_t lit)
{
    paint_fill(0, 44, 240, 24, BACK);
    unsigned cw = n == 2 ? 60U : 228U / n; /* two tabs sit close in the centre */
    for (unsigned i = 0; i < n; ++i) {
        int cx = 120 + (int)(cw * (2U * i + 1U)) / 2 - (int)(cw * n) / 2;
        unsigned w = lcd_width(&lcd13, names[i]);
        lcd_center(cx, 48, names[i], &lcd13, i == sel ? c : (lit >> i & 1U) ? TEXT : V_DIM, BACK);
        if (i == sel) paint_fill((unsigned)(cx - (int)w / 2), 64, w, 2, c);
    }
}
static void head20(const char *caption, const char *name, uint16_t c)
{
    rbox(6, 72, 228, 116, 2, 0x0841, 0x2926);
    lcd_center(120, 78, caption, &lcd11, V_DIM, 0x0841);
    lcd_center(120, 92, name, lcd_width(&lcd26, name) <= 220 ? &lcd26 : &lcd22, c, 0x0841);
}
/* v7 MASTER FX: the nine effects in the 20b strip (white = on, dim = off),
 * the chosen effect's name large, its type or number small above, its curve
 * below in the same box, its controls on the knobs. */
static __attribute__((noinline)) void master_fx_draw(void)
{
    static const char *const shortn[FX_ROWS] = {"FLT", "DRV", "CHO", "PHA", "RNG", "REV", "EQ", "DLY", "SND"};
    static const char *const longn[FX_ROWS] = {"FILTER", "DRIVE", "CHORUS", "PHASER", "RING MOD", "REVERB", "EQ", "DELAY", "SENDS"};
    for (unsigned c = 0; c < 4; ++c) {
        char kv[12]; fx_value(fx_row, c, kv);
        knob(c, fx_label(fx_row, c), kv, colors[c]);
    }
    if (paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44)) return;
    uint32_t sig = fx_row;
    for (unsigned row = 0; row < FX_ROWS; ++row) sig |= fx_active(row) << (8 + row);
    char caption[24];
    if (!strcmp(fx_label(fx_row, 0), "TYPE")) fx_value(fx_row, 0, caption);
    else { decimal(caption, (int)fx_row + 1, 1); strcat(caption, " / 9"); }
    if (!fx_active(fx_row)) strcat(caption, "  OFF");
    strcpy(fx_caption, caption); fx_title = longn[fx_row];
    sig = signature(sig, caption, (unsigned)strlen(caption));
    if (cache->title_cache != sig) {
        cache->title_cache = sig;
        uint32_t lit = 0;
        for (unsigned i = 0; i < FX_ROWS; ++i) lit |= fx_active(i) << i;
        strip20(shortn, FX_ROWS, fx_row, knob_colors[0], lit);
        head20(caption, longn[fx_row], knob_colors[0]);
        cache->middle_shape = ~0U; /* the curve sits inside the new box */
    }
    fx_visual();
}
static void master_fx_edit(const int16_t *enc)
{
    for (unsigned c = 0; c < 4; ++c) {
        int d = enc[2 + c];
        if (!d) continue;
        if (c) d = fm_encoder_delta(2 + c, (int16_t)d); /* the type stays one step */
        if (fx_row < FX_DELAY) mfx_edit(fx_row, c, d);
        else if (fx_row == FX_DELAY) { if (c != 2) groove_effects_edit(c == 0 ? 1 : c == 1 ? 2 : 3, d); }
        else if (c == 1 || c == 3) groove_effects_edit(c == 1 ? 0 : 3, d);
    }
}
static __attribute__((noinline)) void effects_draw(void)
{
    const uint8_t *values = frame_effects;
    unsigned title = 1U + track + effects_page * 8U;
    if (cache->title_cache != title) {
        /* v7, as screen 20b: PRESETS turns the strip; the box shows the
         * four values as bars in the knob colours (numbers stay on the knobs). */
        static const char *const pages[3] = {"TRACK", "SHARED", "DELAY"};
        static const char *const names[3] = {"TRACK FX", "SHARED FX", "DELAY"};
        cache->title_cache = title;
        char caption[16];
        if (effects_page) strcpy(caption, "ALL TRACKS");
        else { strcpy(caption, "TRACK "); decimal(caption + 6, (int)track + 1, 1); }
        strip20(pages, 3, effects_page, knob_colors[0], 7U);
        head20(caption, names[effects_page], knob_colors[0]);
        memset(cache->row_cache, 0xff, sizeof cache->row_cache);
    }
    static const char *const shared[4] = {"REV TIME", "DLY TIME", "FEEDBACK", "RETURN"};
    static const char *const local[4] = {"REV SEND", "DLY SEND", "TRK VOL", "ALL RET"};
    static const char *const delay[4] = {"SYNC", "TIME MS", "BPM", "FEEDBACK"};
    static const char *const division[] = {"1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/8D", "1/4"};
    const char *const *labels = effects_page == 2 ? delay : effects_page ? shared : local;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned value = effects_page == 2 && !i ? values[1] : values[i], synced = groove_delay_synced(values[1]);
        const char *label = labels[i];
        char display[12];
        if (effects_page == 2 && !i) strcpy(display, synced ? "ON" : "OFF");
        else if (effects_page && i == 1 && synced) {
            label = "DIVISION"; strcpy(display, division[values[1] - GROOVE_DELAY_SYNC]);
        } else if (effects_page == 2 && i == 2 && frame_views[track].clock_mode) {
            strcpy(display, "MIDI");
        } else {
            if (effects_page && i == 1) value = (groove_delay_ticks() * 80U + 220U) / 441U;
            else if (effects_page == 2) value = i == 2 ? frame_views[track].tempo : values[2];
            decimal(display, (int)value, 1);
        }
        /* Knob cache covers labels as well as values when FREE/SYNC changes. */
        if (cache->row_cache[i] != (value | (synced << 16) | ((unsigned)frame_views[track].clock_mode << 17))) {
            cache->row_cache[i] = (value | (synced << 16) | ((unsigned)frame_views[track].clock_mode << 17));
            unsigned raw = effects_page == 2 && !i ? (synced ? 127U : 0U) : effects_page == 2 && i == 2 ? 64U : values[i];
            paint_fill(20, 126 + i * 14, 200, 6, 0x18c4);
            paint_fill(20, 126 + i * 14, (raw > 127U ? 127U : raw) * 200U / 127U, 6, knob_colors[i]);
        }
        knob(i, label, display, colors[i]);
    }
}
static void leds(const struct groove_view *v, unsigned visible)
{
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
    } else if (visible == STEPS && !writing) {
        uint64_t lane =
            groove_pad_count(track) != 0 ? groove_drum_steps(track, drum) : v->active_steps;
        for (unsigned i = 0; i < 16; ++i)
            if ((lane >> (first + i)) & 1U)
                bits |= 1U << white_keys[i];
        if (v->playing && (v->play_step & ~15U) == first)
            bits ^= 1U << white_keys[v->play_step & 15U];
    } else if (visible == PRESET_BROWSER) {
        bits = 1U << white_keys[preset_slot];
    } else if (visible == MIX) {
        for (unsigned t = 0; t < 8; ++t) {
            if (frame_views[t].muted)
                bits |= 1U << white_keys[t];
            if (frame_views[t].solo)
                bits |= 1U << white_keys[t + 8];
        }
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
    uint32_t changed = bits ^ led_cache;
    for (unsigned key = 0; key < 27; ++key)
        if (changed & (1U << key))
            fm1_led_set(key + 14, (bits >> key) & 1U);
    led_cache = bits;
    unsigned step_pages = (visible == STEPS && seq_page) || visible == ACTIONS;
    bits = ((step_pages ? first > 0 : octave < 0) << OCT_DOWN)
        | ((step_pages ? first + 16 < v->sequence_length : octave > 0) << OCT_UP)
        | ((visible == TRACKS) << HOME) | ((visible == EFFECTS || visible == MASTER_FX) << FX) | ((visible == LFOS) << LFO)
        | ((arp_enabled(track) != 0) << ARP)
        | ((visible == ACTIONS) << SCL)
        | ((visible == STEPS || visible == STEP_EDIT || visible == RHYTHM || visible == VARS || visible == CHAIN) << SEQ)
        | ((writing != 0 || (visible == SMP_REC && (rec_state() == REC_RUN || (rec_state() == REC_ARMED && blink)))) << REC)
        | ((v->playing != 0) << PLAY) | ((visible == MIX || visible == GLOBAL) << GLO)
        | ((visible == FM_EDIT || visible == TONE_EDIT || visible == FM_QUICK || visible == STEP_EDIT) << EDIT)
        | ((visible == FM_ENV) << ENV) | ((visible == FM_LFO) << LFO)
        | ((visible == PROJECT || visible == SOURCE || (visible == ACTIONS && action_confirm)) << SAVE);
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
static __attribute__((noinline)) void edit_page_draw(unsigned visible)
{
    if (paint_rendering && paint_top + PAINT_ROWS <= 44) return;
    if (!paint_rendering) {
        uint32_t sig = visible * 33U + action + (action_confirm << 8) + ((unsigned)(sel_armed + 1) << 10);
        sig = sig * 33U + cursor;
        sig = sig * 33U + source_preview;
        sig = sig * 33U + (unsigned)source_result;
        for (unsigned i = 0; i < 7; ++i) sig = sig * 33U + groove_step_setting(track, cursor, i);
        for (unsigned i = 0; i < 3; ++i) sig = sig * 33U + groove_sequence_setting(i);
        sig = sig * 33U + groove_sequence_count_in();
        sig = sig * 33U + frame_views[track].volume;
        if (notice) sig = signature(sig, notice, (unsigned)strlen(notice));
        if (visible == FM_QUICK) {
            sig = signature(sig, frame_patch, 156);
            sig = sig * 33U + frame_views[track].program;
            sig = sig * 33U + frame_views[track].modified;
            sig = signature(sig, &frame_controls, sizeof frame_controls);
        }
        if (visible == ARP_EDIT) {
            sig = sig * 33U + track + (arp_enabled(track) << 8);
            for (unsigned i = 0; i < ARP_PARAMS; ++i) sig = sig * 33U + arp_get(track, i);
            sig = sig * 33U + arp_cat_beat();
        }
        if (cache->middle_cache == sig) return;
        cache->middle_cache = sig;
    }
    paint_fill(0, 44, 240, 196, BACK); /* below the v6 top bar */
    memset(cache->foot_cache, 0xff, sizeof cache->foot_cache);
    if (visible == SOURCE) {
        text(8, 78, source_names[source_preview], 2, ACCENT, BACK);
        text(8, 120, "! SOUND OF THIS TRACK IS RESET", 1, AMBER, BACK);
        if ((source_preview == GROOVE_808 || source_preview == GROOVE_SAMPLE) != (groove_pad_count(track) != 0))
            text(8, 137, "! ITS STEPS WILL BE CLEARED", 1, AMBER, BACK);
        text(8, 170, "SAVE PROJECT FIRST: [SAVE]", 1, DIM, BACK);
        text(8, 186, source_result ? "NOT ENOUGH MEMORY" : "UNDO: [SEL] > UNDO / REDO", 1, source_result ? AMBER : DIM, BACK);
        hint_text(8, 231, "ALGO^ ENGINE  [SAVE] \x7f  [HOME] X", 1, DIM, BACK);
    } else if (visible == ACTIONS) {
        /* v7 drum: SELECT turns STEP / PAGE / TRACK / SOUND; the chosen one
         * large in a dark band, neighbours smaller. Knobs below are grey; the
         * one turned right lights in its colour, SAVE does it. */
        unsigned row = sel_row();
        static const int8_t drum_d[4] = {-1, 0, 1, 2};
        static const uint8_t drum_mid[4] = {64, 106, 148, 178};
        static const struct lcd_font *const drum_f[4] = {&lcd22, &lcd30, &lcd22, &lcd13};
        static const uint16_t drum_c[4] = {0xad16, TEXT, 0xad16, 0x6b2e};
        rbox(8, 106 - 21, 224, 42, 2, 0x18c4, V_NONE);
        for (unsigned k = 0; k < 4; ++k) {
            unsigned r = (row + SEL_ROWS + (unsigned)drum_d[k]) % SEL_ROWS;
            char label[16]; strcpy(label, sel_rows[r]);
            if (r < 2) { char num[12]; decimal(num, (int)(r ? cursor / 16U : cursor) + 1, 1); strcat(label, " "); strcat(label, num); }
            const struct lcd_font *f = drum_f[k];
            lcd_center(120, (unsigned)(drum_mid[k] - f->digit_top - f->digit_height / 2), label, f, drum_c[k], k == 1 ? 0x18c4 : BACK);
        }
        for (unsigned i = 0; i < 4; ++i) { /* bars always in knob colours; the action word lights */
            unsigned lit = sel_armed == (int)i, x = i * 60;
            paint_fill(x + 6, 197, 48, 2, knob_colors[i]);
            if (sel_grid[row][i] != 255)
                lcd_center((int)x + 30, 207, sel_items[row][i], &lcd16, lit ? knob_colors[i] : V_DIM, BACK);
        }
    } else if (visible == ARP_EDIT) {
        /* v6: piano roll of the pattern, then two mirrored blocks (HOLD /
         * PRESETS = mode, OCT- OCT+ / octaves) with the cat on grass between. */
        uint8_t held[27], pat[64]; unsigned hn = 0;
        for (unsigned k = 0; k < 27; ++k) if (key_note[k] != 0xff && key_track[k] == track && hn < 27) held[hn++] = key_note[k];
        unsigned pn = arp_pattern(track, held, hn, pat, 64), lo = 127, hi = 0;
        if (pn > 12) pn = 12;
        for (unsigned i = 0; i < pn; ++i) { if (pat[i] < lo) lo = pat[i]; if (pat[i] > hi) hi = pat[i]; }
        unsigned span = hi > lo ? hi - lo : 1;
        rbox(6, 46, 228, 70, 2, 0x0841, 0x2926);
        for (unsigned i = 0; i < pn; ++i)
            rbox(12 + i * 18, 104 - (pat[i] - lo) * 54U / span, 15, 6, 2, i == 0 ? TEXT : 0x3f0f, V_NONE);
        char mode[12], oct[12];
        arp_value_name(track, ARP_MODE, mode); decimal(oct, (int)arp_get(track, ARP_OCT) + 1, 1);
        label_lamp(6, 124, "HOLD", &lcd12, TEXT, arp_get(track, ARP_HOLD) != 0);
        lcd_text(6, 140, "PRESETS", &lcd17, 0xce1a, BACK);
        lcd_text(6, 160, mode, lcd_width(&lcd24, mode) <= 64 ? &lcd24 : &lcd17, TEXT, BACK);
        rbox(172, 124, 29, 12, 2, 0x2926, V_NONE); lcd_center(186, 125, "OCT-", &lcd9, TEXT, 0x2926);
        rbox(205, 124, 29, 12, 2, 0x2926, V_NONE); lcd_center(219, 125, "OCT+", &lcd9, TEXT, 0x2926);
        lcd_center(203, 140, oct, &lcd44, TEXT, BACK);
        paint_fill(70, 181, 98, 4, 0x1bc7);
        for (unsigned x = 70; x < 168; x += 3) {
            unsigned h = 3 + (x * 7) % 5;
            paint_fill(x, 181 - h, 1, h, x & 1U ? 0x3f0f : 0x2dcb);
        }
        unsigned beat = arp_cat_beat(); /* paws up every other beat, turns every two */
        cat_v6(96, 136, 3, beat && ((beat - 1U) & 1U), beat && ((beat - 1U) & 2U), 0, 0);
        static const unsigned knob_param[3] = {ARP_RATE, ARP_GATE, ARP_HUMAN};
        for (unsigned c = 0; c < 3; ++c) {
            char v[12]; arp_value_name(track, knob_param[c], v);
            knob(c, knob_param[c] == ARP_HUMAN ? "HUMAN" : arp_param_name(knob_param[c]), v, colors[c]);
        }
        knob(3, "HOLD", arp_get(track, ARP_HOLD) ? "ON" : "OFF", colors[3]); /* v7: HOLD on KNOB 4 */
    } else {
        /* v6 EDIT: bank and patch around the big voice name, then the TONE
         * row (on the knobs) and the ENV row below it. */
        char name[11]; memcpy(name, frame_patch + 145, 10); name[10] = 0;
        unsigned n = 10; while (n && name[n - 1] == ' ') name[--n] = 0;
        unsigned program = frame_views[track].program;
        char line[24], num[12];
        strcpy(line, "BANK "); line[5] = (char)('A' + program / 32U % 4U); line[6] = 0;
        lcd_center(120, 46, line, &lcd12, V_DIM, BACK);
        const struct lcd_font *nf = lcd_width(&lcd30, name) <= 220 ? &lcd30 : &lcd24;
        lcd_center(120, nf == &lcd30 ? 60U : 63U, name, nf, TEXT, BACK);
        strcpy(line, "PATCH "); decimal(num, (int)(program % 32U) + 1, 1); strcat(line, num); strcat(line, " / 32");
        if (frame_views[track].modified) strcat(line, " *");
        lcd_center(120, 94, line, &lcd12, frame_views[track].modified ? V_ORANGE : V_DIM, BACK);
        static const char *const row_names[2] = {"TONE", "ENV"};
        static const char *const labels[2][4] = {{"CHOIR", "FDBK", "TRANS", "LEVEL"}, {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"}};
        int values[2][4] = {{frame_patch[134] + 1, frame_patch[135], (int)frame_patch[144] - 24, frame_views[track].volume},
            {frame_controls.envelope[0], frame_controls.envelope[1], frame_controls.envelope[2], frame_controls.envelope[3]}};
        for (unsigned r = 0; r < 2; ++r) {
            unsigned y = 118 + r * 36, on = r == 0;
            if (on) rbox(4, y - 2, 232, 34, 2, 0x18c4, V_NONE);
            uint16_t bg = on ? 0x18c4 : BACK;
            lcd_text(10, y, row_names[r], &lcd11, on ? TEXT : V_DIM, bg);
            for (unsigned i = 0; i < 4; ++i) {
                char value[12];
                if (r == 0 && i == 2 && values[r][i] > 0) { value[0] = '+'; decimal(value + 1, values[r][i], 1); }
                else decimal(value, values[r][i], 1);
                lcd_center(30 + (int)i * 60, y + 12, labels[r][i], &lcd11, on ? TEXT : V_DIM, bg);
                lcd_center(30 + (int)i * 60, y + 21, value, &lcd12, on ? knob_colors[i] : V_FAINT, bg);
            }
        }
        static const char *const knob_labels[] = {"CHOIR", "FDBK", "TRANS", "LEVEL"};
        for (unsigned i = 0; i < 4; ++i) {
            char value[12];
            if (i == 2 && values[0][i] > 0) { value[0] = '+'; decimal(value + 1, values[0][i], 1); }
            else decimal(value, values[0][i], 1);
            knob(i, knob_labels[i], value, colors[i]);
        }
    }
}
static __attribute__((noinline, cold, minsize)) void boot_draw(void)
{
    if (cache->title_cache != 0) {
        cache->title_cache = 0;
        /* MPC FM-1 splash: our name large, our version, the base it is built on. */
        lcd_center(120, 46, "MPC FM-1", &lcd30, TEXT, BACK);
        lcd_center(120, 92, "V0.1 BETA", &lcd17, ACCENT, BACK);
        lcd_center(120, 118, "BASED ON FM1 OPEN " FM1_RELEASE_VERSION, &lcd11, DIM, BACK);
        lcd_center(120, 206, "FOR M-VAVE FM-1", &lcd11, DIM, BACK);
    }
    if (cache->middle_cache == boot_phase) return;
    cache->middle_cache = boot_phase;
    static const uint8_t height[8] = {8, 18, 30, 42, 42, 30, 18, 8};
    for (unsigned i = 0; i < 8; ++i)
        paint_fill(58 + 16 * i, 166 - height[i] / 2, 12, height[i],
                   i < boot_phase ? colors[i] : PANEL);
}
static void draw(const struct fm1_ble_status *ble);
/* SCOPE (v6): seven frequency bands as arcs around a dancing cat, bass inside
 * and thickest; a disco ball and drifting light spots behind; loud peaks throw
 * sparks. Master FX copies the output only while this page is open; the band
 * split runs here once per frame (512 samples x 6 two-pole low-passes). */
enum { SCOPE_N = 512, SCOPE_ARC = 64, SCOPE_SPARKS = 24, SCOPE_CX = 120, SCOPE_CY = 236 };
static uint8_t scope_level[7], scope_coat, scope_sleep;
static uint32_t scope_loud_ms, scope_seed = 1;
static int16_t scope_r[7][SCOPE_ARC + 1]; /* arc radius per angle, 1/10 px */
static float scope_cos[SCOPE_ARC + 1], scope_sin[SCOPE_ARC + 1];
static struct { int16_t x, y, vx, vy; uint8_t life, band; } scope_spark[SCOPE_SPARKS];
static const uint16_t scope_colors[7] = {0xfa49, 0xfcc7, 0xfe87, 0x3f0f, 0x2ed9, 0x4d1f, 0xc45f};
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
        unsigned e = (unsigned)((uint64_t)lvl * lvl * 256U / ((uint64_t)lvl * lvl + 2000U * 2000U));
        if (e > 255U) e = 255U;
        total += e;
        /* fast rise, slow fall */
        scope_level[b] = (uint8_t)(e >= scope_level[b] ? e : scope_level[b] - (scope_level[b] - e + 5U) / 6U);
    }
    if (total > 12U) scope_loud_ms = frame_time;
    scope_sleep = frame_time - scope_loud_ms > 1500U;
    float t = (float)(frame_time % 600000U) / 1000.0f;
    for (unsigned b = 0; b < 7; ++b) {
        float e = scope_sleep ? 0.0f : (float)scope_level[b] / 255.0f;
        for (unsigned k = 0; k <= SCOPE_ARC; ++k) {
            float ph = ((float)k * 1.5f * (0.25f + 0.12f * (float)b) + t * (3.0f + 1.5f * (float)b)) * 0.15915494f;
            float w = pic_sin(ph); if (w < 0) w = -w;
            int w10 = (int)(w * e * 110.0f);
            scope_r[b][k] = (int16_t)(640 + 70 * (int)b + w10);
            scope_seed = scope_seed * 1103515245U + 12345U;
            if (w10 > 95 && ((scope_seed >> 16) & 31U) < 2U)
                for (unsigned i = 0; i < SCOPE_SPARKS; ++i)
                    if (!scope_spark[i].life) {
                        int r = scope_r[b][k], v = 12 + (int)((scope_seed >> 8) & 15U);
                        scope_spark[i].x = (int16_t)(SCOPE_CX * 10 + (int)(scope_cos[k] * (float)r));
                        scope_spark[i].y = (int16_t)(SCOPE_CY * 10 + (int)(scope_sin[k] * (float)r));
                        scope_spark[i].vx = (int16_t)(scope_cos[k] * (float)v);
                        scope_spark[i].vy = (int16_t)(scope_sin[k] * (float)v);
                        scope_spark[i].life = 40; scope_spark[i].band = (uint8_t)b;
                        break;
                    }
        }
    }
    for (unsigned i = 0; i < SCOPE_SPARKS; ++i)
        if (scope_spark[i].life) {
            scope_spark[i].x = (int16_t)(scope_spark[i].x + scope_spark[i].vx);
            scope_spark[i].y = (int16_t)(scope_spark[i].y + scope_spark[i].vy);
            scope_spark[i].life = scope_spark[i].life > 3U && scope_spark[i].y >= 440 ? (uint8_t)(scope_spark[i].life - 3U) : 0;
        }
}
static void scope_draw(const struct groove_view *v)
{
    if (!paint_rendering) { paint_fill(0, 44, 240, 196, BACK); return; }
    if (paint_top + PAINT_ROWS <= 44) return;
    paint_fill(0, 44, 240, 196, BACK);
    const int top = (int)paint_top, bot = (int)paint_top + PAINT_ROWS;
    float t = (float)(frame_time % 600000U) / 1000.0f;
    /* drifting light spots */
    static const uint16_t spot[5] = {0xfbf6, 0x2ed9, 0xfe87, 0xffff, 0xc45f};
    for (unsigned k = 0; k < 18; ++k) {
        float an = (t * 0.5f + (float)k * 0.61f) * 0.15915494f, d = (float)(30 + (k * 37U) % 90U);
        int x = 120 + (int)(pic_sin(an + 0.25f) * d * 1.5f), y = 110 + (int)(pic_sin(an * 1.3f + (float)k * 0.15915494f) * d * 0.6f);
        if (y < 44 || y > 190 || y + 2 <= top || y >= bot || x < 2 || x > 236) continue;
        for (int j = 0; j < 4; ++j) paint_blend(x + (j & 1), y + (j >> 1), spot[k % 5], scope_sleep ? 5U : 22U);
    }
    /* disco ball on its string: 3 px mirror tiles that glint in turn */
    if (top < 82) {
        paint_fill(120, 44, 1, 9, 0x5aac);
        for (int i = -3; i <= 3; ++i)
            for (int j = -3; j <= 3; ++j) {
                if (i * i + j * j > 12) continue;
                float lit = (pic_sin(((float)i * 1.7f + (float)j * 2.3f + t * 4.0f) * 0.15915494f + 8.0f) + 1.0f) * 0.5f;
                unsigned g = 150U + (unsigned)(lit * 100.0f), bl = g + 30U > 255U ? 255U : g + 30U;
                paint_fill((unsigned)(119 + i * 4), (unsigned)(65 + j * 4), 3, 3, (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3)));
            }
    }
    /* seven bands, each a stroke of round dots between the arc points */
    for (unsigned b = 0; b < 7; ++b) {
        int rd = (320 - 35 * (int)b) / 20, sp = rd < 7 ? 7 : rd;
        int px = SCOPE_CX * 10 + (int)(scope_cos[0] * scope_r[b][0]), py = SCOPE_CY * 10 + (int)(scope_sin[0] * scope_r[b][0]);
        for (unsigned k = 1; k <= SCOPE_ARC; ++k) {
            int nx = SCOPE_CX * 10 + (int)(scope_cos[k] * scope_r[b][k]), ny = SCOPE_CY * 10 + (int)(scope_sin[k] * scope_r[b][k]);
            int lo = (py < ny ? py : ny) - rd, hi = (py < ny ? ny : py) + rd;
            if (hi >= top * 10 - 10 && lo < bot * 10 + 10) {
                int dx = nx - px, dy = ny - py, n = (int)isqrt_ui((unsigned)(dx * dx + dy * dy)) / sp + 1;
                for (int i = 0; i < n; ++i) fill_ellipse(px + dx * i / n, py + dy * i / n, rd, rd, scope_colors[b]);
            }
            px = nx; py = ny;
        }
    }
    for (unsigned i = 0; i < SCOPE_SPARKS; ++i)
        if (scope_spark[i].life) {
            int x = scope_spark[i].x / 10, y = scope_spark[i].y / 10;
            for (int j = 0; j < 4; ++j) paint_blend(x + (j & 1), y + (j >> 1), scope_colors[scope_spark[i].band], scope_spark[i].life);
        }
    /* sleeping: Z z rising above the cat, inside the inner arc */
    if (scope_sleep) {
        unsigned z = (frame_time / 80U) % 12U;
        lcd_text(SCOPE_CX + 28, (unsigned)(SCOPE_CY - 32) - z, "Z", &lcd10, TEXT, BACK);
        lcd_text(SCOPE_CX + 36, (unsigned)(SCOPE_CY - 38) - (z + 6U) % 12U, "Z", &lcd9, V_DIM, BACK);
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
        /* Large and plain: what happened, how to get back, the two holds. */
        lcd_center(120, 6, "SAFE MODE", &lcd30, AMBER, BACK);
        lcd_center(120, 42, why, &lcd16, TEXT, BACK);
        lcd_center(120, 62, "SONGS AND SETTINGS NOT LOADED", &lcd13, V_DIM, BACK);
        lcd_center(120, 84, "POWER OFF AND ON", &lcd22, TEXT, BACK);
        lcd_center(120, 108, "= NORMAL START", &lcd16, 0x3f0f, BACK);
        lcd_center(120, 134, "HOLD SAVE 2 S", &lcd17, 0x2ed9, BACK);
        lcd_center(120, 153, "FORGET FAILED STARTS", &lcd13, V_DIM, BACK);
        lcd_center(120, 174, "HOLD OCT- AND OCT+ 3 S", &lcd17, 0x2ed9, BACK);
        lcd_center(120, 193, "USB DOWNLOAD MODE", &lcd13, V_DIM, BACK);
    }
    uint32_t sig = safe_note ? signature(9U, safe_note, (unsigned)strlen(safe_note)) : 1U;
    if (sig == cache->middle_cache) return;
    cache->middle_cache = sig;
    paint_fill(0, 212, 240, 28, BACK);
    if (safe_note) lcd_center(120, 216, safe_note, &lcd16, AMBER, BACK);
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
static void draw_page_body(const struct fm1_ble_status *ble)
{
    unsigned visible = boot_active ? STARTUP : glo_down && glo_used ? MIX : page;
    if (visible != cache->shown_page) {
        cache->shown_page = visible;
        paint_fill(0, 0, 240, 240, BACK);
        if (visible != STARTUP && !v6_page(visible)) paint_fill(0, 29, 240, 1, PANEL);
        cache->header_cache = cache->title_cache = cache->middle_cache = cache->hint_cache = cache->hold_cache = cache->seq_cache = ~0U;
        memset(cache->foot_cache, 0xff, sizeof cache->foot_cache);
        memset(cache->row_cache, 0xff, sizeof cache->row_cache);
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
        static const char *const sections[] = {[TRACKS] = "MIXER", [STEPS] = "SEQUENCER", [GLOBAL] = "GLOBAL",
            [ACTIONS] = "ACTIONS", [ARP_EDIT] = "ARPEGGIATOR", [SOURCE] = "ENGINE", [FM_QUICK] = "EDIT", [MASTER_FX] = "MASTER FX", [FM_EDIT] = "FM LAB", [FM_ENV] = "AMP ENV", [FM_LFO] = "DX7 LFO", [CHAIN] = "CHAIN", [VARS] = "SEQUENCER", [SMP_REC] = "SAMPLER REC", [SMP_CHOP] = "SAMPLER CHOP", [LFOS] = "GLOBAL LFO", [SCOPE] = "SCOPE",
            [TONE_EDIT] = "SOUND", [MIX] = "MIX", [BLE] = "BLUETOOTH", [PROJECT] = "SAVE PROJECT", [NAME] = "PATCH NAME",
            [EFFECTS] = "TRACK FX", [STEP_EDIT] = "SEQUENCER", [RHYTHM] = "SEQUENCER", [PRESET_BROWSER] = "PRESETS"};
        char object[24];
        uint16_t object_color = TEXT;
        track_title(v, track, object);
        char title0[24]; strcpy(title0, object);
        int engine = -1; /* v7: engine icon + patch name instead of "N · ENGINE · NAME" */
        if (visible == GLOBAL || visible == MASTER_FX || visible == SCOPE) object[0] = 0;
        if (visible == LFOS) object[0] = 0;
        if (visible == CHAIN) strcpy(object, chain_on ? "PATTERN CHAIN ON" : "PATTERN CHAIN OFF");
        const char *section = sections[visible];
        char tone_title[16];
        if (v->type != GROOVE_DX7 && (visible == FM_ENV || visible == FM_LFO)) section = visible == FM_ENV ? "SOURCE ENV" : "TRACK LFO";
        if (visible == TONE_EDIT) {
            unsigned part = tone_part(), pg = tone_pages[track][tone_slot()];
            strcpy(tone_title, v->type == GROOVE_303 ? "303 EDIT " : v->type == GROOVE_PSG ? "PSG EDIT " : v->type == GROOVE_PLUCK ? "PLUCK "
                               : v->type == GROOVE_SAMPLE ? "SAMPLER " : "808 EDIT ");
            char num[12]; decimal(num, (int)pg + 1, 1); strcat(tone_title, num); strcat(tone_title, "/");
            decimal(num, (int)(groove_tone_count(track, part) + 3) / 4, 1); strcat(tone_title, num);
            section = tone_title;
        }
        if (visible == BLE && ble_view) section = "MIDI HOST";
        if (visible == EFFECTS) section = effects_page == 2 ? "DELAY / SEQ TAP" : effects_page ? "SHARED FX" : "TRACK FX";
        if (visible == MIX || visible == BLE || visible == PROJECT || (visible == EFFECTS && effects_page)) object[0] = 0;
        if ((visible == FM_EDIT || visible == FM_ENV || visible == FM_LFO) && v->type == GROOVE_DX7) {
            /* icon + "VOICE NAME  CHOIR n" (a DX7 algorithm = how the voices sing together) */
            char name[11], num[12]; memcpy(name, frame_patch + 145, 10); name[10] = 0;
            unsigned k = 10; while (k && name[k - 1] == ' ') name[--k] = 0;
            strcpy(object, name);
            if (visible == FM_EDIT) { strcat(object, "  CHOIR "); decimal(num, frame_patch[134] + 1, 1); strcat(object, num); }
            engine = GROOVE_DX7; strcpy(title0, object);
            if (visible == FM_EDIT && (fm_group == 1 || fm_group == 2 || fm_group == 6 || fm_group == 7 || fm_group > 0))
                section = fm_pages[fm_group].title;
        }

        if (visible == STEPS && seq_page) {
            static const char *const titles[8] = {"", "STEP DETAILS 1/2", "SEQ SETTINGS", "STEP DETAILS 2/2",
                "CLOCK / LENGTH", "MIDI ROUTING 1/2", "MIDI ROUTING 2/2", "TRACK ROLE / MIDI IN"};
            unsigned parameter = seq_page == 3 ? groove_step_setting(track, cursor, 5) : 0;
            strncpy(object, parameter ? groove_lock_parameter_name(track, parameter - 1) : titles[seq_page & 7U], 23);
            object[23] = 0;
        }
        if (notice && (int32_t)(frame_time - notice_until) >= 0) notice = NULL;
        const char *alert = notice ? notice : source_result ? "SOURCE MEMORY FULL"
            : visible == ACTIONS && action_confirm ? "REPLACE DATA?"
            : visible == GLOBAL || visible == MASTER_FX || visible == LFOS || visible == SCOPE ? NULL
            : v->clock_mode && v->clock_state != GROOVE_CLOCK_LOCKED ? "WAIT MIDI CLOCK"
            : writing && groove_sequence_count_in() ? "COUNT IN"
            : writing ? v->playing ? "LIVE REC" : "STEP REC"
            : visible == STEPS && v->sequence_length != v->next_length ? "NEXT LENGTH" : NULL;
        if (visible == PROJECT) groove_project_name(object);
        if (!strcmp(object, title0) && object[0]) { /* still the track title: draw it as icon + sound name */
            char sound[10]; track_sound_name(v, track, sound);
            if (engine < 0) { engine = v->type; strcpy(object, sound); }
        }
        if (alert) {
            engine = -1;
            strncpy(object, alert, 23); object[23] = 0;
            object_color = writing ? V_RED : V_ORANGE;
        }
        /* Pages SELECT turns: arrows only where it can still go. */
        unsigned arrows = 0;
        if (visible == FM_EDIT && op < 8) {
            unsigned cur = operator_page[track][op], max = op < 6 ? 5U : op == 6 ? 1U : 0U;
            arrows = (cur > 0) | ((cur < max) << 1);
        } else if (visible == FM_LFO && v->type == GROOVE_DX7)
            arrows = (lfo_page[track] > 0) | ((lfo_page[track] < 1) << 1);
        else if (visible == TONE_EDIT) {
            unsigned pg = tone_pages[track][tone_slot()], max = (groove_tone_count(track, tone_part()) - 1) / 4;
            arrows = (pg > 0) | ((pg < max) << 1);
        }
        top_bar(v, section, object, object_color, arrows, engine);
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
            int f = type == GROOVE_303 ? frame_patch[0] : type == GROOVE_PLUCK ? frame_patch[1]
                  : type == GROOVE_DX7 ? dx7_brightness_value(frame_patch) : -1;
            if (f < 0) strcpy(value, "--");
            else if (type == GROOVE_303) format_hz(value, (unsigned)(314.0f * fexp2_ui(2.9305886f * (float)f / 127.0f)));
            else if (type == GROOVE_PLUCK) { decimal(value, (f * 100 + 63) / 127, 1); strcat(value, "%"); }
            else decimal(value, f, 1);
            knob(1, type == GROOVE_DX7 ? "BRIGHT" : "FILTER", value, colors[1]);
        }
        {
            uint8_t sends[2]; groove_sends_get(track, sends);
            decimal(value, (int)((sends[0] * 100U + 7U) / 15U), 0);
            knob(2, "SEND", value, colors[2]);
        }
        knob(3, "STATE", v->solo ? "SOLO" : v->muted ? "MUTE" : "ON", colors[3]);
        return;
    }
    if (visible == SCOPE) { scope_draw(v); return; }
    if (visible == LFOS) {
        /* v7 (agreed 21a): LFO 1 | LFO 2 in the 20b strip, PRESETS switches.
         * In the box: rate and wave small, the target large, a bipolar depth
         * bar, then the live wave with the yellow four-point star. */
        unsigned l = lfo_sel;
        char name[12], rate[12];
        lfo_target_name(lfo_get(l, 0), name); lfo_rate_name(lfo_get(l, 1), rate);
        char depth[12]; int d = (int)lfo_get(l, 2) - 63;
        decimal(depth, d * 100 / 63, 1); if (d > 0) { memmove(depth + 1, depth, strlen(depth) + 1); depth[0] = '+'; }
        if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44))) {
            uint8_t key[8] = {(uint8_t)lfo_get(l, 0), (uint8_t)lfo_get(l, 1), (uint8_t)lfo_get(l, 2), (uint8_t)lfo_get(l, 3), (uint8_t)l};
            uint32_t sig = signature(0, key, sizeof key);
            if (sig != cache->row_cache[0]) {
                cache->row_cache[0] = sig;
                /* LFO 1 | LFO 2: boxed tabs in the centre, the chosen one filled. */
                paint_fill(0, 44, 240, 24, BACK);
                for (unsigned k = 0; k < 2; ++k) {
                    unsigned x = 71 + k * 51, on = k == l;
                    char tab[8] = "LFO 1"; tab[4] = (char)('1' + k);
                    rbox(x, 46, 48, 16, 3, on ? 0xfe87 : V_NONE, on ? V_NONE : 0x39a8);
                    lcd_center((int)x + 24, 48, tab, &lcd12, on ? BACK : V_DIM, on ? 0xfe87 : BACK);
                }
                label_lamp(8, 43, "LFO", &lcd13, TEXT, lfo_get(l, 0) && d);
                char caption[28];
                strcpy(caption, rate); strcat(caption, "  `  "); strcat(caption, lfo_wave_name(lfo_get(l, 3)));
                head20(caption, name, knob_colors[0]);
                paint_fill(14, 122, 212, 6, 0x2104);
                paint_fill(120, 120, 1, 10, V_DIM);
                if (d > 0) paint_fill(121, 123, (unsigned)d * 105U / 63U, 4, knob_colors[2]);
                else if (d < 0) { unsigned w = (unsigned)(-d) * 105U / 63U; paint_fill(120 - w, 123, w, 4, knob_colors[2]); }
                cache->middle_shape = ~0U;
            }
            sig = lfo_get(l, 3) | (l << 3) | ((frame_time / 100U) << 4); /* 10 frames/s: leaves time for the rest of the UI */
            if (sig != cache->middle_shape) {
                cache->middle_shape = sig;
                unsigned vy = 134, vh = 50, w = lfo_get(l, 3);
                paint_fill(8, vy - 2, 224, vh + 4, 0x0841);
                paint_fill(8, vy + vh / 2, 224, 1, 0x18c4);
                int prev = -1;
                for (unsigned x = 0; x < 224; ++x) {
                    int yy = (int)vy + (int)vh / 2 - (int)(lfo_shape(w, (float)x / 112.0f) * (float)(vh / 2 - 4));
                    int a2 = prev < 0 ? yy : prev, lo2 = a2 < yy ? a2 : yy, hi2 = a2 < yy ? yy : a2;
                    paint_fill(8 + x, (unsigned)lo2, 1, (unsigned)(hi2 - lo2 + 2), knob_colors[3]);
                    prev = yy;
                }
                unsigned px = 8 + (unsigned)(lfo_phase(l) * 112.0f);
                int py = (int)vy + (int)vh / 2 - (int)(lfo_output(l) * (float)(vh / 2 - 4));
                /* yellow four-point star, tips on the diagonals */
                int sx = (int)px * 10 + 5, sy = py * 10 + 5;
                for (int dx = -1; dx <= 1; dx += 2)
                    for (int dy = -1; dy <= 1; dy += 2)
                        fill_tri(sx + dx * 55, sy + dy * 55, sx + 12 * dy, sy - 12 * dx, sx - 12 * dy, sy + 12 * dx, 0xfe87);
                fill_ellipse(sx, sy, 18, 18, 0xfe87);
            }
        }
        knob(0, "TARGET", name, colors[0]);
        knob(1, "RATE", rate, colors[1]);
        knob(2, "DEPTH", depth, colors[2]);
        knob(3, "WAVE", lfo_wave_name(lfo_get(l, 3)), colors[3]);
        return;
    }
    if (visible == CHAIN) {
        /* v6: 16 rounded fields, white frame = cursor, red = playing, number = pattern. */
        if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44))) {
            for (unsigned i = 0; i < 16; ++i) {
                unsigned x = 8 + (i % 4) * 57, y = 47 + (i / 4) * 37;
                uint32_t sig = 1U | ((uint32_t)chain_slot[i] << 1) | ((i == chain_cursor) << 6) |
                               ((chain_on && i == chain_pos) << 7) | ((i == chain_cursor ? pulse_level() : 0U) << 8) |
                               ((uint32_t)chain_rep[i] << 14);
                if (sig == cache->tile_cache[i]) continue;
                cache->tile_cache[i] = sig;
                unsigned playing = chain_on && i == chain_pos;
                paint_fill(x - 2, y - 2, 56, 36, BACK);
                rbox(x, y, 52, 32, 2, chain_slot[i] ? 0x18c4 : V_NONE, 0x39a8);
                if (playing) play_cursor(x, y, 52, 32, 2);
                if (i == chain_cursor) select_cursor(playing ? x + 2 : x, playing ? y + 2 : y, playing ? 48 : 52, playing ? 28 : 32, 2,
                                                     chain_slot[i] ? 0x18c4 : BACK);
                char num[12];
                if (chain_slot[i]) decimal(num, chain_slot[i], 2); else strcpy(num, "-");
                lcd_text((int)x + 6, y + 5, num, &lcd22, chain_slot[i] ? (playing ? V_RED : TEXT) : V_FAINT,
                         chain_slot[i] ? 0x18c4 : BACK);
                if (chain_slot[i]) { /* repeats, bottom right: x1 dim */
                    num[0] = 'X'; decimal(num + 1, chain_rep[i] + 1, 1);
                    lcd_text((int)x + 47 - (int)lcd_width(&lcd12, num), y + 17, num, &lcd12, chain_rep[i] ? 0xfe87 : V_FAINT, 0x18c4);
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
    if (visible == VARS) {
        /* v7 (agreed 21b): the four variations in the 20b strip, the lit
         * (last turned) one large in the box with its picture. */
        static const char *const names[4] = {"MUTATE", "HUMAN", "RATCHET", "CHANCE"};
        unsigned sel = var_sel & 3U;
        if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44))) {
            uint8_t key[6] = {(uint8_t)sel, (uint8_t)track, (uint8_t)var_get(track, 0), (uint8_t)var_get(track, 1),
                              (uint8_t)var_get(track, 2), (uint8_t)var_get(track, 3)};
            uint32_t sig = signature(7U, key, sizeof key);
            if (sig != cache->row_cache[0]) {
                cache->row_cache[0] = sig;
                char cap[12]; decimal(cap, (int)sel + 1, 1); strcat(cap, " / 4");
                strip20(names, 4, sel, knob_colors[sel], 15U);
                head20(cap, names[sel], knob_colors[sel]);
                var_picture(sel, var_get(track, sel), knob_colors[sel]);
            }
        }
        for (unsigned k = 0; k < 4; ++k) { char val[12]; var_value(track, k, val); knob(k, names[k], val, colors[k]); }
        return;
    }
    if (visible == GLOBAL) {
        static const char *const rows[] = {"OUTPUT", "BPM", "SWING", "CLICK", "COUNT IN", "CLOCK", "LOW CUT", "BLUETOOTH", "MIDI ROUTING", "TRACK ROLE", "SCOPE"};
        static const char *const clicks[] = {"OFF", "REC", "ON"};
        char vals[11][12];
        if (mfx_output()) { vals[0][0] = '+'; decimal(vals[0] + 1, (int)mfx_output() * 3, 1); strcat(vals[0], "DB"); }
        else strcpy(vals[0], "0DB");
        if (v->clock_mode && !v->tempo) strcpy(vals[1], "WAIT"); else decimal(vals[1], v->tempo, 1);
        decimal(vals[2], groove_sequence_setting(0), 1); strcat(vals[2], "%");
        strcpy(vals[3], clicks[groove_sequence_setting(1) % 3]);
        strcpy(vals[4], groove_sequence_setting(2) ? "1 BAR" : "OFF");
        strcpy(vals[5], v->clock_mode ? "MIDI" : "INT");
        strcpy(vals[6], mfx_lowcut() ? "100HZ" : "OFF");
        strcpy(vals[7], ">"); strcpy(vals[8], ">"); strcpy(vals[9], ">"); strcpy(vals[10], ">");
        /* v6 drum: the selected row large in the middle, two neighbours each
         * side, smaller and dimmer. Value only here and on KNOB 1. */
        if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 44))) {
            char rowtext[11][24];
            uint32_t sig = glob_row;
            for (unsigned r = 0; r < 11; ++r) {
                strcpy(rowtext[r], rows[r]); strcat(rowtext[r], "  "); strcat(rowtext[r], vals[r]);
                sig = signature(sig, rowtext[r], (unsigned)strlen(rowtext[r]));
            }
            if (sig != cache->row_cache[0]) {
                cache->row_cache[0] = sig;
                paint_fill(0, 44, 240, 148, BACK);
                static const struct lcd_font *const drum_font[3] = {&lcd24, &lcd17, &lcd13};
                static const uint16_t drum_color[3] = {TEXT, 0xad16, 0x6b2e};
                static const uint8_t drum_y[5] = {54, 78, 106, 142, 170};
                rbox(8, 102, 224, 32, 2, 0x18c4, V_NONE);
                for (int d = -2; d <= 2; ++d) {
                    unsigned i = (glob_row + 11U + (unsigned)d) % 11U, ad = (unsigned)(d < 0 ? -d : d);
                    lcd_center(120, drum_y[d + 2], rowtext[i], drum_font[ad], drum_color[ad], ad ? BACK : 0x18c4);
                }
            }
        }
        knob(0, rows[glob_row], vals[glob_row], colors[0]);
        knob(1, "", "", colors[1]); knob(2, "", "", colors[2]); knob(3, "", "", colors[3]);
        /* Service line where knobs 2-4 would be: audio load (now / peak, % of
         * the block deadline), overloads and the raw battery ADC value. */
        struct fm1_render_stats st; groove_audio_stats(&st);
        unsigned dl = st.deadline_ticks ? st.deadline_ticks : 1U;
        unsigned now_pc = st.render_ticks / (dl / 100U + 1U), peak_pc = st.peak_ticks / (dl / 100U + 1U);
        char line[40], num[12];
        strcpy(line, "CPU "); decimal(num, (int)(now_pc > 999 ? 999 : now_pc), 1); strcat(line, num);
        strcat(line, "% PEAK "); decimal(num, (int)(peak_pc > 999 ? 999 : peak_pc), 1); strcat(line, num);
        strcat(line, "%");
        char line2[24];
        strcpy(line2, "OVR "); decimal(num, (int)(st.overloads % 1000U), 1); strcat(line2, num);
        strcat(line2, "  BAT "); decimal(num, (int)battery_adc, 1); strcat(line2, num);
        uint32_t hs = signature(signature(5U, line, (unsigned)strlen(line)), line2, (unsigned)strlen(line2));
        if (cache->hint_cache != hs && !(paint_rendering && (paint_top >= 233 || paint_top + PAINT_ROWS <= 199))) {
            cache->hint_cache = hs;
            paint_fill(66, 199, 174, 34, BACK);
            lcd_text(70, 202, line, &lcd11, V_DIM, BACK);
            lcd_text(70, 216, line2, &lcd11, V_DIM, BACK);
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
    else if (visible == MIX)
        mix_draw(views);
    else if (visible == TONE_EDIT || (visible == FM_ENV && v->type != GROOVE_DX7)) {
        tone_draw(patch);
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
        if (visible == STEPS && (seq_page == 1 || seq_page == 3)) {
            step_knob(col, seq_page == 3); continue;
        } else if (visible == STEPS && !seq_page) {
            static const char *const labels[] = {"STEP", "VELOCITY", "GATE", "NOTE"};
            struct groove_step c = frame_cells[cursor % GROOVE_STEPS];
            unsigned has = c.velocity != 0, pads = groove_pad_count(track) != 0;
            label = labels[col];
            if (!col) decimal(value, cursor + 1U, 2);
            else if (col == 1) { if (has || pads) decimal(value, has ? groove_step_setting(track, cursor, 0) : 0, 1); else strcpy(value, "--"); }
            else if (col == 2) {
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
                else if (groove_step_setting(track, cursor, 4)) strcpy(value, "TIE");
                else { decimal(value, g ? g == 15 ? 100 : (g * 100 + 8) / 16 : 50, 1); strcat(value, "%"); }
            } else if (pads) {
                label = "LAYER"; strncpy(value, groove_pad_name(track, drum), 11); value[11] = 0;
                color = pad_colors[drum % 12];
            }
            else note_name(has ? c.note : selected_note, value);
        } else if (visible == STEPS && seq_page == 2) {
            static const char *const labels[] = {"SWING", "CLICK", "COUNT IN", "LENGTH"};
            static const char *const clicks[] = {"OFF", "REC", "ON"};
            label = labels[col];
            if (!col) { decimal(value, groove_sequence_setting(0), 1); strcat(value, "%"); }
            else if (col == 1) strcpy(value, clicks[groove_sequence_setting(1)]);
            else if (col == 2) strcpy(value, groove_sequence_setting(2) ? "1 BAR" : "OFF");
            else decimal(value, v->next_length, 1);
        } else if (visible == STEPS && seq_page == 4) {
            static const char *const labels[] = {"CLOCK", "-", "LENGTH", "CLK IN"};
            label = labels[col];
            if (!col) strcpy(value, v->clock_mode ? "MIDI" : "INT");
            else if (col == 2) decimal(value, v->next_length, 1);
            else if (col == 1) strcpy(value, "--");
            else { static const char *const sources[] = {"AUTO", "USB", "BLE", "DIN"}; strcpy(value, sources[groove_midi_setting(track,6)]); }
        } else if (visible == STEPS && seq_page == 7) {
            static const char *const roles[] = {"NORMAL", "PLAY", "OFF"};
            label = col == 0 ? "ROLE" : col == 1 ? "IN CH" : "";
            if (!col) strcpy(value,roles[groove_track_role(track)]);
            else if (col == 1) decimal(value,groove_midi_input_channel(track)+1,1);
            else value[0] = 0;
        } else if (visible == STEPS && seq_page >= 5) {
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
        } else if (visible == MIX) {
            unsigned t = (track & ~3U) + col;
            static const char *const labels[8] = {"TRACK 1", "TRACK 2", "TRACK 3", "TRACK 4",
                                                  "TRACK 5", "TRACK 6", "TRACK 7", "TRACK 8"};
            label = labels[t];
            color = colors[t];
            decimal(value, views[t].volume, 1);
        } else if (visible == TONE_EDIT || (visible == FM_ENV && v->type != GROOVE_DX7)) {
            unsigned parameter = tone_parameter(col);
            const fm1_param_t *p = groove_tone_parameter(track, tone_part(), parameter);
            label = p ? p->name : "";
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
        if (visible == FM_EDIT && fm_group == 0 && op < 6) {
            const uint8_t *o = frame_patch + (5 - op) * 21;
            float lev = (float)o[16] / 99.0f;
            if (col == 0) wave_knob(0, label, value, o[18] ? (o[18] > 8 ? 8.0f : (float)o[18]) : 0.5f, 8.0f, 0.0f);
            else if (col == 1) wave_knob(1, label, value, 2.0f * (1.0f + (float)o[19] / 100.0f), 8.0f, 0.0f);
            else if (col == 2) wave_knob(2, label, value, 2.0f, 8.0f, ((float)o[20] - 7.0f) * 0.05f);
            else wave_knob(3, label, value, 2.0f, 10.0f * lev, 0.0f);
            continue;
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
                       : visible == STEPS && !seq_page ? "SELECT<> STEP  K1^ ROW  [EDIT] STEP"
                       : visible == STEPS && (seq_page == 1 || seq_page == 3) ? "EDIT BACK TO GRID  ALGO PAGE"
                       : visible == STEPS    ? "SEQ NEXT PAGE  ALGO PAGE"
                       : visible == MIX      ? "ALGO BPM  KEYS MUTE/SOLO  [GLO] BACK"
                       : visible == FM_EDIT  ? "BLACK VOICE  SELECT PAGE  EDIT BACK"
                       : visible == FM_ENV && v->type != GROOVE_DX7 ? "ENV BACK  SELECT TRACK"
                       : visible == FM_ENV   ? "PRESETS ENV ON/OFF  HOME TRACKS"
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
static void draw_page(const struct fm1_ble_status *ble)
{
    draw_page_body(ble);
    if (boot_active || page == SAFE || (paint_rendering && (paint_top > 194 || paint_top + PAINT_ROWS <= 193))) return;
    uint32_t since = 0;
    unsigned active = 1;
    if (glo_down && !glo_used) since = glo_ms;
    else if (edit_down && !edit_long) since = edit_ms;
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
static void draw(const struct fm1_ble_status *ble)
{
    unsigned visible = glo_down && glo_used ? MIX : page;
    frame_time = fm1_millis();
    mfx_scope_enable(visible == SCOPE);
    if (visible == SCOPE) scope_frame();
    if (boot_active) {
        boot_phase = (frame_time - boot_started) / 120U;
        if (boot_phase > 8) boot_phase = 8;
    }
    if (notice && (int32_t)(frame_time - notice_until) >= 0) notice = NULL;
    for (unsigned t = 0; t < 8; ++t) {
        unsigned n = groove_midi_activity(t);
        if (n != midi_seen[t]) { midi_seen[t] = n; midi_until[t] = frame_time + 120U; }
        midi_active[t] = (int32_t)(midi_until[t] - frame_time) > 0;
    }
    frame_ble = *ble;
    if (visible == BLE) {
        fm1_bluetooth_get_status(&frame_radio);
        frame_radio_flags = groove_settings_radio_flags();
        frame_radio_role = groove_settings_role_value();
        memset(frame_devices, 0, sizeof frame_devices);
        for (unsigned i = 0; i < ble->count && i < FM1_BLE_MAX_DEVICES; ++i)
            fm1_ble_get_device(i, &frame_devices[i]);
    }
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        groove_get_view(t, cursor, &frame_views[t]);
    if (visible == TRACKS)
        for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
            struct groove_step c[16];
            groove_sequence_read(t, mix_first(&frame_views[t], t), c, 16);
            for (unsigned i = 0; i < 16; ++i)
                frame_mix_vel[t][i] = groove_pad_count(t) ? 127U : c[i].velocity & 127U;
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
            frame_effects[0] = 0;
            frame_effects[1] = (uint8_t)((sends[0] * 100U + 7U) / 15U);
            frame_effects[2] = (uint8_t)((sends[1] * 100U + 7U) / 15U);
        }
    }
    memset(frame_patch, 0, sizeof frame_patch);
    memset(&frame_controls, 0, sizeof frame_controls);
    if (visible == TONE_EDIT || (visible == FM_ENV && groove_source_type(track) != GROOVE_DX7))
        groove_tone_copy(track, tone_part(), frame_patch);
    else if (visible == FM_LFO && groove_source_type(track) != GROOVE_DX7)
        groove_lfo_copy(track, frame_patch);
    else if ((visible >= FM_EDIT && visible <= FM_LFO) || visible == NAME || visible == FM_QUICK ||
             (visible == TRACKS && groove_source_type(track) == GROOVE_DX7))
        groove_fm_copy(track, frame_patch, &frame_controls);
    else if (visible == TRACKS)
        groove_tone_copy(track, 0, frame_patch);
    /* Merge new damage with unfinished bands. Input can replace a pending
     * frame; no stale queue of old encoder positions needs to be displayed. */
    paint_priority = 0;
    cache = &saved_cache;
    draw_page(&frame_ble);
}
static void paint_flush(void)
{
    uint32_t started = fm1_millis();
    paint_rendering = 1;
    cache = &band_cache;
    for (unsigned visited = 0; visited < PAINT_BANDS; ++visited) {
        int pending = fm1_screen_pending();
        if (pending) {
            if (pending < 0) saved_cache.shown_page = ~0U;
            break;
        }
        unsigned band = paint_next;
        /* Fresh knob/key feedback goes ahead of the rest of a page repaint.
         * Rotate within that set so continuous motion cannot starve its text.
         * Every other band is taken in plain order, so bands that animation
         * keeps out of the priority set (scope edges, a knob after the turn)
         * still get painted. */
        static unsigned fair;
        fair ^= 1U;
        if (paint_priority && fair)
            for (unsigned i = 0; i < PAINT_BANDS; ++i) {
                unsigned candidate = (paint_next + i) % PAINT_BANDS;
                if (paint_priority & (1U << candidate)) { band = candidate; break; }
            }
        paint_next = (band + 1U) % PAINT_BANDS;
        unsigned left = paint_left[band], width = paint_right[band] - left;
        if (left >= paint_right[band]) continue;
        paint_top = band * PAINT_ROWS;
        paint_x = left;
        paint_end = left + width;
        memset(cache, 0xff, sizeof *cache);
        draw_page(&frame_ble);
        /* Rasterize directly into the tightly packed DMA rectangle. */
        if (fm1_screen_submit(left, paint_top, width, PAINT_ROWS,
                            paint_pixels, width * PAINT_ROWS)) {
            saved_cache.shown_page = ~0U; /* Retry the whole page after a transport error. */
            break;
        }
        paint_left[band] = FM1_SCREEN_WIDTH;
        paint_right[band] = 0;
        paint_priority &= ~(1U << band);
        /* The DMA borrows paint_pixels. The next iteration/poll must observe
         * completion before composing another band into that same buffer. */
        if (fm1_millis() - started >= 4U) break;
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
static void toggle_step(unsigned step)
{
    struct groove_view v = {0};
    groove_get_view(track, step, &v);
    unsigned enabled =
        groove_pad_count(track) != 0 ? (v.step.drums & (1U << drum)) != 0 : v.step.velocity != 0;
    if (!groove_pad_count(track) && enabled) selected_note = v.step.note;
    groove_sequence_edit(track, step, selected_note, enabled ? 0 : 100);
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
    glo_down = glo_used = ble_device = ble_was_linked = ble_view = ble_action = 0;
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
    memset(encoder_direction, 0, sizeof encoder_direction);
    memset(encoder_speed, 0, sizeof encoder_speed);
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
    unsigned arp_tap = arp_down && !(controls->buttons & BUTTON(ARP)) && !arp_long;
    if (!(controls->buttons & BUTTON(ARP))) arp_down = arp_long = 0;
    if (pressed & BUTTON(ARP)) {
        arp_down = 1; arp_long = 0; arp_ms = now;
    }
    if (!(controls->buttons & BUTTON(EDIT))) edit_down = edit_long = 0;
    if (seq_tap && !(controls->buttons & BUTTON(SEQ))) { /* v7: SEQ tapped on the grid = VARIATION */
        seq_tap = 0;
        if (page == STEPS && !seq_page) { page = VARS; cache->shown_page = ~0U; goto controls_done; }
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
    if ((pressed & BUTTON(REC)) && page == OVERVIEW && !glo_down &&
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
                if (page == ACTIONS && action_confirm) action_confirm = 0;
                else if (page == BLE && ble_forget == 1) ble_forget = 0;
                else if (page == STEPS && seq_page) { seq_page = 0; cache->shown_page = ~0U; }
                else {
                    /* v7: one level up, never back through every page visited:
                     * a sub page goes to its main page, a main page to the mixer. */
                    unsigned sound = groove_source_type(track) == GROOVE_DX7 ? FM_QUICK : TONE_EDIT;
                    unsigned up = page == VARS || page == CHAIN || page == STEP_EDIT || page == RHYTHM ? STEPS
                                : page == FM_EDIT || page == FM_ENV || page == FM_LFO || page == SMP_REC || page == SMP_CHOP || page == NAME ? sound
                                : page == BLE || page == SCOPE ? GLOBAL : TRACKS;
                    if (page == SMP_REC || page == SMP_CHOP) { rec_free(); rec_listen(0, 0, 0); }
                    if (up == page) up = TRACKS;
                    seq_page = 0; page = up; back_n = 0; cache->shown_page = ~0U;
                }
                goto controls_done;
            }
        } else if (!home_long && !home_used && now - home_ms >= 600U) {
            home_long = 1; back_n = 0;
            page = TRACKS; cache->shown_page = ~0U;
            goto controls_done;
        }
    }
    /* FX: one press opens the effects (all of them in one list); again goes back. */
    if (pressed & BUTTON(FX)) {
        if (page == MASTER_FX || page == EFFECTS) page = music_page == page ? TRACKS : music_page;
        else page = MASTER_FX;
        cache->shown_page = ~0U;
        goto controls_done;
    }
    if (page == GLOBAL && !(pressed & ~BUTTON(SAVE))) {
        if (controls->encoders[0]) glob_row = (unsigned)(((int)glob_row + controls->encoders[0] % 11 + 11) % 11); /* round menu */
        int d = controls->encoders[2];
        if (d) {
            if (glob_row == 0) mfx_output_set((unsigned)clamp((int)mfx_output() + (d > 0 ? 1 : -1), 0, 4));
            else if (glob_row == 1) groove_tempo_edit(d);
            else if (glob_row >= 2 && glob_row <= 4) groove_sequence_setting_edit(glob_row - 2, d);
            else if (glob_row == 5) groove_settings_clock(d > 0 ? GROOVE_CLOCK_MIDI : GROOVE_CLOCK_INTERNAL);
            else if (glob_row == 6) mfx_lowcut_set(d > 0);
        }
        if (pressed & BUTTON(SAVE)) {
            if (glob_row == 7) { page = BLE; ble_view = ble_forget = ble_action = 0; ble_result = 0; }
            else if (glob_row == 10) page = SCOPE;
            else if (glob_row >= 8) { page = STEPS; seq_page = glob_row == 8 ? 5 : 7; step_follow = 0; cache->shown_page = ~0U; }
        }
        goto controls_done;
    }
    if (page == SCOPE && !pressed) {
        for (unsigned i = 0; i < 7; ++i)
            if (controls->encoders[i]) scope_coat = (uint8_t)((scope_coat + (controls->encoders[i] > 0 ? 1U : 3U)) % 4U);
        goto controls_done;
    }
    if (page == LFOS && !pressed) {
        if (controls->encoders[6]) lfo_sel = controls->encoders[6] > 0; /* PRESETS: LFO 1 / LFO 2 */
        for (unsigned i = 0; i < 4; ++i) lfo_edit(lfo_sel, i, i == 2 ? fm_encoder_delta(4, controls->encoders[4]) : controls->encoders[2 + i]);
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
    if (page == VARS && !pressed) {
        select_track(controls->encoders[0]);
        for (unsigned i = 0; i < 4; ++i)
            if (controls->encoders[2 + i]) { var_sel = (uint8_t)i; var_edit(track, i, controls->encoders[2 + i]); }
        goto controls_done;
    }
    if (page == MASTER_FX && !pressed) {
        if (controls->encoders[0]) {
            fx_row = (unsigned)clamp((int)fx_row + controls->encoders[0], 0, FX_ROWS - 1);
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
        writing = glo_down = glo_used = project_reload = edit_down = edit_long = 0;
        arp_down = arp_long = seq_down = rec_down = 0;
        groove_edit_record(0, track, cursor);
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
    if ((pressed & BUTTON(SEQ)) && ((controls->buttons & BUTTON(HOME)) ||
        (page == EFFECTS && effects_page == 2 && controls->buttons == BUTTON(SEQ)))) {
        groove_screen_tap(now);
        seq_down = edit_down = 0;
        if (controls->buttons & BUTTON(HOME)) page = music_page;
        goto controls_done;
    }
    if ((controls->buttons & (BUTTON(HOME) | BUTTON(GLO))) == (BUTTON(HOME) | BUTTON(GLO)) &&
        (pressed & (BUTTON(HOME) | BUTTON(GLO)))) {
        page = BLE; glo_down = glo_used = ble_view = ble_forget = ble_action = 0; ble_result = 0;
        goto controls_done;
    }
    if ((controls->buttons & (BUTTON(SEQ) | BUTTON(EDIT))) == (BUTTON(SEQ) | BUTTON(EDIT)) &&
        (pressed & (BUTTON(SEQ) | BUTTON(EDIT)))) {
        if (!release_keys()) return 0;
        blocked_keys |= controls->keys;
        page = ACTIONS; action_confirm = 0; edit_down = seq_down = 0;
        goto controls_done;
    }
    if (arp_down && !arp_long && now - arp_ms >= 600U) {
        arp_long = 1; if (page != ARP_EDIT) arp_page = 0; page = ARP_EDIT;
        goto controls_done;
    }
    /* Panel shortcuts precede modal editors. A menu must not swallow a
     * navigation key, transport control or the release of a held gesture. */
    if (glo_down && !(controls->buttons & BUTTON(GLO))) {
        unsigned tapped = !glo_used && now - glo_ms < 600U;
        glo_down = 0;
        blocked_keys |= controls->keys;
        if (tapped) page = page == MIX ? music_page : MIX;
    }
    if ((controls->buttons & (BUTTON(EDIT) | BUTTON(REC))) == (BUTTON(EDIT) | BUTTON(REC)) &&
        (pressed & (BUTTON(EDIT) | BUTTON(REC))) && groove_source_type(track) == GROOVE_DX7) {
        writing = rec_before_press;
        edit_down = glo_down = 0;
        page = NAME; name_cursor = 0;
        goto controls_done;
    }
    if (pressed & (BUTTON(HOME) | BUTTON(SEQ) | BUTTON(FX) | BUTTON(SCL) |
                   BUTTON(EDIT) | BUTTON(ENV) | BUTTON(LFO))) {
        glo_down = glo_used = 0;
        if (pressed & BUTTON(HOME)) {
            /* Acts on release (short) or after 600 ms (hold); see home_down. */
        } else if (pressed & BUTTON(SEQ)) {
            /* v7: SEQ tapped on the grid opens VARIATION (on release, so a hold
             * still means CHAIN); SEQ anywhere else returns to the grid. */
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
        } else if ((pressed & BUTTON(EDIT)) && page == STEPS && seq_page != 2 && seq_page != 4) {
            seq_page = seq_page ? 0 : 1; step_follow = 0; cache->shown_page = ~0U;
        } else if (pressed & BUTTON(EDIT)) {
            unsigned edit = groove_source_type(track) == GROOVE_DX7 ? FM_QUICK : TONE_EDIT;
            edit_down = 1; edit_long = 0; edit_ms = now;
            page = page == FM_QUICK || page == FM_EDIT || page == edit ? music_page : edit;
            advanced_edit = 0;
            if (page == TONE_EDIT) tone_pages[track][tone_slot()] = 0;
            if (page == TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
        } else if (pressed & BUTTON(ENV)) {
            /* v7: only DX7 has an envelope of its own; elsewhere ENV is inactive. */
            if (groove_source_type(track) == GROOVE_DX7) open_subpage(FM_ENV);
        } else { page = page == LFOS ? (music_page == LFOS ? TRACKS : music_page) : LFOS; cache->shown_page = ~0U; }
        goto controls_done;
    }
    if (edit_down && !edit_long && now - edit_ms >= 600U &&
        !(controls->buttons & (BUTTON(SEQ) | BUTTON(REC) | BUTTON(HOME)))) {
        edit_long = advanced_edit = 1;
        page = groove_source_type(track) == GROOVE_DX7 ? FM_EDIT : TONE_EDIT;
        goto controls_done;
    }
    if (arp_tap) {
        if (!release_keys()) return 0;
        blocked_keys |= controls->keys;
        groove_arp_set(track, 0); /* the engine arpeggiator stays off; ours runs instead */
        arp_set(track, !arp_enabled(track));
        notice = arp_enabled(track) ? "ARP ON" : "ARP OFF";
        notice_until = now + 2000U;
    }
    if (pressed & BUTTON(REC)) {
        writing ^= 1U;
        glo_down = 0;
        if (writing) { /* the chosen mode decides the transport */
            struct groove_view rv = {0}; groove_get_view(track, cursor, &rv);
            if (rec_mode && !rv.playing) (void)post(groove_sequence_setting(2) && groove_sequence_rewound() ? 0xfa : 0xfb, 0, 0, 1);
            else if (!rec_mode && rv.playing) (void)post(0xfc, 0, 0, 1);
        }
        if (writing && (page == BLE || page == PROJECT || page == PRESET_BROWSER ||
                       page == MIX || page == NAME || (page >= SOURCE && page <= RHYTHM) ||
                       (page == STEPS && (seq_page == 1 || seq_page == 3)))) {
            page = STEPS; seq_page = 0; step_follow = 1;
        }
        goto controls_done;
    }
    if (page != ACTIONS && !(page == STEPS && seq_page))
        octave = clamp(octave + !!(pressed & BUTTON(OCT_UP)) - !!(pressed & BUTTON(OCT_DOWN)), -2, 2);
    if ((pressed & BUTTON(SAVE)) && page != PROJECT && page != BLE &&
        page != SOURCE && page != ACTIONS) {
        project_return = page; page = PROJECT; project_result = 0;
        project_reload = project_operation = 0;
        project_slot = groove_project_slot(); project_factory = name_cursor = 0;
        glo_down = 0; project_refresh();
        goto controls_done;
    }
    if (pressed & BUTTON(GLO)) { /* GLO: global settings */
        page = page == GLOBAL ? (music_page == GLOBAL ? TRACKS : music_page) : GLOBAL;
        cache->shown_page = ~0U;
        goto controls_done;
    }
    if (glo_down || page == MIX) {
        if (glo_down && (now - glo_ms >= 600U || key_down || encoder_active(controls->encoders)))
            glo_used = 1;
        select_track(controls->encoders[0]);
        if (controls->encoders[1]) groove_tempo_edit(controls->encoders[1]); /* global BPM */
        for (unsigned i = 0; i < 4; ++i)
            if (controls->encoders[2 + i])
                groove_control_edit((track & ~3U) + i, 0, controls->encoders[2 + i], 0);
        for (unsigned i = 0; i < 16; ++i)
            if ((key_down & ~blocked_keys) & (1U << white_keys[i]))
                groove_mix_toggle(i & 7U, i >= 8);
        goto controls_done;
    }
    if (page == TRACKS && !glo_down && (controls->encoders[6] || controls->encoders[1])) {
        /* v7: PRESETS = engine, ALGORITHM = sound, in place; autosave later. */
        if (controls->encoders[6]) {
            blocked_keys |= controls->keys;
            if (!release_keys()) return 0;
            unsigned next = (unsigned)((int)groove_source_type(track) + controls->encoders[6] % GROOVE_SOURCES + GROOVE_SOURCES) % GROOVE_SOURCES;
            source_result = groove_source_set(track, next);
            if (!source_result) {
                sounds_forget(track);
                memset(tone_pages[track], 0, sizeof tone_pages[track]);
                drum_parts[track] = drum = 0;
                selected_note = groove_pad_count(track) ? groove_pad_note(track, 0) : 60;
            } else { notice = "ENGINE MEMORY FULL"; notice_until = now + 2000U; }
        }
        if (controls->encoders[1] && sounds_step(track, controls->encoders[1], now)) groove_control_edit(track, controls->encoders[1], 0, 0);
        autosave_at = now + 3000U; if (!autosave_at) autosave_at = 1;
        goto controls_done;
    }
    if (page == SOURCE) {
        int delta = controls->encoders[0] + controls->encoders[1] + controls->encoders[3] + controls->encoders[6];
        source_preview = (unsigned)((int)source_preview + delta % GROOVE_SOURCES + GROOVE_SOURCES) % GROOVE_SOURCES;
        if (pressed & BUTTON(SAVE)) {
            source_result = groove_source_set(track, source_preview);
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
            sel_armed = -1;
            action_confirm = 1; /* the lit knob was the choice: SAVE confirms */
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
            else if (action == 20 || action == 21) { /* V14 settings: MIDI routing (2 pages), track role */
                page = STEPS; seq_page = action == 20 ? 5 : 7; step_follow = 0; cache->shown_page = ~0U;
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
            /* v6: KNOB 1 (or SELECT / PRESETS) slot, KNOB 2 cursor, KNOB 3 letter. */
            int slot_move = controls->encoders[0] + controls->encoders[6] + controls->encoders[2];
            if (slot_move) {
                project_slot = (unsigned)clamp((int)project_slot + slot_move, 0, GROOVE_PROJECT_SLOTS - 1);
                project_reload = project_operation = 0;
                project_refresh();
            }
            name_cursor = (unsigned)clamp((int)name_cursor + controls->encoders[3], 0, 11);
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
            if (controls->encoders[4] && !project_factory) {
                char name[13];
                groove_project_name(name);
                groove_project_rename(name_cursor, edit_character(name[name_cursor], controls->encoders[4]));
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
        if (controls->encoders[2]) {
            uint8_t patch[156];
            if (!groove_fm_copy(track, patch, NULL))
                groove_fm_name_edit(track, name_cursor,
                    edit_character((char)patch[145 + name_cursor], controls->encoders[2]));
        }
    } else if (page == BLE) {
        if (encoder_active(controls->encoders)) ble_forget = 0;
        ble_action = (unsigned)clamp((int)ble_action + controls->encoders[6], 0, ble_view ? 3 : 1);
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
            if (pressed & BUTTON(SAVE)) {
                if (!ble_action) ble_result = fm1_ble_connect(ble_device);
                else if (ble_action == 1) {
                    ble_result = fm1_ble_scan();
                    if (!ble_result) ble_device = 0;
                } else if (ble_action == 2) ble_result = fm1_ble_disconnect();
                else { ble_view = ble_action = ble_forget = 0; ble_result = 0; }
            }
        }
    } else {
        if (page == STEPS && !seq_page && !glo_down) {
            /* v7: SELECT moves along the row, KNOB1 between rows (clockwise = down),
             * KNOB2 velocity, KNOB3 chance, KNOB4 note. ALGORITHM sets the
             * length (16/32/48/64); EDIT opens the selected step's details,
             * where ALGORITHM pages on through the other SEQ pages. */
            struct groove_view sv = {0};
            groove_get_view(track, cursor, &sv);
            int cols = controls->encoders[0];
            int rows = controls->encoders[2];
            if (rows || cols) { move_step(&sv, cols, rows); writing = 0; groove_get_view(track, cursor, &sv); }
            /* v7: PRESETS = this track's length (16/32/48/64), ALGORITHM = STEP / LIVE recording. */
            sequence_length_adjust(controls->encoders[6]);
            if (controls->encoders[1]) rec_mode = controls->encoders[1] > 0;
            /* The engine is chosen only on the mixer (it resets the track sound). */
            if (controls->encoders[3] && groove_step_setting_edit(track, cursor, 0, controls->encoders[3])) {
                notice = "EMPTY STEP"; notice_until = now + 2000U;
            }
            if (controls->encoders[4] && groove_source_type(track) == GROOVE_SAMPLE) {
                /* SAMPLER: KNOB 3 = this pad's LENGTH on the chosen step (parameter lock) */
                if (!(groove_drum_steps(track, drum) >> cursor & 1U)) { notice = "EMPTY STEP"; notice_until = now + 2000U; }
                else {
                    int r = step_length_edit(cursor, fm_encoder_delta(4, controls->encoders[4]));
                    if (r == -1) { notice = "ALL 16 LOCKS IN USE"; notice_until = now + 2000U; }
                    else if (r == -2) { notice = "STEP LOCKS ANOTHER PARAM"; notice_until = now + 2000U; }
                }
            }
            if (controls->encoders[4] && !groove_pad_count(track) && sv.step.velocity) {
                /* LENGTH: 1/16 .. full step, then TIE (holds until the next note). */
                unsigned tie = groove_step_setting(track, cursor, 4), g = groove_step_setting(track, cursor, 1);
                int pos = tie ? 15 : (g ? (int)g : 8) - 1;
                int next = clamp(pos + controls->encoders[4], 0, 15);
                if (next != pos) {
                    if (next == 15) { if (!tie) groove_step_setting_edit(track, cursor, 4, 1); }
                    else {
                        if (tie) groove_step_setting_edit(track, cursor, 4, -1);
                        groove_step_setting_edit(track, cursor, 1, (next + 1) - (int)g);
                    }
                }
            }
            if (controls->encoders[5] && groove_pad_count(track)) {
                drum = (unsigned)clamp((int)drum + controls->encoders[5], 0, (int)groove_pad_count(track) - 1);
                selected_note = groove_pad_note(track, drum);
            } else if (controls->encoders[5]) {
                if (sv.step.velocity) {
                    groove_sequence_edit(track, cursor, (unsigned)clamp((int)sv.step.note + controls->encoders[5], 0, 127),
                                         sv.step.velocity);
                    groove_get_view(track, cursor, &sv);
                    selected_note = sv.step.note;
                } else selected_note = (unsigned)clamp((int)selected_note + controls->encoders[5], 0, 127);
            }
            goto controls_done;
        }
        if (page != BLE) {
            unsigned tracks = page == TRACKS || page == OVERVIEW || page == STEPS || page == FM_QUICK || page == ARP_EDIT ||
                page == EFFECTS || page == FM_ENV || (page == FM_LFO && groove_source_type(track) != GROOVE_DX7);
            /* v7: in the sound editors SELECT always turns the pages. */
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
                    unsigned next = !controls->encoders[1] ? seq_page
                                  : seq_page == 1 || seq_page == 3 ? 4U - seq_page
                                  : seq_page == 5 || seq_page == 6 ? 11U - seq_page : seq_page;
                    struct groove_view v = {0}; groove_get_view(track, cursor, &v);
                    int move = controls->encoders[6] + (!next ? controls->encoders[3] : 0);
                    if (next != seq_page || move || oct_delta) {
                        move_step(&v, move, oct_delta);
                        if (move) writing = 0;
                    }
                    if (next != seq_page) { seq_page = next; cache->shown_page = ~0U; }
                    if (seq_page == 1 || seq_page == 3) step_adjust(seq_page == 3, controls->encoders, now);
                    else if (seq_page == 2) {
                        for (unsigned i = 0; i < 3; ++i) groove_sequence_setting_edit(i, controls->encoders[2 + i]);
                        sequence_length_adjust(controls->encoders[5]);
                    } else if (seq_page == 4) {
                        if (controls->encoders[2]) groove_settings_clock(controls->encoders[2] > 0
                            ? GROOVE_CLOCK_MIDI : GROOVE_CLOCK_INTERNAL);
                        sequence_length_adjust(controls->encoders[4]);
                        groove_settings_midi(track,6,controls->encoders[5]);
                        if (controls->encoders[3] && groove_clock_mode()) {
                            notice = "BPM SET BY EXT CLOCK"; notice_until = now + 2000U;
                        }
                    } else if (seq_page == 7) {
                        if (controls->encoders[2] || controls->encoders[3]) { blocked_keys |= controls->keys; if (!release_keys()) return 0; }
                        for (unsigned i=0;i<2;++i) if (controls->encoders[2+i]) {
                            int r=groove_settings_track(track,i,controls->encoders[2+i]);
                            notice=r ? "TRACK SETTING FAILED" : "TRACK SET, NOTES OFF";
                            notice_until=now+1500U;
                        }
                    } else if (seq_page >= 5) {
                        static const uint8_t parameters[2][4] = {{0,1,2,4},{3,5,7,8}};
                        for (unsigned i = 0; i < 4; ++i) if (controls->encoders[2+i]) {
                            groove_settings_midi(track,parameters[seq_page-5][i],controls->encoders[2+i]);
                            notice = "MIDI SET, NOTES OFF"; notice_until = now + 1500U;
                        }
                    }
                }
                if (page == ARP_EDIT) {
                    /* v7: K1 rate, K2 gate, K3 human, K4 hold, PRESETS mode, ALGORITHM octaves. */
                    static const unsigned knob_param[4] = {ARP_RATE, ARP_GATE, ARP_HUMAN, ARP_HOLD};
                    for (unsigned i = 0; i < 4; ++i) arp_edit(track, knob_param[i], controls->encoders[2 + i]);
                    arp_edit(track, ARP_MODE, controls->encoders[6]);
                    arp_edit(track, ARP_OCT, controls->encoders[1]);
                } else if (page == NAME) {
                    name_cursor = (unsigned)clamp((int)name_cursor + controls->encoders[0], 0, 9);
                    if (controls->encoders[2]) {
                        uint8_t name_patch[156];
                        if (!groove_fm_copy(track, name_patch, NULL))
                            groove_fm_name_edit(track, name_cursor,
                                edit_character((char)name_patch[145 + name_cursor], controls->encoders[2]));
                    }
                } else if (page == FM_QUICK) {
                    static const uint8_t parameters[] = {134, 135, 144};
                    dx7_algorithm_edit(track, controls->encoders[2]);
                    for (unsigned i = 1; i < 3; ++i) groove_fm_edit(track, parameters[i], controls->encoders[2 + i]);
                    groove_control_edit(track, 0, controls->encoders[5], 0);
                } else if (page == EFFECTS) {
                    unsigned next = (unsigned)clamp((int)effects_page + controls->encoders[6], 0, 2);
                    if (next != effects_page) { effects_page = next; cache->shown_page = ~0U; }
                    if (effects_page == 2) {
                        groove_effects_edit(4, controls->encoders[2]);
                        groove_effects_edit(1, controls->encoders[3]);
                        groove_tempo_edit(controls->encoders[4]);
                        groove_effects_edit(2, controls->encoders[5]);
                    } else if (effects_page) {
                        for (unsigned i = 0; i < 4; ++i)
                            groove_effects_edit(i, controls->encoders[2 + i]);
                    } else {
                        for (unsigned i = 0; i < 2; ++i)
                            groove_sends_edit(track, i, controls->encoders[2 + i]);
                        groove_control_edit(track, 0, controls->encoders[4], 0);
                        groove_effects_edit(3, controls->encoders[5]);
                    }
                } else if (page == FM_LFO && groove_source_type(track) != GROOVE_DX7) {
                    for (unsigned i = 0; i < 4; ++i)
                        groove_lfo_edit(track, i, i < 2 ? fm_encoder_delta(2 + i, controls->encoders[2 + i]) : controls->encoders[2 + i]);
                } else if (page == TONE_EDIT || (page == FM_ENV && groove_source_type(track) != GROOVE_DX7)) {
                    if (page == TONE_EDIT && groove_source_type(track) == GROOVE_303)
                        for (unsigned i = 8; i < 10; ++i)
                            if ((key_down & ~blocked_keys) & (1U << edit_keys[i]))
                                groove_mono_set(track, i == 8);
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
                    if (part_turn || direction) tone_active = 0; /* new part / page: start from KNOB 1 */
                    for (unsigned col = 0; col < 4; ++col)
                        if (controls->encoders[2 + col]) {
                            tone_active = col;
                            unsigned parameter = tone_parameter(col);
                            const fm1_param_t *p =
                                groove_tone_parameter(track, tone_part(), parameter);
                            if (p)
                                groove_tone_edit(
                                    track, tone_part(), parameter,
                                    p->names ? controls->encoders[2 + col]
                                             : fm_encoder_delta(2 + col, controls->encoders[2 + col]));
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
                                                 fm_encoder_delta(2 + col, controls->encoders[2 + col]));
                            else
                                groove_fm_edit(track, fm_parameter(col),
                                               fm_encoder_delta(2 + col, controls->encoders[2 + col]));
                        }
                } else if (page == TRACKS) {
                    /* v6 order: KNOB1 volume; KNOB2 brightness/filter; KNOB3 FX send (reverb+delay);
                     * KNOB4 cycles ON > MUTE > SOLO; ALGORITHM is the tempo. */
                    if (controls->encoders[2]) groove_control_edit(track, 0, fm_encoder_delta(2, controls->encoders[2]), 0);
                    if (controls->encoders[3]) track_filter_edit(track, fm_encoder_delta(3, controls->encoders[3]));
                    if (controls->encoders[4]) {
                        int d = fm_encoder_delta(4, controls->encoders[4]);
                        groove_sends_edit(track, 0, d);
                        groove_sends_edit(track, 1, d);
                    }
                    if (controls->encoders[5]) mix_cycle(track, controls->encoders[5]);
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
                            groove_control_edit(track, 0, controls->encoders[3], 0);
                    } else if (controls->encoders[2])
                        groove_control_edit(track, 0, controls->encoders[2], 0);
                    if (page == OVERVIEW && controls->encoders[5]) mix_cycle(track, controls->encoders[5]);
                    else if (controls->encoders[5]) {
                        groove_tempo_edit(controls->encoders[5]);
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
    if (sounds_tick(now)) { autosave_at = now + 3000U; if (!autosave_at) autosave_at = 1; }
    /* Autosave writes Flash, which briefly silences audio: never while playing. */
    if (autosave_at && (int32_t)(now - autosave_at) >= 0 && !frame_views[0].playing) {
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
        glo_down = glo_used = action_confirm = ble_forget = project_reload = 0;
        if (!(pressed & BUTTON(EDIT))) edit_down = 0;
        if (page != STEPS) seq_down = 0;
    }
    if (page != OVERVIEW) rec_down = 0;
    if (page != old_page && page != ARP_EDIT) arp_down = arp_long = 0;
    /* Manual selection must not be overwritten by the recording cursor on
     * the next poll. Entering sequence editing finishes the recording take. */
    if (page == ACTIONS || page == STEP_EDIT || (page == STEPS && (seq_page == 1 || seq_page == 3))) writing = 0;
    groove_edit_record(writing, track, cursor);
    if ((pressed & BUTTON(PLAY)) && !play_handled) {
        struct groove_view view = {0};
        groove_get_view(track, cursor, &view);
        uint8_t command = view.playing ? 0xfc : (writing && groove_sequence_setting(2) && groove_sequence_rewound() ? 0xfa : 0xfb);
        if (!post(command, 0, 0, 1)) return 0;
    }
    unsigned mode = navigation_mode();
    if (page != FM_EDIT || glo_down)
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
    if (page == STEPS && !writing && !glo_down) {
        struct groove_view view = {0};
        groove_get_view(track, cursor, &view);
        unsigned first = step_first(&view);
        if (!seq_page) {
            /* Grid: a key writes its note into the selected step; the same
             * note again clears it. Drum keys toggle their pad on that step. */
            for (unsigned key = 0; key < 27; ++key) {
                if (!((key_down & keys) & (1U << key))) continue;
                unsigned note = groove_key_note(track, key, octave);
                if (note > 127) continue;
                struct groove_view cell = {0};
                groove_get_view(track, cursor, &cell);
                if (groove_pad_count(track)) {
                    unsigned c = cursor; toggle_step(c); /* current layer, whatever key */
                } else if (cell.step.velocity && cell.step.note == note) {
                    groove_sequence_edit(track, cursor, note, 0);
                } else {
                    groove_sequence_edit(track, cursor, note, cell.step.velocity ? cell.step.velocity : 100);
                    selected_note = note;
                }
                step_follow = 0;
            }
        } else for (unsigned i = 0; i < 16; ++i)
            if ((key_down & keys) & (1U << white_keys[i])) {
                if (seq_page == 1 || seq_page == 3) { cursor = first + i; step_follow = 0; }
                else toggle_step(first + i);
            }
    }
    unsigned layer_keys = page == STEPS && !seq_page && !glo_down && groove_pad_count(track) != 0;
    for (unsigned key = 0; key < 27; ++key) {
        unsigned note = layer_keys ? groove_pad_note(track, drum) : groove_key_note(track, key, octave);
        if (key_note[key] != 0xff &&
            (mode || !(keys & (1U << key)) ||
             (key_note[key] != note && !(project_keys & (1U << key))) || key_track[key] != track)) {
            if (!key_arp[key] && !post((uint8_t)(0x80 + groove_midi_input_channel(key_track[key])), key_note[key], 0, 3))
                return 0;
            key_note[key] = 0xff; key_arp[key] = 0;
            project_keys &= ~(1U << key);
        }
        if (!mode && note < 128 && (keys & (1U << key)) && key_note[key] == 0xff) {
            /* With our arpeggiator on, held keys feed it instead of sounding. */
            key_arp[key] = arp_enabled(track) && !groove_pad_count(track);
            key_seq[key] = (uint8_t)(++key_counter);
            if (!key_arp[key] && !post((uint8_t)(0x90 + groove_midi_input_channel(track)), (uint8_t)note, 100, 3))
                return 0;
            key_note[key] = (uint8_t)note;
            key_track[key] = (uint8_t)track;
            selected_note = note;
            if (groove_pad_count(track) != 0)
                drum = (unsigned)groove_pad_index(track, note);

        }
    }
    if (page != TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
    {
        /* LFOs, arpeggiator and sequence chain run here, every control tick. */
        static uint32_t last_tick;
        unsigned dt = last_tick ? now - last_tick : 0;
        last_tick = now;
        struct groove_view tv = {0};
        groove_get_view(0, 0, &tv);
        struct lfo_cc cc[8];
        unsigned n = lfo_tick(dt > 200U ? 200U : dt, tv.tempo, tv.playing, !writing, cc, 8);
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
        unsigned m = arp_tick(track, groove_midi_input_channel(track), now, tv.tempo, tv.playing, held, hn, ev, 6);
        for (unsigned i = 0; i < m; ++i) (void)post(ev[i].status, ev[i].note, ev[i].velocity, 3);
        var_tick(now, var_post);
        /* Chain: when track 1 wraps to its first step (after its repeats), load the next sequence. */
        if (chain_on && tv.playing) {
            unsigned stepnow = tv.play_step;
            if (stepnow < chain_prev_step && ++chain_loops > chain_rep[chain_pos]) {
                chain_loops = 0;
                unsigned next = chain_pos;
                for (unsigned i = 1; i <= 16; ++i) { unsigned c = (chain_pos + i) % 16U; if (chain_slot[c]) { next = c; break; } }
                if (chain_slot[next] && next != chain_pos) {
                    unsigned keep = page, keep_music = music_page;
                    chain_pos = (uint8_t)next;
                    preset_slot = chain_slot[next] - 1U; preset_bank = preset_current_bank; preset_refresh();
                    preset_load(1, now);
                    page = keep; music_page = keep_music; cache->shown_page = ~0U;
                }
            }
            chain_prev_step = (uint8_t)stepnow;
        } else chain_prev_step = 0;
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
    unsigned master_changed = groove_master_update(controls.knob);
    if (master_changed && (!notice || notice == master_notice || (int32_t)(now - notice_until) >= 0)) {
        unsigned level = fm1_engine_output_level();
        if (level) {
            memcpy(master_notice, "MASTER ", 7);
            decimal(master_notice + 7, (int)((level * 100U + 63U) / 127U), 1);
            memcpy(master_notice + strlen(master_notice), " / 100", sizeof " / 100");
        }
        else memcpy(master_notice, "MASTER MUTED", sizeof "MASTER MUTED");
        notice = master_notice;
        notice_until = now + 1200U;
    }
    if (now - health_at >= 500U) {
        struct fm1_engine_stats health;
        fm1_engine_stats_read(&health);
        struct fm1_render_stats audio;
        groove_audio_stats(&audio);
        struct fm1_audio_stats dma;
        fm1_audio_stats_read(&dma);
        unsigned adapted = audio.adapted;
        if (health.underruns != health_underruns || dma.overruns != health_dma) {
            notice = "SOUND OVERLOAD!"; notice_until = now + 2500U;
        } else if (health.midi_dropped != health_dropped) {
            notice = "MIDI IN OVERFLOW"; notice_until = now + 2500U;
        } else if (groove_midi_dropped() != health_output) {
            notice = "MIDI OUT OVERFLOW"; notice_until = now + 2500U;
        } else if (adapted > health_adapted) {
            notice = "CPU BUSY: VOICES CUT"; notice_until = now + 2500U;
        }
        health_output = groove_midi_dropped();
        health_adapted = adapted;
        health_dma = dma.overruns;
        health_at = now; health_underruns = health.underruns; health_dropped = health.midi_dropped;
    }
    unsigned input_changed = master_changed || controls.buttons != previous_buttons || controls.keys != previous_keys ||
                             encoder_active(controls.encoders);
    /* The logo never blocks control/audio or consumes the first gesture.
     * Ignore the initial MASTER ADC pickup when deciding whether to skip. */
    if (boot_active && (now - boot_started >= 1200U || controls.buttons || controls.keys ||
        encoder_active(controls.encoders) || (master_changed && now - boot_started >= 150U))) {
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
    writing = groove_edit_recording();
    if (writing) cursor = groove_edit_cursor();
    unsigned error = groove_edit_error();
    if (error && !notice) { notice = error == 1 ? "REC STOP: MIDI FULL" : "CHORD LIMIT REACHED"; notice_until = now + 2000U; }
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
    if (page == FM_EDIT && !glo_down) {
        for (unsigned i = 0; i < 10; ++i)
            boundary |= (key_down & (1U << edit_keys[i])) != 0;
    } else if (page == TONE_EDIT && groove_source_type(track) == GROOVE_303 && !glo_down) {
        boundary |= (key_down & ((1U << edit_keys[8]) | (1U << edit_keys[9]))) != 0;
    } else if (page == TRACKS || page == OVERVIEW || page == STEPS || glo_down ||
               ((page == TONE_EDIT || page == FM_ENV) && groove_pad_count(track) != 0))
        boundary |= key_down != 0;
    if (boundary) {
        if ((glo_down && !(controls.buttons & BUTTON(GLO))) ||
            (rec_down && !(controls.buttons & BUTTON(REC)))) {
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
    groove_edit_record(writing, track, cursor);
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
    if (target > ARP_EDIT || target == PROJECT || page == SAFE) return -1;
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
    glo_down = glo_used = edit_down = seq_down = arp_down = arp_long = rec_down = 0;
    if (parameter_page(target)) {
        if (!parameter_page(page)) subpage_return = page;
    } else advanced_edit = !quick && (target == FM_EDIT || target == TONE_EDIT);
    page = target == OVERVIEW ? TRACKS : target;
    if (target == STEP_EDIT || target == RHYTHM) { page = STEPS; seq_page = target == STEP_EDIT ? 1 : 0; step_follow = 0; }
    else if (page == STEPS) seq_page = 0;
    if (page == ARP_EDIT) arp_page = 0;
    if (page == ACTIONS || (page == STEPS && seq_page == 1)) {
        writing = 0;
        groove_edit_record(0, track, cursor);
    }
    if (quick && page == TONE_EDIT) tone_pages[track][tone_slot()] = 0;
    if (page == FM_ENV && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
    action_confirm = name_cursor = 0;
    if (page == OVERVIEW || page == STEPS || page == TRACKS) music_page = page;
    if (page == SOURCE) { source_preview = groove_source_type(track); source_result = 0; }
    if (page == PRESET_BROWSER) { project_result = 0; preset_refresh(); }
    if (page == BLE) { ble_view = ble_forget = ble_action = 0; ble_result = 0; }
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
        ble_view, advanced_edit, glo_down && glo_used,
        (page == STEP_EDIT || (page == STEPS && seq_page)) ? cursor : 0, seq_page, arp_page};
    return signature(2166136261U, context, sizeof context);
}
unsigned groove_screen_web_mask(void)
{
    unsigned mask = 0;
    if (glo_down && glo_used) return 0;
    if (page == TRACKS || page == OVERVIEW || page == STEPS || page == MIX || page == FM_EDIT ||
        page == FM_ENV || page == FM_LFO || page == TONE_EDIT || page == EFFECTS ||
        page == STEP_EDIT || page == RHYTHM || page == FM_QUICK || page == ARP_EDIT) {
        for (unsigned i = 0; i < 4; ++i)
            if (web_knobs[i][0][0] && strcmp(web_knobs[i][0], "-") &&
                web_knobs[i][1][0] && strcmp(web_knobs[i][1], "--")) mask |= 1U << i;
    }
    if ((page == TRACKS || page == OVERVIEW || (page == STEPS && !seq_page)) && groove_clock_mode()) mask &= ~8U;
    if (page == STEPS && seq_page == 4 && groove_clock_mode()) mask &= ~2U;
    if (page == EFFECTS && effects_page == 2 && groove_clock_mode()) mask &= ~4U;
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
    if (glo_down && glo_used) return web_targets;
    struct groove_view v; groove_get_view(track, cursor, &v);
    unsigned type = groove_source_type(track);
    if (page != BLE && page != PROJECT && page != PRESET_BROWSER && !v.clock_mode)
        web_target(6, 4, 65, 23, 1, 0, track, "BPM", v.tempo, NULL);
    if (page == TRACKS) {
        for (unsigned t = 0; t < 8; ++t) {
            struct groove_view row; groove_get_view(t, cursor, &row);
            char label[16] = "T1 LEVEL"; label[1] += t;
            web_target(70, TRK_Y + t * TRK_PITCH, 8, TRK_H, 2, 0, t, label, row.volume, NULL);
        }
    } else if (page == STEPS) {
        web_target(180, 42, 56, 14, 8, 0, track, "LOOP LENGTH", v.next_length, NULL);
        web_target(64, 15, 110, 15, 6, 1, track, "SEQ PAGE", seq_page + 1, seq_pages[seq_page]);
    } else if (page == ARP_EDIT) {
        web_target(6, 176, 222, 16, 6, 1, track, "ARP PAGE", arp_page + 1, arp_page ? "SWING / HOLD" : "NOTES");
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
        web_target(6, 59, 228, 18, 6, 6, track, "FX PAGE", effects_page, effects_page == 2 ? "DELAY" : effects_page ? "SHARED" : "TRACK");
    } else if (page == MIX) {
        for (unsigned i = 0; i < 16; ++i) {
            struct groove_view row; groove_get_view(i & 7U, cursor, &row);
            unsigned on = i < 8 ? row.muted : row.solo;
            char label[16] = "T1 MUTE"; label[1] += i & 7U;
            if (i >= 8) memcpy(label + 3, "SOLO", 4);
            web_target(8 + i % 4 * 57, 72 + i / 4 * 29, 53, 25, 7, i >= 8, i & 7U, label, on, on ? "ON" : "OFF");
        }
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
    else if (action == 2) groove_control_edit(owner, 0, delta, 0);
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
    groove_edit_record(0, track, cursor);
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
static void algorithm_draw(unsigned algorithm, unsigned muted)
{
    const uint8_t *layout = algorithms[algorithm & 31U];
    unsigned left = 239, right = 0;
    /* Draw wires first so every destination terminates behind an operator. */
    for (unsigned i = 0; i < 6; ++i) {
        unsigned column = layout[i * 2] >> 4, row = layout[i * 2] & 15;
        int x = 19 + (int)column * 36, y = 151 - (int)row * 15;
        unsigned branch = layout[i * 2 + 1] >> 4, feedback = layout[i * 2 + 1] & 15;
        int bottom = row ? y + 15 : 169;
        if (!row) {
            if ((unsigned)x < left)
                left = (unsigned)x;
            if ((unsigned)x > right)
                right = (unsigned)x;
        }
        if (branch == 0)
            line(x, y + 12, x, bottom, DIM);
        else {
            line(x, y + 12, x, y + 15, DIM);
            int first = branch == 1 ? x - 36 : branch == 2 ? x + 36 : x;
            int last = branch == 4 ? x + 72 : branch >= 2 ? x + 36 : first;
            line(first < x ? first : x, y + 15, last > x ? last : x, y + 15, DIM);
            for (int dest = first; dest <= last; dest += 36)
                line(dest, y + 15, dest, bottom, DIM);
        }
        if (feedback) {
            int end_y = y + (int)(feedback - 1) * 15 + 12;
            line(x, end_y, x + 17, end_y, AMBER);
            line(x + 17, end_y, x + 17, y - 4, AMBER);
            line(x + 17, y - 4, x, y - 4, AMBER);
            line(x, y - 4, x, y, AMBER);
        }
    }
    line((int)left, 169, (int)right, 169, DIM);
    text(104, 172, "OUT", 1, DIM, BACK);
    for (unsigned i = 0; i < 6; ++i) {
        unsigned x = 8 + (layout[i * 2] >> 4) * 36, y = 151 - (layout[i * 2] & 15) * 15;
        uint16_t bg = muted & (1U << i) ? PANEL : i == op ? colors[track] : DIM;
        paint_fill(x, y, 22, 12, bg);
        number(x + 8, y + 3, i + 1, 1, 1, muted & (1U << i) ? DIM : BACK, bg);
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
}
void groove_screen_extras2_put(const uint8_t in[54])
{
    if (!chain_on) for (unsigned i = 0; i < 16; ++i) chain_rep[i] = in[i] <= 15 ? in[i] : 0;
    var_state_put(in + 16);
}
#ifdef FM1_HOST_TEST
/* Tests: jump straight to the master effects page. */
void groove_screen_test_fx(unsigned row) { page = MASTER_FX; fx_row = row % FX_ROWS; cache->shown_page = ~0U; }
#endif
