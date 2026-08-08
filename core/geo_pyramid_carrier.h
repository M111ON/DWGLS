/*
 * geo_pyramid_carrier.h — Pyramid Swing Carrier v1
 * ═══════════════════════════════════════════════════════════════════════════
 * User design: "square → pyramid → 4 vertex sealed → another square → loop"
 *
 * The KIS swing stripped to its minimal carrier: a SQUARE PYRAMID is
 * SELF-DUAL (V=5, F=5, E=8) — the spike/seal cycle happens inside ONE
 * shape, no dodeca↔icosa dual pair needed:
 *
 *   SEALED  (square, 4 base nodes)        node count = 4
 *      │ spike (apex up)
 *   SPIKED  (pyramid, +apex = 5 nodes)   node count = 5
 *      │ seal (apex down / flatten)
 *   SEALED  (square, 4 base nodes)       node count = 4  → loop
 *
 * Recurrence: stateA(n+1) = stateB(n) + constant
 *   spike = +1 node (4→5), seal = −1 node (5→4), net per pair = 9 = 4+5.
 *   Swinging upward forever = the "no zero, grows to infinity" timeline.
 *
 * Overlapped invert: two pyramids base-to-base = OCTAHEDRON (V=6, F=8, E=12)
 * → tet+oct honeycomb fills 3D with no gaps.
 *
 * Pure integer, no malloc, no trig. Layer n contributes 4 or 5 nodes
 * (even layer = sealed, odd layer = spiked).
 * ═══════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_PYRAMID_CARRIER_H
#define GEO_PYRAMID_CARRIER_H

#include <stdint.h>

/* ── Sacred counts (from square pyramid) ─────────────────────── */
#define PYR_BASE        4u   /* square base vertices   */
#define PYR_APEX        1u   /* apex vertex            */
#define PYR_MAX         5u   /* pyramid vertices (4+1) */
#define PYR_FACES       5u   /* 4 side + 1 base = self-dual (V=F) */
#define PYR_EDGES       8u   /* 4 base + 4 side edges  */
#define PYR_PAIR        9u   /* 4+5 — one full swing   */

/* ── Layer state ------------------------------------------------- */
enum {
    PYR_SEALED = 0,          /* square: 4 nodes visible */
    PYR_SPIKED = 1           /* pyramid: apex up, 5 nodes */
};

/* Kind of layer n (even = sealed, odd = spiked) */
static inline uint8_t pyr_layer_kind(uint32_t layer)
{
    return (uint8_t)(layer & 1u);
}

/* Slots contributed by layer n: 4 or 5 */
static inline uint32_t pyr_layer_slots(uint32_t layer)
{
    return (layer & 1u) ? PYR_MAX : PYR_BASE;
}

/* Total slots in layers [0, n_layers) — 9 per sealed+spiked pair */
static inline uint32_t pyr_total(uint32_t n_layers)
{
    uint32_t pairs = n_layers >> 1;
    uint32_t lone  = n_layers & 1u;
    return pairs * PYR_PAIR + (lone ? PYR_BASE : 0u);
}

/* Byte/node offset of layer n */
static inline uint32_t pyr_offset(uint32_t layer)
{
    return pyr_total(layer);
}

/* Address: (layer, node) → flat index in the pyramid field */
static inline uint32_t pyr_addr(uint32_t layer, uint32_t node)
{
    return pyr_offset(layer) + node;
}

/* Inverse: which layer owns a flat slot? (upper-bound search free:
   iterative climb for small n used by tests; production can bisect) */
static inline uint32_t pyr_layer_of(uint32_t flat, uint32_t *node_out)
{
    uint32_t layer = 0;
    while (flat >= pyr_layer_slots(layer)) {
        flat -= pyr_layer_slots(layer);
        layer++;
    }
    *node_out = flat;
    return layer;
}

/* ── Swing recurrence ────────────────────────────────────────────
 * stateA(n+1) = stateB(n) + constant
 *   spike: SEALED(4) → SPIKED(5)   (+1 node)
 *   seal : SPIKED(5) → SEALED(4)   (−1 node)
 * Returns the state AFTER the operation (2-cycle). */
static inline uint8_t pyr_swing(uint8_t state)
{
    return state == PYR_SEALED ? PYR_SPIKED : PYR_SEALED;
}

/* ── Octahedron fill unit ────────────────────────────────────────
 * up pyramid ∪ down pyramid (base-to-base, apex opposite directions)
 * = octahedron: V=6 (2 apices + 4 shared base), F=8, E=12.
 * This is the no-gap fill cell of the pyramid 3D field. */
static inline uint32_t octa_verts(uint32_t n_pairs) { return n_pairs * 6u; }
static inline uint32_t octa_faces(uint32_t n_pairs) { return n_pairs * 8u; }
static inline uint32_t octa_edges(uint32_t n_pairs) { return n_pairs * 12u; }

/* Pair node count: up-pyramid (5) + down-pyramid (5) sharing 4 base
   nodes = 6 unique nodes (apex_up + 4 base + apex_down). */
static inline uint32_t octa_unique(uint32_t n_pairs) { return n_pairs * 6u; }

#endif /* PYR_KIS_PYRA_CARRIER_H */