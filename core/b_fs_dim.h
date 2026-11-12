/*
 * b_fs_dim.h — Phase 2: Cross-Dimension Adapters
 * ════════════════════════════════════════════════════════════════════
 * Dimension = a view (coordinate system) over the same 20736 flat space.
 *   FLAT : identity
 *   TESS : 18 towers × 1152 slots (tiles flat space, identity-ish)
 *   KIS  : stride-37 permutation (bijective, proven coprime w/ 20736)
 * Transfer between dims is a bijection: write in dim A → read in dim B
 * at the transferred address yields the same byte.
 * All transforms are pure arithmetic — no lookup, no data, no storage.
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_DIM_H
#define B_FS_DIM_H

#include <stdint.h>
#include "b_fs_base.h"

#define BFS_TESS_TOWERS   18u
#define BFS_TESS_PER_TOWER 1152u   /* 18 * 1152 = 20736 */

/* flat <-> dim-local address */
static inline uint32_t bfs_dim_to_flat(BfsDim dim, uint32_t local) {
    local %= BFS_PAIR_N;
    if (dim == BFS_DIM_FLAT) return local;
    if (dim == BFS_DIM_TESS) return local;   /* tesseract tiles flat 1:1 */
    /* KIS: apply inverse of stride-37 to get flat */
    return bfs_pair_inverse(BFS_DIM_FLAT, BFS_DIM_KIS, local);
}

static inline uint32_t bfs_dim_from_flat(BfsDim dim, uint32_t flat) {
    flat %= BFS_PAIR_N;
    if (dim == BFS_DIM_FLAT) return flat;
    if (dim == BFS_DIM_TESS) return flat;
    return bfs_pair_transfer(BFS_DIM_FLAT, BFS_DIM_KIS, flat);
}

/* Twin views across two dimensions: hot in src dim, cold in dst dim. */
typedef struct {
    BfsDim src, dst;
    uint32_t src_addr, dst_addr;
} BfsDimTwin;

static inline void bfs_dim_twin_init(BfsDimTwin *t, BfsDim src, BfsDim dst, uint32_t addr) {
    if (!t) return;
    t->src = src; t->dst = dst;
    t->src_addr = addr % BFS_PAIR_N;
    t->dst_addr = bfs_pair_transfer(src, dst, t->src_addr);
}

static inline int bfs_dim_twin_roundtrip_ok(const BfsDimTwin *t) {
    if (!t) return 0;
    return (bfs_pair_inverse(t->src, t->dst, t->dst_addr) == t->src_addr) ? 1 : 0;
}

#endif /* B_FS_DIM_H */
