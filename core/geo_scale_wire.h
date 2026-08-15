/*
 * geo_scale_wire.h — Subdivision ↔ Scale-Timeline Wiring
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Wires th_subdivide (4-subdivision depth d) into the scale timeline
 * (144 scale positions w ∈ [0,144), affine view p = (a_w·l + b_w) % 144).
 *
 * BRIDGE — mixed radix, pure integer (rescope: geometry = rule):
 *   node = hi·81 + lo,  hi = 16·H + h ∈ [0,256),  lo = 9·L + l ∈ [0,81)
 *   scale position w = 9·H + L      ← OUTER digits (window-head axis)
 *   local position  pos = 9·h + l   ← INNER digits (intra-window axis)
 *   144 × 144 = 20736 — (w, pos) ↔ node is a bijection, no table, no float.
 *
 * DEPTH → SCALE (the wiring):
 *   the 4-ladder has 4 base-4 digits; the first TWO live in H (the scale
 *   axis), the last TWO in h (the local axis):
 *     d ≤ 2: cells are contiguous w-blocks 144/4^d wide (144, 36, 9)
 *            × the whole pos axis      — cell = w-block × 144
 *     d = 3: w frozen at 9 (saturated); pos splits 144 → 4 × 36
 *     d = 4: w frozen at 9;            pos splits 36 → 4 × 9
 *   every depth-d cell is EXACTLY one rectangle  w_ext × pos_ext,
 *   and 4^d cells × (20736/4^d) = 20736 — the ladder tiles the grid.
 *
 *   canonical depth scale: th_depth_scale(d) = 144 − w_ext(d)
 *     = {0, 108, 135, 135, 135} — the last w-block start of depth d.
 *
 * a_w ↔ th_cell_anchor (proved in test_tess_scale_wire.c):
 *   at ANY scale w, the a_w view permutes pos and the depth-d cells whose
 *   w-block contains w have pos-images that partition pos exactly — the
 *   view never conflates two cells' addresses at any level. Reading at a
 *   cell's home scale (empty log) roundtrips every node of the cell.
 */

#ifndef GEO_SCALE_WIRE_H
#define GEO_SCALE_WIRE_H

#include <stdint.h>

#define GSW_LOCAL    144u   /* scale positions w ∈ [0,144)      */
#define GSW_FULL     20736u /* 144 × 144 — the (w, pos) grid    */
#define GSW_4_DEPTH  4u     /* 4-subdivision levels (0..4)      */

/* ── Mixed-radix bridge: node ↔ (w, pos) ─────────────────────── */
static inline uint32_t gsw_scale_of_node(uint32_t n) {
    uint32_t hi = n / 81u, lo = n % 81u;        /* hi·81 + lo     */
    uint32_t H = hi >> 4u, L = lo / 9u;         /* outer digits   */
    return 9u * H + L;                          /* scale position */
}

static inline uint32_t gsw_pos_of_node(uint32_t n) {
    uint32_t hi = n / 81u, lo = n % 81u;
    uint32_t h = hi & 15u, l = lo % 9u;         /* inner digits   */
    return 9u * h + l;                          /* local position */
}

static inline uint32_t gsw_node_of_scale(uint32_t w, uint32_t pos) {
    uint32_t H = w / 9u, L = w % 9u;
    uint32_t h = pos / 9u, l = pos % 9u;
    return (16u * H + h) * 81u + 9u * L + l;    /* hi·81 + lo     */
}

/* ── Depth d ↔ scale axis (4-subdivision wiring) ─────────────── */
/* w-extent of a depth-d cell on the scale axis (blocks of 144/4^d) */
static inline uint32_t gsw_scale_ext(uint32_t d) {
    return (d <= 2u) ? (144u >> (2u * d)) : 9u; /* 144, 36, 9, 9, 9 */
}

/* pos-extent of a depth-d cell on the local axis */
static inline uint32_t gsw_pos_ext(uint32_t d) {
    return (d <= 2u) ? 144u : ((d == 3u) ? 36u : 9u); /* 144,144,144,36,9 */
}

/* w-block start of depth-d cell c (the cell's home scale) */
static inline uint32_t gsw_cell_scale(uint32_t cell, uint32_t d) {
    if (d == 0u)       return 0u;
    if (d == 1u)       return cell * 36u;          /* c ∈ [0,4)     */
    if (d == 2u)       return cell * 9u;           /* c ∈ [0,16)    */
    if (d == 3u)       return (cell >> 2u) * 9u;   /* c ∈ [0,64)    */
    return (cell >> 4u) * 9u;                      /* c ∈ [0,256)   */
}

/* pos-block start of depth-d cell c */
static inline uint32_t gsw_cell_pos_base(uint32_t cell, uint32_t d) {
    if (d <= 2u)       return 0u;
    if (d == 3u)       return (cell & 3u) * 36u;   /* 4 per w-block */
    return (cell & 15u) * 9u;                      /* 16 per w-block*/
}

/* canonical scale position of depth d — the LAST w-block start:
 * {0, 108, 135, 135, 135} — deepest position the depth-d partition
 * reaches on the scale axis. */
static inline uint32_t gsw_depth_scale(uint32_t d) {
    return GSW_LOCAL - gsw_scale_ext(d);
}

/* depth-d cell count (4^d) — same ladder as th_cell_count(4, d) */
static inline uint32_t gsw_cell_count(uint32_t d) {
    return 1u << (2u * d);                          /* 1,4,16,64,256 */
}

/* ── Scale timeline addressing (convention §2.4) ────────────────
 * physical p = (a_w·l + b_w) % 144, gcd(a_w,144)=1 → bijection.
 * a_w cycles the 48 odd numbers < 144 not divisible by 3;
 * b_w = 13·w mod 144 breaks ties → 144 distinct bijective views. */
typedef struct {
    uint8_t a[GSW_LOCAL];
    uint8_t b[GSW_LOCAL];
    uint8_t inv[GSW_LOCAL];
} GSWScale;

static inline void gsw_scale_init(GSWScale *s) {
    uint8_t cop[48];
    uint32_t n = 0;
    for (uint32_t k = 1; k < GSW_LOCAL && n < 48; k += 2)
        if (k % 3u != 0) cop[n++] = (uint8_t)k;
    for (uint32_t w = 0; w < GSW_LOCAL; w++) {
        s->a[w] = cop[w % 48];
        s->b[w] = (uint8_t)((w * 13u) % GSW_LOCAL);
        uint8_t inv = 0;
        for (uint32_t x = 1; x < GSW_LOCAL && inv == 0; x++)
            if (((uint32_t)s->a[w] * x) % GSW_LOCAL == 1u) inv = (uint8_t)x;
        s->inv[w] = inv;
    }
}

/* the a_w view: logical local l → physical p at scale w */
static inline uint32_t gsw_view(const GSWScale *s, uint32_t w, uint32_t l) {
    return ((uint32_t)s->a[w] * l + (uint32_t)s->b[w]) % GSW_LOCAL;
}

#endif /* GEO_SCALE_WIRE_H */
