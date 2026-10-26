/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "recorder.h"
#include "sea_rec.h"
#include <stdlib.h>
#include <string.h>

/* Shared with the audio ISR: the control task changes state only while the
 * ISR ignores it (IDLE / DONE), or by a single word store. */
static volatile unsigned state, frames, limit, level, listening, muted, source;
static volatile int threshold;
static uint8_t *buffer; /* the take: one mu-law byte per frame (half the RAM of 16 bit) */
/* G.711 mu-law. The bank stores ~5 bits a sample (SEA5), so 8-bit mu-law
 * costs nothing audible and halves the memory a take needs. */
FM1_AUDIO_CODE static uint8_t ulaw(int32_t x)
{
    unsigned sign = x < 0 ? 0x80U : 0U;
    unsigned m = (unsigned)(x < 0 ? -x : x);
    if (m > 32635U) m = 32635U;
    m += 0x84U;
    unsigned e = 7;
    for (unsigned mask = 0x4000U; e && !(m & mask); mask >>= 1) --e;
    return (uint8_t)~(sign | (e << 4) | ((m >> (e + 3)) & 0x0fU));
}
int rec_sample(unsigned i)
{
    if (!buffer || i >= frames) return 0;
    unsigned u = (uint8_t)~buffer[i], e = (u >> 4) & 7U;
    int m = (int)((((u & 0x0fU) << 3) + 0x84U) << e) - 0x84;
    return u & 0x80U ? -m : m;
}
/* 44.1 kHz -> 32 kHz: a one-pole low-pass, then linear interpolation. */
static int32_t lp, prev;
static uint32_t phase; /* Q16 position between prev and the next input */
enum { STEP = (uint32_t)((44100.0 / 32000.0) * 65536.0 + 0.5) };

int rec_arm(unsigned src, int db, unsigned n)
{
    rec_free();
    if (n > REC_MAX_FRAMES) n = REC_MAX_FRAMES;
    if (n < REC_RATE / 4) n = REC_RATE / 4;
    /* The engines share the heap: take what fits, shortening the take step by
     * step (down to 0.25 s), and leave room for saving it later. */
    for (;;) {
        buffer = malloc(n);
        if (buffer) {
            void *room = malloc(12288U); /* a margin for USB audio and the system */
            if (room) { free(room); break; }
            free(buffer); buffer = NULL;
        }
        if (n <= REC_RATE / 4) return -1;
        n = n * 3U / 4U;
        if (n < REC_RATE / 4) n = REC_RATE / 4;
    }
    /* -60..0 dB as a 16-bit amplitude: 32767 * 10^(db / 20), stepwise. */
    int a = 32767;
    for (int d = 0; d > db; d -= 6) a /= 2;
    threshold = a; source = src < REC_SOURCES ? src : 0; limit = n; frames = 0;
    lp = prev = 0; phase = 0;
    state = REC_ARMED;
    return 0;
}
void rec_stop(void)
{
    if (state == REC_RUN) state = frames ? REC_DONE : REC_IDLE;
    else if (state == REC_ARMED) state = REC_IDLE;
}
void rec_free(void)
{
    state = REC_IDLE;
    free(buffer); buffer = NULL; frames = 0;
}
unsigned rec_state(void) { return state; }
unsigned rec_frames(void) { return frames; }
unsigned rec_limit(void) { return limit; }
unsigned rec_level(void) { return level; }
unsigned rec_ram_frames(void)
{
    if (buffer) return limit;
    for (unsigned n = REC_MAX_FRAMES; n >= REC_RATE / 4; n = n * 3U / 4U) {
        void *a = malloc(n);
        if (!a) continue;
        void *room = malloc(12288U);
        free(a);
        if (room) { free(room); return n; }
    }
    return 0;
}
void rec_listen(unsigned src, unsigned on, unsigned mute)
{
    if (state != REC_ARMED && state != REC_RUN) source = src < REC_SOURCES ? src : 0;
    listening = on; muted = on && mute;
    if (!on) level = 0;
}
FM1_AUDIO_CODE unsigned rec_input_muted(void) { return muted; }

FM1_AUDIO_CODE void rec_capture(const int32_t *usb, const int16_t *bt, const int32_t *mix)
{
    unsigned st = state;
    if (!listening && st != REC_ARMED && st != REC_RUN) return;
    unsigned src = source, peak = 0;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        int32_t x; /* mono, 16-bit */
        if (src == REC_BT) x = bt ? ((int32_t)bt[2 * i] + bt[2 * i + 1]) >> 1 : 0;
        else {
            const int32_t *p = src == REC_USB ? usb : mix;
            x = p ? (p[2 * i] + p[2 * i + 1]) >> 9 : 0; /* Q23 -> 16 bit, halved for the sum */
        }
        unsigned a = (unsigned)(x < 0 ? -x : x);
        if (a > peak) peak = a;
        if (st == REC_ARMED && (int)a >= threshold) st = state = REC_RUN;
        lp += ((x - lp) * 3) >> 2; /* gentle: keeps 16 kHz content from folding loudly */
        if (st == REC_RUN) {
            while (phase < 65536U) {
                int32_t y = prev + (int32_t)(((int64_t)(lp - prev) * phase) >> 16);
                unsigned f = frames;
                if (f >= limit) { st = state = REC_DONE; break; }
                buffer[f] = ulaw(y);
                frames = f + 1;
                phase += STEP;
            }
            phase -= 65536U;
        }
        prev = lp;
    }
    unsigned m = peak >> 8; /* 0..127 */
    level = m > level ? m : level > 2 ? level - 2 : 0;
}

int rec_encode(const unsigned *starts, unsigned n, struct rec_slice *out)
{
    if (state != REC_DONE || !buffer || !n || n > 8 || starts[0]) return -1;
    unsigned total = frames, at = 0; /* bytes written so far, at the buffer's start */
    uint8_t *bytes = buffer;
    for (unsigned k = 0; k < n; ++k) {
        unsigned a = starts[k], b = k + 1 < n ? starts[k + 1] : total;
        if (b <= a || b - a < 1600U || b > total) return -1;
        /* Each chunk is decoded out before its encoded bytes land at `at`,
         * which stays behind the unread input: 228 out for 320 in a chunk, and
         * slices are at least 1600 frames, so the padding of a slice's last
         * chunk is always covered. */
        struct sea_rec e; sea_rec_start(&e);
        out[k].data = bytes + at; out[k].frames = b - a; out[k].bytes = 0;
        for (unsigned f = a; f < b; f += SEA_REC_FRAMES) {
            int16_t pcm[SEA_REC_FRAMES];
            uint8_t chunk[SEA_REC_BYTES];
            unsigned m = b - f < SEA_REC_FRAMES ? b - f : SEA_REC_FRAMES;
            for (unsigned i = 0; i < SEA_REC_FRAMES; ++i) pcm[i] = (int16_t)(i < m ? rec_sample(f + i) : 0);
            sea_rec_chunk(&e, pcm, chunk);
            memcpy(bytes + at, chunk, SEA_REC_BYTES);
            at += SEA_REC_BYTES; out[k].bytes += SEA_REC_BYTES;
        }
    }
    state = REC_IDLE; /* the PCM is gone now; the buffer holds the encoded slices */
    return 0;
}
