/*
 * beam_geometric.c — Geometric Beam Codec Prototype
 * ═══════════════════════════════════════════════════════════════════
 *
 * "ใช้ weight เป็นความยาวของรัศมีที่เป็น beam"
 *
 * Grid construction:
 *   - Any point → project 60° → 3 vertices of equilateral triangle
 *   - Circumscribed circle through 3 points
 *   - Diameter D = single parameter (controls entire scale)
 *   - D/2 = radius R = centroid of triangle automatically
 *
 * Beam = weight:
 *   - weight w = radius of a beam from centroid
 *   - beam "drifts" the grid: w is decomposed as
 *     w = n*R + delta    (n = which cell, delta = within-cell remainder)
 *   - The grid's geometry (D parameter) defines R = D/2, which
 *     determines the scale of the entire icosahedron tiling.
 *   - Changing D shifts the entire scale — adaptive grid.
 *
 * Encode: weight → (cell_index, delta)
 *   1. Find which cell the beam drifts through: n = floor(|w|/R)
 *   2. Within-cell remainder: delta = |w| - n*R
 *   3. Pack as (n << 16) | delta
 *
 * Decode: (cell_index, delta) → weight
 *   1. w = n*R + delta
 *
 * The triangle geometry provides R = D/2. This is the ONLY parameter
 * needed — the beam is always relative to the cell scale.
 *
 * No float in hot path (all Q12 fixed-point integer).
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef BEAM_GEOMETRIC_C
#define BEAM_GEOMETRIC_C

#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
   SINGLE ADAPTIVE TRIANGLE CELL
   ══════════════════════════════════════════════════════════════
 *
 *   Equilaterial triangle with circumscribed circle diameter = D
 *
 *           V1 (60°)
 *           /\
 *          /  \
 *     R   / C  \   R = D/2 (circumradius)
 *        /      \
 *       /________\
 *     V0 (0°)    V2 (120°)
 *
 *   C = centroid = center of circumscribed circle
 *   R = D/2 = circumradius = centroid-to-vertex distance
 *
 *   The beam (radius = weight) extends from centroid.
 *   Within one cell: 0 ≤ beam ≤ R = D/2.
 *   Beam beyond R drifts into the next cell in the icosahedron.
 *
 *   Properties:
 *     side length s = R * sqrt(3) = D*sqrt(3)/2
 *     incircle radius r_in = R/2 = D/4
 *     height h = 3*R/2 = 3*D/4
 */

/* Fixed-point scaling: 1.0 = FP_SCALE (Q12: 12 fractional bits) */
#define FP_SCALE 4096

/* Cell descriptor — immutable geometry */
typedef struct {
    int32_t diameter;     /* D in Q12 fixed-point */
    int32_t radius;       /* R = D/2 in Q12 */
    int32_t side;         /* s = D*sqrt(3)/2 ≈ D*0.866 in Q12 */
    int32_t incircle_r;   /* r_in = D/4 in Q12 */
} GeoCell;

/* Initialize cell from diameter (in Q12 fixed-point) */
static inline GeoCell geo_cell_init(int32_t diameter_q12)
{
    GeoCell c;
    c.diameter = diameter_q12;
    c.radius   = diameter_q12 >> 1;                           /* D/2 */
    c.incircle_r = diameter_q12 >> 2;                         /* D/4 */
    /* side = D * sqrt(3)/2 ≈ D * 0.8660254 */
    c.side = (int32_t)((int64_t)diameter_q12 * 8660 / 10000); /* ~D*0.866 */
    return c;
}

/* ══════════════════════════════════════════════════════════════
   BEAM — radius from centroid that "drifts" the grid
   ══════════════════════════════════════════════════════════════
 *
 *   For a SINGLE cell: the beam is within the cell (|w| ≤ R).
 *   The delta = w (the beam is the drift directly).
 *   The cell's geometry provides scale context (R = D/2).
 *
 *   For the GRID (icosahedron): cells repeat with spacing R.
 *   A beam of radius w maps to:
 *     cell n:  w = n*R + delta   where delta ∈ [0, R)
 *     n = floor(|w| / R)
 *     delta = |w| - n*R
 *
 *   This decomposition IS the geometric codec:
 *   - n = which face/cell on the icosahedron
 *   - delta = within-cell position (what we store)
 *   - R = D/2 from the triangle geometry (single parameter)
 *
 *   Encode and decode at the single-cell level are trivial
 *   (delta = weight, weight = delta). The geometry is in the
 *   GRID tiling, not per-cell transformation.
 */

/* Encode: weight (Q12) → delta (Q12) within a single cell. */
static inline int32_t geo_beam_encode(const GeoCell *cell, int32_t weight_q12)
{
    (void)cell;
    return weight_q12;
}

/* Decode: delta (Q12) → weight (Q12) for a single cell. */
static inline int32_t geo_beam_decode(const GeoCell *cell, int32_t delta_q12)
{
    (void)cell;
    return delta_q12;
}

/* ══════════════════════════════════════════════════════════════
   GRID CHAIN — sequence of cells for multi-scale encoding
   ══════════════════════════════════════════════════════════════
 *
 *   Grid centroids form a lattice with spacing R = D/2:
 *     Centroid[n] = n * R
 *
 *   A beam of radius w is decomposed:
 *     n = floor(|w| / R)      → which cell
 *     delta = |w| - n*R        → within-cell position
 *
 *   The delta is in [0, R) and is stored as:
 *     upper 16 bits = cell index n
 *     lower 16 bits = delta in Q12 (unsigned)
 */

#define GEO_MAX_CHAIN 256

typedef struct {
    GeoCell cells[GEO_MAX_CHAIN];   /* chain of cells (same D) */
    uint32_t count;                  /* number of cells */
    int32_t  diameter;               /* common diameter (Q12) */
} GeoChain;

static inline GeoChain geo_chain_init(int32_t diameter_q12, uint32_t count)
{
    GeoChain ch;
    ch.diameter = diameter_q12;
    ch.count = (count > GEO_MAX_CHAIN) ? GEO_MAX_CHAIN : count;
    for (uint32_t i = 0; i < ch.count; i++) {
        ch.cells[i] = geo_cell_init(diameter_q12);
    }
    return ch;
}

/* Encode weight through chain.
 *   Packed return: sign * ((cell_index << 16) | normalized_within)
 *
 *   upper 16 bits = cell index (which centroid ring)
 *   lower 16 bits = within-cell fraction (0..65535 ≈ 0..R-epsilon)
 *     normalized_within = (within << 16) / R  → [0, 65535]
 *   sign of result = sign of weight
 *
 *   Normalizing keeps the packed format independent of D.
 *   Changing D rescales the entire codec.
 */
static inline int32_t geo_chain_encode(const GeoChain *ch, int32_t weight_q12)
{
    int32_t abs_w = (weight_q12 < 0) ? -weight_q12 : weight_q12;
    int32_t R     = ch->diameter >> 1;  /* R = D/2 */

    /* Which cell? */
    uint32_t cell_idx = (uint32_t)(abs_w / R);
    if (cell_idx >= ch->count) cell_idx = ch->count - 1;

    /* Within-cell remainder */
    int32_t within = abs_w - (int32_t)(cell_idx * R);
    if (within < 0) within = 0;
    if (within >= R) within = R - 1;

    /* Normalize within to [0, 65535] as fraction of R */
    uint32_t norm_within = (within == 0) ? 0 :
        (uint32_t)(((uint64_t)within << 16) / (uint32_t)R);
    if (norm_within > 65535) norm_within = 65535;

    /* Pack */
    uint32_t packed = (cell_idx << 16) | (norm_within & 0xFFFF);
    return (int32_t)((weight_q12 < 0) ? -(int32_t)packed : (int32_t)packed);
}

/* Decode: extract weight from packed.
 *   The sign of packed gives the sign of the result.
 */
static inline int32_t geo_chain_decode(const GeoChain *ch, int32_t packed)
{
    uint32_t abs_packed = (uint32_t)((packed < 0) ? -packed : packed);
    uint32_t cell_idx = (abs_packed >> 16) & 0xFFFF;
    uint32_t norm_within = abs_packed & 0xFFFF;

    if (cell_idx >= ch->count) cell_idx = 0;

    /* De-normalize: within = (norm_within * R) >> 16 */
    int32_t R = ch->diameter >> 1;
    int32_t within = (norm_within == 0) ? 0 :
        (int32_t)(((uint64_t)norm_within * (uint32_t)R) >> 16);

    int32_t w = (int32_t)(cell_idx * R) + within;
    return (packed < 0) ? -w : w;
}

/* ══════════════════════════════════════════════════════════════
   VERIFY — call once at init, returns 0 on pass
   ══════════════════════════════════════════════════════════════ */

static inline int beam_geometric_verify(void)
{
    /* T1: cell geometry */
    {
        GeoCell c = geo_cell_init(FP_SCALE * 2);  /* D=2.0 */
        if (c.radius != FP_SCALE)        return -1;  /* D/2 = 1.0 */
        if (c.incircle_r != FP_SCALE/2)  return -2;  /* D/4 = 0.5 */
        if (c.side <= 0)                 return -3;
    }

    /* T2: single cell roundtrip — trivial (delta = weight) */
    {
        GeoCell c = geo_cell_init(FP_SCALE * 4);
        if (geo_beam_decode(&c, geo_beam_encode(&c, FP_SCALE * 3)) != FP_SCALE * 3)
            return -4;
        if (geo_beam_decode(&c, geo_beam_encode(&c, -FP_SCALE * 2)) != -FP_SCALE * 2)
            return -5;
        if (geo_beam_decode(&c, geo_beam_encode(&c, 0)) != 0)
            return -6;
    }

    /* T3: chain roundtrip — with tolerance for fraction normalization */
    {
        GeoChain ch = geo_chain_init(FP_SCALE * 4, 16);  /* D=4.0, R=2.0 */
        for (int32_t w = -FP_SCALE * 10; w <= FP_SCALE * 10; w += FP_SCALE / 8) {
            int32_t packed = geo_chain_encode(&ch, w);
            int32_t r = geo_chain_decode(&ch, packed);
            int32_t err = r - w; if (err < 0) err = -err;
            if (err > 1) return -7;  /* fractions have ≤1 Q12 error */
        }
    }

    /* T4: chain roundtrip for small values (step=1) */
    {
        GeoChain ch = geo_chain_init(FP_SCALE * 4, 16);
        int32_t R = ch.diameter >> 1;
        for (int32_t w = -R * 5; w <= R * 5; w += 1) {
            int32_t packed = geo_chain_encode(&ch, w);
            int32_t r = geo_chain_decode(&ch, packed);
            int32_t err = r - w; if (err < 0) err = -err;
            if (err > 1) return -8;
        }
    }

    /* T5: negative roundtrip */
    {
        GeoChain ch = geo_chain_init(FP_SCALE * 8, 8);
        int32_t w = -FP_SCALE * 5;
        int32_t p = geo_chain_encode(&ch, w);
        int32_t r = geo_chain_decode(&ch, p);
        int32_t err = r - w; if (err < 0) err = -err;
        if (err > 1) return -9;
    }

    /* T6: chain roundtrip for large D */
    {
        GeoChain ch = geo_chain_init(FP_SCALE * 100, 16);  /* D=100 */
        int32_t R = ch.diameter >> 1;
        int bad = 0;
        for (int32_t w = -R * 7; w <= R * 7 + R - 1; w += R / 8) {
            int32_t packed = geo_chain_encode(&ch, w);
            int32_t r = geo_chain_decode(&ch, packed);
            int32_t err = r - w; if (err < 0) err = -err;
            if (err > 1) { bad++; if (bad <= 3) return -10; }
        }
        if (bad > 3) return -10;
    }

    return 0; /* ALL PASS */
}

#endif /* BEAM_GEOMETRIC_C */
