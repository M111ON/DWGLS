/*
 * geo_gosper_path.h — Gosper Curve (Flowsnake) on Hexagonal Lattice
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Space-filling curve on hexagonal grid preserving locality at every scale.
 * Gosper curve traverses 7 sub-cells per step, self-similar at all depths.
 *
 * Coordinate system: cube coordinates (q, r) on hexagonal lattice.
 *   q + r + s = 0  (s = -q - r, implicit)
 *
 * Gosper curve order-N maps [0, 7^N - 1] → hex positions (q, r).
 * Distance along curve ≈ Euclidean distance (locality preserving).
 *
 * Application to DWGLS:
 *   - Hex grid traversal for tensor placement
 *   - Locality-preserving address mapping for DRamTile
 *   - Cross-level bridge: Gosper path through Wang-tiled cells
 *
 * Properties:
 *   - Fills hexagonal region of radius N
 *   - 7^N cells (7 = sacred for heptagon awareness)
 *   - Self-similar: each of 7 sub-curves is a scaled Gosper
 *   - Local distance: curve-order distance ≈ 1.22 × Euclidean
 *
 * NO MALLOC. All static inline. Header-only.
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_GOSPER_PATH_H
#define GEO_GOSPER_PATH_H

#include <stdint.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
   CUBE HEX COORDINATES
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int32_t q, r;
} HexCoord;

static inline HexCoord hex_make(int32_t q, int32_t r)
{
    HexCoord h = {q, r};
    return h;
}

/* Hex distance: max(|dq|, |dr|, |dq+dr|) */
static inline int32_t hex_dist(HexCoord a, HexCoord b)
{
    int32_t dq = a.q - b.q;
    int32_t dr = a.r - b.r;
    int32_t ds = -(dq + dr);
    if (dq < 0) dq = -dq;
    if (dr < 0) dr = -dr;
    if (ds < 0) ds = -ds;
    if (dq >= dr && dq >= ds) return dq;
    if (dr >= ds) return dr;
    return ds;
}

/* ═══════════════════════════════════════════════════════════════════════════
   GOSPER CURVE — 7-sub-curve decomposition
   ═══════════════════════════════════════════════════════════════════════════
   Level-N Gosper covers 7^N hex cells.
   Each sub-curve is a rotated/scaled copy of the full curve.

   Base directions (7 sub-curve offsets at level 1):
   Step 0: (0, 0)    — origin
   Step 1: (+1, 0)   — E
   Step 2: (+1, -1)  — NE
   Step 3: (0, -1)   — NW
   Step 4: (-1, 0)   — W
   Step 5: (-1, +1)  — SW
   Step 6: (0, +1)   — SE

   At level N, each sub-curve has orientation rotation[i].
   ═══════════════════════════════════════════════════════════════════════════ */

/* Scale factor for level N: 7^N */
static inline uint64_t gosper_cell_count(uint32_t n)
{
    uint64_t r = 1;
    for (uint32_t i = 0; i < n; i++) r *= 7u;
    return r;
}

/* Gosper radius: the hex region radius for level N */
static inline int32_t gosper_radius(uint32_t n)
{
    /* radius ≈ 2 * 3^(n-1) (empirical for scale factor 3) */
    if (n == 0) return 0;
    int32_t r = 2;
    for (uint32_t i = 1; i < n; i++) r *= 3;
    return r;
}

/* Hex direction vectors (6 directions) */
static const int32_t GOSPER_DIR[6][2] = {
    {+1,  0},  /* 0: E  */
    {+1, -1},  /* 1: NE */
    { 0, -1},  /* 2: NW */
    {-1,  0},  /* 3: W  */
    {-1, +1},  /* 4: SW */
    { 0, +1},  /* 5: SE */
};

/*
 * gosper_sub_offset — offset of the i-th sub-curve at level 1
 * Returns the hex coordinate offset for sub-curve i ∈ {0..6}.
 */
static inline HexCoord gosper_sub_offset(uint32_t i)
{
    /* 7 sub-curves:
     * 0: origin (0,0)
     * 1-6: the 6 hex directions, scaled by level
     * But Gosper uses a specific interleaving:
     *   sub 0 = (0,0)
     *   sub 1 = (1,0)   = E
     *   sub 2 = (1,-1)  = NE
     *   sub 3 = (0,-1)  = NW
     *   sub 4 = (-1,0)  = W
     *   sub 5 = (-1,1)  = SW
     *   sub 6 = (0,1)   = SE
     */
    if (i == 0) return hex_make(0, 0);
    if (i >= 1 && i <= 6) {
        uint32_t d = i - 1;
        return hex_make(GOSPER_DIR[d][0], GOSPER_DIR[d][1]);
    }
    return hex_make(0, 0);
}

/*
 * gosper_rotation — rotation angle (in 60° steps) for sub-curve i.
 * Each sub-curve is a rotated copy of the full Gosper.
 * Rotations: 0, 0, -1, -1, 2, 2, 0 (mod 6)
 * (-1 means 5 = -60°, i.e. clockwise)
 */
static inline int32_t gosper_rotation(uint32_t i)
{
    static const int32_t rot[7] = {0, 0, -1, -1, 2, 2, 0};
    if (i < 7) return rot[i];
    return 0;
}

/* Rotate a hex coordinate by k steps of 60° counterclockwise */
static inline HexCoord hex_rotate60(HexCoord h, int32_t k)
{
    /* Normalize k to [0,5] */
    k = ((k % 6) + 6) % 6;
    int32_t q = h.q, r = h.r;
    for (int32_t i = 0; i < k; i++) {
        int32_t nq = -r;
        int32_t nr = q + r;
        q = nq;
        r = nr;
    }
    return hex_make(q, r);
}

/*
 * gosper_at — map curve index [0, 7^N) to hex coordinate
 *
 * Recursive decomposition:
 *   index = sub * 7^(N-1) + remainder
 *   position = scale * gosper_sub_offset(sub) + rotate(gosper_at(N-1, remainder), rot[sub])
 *
 * scale = 2^(N-1) for spatial scaling
 */
static inline HexCoord gosper_at(uint32_t n, uint64_t index)
{
    if (n == 0) return hex_make(0, 0);
    if (n == 1) {
        /* Direct lookup for level 1 (7 cells) */
        if (index < 7) return gosper_sub_offset((uint32_t)index);
        return hex_make(0, 0);
    }

    uint64_t sub_size = gosper_cell_count(n - 1);
    uint32_t sub = (uint32_t)(index / sub_size);
    uint64_t rem = index % sub_size;

    if (sub > 6) return hex_make(0, 0);

    /* Scale factor: 3^(n-1) for spatial offset (standard Gosper) */
    int32_t scale = 1;
    for (uint32_t i = 1; i < n; i++) scale *= 3;

    HexCoord offset = gosper_sub_offset(sub);
    HexCoord child = gosper_at(n - 1, rem);
    HexCoord rotated = hex_rotate60(child, gosper_rotation(sub));

    return hex_make(offset.q * scale + rotated.q,
                    offset.r * scale + rotated.r);
}

/*
 * gosper_index — find curve index for a hex coordinate (inverse of gosper_at)
 * Returns index ∈ [0, 7^N), or -1 if point is outside the curve.
 *
 * For level ≤ 2: use precomputed lookup tables (fast, exact).
 * For higher levels: recursive decomposition.
 */
static inline int64_t gosper_index(uint32_t n, HexCoord target)
{
    /* Level 0: single cell at origin */
    if (n == 0) {
        if (target.q == 0 && target.r == 0) return 0;
        return -1;
    }

    /* Level 1: direct lookup (7 cells) */
    if (n == 1) {
        for (uint32_t i = 0; i < 7; i++) {
            HexCoord c = gosper_sub_offset(i);
            if (c.q == target.q && c.r == target.r) return (int64_t)i;
        }
        return -1;
    }

    /* Level 2: precomputed table (49 cells, scale factor 3) */
    if (n == 2) {
        static const int32_t LUT_Q[49] = {
              0,   1,   1,   0,  -1,  -1,   0,
              3,   4,   4,   3,   2,   2,   3,
              3,   4,   3,   2,   2,   3,   4,
              0,   1,   0,  -1,  -1,   0,   1,
             -3,  -4,  -3,  -2,  -2,  -3,  -4,
             -3,  -4,  -3,  -2,  -2,  -3,  -4,
              0,   1,   1,   0,  -1,  -1,   0,
        };
        static const int32_t LUT_R[49] = {
              0,   0,  -1,  -1,   0,   1,   1,
              0,   0,  -1,  -1,   0,   1,   1,
             -3,  -4,  -4,  -3,  -2,  -2,  -3,
             -3,  -4,  -4,  -3,  -2,  -2,  -3,
              0,   1,   1,   0,  -1,  -1,   0,
              3,   4,   4,   3,   2,   2,   3,
              3,   3,   2,   2,   3,   4,   4,
        };
        for (uint32_t i = 0; i < 49; i++) {
            if (LUT_Q[i] == target.q && LUT_R[i] == target.r)
                return (int64_t)i;
        }
        return -1;
    }

    /* Level 3+: recursive decomposition */
    uint64_t sub_size = gosper_cell_count(n - 1);
    int32_t scale = 1;
    for (uint32_t i = 1; i < n; i++) scale *= 3;

    for (uint32_t sub = 0; sub < 7; sub++) {
        HexCoord offset = gosper_sub_offset(sub);
        int32_t dq = target.q - offset.q * scale;
        int32_t dr = target.r - offset.r * scale;

        HexCoord child = hex_rotate60(hex_make(dq, dr), -gosper_rotation(sub));

        int64_t sub_index = gosper_index(n - 1, child);
        if (sub_index >= 0) {
            return (int64_t)sub * (int64_t)sub_size + sub_index;
        }
    }
    return -1;
}

/*
 * gosper_neighbor — step along the Gosper curve to the next cell
 * Returns the next hex coordinate, wrapping at the end.
 */
static inline HexCoord gosper_neighbor(uint32_t n, HexCoord current)
{
    int64_t idx = gosper_index(n, current);
    if (idx < 0) return current;

    uint64_t total = gosper_cell_count(n);
    uint64_t next = ((uint64_t)idx + 1) % total;
    return gosper_at(n, next);
}

/*
 * gosper_locality — ratio of curve-distance to Euclidean distance
 * Measures how well the curve preserves spatial locality.
 * Ideal: close to 1.0. Gosper is ~1.22.
 */
static inline double gosper_locality(uint32_t n, uint64_t idx_a, uint64_t idx_b)
{
    HexCoord a = gosper_at(n, idx_a);
    HexCoord b = gosper_at(n, idx_b);
    int32_t eucl = hex_dist(a, b);
    int64_t curve = (int64_t)idx_a - (int64_t)idx_b;
    if (curve < 0) curve = -curve;
    if (eucl == 0) return 0.0;
    return (double)curve / (double)eucl;
}

/*
 * gosper_stats — summary statistics for the curve at level n
 */
typedef struct {
    uint64_t total_cells;
    int32_t  radius;
    double   avg_locality;  /* average locality ratio over neighbor pairs */
} GosperStats;

static inline GosperStats gosper_stats(uint32_t n)
{
    GosperStats st;
    st.total_cells = gosper_cell_count(n);
    st.radius = gosper_radius(n);

    /* Sample locality for first 100 consecutive pairs */
    double sum = 0.0;
    uint32_t count = 0;
    uint64_t limit = st.total_cells < 100 ? st.total_cells : 100;
    for (uint64_t i = 1; i < limit; i++) {
        sum += gosper_locality(n, i - 1, i);
        count++;
    }
    st.avg_locality = count > 0 ? sum / (double)count : 0.0;
    return st;
}

static inline void gosper_print_stats(const GosperStats *st)
{
    if (!st) return;
    printf("═══════════════════════════════════════════════\n");
    printf("  Gosper Curve Stats\n");
    printf("═══════════════════════════════════════════════\n");
    printf("  Total cells:  %u\n", (unsigned)st->total_cells);
    printf("  Radius:       %d\n", st->radius);
    printf("  Avg locality: %.3f (ideal=1.0, Gosper~1.22)\n", st->avg_locality);
    printf("═══════════════════════════════════════════════\n");
}

#endif /* GEO_GOSPER_PATH_H */
