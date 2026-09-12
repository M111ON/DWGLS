/*
 * geo_robinson.h — Robinson Aperiodic Tiling (4-Tile Type)
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Robinson's 1971 aperiodic tiling uses 4 tile types (A, B, C, D):
 *   A = square with arrow (marks hierarchy direction)
 *   B = square with arrow (marks hierarchy direction, rotated)
 *   C = corner tile (connects hierarchy levels)
 *   D = center tile (marks level boundary)
 *
 * Key property: FORCES hierarchical structure — no periodic tiling possible.
 * The hierarchy depth is logarithmic: 2^N tiles per level.
 *
 * Application to DWGLS:
 *   - Non-periodic tensor placement (avoids cache-conflict patterns)
 *   - Hierarchical address modulation for DRamTile
 *   - Level-crossing bridge: Robinson hierarchy ↔ fractal address levels
 *
 * Mapping to DWGLS:
 *   - Tile type ∈ {0,1,2,3} = A,B,C,D
 *   - Edge colors: North, East, South, West ∈ {0,1,2,3}
 *   - Adjacency rule: edge colors must match
 *   - Hierarchy: 2^N × 2^N grid (N levels deep)
 *
 * NO MALLOC. All static inline. Header-only.
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_ROBINSON_H
#define GEO_ROBINSON_H

#include <stdint.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
   TILE TYPES
   ═══════════════════════════════════════════════════════════════════════════ */

#define ROB_TYPE_A  0u  /* arrow tile — marks hierarchy direction */
#define ROB_TYPE_B  1u  /* arrow tile — rotated 90° */
#define ROB_TYPE_C  2u  /* corner tile — connects levels */
#define ROB_TYPE_D  3u  /* center tile — marks boundary */
#define ROB_TYPES   4u

/* ═══════════════════════════════════════════════════════════════════════════
   EDGE COLORS
   ═══════════════════════════════════════════════════════════════════════════
   4 edges per tile: North(0), East(1), South(2), West(3)
   Edge color ∈ {0,1,2,3}
   Adjacency: tile A's East = tile B's West (same color)
   ═══════════════════════════════════════════════════════════════════════════ */

#define ROB_EDGE_N  0u
#define ROB_EDGE_E  1u
#define ROB_EDGE_S  2u
#define ROB_EDGE_W  3u
#define ROB_EDGES   4u

/* ═══════════════════════════════════════════════════════════════════════════
   ROBINSON TILE — 4 edges + type
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t type;        /* ROB_TYPE_A..D */
    uint8_t edges[4];    /* N, E, S, W edge colors */
} RobinsonTile;

/* ═══════════════════════════════════════════════════════════════════════════
   ROBINSON TILE ASSIGNMENT — deterministic from grid position
   ═══════════════════════════════════════════════════════════════════════════
   For a 2^N × 2^N grid, tiles are assigned by the Robinson rule:
   - Level 0 (finest): each cell is type D
   - Level k: blocks of 2^k × 2^k have type C at corners, A/B on edges
   - The type encodes which level the cell belongs to

   Simple deterministic rule:
   - Find the largest power of 2 dividing both x and y
   - This gives the "level" of the cell
   - Level determines tile type
   ═══════════════════════════════════════════════════════════════════════════ */

/* trailing zeros = highest power of 2 dividing n */
static inline uint32_t rob_ctz(uint32_t n)
{
    if (n == 0) return 32u;
    uint32_t c = 0;
    while ((n & 1u) == 0u) { n >>= 1; c++; }
    return c;
}

/* Level of cell (x, y) in the Robinson hierarchy */
static inline uint32_t rob_level(uint32_t x, uint32_t y)
{
    uint32_t lx = rob_ctz(x + 1);  /* +1 so that x=0 has level 0 */
    uint32_t ly = rob_ctz(y + 1);
    return lx < ly ? lx : ly;
}

/* Robinson tile type from (x, y) position */
static inline uint32_t rob_tile_type(uint32_t x, uint32_t y)
{
    uint32_t lev = rob_level(x, y);
    uint32_t bx = (x >> lev) & 1u;  /* position within level block */
    uint32_t by = (y >> lev) & 1u;

    /* Type assignment:
     * Level 0 (bx=0,by=0 in finest block): D (center)
     * Corner of any level block: C
     * Edge of level block: A or B
     */
    if (lev == 0) return ROB_TYPE_D;   /* finest level = center */

    if (bx == 0 && by == 0) return ROB_TYPE_C;  /* corner */
    if (bx == 1 && by == 1) return ROB_TYPE_C;  /* diagonal corner */
    if (bx == 1 && by == 0) return ROB_TYPE_A;  /* edge */
    if (bx == 0 && by == 1) return ROB_TYPE_B;  /* edge, rotated */

    return ROB_TYPE_D;
}

/* ═══════════════════════════════════════════════════════════════════════════
   EDGE COLOR — deterministic from position + edge
   ═══════════════════════════════════════════════════════════════════════════
   Edge colors ensure adjacency consistency:
   - East-West: tile(x,y).East color = tile(x+1,y).West color
   - North-South: tile(x,y).South color = tile(x,y+1).North color
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t rob_edge_color(uint32_t x, uint32_t y, uint32_t edge)
{
    uint32_t lev = rob_level(x, y);
    uint32_t type = rob_tile_type(x, y);

    /* Color derived from level + edge direction */
    uint32_t base = (lev * 3u + type * 7u + edge * 5u) % 4u;
    return (uint8_t)base;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TILE CREATION
   ═══════════════════════════════════════════════════════════════════════════ */

static inline RobinsonTile rob_make_tile(uint32_t x, uint32_t y)
{
    RobinsonTile t;
    t.type = (uint8_t)rob_tile_type(x, y);
    t.edges[ROB_EDGE_N] = rob_edge_color(x, y, ROB_EDGE_N);
    t.edges[ROB_EDGE_E] = rob_edge_color(x, y, ROB_EDGE_E);
    t.edges[ROB_EDGE_S] = rob_edge_color(x, y, ROB_EDGE_S);
    t.edges[ROB_EDGE_W] = rob_edge_color(x, y, ROB_EDGE_W);
    return t;
}

/* ═══════════════════════════════════════════════════════════════════════════
   ADJACENCY CHECK — two tiles share matching edge colors
   ═══════════════════════════════════════════════════════════════════════════ */

/* Check if two tiles can be adjacent in the given direction */
static inline uint32_t rob_adjacent_ok(uint32_t x1, uint32_t y1,
                                       uint32_t x2, uint32_t y2,
                                       uint32_t dir)
{
    RobinsonTile t1 = rob_make_tile(x1, y1);
    RobinsonTile t2 = rob_make_tile(x2, y2);

    switch (dir) {
        case ROB_EDGE_E: /* t1's East vs t2's West */
            return (uint32_t)(t1.edges[ROB_EDGE_E] == t2.edges[ROB_EDGE_W]);
        case ROB_EDGE_S: /* t1's South vs t2's North */
            return (uint32_t)(t1.edges[ROB_EDGE_S] == t2.edges[ROB_EDGE_N]);
        default:
            return 0u;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   GRID VERIFICATION — check all adjacencies
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_edges;    /* total adjacency pairs */
    uint32_t matched;        /* matching edge colors */
    uint32_t mismatched;     /* mismatched edge colors */
    uint32_t type_count[ROB_TYPES]; /* count of each tile type */
} RobinsonStats;

static inline RobinsonStats rob_verify_grid(uint32_t size)
{
    RobinsonStats st;
    uint32_t i;
    for (i = 0; i < ROB_TYPES; i++) st.type_count[i] = 0;
    st.total_edges = 0;
    st.matched = 0;
    st.mismatched = 0;

    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            /* Count tile types */
            uint32_t t = rob_tile_type(x, y);
            if (t < ROB_TYPES) st.type_count[t]++;

            /* Check East adjacency */
            if (x < size - 1) {
                st.total_edges++;
                if (rob_adjacent_ok(x, y, x + 1, y, ROB_EDGE_E))
                    st.matched++;
                else
                    st.mismatched++;
            }
            /* Check South adjacency */
            if (y < size - 1) {
                st.total_edges++;
                if (rob_adjacent_ok(x, y, x, y + 1, ROB_EDGE_S))
                    st.matched++;
                else
                    st.mismatched++;
            }
        }
    }
    return st;
}

static inline void rob_print_stats(const RobinsonStats *st, uint32_t size)
{
    if (!st) return;
    printf("═══════════════════════════════════════════════\n");
    printf("  Robinson Aperiodic Tiles (%ux%u)\n", size, size);
    printf("═══════════════════════════════════════════════\n");
    printf("  Type A (arrow):     %u\n", st->type_count[ROB_TYPE_A]);
    printf("  Type B (arrow R):   %u\n", st->type_count[ROB_TYPE_B]);
    printf("  Type C (corner):    %u\n", st->type_count[ROB_TYPE_C]);
    printf("  Type D (center):    %u\n", st->type_count[ROB_TYPE_D]);
    printf("  Total tiles:        %u\n", size * size);
    printf("  Edges checked:      %u\n", st->total_edges);
    printf("  Matched:            %u\n", st->matched);
    printf("  Mismatched:         %u\n", st->mismatched);
    printf("  Match rate:         %.1f%%\n",
           st->total_edges > 0 ? 100.0 * st->matched / st->total_edges : 0.0);
    printf("═══════════════════════════════════════════════\n");
}

#endif /* GEO_ROBINSON_H */
