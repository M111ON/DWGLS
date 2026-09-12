/*
 * geo_entropy_quadtree.h — Entropy-Driven QuadTree on Fractal Address Space
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Multi-resolution subdivision of the 20736 address space.
 * Split decision driven by entropy of each region.
 *
 * Hierarchy:
 *   h=4: root (1 cell, 20736 addresses)
 *   h=3: 12 groups
 *   h=2: 144 regions (12×12)
 *   h=1: 1728 sub-regions (144×12)
 *   h=0: 20736 leaves (144×144)
 *
 * Myelination principle:
 *   High entropy → split deeper → pointer resolves faster
 *   Low entropy → group coarser → bulk skip
 *
 * DEPENDS: geo_fractal_addr.h
 * No malloc. All static inline. Header-only.
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_ENTROPY_QUADTREE_H
#define GEO_ENTROPY_QUADTREE_H

#include <stdint.h>
#include <stdio.h>
#include "geo_fractal_addr.h"

/* ═══════════════════════════════════════════════════════════════════════════
   NODE — one cell in the entropy tree
   ═══════════════════════════════════════════════════════════════════════════ */

#define EQ_SPLIT_NONE   0u
#define EQ_SPLIT_X      1u  /* split along x (column boundary) */
#define EQ_SPLIT_Y      2u  /* split along y (row boundary) */
#define EQ_SPLIT_BOTH   3u  /* split both (full quad) */

typedef struct {
    uint32_t addr;      /* flat start address of this cell */
    uint32_t size;      /* number of addresses in cell (12^h) */
    uint8_t  h;         /* fractal height (0=leaf, 4=root) */
    uint8_t  entropy;   /* 0-255, higher = more complex */
    uint8_t  split;     /* EQ_SPLIT_*: how this cell is subdivided */
    uint8_t  myelinated;/* 1 = hot path (high traffic) */
} EQNode;

/* ═══════════════════════════════════════════════════════════════════════════
   ENTROPY COMPUTATION — from raw data
   ═══════════════════════════════════════════════════════════════════════════
   Counts unique byte values, scales to 0-255.
   1 unique byte = 0, 256 unique bytes = 255.
   Simple, fast, correct for our purposes.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t eq_compute_entropy(const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return 0;

    uint8_t seen[256];
    __builtin_memset(seen, 0, sizeof(seen));
    uint16_t unique = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (!seen[data[i]]) { seen[data[i]] = 1; unique++; }
    }
    /* 1 unique = entropy 0, 256 unique = entropy 255 */
    return (uint8_t)(unique > 1 ? unique - 1 : 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SPLIT DECISION — entropy-driven
   ═══════════════════════════════════════════════════════════════════════════ */

#define EQ_THRESHOLD_LOW    64u     /* below → group (no split) */
#define EQ_THRESHOLD_HIGH   128u    /* above → full quad split (max in 144B window is 143) */

static inline uint8_t eq_split_decision(uint8_t entropy, uint32_t h)
{
    if (h == 0) return EQ_SPLIT_NONE;          /* leaf: cannot split */
    if (h >= FRACTAL_MAX_H) return EQ_SPLIT_BOTH; /* root: always split */

    if (entropy < EQ_THRESHOLD_LOW)
        return EQ_SPLIT_NONE;                   /* low entropy: group */
    if (entropy > EQ_THRESHOLD_HIGH)
        return EQ_SPLIT_BOTH;                   /* high entropy: full quad */
    /* Medium: split on one axis only */
    return (entropy & 1u) ? EQ_SPLIT_X : EQ_SPLIT_Y;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TREE NODE INIT
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EQNode eq_make_node(uint32_t h, uint32_t x, uint32_t y,
                                   const uint8_t *data, uint32_t data_len)
{
    EQNode n;
    n.addr = fractal_to_flat(h, x, y);
    n.size = fractal_cell_size(h);
    n.h    = (uint8_t)h;
    n.entropy = eq_compute_entropy(
        data ? data + n.addr : NULL,
        data ? (n.size <= data_len - n.addr ? n.size : 0) : 0);
    n.split = eq_split_decision(n.entropy, h);
    n.myelinated = (n.entropy > EQ_THRESHOLD_HIGH) ? 1u : 0u;
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TREE STATS
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_nodes;
    uint32_t split_none;
    uint32_t split_x;
    uint32_t split_y;
    uint32_t split_both;
    uint32_t myelinated;
    uint32_t max_depth;     /* deepest split reached */
    uint32_t total_addrs;   /* sum of all leaf sizes */
} EQStats;

static inline void eq_stats_init(EQStats *s) { if (s) __builtin_memset(s, 0, sizeof(*s)); }

static inline void eq_stats_add(EQStats *s, const EQNode *n)
{
    if (!s || !n) return;
    s->total_nodes++;
    s->total_addrs += n->size;
    if (n->myelinated) s->myelinated++;
    switch (n->split) {
        case EQ_SPLIT_NONE:  s->split_none++;  break;
        case EQ_SPLIT_X:     s->split_x++;     break;
        case EQ_SPLIT_Y:     s->split_y++;     break;
        case EQ_SPLIT_BOTH:  s->split_both++;  break;
    }
    if (n->h < s->max_depth || s->total_nodes == 1)
        s->max_depth = n->h;
}

static inline void eq_stats_print(const EQStats *s)
{
    if (!s) return;
    printf("═══════════════════════════════════════════════\n");
    printf("  Entropy QuadTree Stats\n");
    printf("═══════════════════════════════════════════════\n");
    printf("  Total nodes:    %u\n", s->total_nodes);
    printf("  Split NONE:     %u (%.1f%% — grouped)\n",
           s->split_none,
           s->total_nodes ? 100.0 * s->split_none / s->total_nodes : 0);
    printf("  Split X:        %u\n", s->split_x);
    printf("  Split Y:        %u\n", s->split_y);
    printf("  Split BOTH:     %u (%.1f%% — full quad)\n",
           s->split_both,
           s->total_nodes ? 100.0 * s->split_both / s->total_nodes : 0);
    printf("  Myelinated:     %u (%.1f%% — hot path)\n",
           s->myelinated,
           s->total_nodes ? 100.0 * s->myelinated / s->total_nodes : 0);
    printf("  Max depth:      %u\n", s->max_depth);
    printf("  Total addrs:    %u\n", s->total_addrs);
    printf("═══════════════════════════════════════════════\n");
}

#endif /* GEO_ENTROPY_QUADTREE_H */
