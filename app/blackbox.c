/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "blackbox.h"
#include <string.h>

enum { VERSION = 1 };
static const uint32_t MAGIC = 0x58424d46U; /* 'FMBX' */

#if defined(FM1_HOST_TEST) || defined(__wasm__)
static struct bb_record cell;
#define RAM (&cell)
#else
/* sdk/config/linker.ld.in: 32 bytes above the heap, outside .ram0_bss. */
extern struct bb_record fm1_blackbox_ram;
#define RAM (&fm1_blackbox_ram)
#endif

static struct bb_record last, live;
static unsigned started, have_last, event_count, event_head;
static struct bb_event events[BB_EVENTS];
static uint32_t start_ms, updated_ms, seen_overruns, seen_sheds;
static uint8_t high;

/* CRC-32 (reflected, 0xEDB88320), bit by bit: 24 bytes ten times a second. */
static uint32_t crc32(const void *data, unsigned bytes)
{
    const uint8_t *p = data;
    uint32_t c = ~0U;
    while (bytes--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}
static uint32_t checksum(const struct bb_record *r) { return crc32(r, sizeof *r - sizeof r->crc); }
static void store(void)
{
    live.crc = checksum(&live);
    /* Mark the record unreadable while it changes: a reset in the middle of
     * the copy must not leave a mixed record with a matching checksum. */
    volatile struct bb_record *ram = RAM;
    ram->magic = 0;
    struct bb_record copy = live;
    copy.magic = 0;
    memcpy((void *)RAM, &copy, sizeof copy);
    ram->magic = MAGIC;
}
unsigned bb_start(uint32_t now_ms, unsigned safe)
{
    if (started) return have_last;
    started = 1;
    memcpy(&last, RAM, sizeof last);
    have_last = last.magic == MAGIC && last.version == VERSION && last.crc == checksum(&last);
    memset(&live, 0, sizeof live);
    live.magic = MAGIC;
    live.version = VERSION;
    live.control = BB_NO_CONTROL;
    live.flags = safe ? BB_FLAG_SAFE : 0;
    live.restarts = have_last && last.restarts < 65535U ? (uint16_t)(last.restarts + 1U) : 0;
    start_ms = updated_ms = now_ms;
    seen_overruns = seen_sheds = 0;
    event_count = event_head = 0;
    high = 0;
    store();
    return have_last;
}
const struct bb_record *bb_last(void) { return have_last ? &last : 0; }
void bb_control(unsigned control, uint32_t now_ms)
{
    if (!started) return;
    live.control = (uint8_t)control;
    live.control_ms = now_ms - start_ms;
}
static void add_event(unsigned kind, unsigned count)
{
    struct bb_event *e = &events[event_head];
    event_head = (event_head + 1U) % BB_EVENTS;
    if (event_count < BB_EVENTS) ++event_count;
    e->uptime_ms = live.uptime_ms;
    e->kind = (uint8_t)kind;
    e->page = live.page;
    e->control = live.control;
    e->cpu = live.cpu;
    e->count = (uint16_t)(count > 65535U ? 65535U : count);
}
void bb_poll(uint32_t now_ms, unsigned page, unsigned cpu_percent, uint32_t overruns, uint32_t sheds, unsigned flags)
{
    if (!started || now_ms - updated_ms < BB_UPDATE_MS) return;
    updated_ms = now_ms;
    live.uptime_ms = now_ms - start_ms;
    live.page = (uint8_t)page;
    live.cpu = (uint8_t)(cpu_percent > 255U ? 255U : cpu_percent);
    if (live.cpu > live.cpu_peak) live.cpu_peak = live.cpu;
    live.flags = (uint8_t)flags;
    uint32_t dropped = overruns - seen_overruns, shed = sheds - seen_sheds;
    seen_overruns = overruns;
    seen_sheds = sheds;
    live.overruns = (uint16_t)(live.overruns + dropped > 65535U ? 65535U : live.overruns + dropped);
    live.sheds = (uint16_t)(live.sheds + shed > 65535U ? 65535U : live.sheds + shed);
    if (dropped) add_event(BB_EVENT_DROP, dropped);
    if (shed) add_event(BB_EVENT_SHED, shed);
    /* One entry per climb over the line, not one per update while it stays there. */
    if (live.cpu >= BB_HIGH_PERCENT) { if (!high) { high = 1; add_event(BB_EVENT_HIGH, 1); } }
    else if (live.cpu + 10U < BB_HIGH_PERCENT) high = 0;
    store();
}
const struct bb_event *bb_event(unsigned index)
{
    if (index >= event_count) return 0;
    return &events[(event_head + BB_EVENTS - 1U - index) % BB_EVENTS];
}
static uint8_t *put(uint8_t *p, uint32_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) *p++ = (uint8_t)(v >> (8U * i));
    return p;
}
static void put_record(uint8_t *p, const struct bb_record *r)
{
    p = put(p, r->uptime_ms, 4); p = put(p, r->control_ms, 4);
    p = put(p, r->restarts, 2); p = put(p, r->overruns, 2); p = put(p, r->sheds, 2);
    *p++ = r->page; *p++ = r->control; *p++ = r->cpu; *p++ = r->cpu_peak; *p++ = r->flags; *p = r->version;
}
unsigned bb_read(unsigned part, uint8_t out[BB_READ_BYTES])
{
    if (part >= BB_READ_PARTS) return 0;
    memset(out, 0, BB_READ_BYTES);
    out[0] = BB_READ_VERSION; out[1] = (uint8_t)part;
    out[2] = (uint8_t)((started ? 1U : 0U) | (started && have_last ? 2U : 0U));
    out[3] = (uint8_t)(started ? event_count : 0U);
    if (!started) return BB_READ_BYTES;
    if (!part) {
        put_record(out + 4, &live);
        if (have_last) put_record(out + 24, &last);
        return BB_READ_BYTES;
    }
    for (unsigned k = 0; k < 4; ++k) {
        const struct bb_event *e = bb_event((part - 1U) * 4U + k);
        uint8_t *p = out + 4 + k * 10U;
        if (!e) continue;
        p = put(p, e->uptime_ms, 4);
        *p++ = e->kind; *p++ = e->page; *p++ = e->control; *p++ = e->cpu;
        put(p, e->count, 2);
    }
    return BB_READ_BYTES;
}
#ifdef FM1_HOST_TEST
struct bb_record *bb_test_ram(void) { return RAM; }
void bb_test_reset(void) { started = have_last = 0; }
#endif
