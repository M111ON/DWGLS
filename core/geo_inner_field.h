/* ═══════════════════════════════════════════════════════════════════════════
 * geo_inner_field.h — Inner-field digit-extension nesting (spec 2026-09-13)
 * ═══════════════════════════════════════════════════════════════════════════
 * Naming scheme (view of L0 addressing), NOT a storage layer. Zero bytes.
 *
 * L0 (existing): slot = q*9 + l, q ∈ [0,16) quadtree branch,
 *   l ∈ [0,9) loshu cell (144 = 16x9 per cube).
 * L1 (inner): coarse digits extended with fine digits q2 ∈ [0,16),
 *   l2 ∈ [0,9): Q = q*16+q2 ∈ [0,256), L = l*9+l2 ∈ [0,81),
 *   inner = Q*81+L ∈ [0,20736) — same shape as th_node(hi,lo).
 *   Each cube holds one FULL 20736-field; outer slot is its coarse prefix.
 * L2 (total, not per cube): (X,Y,Z) triple of L1 addresses, one per KIS
 *   axis = 20736^3 = 12^12. Cube/tess identity lives inside each axis
 *   coordinate (resolves via flat_to_tess). Packed as u64.
 *
 * parent = LOSSY projection (144:1, drops fine digits).
 * refine = injective per coarse prefix. Roundtrip holds coarse-side only.
 *
 * Header-only, int-only, no malloc, no float. No dependencies.
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_INNER_FIELD_H
#define GEO_INNER_FIELD_H

#include <stdint.h>

/* ── Constants ─────────────────────────────────────────────────────── */
#define IF_Q_BRANCH    16u     /* quadtree branch per slot digit  */
#define IF_L_CELL      9u      /* loshu cell per slot digit       */
#define IF_CUBE_SLOTS  144u    /* IF_Q_BRANCH * IF_L_CELL         */
#define IF_HI_BASE     256u    /* 16*16 — inner Q range           */
#define IF_LO_BASE     81u     /* 9*9 — inner L range             */
#define IF_INNER_SLOTS 20736u  /* IF_HI_BASE * IF_LO_BASE         */
#define IF_L1_TOTAL    2985984u            /* 20736*8*18 = 144^3            */
#define IF_L2_SLOTS    8916100448256ull   /* 20736^3 = 12^12 (count; max addr = -1) */

/* ── L0: slot <-> (q, l) ───────────────────────────────────────────── */
static inline uint32_t if_slot(uint32_t q, uint32_t l) { return q * IF_L_CELL + l; }

static inline void if_slot_split(uint32_t slot, uint32_t *q, uint32_t *l) {
    if (q) *q = slot / IF_L_CELL;
    if (l) *l = slot % IF_L_CELL;
}

/* ── L1: (q,l,q2,l2) <-> inner ─────────────────────────────────────── */
static inline uint32_t if_inner_from(uint32_t q, uint32_t l,
                                     uint32_t q2, uint32_t l2) {
    uint32_t Q = q * IF_Q_BRANCH + q2;
    uint32_t L = l * IF_L_CELL + l2;
    return Q * IF_LO_BASE + L;
}

static inline void if_inner_split(uint32_t inner, uint32_t *Q, uint32_t *L) {
    if (Q) *Q = inner / IF_LO_BASE;
    if (L) *L = inner % IF_LO_BASE;
}

/* coarse (outer) digits of an inner address */
static inline void if_inner_coarse(uint32_t inner, uint32_t *q, uint32_t *l) {
    uint32_t Q = inner / IF_LO_BASE, L = inner % IF_LO_BASE;
    if (q) *q = Q / IF_Q_BRANCH;
    if (l) *l = L / IF_L_CELL;
}

/* LOSSY parent: inner -> outer slot (144:1 projection) */
static inline uint32_t if_inner_parent(uint32_t inner) {
    uint32_t Q = inner / IF_LO_BASE, L = inner % IF_LO_BASE;
    return (Q / IF_Q_BRANCH) * IF_L_CELL + (L / IF_L_CELL);
}

/* refinement: outer slot + fine digits -> inner (injective per prefix) */
static inline uint32_t if_inner_refine(uint32_t slot, uint32_t q2, uint32_t l2) {
    return if_inner_from(slot / IF_L_CELL, slot % IF_L_CELL, q2, l2);
}

static inline int if_inner_valid(uint32_t inner) { return inner < IF_INNER_SLOTS; }

/* ── L2: (X,Y,Z) <-> u64 pack ──────────────────────────────────────── */
static inline uint64_t if_l2_pack(uint32_t X, uint32_t Y, uint32_t Z) {
    return ((uint64_t)X * IF_INNER_SLOTS + Y) * IF_INNER_SLOTS + Z;
}

static inline void if_l2_unpack(uint64_t v, uint32_t *X, uint32_t *Y, uint32_t *Z) {
    uint32_t z = (uint32_t)(v % IF_INNER_SLOTS);
    uint64_t h = v / IF_INNER_SLOTS;
    uint32_t y = (uint32_t)(h % IF_INNER_SLOTS);
    uint32_t x = (uint32_t)(h / IF_INNER_SLOTS);
    if (X) *X = x;
    if (Y) *Y = y;
    if (Z) *Z = z;
}

#endif /* GEO_INNER_FIELD_H */
