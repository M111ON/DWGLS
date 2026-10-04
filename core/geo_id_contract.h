/* ═══════════════════════════════════════════════════════════════════════════
 * geo_id_contract.h — Identity / World / Address contract
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * THE 5-TUPLE BINDING (P2 of the 4-P stack):
 *
 *   GeoIdent = (name_hash, bytes_hash, addr_slot, walk_round, walk_tick)
 *
 * Tri-bind rule (P2 → P4 → P5):
 *
 *     name  ─hash→  name_hash
 *       │
 *       └──addr──▶ addr_slot ──walk──▶ (walk_round, walk_tick)
 *                            ▲                              │
 *                            │                              │
 *                            └──────  bytes_hash  ◀──────────┘
 *                                     (slot fingerprint)
 *
 *   - name_hash     : FNV-1a 64 of the tensor/file/byte-string name
 *   - bytes_hash    : FNV-1a 64 of the slot's 64B (zero-init = 0)
 *   - addr_slot     : uint32 in [0, 20736) — flat slot address
 *   - walk_round    : uint32 — round on the fibo walk clock (rq = round * ticks + tick)
 *   - walk_tick     : uint32 in [0, ticks) — local tick inside the round
 *
 * Same input (name, walk_state, slot_bytes) ⇒ same 5-tuple. ALWAYS.
 *
 * RESOLVERS
 *
 *   geo_ident_from_name(name, walk_state, slot_bytes)
 *     → derive the full 5-tuple from name + the slot's 64 bytes at walk_state.
 *
 *   geo_ident_resolve_addr(ident)
 *     → addr_slot (= the flat coordinate the bytes live at).
 *
 *   geo_ident_resolve_walk(ident)
 *     → (walk_round, walk_tick) (= where the slot lives on the walk clock).
 *
 *   geo_ident_from_slot(slot)
 *     → ident when bytes are present (re-derive bytes_hash from slot bytes).
 *
 * REPLAY (partial rebuild + log replay → identical 5-tuple)
 *
 *   geo_ident_replay(name, addr_slot, walk_state)
 *     → 5-tuple reconstructed from (name, addr_slot, walk_state) only.
 *       bytes_hash is recomputed from the bytes AT addr_slot — if rebuild
 *       restored the same content (memcmp verified), bytes_hash matches.
 *
 * INVARIANTS (the test contract gates these):
 *
 *   I1. Two independent derivations of the same ident agree.
 *   I2. addr_slot = ident→addr_slot (obvious, but exercised).
 *   I3. walk_round * ticks + walk_tick = ident→rq (one consolidated clock).
 *   I4. After partial rebuild (clear some slots) + replay → all five-tuple
 *       fields match what was there before.
 *   I5. Walk one stride → walk_tick advances, addr+bytes no longer match
 *       unless the caller explicitly moves bytes (the contract is honest
 *       about WHEN the binding changes).
 *
 * DEPENDS: stdint.h, string.h, stddef.h
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_ID_CONTRACT_H
#define GEO_ID_CONTRACT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants (same as fibo_walk.h / geofs_mdim.h) ───────────────────── */
#define GEO_ID_FIELD_SLOTS    20736u
#define GEO_ID_WALK_STRIDE    37u     /* coprime with 20736 — bijective walk */
#define GEO_ID_SLOT_BYTES     64u     /* slot byte width */

#define GEO_ID_NAME_HASH_OFF  UINT64_C(0xCBF29CE484222325)  /* FNV-1a 64 offset basis */
#define GEO_ID_NAME_HASH_PRIM UINT64_C(0x00000100000001B3)  /* FNV-1a 64 prime */

/* ── Walk state (caller-owned) ────────────────────────────────────────── */
typedef struct {
    uint32_t seed;       /* fibo walk seed */
    uint32_t ticks;      /* ticks per cycle (FS_TICKS = 12 in fibo_walk.h) */
    uint32_t cycles;     /* rq ∈ [0, cycles) — rounds on the walk clock */
    uint32_t round;      /* current round (round ∈ [0, cycles)) */
    uint32_t tick;       /* current tick  (tick  ∈ [0, ticks))   */
} GeoWalkState;

#define GEO_ID_WALK_DEFAULT_TICKS   12u   /* matches fibo_walk.h FS_TICKS */
#define GEO_ID_WALK_DEFAULT_CYCLES  GEO_ID_FIELD_SLOTS  /* full field = 1 full round */

/* ── The 5-tuple ──────────────────────────────────────────────────────── */
typedef struct {
    uint64_t name_hash;    /* FNV-1a of name bytes */
    uint64_t bytes_hash;   /* FNV-1a of slot bytes (0 if slot empty) */
    uint32_t addr_slot;    /* flat coordinate ∈ [0, 20736) */
    uint32_t walk_round;   /* round on walk clock */
    uint32_t walk_tick;    /* tick on walk clock ∈ [0, ticks) */
} GeoIdent;

/* ── FNV-1a 64 ────────────────────────────────────────────────────────── */
static inline uint64_t geo_id_fnv1a64(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = GEO_ID_NAME_HASH_OFF;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= GEO_ID_NAME_HASH_PRIM;
    }
    return h;
}

/* Convenience: hash a NUL-terminated name string (no embedded NULs). */
static inline uint64_t geo_id_hash_name(const char *name) {
    if (!name) return 0;
    return geo_id_fnv1a64(name, strlen(name));
}

/* Convenience: hash the 64-byte slot payload. bytes==NULL ⇒ 0. */
static inline uint64_t geo_id_hash_slot(const uint8_t slot64[GEO_ID_SLOT_BYTES]) {
    if (!slot64) return 0;
    return geo_id_fnv1a64(slot64, GEO_ID_SLOT_BYTES);
}

/* ── Walk helpers (mirrors ggf_walk_rq_of / ggf_walk_tick_of) ─────────── */

/* addr_slot → walk_round and walk_tick
 *  rq      = (seed * (idx + 1)) % cycles        (deterministic from seed)
 *  walk_round = rq
 *  walk_tick  = rq % ticks
 */
static inline void geo_id_walk_resolve(uint32_t idx, const GeoWalkState *w,
                                       uint32_t *out_round, uint32_t *out_tick) {
    uint32_t cycles = w->cycles ? w->cycles : GEO_ID_WALK_DEFAULT_CYCLES;
    uint32_t rq = (uint32_t)((uint64_t)w->seed * (uint64_t)(idx + 1u) % (uint64_t)cycles);
    *out_round = rq;
    *out_tick  = rq % w->ticks;
}

/* Inverse: walk_round * ticks + walk_tick → consolidated rq */
static inline uint32_t geo_id_walk_rq(const GeoIdent *id) {
    return id->walk_round * 12u + id->walk_tick;
    /* ticks = 12 is baked in (FS_TICKS). Caller with non-12 ticks must use
     * geo_id_walk_rq_t(id, ticks) instead. Kept this overload for fast path. */
}
static inline uint32_t geo_id_walk_rq_t(const GeoIdent *id, uint32_t ticks) {
    return id->walk_round * ticks + id->walk_tick;
}

/* ── Resolvers ────────────────────────────────────────────────────────── */

/* Build the full 5-tuple from (name, walk_state, slot_bytes).
 * - addr_slot is the round index inside the walk cycle for THIS walk_state.
 * - name_hash / bytes_hash are FNV-1a of the inputs.
 * - walk_round = rq / ticks, walk_tick = rq % ticks  (round/tick split)
 */
static inline GeoIdent geo_ident_from_name(const char *name,
                                           const GeoWalkState *w,
                                           const uint8_t slot64[GEO_ID_SLOT_BYTES]) {
    GeoIdent id;
    id.name_hash  = geo_id_hash_name(name);
    id.bytes_hash = geo_id_hash_slot(slot64);
    /* addr_slot defaults to 0; caller may use geo_ident_with_slot to set it
     * if they have a specific slot in mind. The contract is: name + walk_state
     * ⇒ walk coords, addr_slot is the SHAPE the bytes were originally placed at.
     * Most callers want geo_ident_from_slot (below) when they know the slot. */
    id.addr_slot  = 0;
    if (w) {
        uint32_t rq = (uint32_t)(((uint64_t)w->seed * (uint64_t)(id.addr_slot + 1u))
                                 % (uint64_t)(w->cycles ? w->cycles : GEO_ID_WALK_DEFAULT_CYCLES));
        id.walk_round = rq / w->ticks;
        id.walk_tick  = rq % w->ticks;
    } else {
        id.walk_round = 0;
        id.walk_tick  = 0;
    }
    return id;
}

/* Build 5-tuple from (name, slot, walk_state) — addr_slot is KNOWN.
 * bytes_hash is derived from the slot's actual 64B. This is the canonical
 * "I have a slot's worth of bytes and a name; what's the ident?" entry.
 *
 * walk_round = rq / ticks, walk_tick = rq % ticks  (round/tick split)
 */
static inline GeoIdent geo_ident_from_slot(const char *name,
                                         uint32_t addr_slot,
                                         const GeoWalkState *w,
                                         const uint8_t slot64[GEO_ID_SLOT_BYTES]) {
    GeoIdent id;
    id.name_hash  = geo_id_hash_name(name);
    id.bytes_hash = geo_id_hash_slot(slot64);
    id.addr_slot  = addr_slot;
    if (w) {
        uint32_t cycles = w->cycles ? w->cycles : GEO_ID_WALK_DEFAULT_CYCLES;
        uint32_t rq = (uint32_t)(((uint64_t)w->seed * (uint64_t)(addr_slot + 1u))
                                 % (uint64_t)cycles);
        id.walk_round = rq / w->ticks;
        id.walk_tick  = rq % w->ticks;
    } else {
        id.walk_round = 0;
        id.walk_tick  = 0;
    }
    return id;
}

/* Set the addr_slot on an existing ident and re-derive walk_round/walk_tick.
 * Useful when caller knows the slot AFTER building the ident from a name. */
static inline void geo_ident_set_slot(GeoIdent *id, uint32_t addr_slot,
                                      const GeoWalkState *w) {
    id->addr_slot = addr_slot;
    if (w) {
        uint32_t cycles = w->cycles ? w->cycles : GEO_ID_WALK_DEFAULT_CYCLES;
        uint32_t rq = (uint32_t)(((uint64_t)w->seed * (uint64_t)(addr_slot + 1u))
                                 % (uint64_t)cycles);
        id->walk_round = rq / w->ticks;
        id->walk_tick  = rq % w->ticks;
    }
}

/* resolver: addr_slot (just the slot back) */
static inline uint32_t geo_ident_resolve_addr(const GeoIdent *id) {
    return id->addr_slot;
}

/* resolver: walk coords (the 2-tuple) */
static inline void geo_ident_resolve_walk(const GeoIdent *id,
                                          uint32_t *out_round, uint32_t *out_tick) {
    *out_round = id->walk_round;
    *out_tick  = id->walk_tick;
}

/* ── Replay (post-rebuild) ────────────────────────────────────────────── */

/* Replay entry in the walk log:
 *  - one record per slot write (the durable walk event).
 *  - (addr_slot, walk_round, walk_tick) is enough to fully reconstruct the
 *    5-tuple as long as the caller's bytes_at[addr_slot] matches the pre-rebuild
 *    content (verified by memcmp / bytes_hash comparison). */
typedef struct {
    char     name[32];     /* short tensor/file name */
    uint32_t addr_slot;   /* where the bytes live */
    uint32_t walk_round;  /* walk clock round at write */
    uint32_t walk_tick;   /* walk clock tick  at write */
} GeoIdentEvent;

/* Reconstruct ident from (event, slot_bytes) — caller verifies bytes match. */
static inline GeoIdent geo_ident_replay(const GeoIdentEvent *ev,
                                        const uint8_t slot64[GEO_ID_SLOT_BYTES]) {
    GeoIdent id;
    id.name_hash  = geo_id_fnv1a64(ev->name, strnlen(ev->name, 32));
    id.bytes_hash = geo_id_hash_slot(slot64);
    id.addr_slot  = ev->addr_slot;
    id.walk_round = ev->walk_round;
    id.walk_tick  = ev->walk_tick;
    return id;
}

/* ── Comparison ───────────────────────────────────────────────────────── */
static inline int geo_ident_eq(const GeoIdent *a, const GeoIdent *b) {
    return a->name_hash  == b->name_hash  &&
           a->bytes_hash == b->bytes_hash &&
           a->addr_slot  == b->addr_slot  &&
           a->walk_round == b->walk_round &&
           a->walk_tick  == b->walk_tick;
}

static inline int geo_ident_eq_strict(const GeoIdent *a, const GeoIdent *b) {
    return geo_ident_eq(a, b);
}

/* ── Walk forward (advance the walk clock by 1 stride-37 step) ────────── */
/* Returns the next addr_slot under stride-37 walk:  slot' = (slot * 37) % 20736.
 * bytes_hash recomputation is the caller's responsibility if the slot content
 * changed. The 5-tuple contract: walk_round/walk_tick advance, addr_slot
 * becomes the new stride-37 slot — bytes_hash follows whatever bytes are
 * there post-replay. */
static inline uint32_t geo_id_walk_stride37(uint32_t slot) {
    return (uint32_t)(((uint64_t)slot * (uint64_t)GEO_ID_WALK_STRIDE)
                      % (uint64_t)GEO_ID_FIELD_SLOTS);
}

#ifdef __cplusplus
}
#endif

#endif /* GEO_ID_CONTRACT_H */