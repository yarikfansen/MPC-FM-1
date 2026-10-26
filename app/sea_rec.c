/* SPDX-License-Identifier: MIT
 * SEA5 encoder for on-device sample recording, one 320-frame chunk at a time.
 * Adapted from web/synth/sea_encode.h (Copyright (c) 2025 Dani Biro;
 * modifications (c) 2026 M-VAVE) and the fixed tables in
 * web/synth/sample_metadata.inc. See THIRD_PARTY.md for the MIT terms.
 * Control task only (slow search; never in the audio ISR). */
#include "sea_rec.h"
#include <string.h>

static const int16_t dqt[512] = {
    1,-1,3,-3,5,-5,7,-7,9,-9,11,-11,13,-13,15,-15,
    17,-17,19,-19,21,-21,23,-23,25,-25,27,-27,29,-29,31,-31,
    4,-4,13,-13,23,-23,33,-33,43,-43,53,-53,63,-63,73,-73,
    83,-83,93,-93,103,-103,113,-113,123,-123,133,-133,143,-143,155,-155,
    11,-11,35,-35,63,-63,91,-91,119,-119,147,-147,175,-175,203,-203,
    231,-231,259,-259,287,-287,315,-315,343,-343,371,-371,399,-399,434,-434,
    21,-21,70,-70,126,-126,182,-182,238,-238,294,-294,350,-350,406,-406,
    462,-462,518,-518,574,-574,630,-630,686,-686,742,-742,798,-798,868,-868,
    36,-36,120,-120,216,-216,312,-312,408,-408,504,-504,600,-600,696,-696,
    792,-792,888,-888,984,-984,1080,-1080,1176,-1176,1272,-1272,1368,-1368,1488,-1488,
    56,-56,188,-188,338,-338,488,-488,638,-638,788,-788,938,-938,1088,-1088,
    1238,-1238,1388,-1388,1538,-1538,1688,-1688,1838,-1838,1988,-1988,2138,-2138,2325,-2325,
    81,-81,270,-270,486,-486,702,-702,918,-918,1134,-1134,1350,-1350,1566,-1566,
    1782,-1782,1998,-1998,2214,-2214,2430,-2430,2646,-2646,2862,-2862,3078,-3078,3348,-3348,
    113,-113,375,-375,675,-675,975,-975,1275,-1275,1575,-1575,1875,-1875,2175,-2175,
    2475,-2475,2775,-2775,3075,-3075,3375,-3375,3675,-3675,3975,-3975,4275,-4275,4650,-4650,
    149,-149,498,-498,896,-896,1294,-1294,1692,-1692,2090,-2090,2488,-2488,2886,-2886,
    3284,-3284,3682,-3682,4080,-4080,4478,-4478,4876,-4876,5274,-5274,5672,-5672,6169,-6169,
    193,-193,643,-643,1157,-1157,1671,-1671,2185,-2185,2699,-2699,3213,-3213,3727,-3727,
    4241,-4241,4755,-4755,5269,-5269,5783,-5783,6297,-6297,6811,-6811,7325,-7325,7967,-7967,
    242,-242,808,-808,1454,-1454,2100,-2100,2746,-2746,3392,-3392,4038,-4038,4684,-4684,
    5330,-5330,5976,-5976,6622,-6622,7268,-7268,7914,-7914,8560,-8560,9206,-9206,10013,-10013,
    299,-299,995,-995,1791,-1791,2587,-2587,3383,-3383,4179,-4179,4975,-4975,5771,-5771,
    6567,-6567,7363,-7363,8159,-8159,8955,-8955,9751,-9751,10547,-10547,11343,-11343,12338,-12338,
    362,-362,1208,-1208,2174,-2174,3140,-3140,4106,-4106,5072,-5072,6038,-6038,7004,-7004,
    7970,-7970,8936,-8936,9902,-9902,10868,-10868,11834,-11834,12800,-12800,13766,-13766,14973,-14973,
    434,-434,1445,-1445,2601,-2601,3757,-3757,4913,-4913,6069,-6069,7225,-7225,8381,-8381,
    9537,-9537,10693,-10693,11849,-11849,13005,-13005,14161,-14161,15317,-15317,16473,-16473,17918,-17918,
    512,-512,1705,-1705,3069,-3069,4433,-4433,5797,-5797,7161,-7161,8525,-8525,9889,-9889,
    11253,-11253,12617,-12617,13981,-13981,15345,-15345,16709,-16709,18073,-18073,19437,-19437,21142,-21142,
    598,-598,1993,-1993,3587,-3587,5181,-5181,6775,-6775,8369,-8369,9963,-9963,11557,-11557,
    13151,-13151,14745,-14745,16339,-16339,17933,-17933,19527,-19527,21121,-21121,22715,-22715,24707,-24707,
};
static int32_t sat(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }
static int32_t predict(const int32_t s[8])
{
    int64_t sum = 0;
    for (unsigned i = 0; i < 4; ++i) sum += (int64_t)s[i] * s[4 + i];
    return (int32_t)(sum >> 13);
}
static void update(int32_t s[8], int32_t pcm, int32_t residual)
{
    int32_t delta = residual >> 4;
    for (unsigned i = 0; i < 4; ++i) s[4 + i] = sat(s[4 + i] + (s[i] < 0 ? -delta : delta));
    for (unsigned i = 0; i < 3; ++i) s[i] = s[i + 1];
    s[3] = pcm;
}
static const int32_t reciprocal[16] = {65536, 13107, 4681, 2340, 1365, 873, 606, 436, 329, 255, 202, 164, 135, 113, 96, 82};
static uint64_t penalty(const int32_t s[8])
{
    int64_t sum = 0;
    for (unsigned i = 4; i < 8; ++i) sum += (int64_t)s[i] * s[i];
    int64_t v = (sum >> 18) - 0x8ff;
    return v > 0 ? (uint64_t)(v * v) : 0;
}
static int32_t divide(int32_t v, int32_t r)
{
    int64_t n = ((int64_t)v * r + 32768) >> 16;
    return (int32_t)(n + ((v > 0) - (v < 0)) - ((n > 0) - (n < 0)));
}
void sea_rec_start(struct sea_rec *e)
{
    static const int32_t seed[8] = {0, 0, 0, 0, 0, 0, -8192, 16384};
    memcpy(e->state, seed, sizeof seed);
    e->previous = 0;
}
void sea_rec_chunk(struct sea_rec *e, const int16_t pcm[SEA_REC_FRAMES], uint8_t out[SEA_REC_BYTES])
{
    enum { bits = 5, limit = 1 << bits, group_frames = 20 };
    uint8_t quant[2 * limit + 1];
    int x = limit - 1;
    quant[0] = (uint8_t)x;
    for (unsigned i = 1; i < limit; i += 2) { quant[i] = quant[i + 1] = (uint8_t)x; x -= 2; }
    x = 0;
    for (unsigned i = limit; i < 2 * limit; i += 2) { quant[i] = quant[i + 1] = (uint8_t)x; x += 2; }
    quant[2 * limit] = (uint8_t)(x - 2);
    memset(out, 0, SEA_REC_BYTES);
    out[0] = 1; out[1] = (uint8_t)(0x40 | bits); out[2] = 20; out[3] = 0x5a;
    for (unsigned i = 0; i < 8; ++i) { out[4 + 2 * i] = (uint8_t)e->state[i]; out[5 + 2 * i] = (uint8_t)((uint32_t)e->state[i] >> 8); }
    for (unsigned group = 0; group < SEA_REC_FRAMES / group_frames; ++group) {
        uint64_t best_rank = UINT64_MAX;
        int32_t best_state[8];
        uint8_t best_codes[group_frames], codes[group_frames];
        unsigned best_sf = 0;
        memcpy(best_state, e->state, sizeof best_state);
        memset(best_codes, 0, sizeof best_codes);
        for (unsigned search = 0; search < 16; ++search) {
            unsigned sf = (search + e->previous) & 15U, i;
            int32_t trial[8]; memcpy(trial, e->state, sizeof trial);
            uint64_t rank = 0;
            for (i = 0; i < group_frames; ++i) {
                int32_t p = pcm[group * group_frames + i], prediction = predict(trial);
                int32_t scaled = divide(p - prediction, reciprocal[sf]);
                if (scaled > limit) scaled = limit;
                if (scaled < -limit) scaled = -limit;
                unsigned q = quant[limit + scaled];
                int32_t residual = dqt[(sf << bits) + q], rebuilt = sat(prediction + residual);
                int64_t error = (int64_t)p - rebuilt;
                rank += (uint64_t)(error * error) + penalty(trial);
                if (rank > best_rank) break;
                codes[i] = (uint8_t)q;
                update(trial, rebuilt, residual);
            }
            if (i == group_frames && rank < best_rank) {
                best_rank = rank; best_sf = sf;
                memcpy(best_state, trial, sizeof trial); memcpy(best_codes, codes, sizeof codes);
            }
        }
        memcpy(e->state, best_state, sizeof best_state); e->previous = best_sf;
        out[20 + group / 2] |= (uint8_t)(best_sf << ((group & 1U) ? 0 : 4));
        for (unsigned i = 0; i < group_frames; ++i) {
            unsigned at = (group * group_frames + i) * bits, packed = (unsigned)best_codes[i] << (11U - (at & 7U));
            out[28 + at / 8] |= (uint8_t)(packed >> 8);
            if ((at & 7U) > 3U) out[29 + at / 8] |= (uint8_t)packed;
        }
    }
}
