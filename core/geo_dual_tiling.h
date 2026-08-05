/*
 * geo_dual_tiling.h — Dual-World Weight Tiling & Sparsity Analysis
 * ══════════════════════════════════════════════════════════════
 *
 * Splits a flat weight buffer into World A (border, 28 cells)
 * and World B (inner, 36 cells) based on geo_dual_place mapping.
 *
 * Key insight: World A (border) tends to have sparse/near-zero weights.
 * World B (inner) tends to have dense/important weights.
 * During inference, World A can be skipped for speedup.
 *
 * Depends: geo_dual_place.h, geo_cube_addr.h
 *
 * No malloc. Stateless O(N).
 * ══════════════════════════════════════════════════════════════
 */

#ifndef GEO_DUAL_TILING_H
#define GEO_DUAL_TILING_H

#include "geo_dual_place.h"
#include "geo_cube_addr.h"

#include <stdio.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define DUAL_TILE_NEAR_ZERO  1e-6f   /* threshold for "sparse" weight  */
#define DUAL_TILE_GRID_SIZE  GDP_GRID  /* 64 — from geo_dual_place.h  */

/* ══════════════════════════════════════════════════════════════
   STRUCT — DualTileResult
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t world_a_count;            /* always 28 */
    uint32_t world_b_count;            /* always 36 */
    uint32_t world_a_indices[GDP_BORDER]; /* flat grid positions of World A */
    uint32_t world_b_indices[GDP_INNER];  /* flat grid positions of World B */
    float    world_a_sum;              /* sum of |World A weights| */
    float    world_b_sum;              /* sum of |World B weights| */
    uint32_t world_a_sparse;           /* count of near-zero in World A */
    uint32_t world_b_sparse;           /* count of near-zero in World B */
} DualTileResult;

/* ══════════════════════════════════════════════════════════════
   geo_dual_tile_split — split weights into World A / World B
   ══════════════════════════════════════════════════════════════
 *
 * weights[0..63] — flat 8×8 grid weights (or any 64-element buffer)
 *
 * Populates result with:
 *   - World A indices (border cells, 28 entries)
 *   - World B indices (inner cells, 36 entries)
 *   - Sum of |weights| per world
 *   - Sparse count per world (|w| < DUAL_TILE_NEAR_ZERO)
 */
static inline DualTileResult geo_dual_tile_split(const float *weights,
                                                  uint32_t    n_weights)
{
    DualTileResult r;
    r.world_a_count = 0;
    r.world_b_count = 0;
    r.world_a_sum   = 0.0f;
    r.world_b_sum   = 0.0f;
    r.world_a_sparse = 0;
    r.world_b_sparse = 0;

    /* Build a fast lookup: is this grid position border or inner? */
    uint8_t is_border[DUAL_TILE_GRID_SIZE];
    for (uint32_t i = 0; i < DUAL_TILE_GRID_SIZE; i++) is_border[i] = 0;
    for (uint32_t i = 0; i < GDP_BORDER; i++)
        is_border[GDP_BORDER_IDX[i]] = 1;

    for (uint32_t i = 0; i < DUAL_TILE_GRID_SIZE && i < n_weights; i++) {
        float w = weights[i];
        float aw = fabsf(w);

        if (is_border[i]) {
            /* World A — border */
            r.world_a_indices[r.world_a_count++] = i;
            r.world_a_sum += aw;
            if (aw < DUAL_TILE_NEAR_ZERO) r.world_a_sparse++;
        } else {
            /* World B — inner */
            r.world_b_indices[r.world_b_count++] = i;
            r.world_b_sum += aw;
            if (aw < DUAL_TILE_NEAR_ZERO) r.world_b_sparse++;
        }
    }

    return r;
}

/* ══════════════════════════════════════════════════════════════
   geo_dual_tile_sparsity — sparsity ratio (near-zero / total)
   ══════════════════════════════════════════════════════════════
 *
 * Returns fraction of near-zero weights in each world.
 * Higher = sparser.
 *
 * result[0] = World A sparsity (border)
 * result[1] = World B sparsity (inner)
 */
static inline void geo_dual_tile_sparsity(const float *weights,
                                           uint32_t    n_weights,
                                           float       result[2])
{
    DualTileResult t = geo_dual_tile_split(weights, n_weights);

    result[0] = (t.world_a_count > 0)
        ? (float)t.world_a_sparse / (float)t.world_a_count
        : 0.0f;

    result[1] = (t.world_b_count > 0)
        ? (float)t.world_b_sparse / (float)t.world_b_count
        : 0.0f;
}

/* ══════════════════════════════════════════════════════════════
   geo_dual_tile_demo — demonstrate split + sparsity
   ══════════════════════════════════════════════════════════════ */

static inline void geo_dual_tile_demo(const float *weights,
                                       uint32_t    n_weights)
{
    DualTileResult t = geo_dual_tile_split(weights, n_weights);

    printf("===============================================================\n");
    printf("  DualTile — World A (Border) / World B (Inner) Split\n");
    printf("---------------------------------------------------------------\n");
    printf("  World A (border):  %u cells, |sum|=%.4f, sparse=%u\n",
           t.world_a_count, t.world_a_sum, t.world_a_sparse);
    printf("  World B (inner):  %u cells, |sum|=%.4f, sparse=%u\n",
           t.world_b_count, t.world_b_sum, t.world_b_sparse);
    printf("---------------------------------------------------------------\n");

    float sparsity[2];
    geo_dual_tile_sparsity(weights, n_weights, sparsity);
    printf("  World A sparsity: %.1f%%\n", sparsity[0] * 100.0f);
    printf("  World B sparsity: %.1f%%\n", sparsity[1] * 100.0f);

    if (sparsity[0] > sparsity[1]) {
        printf("  → World A is sparser (border skip candidate)\n");
    } else {
        printf("  → World B is sparser (unusual — check weight distribution)\n");
    }

    printf("---------------------------------------------------------------\n");
    printf("  World A indices: ");
    for (uint32_t i = 0; i < t.world_a_count; i++) {
        printf("%u ", t.world_a_indices[i]);
    }
    printf("\n  World B indices: ");
    for (uint32_t i = 0; i < t.world_b_count; i++) {
        printf("%u ", t.world_b_indices[i]);
    }
    printf("\n");
    printf("===============================================================\n");
}

#endif /* GEO_DUAL_TILING_H */
