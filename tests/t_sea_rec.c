/* Host check for app/sea_rec.c: encode a tone and noise burst chunk by chunk,
 * decode with the SDK decoder (web/synth/sea_decode.c), compare.
 * Exit 0 = signal-to-error ratio above 20 dB; 1 = decode failed; 2 = too noisy. */
#include "sea_rec.h"
#include "fm1_sea.h"
#include <stdio.h>
#include <math.h>
#define FM1_DSP_DATA
enum { SAMPLE_DRUM_PADS = 8 };
#include "sample_metadata.inc"

int main(void)
{
    enum { CHUNKS = 40 };
    static int16_t in[CHUNKS * SEA_REC_FRAMES], out[SEA_REC_FRAMES];
    uint32_t seed = 1;
    for (unsigned i = 0; i < CHUNKS * SEA_REC_FRAMES; ++i) {
        seed = seed * 1664525U + 1013904223U;
        double env = exp(-(double)(i % 6400) / 1500.0);
        in[i] = (int16_t)(env * (14000.0 * sin(i * 2 * 3.14159265 * 220.0 / 32000.0) + (double)((int32_t)seed >> 20)));
    }
    struct sea_rec e; sea_rec_start(&e);
    double sig = 0, err = 0;
    for (unsigned c = 0; c < CHUNKS; ++c) {
        uint8_t chunk[SEA_REC_BYTES];
        sea_rec_chunk(&e, in + c * SEA_REC_FRAMES, chunk);
        if (fm1_sea_decode(chunk, SEA_REC_BYTES, 5, sample_dqt, out)) return 1;
        for (unsigned i = 0; i < SEA_REC_FRAMES; ++i) {
            double a = in[c * SEA_REC_FRAMES + i], d = a - out[i];
            sig += a * a; err += d * d;
        }
    }
    double snr = 10.0 * log10(sig / (err + 1e-9));
    printf("SNR %.1f dB\n", snr);
    return snr > 20.0 ? 0 : 2;
}
