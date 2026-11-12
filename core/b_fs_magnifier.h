/*
 * b_fs_magnifier.h — Magnifier adapter for b_fs (wraps bfs_magnify.h)
 * ════════════════════════════════════════════════════════════════════
 * Uses the proven magnifier glass from bfs_magnify.h:
 *   Glass = 144/4 = 36 slots (middle half), outer = compressed.
 *   Antipodal inversion: a_w × a_{w+72} ≡ 1 (mod 144).
 *   Glass rates: upper=5, lower=7 (magnify). Outer: 29, 103 (compress).
 *   Walking half-distance simulates near-0; further walking pushes
 *   deterministic delta to hyperbolic side for storage.
 *
 * All functions delegate to bfs_magnify.h (proven constants).
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_MAGNIFIER_H
#define B_FS_MAGNIFIER_H

#include <stdint.h>
#include "b_fs_base.h"
#include "bfs_magnify.h"

/* ── Magnifier state on b_fs ───────────────────────────────────── */
typedef struct {
    uint32_t pos;          /* current flat position                 */
    uint32_t write_pos;    /* where data was written (lossless point)*/
} BfsMagnifier;

static inline void bfmag_init(BfsMagnifier *m, uint32_t write_pos) {
    if (!m) return;
    m->pos = write_pos % BFS_PAIR_N;
    m->write_pos = write_pos % BFS_PAIR_N;
}

static inline void bfmag_move(BfsMagnifier *m, uint32_t new_pos) {
    if (!m) return;
    m->pos = new_pos % BFS_PAIR_N;
}

/* Lossless iff magnifier stands at write point */
static inline int bfmag_is_lossless(const BfsMagnifier *m) {
    if (!m) return 0;
    return (m->pos == m->write_pos) ? 1 : 0;
}

/* Is current position inside the magnify glass? */
static inline int bfmag_in_glass(const BfsMagnifier *m) {
    if (!m) return 0;
    return bfs_mg_flat_in_glass(m->pos);
}

/* Antipodal position (opposite side of glass) */
static inline uint32_t bfmag_antipode(const BfsMagnifier *m) {
    if (!m) return 0;
    return bfs_mg_flat_antipode(m->pos);
}

/* Inverted rate at current position (magnify or compress) */
static inline uint32_t bfmag_rate(const BfsMagnifier *m) {
    if (!m) return 1;
    return bfs_mg_rate(m->pos % BFS_MG_CELLS);
}

/* Verify inversion property (a_w × a_{w+72} ≡ 1 mod 144) */
static inline int bfmag_verify_inversion(void) {
    return bfs_mg_verify_inversion();
}

#endif /* B_FS_MAGNIFIER_H */
