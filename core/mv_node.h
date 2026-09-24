/* mv_node.h — multiverse node: (id × layer × slide) + Peano entry/exit.
 *
 * A node is one 3×3 Lo Shu cell block living in a 4D stack of near-duplicate
 * layers (~0.01% apart) at a climate slide offset. Peano is PLACED on the
 * node (no computation): the system's standard Peano-3×3 orientation gives
 * 1 entry + 1 exit for free (curve endpoints); 4 rotations + traversal
 * direction select which corners. Major route chains exit→entry; minors
 * use the remaining 7 cells and never touch pinned cells.
 *
 * Base order (system standard, cells row-major idx=y*3+x):
 *   0→1→2→5→4→3→6→7→8, endpoints {0,8} at orient 0.
 *
 * Header-only, std-only. No separate build.
 */
#ifndef MV_NODE_H
#define MV_NODE_H

#include <stdint.h>

typedef struct {
    uint32_t node;   /* anchor id */
    uint32_t slide;  /* climate offset k (cf. ClimRec.offset) */
    uint16_t layer;  /* universe layer in the 4D stack */
    uint8_t entry;   /* pinned entry cell 0..8 */
    uint8_t exit;    /* pinned exit cell 0..8 */
} MVNode;            /* 12 bytes, naturally packed */

/* 3x3 rot90 clockwise on cell idx */
static inline uint8_t mv_rot90(uint8_t c) {
    uint8_t x = c % 3, y = c / 3;
    return (uint8_t)((2u - y) + x * 3u);
}

/* endpoints for orientation r (0..3 rotation, bit2 = traverse exit→entry).
 * 0=ok, -1=bad orient. */
static inline int mv_entry_exit(uint8_t orient, uint8_t *e, uint8_t *x) {
    if (orient > 7) return -1;
    uint8_t a = 0, b = 8;
    for (uint8_t i = 0; i < (orient & 3u); i++) { a = mv_rot90(a); b = mv_rot90(b); }
    if (orient & 4u) { uint8_t t = a; a = b; b = t; }
    if (e) *e = a;
    if (x) *x = b;
    return 0;
}

/* init node. 0=ok, -1=bad orient. */
static inline int mv_init(MVNode *n, uint32_t node, uint16_t layer,
                          uint32_t slide, uint8_t orient) {
    uint8_t e, x;
    if (!n || mv_entry_exit(orient, &e, &x) != 0) return -1;
    n->node = node;
    n->slide = slide;
    n->layer = layer;
    n->entry = e;
    n->exit = x;
    return 0;
}

/* edge-adjacency on 3x3 (Manhattan distance 1). */
static inline int mv_edge_adj(uint8_t a, uint8_t b) {
    if (a > 8 || b > 8) return 0;
    int dx = (int)(a % 3) - (int)(b % 3);
    int dy = (int)(a / 3) - (int)(b / 3);
    return (dx == 0 && (dy == 1 || dy == -1)) || (dy == 0 && (dx == 1 || dx == -1));
}

/* pinned cell test: 1 if cell is entry/exit (major-only), 0 if minor-usable. */
static inline int mv_pinned(const MVNode *n, uint8_t cell) {
    if (!n || cell > 8) return -1;
    return (cell == n->entry || cell == n->exit) ? 1 : 0;
}

#endif /* MV_NODE_H */
