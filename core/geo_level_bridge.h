/*
 * geo_level_bridge.h — Wang Gate Bridge Between Fractal Levels
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Connects Wang tile edge invariants to the fractal address hierarchy.
 * Each cell boundary must satisfy chord 2&7 invariant across levels.
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 * Chord Invariant: edge_a + edge_b = 9 (same as Wang tile)
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Level h cell boundaries have 4 edges (TOP, RIGHT, BOTTOM, LEFT).
 * Adjacent cells share an edge → edge values must sum to 9.
 *
 * Ancestor connections (h+1 → h) use the Wang edge value at the parent
 * boundary to constrain which child cells are valid.
 *
 * NO MALLOC. All static inline. Header-only.
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_LEVEL_BRIDGE_H
#define GEO_LEVEL_BRIDGE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "geo_fractal_addr.h"

/* ═══════════════════════════════════════════════════════════════════════════
   EDGE IDENTIFICATION — which side of a cell
   ═══════════════════════════════════════════════════════════════════════════ */

#define BRIDGE_EDGE_TOP     0u
#define BRIDGE_EDGE_RIGHT   1u
#define BRIDGE_EDGE_BOTTOM  2u
#define BRIDGE_EDGE_LEFT    3u

/* ═══════════════════════════════════════════════════════════════════════════
   CHORD INVARIANT — edge_a + edge_b = 9
   ═══════════════════════════════════════════════════════════════════════════
   Returns 1 if the two edge values satisfy the Wang invariant.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t bridge_chord_ok(uint8_t edge_a, uint8_t edge_b)
{
    return (uint32_t)(edge_a + edge_b == 9u);
}

/* ═══════════════════════════════════════════════════════════════════════════
   CELL EDGE VALUE — deterministic from address
   ═══════════════════════════════════════════════════════════════════════════
   Each cell (h,x,y) has 4 edge values derived from its address.
   Formula: edge = (addr * 7 + edge_id * 13) mod 9 → [0..8]
   *7 and *13 are coprime to 12 → ensures good distribution.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t bridge_cell_edge(uint32_t h, uint32_t x, uint32_t y,
                                       uint32_t edge_id)
{
    /* Deterministic Wang tiling from address bits.
     * LEFT  = (addr + x*7) % 9
     * RIGHT = 9 - LEFT of (x+1,y) = 9 - ((addr_right + (x+1)*7) % 9)
     * TOP   = (addr + y*13) % 9
     * BOTTOM = 9 - TOP of (x,y+1) = 9 - ((addr_bot + (y+1)*13) % 9)
     * This ensures chord invariant holds by construction.
     */
    uint32_t addr = fractal_to_flat(h, x, y);
    uint32_t gw = fractal_grid_w(h);

    switch (edge_id) {
        case BRIDGE_EDGE_LEFT:
            return (uint8_t)((addr + x * 7u) % 9u);
        case BRIDGE_EDGE_RIGHT: {
            uint32_t nx = (x + 1u) % gw;
            uint32_t naddr = fractal_to_flat(h, nx, y);
            return (uint8_t)(9u - ((naddr + nx * 7u) % 9u));
        }
        case BRIDGE_EDGE_TOP:
            return (uint8_t)((addr + y * 13u) % 9u);
        case BRIDGE_EDGE_BOTTOM: {
            uint32_t ny = (y + 1u) % fractal_grid_h(h);
            uint32_t naddr = fractal_to_flat(h, x, ny);
            return (uint8_t)(9u - ((naddr + ny * 13u) % 9u));
        }
        default:
            return (uint8_t)(addr % 9u);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   NEIGHBOR CHORD CHECK — two adjacent cells must satisfy invariant
   ═══════════════════════════════════════════════════════════════════════════
   Given cell A at (h, x, y) and its neighbor in direction `dir`:
   - A's edge in that direction vs neighbor's opposing edge must sum to 9.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t bridge_neighbor_chord_ok(uint32_t h, uint32_t x, uint32_t y,
                                                 uint32_t dir)
{
    uint32_t gw = fractal_grid_w(h);
    uint32_t gh = fractal_grid_h(h);

    /* Neighbor coordinates */
    uint32_t nx = x, ny = y;
    uint32_t my_edge, opp_edge;
    switch (dir) {
        case BRIDGE_EDGE_TOP:
            if (y == 0) return 0;
            ny = y - 1;
            my_edge = BRIDGE_EDGE_TOP;
            opp_edge = BRIDGE_EDGE_BOTTOM;
            break;
        case BRIDGE_EDGE_BOTTOM:
            if (y >= gh - 1) return 0;
            ny = y + 1;
            my_edge = BRIDGE_EDGE_BOTTOM;
            opp_edge = BRIDGE_EDGE_TOP;
            break;
        case BRIDGE_EDGE_LEFT:
            if (x == 0) return 0;
            nx = x - 1;
            my_edge = BRIDGE_EDGE_LEFT;
            opp_edge = BRIDGE_EDGE_RIGHT;
            break;
        case BRIDGE_EDGE_RIGHT:
            if (x >= gw - 1) return 0;
            nx = x + 1;
            my_edge = BRIDGE_EDGE_RIGHT;
            opp_edge = BRIDGE_EDGE_LEFT;
            break;
        default: return 0;
    }

    uint8_t ea = bridge_cell_edge(h, x,  y, my_edge);
    uint8_t eb = bridge_cell_edge(h, nx, ny, opp_edge);
    return bridge_chord_ok(ea, eb);
}

/* ═══════════════════════════════════════════════════════════════════════════
   ANCESTRAL EDGE — parent's edge constrains child validity
   ═══════════════════════════════════════════════════════════════════════════
   At level h, a cell's parent (h+1) has an edge value. The child cell
   at that boundary must satisfy chord with the parent edge.

   Returns 1 if child (h, x, y) satisfies the chord with its parent.
   Root (h = FRACTAL_MAX_H) always passes (no parent).
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t bridge_ancestral_chord_ok(uint32_t h, uint32_t x, uint32_t y)
{
    if (h >= FRACTAL_MAX_H) return 1u; /* root has no parent */

    FractalAddr parent = fractal_parent(x, y, h);
    /* Check child's LEFT edge vs parent's LEFT edge */
    uint8_t parent_edge = bridge_cell_edge(h + 1, parent.x, parent.y, BRIDGE_EDGE_LEFT);
    uint8_t child_edge  = bridge_cell_edge(h, x, y, BRIDGE_EDGE_LEFT);
    return bridge_chord_ok(parent_edge, child_edge);
}

/* ═══════════════════════════════════════════════════════════════════════════
   BRIDGE VERIFICATION — check all boundaries in a level
   ═══════════════════════════════════════════════════════════════════════════
   Returns number of chord violations in the given level.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t bridge_verify_level(uint32_t h)
{
    uint32_t gw = fractal_grid_w(h);
    uint32_t gh = fractal_grid_h(h);
    uint32_t violations = 0;

    for (uint32_t y = 0; y < gh; y++) {
        for (uint32_t x = 0; x < gw; x++) {
            /* Check RIGHT neighbor (avoids double-count) */
            if (x < gw - 1)
                violations += (1u - bridge_neighbor_chord_ok(h, x, y, BRIDGE_EDGE_RIGHT));
            /* Check BOTTOM neighbor */
            if (y < gh - 1)
                violations += (1u - bridge_neighbor_chord_ok(h, x, y, BRIDGE_EDGE_BOTTOM));
        }
    }
    return violations;
}

/* ═══════════════════════════════════════════════════════════════════════════
   BRIDGE STATS — summary of chord compliance per level
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_bridges;   /* total neighbor pairs checked */
    uint32_t passed;          /* chord satisfied */
    uint32_t failed;          /* chord violated */
    uint32_t ancestral_ok;    /* parent-child chords satisfied */
    uint32_t ancestral_fail;  /* parent-child chords violated */
} BridgeStats;

static inline BridgeStats bridge_verify_all(void)
{
    BridgeStats st;
    __builtin_memset(&st, 0, sizeof(st));

    for (uint32_t h = 0; h <= FRACTAL_MAX_H; h++) {
        uint32_t gw = fractal_grid_w(h);
        uint32_t gh = fractal_grid_h(h);
        for (uint32_t y = 0; y < gh; y++) {
            for (uint32_t x = 0; x < gw; x++) {
                /* Neighbor chords */
                if (x < gw - 1) {
                    st.total_bridges++;
                    if (bridge_neighbor_chord_ok(h, x, y, BRIDGE_EDGE_RIGHT))
                        st.passed++;
                    else
                        st.failed++;
                }
                if (y < gh - 1) {
                    st.total_bridges++;
                    if (bridge_neighbor_chord_ok(h, x, y, BRIDGE_EDGE_BOTTOM))
                        st.passed++;
                    else
                        st.failed++;
                }
                /* Ancestral chord */
                if (h < FRACTAL_MAX_H) {
                    if (bridge_ancestral_chord_ok(h, x, y))
                        st.ancestral_ok++;
                    else
                        st.ancestral_fail++;
                }
            }
        }
    }
    return st;
}

static inline void bridge_stats_print(const BridgeStats *st)
{
    if (!st) return;
    printf("═══════════════════════════════════════════════\n");
    printf("  Level Bridge (Wang) Stats\n");
    printf("═══════════════════════════════════════════════\n");
    printf("  Neighbor bridges:    %u passed / %u failed / %u total\n",
           st->passed, st->failed, st->total_bridges);
    printf("  Ancestral bridges:   %u passed / %u failed\n",
           st->ancestral_ok, st->ancestral_fail);
    printf("═══════════════════════════════════════════════\n");
}

#endif /* GEO_LEVEL_BRIDGE_H */
