/*
 * geo_box_axes.h -- 6-axis box path addressing
 *
 * 20736 is one immutable local box. The larger field is not modeled as a
 * cubic volume. It is six deterministic paths overlaid on the same box:
 *   xyz = square/cube tessellation axes
 *   ijk = triangle/tetra tessellation axes
 *
 * A field address is: which axis path, which position on that path, and which
 * local slot inside the 20736-cell box. This keeps old 20736 codecs unchanged
 * and adds outer identity without pretending data repeats as x*y*z volume.
 */

#ifndef GEO_BOX_AXES_H
#define GEO_BOX_AXES_H

#include <stdint.h>
#include "geo_octant.h"

#define GBA_BOX_SIZE       OCT_FULL     /* 20736 slots per box */
#define GBA_AXIS_COUNT     6u
#define GBA_SQUARE_AXES    3u           /* x, y, z */
#define GBA_TRI_AXES       3u           /* i, j, k */

typedef enum {
    GBA_AXIS_X = 0,
    GBA_AXIS_Y = 1,
    GBA_AXIS_Z = 2,
    GBA_AXIS_I = 3,
    GBA_AXIS_J = 4,
    GBA_AXIS_K = 5,
} GBA_Axis;

typedef struct {
    uint32_t axis;       /* one of GBA_AXIS_* */
    uint64_t position;   /* fixed identity on that path, floor = 0 */
    uint32_t local;      /* slot inside one 20736-cell box */
} GBA_Address;

static inline int gba_axis_valid(uint32_t axis) {
    return axis < GBA_AXIS_COUNT;
}

static inline int gba_is_square_axis(uint32_t axis) {
    return axis < GBA_SQUARE_AXES;
}

static inline int gba_is_triangle_axis(uint32_t axis) {
    return axis >= GBA_SQUARE_AXES && axis < GBA_AXIS_COUNT;
}

static inline GBA_Address gba_make(uint32_t axis, uint64_t position, uint32_t local) {
    GBA_Address a;
    a.axis = axis % GBA_AXIS_COUNT;
    a.position = position;
    a.local = local % GBA_BOX_SIZE;
    return a;
}

/*
 * Lane-tagged flattening for storage/indexing:
 *   lane = position*6 + axis
 *   flat = lane*20736 + local
 *
 * This is bijective for (axis, position, local). It is not x*y*z volume.
 */
static inline uint64_t gba_to_flat(GBA_Address a) {
    return ((a.position * GBA_AXIS_COUNT) + (uint64_t)(a.axis % GBA_AXIS_COUNT))
         * GBA_BOX_SIZE + (uint64_t)(a.local % GBA_BOX_SIZE);
}

static inline GBA_Address gba_from_flat(uint64_t flat) {
    uint64_t lane = flat / GBA_BOX_SIZE;
    GBA_Address a;
    a.local = (uint32_t)(flat % GBA_BOX_SIZE);
    a.axis = (uint32_t)(lane % GBA_AXIS_COUNT);
    a.position = lane / GBA_AXIS_COUNT;
    return a;
}

static inline GBA_Address gba_seek(GBA_Address a, int64_t delta) {
    if (delta < 0 && (uint64_t)(-delta) > a.position) {
        a.position = 0;
    } else {
        a.position = (uint64_t)((int64_t)a.position + delta);
    }
    return a;
}

static inline uint64_t gba_distance(GBA_Address a, GBA_Address b) {
    if (a.axis != b.axis) return UINT64_MAX;
    return (a.position > b.position) ? (a.position - b.position) : (b.position - a.position);
}

static inline int gba_equal(GBA_Address a, GBA_Address b) {
    return a.axis == b.axis && a.position == b.position && a.local == b.local;
}

static inline void gba_local_decompose(GBA_Address a,
                                       uint32_t *tess, uint32_t *cube,
                                       uint32_t *sq12, uint32_t *tri12,
                                       uint32_t *sq0, uint32_t *tri0) {
    oct_full_decompose(a.local % GBA_BOX_SIZE, tess, cube, sq12, tri12, sq0, tri0);
}

static inline int geo_box_axes_verify(void) {
    for (uint32_t axis = 0; axis < GBA_AXIS_COUNT; axis++) {
        for (uint64_t pos = 0; pos < 32; pos++) {
            for (uint32_t local = 0; local < GBA_BOX_SIZE; local += 137) {
                GBA_Address a = gba_make(axis, pos, local);
                GBA_Address rt = gba_from_flat(gba_to_flat(a));
                if (!gba_equal(a, rt)) return 0;

                GBA_Address f = gba_seek(a, -1000000);
                if (f.position != 0) return 0;
                if (f.axis != a.axis || f.local != a.local) return 0;
            }
        }
    }
    return 1;
}

#endif /* GEO_BOX_AXES_H */
