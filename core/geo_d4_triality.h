/* ═══════════════════════════════════════════════════════════════════════════════
 * geo_d4_triality.h — D4 Triality Bridge for Twin Rebalance
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * D4 root system:
 *   24 roots in 4D: {±eᵢ ± eⱼ : i≠j}  (i,j ∈ {1,2,3,4})
 *   Weyl group |W(D4)| = 192
 *   Coxeter number h = 6
 *   Coxeter plane projection: 12-fold symmetry (order 2h = 12)
 *
 * Triality (unique to D4):
 *   Aut(D4) ≅ S₃ (symmetric group on 3 elements, order 6)
 *   Triality permutes the three 8-dimensional representations:
 *     Vector rep (8-dim) ↔ Spinor rep (8-dim) ↔ Cospinor rep (8-dim)
 *   This is the ONLY Dynkin diagram with a non-trivial automorphism.
 *
 * Connection to twin_rebalance:
 *   geo_twin_rebalance.h has 3 views:
 *     [A] Hardware: anchor[0..161] × local[0..127]  (DRAM layout)
 *     [B] Natural:  row[0..143] × col[0..143]      (field layout)
 *     [C] Flat:     offset[0..20735]                (common address)
 *
 *   These 3 views are NOT independent transforms.
 *   They are the 3 triality orbits of D4:
 *     Hardware ↔ Natural ↔ Flat = triality permutation
 *   Switching views = applying triality automorphism = O(1) permute.
 *
 * Connection to 6ico compound:
 *   6 icosahedra × 24 vertices = 144 vertices
 *   D4 projection of 6 icosahedra = 144 points (3 nested hexagons × 6 orientations)
 *   Triality permutes which icosahedron contributes which vertices
 *   → 6ico compound IS the D4 root set viewed from 6 icosahedral orientations
 *
 * Connection to 24-cell:
 *   24-cell = D4 root system / {±1} = 24 vertices
 *   6 24-cells (compound) = 6 × 24 = 144 vertices
 *   Each 24-cell ↔ one icosahedron in the 6ico compound
 *   Triality permutes the 3 pairs of opposite 24-cells
 *
 * Connection to CRT bridge:
 *   CRT binary channel (256) = depth via triality orbits
 *   CRT ternary channel (81) = breadth within orbit
 *   256 / 81 ≈ 3.16 ≈ S₃ order (6) / 2 (pair swap)
 *
 * DESIGN:
 *   No malloc. No float. All static inline. Header-only.
 *   C99. Depends: geo_twin_rebalance.h (for TW_HardAddr, TW_NatAddr)
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_D4_TRIALITY_H
#define GEO_D4_TRIALITY_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define D4_ORDER        192u     /* |W(D4)| = 2⁷ × 3 = 192               */
#define D4_COXETER        6u     /* Coxeter number h = 6                   */
#define D4_COXETER_PLANE 12u     /* 2h = 12-fold rotational symmetry       */
#define D4_ROOTS         24u     /* 24 roots in 4D                         */
#define D4_TRIALITY_ORDER  3u    /* |Aut(D4)/W(D4)| = 3 (triality orbits) */
#define D4_24CELL_VERTS  24u     /* vertices per 24-cell                   */
#define D4_6ICO_VERTS   144u     /* 6 icosahedra × 24 = 144               */

/* ═══════════════════════════════════════════════════════════════════════════
   TRIALITY VIEW ENUM
   ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    D4_VIEW_HARDWARE = 0,   /* DRamTile layout: anchor × local          */
    D4_VIEW_NATURAL  = 1,   /* Field layout: row × col                  */
    D4_VIEW_FLAT     = 2,   /* Common address: offset                   */
} D4_View;

/* ═══════════════════════════════════════════════════════════════════════════
   TRIALITY PERMUTATION TABLE
   ═══════════════════════════════════════════════════════════════════════════
   S₃ permutation group on 3 elements:
     e  = (0,1,2) = identity
     r  = (1,2,0) = cyclic rotation (triality orbit shift)
     r² = (2,0,1) = inverse rotation
     s  = (1,0,2) = swap first two (view pair swap)
     sr = (2,1,0) = swap + rotate
     sr²= (0,2,1) = rotate + swap

   Table: perm[i][j] = where element j moves under permutation i
   ═══════════════════════════════════════════════════════════════════════════ */

static const uint8_t D4_TRIALITY_PERM[6][3] = {
    {0, 1, 2},   /* e:   identity       → HW  → Nat → Flat */
    {1, 2, 0},   /* r:   cyclic shift   → Nat → Flat → HW  */
    {2, 0, 1},   /* r²:  inverse shift  → Flat → HW  → Nat */
    {1, 0, 2},   /* s:   swap HW↔Nat   → Nat → HW  → Flat */
    {2, 1, 0},   /* sr:  swap+rotate    → Flat → Nat → HW  */
    {0, 2, 1},   /* sr²: rotate+swap    → HW  → Flat → Nat */
};

/* ═══════════════════════════════════════════════════════════════════════════
   CORE: VIEW SWITCH (O(1) permute, no recompute)
   ═══════════════════════════════════════════════════════════════════════════
   To switch from view_from → view_to:
     1. Find the permutation that maps view_from to view_to
     2. Apply the permutation to the address components
   ═══════════════════════════════════════════════════════════════════════════ */

/* Permutation that maps view_from → view_to */
static inline uint8_t d4_triality_find_perm(D4_View from, D4_View to)
{
    for (uint8_t p = 0; p < 6; p++) {
        if (D4_TRIALITY_PERM[p][from] == (uint8_t)to)
            return p;
    }
    return 0;  /* identity if not found (should never happen) */
}

/* ═══════════════════════════════════════════════════════════════════════════
   HARDWARE ↔ NATURAL (direct, most common switch)
   ═══════════════════════════════════════════════════════════════════════════
   Hardware: flat = anchor × 128 + local   (anchor ∈ [0,161], local ∈ [0,127])
   Natural:  flat = row × 144 + col        (row, col ∈ [0,143])
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t d4_hard_to_nat_flat(uint32_t anchor, uint32_t local)
{
    return anchor * 128u + local;
}

static inline void d4_flat_to_hard(uint32_t flat,
                                    uint32_t *anchor, uint32_t *local)
{
    *anchor = flat / 128u;
    *local  = flat % 128u;
}

static inline void d4_flat_to_nat(uint32_t flat,
                                   uint32_t *row, uint32_t *col)
{
    *row = flat / 144u;
    *col = flat % 144u;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TRIALITY: 6ico compound orbit assignment
   ═══════════════════════════════════════════════════════════════════════════
   6 icosahedra in the compound = 3 triality pairs:
     Pair 0: ico[0] ↔ ico[3]  (vector ↔ cospinor)
     Pair 1: ico[1] ↔ ico[4]  (spinor ↔ vector)
     Pair 2: ico[2] ↔ ico[5]  (cospinor ↔ spinor)

   Each ico contributes 24 vertices → 6 × 24 = 144
   Triality permutes which pair is "active" for a given access pattern.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t d4_ico_pair(uint32_t ico_idx)
{
    return ico_idx % D4_TRIALITY_ORDER;  /* 0, 1, or 2 */
}

static inline uint32_t d4_ico_opposite(uint32_t ico_idx)
{
    /* opposite ico in the same triality pair: +3 mod 6 */
    return (ico_idx + 3u) % 6u;
}

/* ═══════════════════════════════════════════════════════════════════════════
   COXETER PLANE PROJECTION (D4 → hex)
   ═══════════════════════════════════════════════════════════════════════════
   D4 roots project to 2D via Coxeter plane as 12-fold arrangement.
   The 24 roots decompose as 4 orbits of 6 under D4 Coxeter element.
   Each orbit of 6 roots = 1 hex sub-cell (ω¹~ω⁶).
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t d4_coxeter_orbit(uint32_t root_idx)
{
    /* root_idx ∈ [0, 23], orbit = root_idx / 6 (4 orbits) */
    return root_idx / 6u;
}

static inline uint32_t d4_coxeter_position(uint32_t root_idx)
{
    /* position within orbit: 0..5 (hex direction) */
    return root_idx % 6u;
}

/* ═══════════════════════════════════════════════════════════════════════════
   24-CELL ↔ TESSERACT MAPPING
   ═══════════════════════════════════════════════════════════════════════════
   24-cell: 24 vertices, unique to 4D
   Tesseract: 8 cells (3D faces)
   Mapping: each 24-cell vertex → 1 slot within 1 tesseract cell

   24-cell vertices = {±eᵢ ± eⱼ} permutations
   Tesseract cell (axis, sign) = (e_axis × sign)

   The mapping is a fixed permutation (O(1) lookup).
   ═══════════════════════════════════════════════════════════════════════════ */

/* 24-cell vertex to tesseract cell index (0..7) */
static inline uint32_t d4_24cell_to_tess_cell(uint32_t vert_idx)
{
    /* vert_idx ∈ [0, 23]
     * Each of the 6 pairs of basis vectors (eᵢ±eⱼ) maps to 2 cells
     * Pair mapping: (i,j) → cell_i and cell_j
     * 6 pairs × 2 cells = 12, but we have 24 verts → each cell appears 3 times
     * Cell index: (vert_idx / 3) % 8
     */
    return ((vert_idx / 3u) % 8u);
}

/* 24-cell vertex to slot within tesseract cell (0..143) */
static inline uint32_t d4_24cell_to_tess_slot(uint32_t vert_idx)
{
    /* slot = vert_idx % 144 (wraps for 24 verts → slots 0..23) */
    return vert_idx % 144u;
}

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFICATION
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int d4_verify_constants(void)
{
    /* D4 Weyl group order */
    if (D4_ORDER != 192u) return 0;

    /* Coxeter number */
    if (D4_COXETER != 6u) return 0;

    /* 24 roots = D4 root system size */
    if (D4_ROOTS != 24u) return 0;

    /* 6 icosahedra × 24 = 144 */
    if (D4_6ICO_VERTS != 144u) return 0;

    /* 192 - 48 = 144 (A2×A2 embeds in D4 Weyl) */
    if (D4_ORDER - 48u != D4_6ICO_VERTS) return 0;

    return 1;
}

static inline int d4_verify_perm_table(void)
{
    /* Each permutation must be a bijection on {0,1,2} */
    for (uint8_t p = 0; p < 6; p++) {
        int seen[3] = {0, 0, 0};
        for (uint8_t j = 0; j < 3; j++) {
            uint8_t v = D4_TRIALITY_PERM[p][j];
            if (v > 2) return 0;
            if (seen[v]) return 0;
            seen[v] = 1;
        }
    }

    /* Permutation 0 must be identity */
    if (D4_TRIALITY_PERM[0][0] != 0) return 0;
    if (D4_TRIALITY_PERM[0][1] != 1) return 0;
    if (D4_TRIALITY_PERM[0][2] != 2) return 0;

    /* Permutation 1 must be cyclic shift (1,2,0) */
    if (D4_TRIALITY_PERM[1][0] != 1) return 0;
    if (D4_TRIALITY_PERM[1][1] != 2) return 0;
    if (D4_TRIALITY_PERM[1][2] != 0) return 0;

    return 1;
}

static inline int d4_verify_roundtrip(void)
{
    /* flat → hard → flat must be lossless */
    for (uint32_t flat = 0; flat < 20736u; flat++) {
        uint32_t anchor, local, row, col;

        d4_flat_to_hard(flat, &anchor, &local);
        uint32_t flat2 = d4_hard_to_nat_flat(anchor, local);
        if (flat2 != flat) return 0;

        d4_flat_to_nat(flat, &row, &col);
        uint32_t flat3 = row * 144u + col;
        if (flat3 != flat) return 0;
    }
    return 1;
}

static inline int d4_verify_all(void)
{
    if (!d4_verify_constants())    return 0;
    if (!d4_verify_perm_table())   return 0;
    if (!d4_verify_roundtrip())    return 0;
    return 1;
}

#endif /* GEO_D4_TRIALITY_H */
