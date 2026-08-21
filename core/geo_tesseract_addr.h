/*
 * geo_tesseract_addr.h — Fixed-frame 4D addressing (no camera move)
 *
 * User concept (2026-08-21): 4D object is STATIONARY memory.
 * We pin a frame index and access interior via address — no distortion.
 * Moving through 4D = switching the index state, not changing a view angle.
 *
 * Tesseract = 8 cubes (cells) = (axis 0..3, sign 0/1) → 3 bits = 8 values
 *   idx = (axis << 1) | sign   — 0..7, no geometry, just integer
 *   axis = idx >> 1, sign = idx & 1
 *
 * 18tes protagonist field: 18 tesseracts × 8 cells × 144 slots = 20736
 *   flat = tess*1152 + cell*144 + slot   — deterministic, int-only
 *   Maps the whole GEO_FULL field without hash or lookup table.
 *
 * Two adjacency views (both deterministic, O(1)):
 *   [A] XOR-cube (per doc): neighbor = idx ^ (1<<k), k=0..2 — 3-cube,
 *       degree 3, illustrates "bit-flip graph" for any N (2N cells via 3 bits).
 *   [B] True tesseract cells: cell (axis,sign) adjacent to the 6 cells
 *       with axis' != axis (all except opposite (axis,1-sign)).
 *       Degree 6, matches the 8-cell tesseract topology.
 *
 * Fixed-frame guarantee: once idx is pinned, any interior slot access is
 * flat(idx,slot) without field-wide side effects — no magnify invert,
 * no hex residual. The field only matters when picking the frame.
 */

#ifndef GEO_TESSERACT_ADDR_H
#define GEO_TESSERACT_ADDR_H

#include <stdint.h>

#define TESS_AXES        4u
#define TESS_CELLS       8u
#define TESS_SLOTS       144u
#define TESS_PER_TESS    1152u                  /* 8*144 */
#define TESS_COUNT       18u
#define TESS_GEO_FULL    20736u                 /* 18*1152 = 144*144 */

/* ── index encode / decode ──────────────────────────────────────── */
static inline uint32_t tess_index(uint32_t axis, uint32_t sign) {
    return ((axis & 3u) << 1) | (sign & 1u);
}
static inline uint32_t tess_axis(uint32_t idx) { return (idx >> 1) & 3u; }
static inline uint32_t tess_sign(uint32_t idx) { return idx & 1u; }

/* ── XOR-cube neighbors (doc's bit-flip graph) ─────────────────── */
static inline uint32_t tess_neighbor_xor(uint32_t idx, uint32_t k) {
    return (idx ^ (1u << (k % 3u))) & 7u;
}

/* ── true tesseract cell adjacency: 6 neighbors (axis' != axis) ── */
static inline uint32_t tess_adjacent(uint32_t idx, uint32_t out[6]) {
    uint32_t ax = tess_axis(idx);
    uint32_t n = 0;
    for (uint32_t a = 0; a < TESS_AXES; a++) if (a != ax) {
        out[n++] = tess_index(a, 0);
        out[n++] = tess_index(a, 1);
    }
    return n; /* always 6 */
}

/* ── flat address in the 20736 field (fixed frame) ─────────────── */
static inline uint32_t tess_flat(uint32_t tess, uint32_t cell, uint32_t slot) {
    return (tess % TESS_COUNT) * TESS_PER_TESS
         + (cell % TESS_CELLS) * TESS_SLOTS
         + (slot % TESS_SLOTS);
}
static inline void tess_unflat(uint32_t flat,
                                uint32_t *tess, uint32_t *cell, uint32_t *slot) {
    if (tess) *tess = flat / TESS_PER_TESS;
    if (cell) *cell = (flat % TESS_PER_TESS) / TESS_SLOTS;
    if (slot) *slot = flat % TESS_SLOTS;
}

/* ── verification (call from tests) ─────────────────────────────── */
static inline int geo_tesseract_verify(void) {
    /* encode/decode roundtrip */
    for (uint32_t ax = 0; ax < TESS_AXES; ax++)
        for (uint32_t s = 0; s < 2; s++) {
            uint32_t idx = tess_index(ax, s);
            if (tess_axis(idx) != ax || tess_sign(idx) != s) return -1;
        }
    /* XOR involution: flip twice returns */
    for (uint32_t idx = 0; idx < TESS_CELLS; idx++)
        for (uint32_t k = 0; k < 3; k++)
            if (tess_neighbor_xor(tess_neighbor_xor(idx, k), k) != idx) return -2;
    /* adjacency degree 6 and excludes opposite */
    for (uint32_t idx = 0; idx < TESS_CELLS; idx++) {
        uint32_t out[6]; uint32_t n = tess_adjacent(idx, out);
        if (n != 6) return -3;
        uint32_t opp = tess_index(tess_axis(idx), tess_sign(idx) ^ 1u);
        for (uint32_t i = 0; i < 6; i++) if (out[i] == opp) return -4;
        for (uint32_t i = 0; i < 6; i++) if (out[i] == idx) return -5;
    }
    /* flat/unflat + coverage of 20736 */
    for (uint32_t f = 0; f < TESS_GEO_FULL; f += 997) {
        uint32_t t, c, s; tess_unflat(f, &t, &c, &s);
        if (tess_flat(t, c, s) != f) return -6;
    }
    if (TESS_GEO_FULL != 20736u) return -7;
    if (TESS_PER_TESS * TESS_COUNT != TESS_GEO_FULL) return -8;
    return 0;
}

#endif /* GEO_TESSERACT_ADDR_H */
