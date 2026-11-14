/* geo_loshu.h — Lo Shu 3x3 projection: pyramid -> 9-vertex grid.
 *
 * Pyramid: apex(1) + base(4) + outer(8) + center(1) = 14 points.
 * Projection -> 2D 3x3 grid: 8 outer + 1 center = 9 vertices.
 * Projection is a view (idempotent on grid positions); data stays in field.
 *
 * Self-contained: only <stdint.h>.
 */
#ifndef GEO_LOSHU_H
#define GEO_LOSHU_H

#include <stdint.h>

#define LOSHU_GRID  9u   /* 3x3 */
#define LOSHU_CENTER 4u  /* middle of 3x3 */
/* pyramid point count */
#define PYR_POINTS 14u

/* projection table: pyramid index 0..13 -> grid index 0..8 */
/* 0=apex->center(4), 1..4=base->corners(0,2,6,8), 5..12=outer->edges/corners, 13=center->center(4) */
static const uint8_t loshu_proj[14] = {
    4,        /* 0: apex -> center */
    0, 2, 6, 8,  /* 1-4: base -> 4 corners */
    1, 3, 5, 7,  /* 5-8: outer -> 4 edges */
    0, 2, 6, 8,  /* 9-12: outer -> 4 corners (may collide) */
    4             /* 13: center -> center */
};

static inline uint8_t loshu_project(uint32_t pyr_idx) {
    if (pyr_idx >= PYR_POINTS) return LOSHU_CENTER;
    return loshu_proj[pyr_idx];
}

/* grid identity: grid position maps to itself */
static inline uint8_t loshu_grid_id(uint8_t g) { return g; }

#endif /* GEO_LOSHU_H */
