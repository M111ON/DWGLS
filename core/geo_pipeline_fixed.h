/*
 * geo_pipeline_fixed.h — Wired pipeline: fixed 1tes frame → GeoFS → hyperbolic
 *
 * 1tes = 8 cells × 144 slots =1152 blocks (73KB). We pin tess 0 and use
 *   flat = cell*144 + slot  (tess_flat(0,cell,slot)) as the GeoFS block
 *   allocator — no freelist scan, no header collision, O(1).
 * Inside each cell we can optionally scatter with hyperbolic stride
 *   (9/27/81) staying inside the cell's 144-slot window:
 *     addr = cell_base + (slot0 + stride·b) % 144
 * This keeps "fixed frame" guarantee: interior walk never leaves the
 * pinned cell/tesseract, so no field-wide distortion.
 *
 * Extends to 5D: add axis 4 → 10 cells (2*5), TESS_CELLS=10, same flat math
 * with TESS_SLOTS=144, just grow the cell count.
 */

#ifndef GEO_PIPELINE_FIXED_H
#define GEO_PIPELINE_FIXED_H

#include <stdint.h>
#include <string.h>
#include "geo_tesseract_addr.h"
#include "geofs_core.h"
#include "geo_hyperbolic_walk.h"

#define PIPE_TESS     0u
#define PIPE_CELLS    TESS_CELLS
#define PIPE_SLOTS    TESS_SLOTS

/* cell base in the 20736 field */
static inline uint32_t pipe_cell_base(uint32_t cell) {
    return tess_flat(PIPE_TESS, cell, 0);
}

/* allocate a contiguous run inside one fixed cell — returns base flat */
static inline uint32_t pipe_alloc_in_cell(uint32_t cell, uint32_t n_blocks) {
    if (cell >= PIPE_CELLS || n_blocks == 0 || n_blocks > PIPE_SLOTS) return 0xFFFFFFFFu;
    return pipe_cell_base(cell);
}

/* scatter address inside a pinned cell: base + (stride·b % 144) */
static inline uint32_t pipe_scatter_in_cell(uint32_t cell, uint32_t axis, uint32_t b) {
    uint32_t base = pipe_cell_base(cell);
    uint32_t stride = hw_stride(axis);
    uint32_t slot = (stride * b) % PIPE_SLOTS;
    return base + slot;
}

/* place a buffer as one file occupying exactly one fixed cell (contiguous) */
static inline GeosInode* pipe_place_cell(GeosVolume *vol,
                                         const char *name,
                                         const uint8_t *data, uint32_t size,
                                         uint32_t cell) {
    if (!vol || !name || !data) return NULL;
    uint32_t n_blocks = (size + GEOS_BLOCK_SZ - 1) / GEOS_BLOCK_SZ;
    if (n_blocks > PIPE_SLOTS) return NULL;
    uint32_t base = pipe_cell_base(cell);
    /* ensure blocks are free (fixed frame has no header at 0..255? cell bases are 0,144,288... some <256.
       For 1tes, cells 0 and 1 overlap header (0..255). So we skip those cells or offset.
       Use cells 2..7 for data to avoid header 0..255. */
    for (uint32_t b = 0; b < n_blocks; b++) {
        uint32_t flat = base + b;
        if (flat < GEOS_VOL_DATA_START) return NULL;
        if (vol->block_map[flat/8] & (1u << (flat%8))) return NULL;
    }
    /* mark */
    for (uint32_t b = 0; b < n_blocks; b++) {
        uint32_t flat = base + b;
        vol->block_map[flat/8] |= (1u << (flat%8));
        vol->total_blocks_free--;
        memcpy(&vol->data[flat*GEOS_BLOCK_SZ], data + b*GEOS_BLOCK_SZ, GEOS_BLOCK_SZ);
    }
    /* inode: reuse geofs slot but store flat base directly */
    if (vol->inode_count >= GEOS_MAX_INODES) return NULL;
    GeosInode *in = &vol->inodes[vol->inode_count++];
    memset(in, 0, sizeof(*in));
    strncpy(in->name, name, GEOS_MAX_NAME-1);
    in->block_start = base;
    in->block_count = (uint16_t)n_blocks;
    in->size_bytes = size;
    in->flags = 0;
    return in;
}

static inline int pipe_verify(void) {
    if (PIPE_CELLS * PIPE_SLOTS != TESS_PER_TESS) return -1;
    if (geo_tesseract_verify() != 0) return -2;
    return 0;
}

#endif
