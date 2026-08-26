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
 *   • CANONICAL WIRE ORDER (interop v1, 2026-08-26): RAM-side wire เก็บ
 *     call order + owner array; serialize เรียง canonical = block id ขึ้น,
 *     ใน block เดียวคือ append order → disk layout deterministic ทุกลำดับ
 *     lift (ก่อนหน้านี้ replay map sorted-position ↔ call-order byte —
 *     lift คนละ block ไม่เรียง = อ่าน Δ ของ block อื่น แก้แล้ว)
 *   • wire = {q:3b|dc:3b|dx:2b} 1 B FREE (fg_crt bijection, home tooth
 *     Δ=0 ถูกปฏิเสธที่ encoder)
 *   • flag GHOST_FLAG_GEAR (0x08, บิตว่างของ entry) ประกาศ "entry นี้
 *     มี wire byte"
 *   • SEAL invariant (interop v1): non-seal wire bytes == geared entries
 *     (0..k seals ถูกต้องทั้งหมด — เดิมบังคับ geared+1 เฉพาะ expire เดียว,
 *      container ไม่เคย expire โหลดไม่ได้ = bug ที่ suite หลุด)
 *
 * SERIALIZATION: [GHST blob][canonical wire] — reader เก่าอ่าน GHST part
 * ได้เหมือนเดิม (backward compatible), reader ใหม่ใช้ count เป็น offset
 * หา wire (derived layout — ไม่มี TOC).
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

/* seal "from" sentinel in owner keys: greater than any real from_scale
 * (u8) so a block's seals canonically sort after its events */
#define GHOST_GEAR_SEAL_FROM 256u

/* ── Gear side-table: wire bytes for a log ─────────────────────────────── */
typedef struct {
    uint8_t  wire[GHOST_LOG_MAX];   /* 1 B per geared entry + seals       */
    uint32_t owner[GHOST_LOG_MAX];  /* per byte: (block<<16)|from;
                                       seals: (block<<16)|SEAL_FROM       */
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
    FGGearEv e = fg_enc(from_scale, to_scale);
    gw->wire[gw->n] = (uint8_t)(e.q | ((uint8_t)(e.dc) << 3) |
                                     ((uint8_t)(e.dx) << 6));
    gw->owner[gw->n] = ((uint32_t)block_id << 16) | (uint32_t)from_scale;
    gw->n++;
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
    if (expired && gw->n < GHOST_LOG_MAX) {
        gw->wire[gw->n] = GHOST_GEAR_SEAL;
        gw->owner[gw->n] = ((uint32_t)block_id << 16) | GHOST_GEAR_SEAL_FROM;
        gw->n++;
    }
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
    (void)log;   /* values come from the wire alone — log not read */
    if (!gw || !out_to || cap == 0) return (uint32_t)-1;
    /* owner-tracked: this block's bytes in call order (index ascending).
     * Order-safe for ANY lift interleaving — no sorted-position mapping. */
    uint32_t w = birth, hops = 0;
    for (uint16_t k = 0; k < gw->n && hops < cap; k++) {
        if ((gw->owner[k] >> 16) != (uint32_t)block_id) continue;
        uint8_t b = gw->wire[k];
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
    /* CANONICAL ORDER: blocks ascending by id, each block's bytes in
     * append (call) order. Insertion sort of indices — n ≤ 4096, persist
     * is rare; allocation-free and deterministic. */
    uint16_t idx[GHOST_LOG_MAX];
    for (uint16_t k = 0; k < gw->n; k++) idx[k] = k;
    for (uint16_t i = 1; i < gw->n; i++) {
        uint16_t x = idx[i];
        uint32_t kb = gw->owner[x] >> 16;
        uint16_t j = i;
        while (j > 0 && (gw->owner[idx[j - 1]] >> 16) > kb) {
            idx[j] = idx[j - 1]; j--;
        }
        idx[j] = x;
    }
    uint8_t *out = (uint8_t *)buf + base;
    for (uint16_t k = 0; k < gw->n; k++) out[k] = gw->wire[idx[k]];
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
    const uint8_t *wb = (const uint8_t *)buf + base;
    memcpy(gw->wire, wb, (size_t)wlen);
    gw->n = (uint16_t)wlen;
    /* SEAL invariant (interop v1): non-seal bytes == geared-flagged
     * entries (EXPIRED keep their flag — audit trail owns its byte).
     * 0..k seals all valid; geared+1-only was the old off-by-one that
     * rejected never-expired containers. Mismatch → loud corrupt.       */
    uint32_t geared = 0, evs = 0;
    for (uint32_t i = 0; i < log->count; i++)
        if (log->entries[i].flags & GHOST_FLAG_GEAR) geared++;
    for (uint64_t k = 0; k < wlen; k++)
        if (wb[k] != GHOST_GEAR_SEAL) evs++;
    if (evs != geared) return -2;
    /* reconstruct owners from the canonical layout: k-th event byte ↔
     * k-th geared entry (sorted); seal inherits the preceding event's
     * block as tail (SEAL_FROM ranks it after that block's events).      */
    uint32_t g = 0, last_block = 0;
    for (uint64_t k = 0; k < wlen; k++) {
        if (wb[k] == GHOST_GEAR_SEAL) {
            gw->owner[k] = (last_block << 16) | GHOST_GEAR_SEAL_FROM;
            continue;
        }
        while (g < log->count &&
               !(log->entries[g].flags & GHOST_FLAG_GEAR)) g++;
        if (g >= log->count) return -2;         /* more events than flags */
        last_block = log->entries[g].block_id;
        gw->owner[k] = ((uint32_t)last_block << 16) |
                       log->entries[g].from_scale;
        g++;
    }
    return 0;
}

#endif /* GEO_GHOST_GEAR_ADAPTER_H */
