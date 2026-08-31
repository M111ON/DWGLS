/* moe_expert_addr.h — MoE Expert ↔ Geometry Address Mapping
 *
 * "MAP not COMPRESS" applied to Mixture-of-Experts:
 *   expert_id = geometry coordinate = disk offset.
 *   No hash, no lookup table. Pure integer O(1).
 *
 * Address: (layer, expert_num, weight_type)
 *   layer      ∈ [0, N_LAYERS)
 *   expert_num ∈ [0, N_EXPERTS)
 *   weight_type ∈ {GATE=0, UP=1, DOWN=2}
 *
 * Flat address = layer * N_EXPERTS * 3 + expert_num * 3 + weight_type
 * Geometry = tess_to_flat(flat / 1152, (flat % 1152) / 144, flat % 144)
 * Disk offset = flat * BLOCK_SIZE
 *
 * 18 tesseracts × 8 cubes × 144 slots = 20736 flat addresses
 * Capacity: up to 6912 experts (20736 / 3 weight types)
 *   e.g. 32 layers × 64 experts × 3 types = 6144 (< 6912)
 *
 * REFERENCE:
 *   kimi-k3-in-c: expert_id = (layer, offset) → disk read at known offset
 *   DWGLS: expert_id = geometry coordinate → slot address → DtSlotRegion
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef MOE_EXPERT_ADDR_H
#define MOE_EXPERT_ADDR_H

#include <stdint.h>
#include "geo_tess_wiring.h"

/* ═══════════════ CONSTANTS ═══════════════ */

#define MOE_MAX_LAYERS      64u
#define MOE_MAX_EXPERTS     64u     /* per layer */
#define MOE_WEIGHT_TYPES    3u      /* gate, up, down */
#define MOE_MAX_FLAT        TESS_TOTAL  /* 20736 */

typedef enum {
    MOE_WTYPE_GATE = 0,
    MOE_WTYPE_UP   = 1,
    MOE_WTYPE_DOWN = 2,
} MoeWType;

/* ═══════════════ EXPERT → FLAT ═══════════════ */

/* Expert address → flat address in [0, 20736) */
static inline uint32_t moe_expert_to_flat(uint32_t layer, uint32_t expert,
                                           uint32_t wtype) {
    return (layer * MOE_MAX_EXPERTS * MOE_WEIGHT_TYPES
          + expert * MOE_WEIGHT_TYPES
          + wtype) % MOE_MAX_FLAT;
}

/* Flat address → expert address */
static inline void moe_flat_to_expert(uint32_t flat, uint32_t *layer,
                                       uint32_t *expert, uint32_t *wtype) {
    flat = flat % MOE_MAX_FLAT;
    *wtype  = flat % MOE_WEIGHT_TYPES;
    uint32_t le = flat / MOE_WEIGHT_TYPES;
    *expert  = le % MOE_MAX_EXPERTS;
    *layer   = le / MOE_MAX_EXPERTS;
}

/* ═══════════════ EXPERT → GEOMETRY ═══════════════ */

/* Expert address → (tess, cube, slot) geometry coordinate */
static inline void moe_expert_to_geom(uint32_t layer, uint32_t expert,
                                       uint32_t wtype, uint32_t *tess,
                                       uint32_t *cube, uint32_t *slot) {
    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
    flat_to_tess(flat, tess, cube, slot);
}

/* Geometry coordinate → expert address */
static inline void moe_geom_to_expert(uint32_t tess, uint32_t cube,
                                       uint32_t slot, uint32_t *layer,
                                       uint32_t *expert, uint32_t *wtype) {
    uint32_t flat = tess_to_flat(tess, cube, slot);
    moe_flat_to_expert(flat, layer, expert, wtype);
}

/* ═══════════════ EXPERT → DISK OFFSET ═══════════════ */

/* Expert address → byte offset in backing file (deterministic) */
static inline uint64_t moe_expert_to_offset(uint32_t layer, uint32_t expert,
                                              uint32_t wtype,
                                              uint32_t block_size) {
    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
    return (uint64_t)flat * block_size;
}

/* ═══════════════ GEOMETRY PROPERTIES ═══════════════ */

/* Two experts are "neighbors" if they share a cube (same layer group) */
static inline int moe_experts_same_group(uint32_t e1_flat, uint32_t e2_flat) {
    uint32_t t1, c1, s1, t2, c2, s2;
    flat_to_tess(e1_flat, &t1, &c1, &s1);
    flat_to_tess(e2_flat, &t2, &c2, &s2);
    return (t1 == t2 && c1 == c2);
}

/* Expert weight types share geometry (same layer+expert, different wtype) */
static inline uint32_t moe_expert_sibling(uint32_t flat, uint32_t wtype) {
    uint32_t layer, expert, wt;
    moe_flat_to_expert(flat, &layer, &expert, &wt);
    return moe_expert_to_flat(layer, expert, wtype);
}

/* ═══════════════ CAPACITY CHECK ═══════════════ */

/* How many experts fit in the field? */
static inline uint32_t moe_capacity(uint32_t n_layers, uint32_t n_experts) {
    uint32_t total = n_layers * n_experts * MOE_WEIGHT_TYPES;
    return total <= MOE_MAX_FLAT ? total : 0;  /* 0 = overflow */
}

#endif /* MOE_EXPERT_ADDR_H */
