/* Host harness for app/micro_d.c, built as a DLL and driven by
 * tests/t_micro_d.py (renders presets to WAV, checks levels and clicks). */
#include "micro_d.h"
#include "instrument.h"
#include <stdint.h>

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, unsigned long long n)
{
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++; else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
unsigned long long strlen(const char *s) { unsigned long long n = 0; while (s[n]) ++n; return n; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
int _fltused;

static uint8_t vol[GROOVE_TRACKS] = {100, 100, 100, 100, 100, 100, 100, 100}, type[GROOVE_TRACKS];
void groove_get_view(unsigned track, unsigned step, struct groove_view *v) { (void)step; v->volume = vol[track]; v->type = type[track]; }
void groove_control_edit(unsigned track, int program_steps, int volume_steps, int cutoff_steps)
{ (void)program_steps; (void)cutoff_steps; int x = vol[track] + volume_steps; vol[track] = (uint8_t)(x < 0 ? 0 : x > 127 ? 127 : x); }
int groove_source_set(unsigned track, unsigned t) { type[track] = (uint8_t)t; return 0; }
/* micro_d_host_guard (tests/t_md_host_guard.c) is not exercised here */
unsigned groove_track_role(unsigned t) { (void)t; return GROOVE_TRACK_NORMAL; }
unsigned groove_midi_input_channel(unsigned t) { return t; }
int fm1_midi_post(const uint8_t *m, size_t n) { (void)m; (void)n; return 0; }

__declspec(dllexport) int md_attach(unsigned t) { return micro_d_attach(t); }
__declspec(dllexport) void md_preset(unsigned t, unsigned p) { micro_d_preset_load(t, p); }
__declspec(dllexport) unsigned md_presets(void) { return micro_d_preset_count(); }
__declspec(dllexport) const char *md_name(unsigned p) { return micro_d_preset_name(p); }
__declspec(dllexport) void md_set(unsigned t, unsigned p, int v) { micro_d_edit(t, p, -255); micro_d_edit(t, p, v); }
__declspec(dllexport) void md_note(unsigned t, unsigned n, unsigned v) { micro_d_note(t, n, v); }
__declspec(dllexport) unsigned md_host_volume(unsigned t) { return vol[t]; }
__declspec(dllexport) void md_render(int32_t *buf)
{
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = 0;
    micro_d_render(buf);
}
__declspec(dllexport) uint64_t md_cycles(unsigned blocks, int32_t *buf)
{
    uint64_t t0 = __builtin_ia32_rdtsc();
    for (unsigned b = 0; b < blocks; ++b) micro_d_render(buf);
    return __builtin_ia32_rdtsc() - t0;
}
__declspec(dllexport) void md_text(unsigned t, unsigned p, char *out) { micro_d_text(t, p, out); }
__declspec(dllexport) void md_state_get(uint8_t *out) { micro_d_state_get(out); }
__declspec(dllexport) void md_state_put(const uint8_t *in) { micro_d_state_put(in); }
__declspec(dllexport) int md_owns(unsigned t) { return micro_d_track(t); }
__declspec(dllexport) unsigned md_value(unsigned t, unsigned p) { return micro_d_value(t, p); }
__declspec(dllexport) unsigned md_volume(unsigned t) { return micro_d_volume(t); }
__declspec(dllexport) void md_detach(unsigned t) { micro_d_detach(t); }
__declspec(dllexport) void md_pan(unsigned t, unsigned side) { micro_d_pan(t, side); }
__declspec(dllexport) unsigned md_voices(unsigned t, uint8_t *notes) { return micro_d_voices(t, notes); }

/* ---- old / new comparison (W4). Both the new app/micro_d.c and the
 * instrumented reference copy define these test-only counters. ---- */
extern unsigned md_test_tanh, md_test_div, md_test_sat, md_test_busy;
__declspec(dllexport) void md_ops(unsigned *o) { o[0] = md_test_tanh; o[1] = md_test_div; o[2] = md_test_sat; o[3] = md_test_busy; }
/* Renders blocks, keeps the left channel; busy[b] = 1 when block b ran the full render. */
__declspec(dllexport) void md_play(int32_t *left, unsigned blocks, uint8_t *busy)
{
    static int32_t b[FM1_BLOCK_WORDS];
    for (unsigned k = 0; k < blocks; ++k) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) b[i] = 0;
        unsigned before = md_test_busy;
        micro_d_render(b);
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) left[k * FM1_BLOCK_FRAMES + i] = b[2 * i];
        if (busy) busy[k] = md_test_busy != before;
    }
}
/* sin for the FFT twiddles, no libm: reduce to [-pi, pi], Taylor series. */
static double dsin(double a)
{
    const double pi = 3.14159265358979323846;
    while (a > pi) a -= 2 * pi;
    while (a < -pi) a += 2 * pi;
    double t = a, s = a, q = a * a;
    for (int n = 1; n < 16; ++n) { t *= -q / ((2 * n) * (2 * n + 1)); s += t; }
    return s;
}
enum { FFT_MAX = 1 << 17 };
static double fre[FFT_MAX], fim[FFT_MAX];
/* res[0] energy (sum of squares), res[1] peak |x|, res[2..9] energy in the
 * octave bands 62.5-125 Hz .. 8-16 kHz (one-sided FFT power, zero padded),
 * res[10] one-sided FFT power over all bins. */
__declspec(dllexport) void md_analyse(const int32_t *x, unsigned n, double *res)
{
    unsigned N = 1;
    while (N < n && N < FFT_MAX) N <<= 1;
    if (n > N) n = N;
    double e = 0, pk = 0;
    for (unsigned i = 0; i < N; ++i) {
        double v = i < n ? (double)x[i] : 0.0;
        fre[i] = v; fim[i] = 0;
        e += v * v; if ((v < 0 ? -v : v) > pk) pk = v < 0 ? -v : v;
    }
    for (unsigned i = 1, j = 0; i < N; ++i) { /* bit reversal */
        unsigned bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = fre[i]; fre[i] = fre[j]; fre[j] = t; t = fim[i]; fim[i] = fim[j]; fim[j] = t; }
    }
    for (unsigned len = 2; len <= N; len <<= 1) {
        double ang = -2 * 3.14159265358979323846 / len, wr = dsin(ang + 1.57079632679489661923), wi = dsin(ang);
        for (unsigned i = 0; i < N; i += len) {
            double cr = 1, ci = 0;
            for (unsigned j = 0; j < len / 2; ++j) {
                unsigned a = i + j, b = a + len / 2;
                double tr = fre[b] * cr - fim[b] * ci, ti = fre[b] * ci + fim[b] * cr;
                fre[b] = fre[a] - tr; fim[b] = fim[a] - ti; fre[a] += tr; fim[a] += ti;
                double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
    for (unsigned b = 0; b < 11; ++b) res[b] = 0;
    res[0] = e; res[1] = pk;
    for (unsigned kk = 0; kk <= N / 2; ++kk) {
        double p = fre[kk] * fre[kk] + fim[kk] * fim[kk], f = kk * 44100.0 / N;
        res[10] += p;
        if (f < 62.5 || f >= 16000.0) continue;
        unsigned band = 0; double lo = 62.5;
        while (f >= lo * 2) { lo *= 2; ++band; }
        res[2 + band] += p;
    }
}
