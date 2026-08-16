/*
 * geo_ghost_lift.h — Ghost Lift: hyperbolic track wired into residual_space
 * ═══════════════════════════════════════════════════════════════════════════
 * Rescope working model (§5.2 passive log + §15.2 ghost):
 *
 *   Block requests scale beyond its envelope (ROI cliff at k ≥ 4–5) →
 *   instead of expanding the field (ทุกอย่างถูกลาก — global scale เดียว),
 *   FREEZE the block's data into residual_space and let the hyperbolic
 *   log TRACK the ghost.  Time/scale is cut from the data (freeze) —
 *   access goes through the ghost via delta.
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │  bond = BIRTH IDENTITY   (block_id, from_scale)            │
 *   │  log  = ROUTE            {block_id, from→to} = 5 B/event   │
 *   └────────────────────────────────────────────────────────────┘
 *
 * Mapping rule (the "address"):
 *   origin_seed = fibo_addr((block_id << 8) | from_scale)
 *   piece       = pogls_make_piece(origin_seed, axis(to_scale))
 *   bond_key    = pogls_bond_key(piece) = bond_L XOR bond_R
 *
 *   - from_scale is PART of the address → เสาเข็มห้ามขยับ: read with a
 *     different from_scale → bond_key changes → thaw/verify fail
 *     automatically.  No lookup table, coordinate-bound by construction.
 *   - to_scale (the route) is NOT part of the bond → same frozen data can
 *     be reached by several routes (วางครั้งเดียว ไม่เคยเขียนซ้ำ — T8b).
 *   - axis(to_scale) = route flavor (I/O/T/S/Z/L/J); shape does NOT affect
 *     bond_key — bond stays pure birth identity.
 *
 * Re-attach (เสาเข็มใหม่ + reroute link, §15.2):
 *   block re-homes at the target scale → OLD pile's ghosts die via
 *   rs_expire_by_origin (data freed, log entries kept as audit trail,
 *   flagged GHOST_FLAG_EXPIRED — same idea as residual tombstones).
 *
 * Delta ∝ events, not data: one 5 B entry per lift; telescope — one entry
 * covers ANY distance on the scale axis (from 3 → to 140 is 1 entry,
 * not 137 steps).
 *
 * All header-only, static inline, int + fibo mixing only (no float,
 * no trig, no table — MAP not COMPRESS).
 */

#ifndef GEO_GHOST_LIFT_H
#define GEO_GHOST_LIFT_H

#include <stdint.h>
#include <string.h>
#include "residual_space.h"
#include "pogls_bond.h"
#include "geo_ghost_envelope.h"

/* ── Constants ─────────────────────────────────────────────── */
#define GHOST_LOG_MAX      4096u    /* max routes in one log — สมมาตร
                                     * กับ RS_DEFAULT_CAPACITY 4096    */
#define GHOST_SEED_MAGIC   UINT64_C(0x0D0C0A1100000000) /* "DWGLS"  */
#define GHOST_AXIS_SHAPES  7u       /* I O T S Z L J (fold 1..7)    */

/* ── Entry flags ───────────────────────────────────────────── */
#define GHOST_FLAG_LIFT    0x01u    /* live lift (frozen + tracked) */
#define GHOST_FLAG_EXPIRED 0x02u    /* re-attached — audit trail    */

/* ════════════════════════════════════════════════════════════
   GHOST LOG ENTRY — the passive scale-change record (5 B)
   ════════════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) {
    uint16_t block_id;    /* which pile this ghost belongs to      */
    uint8_t  from_scale;  /* birth scale w0 (envelope floor)       */
    uint8_t  to_scale;    /* requested scale — why it was lifted   */
    uint8_t  flags;       /* GHOST_FLAG_*                          */
} GhostLogEntry;          /* 5 B total — delta ∝ events, not data  */

typedef struct {
    GhostLogEntry entries[GHOST_LOG_MAX];
    uint32_t      count;
} GhostLog;

/* ════════════════════════════════════════════════════════════
   BOND_KEY MAPPING — ghost log entry → piece → bond_key
   ════════════════════════════════════════════════════════════ */

/* origin seed: birth pile identity packed + mixed (fibo = one-way) */
static inline uint64_t ghost_origin_seed(uint16_t block_id, uint8_t from_scale) {
    uint64_t packed = ((uint64_t)block_id << 8) | (uint64_t)from_scale;
    return pogls_fibo_addr(packed ^ GHOST_SEED_MAGIC);
}

/* fold axis = route flavor of the destination (1..7 → I..J);
   does NOT affect bond_key — bond is birth identity only */
static inline uint8_t ghost_fold_axis(uint8_t to_scale) {
    return (uint8_t)((to_scale % GHOST_AXIS_SHAPES) + 1u);
}

/* piece of the ghost — same (block_id, from_scale) always → same piece */
static inline PoglsPiece ghost_piece(uint16_t block_id, uint8_t from_scale,
                                     uint8_t to_scale) {
    return pogls_make_piece(ghost_origin_seed(block_id, from_scale),
                            ghost_fold_axis(to_scale));
}

/* bond_key = deterministic address of the ghost's birth pile */
static inline uint64_t ghost_bond_key(uint16_t block_id, uint8_t from_scale,
                                      uint8_t to_scale) {
    PoglsPiece p = ghost_piece(block_id, from_scale, to_scale);
    return pogls_bond_key(&p);
}

/* ════════════════════════════════════════════════════════════
   LOG ACCESS
   ════════════════════════════════════════════════════════════ */

static inline void ghost_log_init(GhostLog *log) {
    if (!log) return;
    memset(log, 0, sizeof(GhostLog));
}

/* find a LIVE route (block_id, from→to). Returns index or -1.
   Expired entries are kept for audit but never matched. */
static inline int ghost_log_find(const GhostLog *log, uint16_t block_id,
                                 uint8_t from_scale, uint8_t to_scale) {
    if (!log) return -1;
    for (uint32_t i = 0; i < log->count; i++) {
        const GhostLogEntry *e = &log->entries[i];
        if (e->flags & GHOST_FLAG_EXPIRED) continue;
        if (e->block_id == block_id && e->from_scale == from_scale &&
            e->to_scale == to_scale)
            return (int)i;
    }
    return -1;
}

/* number of routes recorded for a block (live + expired = audit trail) */
static inline uint32_t ghost_route_count(const GhostLog *log, uint16_t block_id) {
    if (!log) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < log->count; i++)
        if (log->entries[i].block_id == block_id) n++;
    return n;
}

/* ════════════════════════════════════════════════════════════
   LIFT — freeze block data as ghost + record the route
   ════════════════════════════════════════════════════════════
   Returns bond_key on success, 0 (RS_BOND_KEY_RESERVED) on failure.
   Data is frozen ONCE per birth pile; lifting to another to_scale
   appends a route to the SAME frozen data (ไม่เคยเขียนซ้ำ). */
static inline uint64_t ghost_lift(GhostLog *log, ResidualSpace *rs,
                                  uint16_t block_id, uint8_t from_scale,
                                  uint8_t to_scale,
                                  const void *data, uint32_t size) {
    if (!log || !rs || !data || size == 0)
        return RS_BOND_KEY_RESERVED;
    if (log->count >= GHOST_LOG_MAX)
        return RS_BOND_KEY_RESERVED;    /* log full — reserve first */
    if (ghost_log_find(log, block_id, from_scale, to_scale) >= 0)
        return RS_BOND_KEY_RESERVED;    /* route already exists */

    PoglsPiece p = ghost_piece(block_id, from_scale, to_scale);
    uint64_t bk = rs_freeze(rs, &p, data, size, 0);
    if (bk == RS_BOND_KEY_RESERVED)
        return RS_BOND_KEY_RESERVED;

    GhostLogEntry *e = &log->entries[log->count++];
    e->block_id   = block_id;
    e->from_scale = from_scale;
    e->to_scale   = to_scale;
    e->flags      = GHOST_FLAG_LIFT;
    return bk;
}

/* ════════════════════════════════════════════════════════════
   READ — derive bond from (block_id, from_scale), require a live
   route to (to_scale), then thaw.  Two-layer truth:
     wrong from_scale → bond breaks (NULL)
     wrong to_scale   → route not found (NULL)
   ════════════════════════════════════════════════════════════ */
static inline const void *ghost_read(const GhostLog *log, const ResidualSpace *rs,
                                     uint16_t block_id, uint8_t from_scale,
                                     uint8_t to_scale, uint32_t *out_size) {
    if (!log || !rs) return NULL;
    if (ghost_log_find(log, block_id, from_scale, to_scale) < 0)
        return NULL;                     /* route is the authority  */

    PoglsPiece p = ghost_piece(block_id, from_scale, to_scale);
    return rs_thaw(rs, pogls_bond_key(&p), out_size);
}

/* ════════════════════════════════════════════════════════════
   AUTO LIFT — envelope gate (§11.6 — decided)
   ════════════════════════════════════════════════════════════
   Requested expansion depth > envelope_depth(gate) → FREEZE data as
   ghost + record the route (instead of expanding the pile — ย่อฟรี
   ขยายจ่าย; ROI cliff at k=4-5, knob = gate).

   Returns GHOST_AUTO_PLACE (within envelope — normal placement),
   GHOST_AUTO_LIFT (lifted), or GHOST_AUTO_ERR. */
#define GHOST_AUTO_PLACE 0
#define GHOST_AUTO_LIFT  1
#define GHOST_AUTO_ERR   (-1)

static inline int ghost_lift_auto(GhostLog *log, ResidualSpace *rs,
                                  double gate, uint16_t block_id,
                                  uint8_t from_scale, uint8_t to_scale,
                                  const void *data, uint32_t size) {
    if (!log || !rs || !data || size == 0)
        return GHOST_AUTO_ERR;
    if (ght_scale_depth(from_scale, to_scale) <= ght_envelope_depth(gate))
        return GHOST_AUTO_PLACE;    /* within envelope — field placement */
    uint64_t bk = ghost_lift(log, rs, block_id, from_scale, to_scale,
                             data, size);
    return (bk != RS_BOND_KEY_RESERVED) ? GHOST_AUTO_LIFT : GHOST_AUTO_ERR;
}

/* ════════════════════════════════════════════════════════════
   EXPIRE — re-attach: kill the OLD pile's ghosts
   ════════════════════════════════════════════════════════════
   Uses rs_expire_by_origin with the derived geo_key (birth pile
   identity).  Frozen data is tombstoned; log entries stay as audit
   trail flagged GHOST_FLAG_EXPIRED.  Returns number of routes expired. */
static inline uint32_t ghost_expire(GhostLog *log, ResidualSpace *rs,
                                    uint16_t block_id, uint8_t from_scale) {
    if (!log || !rs) return 0;

    PoglsPiece p = ghost_piece(block_id, from_scale, 0);
    rs_expire_by_origin(rs, p.geo_key);   /* data dies by birth pile */

    uint32_t expired = 0;
    for (uint32_t i = 0; i < log->count; i++) {
        GhostLogEntry *e = &log->entries[i];
        if (e->block_id == block_id && e->from_scale == from_scale &&
            !(e->flags & GHOST_FLAG_EXPIRED)) {
            e->flags |= GHOST_FLAG_EXPIRED;
            expired++;
        }
    }
    return expired;
}

#endif /* GEO_GHOST_LIFT_H */
