/*
 * tri_hex_tess.h — TriHex Tessellation Bond Layer
 *
 * Y-triangle retarget: THCoord is now a single node_id (0..20735).
 * Bond metric = step count on trihex graph (same pentagon=0-2, diff=3)
 * Coverage: 12 pentagons × 1728 nodes = 20736 nodes, zero gaps
 *
 * SUBDIVISION (rule-only, rescope 2026-08-14): the equal-triangle floor
 * is a RULE over addresses, not constructed geometry. Mixed radix:
 *   node_id = hi·81 + lo        hi ∈ [0,256) = 4-ladder (2⁸ = 4⁴)
 *                                lo ∈ [0,81)  = 3-ladder (3⁴)
 *   20736 = 4⁴·3⁴ = 256·81      — two ladders tile the same space
 *
 * th_subdivide(node, aperture, depth, child) — one level deeper:
 *   aperture 4 → 4 children (1/4 per level: 1→4→16→64→256) — refines hi
 *     child picks the next 2 bits of hi (base-4 digit)
 *   aperture 3 → 3 children (1/3 per level: 1→3→9→27→81)   — refines lo
 *     child picks the next base-3 digit of lo
 *   aperture 7 → hexagon: 1 center + 6 ring (HEX_CELLS=7)  — groups 7
 * th_parent(node, aperture, depth) — one level coarser (lossless inverse)
 *
 * All integer, no trig, no table, no coordinate math — geometry as
 * address-space rule (MAP not COMPRESS).
 */

#ifndef TRI_HEX_TESS_H
#define TRI_HEX_TESS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────────────── */
#define TH_PENTAGON_NODES  1728u  /* 20736 / 12 */
#define GEO_FULL           20736u
#define GEO_PENTAGONS      12u

/* ── Subdivision constants ─────────────────────────────────── */
#define TH_HI_BASE         256u   /* 4⁴ = 2⁸ — binary floor axis   */
#define TH_LO_BASE         81u    /* 3⁴      — Peano 3-adic axis   */
#define TH_SUBDIV_4_DEPTH  4u     /* 4 levels: 1→4→16→64→256       */
#define TH_SUBDIV_3_DEPTH  4u     /* 4 levels: 1→3→9→27→81         */

/* mixed-radix split: node_id = hi·81 + lo */
static inline uint32_t th_hi4(uint32_t node) { return node / TH_LO_BASE; }
static inline uint32_t th_lo3(uint32_t node) { return node % TH_LO_BASE; }
static inline uint32_t th_node(uint32_t hi, uint32_t lo) { return hi * TH_LO_BASE + lo; }

/* ── Types ──────────────────────────────────────────────────── */
typedef struct {
    uint32_t node_id;     /* 0..20735 Y-triangle node_id */
} THCoord;

typedef struct {
    int toggle_level;     /* 0=off 1=pentagon 2=hex 3=trihex */
    int aperture;         /* 3/4/7 — subdivision factor */
} THGrid;

/* ── Inline helpers ─────────────────────────────────────────── */
static inline uint8_t th_pentagon(THCoord c) {
    return (uint8_t)(c.node_id / TH_PENTAGON_NODES);
}

static inline uint16_t th_local(uint32_t node_id) {
    return (uint16_t)(node_id % TH_PENTAGON_NODES);
}

static inline uint8_t th_shell(uint32_t node_id) {
    uint16_t local = th_local(node_id);
    if (local < 6) return 0;        /* center */
    if (local < 18) return 1;       /* ring 1 */
    if (local < 36) return 2;       /* ring 2 */
    return (uint8_t)(3 + (local - 36) / 30);  /* outer rings */
}

/* ── Bond metric ────────────────────────────────────────────── */
static inline int th_steps(THCoord a, THCoord b) {
    uint32_t pa = th_pentagon(a);
    uint32_t pb = th_pentagon(b);
    if (pa != pb) return 3;
    uint16_t la = th_local(a.node_id);
    uint16_t lb = th_local(b.node_id);
    if (la == lb) return 0;
    uint16_t diff = (la > lb) ? (la - lb) : (lb - la);
    return (diff <= 6) ? 1 : 2;
}

static inline float th_bond_strength(THCoord a, THCoord b) {
    static const float W[4] = {1.0f, 0.85f, 0.5f, 0.05f};
    int s = th_steps(a, b);
    return W[s < 4 ? s : 3];
}

static inline int th_is_always_warm(THCoord norm, THCoord weight) {
    return th_steps(norm, weight) <= 1;
}

/* ═══════════════ SUBDIVISION — rule-only (rescope) ═══════════════ */
/*
 * node = hi·81 + lo. Subdividing one level:
 *   aperture 4: hi gains 2 bits (base-4 digit) → 4 children, area /4
 *   aperture 3: lo gains a base-3 digit → 3 children, area /3
 * depth = CURRENT level (0 = whole field, no digits fixed yet).
 * child selects the next digit.
 */

/* 4-subdivision: next 2 bits of hi (base-4 digit) at bit position 6-2·depth */
static inline uint32_t th_subdivide4(uint32_t node, uint32_t depth, uint32_t child) {
    uint32_t hi = th_hi4(node);
    uint32_t lo = th_lo3(node);
    uint32_t shift = 6u - 2u * depth;          /* bit position of new digit */
    uint32_t mask = (uint32_t)((1u << (8u - 2u * depth)) - 1u);
    uint32_t parent_hi = hi & ~mask;           /* keep fixed prefix */
    uint32_t child_hi = parent_hi | (child << shift);
    return th_node(child_hi, lo);
}

/* parent of a 4-subdivided cell: drop the last base-4 digit of hi.
 * child_depth = depth of the CHILD (1..4). Clears digits child_depth..3,
 * i.e. keeps the first child_depth-1 base-4 digits (bits 2·(5-child_depth)..7). */
static inline uint32_t th_parent4(uint32_t node, uint32_t child_depth) {
    uint32_t hi = th_hi4(node);
    uint32_t lo = th_lo3(node);
    uint32_t clear = 10u - 2u * child_depth;   /* bits 0..clear-1 to zero */
    if (clear >= 8u) return th_node(0, lo);
    uint32_t mask = (uint32_t)((1u << clear) - 1u);
    return th_node(hi & ~mask, lo);
}

/* 3-subdivision: next base-3 digit of lo at position 3^(3-depth) */
static inline uint32_t th_pow3(uint32_t k) {
    uint32_t p = 1u;
    for (uint32_t i = 0; i < k; i++) p *= 3u;
    return p;
}
static inline uint32_t th_subdivide3(uint32_t node, uint32_t depth, uint32_t child) {
    uint32_t hi = th_hi4(node);
    uint32_t lo = th_lo3(node);
    uint32_t step = th_pow3(3u - depth);       /* child span at this depth */
    uint32_t base = (lo / (step * 3u)) * (step * 3u);  /* parent cell start */
    return th_node(hi, base + child * step);
}

/* parent of a 3-subdivided cell: drop the last base-3 digit of lo */
static inline uint32_t th_parent3(uint32_t node, uint32_t child_depth) {
    uint32_t hi = th_hi4(node);
    uint32_t lo = th_lo3(node);
    uint32_t step = th_pow3(4u - child_depth); /* parent cell span */
    return th_node(hi, (lo / (step * 3u)) * (step * 3u));
}

/* unified subdivide: aperture 3/4 → one level deeper */
static inline uint32_t th_subdivide(uint32_t node, uint32_t aperture,
                                    uint32_t depth, uint32_t child) {
    if (aperture == 3u) return th_subdivide3(node, depth, child);
    if (aperture == 4u) return th_subdivide4(node, depth, child);
    return node;
}

/* unified parent: aperture 3/4 → one level coarser (lossless inverse) */
static inline uint32_t th_parent(uint32_t node, uint32_t aperture,
                                 uint32_t child_depth) {
    if (aperture == 3u) return th_parent3(node, child_depth);
    if (aperture == 4u) return th_parent4(node, child_depth);
    return node;
}

/* cell base at depth: the canonical address of the cell containing node.
 * 4-ladder: low (8-2d) hi-bits zeroed (d base-4 digits fixed).
 * 3-ladder: low lo digits below 3^(4-d) zeroed (d trits fixed).
 * subdivide/parent roundtrip is exact ON cell bases. */
static inline uint32_t th_cell_anchor(uint32_t node, uint32_t aperture, uint32_t depth) {
    uint32_t hi = th_hi4(node), lo = th_lo3(node);
    if (aperture == 4u) {
        uint32_t clear = 8u - 2u * depth;           /* low hi-bits to zero */
        if (clear >= 8u) return th_node(0, lo);
        uint32_t mask = (uint32_t)((1u << clear) - 1u);
        return th_node(hi & ~mask, lo);
    }
    if (aperture == 3u) {
        uint32_t step = th_pow3(4u - depth);
        return th_node(hi, (lo / step) * step);
    }
    return node;
}

/* cell count at depth (aperture 4 → 4^depth; aperture 3 → 3^depth) */
static inline uint32_t th_cell_count(uint32_t aperture, uint32_t depth) {
    uint32_t c = 1u;
    for (uint32_t i = 0; i < depth; i++) c *= aperture;
    return c;
}

/* cell size in nodes at depth: 20736 / count */
static inline uint32_t th_cell_size(uint32_t aperture, uint32_t depth) {
    return GEO_FULL / th_cell_count(aperture, depth);
}

/* 1/2-step: subdividing by 4 twice == area/16; a single "1/2" step =
 * half of a 4-subdivision — express as two 4-subdivisions of one child */
static inline uint32_t th_quarter2(uint32_t node, uint32_t depth, uint32_t child) {
    /* two 4-subdivisions: child ∈ [0,16) picks the 2-digit pair */
    return th_subdivide4(th_subdivide4(node, depth, child / 4u), depth + 1u,
                         child % 4u);
}

/* ═══════════════ HEXAGON TILE — aperture 7 (1 center + 6 ring) ═══════════════ */
/* Groups 7 consecutive nodes into one hex tile: c[0..5] = ring, c[6] = center.
 * Matches HEX_CELLS=7 / HEX_CENTER=6 in hex_tile.h. Deterministic, integer. */
static inline void th_hex7_tile(uint32_t node, uint32_t out[7]) {
    uint32_t tile_base = (node / 7u) * 7u;
    for (uint32_t i = 0; i < 7u; i++) out[i] = tile_base + i;
}
static inline uint32_t th_hex7_tile_id(uint32_t node) { return node / 7u; }
static inline uint32_t th_hex7_center(uint32_t node) { return (node / 7u) * 7u + 6u; }

#endif /* TRI_HEX_TESS_H */
