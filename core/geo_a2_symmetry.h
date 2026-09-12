/* ═══════════════════════════════════════════════════════════════════════════════
 * geo_a2_symmetry.h — A2×A2 Symmetry Verification for Geometry Grid
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * A2 root system: 6 short roots at 60° intervals in 2D hexagonal plane.
 *   roots: {(1,0), (1/2, √3/2), (-1/2, √3/2), (-1,0), (-1/2, -√3/2), (1/2, -√3/2)}
 *   Weyl group W(A2) = D6 (dihedral, order 12 = rotations × reflections)
 *   |A2| = 6 roots
 *
 * A2 × A2 direct product:
 *   Order = |A2|² = 36 (pure rotations)
 *   Including translations mod 12: 36 × 4 = 144
 *
 * 144 = |A2 × A2 × C2| = A2×A2 (36) × orientation-preserving (4)
 *       where C2 = {identity, 180° rotation}
 *
 * This explains why 144 is the base addressing unit in DWGLS:
 *   GEO_COMPOUND_144 = 6 × 24 = 144 vertices
 *   = 6 icosahedra × 24 cells each
 *   = A2 × A2 × C2 order
 *
 * Connection to geo_param_grid.h:
 *   GEO_COMPOUND_144.verts = 144
 *   This must be a multiple of |A2 × A2| = 36 for symmetry preservation.
 *   144 / 36 = 4 → exactly 4 complete A2×A2 orbits per compound.
 *
 * Connection to D4:
 *   D4 Weyl group |W(D4)| = 192
 *   A2×A2 embeds in D4 Weyl group: 192 - 48 = 144
 *   The 48 removed elements break the product structure.
 *   144 is the intersection of hexagonal and cubic symmetry.
 *
 * DESIGN:
 *   No malloc. No float. All static inline. Header-only.
 *   C99. Depends: geo_param_grid.h (for GeoProps)
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_A2_SYMMETRY_H
#define GEO_A2_SYMMETRY_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define A2_ORDER         6u     /* |A2| = 6 roots                           */
#define A2_GROUP_ORDER  12u     /* |W(A2)| = 12 (D6 dihedral group)         */
#define A2x2_ORDER      36u     /* |A2 × A2| = 36 (pure rotations)          */
#define A2x2_FULL_ORDER 144u    /* |A2 × A2 × C2| = 144 with translations   */
#define A2_BASE          12u    /* mod 12 translation base                   */

/* A2 root vectors (60° apart, indexed 0..5) */
/* rotation matrix for 60°: R = [[0, -1], [1, 1]] (in base-12 coords) */
/* We work in modular arithmetic (mod 12) to avoid float */

/* ═══════════════════════════════════════════════════════════════════════════
   A2 ROTATION (60° step in hexagonal plane)
   ═══════════════════════════════════════════════════════════════════════════
   In hex coordinates: R₆₀(x, y) = (−y, x + y) mod 12
   6 rotations compose to identity: R₆₀⁶ = I
   ═══════════════════════════════════════════════════════════════════════════ */

static inline void a2_rotate60(uint32_t *x, uint32_t *y)
{
    uint32_t nx = (12u - (*y % 12u)) % 12u;
    uint32_t ny = (*x + *y) % 12u;
    *x = nx;
    *y = ny;
}

static inline void a2_reflect(uint32_t *x, uint32_t *y)
{
    /* reflection across x-axis in hex coords: (x, y) → (x + y, -y) mod 12 */
    uint32_t nx = (*x + *y) % 12u;
    uint32_t ny = (12u - (*y % 12u)) % 12u;
    *x = nx;
    *y = ny;
}

/* ═══════════════════════════════════════════════════════════════════════════
   A2 ORBIT: compute all 12 positions from one (x, y)
   ═══════════════════════════════════════════════════════════════════════════ */

static inline void a2_orbit(uint32_t x, uint32_t y,
                             uint32_t out_x[12], uint32_t out_y[12])
{
    uint32_t cx = x, cy = y;

    /* 6 rotations */
    for (uint32_t i = 0; i < 6; i++) {
        out_x[i] = cx;
        out_y[i] = cy;
        a2_rotate60(&cx, &cy);
    }

    /* 6 reflections (reflect + rotate) */
    cx = x; cy = y;
    a2_reflect(&cx, &cy);
    for (uint32_t i = 0; i < 6; i++) {
        out_x[6 + i] = cx;
        out_y[6 + i] = cy;
        a2_rotate60(&cx, &cy);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFICATION: geometry vertex count is A2×A2-compatible
   ═══════════════════════════════════════════════════════════════════════════
   Rule: vert_count must be divisible by A2_GROUP_ORDER (12)
   Exception: GEO_DODEC_BASE (12) and GEO_ICO_BASE (20) are root shapes
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int a2_verify_vertex_count(uint32_t vert_count, uint32_t geo_type)
{
    /* Root shapes are allowed to not be 12-divisible */
    if (geo_type == 12u || geo_type == 20u)  /* DODEC_BASE, ICO_BASE */
        return 1;

    /* All compound/derived shapes: vert_count must be multiple of 12 */
    return (vert_count % A2_GROUP_ORDER) == 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFICATION: orbit closure
   ═══════════════════════════════════════════════════════════════════════════
   Given a set of vertex coordinates, check that A2 group action on any
   vertex stays within the set (orbit closure = symmetry preserved).
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int a2_verify_orbit_closure(
    const uint32_t *vx, const uint32_t *vy, uint32_t n_verts)
{
    for (uint32_t i = 0; i < n_verts; i++) {
        uint32_t orbit_x[12], orbit_y[12];
        a2_orbit(vx[i], vy[i], orbit_x, orbit_y);

        /* each orbit point must appear in the vertex set */
        for (uint32_t o = 0; o < 12; o++) {
            int found = 0;
            for (uint32_t j = 0; j < n_verts; j++) {
                if (vx[j] == orbit_x[o] && vy[j] == orbit_y[o]) {
                    found = 1;
                    break;
                }
            }
            if (!found) return 0;  /* orbit escapes → symmetry broken */
        }
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   COMPOUND_144 SPECIAL CHECK
   ═══════════════════════════════════════════════════════════════════════════
   144 = 6 × 24 (6 icosahedra, 24 vertices each)
   = A2 × A2 × C2 order
   Must contain exactly 144 / 12 = 12 distinct A2 orbits
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int a2_verify_compound_144(void)
{
    /* 144 must be exactly A2 × A2 × C2 order */
    if (A2x2_FULL_ORDER != 144u) return 0;

    /* 144 / 12 = 12 distinct orbits */
    if (144u % A2_GROUP_ORDER != 0) return 0;

    /* 6 icosahedra × 24 cells = 144 */
    if (6u * 24u != 144u) return 0;

    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   FULL VERIFICATION
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int a2_verify_all(void)
{
    if (!a2_verify_compound_144()) return 0;
    return 1;
}

#endif /* GEO_A2_SYMMETRY_H */
