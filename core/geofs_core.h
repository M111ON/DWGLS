/* ═══════════════════════════════════════════════════════════════════════════
 * geofs_core.h — GeoFS: Geometric Filesystem Prototype
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * "No compute, only lookup and sync transport" — applied to a filesystem.
 *
 * Maps POSIX filesystem primitives onto DWGLS's 20736 address space.
 * Every file, block, and directory entry has a geometric address.
 *
 * DEPENDS:
 *   geo_cube_addr.h      — (generation, face, slot) addressing
 *   geo_cell_classify.h  — 3-bit parity cell types
 *   geo_adaptive_store.h — entropy-based tier system
 *   geo_kis_container.h  — KIS container (CRC-64, tiers)
 *   geo_frame_seek.h     — KIS timeline (stride-37, 1440 cycle)
 *   geo_tring_walk.h     — TRing spoke routing
 *   infra/gear_lock.h    — GEAR_GEO_FULL = 20736
 *   infra/fibo_spine.h   — FS_PIPES = 1728, FS_TICKS = 12
 *
 * No malloc in hot path. All static inline. Header-only.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_GEOFS_H
#define GEO_GEOFS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── DWGLS dependencies ─────────────────────────────────── */
#include "geo_cube_addr.h"
#include "geo_cell_addr.h"
#include "geo_cell_classify.h"
#include "geo_cell_prune.h"
#include "geo_adaptive_store.h"
#include "geo_frame_seek.h"
#include "geo_tring_walk.h"
#include "geo_kis_container.h"
#include "geo_fs_voronoi.h"
#include "geo_ghost_lift.h"   /* ghost_read_rule — §15.73: อ่านผ่านกฎที่ train มา */
#include "geo_hyperbolic_walk.h" /* HWRouter — deterministic centroid walk */
#include "infra/gear_lock.h"
#include "infra/fibo_spine.h"

/* ═══════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════ */

#define GEOS_MAGIC          "GFS\0"
#define GEOS_VERSION        1
#define GEOS_BLOCK_SZ       64u
#define GEOS_ADDR_SPACE     20736u
#define GEOS_MAX_INODES     2048u
#define GEOS_MAX_NAME       48u
#define GEOS_MAX_PATH       256u
#define GEOS_MAX_BLOCKS     324u
#define GEOS_MAX_DEPTH      16u

/* ═══════════════════════════════════════════════════════════
   GEOS_ADDR — Unified geometric address
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  flat_id;
    uint8_t   generation;
    uint8_t   face;
    uint16_t  slot;
    uint8_t   cell_type;
} GeosAddr;

static inline GeosAddr geos_addr_from_flat(uint32_t flat_id) {
    GeosAddr a;
    GeoCellAddr ca = geo_cell_addr_from_offset(flat_id);
    a.flat_id    = flat_id % GEOS_ADDR_SPACE;
    a.generation = ca.generation;
    a.face       = ca.face;
    a.slot       = ca.slot;
    a.cell_type  = ca.cell_type;
    return a;
}

static inline GeosAddr geos_addr_make(uint8_t gen, uint8_t face, uint16_t slot) {
    GeoCubeAddr ca = geo_cube_addr(gen, face, slot);
    GeosAddr a;
    a.flat_id    = geo_cube_addr_to_flat(ca);
    a.generation = gen;
    a.face       = face;
    a.slot       = slot;
    a.cell_type  = ca.cell_type;
    return a;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_INODE — File metadata
   ═══════════════════════════════════════════════════════════ */

#define GEOS_FLAG_DIR        0x01u
#define GEOS_FLAG_COMPRESSED 0x02u
#define GEOS_FLAG_MMAP       0x04u
#define GEOS_FLAG_FROZEN     0x08u
#define GEOS_FLAG_PINNED     0x10u
#define GEOS_FLAG_HYPER      0x20u  /* block addresses = centroid walk (MAP) */

typedef struct {
    GeosAddr   addr;
    char       name[GEOS_MAX_NAME];
    uint32_t   size_bytes;
    uint32_t   block_start;
    uint16_t   block_count;
    uint8_t    n_dims;
    uint8_t    dtype;
    uint8_t    entropy;
    uint8_t    tier;
    uint8_t    flags;
    uint8_t    depth;
    uint8_t    hyper_axis;  /* GEOS_FLAG_HYPER: which stride walks the blocks */
    uint32_t   parent_addr;
    uint64_t   created_kis_enc;
    uint64_t   accessed_kis_enc;
    uint64_t   crc64;
} GeosInode;

/* ═══════════════════════════════════════════════════════════
   FREE LIST — sorted ranges for O(k) block allocation
   ═══════════════════════════════════════════════════════════ */

#define GEOS_MAX_FREE_RANGES  512u

typedef struct {
    uint32_t start;
    uint32_t count;
} GeosFreeRange;

typedef struct {
    GeosFreeRange ranges[GEOS_MAX_FREE_RANGES];
    uint16_t      count;
} GeosFreeList;

/* ═══════════════════════════════════════════════════════════
   DATA STORE — in-memory block storage
   ═══════════════════════════════════════════════════════════
   Flat byte array: block N → bytes [N*64 .. N*64+63].
   Total: 20736 × 64 = 1,327,104 bytes (~1.3 MB).
   ═══════════════════════════════════════════════════════════ */

#define GEOS_DATA_STORE_SIZE  (GEOS_ADDR_SPACE * GEOS_BLOCK_SZ)  /* 1.3 MB */
#define GEOS_VOL_HDR_BLOCKS  128u
#define GEOS_VOL_DIR_BLOCKS  128u
#define GEOS_VOL_DATA_START  256u

typedef struct {
    char        magic[4];
    uint8_t     version;
    uint8_t     flags;
    uint16_t    n_files;
    uint16_t    n_dirs;
    uint32_t    total_blocks_used;
    uint32_t    total_blocks_free;
    uint64_t    volume_crc64;
    char        vol_name[32];

    GeosInode   inodes[GEOS_MAX_INODES];
    uint16_t    inode_count;
    uint8_t     block_map[GEOS_ADDR_SPACE / 8];
    uint32_t    total_entropy;
    GeosFreeList free_list;
    uint8_t     *data;          /* heap-allocated block data (1.3 MB) */

    /* ── §15.78 walk clock: state = (round, tick) — อ่าน = เดินนาฬิกา ── */
    uint32_t    walk_round;     /* รอบปัจจุบันบน scale axis (0..143)   */
    uint32_t    walk_tick;      /* tick ปัจจุบัน (0..11)                */
    uint64_t    walk_steps;     /* สะสม steps ที่เดิน (หลักฐาน walk)   */
} GeosVolume;

/* ── Forward declarations (GEOS_HYPER section lives below) ───────── */
static inline GeosInode* geos_hyper_place(GeosVolume *v, const char *name,
                                           uint32_t size_bytes, const uint8_t *data,
                                           uint32_t seed, uint32_t axis);
static inline int geos_hyper_read(GeosVolume *v, const char *name,
                                  uint8_t *buf, uint32_t buf_size);
static inline const uint8_t* geos_hyper_project_block(GeosVolume *v,
                                                      const char *name, uint32_t b);
static inline void _geos_free_inode_blocks(GeosVolume *v, GeosInode *in);

/* ═══════════════════════════════════════════════════════════
   INIT / FREE
   ═══════════════════════════════════════════════════════════ */

static inline void geos_volume_init(GeosVolume *v) {
    memset(v, 0, sizeof(*v));
    memcpy(v->magic, GEOS_MAGIC, 4);
    v->version = GEOS_VERSION;
    v->total_blocks_free = GEOS_ADDR_SPACE - GEOS_VOL_DATA_START;

    /* Allocate data store */
    v->data = (uint8_t *)calloc(1, GEOS_DATA_STORE_SIZE);

    /* Mark volume blocks as used */
    for (uint32_t i = 0; i < GEOS_VOL_DATA_START; i++) {
        v->block_map[i / 8] |= (1u << (i % 8));
    }

    /* Initialize free list: single range [256, 20736) */
    v->free_list.count = 1;
    v->free_list.ranges[0].start = GEOS_VOL_DATA_START;
    v->free_list.ranges[0].count = GEOS_ADDR_SPACE - GEOS_VOL_DATA_START;
}

static inline void geos_volume_free(GeosVolume *v) {
    if (v && v->data) { free(v->data); v->data = NULL; }
}

/* ═══════════════════════════════════════════════════════════
   BLOCK ALLOCATION — free-list allocator (fast)
   ═══════════════════════════════════════════════════════════
   Free list: sorted array of (start, count) ranges.
   Alloc = first-fit scan of free ranges (typically <50 entries).
   Free = insert + merge adjacent. Both O(k) where k = range count.
   Bitmap kept for backward compat (entropy calc, serialize).
   ═══════════════════════════════════════════════════════════ */

/* ── Internal: add range to free list (sorted by start) ────── */

static inline void _geos_freelist_insert(GeosFreeList *fl, uint32_t start, uint32_t count) {
    if (fl->count >= GEOS_MAX_FREE_RANGES) return;

    /* Find insertion point (sorted by start) */
    uint16_t pos = 0;
    while (pos < fl->count && fl->ranges[pos].start < start) pos++;

    /* Shift right */
    for (uint16_t i = fl->count; i > pos; i--)
        fl->ranges[i] = fl->ranges[i - 1];

    fl->ranges[pos].start = start;
    fl->ranges[pos].count = count;
    fl->count++;
}

/* ── Internal: merge adjacent ranges ──────────────────────── */

static inline void _geos_freelist_merge(GeosFreeList *fl) {
    uint16_t write = 0;
    for (uint16_t read = 1; read < fl->count; read++) {
        GeosFreeRange *prev = &fl->ranges[write];
        GeosFreeRange *curr = &fl->ranges[read];
        if (prev->start + prev->count == curr->start) {
            /* Merge */
            prev->count += curr->count;
        } else {
            write++;
            fl->ranges[write] = *curr;
        }
    }
    fl->count = write + 1;
}

/* ── Internal: remove a single scattered address from free list ──── */
/* The centroid walk scatters blocks (stride > 1); a hyper file's
 * addresses are not contiguous, so each reservation splits a range.
 * No-op if the address is not free (already allocated). */

static inline void _geos_freelist_remove_addr(GeosFreeList *fl, uint32_t addr) {
    for (uint16_t i = 0; i < fl->count; i++) {
        GeosFreeRange *r = &fl->ranges[i];
        if (addr < r->start || addr >= r->start + r->count) continue;
        uint32_t before = addr - r->start;
        uint32_t after  = (r->start + r->count) - addr - 1u;
        if (before == 0u && after == 0u) {
            for (uint16_t j = i; j < fl->count - 1u; j++)
                fl->ranges[j] = fl->ranges[j + 1u];
            fl->count--;
        } else if (before == 0u) {
            r->start++; r->count--;
        } else if (after == 0u) {
            r->count--;
        } else {
            uint32_t second_start = addr + 1u;
            uint32_t second_count = after;
            r->count = before;
            _geos_freelist_insert(fl, second_start, second_count);
        }
        return;
    }
}

/* ── Internal: mark bitmap for range ──────────────────────── */

static inline void _geos_bitmap_set(GeosVolume *v, uint32_t start, uint32_t count, int used) {
    for (uint32_t i = start; i < start + count; i++) {
        if (used)
            v->block_map[i / 8] |= (1u << (i % 8));
        else
            v->block_map[i / 8] &= ~(1u << (i % 8));
    }
}

/* ── ALLOC — first-fit from free list ─────────────────────── */

static inline uint32_t geos_alloc_blocks(GeosVolume *v, uint16_t count) {
    if (count == 0) return 0xFFFF;
    if (count > v->total_blocks_free) return 0xFFFF;

    GeosFreeList *fl = &v->free_list;

    /* First-fit scan */
    for (uint16_t i = 0; i < fl->count; i++) {
        if (fl->ranges[i].count >= count) {
            uint32_t start = fl->ranges[i].start;

            /* Shrink or remove range */
            fl->ranges[i].start += count;
            fl->ranges[i].count -= count;
            if (fl->ranges[i].count == 0) {
                /* Remove: shift left */
                for (uint16_t j = i; j < fl->count - 1; j++)
                    fl->ranges[j] = fl->ranges[j + 1];
                fl->count--;
            }

            /* Mark bitmap */
            _geos_bitmap_set(v, start, count, 1);
            v->total_blocks_used += count;
            v->total_blocks_free -= count;
            return start;
        }
    }

    return 0xFFFF;  /* no fit found */
}

/* ── FREE — return range to free list + merge ─────────────── */

static inline void geos_free_blocks(GeosVolume *v, uint32_t start, uint16_t count) {
    if (count == 0) return;

    GeosFreeList *fl = &v->free_list;

    /* Clear bitmap */
    _geos_bitmap_set(v, start, count, 0);

    /* Insert into free list */
    _geos_freelist_insert(fl, start, count);

    /* Merge adjacent ranges */
    _geos_freelist_merge(fl);

    v->total_blocks_used -= count;
    v->total_blocks_free += count;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_CREATE — Create a new file
   ═══════════════════════════════════════════════════════════ */

static inline GeosInode* geos_create(GeosVolume *v,
                                       const char *name,
                                       uint32_t size_bytes,
                                       const uint8_t *data)
{
    if (!v || !name || v->inode_count >= GEOS_MAX_INODES) return NULL;

    uint16_t n_blocks = (uint16_t)((size_bytes + GEOS_BLOCK_SZ - 1) / GEOS_BLOCK_SZ);
    if (n_blocks == 0) n_blocks = 1;

    uint32_t block_start = geos_alloc_blocks(v, n_blocks);
    if (block_start == 0xFFFF) return NULL;

    GeosInode *inode = &v->inodes[v->inode_count];
    memset(inode, 0, sizeof(*inode));
    inode->addr = geos_addr_from_flat(block_start);
    strncpy(inode->name, name, GEOS_MAX_NAME - 1);
    inode->size_bytes = size_bytes;
    inode->block_start = block_start;
    inode->block_count = n_blocks;
    inode->created_kis_enc = frame_enc(v->inode_count);
    inode->accessed_kis_enc = inode->created_kis_enc;

    /* Compute entropy */
    if (data && size_bytes > 0) {
        uint8_t seen[256] = {0};
        for (uint32_t i = 0; i < size_bytes && i < n_blocks * GEOS_BLOCK_SZ; i++)
            seen[data[i]] = 1;
        uint16_t unique = 0;
        for (int i = 0; i < 256; i++) unique += seen[i];
        inode->entropy = (uint8_t)((unique * 255) / 256);
        inode->tier = adaptive_tier(inode->entropy);
    }

    v->inode_count++;
    v->n_files++;
    return inode;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_FIND — Find inode by name
   ═══════════════════════════════════════════════════════════ */

static inline GeosInode* geos_find(GeosVolume *v, const char *name) {
    if (!v || !name) return NULL;
    for (uint16_t i = 0; i < v->inode_count; i++) {
        if (strcmp(v->inodes[i].name, name) == 0)
            return &v->inodes[i];
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_WRITE — write data to file blocks
   ═══════════════════════════════════════════════════════════ */

static inline int geos_write(GeosVolume *v, const char *name,
                              const uint8_t *data, uint32_t size) {
    GeosInode *inode = geos_find(v, name);
    if (!inode || !data || size == 0) return -1;

    uint32_t max_bytes = inode->block_count * GEOS_BLOCK_SZ;
    uint32_t bytes = (size < max_bytes) ? size : max_bytes;

    for (uint32_t b = 0; b < inode->block_count && b * GEOS_BLOCK_SZ < bytes; b++) {
        uint32_t offset = (inode->block_start + b) * GEOS_BLOCK_SZ;
        uint32_t chunk = bytes - b * GEOS_BLOCK_SZ;
        if (chunk > GEOS_BLOCK_SZ) chunk = GEOS_BLOCK_SZ;
        memcpy(&v->data[offset], data + b * GEOS_BLOCK_SZ, chunk);
    }

    inode->size_bytes = bytes;
    return (int)bytes;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_READ — read data from file blocks
   ═══════════════════════════════════════════════════════════ */

static inline int geos_read(GeosVolume *v, const char *name,
                             uint8_t *buf, uint32_t buf_size) {
    GeosInode *inode = geos_find(v, name);
    if (!inode || !buf) return -1;
    if (inode->flags & GEOS_FLAG_HYPER)
        return geos_hyper_read(v, name, buf, buf_size);

    uint32_t bytes = (buf_size < inode->size_bytes) ? buf_size : inode->size_bytes;

    for (uint32_t b = 0; b < inode->block_count && b * GEOS_BLOCK_SZ < bytes; b++) {
        uint32_t offset = (inode->block_start + b) * GEOS_BLOCK_SZ;
        uint32_t chunk = bytes - b * GEOS_BLOCK_SZ;
        if (chunk > GEOS_BLOCK_SZ) chunk = GEOS_BLOCK_SZ;
        memcpy(buf + b * GEOS_BLOCK_SZ, &v->data[offset], chunk);
    }

    return (int)bytes;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_DELETE — Remove file
   ═══════════════════════════════════════════════════════════ */

static inline int geos_delete(GeosVolume *v, const char *name) {
    if (!v || !name) return -1;

    for (uint16_t i = 0; i < v->inode_count; i++) {
        if (strcmp(v->inodes[i].name, name) == 0) {
            _geos_free_inode_blocks(v, &v->inodes[i]);
            if (i < v->inode_count - 1) {
                memmove(&v->inodes[i], &v->inodes[i + 1],
                        sizeof(GeosInode) * (v->inode_count - 1 - i));
            }
            v->inode_count--;
            v->n_files--;
            return 0;
        }
    }
    return -2;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_SUMMON — Materialize data at a geometric coordinate
   ═══════════════════════════════════════════════════════════
   "Summon at location" — the ONLY way to place data.
   Data lives at (x, y, z). Moving after summon = drift.
   Coordinate IS identity — no rename, no move.
   ═══════════════════════════════════════════════════════════ */

static inline GeosInode* geos_summon(GeosVolume *v,
                                      const char *name,
                                      uint32_t size_bytes,
                                      const uint8_t *data,
                                      uint8_t gen, uint8_t face, uint16_t slot) {
    if (!v || !name || v->inode_count >= GEOS_MAX_INODES) return NULL;

    uint16_t n_blocks = (uint16_t)((size_bytes + GEOS_BLOCK_SZ - 1) / GEOS_BLOCK_SZ);
    if (n_blocks == 0) n_blocks = 1;

    uint32_t block_start = geos_alloc_blocks(v, n_blocks);
    if (block_start == 0xFFFF) return NULL;

    GeosInode *inode = &v->inodes[v->inode_count];
    memset(inode, 0, sizeof(*inode));
    inode->addr = geos_addr_make(gen, face, slot);
    strncpy(inode->name, name, GEOS_MAX_NAME - 1);
    inode->size_bytes = size_bytes;
    inode->block_start = block_start;
    inode->block_count = n_blocks;
    inode->created_kis_enc = frame_enc(v->inode_count);
    inode->accessed_kis_enc = inode->created_kis_enc;

    /* Compute entropy from data */
    if (data && size_bytes > 0) {
        uint8_t seen[256] = {0};
        for (uint32_t i = 0; i < size_bytes && i < n_blocks * GEOS_BLOCK_SZ; i++)
            seen[data[i]] = 1;
        uint16_t unique = 0;
        for (int i = 0; i < 256; i++) unique += seen[i];
        inode->entropy = (uint8_t)((unique * 255) / 256);
        inode->tier = adaptive_tier(inode->entropy);

        /* Write data to blocks */
        for (uint32_t b = 0; b < n_blocks; b++) {
            uint32_t offset = (block_start + b) * GEOS_BLOCK_SZ;
            uint32_t chunk = size_bytes - b * GEOS_BLOCK_SZ;
            if (chunk > GEOS_BLOCK_SZ) chunk = GEOS_BLOCK_SZ;
            memcpy(&v->data[offset], data + b * GEOS_BLOCK_SZ, chunk);
        }
    }

    v->inode_count++;
    v->n_files++;
    return inode;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_UNSUMMON — Remove data from coordinate
   ═══════════════════════════════════════════════════════════
   The ONLY way to remove data. Frees blocks at the coordinate.
   Coordinate becomes available for new summon.
   ═══════════════════════════════════════════════════════════ */

/* ── Internal: free an inode's blocks (hyper = scatter-walk free) ── */

static inline void _geos_free_inode_blocks(GeosVolume *v, GeosInode *in) {
    if (in->flags & GEOS_FLAG_HYPER) {
        HWRouter r; hw_init(&r, in->block_start, in->hyper_axis);
        for (uint32_t b = 0; b < in->block_count; b++) {
            uint32_t addr = hw_at(&r, b);
            _geos_bitmap_set(v, addr, 1, 0);
            _geos_freelist_insert(&v->free_list, addr, 1);
            v->total_blocks_used--;
            v->total_blocks_free++;
        }
        _geos_freelist_merge(&v->free_list);
    } else {
        geos_free_blocks(v, in->block_start, in->block_count);
    }
}

static inline int geos_unsummon(GeosVolume *v, const char *name) {
    if (!v || !name) return -1;
    for (uint16_t i = 0; i < v->inode_count; i++) {
        if (strcmp(v->inodes[i].name, name) == 0) {
            _geos_free_inode_blocks(v, &v->inodes[i]);
            if (i < v->inode_count - 1) {
                memmove(&v->inodes[i], &v->inodes[i + 1],
                        sizeof(GeosInode) * (v->inode_count - 1 - i));
            }
            v->inode_count--;
            v->n_files--;
            return 0;
        }
    }
    return -2;  /* not found */
}

/* ═══════════════════════════════════════════════════════════
   GEOS_HYPER — hyperbolic key-frame files (§2026-08-21)
   ═══════════════════════════════════════════════════════════
   A hyper file's block addresses are COMPUTED from ONE key frame
   (the seed) by the deterministic centroid walk:

       address(block b) = hw_at(&(HWRouter){seed, axis}, b)
                        = (seed + stride[axis]·b) mod 20736

   MAP not COMPRESS: the block list is never stored — geometry IS the
   address map. The walk is a bijection (odd stride, coprime with the
   field) so the placement never collides within the file, and reading
   back is lossless by construction. Key frame = a few bytes.

   - enter anywhere: any seed works (the walk covers the whole field)
   - scatter: stride 9/81 places blocks across the field, not linearly
   - axis 0 (stride 1) degenerates to the linear GeoFS layout
   - f(step): block b's address IS f(b) — deterministic, O(1)
   ═══════════════════════════════════════════════════════════ */

/* address of block `b` in a hyper file — pure computation, no lookup */
static inline uint32_t geos_hyper_address(GeosVolume *v, const char *name,
                                          uint32_t b) {
    if (!v || !name) return 0xFFFFFFFFu;
    GeosInode *in = geos_find(v, name);
    if (!in || !(in->flags & GEOS_FLAG_HYPER) || b >= in->block_count)
        return 0xFFFFFFFFu;
    HWRouter r; hw_init(&r, in->block_start, in->hyper_axis);
    return hw_at(&r, b);
}

/* place a hyper file: walk (seed, axis) and reserve + write each
 * address. Fails atomically on any collision / volume-header hit. */
static inline GeosInode* geos_hyper_place(GeosVolume *v,
                                           const char *name,
                                           uint32_t size_bytes,
                                           const uint8_t *data,
                                           uint32_t seed, uint32_t axis) {
    if (!v || !name || v->inode_count >= GEOS_MAX_INODES) return NULL;

    uint16_t n_blocks = (uint16_t)((size_bytes + GEOS_BLOCK_SZ - 1) / GEOS_BLOCK_SZ);
    if (n_blocks == 0) n_blocks = 1;
    if (n_blocks > hw_round_len(axis % HW_AXES)) return NULL;  /* no wrap */
    if ((uint32_t)n_blocks > v->total_blocks_free) return NULL;

    HWRouter r; hw_init(&r, seed, axis);
    for (uint32_t b = 0; b < n_blocks; b++) {
        uint32_t addr = hw_at(&r, b);
        if (addr < GEOS_VOL_DATA_START) return NULL;              /* volume blocks */
        if (v->block_map[addr / 8] & (1u << (addr % 8))) return NULL;  /* in use */
    }

    for (uint32_t b = 0; b < n_blocks; b++) {
        uint32_t addr = hw_at(&r, b);
        _geos_bitmap_set(v, addr, 1, 1);
        _geos_freelist_remove_addr(&v->free_list, addr);
        v->total_blocks_used++;
        v->total_blocks_free--;
        if (data) {
            uint32_t off = addr * GEOS_BLOCK_SZ;
            uint32_t chunk = size_bytes - b * GEOS_BLOCK_SZ;
            if (chunk > GEOS_BLOCK_SZ) chunk = GEOS_BLOCK_SZ;
            memcpy(&v->data[off], data + b * GEOS_BLOCK_SZ, chunk);
        }
    }

    GeosInode *inode = &v->inodes[v->inode_count];
    memset(inode, 0, sizeof(*inode));
    inode->addr = geos_addr_from_flat(seed);
    strncpy(inode->name, name, GEOS_MAX_NAME - 1);
    inode->size_bytes = size_bytes;
    inode->block_start = seed;          /* the key frame */
    inode->block_count = n_blocks;
    inode->hyper_axis = (uint8_t)(axis % HW_AXES);
    inode->flags |= GEOS_FLAG_HYPER;
    inode->created_kis_enc = frame_enc(v->inode_count);
    inode->accessed_kis_enc = inode->created_kis_enc;

    if (data && size_bytes > 0) {
        uint8_t seen[256] = {0};
        for (uint32_t i = 0; i < size_bytes && i < n_blocks * GEOS_BLOCK_SZ; i++)
            seen[data[i]] = 1;
        uint16_t unique = 0;
        for (int i = 0; i < 256; i++) unique += seen[i];
        inode->entropy = (uint8_t)((unique * 255) / 256);
        inode->tier = adaptive_tier(inode->entropy);
    }

    v->inode_count++;
    v->n_files++;
    return inode;
}

/* read a hyper file back by walking from its key frame — lossless
 * because the walk is a bijection (addresses recomputed, not stored) */
static inline int geos_hyper_read(GeosVolume *v, const char *name,
                                  uint8_t *buf, uint32_t buf_size) {
    GeosInode *in = geos_find(v, name);
    if (!in || !buf || !(in->flags & GEOS_FLAG_HYPER)) return -1;

    uint32_t bytes = (buf_size < in->size_bytes) ? buf_size : in->size_bytes;
    HWRouter r; hw_init(&r, in->block_start, in->hyper_axis);
    for (uint32_t b = 0; b < in->block_count && b * GEOS_BLOCK_SZ < bytes; b++) {
        uint32_t addr = hw_at(&r, b);
        uint32_t off = b * GEOS_BLOCK_SZ;
        uint32_t n = bytes - off; if (n > GEOS_BLOCK_SZ) n = GEOS_BLOCK_SZ;
        memcpy(buf + off, &v->data[addr * GEOS_BLOCK_SZ], n);
    }
    return (int)bytes;
}

/* zero-copy project of one hyper block (computed address → store ptr) */
static inline const uint8_t* geos_hyper_project_block(GeosVolume *v,
                                                      const char *name,
                                                      uint32_t b) {
    uint32_t addr = geos_hyper_address(v, name, b);
    if (addr == 0xFFFFFFFFu) return NULL;
    return &v->data[addr * GEOS_BLOCK_SZ];
}

/* ═══════════════════════════════════════════════════════════
   GEOS_BIJECTION — Twin bijection mapping
   ═══════════════════════════════════════════════════════════
   Maps between flat block address and 3D geometric coordinate.
   This IS the "Aquarium glass" — you view through this mapping.
   Zero-copy: returns pointer directly into data store.
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t block_flat;     /* flat block index in data store */
    uint32_t byte_offset;   /* byte offset = block_flat * 64 */
    uint8_t  gen;            /* generation */
    uint8_t  face;           /* face */
    uint16_t slot;           /* slot */
    uint8_t  cell_type;      /* 3-bit parity type */
} GeosBijection;

/* Forward: 3D coordinate → flat address */
static inline GeosBijection geos_bijection_forward(uint8_t gen, uint8_t face, uint16_t slot) {
    GeosBijection b;
    /* Reconstruct flat from bit-packed coordinate (inverse of geo_cell_addr_from_offset) */
    uint32_t flat = ((uint32_t)gen << CELL_GEN_SHIFT)
                  | ((uint32_t)face << CELL_FACE_SHIFT)
                  | ((uint32_t)slot << CELL_SLOT_SHIFT);
    /* NO mask, NO mod — valid (gen,face,slot) max = 16383 < 20736 */
    if (flat >= GEOS_ADDR_SPACE) { memset(&b, 0, sizeof(b)); return b; }
    b.block_flat  = flat;
    b.byte_offset = flat * GEOS_BLOCK_SZ;
    b.gen         = gen;
    b.face        = face;
    b.slot        = slot;
    GeoCellAddr ca = geo_cell_addr_from_offset(flat);
    b.cell_type   = ca.cell_type;
    return b;
}

/* Reverse: flat address → 3D coordinate */
static inline GeosBijection geos_bijection_reverse(uint32_t flat_id) {
    GeosBijection b;
    GeoCellAddr ca = geo_cell_addr_from_offset(flat_id);
    b.block_flat  = flat_id % GEOS_ADDR_SPACE;
    b.byte_offset = b.block_flat * GEOS_BLOCK_SZ;
    b.gen         = ca.generation;
    b.face        = ca.face;
    b.slot        = ca.slot;
    b.cell_type   = ca.cell_type;
    return b;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_PROJECT — Zero-copy frame read (Aquarium principle)
   ═══════════════════════════════════════════════════════════
   "Look through the glass" — returns pointer to data at coordinate.
   No decompress, no decode, no copy. The data IS at that coordinate.
   Like photographing fish: view from outside, never enter.
   ═══════════════════════════════════════════════════════════ */

/* Project: get direct pointer to block data at coordinate (zero-copy) */
static inline const uint8_t* geos_project(GeosVolume *v,
                                           uint8_t gen, uint8_t face, uint16_t slot) {
    if (!v || !v->data) return NULL;

    GeosBijection b = geos_bijection_forward(gen, face, slot);
    if (b.block_flat >= GEOS_ADDR_SPACE) return NULL;

    return &v->data[b.byte_offset];
}

/* Project by name: find file, return pointer to its data (zero-copy) */
static inline const uint8_t* geos_project_by_name(GeosVolume *v, const char *name,
                                                    uint32_t *out_size) {
    if (!v || !name) return NULL;

    GeosInode *inode = geos_find(v, name);
    if (!inode) return NULL;

    if (out_size) *out_size = inode->size_bytes;
    return &v->data[inode->block_start * GEOS_BLOCK_SZ];
}

/* Project block: get pointer to a specific block within a file */
static inline const uint8_t* geos_project_block(GeosVolume *v, const char *name,
                                                  uint32_t block_idx) {
    if (!v || !name) return NULL;

    GeosInode *inode = geos_find(v, name);
    if (!inode || block_idx >= inode->block_count) return NULL;
    if (inode->flags & GEOS_FLAG_HYPER)
        return geos_hyper_project_block(v, name, block_idx);

    return &v->data[(inode->block_start + block_idx) * GEOS_BLOCK_SZ];
}

/* ═══════════════════════════════════════════════════════════
   GEOS_READ_GHOST — §15.73/15.74: read ผ่าน ghost/hyperbolic path
   placement/admission/read ใช้ CAP_RULE_* เดียวกัน end-to-end:
   block → scale = cap_rule_scale(block) (กฎ trained เดียว) →
   ghost_read_rule_walk (§15.78 — เดินนาฬิกาจาก state ไปตำแหน่ง live ของ
   block แล้ว thaw ผ่าน bond — แทน direct pile lookup ใน log;
   delta self-describing → decode; raw → memcpy)
   — block ที่ถูก lift (freeze ใน residual_space) materialize จาก ghost
   — block ที่ admit (อยู่ใน field store) fallback อ่านจาก data store
   — lossless ทั้งสองกรณี; ใช้กฎเดียวกันกับตอนวางเสมอ
   — state ใน volume เดินหน้าเรื่อยๆ (อ่าน file = เดินผ่านตำแหน่ง live
     ของทุก block) · steps สะสมใน v->walk_steps (หลักฐาน walk) */
static inline int geos_read_ghost(GeosVolume *v, GhostLog *log,
                                  ResidualSpace *rs, const char *name,
                                  uint8_t *buf, uint32_t buf_size) {
    if (!v || !log || !rs || !buf) return -1;
    GeosInode *inode = geos_find(v, name);
    if (!inode) return -2;

    uint32_t bytes = (buf_size < inode->size_bytes) ? buf_size : inode->size_bytes;
    for (uint32_t b = 0; b < inode->block_count && b * GEOS_BLOCK_SZ < bytes; b++) {
        uint32_t block = inode->block_start + b;   /* geos flat block id */
        uint32_t off = b * GEOS_BLOCK_SZ;
        uint32_t n = bytes - off; if (n > GEOS_BLOCK_SZ) n = GEOS_BLOCK_SZ;
        uint32_t got = 0, steps = 0;
        int r = ghost_read_rule_walk(log, rs, (uint16_t)block, 0,
                                     (uint8_t)v->walk_round, (uint8_t)v->walk_tick,
                                     buf + off, n, &got, &steps);
        /* นาฬิกาเดินหน้าไปตำแหน่ง live ของ block ที่อ่าน (stateful walk) */
        v->walk_steps += steps;
        uint8_t to = cap_rule_scale(block);
        v->walk_round = to;
        v->walk_tick  = to % 12u;
        if (r != 0 || got < n)                     /* admit → store */
            memcpy(buf + off, &v->data[block * GEOS_BLOCK_SZ], n);
    }
    return (int)bytes;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_STAT — File status
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    char     name[GEOS_MAX_NAME];
    uint32_t size_bytes;
    uint16_t block_count;
    uint8_t  generation;
    uint8_t  face;
    uint16_t slot;
    uint8_t  cell_type;
    uint8_t  tier;
    uint8_t  entropy;
    uint8_t  flags;
    uint8_t  hyper_axis;
    uint16_t pipe_id;
    uint8_t  tick;
} GeosStat;

static inline int geos_stat(GeosVolume *v, const char *name, GeosStat *st) {
    GeosInode *inode = geos_find(v, name);
    if (!inode || !st) return -1;

    memset(st, 0, sizeof(*st));
    strncpy(st->name, inode->name, GEOS_MAX_NAME);
    st->size_bytes  = inode->size_bytes;
    st->block_count = inode->block_count;
    st->generation  = inode->addr.generation;
    st->face        = inode->addr.face;
    st->slot        = inode->addr.slot;
    st->cell_type   = inode->addr.cell_type;
    st->tier        = inode->tier;
    st->entropy     = inode->entropy;
    st->flags       = inode->flags;
    st->hyper_axis  = inode->hyper_axis;

    geo_cell_addr_offset_to_pipe(inode->block_start, &st->pipe_id, &st->tick);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_VISUALIZE — 144×144 ASCII grid
   ═══════════════════════════════════════════════════════════ */

static inline void geos_visualize(GeosVolume *v, const char *highlight_name) {
    if (!v) return;

    printf("\n=== GeoFS Volume: %s (%u/%u blocks) ===\n",
           v->vol_name, v->total_blocks_used, GEOS_ADDR_SPACE);

    /* Build display grid */
    char grid[144][145];
    for (int r = 0; r < 144; r++) {
        for (int c = 0; c < 144; c++) grid[r][c] = '.';
        grid[r][144] = '\0';
    }

    /* Mark volume header region */
    for (uint32_t i = 0; i < GEOS_VOL_DATA_START; i++) {
        uint32_t row = i / 144, col = i % 144;
        grid[row][col] = (i < GEOS_VOL_HDR_BLOCKS) ? 'V' : 'D';
    }

    /* Mark used blocks */
    for (uint32_t i = GEOS_VOL_DATA_START; i < GEOS_ADDR_SPACE; i++) {
        if (v->block_map[i / 8] & (1u << (i % 8))) {
            uint32_t row = i / 144, col = i % 144;
            if (grid[row][col] == '.') grid[row][col] = '#';
        }
    }

    /* Highlight file */
    if (highlight_name) {
        GeosInode *inode = geos_find(v, highlight_name);
        if (inode) {
            char m = '@';
            int hyper = (inode->flags & GEOS_FLAG_HYPER) != 0;
            HWRouter hr; if (hyper) hw_init(&hr, inode->block_start, inode->hyper_axis);
            for (uint16_t b = 0; b < inode->block_count; b++) {
                uint32_t flat = hyper ? hw_at(&hr, b) : (inode->block_start + b);
                uint32_t row = flat / 144, col = flat % 144;
                grid[row][col] = m;
            }
        }
    }

    /* Print with face separators */
    for (uint32_t row = 0; row < 144; row++) {
        for (uint32_t col = 0; col < 144; col++) {
            putchar(grid[row][col]);
            if ((col + 1) % 24 == 0 && col < 143) putchar('|');
        }
        putchar('\n');
    }

    /* Geometric summary */
    printf("\n  File               gen  face  slot  cell   tier  entropy  blocks\n");
    printf("  ─────────────────  ───  ────  ────  ────   ────  ───────  ──────\n");
    for (uint16_t i = 0; i < v->inode_count; i++) {
        GeosInode *inode = &v->inodes[i];
        GeoCellAddr ca = geo_cell_addr_from_offset(inode->block_start);
        printf("  %-20s  %2u   %2u    %3u  [%s]  %2u    %3u      %3u%s\n",
               inode->name,
               ca.generation, ca.face, ca.slot,
               cell_type_name(ca.cell_type),
               inode->tier, inode->entropy, inode->block_count,
               "");
    }
}

/* ═══════════════════════════════════════════════════════════
   GEOS_SERIALIZE — Write volume to .geofs file
   ═══════════════════════════════════════════════════════════ */

static inline int geos_serialize(GeosVolume *v, const char *path) {
    if (!v || !path) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -2;

    /* Write volume header */
    fwrite(v->magic, 1, 4, f);
    fwrite(&v->version, 1, 1, f);
    fwrite(&v->flags, 1, 1, f);
    fwrite(&v->n_files, 2, 1, f);
    fwrite(&v->n_dirs, 2, 1, f);
    fwrite(&v->total_blocks_used, 4, 1, f);
    fwrite(&v->total_blocks_free, 4, 1, f);
    fwrite(&v->volume_crc64, 8, 1, f);
    fwrite(v->vol_name, 1, 32, f);

    /* Write inode count */
    fwrite(&v->inode_count, 2, 1, f);

    /* Write inodes */
    for (uint16_t i = 0; i < v->inode_count; i++) {
        fwrite(&v->inodes[i], sizeof(GeosInode), 1, f);
    }

    /* Write data blocks (only used blocks, compressed) */
    for (uint16_t i = 0; i < v->inode_count; i++) {
        GeosInode *inode = &v->inodes[i];
        int hyper = (inode->flags & GEOS_FLAG_HYPER) != 0;
        HWRouter hr; if (hyper) hw_init(&hr, inode->block_start, inode->hyper_axis);
        for (uint16_t b = 0; b < inode->block_count; b++) {
            uint32_t flat = hyper ? hw_at(&hr, b) : (inode->block_start + b);
            uint32_t offset = flat * GEOS_BLOCK_SZ;
            fwrite(&v->data[offset], GEOS_BLOCK_SZ, 1, f);
        }
    }

    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_DESERIALIZE — Read volume from .geofs file
   ═══════════════════════════════════════════════════════════ */

static inline int geos_deserialize(GeosVolume *v, const char *path) {
    if (!v || !path) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -2;

    /* Allocate data store if not already */
    if (!v->data) v->data = (uint8_t *)calloc(1, GEOS_DATA_STORE_SIZE);

    /* Read volume header */
    char magic[4];
    fread(magic, 1, 4, f);
    if (memcmp(magic, GEOS_MAGIC, 4) != 0) { fclose(f); return -3; }

    fread(&v->version, 1, 1, f);
    fread(&v->flags, 1, 1, f);
    fread(&v->n_files, 2, 1, f);
    fread(&v->n_dirs, 2, 1, f);
    fread(&v->total_blocks_used, 4, 1, f);
    fread(&v->total_blocks_free, 4, 1, f);
    fread(&v->volume_crc64, 8, 1, f);
    fread(v->vol_name, 1, 32, f);
    memcpy(v->magic, magic, 4);

    /* Read inode count */
    fread(&v->inode_count, 2, 1, f);

    /* Read inodes */
    for (uint16_t i = 0; i < v->inode_count; i++) {
        fread(&v->inodes[i], sizeof(GeosInode), 1, f);
    }

    /* Rebuild block map */
    memset(v->block_map, 0, sizeof(v->block_map));
    for (uint32_t i = 0; i < GEOS_VOL_DATA_START; i++)
        v->block_map[i / 8] |= (1u << (i % 8));

    /* Rebuild free list + read data blocks */
    v->free_list.count = 0;

    for (uint16_t i = 0; i < v->inode_count; i++) {
        GeosInode *in = &v->inodes[i];
        int hyper = (in->flags & GEOS_FLAG_HYPER) != 0;
        HWRouter hr; if (hyper) hw_init(&hr, in->block_start, in->hyper_axis);
        for (uint16_t b = 0; b < in->block_count; b++) {
            uint32_t flat = hyper ? hw_at(&hr, b) : (in->block_start + b);
            v->block_map[flat / 8] |= (1u << (flat % 8));
            /* Read data block from file */
            uint32_t offset = flat * GEOS_BLOCK_SZ;
            fread(&v->data[offset], GEOS_BLOCK_SZ, 1, f);
        }
    }

    /* Rebuild free list from block map gaps */
    {
        uint32_t range_start = GEOS_VOL_DATA_START;
        for (uint32_t i = GEOS_VOL_DATA_START; i <= GEOS_ADDR_SPACE; i++) {
            int used = (i < GEOS_ADDR_SPACE) ?
                       (v->block_map[i / 8] & (1u << (i % 8))) : 1;
            if (used) {
                if (i > range_start) {
                    _geos_freelist_insert(&v->free_list, range_start, i - range_start);
                }
                range_start = i + 1;
            }
        }
    }

    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
   DIRECTORY SUPPORT
   ═══════════════════════════════════════════════════════════ */

#define GEOS_MAX_DIRS  256u
#define GEOS_MAX_ENTRIES_PER_DIR 64u

typedef struct {
    char       name[GEOS_MAX_NAME];
    uint32_t   inode_idx;      /* index into volume inodes[] */
    uint32_t   dir_parent;     /* parent dir index (0xFFFFFFFF = root) */
    uint8_t    is_dir;         /* 1 = directory, 0 = file */
    uint8_t    depth;          /* nesting depth (root = 0) */
} GeosDirEntry;

typedef struct {
    GeosDirEntry entries[GEOS_MAX_ENTRIES_PER_DIR];
    uint16_t     count;
    char         name[GEOS_MAX_NAME];
    uint32_t     parent_idx;    /* parent dir index */
} GeosDir;

/* Directory table inside volume */
#define GEOS_MAX_DIR_PATH  256u

typedef struct {
    GeosDir dirs[GEOS_MAX_DIRS];
    uint16_t dir_count;
    char     cwd[GEOS_MAX_DIR_PATH];  /* current working directory */
} GeosDirTable;

/* ── Create directory ─────────────────────────────────────────── */

static inline int geos_mkdir(GeosVolume *v, GeosDirTable *dt,
                              const char *name, uint32_t parent_idx) {
    if (!v || !dt || !name) return -1;
    if (dt->dir_count >= GEOS_MAX_DIRS) return -2;

    /* Check name collision in parent */
    GeosDir *parent = (parent_idx < dt->dir_count) ? &dt->dirs[parent_idx] : NULL;
    if (parent) {
        for (uint16_t i = 0; i < parent->count; i++) {
            if (strcmp(parent->entries[i].name, name) == 0) return -3;
        }
    }

    GeosDir *dir = &dt->dirs[dt->dir_count];
    memset(dir, 0, sizeof(*dir));
    strncpy(dir->name, name, GEOS_MAX_NAME - 1);
    dir->parent_idx = parent_idx;
    dir->count = 0;

    /* Add entry to parent */
    if (parent && parent->count < GEOS_MAX_ENTRIES_PER_DIR) {
        GeosDirEntry *e = &parent->entries[parent->count];
        strncpy(e->name, name, GEOS_MAX_NAME - 1);
        e->inode_idx = 0xFFFFFFFF;  /* dirs don't have inodes */
        e->dir_parent = parent_idx;
        e->is_dir = 1;
        e->depth = parent->entries[0].depth + 1;
        parent->count++;
    }

    dt->dir_count++;
    return 0;
}

/* ── List directory contents ──────────────────────────────────── */

static inline int geos_ls(GeosVolume *v, GeosDirTable *dt,
                           uint32_t dir_idx, const char **names,
                           int *is_dirs, int max_entries) {
    if (!dt || dir_idx >= dt->dir_count) return -1;

    GeosDir *dir = &dt->dirs[dir_idx];
    int n = 0;

    /* List subdirectories */
    for (uint16_t i = 0; i < dir->count && n < max_entries; i++) {
        if (dir->entries[i].is_dir) {
            if (names) names[n] = dir->entries[i].name;
            if (is_dirs) is_dirs[n] = 1;
            n++;
        }
    }

    /* List files */
    for (uint16_t i = 0; i < v->inode_count && n < max_entries; i++) {
        /* Simple: check if file's parent matches this dir */
        /* (In production, we'd use parent_addr) */
        if (v->inodes[i].parent_addr == dir_idx) {
            if (names) names[n] = v->inodes[i].name;
            if (is_dirs) is_dirs[n] = 0;
            n++;
        }
    }

    return n;
}

/* ── Path resolution (simplified: "dir/file" → inode index) ──── */

static inline int geos_path_resolve(GeosVolume *v, GeosDirTable *dt,
                                     const char *path, uint32_t *out_inode_idx) {
    if (!v || !dt || !path || !out_inode_idx) return -1;

    /* Root path */
    if (strcmp(path, "/") == 0 || strcmp(path, ".") == 0) {
        *out_inode_idx = 0xFFFFFFFF;
        return 0;
    }

    /* Find file by name (simplified: no full path walk yet) */
    GeosInode *inode = geos_find(v, path);
    if (inode) {
        /* Find inode index */
        for (uint16_t i = 0; i < v->inode_count; i++) {
            if (&v->inodes[i] == inode) {
                *out_inode_idx = i;
                return 0;
            }
        }
    }

    return -2;  /* not found */
}

/* ═══════════════════════════════════════════════════════════
   VORONOI-INTEGRATED ACCESS
   ═══════════════════════════════════════════════════════════
   Access file data through Voronoi cell cache.
   If cell is hot → direct access (O(1)).
   If cell is cold → collapse + re-insert.
   ═══════════════════════════════════════════════════════════ */

static inline VoronoiCell* geos_voronoi_access(GeosVolume *v,
                                                 VoronoiCache *vc,
                                                 const char *name,
                                                 uint32_t *out_offset,
                                                 uint32_t *out_size) {
    if (!v || !vc || !name) return NULL;

    GeosInode *inode = geos_find(v, name);
    if (!inode) return NULL;

    /* Look up in Voronoi cache */
    VoronoiCell *cell = voronoi_lookup(vc, inode->block_start);
    if (cell) {
        /* Cache hit — direct access */
        if (out_offset) *out_offset = cell->data_offset;
        if (out_size)   *out_size   = cell->data_size;
        return cell;
    }

    /* Cache miss — insert with entropy tier */
    cell = voronoi_insert(vc, inode->block_start,
                          inode->tier,
                          inode->block_start * GEOS_BLOCK_SZ,
                          inode->block_count * GEOS_BLOCK_SZ);
    if (out_offset && cell) *out_offset = cell->data_offset;
    if (out_size && cell)   *out_size   = cell->data_size;
    return cell;
}

/* ═══════════════════════════════════════════════════════════
   GEOS_DIR_TABLE INIT
   ═══════════════════════════════════════════════════════════ */

static inline void geos_dir_table_init(GeosDirTable *dt) {
    if (!dt) return;
    memset(dt, 0, sizeof(*dt));
    strcpy(dt->cwd, "/");
    /* Create root directory */
    GeosDir *root = &dt->dirs[0];
    strcpy(root->name, "/");
    root->parent_idx = 0xFFFFFFFF;
    root->count = 0;
    dt->dir_count = 1;
}

#endif /* GEO_GEOFS_H */
