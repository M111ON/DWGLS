/* kv_lease.h — reserved blocks with generations, taken from the three proposals
 * ═════════════════════════════════════════════════════════════════════════════
 *
 * What survived the diff against what this project already does:
 *
 *   ✗ RCU                        nothing is deleted here, so no reader ever
 *                                holds bytes a pruner wants (see doc §1.2)
 *   ✗ git-style merge            one append-only chain, the lock IS the order
 *   ✓ generation counter         the one genuinely new mechanism: a reference
 *                                that outlived its block must fail LOUD
 *   ✓ lease + reap as CLOSE      first delete-like action in the system, so it
 *                                closes the path and keeps the bytes
 *   ✓ waste measured as AGE      a percentage gate lets one huge stale
 *                                reservation pass; age cannot
 *   ✓ quota with hysteresis      one step per round, sustained two rounds
 *
 * Units are FIELD SLOTS (0..20735), never bytes: reserving an address spends
 * nothing (#6192), so "speculative waste" means reserved-but-unused *claims*,
 * not disk. One tesseract is 1152 slots = 5.6% of the field (core/iso_fold.h),
 * one anchor is 128 slots — so a 1-3% reserve is 2-5 anchors, not a tesseract.
 *
 * Lifetime: RESERVED -> ACTIVE -> CLOSED.
 *   - generation bumps on reserve-from-free, close, reap and reuse;
 *   - every handle carries the generation it was issued with;
 *   - a handle whose generation != the block's is STALE: read/write/beat refuse
 *     it and report both generations. Nothing is guessed.
 *
 * Deterministic by construction: no clock of its own (every entry point takes
 * `now_ms`), no allocation, no globals. Same call sequence -> same trace.
 */
#ifndef KV_LEASE_H
#define KV_LEASE_H

#include <stdint.h>
#include <string.h>

#define KVB_MAX_BLOCKS 32

typedef enum { KVB_FREE = 0, KVB_RESERVED = 1, KVB_ACTIVE = 2, KVB_CLOSED = 3 } kvb_state;

typedef struct {
    int32_t   owner;          /* subagent/region id; 0 = the parent itself */
    int32_t   slots;          /* reserved size, in field slots */
    int32_t   written_slots;  /* payload actually written into the reservation */
    uint32_t  generation;     /* handle-generation; 0 only while FREE */
    int64_t   reserved_ms;
    int64_t   beat_ms;        /* last heartbeat; reaping is measured off this */
    int64_t   closed_ms;
    kvb_state state;
} kvb_block;

typedef struct {
    kvb_block b[KVB_MAX_BLOCKS];
    int32_t   total;          /* slots that exist (e.g. 20736) */
    int32_t   held;           /* slots currently RESERVED or ACTIVE */
    int32_t   main_floor;     /* slots main may never lose to a borrow */
    int32_t   quota;          /* current per-block quota, in slots */
    int32_t   quota_min;
    int32_t   quota_max;
    int64_t   lease_ms;       /* reap after this long without a heartbeat */

    /* measurement (never drives behaviour except the hysteresis counters) */
    int32_t   borrows;
    int32_t   reap_events;
    int32_t   quota_steps;
    int32_t   over_rounds;
    int32_t   under_rounds;
    int32_t   round;
} kvb_table;

/* A handle is just (block index, generation). Small, copyable, stale-able. */
typedef struct { int32_t idx; uint32_t generation; } kvb_ref;

enum {
    KVB_OK       =  0,
    KVB_EFULL    = -1,
    KVB_ESTALE   = -2,   /* handle's generation != block's */
    KVB_EQUOTA   = -3,   /* over quota and borrowing not allowed */
    KVB_EFLOOR   = -4,   /* borrow would cut into main's floor */
    KVB_ESTATE   = -5,   /* wrong state for the operation */
    KVB_EARGS    = -6
};

static inline void kvb_init(kvb_table *t, int32_t total, int32_t quota,
                            int32_t quota_min, int32_t quota_max,
                            int32_t main_floor, int64_t lease_ms) {
    memset(t, 0, sizeof(*t));
    t->total = total;
    t->quota = quota;
    t->quota_min = quota_min ? quota_min : 1;
    t->quota_max = quota_max ? quota_max : total;
    t->main_floor = main_floor;
    t->lease_ms = lease_ms;
}

/* Why a handle is refused: fill `seen` with the block's CURRENT generation so
 * the caller can print both numbers instead of "stale reference". */
static inline int kvb_check(const kvb_table *t, kvb_ref r, uint32_t *seen) {
    if (r.idx < 0 || r.idx >= KVB_MAX_BLOCKS) return KVB_EARGS;
    if (seen) *seen = t->b[r.idx].generation;
    if (t->b[r.idx].generation != r.generation) return KVB_ESTALE;
    if (t->b[r.idx].state == KVB_CLOSED || t->b[r.idx].state == KVB_FREE) return KVB_ESTALE;
    return KVB_OK;
}

/* Reserve `want` slots. Over quota needs an explicit borrow, and a borrow is
 * refused when it would eat main's floor — main never suffers for a subagent. */
static inline int kvb_reserve(kvb_table *t, int32_t owner, int32_t want, int64_t now_ms,
                              int allow_borrow, kvb_ref *out) {
    if (!out || want <= 0 || want > t->total) return KVB_EARGS;
    int idx = -1;
    for (int i = 0; i < KVB_MAX_BLOCKS; i++)
        if (t->b[i].state == KVB_FREE || t->b[i].state == KVB_CLOSED) { idx = i; break; }
    if (idx < 0) return KVB_EFULL;

    /* The quota is a SUBAGENT budget: main (owner 0) is not quota-limited, it is
     * the thing the floor protects. Applying the quota to main made main's own
     * reservation fail with EQUOTA while holding the floor — measured. */
    const int is_main = (owner == 0);
    if (!is_main && want > t->quota && !allow_borrow) return KVB_EQUOTA;

    int32_t free_slots = t->total - t->held;
    if (want > free_slots) return KVB_EFULL;
    if (!is_main && want > t->quota) {          /* borrow path */
        if (free_slots - want < t->main_floor) return KVB_EFLOOR;
        t->borrows++;
    }

    kvb_block *b = &t->b[idx];
    b->owner         = owner;
    b->slots         = want;
    b->written_slots = 0;
    b->generation    = b->generation + 1 ? b->generation + 1 : 1;
    b->reserved_ms   = now_ms;
    b->beat_ms       = now_ms;
    b->closed_ms     = 0;
    b->state         = KVB_RESERVED;
    t->held += want;
    out->idx = idx;
    out->generation = b->generation;
    return KVB_OK;
}

/* Write into a reservation: the generation must still match. A stale handle
 * NEVER writes, because a write through a stale handle is silent corruption of
 * whoever owns the block now. */
static inline int kvb_write(kvb_table *t, kvb_ref r, int32_t slots, int64_t now_ms) {
    int rc = kvb_check(t, r, NULL);
    if (rc != KVB_OK) return rc;
    kvb_block *b = &t->b[r.idx];
    if (slots < 0 || b->written_slots + slots > b->slots) return KVB_EARGS;
    b->written_slots += slots;
    if (b->state == KVB_RESERVED) b->state = KVB_ACTIVE;
    b->beat_ms = now_ms;
    return KVB_OK;
}

static inline int kvb_beat(kvb_table *t, kvb_ref r, int64_t now_ms) {
    int rc = kvb_check(t, r, NULL);
    if (rc != KVB_OK) return rc;
    t->b[r.idx].beat_ms = now_ms;
    return KVB_OK;
}

/* Reap leases that stopped beating. CLOSE, not free: the slot accounting is
 * returned to the pool and the generation is bumped, but written_slots is left
 * exactly as it was — the payload is a record of a path that was walked, not
 * garbage to be zeroed. Returns the number of blocks reaped. */
static inline int kvb_reap(kvb_table *t, int64_t now_ms) {
    int n = 0;
    for (int i = 0; i < KVB_MAX_BLOCKS; i++) {
        kvb_block *b = &t->b[i];
        if (b->state != KVB_RESERVED && b->state != KVB_ACTIVE) continue;
        if (now_ms - b->beat_ms < t->lease_ms) continue;
        b->state      = KVB_CLOSED;
        b->closed_ms  = now_ms;
        b->generation = b->generation + 1 ? b->generation + 1 : 1;
        t->held      -= b->slots;
        t->reap_events++;
        n++;
    }
    return n;
}

static inline int kvb_close(kvb_table *t, kvb_ref r, int64_t now_ms) {
    int rc = kvb_check(t, r, NULL);
    if (rc != KVB_OK) return rc;
    kvb_block *b = &t->b[r.idx];
    b->state      = KVB_CLOSED;
    b->closed_ms  = now_ms;
    b->generation = b->generation + 1 ? b->generation + 1 : 1;
    t->held      -= b->slots;
    return KVB_OK;
}

/* Age of every live reservation, for the waste gate: a percentage lets one huge
 * stale reservation pass a per-count test, so the gate is AGE. */
static inline int64_t kvb_oldest_live_ms(const kvb_table *t, int64_t now_ms) {
    int64_t oldest = 0;
    for (int i = 0; i < KVB_MAX_BLOCKS; i++) {
        const kvb_block *b = &t->b[i];
        if (b->state != KVB_RESERVED && b->state != KVB_ACTIVE) continue;
        int64_t age = now_ms - b->reserved_ms;
        if (age > oldest) oldest = age;
    }
    return oldest;
}

static inline int32_t kvb_reserved_unused(const kvb_table *t) {
    int32_t unused = 0;
    for (int i = 0; i < KVB_MAX_BLOCKS; i++) {
        const kvb_block *b = &t->b[i];
        if (b->state != KVB_RESERVED && b->state != KVB_ACTIVE) continue;
        unused += b->slots - b->written_slots;
    }
    return unused;
}

/* One quota observation per round. Hysteresis: the deviation must persist TWO
 * rounds and the change is at most half the deficit (grow) or an eighth
 * (shrink) — a quota recomputed from the usage it caused flaps otherwise. O(1). */
static inline void kvb_quota_observe(kvb_table *t, int32_t used_slots) {
    int32_t step = t->quota / 8; if (step < 1) step = 1;
    if (used_slots > t->quota + step) { t->over_rounds++;  t->under_rounds = 0; }
    else if (used_slots + step < t->quota) { t->under_rounds++; t->over_rounds = 0; }
    else { t->over_rounds = 0; t->under_rounds = 0; }
    if (t->over_rounds >= 2) {
        /* jump straight to the demand once it has persisted two rounds: growth is
           the safe direction (nothing in main moves), and half-deficit stepping
           only approaches the demand asymptotically — measured 12 rounds to 97%
           of a 12.5x jump, which is not "converged in 2-3 rounds". */
        t->quota = used_slots;
        if (t->quota > t->quota_max) t->quota = t->quota_max;
        t->quota_steps++; t->over_rounds = 0;
    } else if (t->under_rounds >= 2) {
        int32_t step_down = t->quota / 8; if (step_down < 1) step_down = 1;
        t->quota -= step_down;
        if (t->quota < t->quota_min) t->quota = t->quota_min;
        t->quota_steps++; t->under_rounds = 0;
    }
    t->round++;
}

#endif /* KV_LEASE_H */
