/* Host check that app/audio_output.c NEVER mutes the board output (F12, user
 * 10.11 "Убрать": the recording mute of 7715441 is removed). Whatever the USB
 * capture stream does (closed, open, partial writes, restarts, errors), the
 * speaker / headphone output is exactly the mix USB gets, from the first
 * block on, also at full scale. Freestanding, no C library; a fake
 * fm1_usb_audio_write plays the transport. Exit 0 = every check passed;
 * otherwise the failing check. */
#include "audio_output.h"
#include "fm1_audio_io.h"
#include "fm1_engine_control.h"
#include "master_fx.h"
#include "micro_d.h"
#include "recorder.h"
#include "binaural.h"
#include <stdint.h>

void *memset(void *d, int c, unsigned long long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long long n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

/* ---- fakes for everything fm1_output_mix calls ---- */
static int write_ret;                 /* what the fake USB transport returns */
static unsigned full_scale_in;        /* 1: USB playback input at full scale */
static int32_t usb_copy[FM1_BLOCK_WORDS];
int fm1_usb_audio_write(const int32_t *pcm, unsigned frames)
{
    if (!pcm || frames != FM1_BLOCK_FRAMES) return -1;
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) usb_copy[i] = pcm[i];
    return write_ret;
}
int fm1_usb_audio_read(int32_t *pcm, unsigned frames, uint16_t gains_q7[2])
{
    if (!full_scale_in) return 0;
    for (unsigned i = 0; i < frames * 2U; ++i) pcm[i] = (i & 1U) ? -8388608 : 8388607;
    gains_q7[0] = gains_q7[1] = 128;
    return (int)frames;
}
int fm1_bluetooth_audio_read(int16_t *pcm, unsigned frames) { (void)pcm; (void)frames; return 0; }
unsigned fm1_engine_output_level(void) { return 127; }
void micro_d_render(int32_t *buffer) { (void)buffer; }
void mfx_process(int32_t *buffer) { (void)buffer; }
unsigned mfx_clearing(void) { return 0; }
unsigned mfx_units_on(void) { return 0; }
void mfx_shed(unsigned fade, unsigned cut) { (void)fade; (void)cut; }
unsigned mfx_units_silent(void) { return 0; }
unsigned mfx_units_timed(void) { return 0; }
uint32_t mfx_unit_ticks(unsigned unit) { (void)unit; return 0; }
void mfx_clock_set(uint32_t (*lap)(uint32_t *mark)) { (void)lap; }
void rec_capture(const int32_t *usb, const int16_t *bt, const int32_t *mix) { (void)usb; (void)bt; (void)mix; }
unsigned rec_input_muted(void) { return 0; }
void binaural_render(int32_t *buffer) { (void)buffer; }
int fm1_flash_write(uint32_t offset, const void *data, size_t bytes) { (void)offset; (void)data; (void)bytes; return 0; }
int fm1_flash_erase(uint32_t offset) { (void)offset; return 0; }

/* ---- helpers ---- */
enum { A = 4000000 }; /* engine DC level: L = +A, R = -A */
static int32_t buf[FM1_BLOCK_WORDS];

static void block(int ret)
{
    for (unsigned f = 0; f < FM1_BLOCK_FRAMES; ++f) { buf[2 * f] = A; buf[2 * f + 1] = -A; }
    write_ret = ret;
    fm1_output_mix(buf);
}
/* speaker == USB copy (no mute), and the mix is the expected level */
static int same(void)
{
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (buf[i] != usb_copy[i]) return 0;
    return 1;
}
static int level(int32_t l, int32_t r) { return buf[0] == l && buf[1] == r && buf[FM1_BLOCK_WORDS - 2] == l && buf[FM1_BLOCK_WORDS - 1] == r; }

int main(void)
{
    static const int rets[] = {0, FM1_BLOCK_FRAMES, 37, -2, -1};
    fm1_output_reset();
    /* the output gain ramps in after a reset (upstream); let it settle */
    for (unsigned b = 0; b < 64; ++b) { block(0); if (!same()) return 1; }
    int32_t l = buf[0], r = buf[1];
    if (l <= 0 || r >= 0) return 1;
    /* 2: the capture stream opens and accepts whole blocks for ~1.5 s: never muted, never faded */
    for (unsigned b = 0; b < 500; ++b) { block(FM1_BLOCK_FRAMES); if (!same() || !level(l, r)) return 2; }
    /* 3: partial writes, restarts (-2), bad argument (-1), ring full (0), in every order */
    for (unsigned b = 0; b < 1000; ++b) { block(rets[(b * 7U + b / 5U) % 5U]); if (!same() || !level(l, r)) return 3; }
    /* 4: the stream stops: still the same, during and after the old 104-block hold */
    for (unsigned b = 0; b < 300; ++b) { block(0); if (!same() || !level(l, r)) return 4; }
    /* 5: full-scale USB playback input with the capture open: clipped mix on both */
    full_scale_in = 1;
    for (unsigned b = 0; b < 50; ++b) {
        block(b & 1U ? FM1_BLOCK_FRAMES : -2);
        if (!same()) return 5;
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (usb_copy[i] != ((i & 1U) ? -8388608 : 8388607)) return 5;
    }
    return 0;
}
