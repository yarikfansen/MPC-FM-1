/* Host test for app/extras.c: record versions, unchanged saves, and power
 * loss at every step of a save and of the recovery after it.
 * Flash model follows the SDK contract: writes only clear bits (else
 * NEEDS_ERASE), erase sets one aligned sector to 0xff. A power cut stops the
 * running write after some 64-byte chunks (the next byte half programmed) or
 * an erase after some 512-byte parts; every later Flash call fails.
 * Build/run (Git Bash): CLANG=... sh tests/run_extras.sh
 * Exit code 0 = all passed, otherwise the number of the first failed check. */
#include "extras.h"
#include "instrument.h"
#include "micro_d.h"
#include "binaural.h"
#include "lang.h"
#include "audio_output.h"

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, unsigned long long n)
{
    const unsigned char *p = a, *q = b;
    for (; n; --n, ++p, ++q) if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}

/* ---- heap: bump allocator, reset by the test between operations ---- */
static unsigned char pool[1 << 16];
static unsigned pool_used;
void *malloc(unsigned long long n) { n = (n + 15U) & ~15ULL; if (pool_used + n > sizeof pool) return 0; void *p = pool + pool_used; pool_used += (unsigned)n; return p; }
void free(void *p) { (void)p; }

/* ---- Flash with power cuts ---- */
enum { USER = GROOVE_FLASH_BYTES, SECTOR = FM1_FLASH_SECTOR_BYTES, STAGE = GROOVE_PROJECT_SLOTS * SECTOR };
static unsigned char flash[USER];
static long budget = -1; /* work units left before the power goes; -1 = no cut */
static int dead;
static unsigned writes, erases;
static int spend(long units, long *done) /* 1 = completes */
{
    if (budget < 0) { *done = units; return 1; }
    if (budget >= units) { budget -= units; *done = units; return 1; }
    *done = budget; budget = 0; dead = 1; return 0;
}
uint32_t fm1_flash_size(void) { return USER; }
int fm1_flash_read(uint32_t at, void *data, size_t bytes)
{
    if (dead) return FM1_FLASH_IO;
    if (at > USER || bytes > USER - at) return FM1_FLASH_INVALID;
    memcpy(data, flash + at, bytes); return 0;
}
int fm1_flash_write(uint32_t at, const void *data, size_t bytes)
{
    const unsigned char *s = data;
    if (dead) return FM1_FLASH_IO;
    if (at > USER || bytes > USER - at) return FM1_FLASH_INVALID;
    for (size_t i = 0; i < bytes; ++i) if ((flash[at + i] & s[i]) != s[i]) return FM1_FLASH_NEEDS_ERASE;
    long done, chunks = (long)((bytes + 63) / 64);
    ++writes;
    if (spend(chunks, &done)) { memcpy(flash + at, data, bytes); return 0; }
    size_t n = (size_t)done * 64U;
    if (n > bytes) n = bytes;
    memcpy(flash + at, data, n);
    if (n < bytes) flash[at + n] &= (unsigned char)(s[n] | 0xa5U); /* half programmed */
    return FM1_FLASH_IO;
}
int fm1_flash_erase(uint32_t at)
{
    if (dead) return FM1_FLASH_IO;
    if (at % SECTOR || at > USER - SECTOR) return FM1_FLASH_INVALID;
    long done;
    ++erases;
    if (spend(8, &done)) { memset(flash + at, 0xff, SECTOR); return 0; }
    memset(flash + at, 0xff, (size_t)done * 512U);
    return FM1_FLASH_IO;
}
int flash_write_safe(uint32_t at, const void *data, size_t bytes) { return fm1_flash_write(at, data, bytes); }
int flash_erase_safe(uint32_t at) { return fm1_flash_erase(at); }

/* ---- project.c pieces ---- */
uint32_t groove_crc32(uint32_t crc, const void *data, unsigned bytes)
{
    static const uint32_t table[16] = {
        0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU, 0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU, 0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU};
    const uint8_t *p = data;
    while (bytes--) { crc ^= *p++; crc = (crc >> 4) ^ table[crc & 15U]; crc = (crc >> 4) ^ table[crc & 15U]; }
    return crc;
}
int groove_flash_tail_blocked(void) { return 0; }
int groove_storage_prepare(void) { return extras_recover(); } /* the extras part of recover_staging */

/* ---- module state: getters give cur[], putters fill got[] (same offsets as extras.c) ---- */
static uint8_t cur[EXTRAS_BYTES], got[EXTRAS_BYTES];
static void give(unsigned at, uint8_t *out, unsigned n) { memcpy(out, cur + at, n); }
static void take(unsigned at, const uint8_t *in, unsigned n) { memcpy(got + at, in, n); }
void mfx_state_get(uint8_t out[32]) { give(0, out, 32); }
void mfx_state_put(const uint8_t in[32]) { take(0, in, 32); }
void lfo_state_get(uint8_t out[4]) { give(32, out, 4); }
void lfo_state_put(const uint8_t in[4]) { take(32, in, 4); }
void arp_state_get(uint8_t out[56]) { give(36, out, 56); }
void arp_state_put(const uint8_t in[56]) { take(36, in, 56); }
void groove_screen_extras_get(uint8_t out[26]) { give(92, out, 26); }
void groove_screen_extras_put(const uint8_t in[26]) { take(92, in, 26); }
void lfo2_state_get(uint8_t out[4]) { give(118, out, 4); }
void lfo2_state_put(const uint8_t in[4]) { take(118, in, 4); }
void groove_screen_extras2_get(uint8_t out[54]) { give(122, out, 54); }
void groove_screen_extras2_put(const uint8_t in[54]) { take(122, in, 54); }
void micro_d_state_get(uint8_t out[MD_STATE_BYTES]) { give(176, out, MD_STATE_BYTES); }
void micro_d_state_put(const uint8_t in[MD_STATE_BYTES]) { take(176, in, MD_STATE_BYTES); }
void binaural_state_get(uint8_t out[BIN_STATE_BYTES]) { give(223, out, BIN_STATE_BYTES); }
void binaural_state_put(const uint8_t in[BIN_STATE_BYTES]) { take(223, in, BIN_STATE_BYTES); }
unsigned groove_screen_play_led_get(void) { return cur[235]; }
void groove_screen_play_led_put(unsigned stored) { got[235] = (uint8_t)stored; }
static uint8_t pan_side[2];
void groove_screen_pan_get(uint8_t value[8]) { give(236, value, 8); }
void groove_screen_pan_put(const uint8_t side[2], const uint8_t value[8]) { memcpy(pan_side, side, 2); take(236, value, 8); }
unsigned groove_screen_peak(void) { return 0; }
void groove_screen_last_peak_set(unsigned percent) { (void)percent; }
unsigned lang_get(void) { return 0; }
void lang_set(unsigned language) { (void)language; }
unsigned ui_theme_get(void) { return 0; }
void ui_theme_set(unsigned theme) { (void)theme; }
/* GLOBAL KEY (172): spare extras2 byte, applied at power on only; 173 = old DISPLAY BRIGHT, never applied (F11). */
static unsigned key_put = 999, bright_put = 999;
unsigned key_state_get(void) { return cur[172]; }
void key_state_put(unsigned stored) { key_put = stored; }
unsigned ui_bright_get(void) { return cur[173]; }
void ui_bright_set(unsigned level) { bright_put = level; }

/* The state of slot s in generation g (222 = language/theme/peak, all 0 here). */
static void state(unsigned s, unsigned g)
{
    for (unsigned i = 0; i < EXTRAS_BYTES; ++i) cur[i] = (uint8_t)(s * 29U + g * 101U + i * 7U + 1U);
    cur[222] = 0;
    cur[173] = 10; /* old BRIGHT byte: always saved as 10 (F11) */
}
static int matches(unsigned s, unsigned g)
{
    state(s, g);
    return !memcmp(cur, got, EXTRAS_BYTES);
}
/* Which generation slot s loads now: 0..3, 8 = defaults, 9 = Flash error, 10 = other data. */
static unsigned loaded(unsigned s)
{
    pool_used = 0;
    memset(got, 0, sizeof got);
    int r = extras_load(s);
    if (r < 0) return 9;
    if (r) return 8;
    for (unsigned g = 0; g < 4; ++g) if (matches(s, g)) return g;
    return 10;
}
static int save(unsigned s, unsigned g) { pool_used = 0; state(s, g); return extras_save(s); }

static int failed;
#define CHECK(n, c) do { if (!(c) && !failed) failed = (n); } while (0)
static unsigned char base[USER], cut[USER];

int main(void)
{
    /* 1. Empty Flash: defaults, then every slot saved and read back. */
    memset(flash, 0xff, sizeof flash);
    for (unsigned s = 0; s < 16; ++s) CHECK(1, loaded(s) == 8);
    for (unsigned s = 0; s < 16; ++s) CHECK(2, save(s, 0) == 0);
    for (unsigned s = 0; s < 16; ++s) CHECK(3, loaded(s) == 0);
    /* 2. Saving the same state again writes nothing. */
    writes = erases = 0;
    CHECK(4, save(5, 0) == 0 && writes == 0 && erases == 0);
    /* 3. A change: one journal + one sector rewrite, others unchanged. */
    writes = erases = 0;
    CHECK(5, save(5, 1) == 0 && erases == 2 && writes == 3);
    for (unsigned s = 0; s < 16; ++s) CHECK(6, loaded(s) == (s == 5 ? 1U : 0U));
    /* 4. A version 5 record (236 bytes, no pan) still loads; pan bytes stay 0xff = "take the side". */
    {
        uint8_t rec[256]; memset(rec, 0xff, sizeof rec);
        uint32_t magic = 0x35584d46U, slot = 7, crc;
        state(7, 2);
        memcpy(rec, &magic, 4); memcpy(rec + 4, &slot, 4); memcpy(rec + 8, cur, EXTRAS_BYTES_V5);
        crc = groove_crc32(~0U, rec, 8 + EXTRAS_BYTES_V5);
        memcpy(rec + 8 + EXTRAS_BYTES_V5, &crc, 4);
        pool_used = 0;
        CHECK(7, extras_recover() == 0); /* nothing pending */
        memset(flash + GROOVE_EXTRAS_OFFSET + 7 * 256, 0xff, 256); /* test-only: place it directly */
        memcpy(flash + GROOVE_EXTRAS_OFFSET + 7 * 256, rec, sizeof rec);
        pool_used = 0; memset(got, 0, sizeof got);
        CHECK(8, extras_load(7) == 0);
        CHECK(9, !memcmp(got, cur, EXTRAS_BYTES_V5) && got[236] == 0xff && got[243] == 0xff && pan_side[0] == cur[170]);
        /* A damaged record means defaults. */
        flash[GROOVE_EXTRAS_OFFSET + 7 * 256 + 40] ^= 1;
        CHECK(10, loaded(7) == 8);
        CHECK(11, save(7, 0) == 0 && loaded(7) == 0);
    }
    /* 5. Power-on load (voices only): MICRO D, PLAY LED and pan come back, the rest are defaults. */
    {
        pool_used = 0; memset(got, 0, sizeof got);
        CHECK(12, extras_load_voices(3) == 0);
        state(3, 0);
        CHECK(13, !memcmp(got + 176, cur + 176, MD_STATE_BYTES) && !memcmp(got + 236, cur + 236, 8) &&
                  !memcmp(pan_side, cur + 170, 2) && got[235] == cur[235] && got[0] == 0xff && got[36] == 0xff);
    }
    /* 6. Power lost at every step of saving slot k (generation 0 -> 1), then
     * (a) every slot loads its old or new state, the others their old one;
     * (b) what loads does not change when the next write finishes the copy;
     * (c) for cuts inside the copy, a second cut at every step of that
     *     recovery still leaves the same result. */
    for (unsigned s = 0; s < 16; ++s) (void)save(s, 0);
    memcpy(base, flash, sizeof flash);
    budget = 100000; pool_used = 0; state(9, 1);
    CHECK(20, extras_save(9) == 0);
    long full = 100000 - budget;
    budget = -1;
    CHECK(21, full > 100 && full < 200);
    unsigned saw_new = 0, saw_old = 0;
    for (long c = 0; c <= full && !failed; ++c) {
        memcpy(flash, base, sizeof flash);
        dead = 0; budget = c;
        (void)save(9, 1);
        budget = -1; dead = 0; /* power back */
        unsigned seen[16];
        for (unsigned s = 0; s < 16; ++s) {
            seen[s] = loaded(s);
            CHECK(22, s == 9 ? seen[s] <= 1 : seen[s] == 0);
        }
        saw_new |= seen[9] == 1; saw_old |= seen[9] == 0;
        memcpy(cut, flash, sizeof flash);
        long rfull = 0;
        { pool_used = 0; budget = 100000; CHECK(23, extras_recover() == 0); rfull = 100000 - budget; budget = -1; }
        for (unsigned s = 0; s < 16; ++s) CHECK(24, loaded(s) == seen[s]);
        for (long c2 = 0; c2 < rfull && !failed; ++c2) {
            memcpy(flash, cut, sizeof flash);
            dead = 0; budget = c2; pool_used = 0;
            (void)extras_recover();
            budget = -1; dead = 0;
            for (unsigned s = 0; s < 16; ++s) CHECK(25, loaded(s) == seen[s]);
            pool_used = 0; CHECK(26, extras_recover() == 0);
            for (unsigned s = 0; s < 16; ++s) CHECK(27, loaded(s) == seen[s]);
        }
        /* After any cut the next save still works. */
        CHECK(28, save(2, 3) == 0 && loaded(2) == 3 && loaded(9) == seen[9]);
    }
    CHECK(29, saw_new && saw_old);
    /* 30. KEY rides in byte 172: a normal load does not apply it, the power-on
     * restore does. Byte 173 (the removed DISPLAY BRIGHT, F11) is saved as 10
     * and never applied, also when an older firmware stored 1..9 there. */
    {
        memset(flash, 0xff, sizeof flash); pool_used = 0;
        state(4, 0); cur[172] = 0x1a; cur[173] = 4;
        CHECK(30, extras_save(4) == 0);
        uint8_t raw[EXTRAS_RECORD]; pool_used = 0;
        CHECK(30, extras_read(4, raw) == 0 && raw[8 + 172] == 0x1a && raw[8 + 173] == 10);
        key_put = bright_put = 999; extras_apply(4, raw, 0);
        CHECK(30, key_put == 999 && bright_put == 999);
        extras_apply(4, raw, 1);
        CHECK(30, key_put == 0x1a && bright_put == 999);
        raw[8 + 172] = 0; key_put = bright_put = 999;
        /* a changed byte breaks the CRC: defaults, nothing applied */
        extras_apply(4, raw, 1);
        CHECK(30, key_put == 999 && bright_put == 999);
    }
    return failed;
}
