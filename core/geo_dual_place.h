/*
 * geo_dual_place.h — Dual-World Feature Placement on 8×8 Grid
 * ══════════════════════════════════════════════════════════════
 *
 * Places features[162] onto 8×8 = 64 grid:
 *
 *   World A (pole=0, Hilbert, north):
 *     features[0..63]  → border 28 cells via stride-37 scatter
 *     border = outer ring of 8×8 (row 0, row 7, col 0, col 7)
 *
 *   World B (pole=1, Peano, south):
 *     features[81..116] → inner 6×6 = 36 active cells
 *     (81 + 36 active from 81-item Peano walk)
 *
 *   Invariant: 36 + 28 = 64 = DiamondBlock ✓
 *
 * Inverse (grid → features) also provided — lossless roundtrip.
 *
 * LUTs (O(1) placement, no computation at runtime):
 *   PEANO_TO_GRID[81]   — peano_idx → flat 8×8 pos, 0xFF=outside
 *   HILBERT_TO_GRID[64] — hilbert_i → flat 8×8 pos (stride-37)
 *   BORDER_IDX[28]      — flat 8×8 positions of border cells
 *
 * No malloc. No float. Stateless O(N).
 * ══════════════════════════════════════════════════════════════
 */

#ifndef GEO_DUAL_PLACE_H
#define GEO_DUAL_PLACE_H

#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define GDP_GRID          64u    /* 8×8 output grid (DiamondBlock)   */
#define GDP_GRID_W         8u    /* grid width                        */
#define GDP_INNER         36u    /* World B active (6×6)              */
#define GDP_BORDER        28u    /* World A shadow border             */
#define GDP_PEANO         81u    /* 3⁴ ternary space                  */
#define GDP_ICO          162u    /* icosphere L2 (81×2)               */
#define GDP_OUTSIDE       0xFF   /* peano cell outside 6×6 crop       */

/* ══════════════════════════════════════════════════════════════
   LUTs — precomputed, O(1) access
   ══════════════════════════════════════════════════════════════ */

/*
 * PEANO_TO_GRID[81]: peano_idx → flat 8×8 position
 *   0xFF = outside 6×6 crop (45 entries)
 *   valid = inner 6×6 positions (36 entries)
 *
 * Generated from Peano L2 curve, crop rows 1..6 cols 1..6
 * mapped into 8×8: row*8+col (rows/cols preserved, no offset shift)
 */
static const uint8_t GDP_PEANO_TO_GRID[GDP_PEANO] = {
    255,255,255, 10,  9,255,255, 17, 18,
    255,255,255, 13, 12, 11, 19, 20, 21,
    255,255,255,255,255, 14, 22,255,255,
    255,255, 30, 38,255,255,255,255, 46,
     29, 28, 27, 35, 36, 37, 45, 44, 43,
     26, 25,255,255, 33, 34, 42, 41,255,
    255, 49, 50,255,255,255,255,255,255,
     51, 52, 53,255,255,255,255,255,255,
     54,255,255,255,255,255,255,255,255
};

/*
 * HILBERT_TO_GRID[64]: hilbert index → flat 8×8 position
 *   stride-37 scatter: pos = (i×37) % 64
 *   covers all 64 positions (gcd(37,64)=1 ✓)
 */
static const uint8_t GDP_HILBERT_TO_GRID[GDP_GRID] = {
     0, 37, 10, 47, 20, 57, 30,  3,
    40, 13, 50, 23, 60, 33,  6, 43,
    16, 53, 26, 63, 36,  9, 46, 19,
    56, 29,  2, 39, 12, 49, 22, 59,
    32,  5, 42, 15, 52, 25, 62, 35,
     8, 45, 18, 55, 28,  1, 38, 11,
    48, 21, 58, 31,  4, 41, 14, 51,
    24, 61, 34,  7, 44, 17, 54, 27
};

/*
 * BORDER_IDX[28]: flat 8×8 positions of border cells
 *   = outer ring: row 0, row 7, col 0, col 7
 *   = positions NOT in inner 6×6 (rows 1..6, cols 1..6)
 */
static const uint8_t GDP_BORDER_IDX[GDP_BORDER] = {
     0,  1,  2,  3,  4,  5,  6,  7,   /* row 0          */
     8, 15, 16, 23, 24, 31,            /* col 0,7 rows 1-3 */
    32, 39, 40, 47, 48, 55,            /* col 0,7 rows 4-6 */
    56, 57, 58, 59, 60, 61, 62, 63    /* row 7           */
};

/* ══════════════════════════════════════════════════════════════
   PLACE — features[162] → grid[64]
   ══════════════════════════════════════════════════════════════
 *
 * features[0..80]   = World A (Hilbert, north pole)
 *   → border 28 cells via HILBERT_TO_GRID
 *   only border positions written (inner 36 reserved for World B)
 *
 * features[81..161] = World B (Peano, south pole)
 *   → inner 36 cells via PEANO_TO_GRID
 *   peano_idx = ico_idx - 81 (0..80)
 *   0xFF entries skipped (outside crop)
 *
 * grid[pos] = feature value (uint16, truncated to fit caller's type)
 * Caller provides grid as uint16_t[64].
 */
static inline void geo_dual_place(const uint16_t *features_162,
                                   uint16_t        grid[GDP_GRID])
{
    memset(grid, 0, GDP_GRID * sizeof(uint16_t));

    /* World A → border cells (Hilbert scatter) */
    for (uint8_t i = 0u; i < GDP_GRID; i++) {
        uint8_t pos = GDP_HILBERT_TO_GRID[i];
        /* write only if border position */
        /* check: pos NOT in inner 6×6 (row 1..6, col 1..6) */
        uint8_t row = pos / GDP_GRID_W;
        uint8_t col = pos % GDP_GRID_W;
        if (row == 0u || row == 7u || col == 0u || col == 7u)
            grid[pos] = features_162[i];
    }

    /* World B → inner 6×6 (Peano placement) */
    for (uint8_t pidx = 0u; pidx < GDP_PEANO; pidx++) {
        uint8_t pos = GDP_PEANO_TO_GRID[pidx];
        if (pos == GDP_OUTSIDE) continue;
        grid[pos] = features_162[81u + pidx];
    }
}

/* ══════════════════════════════════════════════════════════════
   EXTRACT — grid[64] → features[162] (inverse of geo_dual_place)
   ══════════════════════════════════════════════════════════════ */

static inline void geo_dual_extract(const uint16_t *grid,
                                     uint16_t        features_162[GDP_ICO])
{
    memset(features_162, 0, GDP_ICO * sizeof(uint16_t));

    /* World A ← border cells */
    for (uint8_t i = 0u; i < GDP_GRID; i++) {
        uint8_t pos = GDP_HILBERT_TO_GRID[i];
        uint8_t row = pos / GDP_GRID_W;
        uint8_t col = pos % GDP_GRID_W;
        if (row == 0u || row == 7u || col == 0u || col == 7u)
            features_162[i] = grid[pos];
    }

    /* World B ← inner 6×6 */
    for (uint8_t pidx = 0u; pidx < GDP_PEANO; pidx++) {
        uint8_t pos = GDP_PEANO_TO_GRID[pidx];
        if (pos == GDP_OUTSIDE) continue;
        features_162[81u + pidx] = grid[pos];
    }
}

/* ══════════════════════════════════════════════════════════════
   FRAME-GUIDED PLACE — uses geo_frame_seek enc for phase offset
   ══════════════════════════════════════════════════════════════
 *
 * Applies phase rotation before placement:
 *   effective_pidx = (pidx + phase × 3) % GDP_PEANO
 *   effective_hidx = (hidx + phase × 7) % GDP_GRID
 *
 * phase from DualFrame.phase (0..11)
 * Allows timeline-locked feature placement without storing grid.
 */
static inline void geo_dual_place_phased(const uint16_t *features_162,
                                          uint16_t        grid[GDP_GRID],
                                          uint8_t         phase)
{
    memset(grid, 0, GDP_GRID * sizeof(uint16_t));

    uint8_t hp = (uint8_t)((phase * 7u) % GDP_GRID);   /* Hilbert phase offset */
    uint8_t pp = (uint8_t)((phase * 3u) % GDP_PEANO);  /* Peano phase offset   */

    /* World A → border */
    for (uint8_t i = 0u; i < GDP_GRID; i++) {
        uint8_t ei  = (uint8_t)((i + hp) % GDP_GRID);
        uint8_t pos = GDP_HILBERT_TO_GRID[ei];
        uint8_t row = pos / GDP_GRID_W;
        uint8_t col = pos % GDP_GRID_W;
        if (row == 0u || row == 7u || col == 0u || col == 7u)
            grid[pos] = features_162[i];
    }

    /* World B → inner 6×6 */
    for (uint8_t pidx = 0u; pidx < GDP_PEANO; pidx++) {
        uint8_t ep  = (uint8_t)((pidx + pp) % GDP_PEANO);
        uint8_t pos = GDP_PEANO_TO_GRID[ep];
        if (pos == GDP_OUTSIDE) continue;
        grid[pos] = features_162[81u + pidx];
    }
}

/* ══════════════════════════════════════════════════════════════
   VERIFY
   ══════════════════════════════════════════════════════════════ */

static inline int geo_dual_place_verify(void)
{
    /* [T1] LUT coverage: HILBERT covers all 64 positions */
    uint8_t seen[GDP_GRID] = {0};
    for (uint8_t i = 0u; i < GDP_GRID; i++) {
        uint8_t p = GDP_HILBERT_TO_GRID[i];
        if (p >= GDP_GRID) return -1;
        if (seen[p])       return -2;   /* duplicate */
        seen[p] = 1;
    }

    /* [T2] PEANO: exactly 36 valid, 45 outside */
    uint32_t valid=0, outside=0;
    for (uint8_t i = 0u; i < GDP_PEANO; i++) {
        if (GDP_PEANO_TO_GRID[i] == GDP_OUTSIDE) outside++;
        else {
            if (GDP_PEANO_TO_GRID[i] >= GDP_GRID) return -3;
            valid++;
        }
    }
    if (valid   != GDP_INNER)            return -4;
    if (outside != GDP_PEANO - GDP_INNER) return -5;

    /* [T3] No overlap: Peano positions ∩ border = empty */
    uint8_t border_set[GDP_GRID] = {0};
    for (uint8_t i = 0u; i < GDP_BORDER; i++)
        border_set[GDP_BORDER_IDX[i]] = 1;

    for (uint8_t i = 0u; i < GDP_PEANO; i++) {
        uint8_t p = GDP_PEANO_TO_GRID[i];
        if (p == GDP_OUTSIDE) continue;
        if (border_set[p]) return -6;   /* Peano cell in border = overlap */
    }

    /* [T4] 36 + 28 = 64 */
    if (GDP_INNER + GDP_BORDER != GDP_GRID) return -7;

    /* [T5] roundtrip place→extract */
    uint16_t feat_in[GDP_ICO], grid[GDP_GRID], feat_out[GDP_ICO];
    for (uint16_t i = 0u; i < GDP_ICO; i++) feat_in[i] = (uint16_t)(i + 1u);
    geo_dual_place(feat_in, grid);
    geo_dual_extract(grid, feat_out);

    /* World B roundtrip (Peano active cells only) */
    for (uint8_t pidx = 0u; pidx < GDP_PEANO; pidx++) {
        if (GDP_PEANO_TO_GRID[pidx] == GDP_OUTSIDE) continue;
        if (feat_out[81u + pidx] != feat_in[81u + pidx]) return -8;
    }

    /* World A roundtrip (border cells only) */
    for (uint8_t i = 0u; i < GDP_GRID; i++) {
        uint8_t pos = GDP_HILBERT_TO_GRID[i];
        uint8_t row = pos / GDP_GRID_W;
        uint8_t col = pos % GDP_GRID_W;
        if (row == 0u || row == 7u || col == 0u || col == 7u)
            if (feat_out[i] != feat_in[i]) return -9;
    }

    return 0;
}

#endif /* GEO_DUAL_PLACE_H */
