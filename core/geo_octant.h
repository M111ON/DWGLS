/*
 * geo_octant.h — Octant Identity + Zero-Sum Binding
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Each slot in the 20736 field has TWO coordinate identities stacked:
 *
 *   XYZ (cube view):  3 axes × 2 states = 8 octants
 *   IJK (tetra view): 3 axes × 2 states = 8 octants, bound by zero-sum
 *
 * The binding rule: i + j + k ∈ {0, 1}
 *   → Only 4 of 8 possible (i,j,k) combinations are valid
 *   → These 4 correspond to the 4 faces of a tetrahedron
 *
 * Combined: 4 (square) × 3 (triangle) = 12 = base unit
 *   → 12² = 144 = slots per cube
 *   → 144² = 20736 = full field
 *
 * CONVENTION (user design):
 *   - Cube index 0..7 = 3-bit octant (axis<<1 | sign)
 *   - Zero-sum selects which 4 of 8 octants are "tetra-active"
 *   - Every slot knows its octant via flat index ONLY (no lookup)
 *
 * Dependencies: <stdint.h> only — self-contained
 */

#ifndef GEO_OCTANT_H
#define GEO_OCTANT_H

#include <stdint.h>

/* ═══════════════ CONSTANTS ═══════════════ */

#define OCT_FULL         20736u
#define OCT_CUBES        8u        /* 3 bits: 2^3 = 8 octants */
#define OCT_CELLS        144u      /* slots per cube */
#define OCT_PER_TESS     1152u     /* 8 × 144 */
#define OCT_TESS_COUNT   18u

/* ═══════════════ OCTANT IDENTITY ═══════════════ */

/*
 * Octant = 3-bit index from (axis, sign):
 *   bit 2 = axis (0=x, 1=y, 2=z)
 *   bit 1 = sign (0=negative, 1=positive)
 *   bit 0 = reserved (cube view extension)
 *
 * Cube index from tesseract addressing:
 *   cell = (axis << 1) | sign   →  0..7
 *
 * This IS the octant identity — no computation needed,
 * just extract from flat address.
 */

/* Extract octant (cube index) from flat address */
static inline uint32_t oct_cube_of(uint32_t flat) {
    return (flat / OCT_CELLS) % OCT_CUBES;
}

/* Extract local slot within cube */
static inline uint32_t oct_slot_of(uint32_t flat) {
    return flat % OCT_CELLS;
}

/* Extract tesseract index from flat address */
static inline uint32_t oct_tess_of(uint32_t flat) {
    return flat / OCT_PER_TESS;
}

/* Reconstruct flat from (tess, cube, slot) */
static inline uint32_t oct_flat_of(uint32_t tess, uint32_t cube, uint32_t slot) {
    return (tess % OCT_TESS_COUNT) * OCT_PER_TESS
         + (cube % OCT_CUBES) * OCT_CELLS
         + (slot % OCT_CELLS);
}

/* ═══════════════ ZERO-SUM BINDING ═══════════════ */

/*
 * Zero-sum constraint: i + j + k ∈ {0, 1}
 *
 * Three axes (i, j, k) each ∈ {0, 1}:
 *   000 → sum=0 ✓ (origin)
 *   001 → sum=1 ✓
 *   010 → sum=1 ✓
 *   100 → sum=1 ✓
 *   011 → sum=2 ✗
 *   101 → sum=2 ✗
 *   110 → sum=2 ✗
 *   111 → sum=3 ✗
 *
 * Valid octants: {0, 1, 2, 4} (binary: 000, 001, 010, 100)
 * Invalid octants: {3, 5, 6, 7} (binary: 011, 101, 110, 111)
 *
 * These 4 valid octants = 4 faces of a tetrahedron inscribed in the cube.
 */

/* Compute i, j, k from cube index (3-bit octant) */
static inline void oct_ijk_of_cube(uint32_t cube, uint32_t *i, uint32_t *j, uint32_t *k) {
    *i = (cube >> 2) & 1u;  /* bit 2 */
    *j = (cube >> 1) & 1u;  /* bit 1 */
    *k = cube & 1u;         /* bit 0 */
}

/* Compute zero-sum from cube index */
static inline uint32_t oct_zero_sum(uint32_t cube) {
    uint32_t i, j, k;
    oct_ijk_of_cube(cube, &i, &j, &k);
    return i + j + k;
}

/* Check if cube index satisfies zero-sum constraint */
static inline int oct_is_valid(uint32_t cube) {
    return oct_zero_sum(cube) <= 1u;
}

/* Get the tetra-active octant for a given cube index.
 * If cube is already valid (sum ∈ {0,1}), return it.
 * If cube is invalid (sum > 1), strip bits until sum ≤ 1.
 *   7(111)→0(000), 6(110)→2(010), 5(101)→4(100), 3(011)→1(001) */
static inline uint32_t oct_tetra_of(uint32_t cube) {
    uint32_t sum = oct_zero_sum(cube);
    if (sum <= 1u) return cube;  /* already valid */
    /* strip lowest set bit repeatedly until sum ≤ 1 */
    uint32_t c = cube;
    while (oct_zero_sum(c) > 1u) {
        c &= c - 1u;  /* clear lowest set bit */
    }
    return c;
}

/* ═══════════════ ANTIPODAL RELATIONSHIP ═══════════════ */

/*
 * Antipodal = opposite corner of the cube.
 * In 3-bit octant: antipode = flip all 3 bits = 7 ^ cube.
 *
 * Zero-sum pairs:
 *   0 (000) ↔ 7 (111) — both invalid? No: 0 is valid (sum=0)
 *   1 (001) ↔ 6 (110) — 1 valid, 6 invalid
 *   2 (010) ↔ 5 (101) — 2 valid, 5 invalid
 *   4 (100) ↔ 3 (011) — 4 valid, 3 invalid
 *
 * Pattern: valid ↔ invalid pairs.
 * Store only the valid side → 1/2 compression.
 */

/* Get antipodal cube index (flip all 3 bits) */
static inline uint32_t oct_antipode_cube(uint32_t cube) {
    return 7u ^ (cube & 7u);
}

/* Get antipodal flat address (same tess, same slot, opposite cube) */
static inline uint32_t oct_antipode_flat(uint32_t flat) {
    uint32_t tess = oct_tess_of(flat);
    uint32_t cube = oct_cube_of(flat);
    uint32_t slot = oct_slot_of(flat);
    uint32_t anti_cube = oct_antipode_cube(cube);
    return oct_flat_of(tess, anti_cube, slot);
}

/* ═══════════════ SQUARE × TRIANGLE = 12 ═══════════════ */

/*
 * 4 (square faces) × 3 (triangle faces) = 12 = base unit
 *
 * Decompose flat address into square and triangle components:
 *   flat = tess × 1152 + cube × 144 + slot
 *   slot = sq × 3 + tri   (sq ∈ [0,48), tri ∈ [0,3))
 *   OR
 *   slot = sq × 3 + tri   where slot ∈ [0,144)
 *   → sq = slot / 3, tri = slot % 3
 *
 * But 144/3 = 48, not a clean Platonic number.
 * Better: use the 12×12 decomposition:
 *   slot = s12 × 12 + s0   (s12 ∈ [0,12), s0 ∈ [0,12))
 *   s12 = sq12 × 3 + tri12  (sq12 ∈ [0,4), tri12 ∈ [0,3))
 *   s0 = sq0 × 3 + tri0     (sq0 ∈ [0,4), tri0 ∈ [0,3))
 */

/* Decompose slot into 12×12 grid components */
static inline void oct_slot_to_12x12(uint32_t slot, uint32_t *s12, uint32_t *s0) {
    *s12 = slot / 12u;
    *s0 = slot % 12u;
}

/* Decompose 12-component into square × triangle */
static inline void oct_12_to_sq_tri(uint32_t s, uint32_t *sq, uint32_t *tri) {
    *sq = s / 3u;    /* ∈ [0,4) */
    *tri = s % 3u;   /* ∈ [0,3) */
}

/* Full decomposition: flat → (tess, cube, sq12, tri12, sq0, tri0) */
static inline void oct_full_decompose(uint32_t flat,
                                       uint32_t *tess, uint32_t *cube,
                                       uint32_t *sq12, uint32_t *tri12,
                                       uint32_t *sq0, uint32_t *tri0) {
    *tess = oct_tess_of(flat);
    *cube = oct_cube_of(flat);
    uint32_t slot = oct_slot_of(flat);
    uint32_t s12, s0;
    oct_slot_to_12x12(slot, &s12, &s0);
    oct_12_to_sq_tri(s12, sq12, tri12);
    oct_12_to_sq_tri(s0, sq0, tri0);
}

/* ═══════════════ VERIFICATION ═══════════════ */

/* Verify octant identity over full 20736 field */
static inline int geo_octant_verify(void) {
    for (uint32_t flat = 0; flat < OCT_FULL; flat++) {
        uint32_t tess = oct_tess_of(flat);
        uint32_t cube = oct_cube_of(flat);
        uint32_t slot = oct_slot_of(flat);

        /* Round-trip: flat → components → flat */
        uint32_t rt = oct_flat_of(tess, cube, slot);
        if (rt != flat) return 0;

        /* Octant range */
        if (cube >= OCT_CUBES) return 0;
        if (slot >= OCT_CELLS) return 0;
        if (tess >= OCT_TESS_COUNT) return 0;

        /* Zero-sum: i+j+k ∈ {0,1} for valid octants */
        uint32_t sum = oct_zero_sum(cube);
        if (sum > 1u) {
            /* Invalid octant — verify tetra mapping exists */
            uint32_t tetra = oct_tetra_of(cube);
            if (!oct_is_valid(tetra)) return 0;
        }

        /* Antipode round-trip: antipode of antipode = original */
        uint32_t anti = oct_antipode_flat(flat);
        uint32_t anti_anti = oct_antipode_flat(anti);
        if (anti_anti != flat) return 0;

        /* 12×12 decomposition round-trip */
        uint32_t s12, s0;
        oct_slot_to_12x12(slot, &s12, &s0);
        if (s12 * 12 + s0 != slot) return 0;
        if (s12 >= 12 || s0 >= 12) return 0;

        /* sq × 3 + tri round-trip */
        uint32_t sq, tri;
        oct_12_to_sq_tri(s12, &sq, &tri);
        if (sq * 3 + tri != s12) return 0;
        if (sq >= 4 || tri >= 3) return 0;
    }
    return 1;
}

#endif /* GEO_OCTANT_H */
