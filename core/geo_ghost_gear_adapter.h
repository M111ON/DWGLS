/*
 * geo_ghost_gear_adapter.h — Gear wire attached to the GhostLog core
 * ════════════════════════════════════════════════════════════════════
 *
 * SWAP (2026-08-26, step ③): ghost_lift/ghost_expire ใน core เดิม
 * append route {from,to} = 2 B/event — ตอนนี้ adapter พัน core จริง:
 *
 *   WRITE: ghost_gear_lift()      → ghost_lift() + gear wire append
 *          ghost_gear_expire()    → ghost_expire() + wire seal
 *   READ : ghost_gear_replay()    → Δ-chain replay จาก WIRE ONLY
 *          (ไม่อ่าน entry fields เลย — พิสูจน์ wire ครบ)
 *
 * การผูก:
 *   • bond (block_id, from_scale) ไม่ถูกแตะ — birth identity/RDH address
 *     ยังอยู่ใน entry fields เดิม (P4 ของ probe: wire พึ่ง Delta เท่านั้น)
 *   • per-block chain: entries ของ block เดียว sorted by from (b-bond
 *     principle ของ log) → chain f0→f1→...→fk deterministic
 *   • wire = {q:3b|dc:3b|dx:2b} 1 B FREE (fg_crt bijection, home tooth
 *     Δ=0 ถูกปฏิเสธที่ encoder)
 *   • flag GHOST_FLAG_GEAR (0x08, บิตว่างของ entry) ประกาศ "entry นี้
 *     มี wire byte" — side table จับคู่ sorted-order ↔ wire index
 *
 * SERIALIZATION: wire bytes วางต่อท้าย GHST blob เดิม — reader เก่า
 * อ่าน header+entries ได้เหมือนเดิม (backward compatible), reader ใหม่
 * ใช้ count เป็น offset หา wire ได้ทันที (derived layout — ไม่มี TOC).
 *
 * DEPENDS: fan24_gear.h (fg_enc/fg_dec/FGGearEv), geo_ghost_lift.h
 */
#ifndef GEO_GHOST_GEAR_ADAPTER_H
#define GEO_GHOST_GEAR_ADAPTER_H

#include <stdint.h>
#include <string.h>
#include "fan24_gear.h"
#include "geo_ghost_lift.h"

/* flag bit 0x08 was free in GhostLogEntry.flags (LIFT 01 / EXPIRED 02 /
 * DELTA 04) — declaring it here keeps the entry at 5 B. */
#define GHOST_FLAG_GEAR 0x08u

/* seal marker: wire byte 0xFF is unreachable by fg_enc (dc<<3 max 111,
 * dx<<6 max 10 → bit7 always 0) → free as an explicit chain terminator */
#define GHOST_GEAR_SEAL 0xFFu

/* ── Gear side-table: wire bytes for a log ─────────────────────────────── */
typedef struct {
    uint8_t  wire[GHOST_LOG_MAX];   /* 1 B per geared entry + seals       */
    uint16_t n;                     /* wired entries (+ seals)            */
} GhostGearWire;

/* ════════════════════════════════════════════════════════════════════
 * WRITE SIDE — wrap the real core lift/expire
 * ════════════════════════════════════════════════════════════════════ */

/* Lift through the core, then record the route on the gear wire.
 * Returns bond_key of the frozen data (same contract as ghost_lift).
 * Wire append failure cannot happen when caller checks gw->n < MAX first
 * (same reserve pattern as the core log itself). */
static inline uint64_t ghost_gear_lift(GhostLog *log, ResidualSpace *rs,
                                       GhostGearWire *gw,
                                       uint16_t block_id, uint8_t from_scale,
                                       uint8_t to_scale,
                                       const void *data, uint32_t size) {
    if (!log || !rs || !data || size == 0 || !gw) return RS_BOND_KEY_RESERVED;
    if (gw->n >= GHOST_LOG_MAX) return RS_BOND_KEY_RESERVED;   /* reserve */

    /* 1) real core lift — bond identity untouched */
    uint64_t bk = ghost_lift(log, rs, block_id, from_scale, to_scale,
                             data, size);
    if (bk == RS_BOND_KEY_RESERVED) return bk;

    /* 2) locate the inserted entry (sorted position) and stamp the flag */
    int pos = _ghost_pile_lo(log, block_id, from_scale);
    if (pos < 0 || pos >= (int)log->count) return bk;  /* lifted anyway */
    log->entries[pos].flags |= GHOST_FLAG_GEAR;

    /* 3) wire append — Δ-only event (home tooth impossible: core dedups
     *    same-(block,from,to); from==to would have found it)           */
    gw->wire[gw->n++] = (uint8_t)(fg_enc(from_scale, to_scale).q |
                                  ((fg_enc(from_scale, to_scale).dc) << 3) |
                                  ((fg_enc(from_scale, to_scale).dx) << 6));
    return bk;
}

/* Expire through the real core, then SEAL the chain on the wire.
 * The seal marks "no live hop beyond this point" so a replay stops at
 * the last live hop instead of walking into expired territory. */
static inline uint32_t ghost_gear_expire(GhostLog *log, ResidualSpace *rs,
                                         GhostGearWire *gw,
                                         uint16_t block_id, uint8_t from_scale) {
    if (!log || !rs || !gw) return 0;
    uint32_t expired = ghost_expire(log, rs, block_id, from_scale);
    if (expired && gw->n < GHOST_LOG_MAX)
        gw->wire[gw->n++] = GHOST_GEAR_SEAL;
    return expired;
}

/* ════════════════════════════════════════════════════════════════════
 * READ SIDE — replay from WIRE ONLY (block-scoped)
 * ════════════════════════════════════════════════════════════════════ */

/* Walk ONE block's chain using the wire + the log's sorted structure.
 * The log supplies only the block's WIRE SPAN (which byte range belongs
 * to this block — derived from sorted order, not read for values); the
 * chain VALUES come from the gear events alone:
 *   birth     : the block's birth scale f0 (reader state — ENTER
 *               ANYWHERE doctrine: position lives in the reader)
 *   out_to[]  : filled with to_scale sequence f1..fk
 *   returns hops decoded, or (uint32_t)-1 on bad input
 * A seal inside the span stops the walk (expired chain tail). */
static inline uint32_t ghost_gear_replay(const GhostLog *log,
                                         const GhostGearWire *gw,
                                         uint16_t block_id, uint8_t birth,
                                         uint8_t *out_to, uint32_t cap) {
    if (!log || !gw || !out_to || cap == 0) return (uint32_t)-1;
    /* wire index of this block's first geared entry = #geared entries
       strictly BEFORE the pile in sorted order (derived, value-free)    */
    int lo = _ghost_pile_lo(log, block_id, 0);
    uint32_t wire_lo = 0;
    for (int i = 0; i < lo; i++)
        if (log->entries[i].flags & GHOST_FLAG_GEAR) wire_lo++;
    /* span length = geared entries of THIS pile                        */
    uint32_t span = 0;
    for (int i = lo; i < (int)log->count; i++) {
        if (log->entries[i].block_id != block_id) break;
        if (log->entries[i].flags & GHOST_FLAG_GEAR) span++;
    }
    uint32_t w = birth, hops = 0;
    for (uint32_t k = 0; k < span && hops < cap; k++) {
        uint8_t b = gw->wire[wire_lo + k];
        if (b == GHOST_GEAR_SEAL) break;
        FGGearEv e;
        e.q  = (uint8_t)(b & 7u);
        e.dc = (uint8_t)((b >> 3) & 7u);
        e.dx = (uint8_t)((b >> 6) & 3u);
        w = fg_dec(w, e);
        out_to[hops++] = (uint8_t)w;
    }
    return hops;
}

/* ════════════════════════════════════════════════════════════════════
 * SERIALIZATION — wire rides AFTER the GHST blob (derived layout)
 * ════════════════════════════════════════════════════════════════════
 * [GHST blob: 12B hdr + count×5B entries][wire: n bytes]
 * Old readers read the GHST part unchanged; new readers take
 * off = ghost_log_serialize_size(log) as the wire base offset. */

static inline uint64_t ghost_gear_serialize(const GhostLog *log,
                                            const GhostGearWire *gw,
                                            void *buf, uint64_t cap) {
    if (!log || !gw || !buf) return 0;
    uint64_t base = ghost_log_serialize_size(log);
    uint64_t need = base + gw->n;
    if (cap < need) return 0;
    uint64_t wrote = ghost_log_serialize(log, buf, base);
    if (wrote != base) return 0;
    memcpy((uint8_t *)buf + base, gw->wire, gw->n);
    return need;
}

static inline int ghost_gear_load(GhostLog *log, GhostGearWire *gw,
                                  const void *buf, uint64_t size) {
    if (!log || !gw || !buf) return -1;
    if (size < 12) return -1;
    /* GHST layout: [0..3] magic [4..5] ver [6..7] resv [8..11] count u32.
     * Derive the GHST-part size from the HEADER count (not the caller's
     * log — it is empty before load; using it here was a real bug).      */
    uint32_t hdr_count;
    memcpy(&hdr_count, (const uint8_t *)buf + 8, 4);
    if (hdr_count > GHOST_LOG_MAX) return -1;
    uint64_t base = 12u + (uint64_t)hdr_count * sizeof(GhostLogEntry);
    if (size < base) return -1;
    if (ghost_log_load(log, buf, base) != 0) return -1;
    memset(gw, 0, sizeof(*gw));
    uint64_t wlen = size - base;
    if (wlen > GHOST_LOG_MAX) wlen = GHOST_LOG_MAX;
    memcpy(gw->wire, (const uint8_t *)buf + base, (size_t)wlen);
    gw->n = (uint16_t)wlen;
    /* geared entries must match wired bytes exactly (seals included).
     * EXPIRED entries KEEP their gear flag (audit trail) — they still
     * own a wire byte, so count flags, not live routes.                  */
    uint32_t geared = 0;
    for (uint32_t i = 0; i < log->count; i++)
        if (log->entries[i].flags & GHOST_FLAG_GEAR) geared++;
    if ((uint32_t)gw->n != geared + 1u) return -2;  /* +1 = trailing seal */
    return 0;
}

#endif /* GEO_GHOST_GEAR_ADAPTER_H */
