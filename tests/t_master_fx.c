/* Host check for app/master_fx.c (freestanding, no C library).
 * Exit 0 = every filter and ring type stays finite and bounded at extreme
 * settings; otherwise the code names the failing case (see below).
 * Built with -DMFX_CYCLES the exit code is instead the CPU cost of the most
 * expensive case in cycles per stereo frame / 10 (x86 TSC, rough guide). */
#include "master_fx.h"
#include <stdint.h>

void *memset(void *d, int c, unsigned long long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static int32_t buf[FM1_BLOCK_WORDS];
static uint32_t seed = 1;

static void set(unsigned fx, unsigned param, int value)
{
    mfx_edit(fx, param, -255);
    mfx_edit(fx, param, value);
}

/* 110 Hz saw plus a little noise, Q23, then silence after `blocks_on`. */
static int run(unsigned blocks, unsigned blocks_on, int32_t *peak)
{
    static unsigned t;
    for (unsigned b = 0; b < blocks; ++b) {
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++t) {
            int32_t v = 0;
            if (b < blocks_on) {
                seed = seed * 1664525U + 1013904223U;
                v = (int32_t)((t % 401U) * 20000U) - 4000000 + (int32_t)(seed >> 12) - (1 << 19);
            }
            buf[2 * i] = v; buf[2 * i + 1] = v;
        }
        mfx_process(buf);
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > *peak) *peak = a;
        }
    }
    return 0;
}

#ifdef MFX_CYCLES
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
#endif

int main(void)
{
    /* Filters: type 0..4 at full resonance, cutoff low / mid / high. */
    for (unsigned type = 0; type < 5; ++type)
        for (unsigned cut = 0; cut < 3; ++cut) {
            set(MFX_FILTER, 0, (int)type); set(MFX_FILTER, 1, (int)(cut * 63)); set(MFX_FILTER, 2, 127); set(MFX_FILTER, 3, 127);
            int32_t peak = 0;
#ifdef MFX_CYCLES
            uint64_t t0 = rdtsc();
            run(200, 200, &peak);
            uint64_t per = (rdtsc() - t0) / (200U * FM1_BLOCK_FRAMES);
            if (type == MFX_CYCLES && cut == 1) return (int)(per / 10U);
#else
            run(300, 200, &peak);
            if (peak >= (1 << 23) * 8 || peak < 0) return 10 + (int)(type * 3 + cut);  /* blew up */
#endif
        }
    set(MFX_FILTER, 3, -127);
    /* Ring modulator: 8 carriers at the lowest and highest FREQ. */
    for (unsigned wave = 0; wave < 8; ++wave)
        for (unsigned f = 0; f < 2; ++f) {
            set(MFX_RING, 0, (int)wave); set(MFX_RING, 1, f ? 127 : 0); set(MFX_RING, 2, 127); set(MFX_RING, 3, 127);
            int32_t peak = 0;
            run(200, 200, &peak);
            if (peak >= (1 << 23) * 4 || peak < 0) return 40 + (int)(wave * 2 + f);
        }
    return 0;
}
void *memcpy(void *d, const void *s, unsigned long long n)
{
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
int _fltused = 1;
