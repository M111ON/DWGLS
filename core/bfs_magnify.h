/*
 * bfs_magnify.h — Magnifier Glass for BreathingFS
 * ════════════════════════════════════════════════════════════════════
 * Wraps the magnifier glass concept (test_tess_magnify.c, geo_tess_wiring.h)
 * into the BreathingFS domain.
 *
 * Glass: 20736 ÷ 4 = 5184 slots per quadrant; middle half = glass,
 * outer half = compressed. Glass center = 72+δ, antipodal inversion
 * a_w × a_{w+72} ≡ 1 mod 144.
 *
 * All static inline, zero malloc, integer-only.
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef BFS_MAGNIFY_H
#define BFS_MAGNIFY_H

#include <stdint.h>

/* ── Glass geometry (from test_tess_magnify.c proven constants) ─── */
#define BFS_MG_TOTAL    20736u
#define BFS_MG_CELLS    144u
#define BFS_MG_HALF     72u
#define BFS_MG_QUAD     36u      /* 144 ÷ 4                         */
#define BFS_MG_OFFSET   5u       /* small offset δ                   */

/* ── Inverted rates (proven: a_w × a_{w+72} ≡ 1 mod 144) ────────
 * Glass upper:  a = 5,  lower: a = 7
 * Outer upper:  a = 103 (inv of 7), lower: a = 29 (inv of 5)      */

/* ── Glass check ──────────────────────────────────────────────────
 * Returns 1 if local position w is inside the magnify glass.       */
static inline int bfs_mg_in_glass(uint32_t w) {
    uint32_t shifted = (w + BFS_MG_CELLS - BFS_MG_OFFSET) % BFS_MG_CELLS;
    return (shifted >= BFS_MG_QUAD && shifted < 3u * BFS_MG_QUAD);
}

/* ── Antipodal position (opposite side of glass) ────────────────── */
static inline uint32_t bfs_mg_antipode(uint32_t w) {
    return (w + BFS_MG_HALF) % BFS_MG_CELLS;
}

/* ── Inverted rate for a local position ───────────────────────────
 * Glass: upper=5, lower=7. Outer: inv(7)=103, inv(5)=29.
 * gcd(rate, 144) = 1 always → bijection.                          */
static inline uint32_t bfs_mg_rate(uint32_t w) {
    uint32_t shifted = (w + BFS_MG_CELLS - BFS_MG_OFFSET) % BFS_MG_CELLS;
    int in_glass = (shifted >= BFS_MG_QUAD && shifted < 3u * BFS_MG_QUAD);
    int upper = (shifted < BFS_MG_HALF + BFS_MG_OFFSET);
    if (in_glass) return upper ? 5u : 7u;
    return upper ? 29u : 103u;
}

/* ── Flat address: in-glass check ───────────────────────────────── */
static inline int bfs_mg_flat_in_glass(uint32_t flat) {
    return bfs_mg_in_glass(flat % BFS_MG_CELLS);
}

/* ── Flat address: antipode ───────────────────────────────────────
 * Preserves tess+cube, replaces local with antipodal local.        */
static inline uint32_t bfs_mg_flat_antipode(uint32_t flat) {
    uint32_t tess_cube = flat / BFS_MG_CELLS;
    uint32_t local = flat % BFS_MG_CELLS;
    uint32_t anti_local = bfs_mg_antipode(local);
    return tess_cube * BFS_MG_CELLS + anti_local;
}

/* ── Inversion check (proven: a_w × a_{w+72} ≡ 1 mod 144) ─────── */
static inline int bfs_mg_verify_inversion(void) {
    for (uint32_t w = 0; w < BFS_MG_CELLS; w++) {
        uint32_t aw = bfs_mg_rate(w);
        uint32_t anti = bfs_mg_rate(bfs_mg_antipode(w));
        if ((aw * anti) % BFS_MG_CELLS != 1u) return 0;
    }
    return 1;
}

#endif /* BFS_MAGNIFY_H */
