/* ═══════════════════════════════════════════════════════════════════════════
 * geo_kis_projection.h — 4D Tesseract → 3D KIS{x,y,z} Projection Engine
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Core insight: 4D object can only be seen 3 properties + 1 frame at a time.
 * Like a 3D cube's shadow is 2D, a 4D tesseract's projection is 3D.
 * KIS Timeline = sequence of 3D projections as we move through scale (time).
 *
 * KIS{x,y,z} = 3 axes from same 1D data at different circular offsets.
 * All axes lock together (uniform constraint) → error correction by nature.
 *
 * Architecture:
 *   4D tesseract (18 tesseracts in 6ico compound)
 *       │ projection at scale t
 *       ▼
 *   3D KIS{x,y,z} ←── 1D weight array (source)
 *       │ we = scale (time)
 *       ▼
 *   2D storage (what computer sees)
 *
 * Sacred constants: 20736, 1728, 144, 12, 128, 162
 * ═══════════════════════════════════════════════════════════════════════════ */
#pragma once

#include <stdint.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */
#define KIS_20736      20736u
#define KIS_1728       1728u    /* 12³ — FiboSpine pipes */
#define KIS_3456       3456u    /* 2 × 1728 */
#define KIS_144        144u     /* 12² — protagonist */
#define KIS_12         12u

/* ── 4D→3D Projection ──────────────────────────────────────────────────── *
 * Standard perspective projection from 4D to 3D.
 * Project along w-axis: divide x,y,z by (w + offset) at given scale.
 * Scale controls the "distance" from which we view the 4D object.
 * No float — use fixed-point arithmetic (<<16 for precision).
 * ───────────────────────────────────────────────────────────────────────── */
static inline uint32_t kis_project_4d_to_3d(uint32_t x4, uint32_t y4,
                                             uint32_t z4, uint32_t w4,
                                             uint32_t scale)
{
    /* Perspective divide: project 4D → 3D at given scale
     * Formula: x3 = (x4 << 16) / (w4 + scale + 1)
     * The +1 prevents division by zero.
     * Result is fixed-point (16.16), masked to 20736 range. */
    uint32_t denom = w4 + scale + 1u;
    if (denom == 0) denom = 1u;

    uint32_t x3 = ((x4 << 16) / denom) % KIS_20736;
    uint32_t y3 = ((y4 << 16) / denom) % KIS_20736;
    uint32_t z3 = ((z4 << 16) / denom) % KIS_20736;

    /* Pack into single 32-bit key: x3[12] | y3[12] | z3[8] */
    return ((x3 & 0x0FFFu) << 20) |
           ((y3 & 0x0FFFu) << 8)  |
           (z3 & 0xFFu);
}

/* ── KIS Axis: 1D → 3D ────────────────────────────────────────────────── *
 * Form KIS{x,y,z} axes from a single 1D weight array.
 * All 3 axes use SAME data at different circular offsets.
 * This creates uniform constraint: any axis gives the same scaling.
 *
 * x-axis = data[i]
 * y-axis = data[(i + 1728) % n]   ← stride = FiboSpine pipes
 * z-axis = data[(i + 3456) % n]   ← stride = 2 × pipes
 *
 * The circular offsets ensure all 3 axes are independent views
 * of the SAME underlying data — the constraint that locks them together.
 * ───────────────────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  x[KIS_20736];     /* KIS_x axis */
    uint8_t  y[KIS_20736];     /* KIS_y axis */
    uint8_t  z[KIS_20736];     /* KIS_z axis */
    uint32_t n;                 /* number of elements per axis */
    uint8_t  initialized;       /* 1 = ready */
} KISAxes;

static inline void kis_axis_from_1d(KISAxes *a, const uint8_t *data, uint32_t n)
{
    if (!a || !data || n == 0) return;
    if (n > KIS_20736) n = KIS_20736;

    a->n = n;
    for (uint32_t i = 0; i < n; i++) {
        a->x[i] = data[i];
        a->y[i] = data[(i + KIS_1728) % n];
        a->z[i] = data[(i + KIS_3456) % n];
    }
    a->initialized = 1;
}

/* ── Axis Verify: check consistency ───────────────────────────────────── *
 * Verify that axes are consistent with their source data.
 * The 3 axes are views of same data at different offsets:
 *   x[i] = data[i]
 *   y[i] = data[(i+1728)%n]
 *   z[i] = data[(i+3456)%n]
 *
 * To verify, we check a round-trip: from x-axis, we can reconstruct
 * what y and z should be. If they match → consistent.
 *
 * More practically: verify that the circular offset pattern holds.
 * If any axis was corrupted (modified independently), the pattern breaks.
 * ───────────────────────────────────────────────────────────────────────── */
static inline int kis_axis_verify(const KISAxes *a, uint32_t coord)
{
    if (!a || !a->initialized || coord >= a->n) return 0;

    /* Check that the 3 axes have consistent relationship:
     * x[coord] and y[coord] and z[coord] should be values from
     * the SAME source data at different positions.
     *
     * Simple check: verify that rotating axes gives consistent results.
     * If y-axis is x-axis rotated by 1728, then:
     *   y[coord] should equal x[(coord + 1728) % n]
     *   z[coord] should equal x[(coord + 3456) % n]
     */
    uint32_t n = a->n;
    uint32_t expected_y = (uint32_t)a->x[(coord + KIS_1728) % n];
    uint32_t expected_z = (uint32_t)a->x[(coord + KIS_3456) % n];

    return ((uint32_t)a->y[coord] == expected_y) &&
           ((uint32_t)a->z[coord] == expected_z);
}

/* ── Axis Lock: freeze all 3 axes ─────────────────────────────────────── *
 * Return the value at coord from the x-axis.
 * Verify that y and z axes are consistent with x at this coord.
 * If inconsistent → error (return 0, data is corrupted).
 *
 * This is the "barrier" for KIS axes: at the lock point,
 * all 3 dimensions must agree before proceeding.
 * ───────────────────────────────────────────────────────────────────────── */
static inline uint32_t kis_axis_lock(const KISAxes *a, uint32_t coord)
{
    if (!a || !a->initialized || coord >= a->n) return 0;
    if (!kis_axis_verify(a, coord)) return 0;  /* inconsistent = error */
    return (uint32_t)a->x[coord];
}

/* ── Axis Correct: 2-of-3 majority vote ───────────────────────────────── *
 * If 2 axes agree and 1 disagrees → return the majority value.
 * If all 3 agree → return that value.
 * If all 3 differ → return 0 (uncorrectable).
 *
 * This is the error correction that 3D constraints give us
 * from 1D data: majority vote across axes.
 *
 * NOTE: "agree" means the axes have the same value at this coord.
 * Since axes are different views of same data, they normally differ.
 * But if we're checking for corruption (someone modified one axis),
 * majority vote can recover the original.
 * ───────────────────────────────────────────────────────────────────────── */
static inline uint32_t kis_axis_correct(const KISAxes *a, uint32_t coord)
{
    if (!a || !a->initialized || coord >= a->n) return 0;

    uint8_t vx = a->x[coord];
    uint8_t vy = a->y[coord];
    uint8_t vz = a->z[coord];

    /* All agree */
    if (vx == vy && vy == vz) return (uint32_t)vx;

    /* 2-of-3 majority vote */
    if (vx == vy) return (uint32_t)vx;  /* x+y agree */
    if (vx == vz) return (uint32_t)vx;  /* x+z agree */
    if (vy == vz) return (uint32_t)vy;  /* y+z agree */

    return 0;  /* all 3 differ → uncorrectable */
}

/* ── Projection Stats ──────────────────────────────────────────────────── */
typedef struct {
    uint32_t total_projections;
    uint32_t total_verifies;
    uint32_t total_locks;
    uint32_t total_corrections;
    uint32_t errors_detected;
} KISProjectionStats;

static inline void kis_projection_stats(KISProjectionStats *s)
{
    if (!s) return;
    static uint32_t _projections = 0;
    static uint32_t _verifies    = 0;
    static uint32_t _locks       = 0;
    static uint32_t _corrections = 0;
    static uint32_t _errors      = 0;

    _projections++;

    s->total_projections = _projections;
    s->total_verifies    = _verifies;
    s->total_locks       = _locks;
    s->total_corrections = _corrections;
    s->errors_detected   = _errors;
}