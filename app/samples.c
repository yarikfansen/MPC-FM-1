/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm1_engine_control.h"
#include "fm1_device.h"
#include "instrument.h"
#include "sampledrum.h"
#include "boot_guard.h"
#include "fm_patch.h"
#include "recorder.h"
#include <stdlib.h>
#include <string.h>

/* Single bank, no hidden second copy. CRC and a last-written commit word make
 * an interrupted import visibly incomplete. Projects occupy separate sectors.
 * The browser retains/downloads a complete bank before replacing one slot. */
struct sample_entry {
    uint32_t offset, frames, bytes;
    char name[16];
};
static struct sample_header {
    uint32_t magic, version, bytes, crc, commit, rate, reserved[2];
    struct sample_entry entry[16];
    uint8_t padding[32];
} header;
_Static_assert(sizeof header == 512, "sample bank header");
static unsigned state, total, position;
static uint32_t checksum, changed_at;
static uint32_t crc32(uint32_t crc, unsigned offset, const uint8_t *p, unsigned bytes)
{
    /* Treat the stored CRC and commit word as zero, including split writes. */
    static const uint8_t zero[8] = {0};
    if (offset < 12) {
        unsigned n = bytes < 12 - offset ? bytes : 12 - offset;
        crc = groove_crc32(crc, p, n);
        p += n; offset += n; bytes -= n;
    }
    if (offset < 20 && bytes) {
        unsigned n = bytes < 20 - offset ? bytes : 20 - offset;
        crc = groove_crc32(crc, zero, n);
        p += n; bytes -= n;
    }
    return groove_crc32(crc, p, bytes);
}
static void unbind(void)
{
    fm1_engine_pause();
    groove_samples_stop();
    for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i)
        (void)sampledrum_bind(i, NULL, 0, 0, NULL);
    fm1_engine_resume();
}
static int valid(void)
{
    if (header.magic != 0x53314d46U || header.version != 1 || header.commit ||
        header.rate != 32000 || header.bytes < sizeof header ||
        header.bytes > GROOVE_SAMPLE_BYTES || header.reserved[0] || header.reserved[1])
        return 0;
    for (unsigned i = 0; i < sizeof header.padding; ++i)
        if (header.padding[i])
            return 0;
    unsigned next = sizeof header;
    for (unsigned i = 0; i < 16; ++i) {
        const struct sample_entry *s = &header.entry[i];
        if (s->offset != next || s->frames > SAMPLE_DRUM_MAX_FRAMES ||
            (i >= SAMPLE_DRUM_PADS && (s->frames || s->name[0])) ||
            s->bytes != ((s->frames + 319U) / 320U) * 228U || s->bytes > header.bytes - next ||
            s->name[15])
            return 0;
        for (unsigned n = 0; n < 15; ++n)
            if ((uint8_t)s->name[n] && (s->name[n] < 32 || s->name[n] > 126))
                return 0;
        next += s->bytes;
    }
    return next == header.bytes;
}
static const uint8_t *mapped_data(void)
{
    const uint8_t *mapped = fm1_flash_map(GROOVE_SAMPLE_OFFSET, total);
    if (!mapped)
        return NULL;
    for (unsigned p = sizeof header; p < total; p += 228U)
        if (mapped[p] != 1 || mapped[p + 1] != 0x45 || mapped[p + 2] != 20 || mapped[p + 3] != 0x5a)
            return NULL;
    return mapped;
}
static int bind(void)
{
    const uint8_t *mapped = mapped_data();
    if (!mapped)
        return GROOVE_PROJECT_INVALID;
    fm1_engine_pause();
    for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i) {
        const struct sample_entry *s = &header.entry[i];
        (void)sampledrum_bind(i, s->frames ? mapped + s->offset : NULL, s->bytes, s->frames,
                              s->name);
    }
    fm1_engine_resume();
    state = 2;
    return 0;
}
void groove_samples_init(void)
{
    /* First of our hooks the SDK calls, before USB and the update handler. */
    boot_guard_start();
    unbind();
    state = total = position = 0;
    checksum = ~0U;
    state = 1;
}
void groove_samples_poll(void)
{
    if (boot_guard_safe()) {
        state = 0; /* SAFE mode: the bank is not read; report "no bank", never "loading" */
        return;
    }
    if (state == 3 && fm1_millis() - changed_at > 10000U)
        groove_samples_cancel();
    if (state != 1)
        return;
    if (!total) {
        if (fm1_flash_size() != GROOVE_FLASH_BYTES) {
            state = 4;
            return;
        }
        int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET, &header, sizeof header);
        if (r == FM1_FLASH_BUSY)
            return;
        if (r) {
            state = 4;
            return;
        }
        if (!valid()) {
            state = 0;
            return;
        }
        total = header.bytes;
    }
    uint8_t bytes[512];
    for (unsigned budget = 0; budget < 8 && position < total; ++budget) {
        unsigned n = total - position;
        if (n > sizeof bytes)
            n = sizeof bytes;
        int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET + position, bytes, n);
        if (r == FM1_FLASH_BUSY) return;
        if (r) {
            state = 4;
            return;
        }
        checksum = crc32(checksum, position, bytes, n);
        position += n;
    }
    if (position == total && (~checksum != header.crc || bind()))
        state = 4;
}
unsigned groove_samples_status(void)
{
    return state;
}
unsigned groove_samples_size(void)
{
    return state == 2 ? total : 0;
}
int groove_samples_begin(unsigned bytes)
{
    if (bytes < sizeof header || bytes > GROOVE_SAMPLE_LIMIT ||
        fm1_flash_size() != GROOVE_FLASH_BYTES)
        return GROOVE_PROJECT_INVALID;
    unbind();
    /* Erase the commit first, before any old sample bytes can be overwritten. */
    groove_edit_record(0, 0, 0);
    fm1_engine_pause();
    fm1_engine_stop();
    fm1_engine_resume();
    int r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET);
    if (r) {
        state = 4;
        return r;
    }
    state = 3;
    total = bytes;
    position = 0;
    checksum = ~0U;
    changed_at = fm1_millis();
    memset(&header, 0, sizeof header);
    return 0;
}
int groove_samples_write(unsigned offset, const uint8_t *data, unsigned bytes)
{
    if (state != 3 || !data || !bytes || bytes > 128 || offset != position ||
        bytes > total - position)
        return GROOVE_PROJECT_INVALID;
    uint8_t copy[128];
    memcpy(copy, data, bytes);
    if (offset < sizeof header) {
        unsigned n = sizeof header - offset;
        if (n > bytes)
            n = bytes;
        memcpy((uint8_t *)&header + offset, data, n);
        for (unsigned i = 0; i < bytes; ++i)
            if (offset + i >= 16 && offset + i < 20)
                copy[i] = 255;
    }
    /* Requests may straddle a sector. Erase only the as-yet-unwritten sector. */
    unsigned first = offset / 4096U, last = (offset + bytes - 1U) / 4096U;
    int r = 0;
    if (offset && !(offset % 4096U))
        r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + first * 4096U);
    if (!r && last != first)
        r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + last * 4096U);
    if (!r)
        r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + offset, copy, bytes);
    if (r) {
        state = 4;
        return r;
    }
    checksum = crc32(checksum, offset, data, bytes);
    position += bytes;
    changed_at = fm1_millis();
    return 0;
}
int groove_samples_finish(void)
{
    if (state != 3 || position != total || header.bytes != total || !valid() ||
        ~checksum != header.crc)
        return GROOVE_PROJECT_INVALID;
    /* Validate codec headers before committing, including data just written
     * through the raw Flash API and not yet visible in the mapped cache. */
    int r = fm1_flash_invalidate(GROOVE_SAMPLE_OFFSET, total);
    if (!r && !mapped_data())
        r = GROOVE_PROJECT_INVALID;
    uint32_t commit = 0;
    if (!r)
        r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + 16, &commit, sizeof commit);
    if (!r)
        r = bind();
    if (r)
        state = 4;
    else groove_fm_library_init();
    return r;
}
void groove_samples_cancel(void)
{
    if (state == 3)
        state = 4;
}
int groove_samples_read(unsigned offset, uint8_t *data, unsigned bytes)
{
    if (state != 2 || !data || !bytes || bytes > 64 || offset > total || bytes > total - offset)
        return GROOVE_PROJECT_INVALID;
    return fm1_flash_read(GROOVE_SAMPLE_OFFSET + offset, data, bytes);
}

/* ---- v7 sampler recording: replace pads in place, in this device ---- */
unsigned groove_samples_pad_frames(unsigned pad)
{
    return state == 2 && pad < SAMPLE_DRUM_PADS ? header.entry[pad].frames : 0;
}
/* Bytes the new data for pads first..first+count-1 may take. */
unsigned groove_samples_room(unsigned first, unsigned count)
{
    if (state != 2 && state != 0 && state != 4) return 0;
    unsigned used = sizeof header;
    if (state == 2)
        for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i)
            if (i < first || i >= first + count) used += header.entry[i].bytes;
    return used < GROOVE_SAMPLE_LIMIT ? GROOVE_SAMPLE_LIMIT - used : 0;
}
/* Bytes pos..pos+n of the new bank: header, kept pads from the old bank
 * (read from Flash at their old place), new pads from RAM, then 0xff. */
static int compose(uint8_t *out, unsigned pos, unsigned n, const struct sample_header *nh,
                   const struct sample_header *old, unsigned first, unsigned count, const struct rec_slice *items)
{
    memset(out, 0xff, n);
    if (pos < sizeof *nh) {
        unsigned k = sizeof *nh - pos < n ? sizeof *nh - pos : n;
        memcpy(out, (const uint8_t *)nh + pos, k);
    }
    for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i) {
        const struct sample_entry *e = &nh->entry[i];
        unsigned a = e->offset > pos ? e->offset : pos, b = e->offset + e->bytes < pos + n ? e->offset + e->bytes : pos + n;
        if (!e->bytes || a >= b) continue;
        if (i >= first && i < first + count) memcpy(out + (a - pos), items[i - first].data + (a - e->offset), b - a);
        else {
            int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET + old->entry[i].offset + (a - e->offset), out + (a - pos), b - a);
            if (r) return r;
        }
    }
    return 0;
}
/* The rewrite runs as a job, one 4 KiB block per control tick, so the
 * control task never stalls and a busy Flash (FM1_FLASH_BUSY, it happens on
 * the device) just waits for the next tick. Phase 1 reads the new bank in
 * address order for its CRC; phase 2 erases and writes sector by sector. Kept
 * pads after the replaced ones move by delta: growing goes from the end,
 * shrinking from the start, so every source byte is read before its sector is
 * erased. Sectors before the first replaced pad stay (sector 0 always: the
 * header, written with commit = ~0 until the very end). */
static struct {
    unsigned active, phase, first, count, next, sectors, keep, k;
    int delta;
    uint32_t crc;
    const struct rec_slice *items;
    uint8_t *buf;
} job;
/* The job's block buffer is static: a save must not depend on free heap
 * (USB audio and the engines may have taken it since the take was armed). */
static uint8_t job_block[4096];
static struct sample_header job_old, job_new;
int groove_samples_replace_begin(unsigned first, unsigned count, const struct rec_slice *items)
{
    if (job.active || !items || !count || first >= SAMPLE_DRUM_PADS || count > SAMPLE_DRUM_PADS - first ||
        state == 1 || state == 3 || fm1_flash_size() != GROOVE_FLASH_BYTES)
        return GROOVE_PROJECT_INVALID;
    if (state == 2) job_old = header;
    else {
        memset(&job_old, 0, sizeof job_old);
        for (unsigned i = 0; i < 16; ++i) job_old.entry[i].offset = sizeof job_old;
    }
    struct sample_header *nh = &job_new;
    *nh = job_old;
    nh->magic = 0x53314d46U; nh->version = 1; nh->rate = 32000; nh->crc = nh->commit = 0;
    nh->reserved[0] = nh->reserved[1] = 0; memset(nh->padding, 0, sizeof nh->padding);
    unsigned next = sizeof *nh;
    for (unsigned i = 0; i < 16; ++i) {
        struct sample_entry *e = &nh->entry[i];
        if (i >= first && i < first + count) {
            const struct rec_slice *it = &items[i - first];
            if (it->bytes != ((it->frames + 319U) / 320U) * 228U || !it->frames || it->frames > SAMPLE_DRUM_MAX_FRAMES)
                return GROOVE_PROJECT_INVALID;
            e->frames = it->frames; e->bytes = it->bytes;
            memset(e->name, 0, sizeof e->name); strncpy(e->name, it->name, 15);
        }
        e->offset = next; next += e->bytes;
    }
    if (next > GROOVE_SAMPLE_LIMIT) return -2; /* bank full */
    nh->bytes = next;
    job.buf = job_block;
    job.active = 1; job.phase = 1; job.first = first; job.count = count; job.items = items;
    job.next = next; job.crc = ~0U; job.k = 0;
    job.delta = (int)nh->entry[first + count].offset - (int)job_old.entry[first + count].offset;
    job.sectors = (next + 4095U) / 4096U; job.keep = job_old.entry[first].offset / 4096U;
    return 0;
}
/* 1 = more to do, 0 = finished and bound, < 0 = failed (the job is over). */
int groove_samples_replace_step(unsigned *out_done, unsigned *out_total)
{
    if (!job.active) return GROOVE_PROJECT_INVALID;
    unsigned blocks = (job.next + 4095U) / 4096U;
    if (out_total) *out_total = 2U * blocks;
    int r = 0;
    if (job.phase == 1) { /* CRC, in address order, before anything is written */
        unsigned pos = job.k * 4096U, n = job.next - pos < 4096U ? job.next - pos : 4096U;
        r = compose(job.buf, pos, n, &job_new, &job_old, job.first, job.count, job.items);
        if (r == FM1_FLASH_BUSY) return 1;
        if (!r) {
            job.crc = crc32(job.crc, pos, job.buf, n);
            if (++job.k == blocks) {
                job_new.crc = ~job.crc;
                job_new.commit = 0xffffffffU; /* written as 0 last: until then the bank reads as unfinished */
                unbind();
                fm1_engine_pause(); fm1_engine_stop(); fm1_engine_resume();
                state = 3; changed_at = fm1_millis();
                job.phase = 2; job.k = 0;
            }
        }
    } else if (job.phase == 2) {
        unsigned j = job.delta > 0 ? job.sectors - 1U - job.k : job.k, pos = j * 4096U;
        if (!j || j >= job.keep) {
            unsigned n = job.next - pos < 4096U ? job.next - pos : 4096U;
            r = compose(job.buf, pos, n, &job_new, &job_old, job.first, job.count, job.items);
            if (!r) r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + pos);
            if (!r) r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + pos, job.buf, n);
            if (r == FM1_FLASH_BUSY) return 1; /* the same sector again next tick (erase is repeatable) */
        }
        changed_at = fm1_millis();
        if (!r && ++job.k == job.sectors) job.phase = 3;
    } else {
        uint32_t commit = 0;
        r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + 16, &commit, sizeof commit);
        if (r == FM1_FLASH_BUSY) return 1;
        if (!r) r = fm1_flash_invalidate(GROOVE_SAMPLE_OFFSET, job.next);
        if (!r) {
            job_new.commit = 0; header = job_new; total = job.next; position = job.next; checksum = job.crc;
            r = bind();
            if (!r) groove_fm_library_init();
        }
        job.buf = NULL; job.active = 0;
        if (r) state = 4;
        return r ? r - 300 : 0; /* -3xx: failed while finishing */
    }
    if (out_done) *out_done = job.phase == 1 ? job.k : job.phase == 2 ? blocks + job.k * blocks / job.sectors : 2U * blocks;
    if (r) {
        job.buf = NULL; job.active = 0;
        if (job.phase >= 2) state = 4; /* the bank was being rewritten: it is unusable until imported again */
        return r - 100 * (int)job.phase; /* -1xx reading, -2xx writing */
    }
    return 1;
}
