/*
 * tri_hex_tess.h — TriHex Tessellation Bond Layer
 *
 * Y-triangle retarget: THCoord is now a single node_id (0..20735).
 * Bond metric = step count on trihex graph (same pentagon=0-2, diff=3)
 * Coverage: 12 pentagons × 1728 nodes = 20736 nodes, zero gaps
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

#endif /* TRI_HEX_TESS_H */
