/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "recorder.h"
#include "sea_rec.h"
#include <stdlib.h>
#include <string.h>

/* Shared with the audio ISR: the control task changes state only while the
 * ISR ignores it (IDLE / DONE), or by a single word store. Before it frees or
 * shrinks the buffer it waits until the ISR is out of rec_capture (busy), so
 * a block still being written can never land in freed memory. */
static volatile unsigned state, frames, limit, level, listening, muted, source, busy;
static volatile int threshold;
static uint8_t *buffer; /* the take: one mu-law byte per frame (half the RAM of 16 bit) */
/* A compiler and memory barrier without a library call (as in audio_output.c). */
static inline __attribute__((always_inline)) void barrier(void)
{
#ifndef FM1_HOST_TEST
    __asm__ volatile("csync" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}
static void quiet(void)
{
    barrier();
    while (busy) { }
    barrier();
}
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
static int unlaw(uint8_t b)
{
    unsigned u = (uint8_t)~b, e = (u >> 4) & 7U;
    int m = (int)((((u & 0x0fU) << 3) + 0x84U) << e) - 0x84;
    return u & 0x80U ? -m : m;
}
/* The encoder's progress (control task only). */
static struct {
    unsigned active, n, k, f, at, done, total;
    unsigned starts[8], offset[8];
    struct rec_slice *out;
    struct sea_rec e;
} enc;
int rec_sample(unsigned i)
{
    unsigned st = state;
    if (!buffer || i >= frames || enc.active || (st != REC_RUN && st != REC_DONE)) return 0;
    return unlaw(buffer[i]);
}
/* 44.1 kHz -> 32 kHz: a one-pole low-pass, then linear interpolation. */
static int32_t lp, prev;
static uint32_t phase; /* Q16 position between prev and the next input */
enum { STEP = (uint32_t)((44100.0 / 32000.0) * 65536.0 + 0.5) };

/* Give the end of the buffer back to the heap (dlmalloc shrinks in place). */
static void shrink(unsigned bytes)
{
    void *p = realloc(buffer, bytes ? bytes : 1U);
    if (p) buffer = p;
}
/* The heap a take of n frames must leave free in one block. */
static unsigned reserve_for(unsigned n) { return n > REC_SHORT_FRAMES ? REC_RESERVE : REC_RESERVE_SHORT; }
static int reserve_free(unsigned n)
{
    void *room = malloc(reserve_for(n));
    if (!room) return 0;
    free(room);
    return 1;
}
int rec_arm(unsigned src, int db, unsigned n)
{
    rec_free();
    if (n > REC_MAX_FRAMES) n = REC_MAX_FRAMES;
    if (n < REC_MIN_FRAMES) n = REC_MIN_FRAMES;
    /* The engines share the heap: take what fits, shortening the take in
     * steps of 1/16 (down to 0.25 s), and keep its reserve free in one block. */
    for (;;) {
        buffer = malloc(n);
        if (buffer) {
            if (reserve_free(n)) break;
            free(buffer); buffer = NULL;
        }
        if (n <= REC_MIN_FRAMES) return -1;
        n -= n / 16U + 1U;
        if (n < REC_MIN_FRAMES) n = REC_MIN_FRAMES;
    }
    /* -60..0 dB as a 16-bit amplitude: 32767 * 10^(db / 20), stepwise. */
    int a = 32767;
    for (int d = 0; d > db; d -= 6) a /= 2;
    threshold = a; source = src < REC_SOURCES ? src : 0; limit = n; frames = 0;
    lp = prev = 0; phase = 0;
    barrier();
    state = REC_ARMED;
    return 0;
}
void rec_stop(void)
{
    unsigned st = state;
    if (st == REC_RUN) state = frames ? REC_DONE : REC_IDLE;
    else if (st == REC_ARMED) state = REC_IDLE;
    else return;
    quiet();
    if (state == REC_IDLE) { free(buffer); buffer = NULL; frames = 0; } /* nothing taken: the RAM goes back */
    else if (frames < limit) shrink(frames); /* stopped early: keep only what was recorded */
}
void rec_free(void)
{
    state = REC_IDLE;
    quiet();
    free(buffer); buffer = NULL; frames = 0;
    enc.active = 0;
}
unsigned rec_state(void) { return state; }
unsigned rec_frames(void) { return frames; }
unsigned rec_limit(void) { return limit; }
unsigned rec_level(void) { return level; }
unsigned rec_ram_fit(unsigned largest)
{
    /* Worst case the take and the reserve both come out of this one block;
     * 128 bytes for two chunk headers and the 32-byte alignment. A short take
     * as RC1 allowed it, a longer one only out of what lies beyond REC_RESERVE. */
    unsigned n = largest > REC_RESERVE_SHORT + 128U ? largest - REC_RESERVE_SHORT - 128U : 0U;
    if (n > REC_SHORT_FRAMES) n = REC_SHORT_FRAMES;
    if (largest > REC_RESERVE + 128U && largest - REC_RESERVE - 128U > n) n = largest - REC_RESERVE - 128U;
    if (n > REC_MAX_FRAMES) n = REC_MAX_FRAMES;
    return n < REC_MIN_FRAMES ? 0 : n;
}
unsigned rec_ram_frames(void)
{
    if (buffer) return limit;
    for (unsigned n = REC_MAX_FRAMES; n >= REC_MIN_FRAMES; n -= n / 8U) {
        void *a = malloc(n);
        if (!a) continue;
        int ok = reserve_free(n);
        free(a);
        if (ok) return n;
    }
    return 0;
}
unsigned rec_bank_fit(unsigned room, unsigned max_slices)
{
    unsigned chunks = room / SEA_REC_BYTES, pad = max_slices ? max_slices - 1U : 0U;
    if (chunks <= pad) return 0;
    unsigned n = (chunks - pad) * SEA_REC_FRAMES;
    return n > REC_MAX_FRAMES ? REC_MAX_FRAMES : n;
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
    busy = 1;
    barrier();
    unsigned st = state;
    if (listening || st == REC_ARMED || st == REC_RUN) {
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
    barrier();
    busy = 0;
}

int rec_encode_begin(const unsigned *starts, unsigned n, struct rec_slice *out)
{
    if (state != REC_DONE || !buffer || enc.active || !n || n > 8 || starts[0]) return -1;
    unsigned total = frames, chunks = 0;
    for (unsigned k = 0; k < n; ++k) {
        unsigned a = starts[k], b = k + 1 < n ? starts[k + 1] : total;
        if (b <= a || b - a < 1600U || b > total) return -1;
        enc.starts[k] = a;
        out[k].data = NULL; out[k].frames = b - a; out[k].bytes = 0;
        chunks += (b - a + SEA_REC_FRAMES - 1U) / SEA_REC_FRAMES;
    }
    enc.n = n; enc.k = 0; enc.f = 0; enc.at = 0; enc.done = 0; enc.total = chunks; enc.out = out;
    enc.offset[0] = 0;
    sea_rec_start(&enc.e);
    enc.active = 1; /* from here on rec_sample() reads 0: the PCM is being replaced */
    return 0;
}
int rec_encode_step(unsigned *done, unsigned *total)
{
    if (!enc.active) return -1;
    /* One chunk. It is decoded out before its encoded bytes land at `at`,
     * which stays behind the unread input: 228 out for 320 in a chunk, and
     * slices are at least 1600 frames, so the padding of a slice's last chunk
     * is always covered (see tests/t_recorder.c). */
    unsigned k = enc.k, b = k + 1 < enc.n ? enc.starts[k + 1] : frames, f = enc.f;
    unsigned m = b - f < SEA_REC_FRAMES ? b - f : SEA_REC_FRAMES;
    int16_t pcm[SEA_REC_FRAMES];
    uint8_t chunk[SEA_REC_BYTES];
    for (unsigned i = 0; i < SEA_REC_FRAMES; ++i) pcm[i] = (int16_t)(i < m ? unlaw(buffer[f + i]) : 0);
    sea_rec_chunk(&enc.e, pcm, chunk);
    memcpy(buffer + enc.at, chunk, SEA_REC_BYTES);
    enc.at += SEA_REC_BYTES; enc.out[k].bytes += SEA_REC_BYTES; ++enc.done;
    f += SEA_REC_FRAMES;
    if (f >= b && ++k < enc.n) { /* next slice: a fresh encoder state */
        f = enc.starts[k]; enc.offset[k] = enc.at;
        sea_rec_start(&enc.e);
    }
    enc.k = k; enc.f = f;
    if (done) *done = enc.done;
    if (total) *total = enc.total;
    if (k < enc.n) return 1;
    /* All slices encoded: keep only their bytes, then point out[] at them. */
    state = REC_IDLE;
    shrink(enc.at);
    for (unsigned j = 0; j < enc.n; ++j) enc.out[j].data = buffer + enc.offset[j];
    enc.active = 0;
    return 0;
}
