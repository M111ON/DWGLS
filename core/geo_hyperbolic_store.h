/*
 * geo_hyperbolic_store.h — Key-frame store for the hyperbolic route
 *
 * HYPERBOLIC REDESIGN (2026-08-21), part 2 — the STORE layer.
 * Concept (user directive):
 *
 *   "hyperbolic สร้าง route ที่ deterministic → เก็บ key frame +
 *    f(step) compute reconstruct กลับมาใหม่ได้"
 *
 * What this header adds over geo_hyperbolic_walk.h:
 *   - a KEY-FRAME GRID: centroids at a chosen depth (coarse grid).
 *   - reconstruction: from the nearest key frame, walk f(step) to
 *     any requested node — lossless because the walk is a bijection
 *     (stride-1 axis covers all 20736 in one round; probe-verified).
 *
 * Storage model (MAP not COMPRESS):
 *   - key frames = hw_cell_centroid(node, aperture, depth) — few bytes
 *     per cell (the cell anchor IS the address, no payload needed for
 *     position reconstruction).
 *   - payload lives AT the centroid address; interior nodes are
 *     derived by walking from the nearest centroid (f(step)).
 *
 * Deterministic, integer, no float, no hash, no lookup for addressing.
 */

#ifndef GEO_HYPERBOLIC_STORE_H
#define GEO_HYPERBOLIC_STORE_H

#include <stdint.h>
#include "tri_hex_tess.h"
#include "geo_hyperbolic_walk.h"

/* ── Key-frame grid ──────────────────────────────────────────────── */
/* A key-frame grid at (aperture, depth) partitions the field into
 * cell_count cells; each cell's centroid = th_cell_anchor. The walk
 * (stride 1, axis 0 — the full-coverage bijection, orbit 20736) from
 * a centroid reconstructs every node: one key frame covers the whole
 * field, and per-cell anchors give the locality layer.
 *
 * HWFrames.axis selects the ROUTE axis (which stride hwf_path uses for
 * generating paths). Reconstruction ALWAYS uses the full-coverage walk
 * (stride 1) because only it reaches every node — a stride-9/81 walk
 * covers only a subgroup of the field. */

typedef struct {
    uint32_t aperture;   /* 3 or 4 — which ladder to anchor on */
    uint32_t depth;      /* 0..4 — how many digits fixed */
    uint32_t axis;       /* which stride walks the interior (0=full) */
} HWFrames;

static inline void hwf_init(HWFrames *f, uint32_t aperture, uint32_t depth,
                            uint32_t axis) {
    f->aperture = aperture;
    f->depth = depth;
    f->axis = axis & (HW_AXES - 1u);
}

/* number of key frames (cells) in the grid */
static inline uint32_t hwf_count(const HWFrames *f) {
    return hw_cell_count(f->aperture, f->depth);
}

/* key-frame index k → its centroid address */
static inline uint32_t hwf_centroid(const HWFrames *f, uint32_t k) {
    /* cell k covers nodes [k·cell_size, (k+1)·cell_size) — anchor its start */
    uint32_t cell_size = th_cell_size(f->aperture, f->depth);
    uint32_t base = k * cell_size;
    return th_cell_anchor(base, f->aperture, f->depth);
}

/* nearest key-frame index to a node (cell containing it) */
static inline uint32_t hwf_index_of(const HWFrames *f, uint32_t node) {
    uint32_t cell_size = th_cell_size(f->aperture, f->depth);
    uint32_t idx = node / cell_size;
    if (idx >= hwf_count(f)) idx = hwf_count(f) - 1u;
    return idx;
}

/* reconstruct any node: walk f(step) from its cell's key frame.
 * Uses the full-coverage stride-1 walk (axis 0): step = node − centroid
 * (mod 20736) reaches every node exactly — lossless by the bijection
 * property (orbit 20736, test T1). */
static inline uint32_t hwf_reconstruct(const HWFrames *f, uint32_t node) {
    uint32_t idx = hwf_index_of(f, node);
    uint32_t centroid = hwf_centroid(f, idx);
    HWRouter r;
    hw_init(&r, centroid, 0u); /* axis 0 = stride 1 = full coverage */
    uint32_t diff = (node >= centroid) ? (node - centroid)
                                       : (GEO_FULL - centroid + node);
    return hw_at(&r, diff);
}

/* ── Payload slot at a centroid ──────────────────────────────────── */
/* Each key frame can carry a small payload at its address. Position
 * reconstruction needs no payload (geometry is the address); payload
 * is optional user data. Returns the payload slot address. */
static inline uint32_t hwf_slot(const HWFrames *f, uint32_t k) {
    return hwf_centroid(f, k);
}

/* walk from key frame k, step t → position (reconstruct path) */
static inline uint32_t hwf_path(const HWFrames *f, uint32_t k, uint32_t t) {
    HWRouter r;
    hw_init(&r, hwf_centroid(f, k), f->axis);
    return hw_at(&r, t);
}

/* ── Lossless guarantee helpers ──────────────────────────────────── */
/* how many distinct nodes the walk from one key frame covers before
 * repeating (orbit of the stride on this field) */
static inline uint32_t hwf_orbit(uint32_t axis) { return hw_round_len(axis); }

#endif /* GEO_HYPERBOLIC_STORE_H */