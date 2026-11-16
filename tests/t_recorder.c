/* Host test for app/recorder.c: how long a take may be (heap reserve, bank
 * room with slice padding), the buffer given back on an early stop, a take
 * that fills the whole bank cut into 8 uneven slices and encoded in place step
 * by step (every slice decodes back, the slices fit the bank), and the ISR
 * hand-shake flag. recorder.c is included so its statics can be checked.
 * Heap model: first-fit over a pool with a settable size, in-place shrink.
 * Build/run (Git Bash): CLANG=... sh tests/run_recorder.sh
 * Exit code 0 = all passed, otherwise the number of the first failed check. */
#include "../app/recorder.c"
#include "fm1_sea.h"
#include "flash_layout.h"
#define FM1_DSP_DATA
enum { SAMPLE_DRUM_PADS = 8 };
#include "sample_metadata.inc"

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

/* ---- heap: up to 16 blocks, first fit inside [0, heap_size) ---- */
static unsigned char pool[1 << 20];
static unsigned heap_size = sizeof pool;
static struct { unsigned at, size; } blocks[16];
static unsigned nblocks;
static unsigned heap_used(void) { unsigned u = 0; for (unsigned i = 0; i < nblocks; ++i) u += blocks[i].size; return u; }
static unsigned block_size(const void *p)
{
    for (unsigned i = 0; i < nblocks; ++i) if (pool + blocks[i].at == p) return blocks[i].size;
    return 0;
}
void *malloc(unsigned long long n)
{
    if (!n || n > heap_size || nblocks == 16) return 0;
    unsigned at = 0;
    for (;;) { /* lowest gap that holds n */
        unsigned next = heap_size, moved = 0;
        for (unsigned i = 0; i < nblocks; ++i) {
            unsigned a = blocks[i].at, b = a + blocks[i].size;
            if (at < b && a < at + n) { at = b; moved = 1; break; }
            if (a >= at && a < next) next = a;
        }
        if (moved) continue;
        if (at + n > heap_size) return 0;
        (void)next;
        break;
    }
    blocks[nblocks].at = at; blocks[nblocks].size = (unsigned)n; ++nblocks;
    return pool + at;
}
void free(void *p)
{
    if (!p) return;
    for (unsigned i = 0; i < nblocks; ++i)
        if (pool + blocks[i].at == p) { blocks[i] = blocks[--nblocks]; return; }
}
void *realloc(void *p, unsigned long long n)
{
    for (unsigned i = 0; i < nblocks; ++i)
        if (pool + blocks[i].at == p) { if (n <= blocks[i].size) { blocks[i].size = (unsigned)n; return p; } return 0; }
    return 0;
}

/* ---- audio: feed blocks of the synth's own mix (Q23 stereo) ---- */
static int32_t mix[2 * FM1_BLOCK_FRAMES];
static uint32_t seed = 1;
static unsigned t_in;
static void feed(unsigned blocks_n, int loud)
{
    for (unsigned b = 0; b < blocks_n; ++b) {
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++t_in) {
            seed = seed * 1664525U + 1013904223U;
            /* a saw at ~300 Hz with a 0.2 s decay, retriggered, plus a little noise */
            int env = loud ? 12000 - (int)(t_in % 8820U) : 0;
            int v = env > 0 ? (int)((t_in % 147U) * 2U * (unsigned)env / 147U) - env + ((int32_t)seed >> 22) : 0;
            mix[2 * i] = mix[2 * i + 1] = (int32_t)v << 8; /* (L + R) >> 9 = v */
        }
        rec_capture(NULL, NULL, mix);
    }
}

static int16_t pcm[REC_MAX_FRAMES];
static int t_isqrt_ratio_ok(long long sig, long long err) { return sig > 100LL * (err + 1); } /* > 20 dB */

int main(void)
{
    /* 1-2: take length from the heap and from the bank */
    /* up to 1.5 s as RC1 (12 KB left), longer only beyond 36 KB */
    if (rec_ram_fit(0) || rec_ram_fit(REC_RESERVE_SHORT + 128U + REC_MIN_FRAMES - 1U)) return 1;
    if (rec_ram_fit(REC_RESERVE_SHORT + 128U + REC_MIN_FRAMES) != REC_MIN_FRAMES) return 1;
    if (rec_ram_fit(REC_RESERVE + 128U + REC_SHORT_FRAMES) != REC_SHORT_FRAMES || rec_ram_fit(70000U) != REC_SHORT_FRAMES) return 1;
    if (rec_ram_fit(REC_RESERVE + 128U + REC_SHORT_FRAMES + 1U) != REC_SHORT_FRAMES + 1U) return 1;
    if (rec_ram_fit(100000U) != 100000U - REC_RESERVE - 128U || rec_ram_fit(1U << 20) != REC_MAX_FRAMES) return 1;
    if (rec_bank_fit(GROOVE_SAMPLE_LIMIT - 512U, 1) != REC_MAX_FRAMES) return 2;
    if (rec_bank_fit(GROOVE_SAMPLE_LIMIT - 512U, 8) != (446U - 7U) * 320U) return 2;
    if (rec_bank_fit(228U * 7U, 8) || rec_bank_fit(228U * 8U, 8) != 320U || rec_bank_fit(227U, 1)) return 2;
    if (REC_MAX_FRAMES != (GROOVE_SAMPLE_LIMIT - 512U) / 228U * 320U) return 2;

    /* 3: arming keeps REC_RESERVE free next to the take, and takes nearly all the rest */
    heap_size = 100000U;
    if (rec_arm(REC_MIX, -60, REC_MAX_FRAMES) || rec_state() != REC_ARMED) return 3;
    {
        unsigned n = rec_limit(), room = heap_size - REC_RESERVE;
        if (n > room || n < room - room / 15U - 2U || block_size(buffer) != n) return 3;
        void *r = malloc(REC_RESERVE); if (!r) return 3; free(r);
    }
    /* 4: a stop before the threshold gives the RAM back */
    rec_stop();
    if (rec_state() != REC_IDLE || buffer || heap_used()) return 4;
    /* 4b: a tight heap still gets a short take with RC1's 12 KB left; a middle
     * one gets 1.5 s, because a longer take could not leave 36 KB */
    heap_size = 40000U;
    if (rec_arm(REC_MIX, -60, REC_MAX_FRAMES)) return 4;
    {
        unsigned n = rec_limit(), room = heap_size - REC_RESERVE_SHORT;
        if (n > room || n < room - room / 15U - 2U) return 4;
        void *r = malloc(REC_RESERVE_SHORT); if (!r) return 4; free(r);
    }
    rec_free();
    heap_size = 70000U;
    if (rec_arm(REC_MIX, -60, REC_MAX_FRAMES) || rec_limit() > REC_SHORT_FRAMES || rec_limit() < REC_SHORT_FRAMES - REC_SHORT_FRAMES / 15U - 2U) return 4;
    rec_free();
    if (rec_arm(REC_MIX, -60, REC_SHORT_FRAMES) || rec_limit() != REC_SHORT_FRAMES) return 4; /* what the page asks for */
    rec_free();
    if (heap_used()) return 4;
    /* 5: too little heap = no take, nothing held; a block in the way is respected */
    heap_size = REC_RESERVE_SHORT + REC_MIN_FRAMES - 1U;
    if (rec_arm(REC_MIX, -60, REC_MAX_FRAMES) != -1 || heap_used()) return 5;
    heap_size = 120000U;
    void *engine = malloc(30000U); /* say an engine sits in the middle of the heap */
    if (!engine || rec_arm(REC_MIX, -60, REC_MAX_FRAMES)) return 5;
    { void *r = malloc(REC_RESERVE); if (!r) return 5; free(r); }
    rec_free(); free(engine);
    if (heap_used()) return 5;

    /* 6: an early stop keeps only what was recorded */
    heap_size = sizeof pool;
    rec_listen(REC_MIX, 1, 0);
    if (rec_arm(REC_MIX, -36, REC_MAX_FRAMES) || rec_limit() != REC_MAX_FRAMES) return 6;
    feed(10, 0);
    if (rec_state() != REC_ARMED || rec_frames()) return 6;
    feed(200, 1);
    if (rec_state() != REC_RUN || rec_frames() < 18000U) return 6;
    rec_stop();
    if (rec_state() != REC_DONE || block_size(buffer) != rec_frames() || busy) return 6;
    rec_free();
    if (heap_used()) return 6;

    /* 7: one take fills the bank (room for 8 slices), stops by itself */
    unsigned want = rec_bank_fit(GROOVE_SAMPLE_LIMIT - 512U, 8);
    if (rec_arm(REC_MIX, -60, want)) return 7;
    feed(1, 1);
    while (rec_state() == REC_RUN) feed(1, 1);
    unsigned total = rec_frames();
    if (rec_state() != REC_DONE || total != want || block_size(buffer) != want) return 7;
    for (unsigned i = 0; i < total; ++i) pcm[i] = (int16_t)rec_sample(i);

    /* 8: bad cuts are refused and leave the take as it was */
    struct rec_slice out[8];
    unsigned bad0[2] = {5, 9000}, bad1[2] = {0, 1000};
    if (!rec_encode_begin(bad0, 2, out) || !rec_encode_begin(bad1, 2, out) || rec_sample(100) != pcm[100]) return 8;

    /* 9: 8 uneven slices (none on a 320-frame boundary), encoded step by step */
    unsigned starts[8];
    for (unsigned k = 0; k < 8; ++k) starts[k] = k ? k * (total / 8U) + 97U * k + 1U : 0U;
    if (rec_encode_begin(starts, 8, out)) return 9;
    if (rec_sample(100)) return 9; /* the PCM is being replaced: reads 0 */
    unsigned done = 0, all = 0, steps = 0;
    int r;
    while ((r = rec_encode_step(&done, &all)) == 1) ++steps;
    if (r || done != all || steps + 1U != all || rec_state() != REC_IDLE) return 9;
    unsigned bytes = 0;
    for (unsigned k = 0; k < 8; ++k) {
        unsigned b = k < 7 ? starts[k + 1] : total;
        if (out[k].frames != b - starts[k] || out[k].bytes != (out[k].frames + 319U) / 320U * 228U) return 9;
        if (out[k].data != buffer + bytes) return 9;
        bytes += out[k].bytes;
    }
    if (bytes > GROOVE_SAMPLE_LIMIT - 512U || block_size(buffer) != bytes) return 10; /* fits; the rest went back */
    /* 11: every slice decodes back close to what was recorded (no in-place damage) */
    for (unsigned k = 0; k < 8; ++k) {
        long long sig = 0, err = 0;
        for (unsigned c = 0; c * 320U < out[k].frames; ++c) {
            int16_t dec[320];
            if (fm1_sea_decode(out[k].data + c * 228U, 228, 5, sample_dqt, dec)) return 11;
            for (unsigned i = 0; i < 320 && c * 320U + i < out[k].frames; ++i) {
                long long a = pcm[starts[k] + c * 320U + i], d = a - dec[i];
                sig += a * a; err += d * d;
            }
        }
        if (!t_isqrt_ratio_ok(sig, err)) return 11;
    }
    if (rec_encode_step(&done, &all) != -1) return 12;
    rec_free(); rec_listen(0, 0, 0);
    if (heap_used() || busy) return 12;
    return 0;
}
