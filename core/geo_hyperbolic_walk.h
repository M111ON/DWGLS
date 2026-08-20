/*
 * geo_hyperbolic_walk.h — Hyperbolic route generator (deterministic centroid walk)
 *
 * HYPERBOLIC REDESIGN (2026-08-21): replaces legacy float Cayley
 * (hyperbolic_seek.h). New concept (user directive):
 *
 *   "hyperbolic สร้าง route ที่ deterministic → เก็บ key frame +
 *    f(step) compute reconstruct กลับมาใหม่ได้"
 *
 * WHAT WALKS: the triangle tessellation field (icosa unwrapped).
 *   node ∈ [0,20736), 12 pentagons × 1728 nodes (GEO_PENTAGONS).
 *   The field IS an icosahedron unfolded onto the plane — verified:
 *   dodeca(20V,30E,12F) is the dual, its vertices = centroids of the
 *   icosa's 20 triangular faces (test_dual_dodeca_probe T1-T6).
 *
 * WALK MECHANIC (probe-verified, test_centroid_30deg_probe + probe):
 *   centroid ของ triangle = จุดตัดของมุม 30° สองเส้น (bisect 60°×2)
 *   = th_cell_anchor(node, aperture, depth) — canonical cell address.
 *   Walking crosses a shared edge: node' = node + stride[axis] (mod 20736).
 *   - reversible:   +s แล้ว −s กลับที่เดิม (all 3 axes, 20736 nodes)
 *   - parity flip:  stride คี่ทุกตัว (1, 9, 81) → 100% up/down flip ต่อก้าว
 *   - coverage:     axis 0 (stride 1) orbit = 20736 → 1 key frame ครอบทั้งสนาม
 *   - snap:         จาก centroid ข้าม 3 edge → 3 centroids ต่างกัน (3-in-1-out)
 *
 * ROUTE = (seed, axis, step). f(step) = (seed + stride[axis]·step) mod 20736.
 *   - key frame = seed (few bytes) — reconstruct ใดๆ จาก seed
 *   - enter anywhere: O(1) — ให้ seed ใดก็ได้
 *   - direction = +/− stride (axis 0/1/2)
 *   - scale dimension = depth (aperture 4 → 2^depth cells) — int-only,
 *     base-2, no float, no trig (rescope 2026-08-14).
 *
 * All integer. No hash, no lookup for addressing.
 */

#ifndef GEO_HYPERBOLIC_WALK_H
#define GEO_HYPERBOLIC_WALK_H

#include <stdint.h>
#include "tri_hex_tess.h"

/* ── Strides: the 3 edge directions of the triangle field ────────── */
/* 1 = lo+1      (3-ladder, finest)   — orbit 20736 (full field)
 * 9 = lo+9      (3-ladder, level 2)  — orbit 2304
 * 81 = hi+1     (4-ladder, coarse)   — orbit 256
 * all odd → parity flips every step (sawtooth / ฟันปลา) */
#define HW_AXES            3u
#define HW_STRIDE_AXIS0    1u
#define HW_STRIDE_AXIS1    9u
#define HW_STRIDE_AXIS2    81u

/* ── Route state ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t seed;        /* key frame / home address (0..20735) */
    uint32_t axis;        /* 0/1/2 — which stride to walk */
    uint32_t step;        /* f(step) — how many crossings from seed */
} HWRouter;

static inline uint32_t hw_stride(uint32_t axis) {
    switch (axis) {
    case 0u:  return HW_STRIDE_AXIS0;
    case 1u:  return HW_STRIDE_AXIS1;
    default:  return HW_STRIDE_AXIS2;
    }
}

static inline void hw_init(HWRouter *r, uint32_t seed, uint32_t axis) {
    r->seed = seed;
    r->axis = axis % HW_AXES;
    r->step = 0u;
}

/* f(step): position after `step` crossings along the axis (unrolled) */
static inline uint32_t hw_at(const HWRouter *r, uint32_t step) {
    return (uint32_t)((r->seed + hw_stride(r->axis) * step) % GEO_FULL);
}

/* current position (step 0 = key frame itself) */
static inline uint32_t hw_pos(const HWRouter *r) { return hw_at(r, r->step); }

/* advance step counter (no position math until hw_pos/hw_at) */
static inline void hw_step(HWRouter *r, int32_t delta) {
    /* step is unbounded; hw_at reduces mod GEO_FULL — round counter
     * semantics: every 20736/stride steps completes one round (fibo) */
    r->step = (uint32_t)((int64_t)r->step + delta);
}

/* walk one edge forward / backward (snap to next centroid position) */
static inline uint32_t hw_forward(HWRouter *r) { hw_step(r, 1); return hw_pos(r); }
static inline uint32_t hw_backward(HWRouter *r) { hw_step(r, -1); return hw_pos(r); }

/* number of steps to complete one full round on this axis (orbit size) */
static inline uint32_t hw_round_len(uint32_t axis) {
    switch (axis) {
    case 0u:  return 20736u;  /* stride 1  — full field */
    case 1u:  return 2304u;   /* stride 9  — 9 rounds */
    default:  return 256u;    /* stride 81 — 81 rounds */
    }
}

/* ── Centroid (key-frame) layer ──────────────────────────────────── */
/* centroid of the cell containing `node` at `depth` — the snap point.
 * aperture 4: 2^depth cells (scale = base-2), anchor zeroes low hi-bits.
 * aperture 3: 3^depth cells (Peano trits), anchor zeroes low lo-trits. */
static inline uint32_t hw_cell_centroid(uint32_t node, uint32_t aperture,
                                        uint32_t depth) {
    return th_cell_anchor(node, aperture, depth);
}

/* cell count / size at depth (independent of the walk) */
static inline uint32_t hw_cell_count(uint32_t aperture, uint32_t depth) {
    return th_cell_count(aperture, depth);
}

/* distance (in crossings) from a node back to its cell centroid on an
 * axis — deterministic, integer, no geometry. Returns the delta so that
 * node = hw_at(&(HWRouter){.seed=centroid}, delta). */
static inline int32_t hw_back_to_centroid(uint32_t node, uint32_t axis,
                                          uint32_t aperture, uint32_t depth) {
    uint32_t centroid = th_cell_anchor(node, aperture, depth);
    uint32_t s = hw_stride(axis);
    /* (node − centroid) must be a multiple of s; delta = diff / s */
    uint32_t diff = (node >= centroid) ? (node - centroid)
                                       : (GEO_FULL - centroid + node);
    /* diff = (s · delta) mod GEO_FULL — if not divisible by s, snap
     * to nearest: walk in s-steps from centroid until we pass node. */
    uint32_t delta = diff / s;
    if (diff % s != 0u) {
        /* walk forward until the centroid-anchored lattice passes node */
        uint32_t cur = (centroid + s * delta) % GEO_FULL;
        if (cur != node) {
            /* add one more step if overshoot-free */
            cur = (cur + s) % GEO_FULL;
            delta++;
            (void)cur;
        }
    }
    return (int32_t)delta;
}

/* ── Parity (sawtooth) ───────────────────────────────────────────── */
/* every crossing flips triangle orientation (up/down) — stride is odd */
static inline uint32_t hw_parity(uint32_t node) { return node & 1u; }

#endif /* GEO_HYPERBOLIC_WALK_H */