/*
 * geo_lblock_bridge.h — geo_jump ↔ L-block Placement Adapter
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Bridges geo_jump coordinates (face, tick, local) → L-block placement.
 *
 * Chain:
 *   geo_jump (face,tick,local) → node (flat 0..20735) → Hilbert address d
 *   → L-block summon → piece (cells[4], rotation, direction)
 *
 * This means: given a geo_jump position, we know EXACTLY where to place
 * data and how to orient the L-shaped container. Deterministic, O(1), int-only.
 *
 * Grid size must be power of 2 (Hilbert requirement).
 * For 20736 slots: grid_n=128 → 16384 slots (wraps remaining 4352).
 *
 * DESIGN:
 *   No malloc. No float. All static inline. Header-only.
 *   C99. Depends: geo_sync_bridge.h (for geo_jump decomposition),
 *                 geo_lblock.h (for Hilbert summon)
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_LBLOCK_BRIDGE_H
#define GEO_LBLOCK_BRIDGE_H

#include <stdint.h>
#include "geo_sync_bridge.h"
#include "geo_lblock.h"

/* ═══════════════════════════════════════════════════════════════════════════════
   PLACEMENT RESULT
   ═══════════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t node;          /* flat address (0..20735) */
    uint32_t d;             /* Hilbert address on grid (0..grid_n²-1) */
    uint32_t grid_n;        /* grid dimension (power of 2) */
    uint32_t rotation;      /* L-block rotation 0..3 */
    int32_t  dx, dy;        /* curve direction entering d */
    int32_t  cells[4][2];   /* 4 L-shaped cells (grid coordinates) */
    uint32_t face;          /* geo_jump face (0..11) */
    uint32_t tick;          /* geo_jump tick (0..11) */
    uint32_t local;         /* geo_jump local (0..143) */
} LBlockPlacement;

/* ═══════════════════════════════════════════════════════════════════════════════
   CORE: geo_jump → L-block placement
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * glb_place — geo_jump coordinates → L-block placement.
 *
 * face, tick, local: geo_jump decomposition
 * grid_n: Hilbert grid size (must be power of 2)
 *
 * Returns LBlockPlacement with all fields filled.
 * Deterministic: same (face,tick,local) → same placement always.
 */
static inline LBlockPlacement glb_place(uint32_t face, uint32_t tick,
                                         uint32_t local, uint32_t grid_n) {
    LBlockPlacement p;
    p.face  = face;
    p.tick  = tick;
    p.local = local;
    p.grid_n = grid_n;

    /* geo_jump → flat node */
    p.node = gsb_node_of(face, tick, local);

    /* flat node → Hilbert address (mod grid) */
    p.d = p.node % (grid_n * grid_n);

    /* L-block summon */
    geo_lb_from_hilbert(p.d, grid_n, p.cells, &p.rotation, &p.dx, &p.dy);

    return p;
}

/*
 * glb_place_node — flat node → L-block placement (simpler API).
 *
 * node: flat address (0..20735)
 * grid_n: Hilbert grid size (must be power of 2)
 */
static inline LBlockPlacement glb_place_node(uint32_t node, uint32_t grid_n) {
    uint32_t face, tick, local;
    gsb_split(node, &face, &tick, &local);
    return glb_place(face, tick, local, grid_n);
}

/* ═══════════════════════════════════════════════════════════════════════════════
   REVERSE: placement → geo_jump coordinates
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * glb_decompose — LBlockPlacement → geo_jump coordinates.
 * Verifies node roundtrip.
 */
static inline void glb_decompose(const LBlockPlacement *p,
                                  uint32_t *face, uint32_t *tick, uint32_t *local) {
    *face  = gsb_face_of(p->node);
    *tick  = gsb_tick_of(p->node);
    *local = gsb_local_of(p->node);
}

/* ═══════════════════════════════════════════════════════════════════════════════
   BATCH: place a range of geo_jump positions
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * glb_place_range — place all 20736 nodes into L-blocks on given grid.
 *
 * out: caller-allocated LBlockPlacement[20736]
 * grid_n: Hilbert grid size (power of 2)
 *
 * Returns 0 on success.
 */
static inline int glb_place_range(LBlockPlacement *out, uint32_t grid_n) {
    for (uint32_t face = 0; face < GSB_FACES; face++) {
        for (uint32_t tick = 0; tick < GSB_TICKS; tick++) {
            for (uint32_t local = 0; local < GSB_TOWER; local++) {
                uint32_t idx = face * GSB_TICKS * GSB_TOWER
                             + tick * GSB_TOWER
                             + local;
                out[idx] = glb_place(face, tick, local, grid_n);
            }
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
   VERIFICATION
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * glb_verify — verify bridge consistency over all 20736 nodes.
 *
 * Checks:
 *   1. node = face·1728 + tick·144 + local (decompose roundtrip)
 *   2. d = node mod grid_n² (Hilbert address consistent)
 *   3. L-block fit guarantee (4 cells non-overlapping)
 *   4. Deterministic: same node → same placement
 *   5. glb_place == glb_place_node (both paths agree)
 *
 * grid_n: Hilbert grid size (power of 2)
 * Returns 0 on success, negative on failure.
 */
static inline int glb_verify(uint32_t grid_n) {
    uint32_t grid_sq = grid_n * grid_n;

    for (uint32_t face = 0; face < GSB_FACES; face++) {
        for (uint32_t tick = 0; tick < GSB_TICKS; tick++) {
            for (uint32_t local = 0; local < GSB_TOWER; local++) {
                uint32_t node = gsb_node_of(face, tick, local);

                /* 1. decompose roundtrip */
                uint32_t f2, t2, l2;
                gsb_split(node, &f2, &t2, &l2);
                if (f2 != face || t2 != tick || l2 != local) return -1;

                /* 2. Hilbert address */
                LBlockPlacement p = glb_place(face, tick, local, grid_n);
                if (p.node != node) return -2;
                if (p.d != node % grid_sq) return -3;

                /* 3. fit guarantee */
                if (!geo_lb_fits_grid(p.d, grid_n)) return -4;

                /* 4. deterministic */
                LBlockPlacement p2 = glb_place(face, tick, local, grid_n);
                if (p.rotation != p2.rotation) return -5;
                for (int i = 0; i < 4; i++) {
                    if (p.cells[i][0] != p2.cells[i][0]) return -6;
                    if (p.cells[i][1] != p2.cells[i][1]) return -6;
                }

                /* 5. both paths agree */
                LBlockPlacement p3 = glb_place_node(node, grid_n);
                if (p3.node != p.node) return -7;
                if (p3.d != p.d) return -8;
                if (p3.rotation != p.rotation) return -9;
            }
        }
    }
    return 0;
}

#endif /* GEO_LBLOCK_BRIDGE_H */
