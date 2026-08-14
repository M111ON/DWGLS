/*
 * wedge_ring_address.h
 * =====================
 * Hierarchical Wedge Addressing System (C port of wedge_ring_address.py)
 *
 * Address = (ring_index, wedge_index, mirror_flag, u, v) -> integer key, integer (x,y)
 * O(1) access. No float leaks past coordinate resolution.
 */

#ifndef WEDGE_RING_ADDRESS_H
#define WEDGE_RING_ADDRESS_H

#include <stdint.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <assert.h>

/* ===================== CONFIG ===================== */
#define N_WEDGES               24          /* 360 / N_WEDGES must be integer */
#define WEDGE_ANGLE_DEG        (360 / N_WEDGES)
#define N_RINGS                6
#define RING_STEP              1000        /* integer radial units per ring */
#define SCALE                  1000
#define POINTS_PER_WEDGE_EDGE  4
/* ==================================================== */

typedef struct {
    int32_t ring_index;
    int32_t wedge_index;
    int32_t mirror_flag;   /* 0 = A, 1 = B */
    int32_t u;             /* radial step within wedge */
    int32_t v;             /* angular step within wedge */
} Address;

typedef struct {
    int64_t x;
    int64_t y;
} Coord;

/* Flatten Address to a single integer key (compact storage). */
static inline int64_t address_key(const Address *a) {
    const int64_t block_wedge  = 2 * POINTS_PER_WEDGE_EDGE * POINTS_PER_WEDGE_EDGE;
    const int64_t block_ring   = (int64_t)N_WEDGES * block_wedge;
    const int64_t block_mirror = (int64_t)POINTS_PER_WEDGE_EDGE * POINTS_PER_WEDGE_EDGE;

    return a->ring_index  * block_ring
         + a->wedge_index * block_wedge
         + a->mirror_flag * block_mirror
         + a->u * POINTS_PER_WEDGE_EDGE
         + a->v;
}

/*
 * Base wedge (A) local coordinate equation.
 * Only place actual geometry math happens (radial interpolation + angle step).
 * Outputs: *radius, *local_angle_deg (both integer)
 */
static inline void wedge_A_local_point(int32_t u, int32_t v, int32_t ring_index,
                                        int64_t *radius, int32_t *local_angle_deg) {
    int64_t inner_radius = (int64_t)ring_index * RING_STEP;
    int64_t outer_radius = (int64_t)(ring_index + 1) * RING_STEP;

    *radius = inner_radius + (outer_radius - inner_radius) * u / POINTS_PER_WEDGE_EDGE;
    *local_angle_deg = WEDGE_ANGLE_DEG * v / POINTS_PER_WEDGE_EDGE;
}

/* B = mirror of A across the wedge's central axis. */
static inline void mirror_point(int64_t radius_in, int32_t angle_in,
                                 int64_t *radius_out, int32_t *angle_out) {
    *radius_out = radius_in;
    *angle_out  = WEDGE_ANGLE_DEG - angle_in;
}

/*
 * Convert (radius, local_angle_within_wedge, wedge_index) to global integer (x, y).
 * Only place trig happens -- once per lookup, not per intersection test.
 */
static inline Coord to_global_integer_coord(int64_t radius, int32_t local_angle_deg,
                                             int32_t wedge_index) {
    double global_angle_deg = (double)(wedge_index * WEDGE_ANGLE_DEG + local_angle_deg);
    double rad = global_angle_deg * M_PI / 180.0;

    Coord c;
    c.x = (int64_t)llround((double)radius * cos(rad));
    c.y = (int64_t)llround((double)radius * sin(rad));
    return c;
}

/*
 * Build an Address and resolve it to a global integer coordinate.
 * Main entry point -- replaces "find intersection" logic entirely. O(1).
 */
static inline void build_address(int32_t ring_index, int32_t wedge_index,
                                  int32_t mirror_flag, int32_t u, int32_t v,
                                  Address *addr_out, Coord *coord_out) {
    assert(ring_index >= 0 && ring_index < N_RINGS);
    assert(wedge_index >= 0 && wedge_index < N_WEDGES);
    assert(mirror_flag == 0 || mirror_flag == 1);
    assert(u >= 0 && u < POINTS_PER_WEDGE_EDGE);
    assert(v >= 0 && v < POINTS_PER_WEDGE_EDGE);

    int64_t radius;
    int32_t angle;
    wedge_A_local_point(u, v, ring_index, &radius, &angle);

    if (mirror_flag == 1) {
        int64_t r2; int32_t a2;
        mirror_point(radius, angle, &r2, &a2);
        radius = r2;
        angle  = a2;
    }

    addr_out->ring_index  = ring_index;
    addr_out->wedge_index = wedge_index;
    addr_out->mirror_flag = mirror_flag;
    addr_out->u = u;
    addr_out->v = v;

    *coord_out = to_global_integer_coord(radius, angle, wedge_index);
}

/* Total number of addressable points in the whole structure. */
static inline int64_t total_address_space(void) {
    return (int64_t)N_RINGS * N_WEDGES * 2 * POINTS_PER_WEDGE_EDGE * POINTS_PER_WEDGE_EDGE;
}

#endif /* WEDGE_RING_ADDRESS_H */
