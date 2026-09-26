/* bfs_quadtree.h — live-block index over the 144 BFS blocks.
 * Blocks are 1D [0,144); viewed as a 12x12 grid (gx=b/12, gy=b%12),
 * padded to 16x16, depth 4. Nodes hold live counts; empty quadrants
 * are skipped on iteration. Rebuilt per query (files <= 64) — no
 * incremental state to go stale.
 */
#ifndef BFS_QUADTREE_H
#define BFS_QUADTREE_H

#include <stdint.h>
#include <string.h>
#include "breathing_fs.h"

#define BQT_GRID 12u
#define BQT_PAD  16u
#define BQT_DEPTH 4u
#define BQT_NODES 341u   /* 1+4+16+64+256 */

typedef struct {
    uint8_t cnt[BQT_NODES];   /* live blocks per node (max 144 fits) */
    uint8_t live[144];        /* per-block bitmap */
} BQTree;

static inline uint32_t bqt_off(unsigned l) {
    /* level offsets: L0:0 L1:1 L2:5 L3:21 L4:85 */
    static const uint32_t o[5] = {0, 1, 5, 21, 85};
    return o[l];
}

static inline uint32_t bqt_idx(unsigned l, uint32_t gx, uint32_t gy) {
    uint32_t nx = gx >> (BQT_DEPTH - l);
    uint32_t ny = gy >> (BQT_DEPTH - l);
    return bqt_off(l) + ny * (1u << l) + nx;
}

static inline void bqt_mark(BQTree *t, uint32_t b) {
    if (!t || b >= 144u || t->live[b]) return;
    t->live[b] = 1;
    uint32_t gx = b / BQT_GRID, gy = b % BQT_GRID;
    for (unsigned l = 0; l <= BQT_DEPTH; l++)
        t->cnt[bqt_idx(l, gx, gy)]++;
}

/* quadrant live count at level l (l=1: four 8x8 quads, l=2: 4x4, ...). */
static inline uint32_t bqt_quad(const BQTree *t, unsigned l,
                                uint32_t qx, uint32_t qy) {
    if (!t || l < 1 || l > BQT_DEPTH) return 0;
    if (qx >= (1u << l) || qy >= (1u << l)) return 0;
    return t->cnt[bqt_off(l) + qy * (1u << l) + qx];
}

static inline void bqt_build(BreathingFS *fs, BQTree *t) {
    if (!fs || !t) return;
    memset(t, 0, sizeof(*t));
    for (uint32_t i = 0; i < fs->n_files; i++) {
        BFSFileEntry *e = &fs->files[i];
        if (!e->valid) continue;
        for (uint32_t b = 0; b < e->n_blocks; b++) {
            uint32_t id = e->home_block + b;
            if (id < 144u) bqt_mark(t, id);
        }
    }
}

static inline uint32_t bqt_count(const BQTree *t) {
    return t ? t->cnt[0] : 0;
}

/* next live block strictly after b (-1 starts at 0); L2 quadrants
 * (4x4 cells) with count 0 are jumped, never scanned. -1 when none. */
static inline int bqt_next_live(const BQTree *t, int b) {
    if (!t) return -1;
    int c = b + 1;
    while (c < 144) {
        uint32_t gx = (uint32_t)c / BQT_GRID, gy = (uint32_t)c % BQT_GRID;
        /* L2 cell of this block; skip the whole cell if empty. */
        uint32_t cx = gx >> 2, cy = gy >> 2;
        if (bqt_quad(t, 2, cx, cy) == 0) {
            /* jump to first block of next L2 cell in scan order */
            int jumped = 0;
            for (; c < 144; c++) {
                uint32_t hx = (uint32_t)c / BQT_GRID, hy = (uint32_t)c % BQT_GRID;
                if ((hx >> 2) != cx || (hy >> 2) != cy) { jumped = 1; break; }
            }
            if (!jumped) return -1;
            continue;
        }
        if (t->live[c]) return c;
        c++;
    }
    return -1;
}

static inline int bqt_first_live(const BQTree *t) {
    return bqt_next_live(t, -1);
}

#endif /* BFS_QUADTREE_H */
