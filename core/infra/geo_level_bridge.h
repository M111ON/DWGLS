/*
 * geo_level_bridge.h — Cross-Level Wang Gate for Multi-Resolution Connectivity
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Connects different resolution levels (h, h-1) via Wang edge invariants.
 *
 * Hierarchy (base-12):
 *   h=4: root (1 cell)
 *   h=3: 12 groups (grid 12×1)
 *   h=2: 144 regions (grid 12×12)
 *   h=1: 1728 pipes (grid 144×12)
 *   h=0: 20736 leaves (grid 144×144)
 *
 * Vertical connection between levels:
 *   Level h (coarse) ↔ Level h-1 (fine)
 *   Connection position determined by:
 *     1. Chord invariant (chord 2&7 from Wang tile)
 *     2. Line-sum compatibility
 *     3. n15 preservation
 *
 * DESIGN:
 *   No malloc. All static inline. Header-only.
 *   C99. Compatible with geo_fractal_addr.h, geo_entropy_quadtree.h.
 *
 * DEPENDS: core/geo_fractal_addr.h
 * ═══════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_LEVEL_BRIDGE_H
#define GEO_LEVEL_BRIDGE_H

#include <stdint.h>
#include "../geo_fractal_addr.h"

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

/* Wang edge chord positions (invariant: sum = 9) */
#define LB_CHORD_A     2u
#define LB_CHORD_B     7u
#define LB_CHORD_SUM   9u

/* Bridge states */
#define LB_PASS        0u
#define LB_BLOCK       1u
#define LB_BROADCAST   2u
#define LB_MERGE       3u

/* Maximum bridges per node */
#define LB_MAX_BRIDGES 16u

/* ═══════════════════════════════════════════════════════════════════════════
   BRIDGE STRUCTURE
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t     level_top;      /* coarse level (h)          */
    uint8_t     level_bot;      /* fine level (h-1)          */
    FractalAddr pos_top;        /* connection on coarse grid */
    FractalAddr pos_bot;        /* connection on fine grid   */
    uint8_t     chord_a;        /* chord from coarse side    */
    uint8_t     chord_b;        /* chord from fine side      */
    uint8_t     state;          /* LB_PASS / LB_BLOCK / ...  */
    uint8_t     weight;         /* synaptic weight (0..255)  */
    uint32_t    flat_top;       /* flat offset coarse         */
    uint32_t    flat_bot;       /* flat offset fine           */
} LB_Bridge;

/* ═══════════════════════════════════════════════════════════════════════════
   CHORD INVARIANT
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t lb_check_chord(uint8_t chord_a, uint8_t chord_b)
{
    return (chord_a + chord_b == LB_CHORD_SUM) ? 1u : 0u;
}

/*
 * lb_compute_chord — derive chord from line-sum fingerprint
 * Chord = sum of first two row sums modulo 9
 */
static inline uint8_t lb_compute_chord(const uint8_t grid[3][3])
{
    FractalLineSum ls = fractal_line_sum_3x3(grid);
    uint32_t raw = ((uint32_t)ls.row[0] + (uint32_t)ls.row[1]) % 9u;
    return (uint8_t)raw;
}

/* ═══════════════════════════════════════════════════════════════════════════
   BRIDGE CREATION
   ═══════════════════════════════════════════════════════════════════════════ */

static inline LB_Bridge lb_bridge_create(
    uint8_t h,
    const uint8_t coarse_grid[3][3],
    const uint8_t fine_grid[3][3],
    FractalAddr pos_coarse,
    FractalAddr pos_fine
)
{
    LB_Bridge b;
    b.level_top = h;
    b.level_bot = (uint8_t)(h - 1u);
    b.pos_top = pos_coarse;
    b.pos_bot = pos_fine;
    b.flat_top = fractal_to_flat(h, pos_coarse.x, pos_coarse.y);
    b.flat_bot = fractal_to_flat(h - 1, pos_fine.x, pos_fine.y);

    b.chord_a = lb_compute_chord(coarse_grid);
    b.chord_b = lb_compute_chord(fine_grid);

    if (lb_check_chord(b.chord_a, b.chord_b)) {
        b.state = LB_PASS;
    } else {
        b.state = LB_BLOCK;
    }

    /* Weight = n15 from coarse side (higher = more magic = stronger) */
    FractalLineSum ls = fractal_line_sum_3x3(coarse_grid);
    uint8_t n15 = fractal_n15_count(ls);
    b.weight = (uint8_t)(n15 * 32u);  /* 0..255 (8*32=256, capped by uint8) */

    return b;
}

/*
 * lb_bridge_auto — auto-detect connection between levels
 *
 * Connection: boundary where coarse cell meets fine cell.
 * At level h, grid is gw_h × gh_h. Each coarse cell maps to
 * 12^(4-h) fine cells. Connection at bottom-right corner.
 */
static inline LB_Bridge lb_bridge_auto(
    uint8_t h,
    const uint8_t coarse_grid[3][3],
    const uint8_t fine_grid[3][3],
    uint32_t cx, uint32_t cy,   /* coarse cell position */
    uint32_t fx, uint32_t fy    /* fine cell position   */
)
{
    FractalAddr pc = { h, cx, cy };
    FractalAddr pf = { (uint8_t)(h - 1), fx, fy };
    return lb_bridge_create(h, coarse_grid, fine_grid, pc, pf);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SIGNAL PROPAGATION
   ═══════════════════════════════════════════════════════════════════════════ */

/* Upward: fine → coarse. Returns weighted signal, 0 if blocked. */
static inline uint8_t lb_propagate_up(const LB_Bridge *b, uint8_t signal)
{
    if (b->state == LB_BLOCK) return 0u;
    return (uint8_t)((uint32_t)signal * (uint32_t)b->weight / 255u);
}

/* Downward: coarse → fine. Returns weighted signal, 0 if blocked. */
static inline uint8_t lb_propagate_down(const LB_Bridge *b, uint8_t signal)
{
    if (b->state == LB_BLOCK) return 0u;
    return (uint8_t)((uint32_t)signal * (uint32_t)b->weight / 255u);
}

/* ═══════════════════════════════════════════════════════════════════════════
   BRIDGE SET — all bridges for a multi-resolution tree
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    LB_Bridge bridges[LB_MAX_BRIDGES];
    uint8_t    count;
    uint8_t    active;
} LB_BridgeSet;

static inline LB_BridgeSet lb_set_init(void)
{
    LB_BridgeSet s;
    s.count = 0;
    s.active = 0;
    for (uint8_t i = 0; i < LB_MAX_BRIDGES; i++) {
        s.bridges[i].state = LB_BLOCK;
    }
    return s;
}

static inline int8_t lb_set_add(LB_BridgeSet *set, LB_Bridge b)
{
    if (set->count >= LB_MAX_BRIDGES) return -1;
    set->bridges[set->count] = b;
    if (b.state == LB_PASS) set->active++;
    set->count++;
    return (int8_t)(set->count - 1);
}

static inline uint8_t lb_set_stats(const LB_BridgeSet *set, uint8_t counts[4])
{
    counts[0] = 0; counts[1] = 0; counts[2] = 0; counts[3] = 0;
    for (uint8_t i = 0; i < set->count; i++) {
        counts[set->bridges[i].state]++;
    }
    return counts[0];
}

static inline uint32_t lb_set_total_weight(const LB_BridgeSet *set)
{
    uint32_t total = 0;
    for (uint8_t i = 0; i < set->count; i++) {
        if (set->bridges[i].state == LB_PASS) {
            total += set->bridges[i].weight;
        }
    }
    return total;
}

#endif /* GEO_LEVEL_BRIDGE_H */
