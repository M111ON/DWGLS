/*
 * kis_cube_views.h — 6 face-views of one KIS cube (S₃ axis permutations)
 * ══════════════════════════════════════════════════════════════════════
 * Generalizes iso_rot90 to all 3 axes at once:
 *
 *   1 cube = 12 × 12 × 12 = 1728 = FS_PIPES        (fibo_spine.h)
 *   1728 pipes × 12 ticks                  = 20736 = GEO_FULL
 *
 * The 6 views are the 6 permutations of the axes (symmetric group S₃):
 * every view is a pure-int bijection of [0,11]³ — same data, same bytes,
 * six ways to walk it. N views from 1 copy, now in 3D.
 *
 *   slot encoding : s = z·144 + y·12 + x     (x,y,z ∈ [0,11])
 *   view v        : apply permutation Pᵥ to (x,y,z)
 *
 * Group structure (verified in tests):
 *   views 1..3 (swaps)      : order 2 — self-inverse
 *   views 4..5 (3-cycles)   : order 3 — v∘v∘v = id
 *   inv table               : mutual inverses both directions
 *
 * Composes with per-axis rot90/rot270 (iso_rot90.h) for tri↔square duality
 * on any chosen axis, and with iso_fold for the hardware anchor view.
 */
#ifndef KIS_CUBE_VIEWS_H
#define KIS_CUBE_VIEWS_H

#include <stdint.h>
#include "iso_rot90.h"          /* ISO_SIDE = 12 */

#define KIS_CUBE_SIDE    ((uint32_t)ISO_SIDE)          /* 12          */
#define KIS_CUBE         (KIS_CUBE_SIDE * KIS_CUBE_SIDE * KIS_CUBE_SIDE) /* 1728 = FS_PIPES */
#define KIS_VIEWS        6u

/* permutation table: view v reads input axis PERM[v][i] into output i */
static const uint8_t KIS_PERM[KIS_VIEWS][3] = {
    { 0, 1, 2 },   /* 0: identity            */
    { 1, 0, 2 },   /* 1: swap x,y   (order 2)*/
    { 2, 1, 0 },   /* 2: swap x,z   (order 2)*/
    { 0, 2, 1 },   /* 3: swap y,z   (order 2)*/
    { 1, 2, 0 },   /* 4: cycle x→y→z (order 3) */
    { 2, 0, 1 },   /* 5: cycle x→z→y (order 3) */
};

/* inverse-view table */
static const uint8_t KIS_VIEW_INV[KIS_VIEWS] = { 0, 1, 2, 3, 5, 4 };

typedef struct { uint32_t x, y, z; } kis_cpt;

static inline uint32_t kis_cube_slot(uint32_t x, uint32_t y, uint32_t z) {
    return z * (KIS_CUBE_SIDE * KIS_CUBE_SIDE) + y * KIS_CUBE_SIDE + x;
}

static inline kis_cpt kis_cube_coords(uint32_t s) {
    kis_cpt p;
    p.x = s % KIS_CUBE_SIDE;
    p.y = (s / KIS_CUBE_SIDE) % KIS_CUBE_SIDE;
    p.z = s / (KIS_CUBE_SIDE * KIS_CUBE_SIDE);
    return p;
}

static inline const uint8_t *kis_perm(uint32_t view) {
    return KIS_PERM[view % KIS_VIEWS];
}

static inline uint32_t kis_view6_slot(uint32_t view, uint32_t s) {
    const uint8_t *pm = kis_perm(view);
    kis_cpt p = kis_cube_coords(s);
    const uint32_t c[3] = { p.x, p.y, p.z };
    return kis_cube_slot(c[pm[0]], c[pm[1]], c[pm[2]]);
}

static inline uint32_t kis_view6_inv(uint32_t view, uint32_t s) {
    return kis_view6_slot(KIS_VIEW_INV[view % KIS_VIEWS], s);
}

#endif /* KIS_CUBE_VIEWS_H */
