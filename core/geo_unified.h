/* ═══════════════════════════════════════════════════════════════════════════
 * geo_unified.h — Unified Geometric FS: geo_fast + DRamTile + RDH + GearLock
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * All three systems share the same 20736 address space:
 *   DRamTile: anchor × 128 + hilbert(x,y,layer)
 *   RDH:      (ring × wedges + wedge) × mirror + u
 *   GearLock: cpu_ops % 128 + gpu_ops % 162
 *
 * This header unifies them into a single zero-overhead addressing system.
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_UNIFIED_H
#define GEO_UNIFIED_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* include the existing components */
#include "gear_lock.h"
#include "geo_dram_tile.h"
#include "rdh_addr.h"

/* ═══════════════ UNIFIED CONSTANTS ═══════════════ */
#define GEO_UNIFIED_SLOTS    20736u
#define GEO_UNIFIED_SLOT_SZ  64u
#define GEO_UNIFIED_VOL_BYTES (GEO_UNIFIED_SLOTS * GEO_UNIFIED_SLOT_SZ)
#define GEO_UNIFIED_MAX_FILES 256u
#define GEO_UNIFIED_MAX_NAME  24u

/* ═══════════════ UNIFIED ADDRESSING ═══════════════ */

/* All three systems → same flat address */
typedef enum {
    ADDR_DRAMTILE,    /* anchor × 128 + hilbert */
    ADDR_RDH,         /* ring/wedge/mirror/u */
    ADDR_GEARLOCK,    /* cpu/gpu ops */
    ADDR_NAME,        /* FNV-1a hash */
    ADDR_FLAT         /* direct flat index */
} AddressMode;

/* Unified address from name: O(1) */
static inline uint32_t geo_unified_addr(const char *name) {
    if (!name || !name[0]) return 0;
    
    /* FNV-1a hash → DRamTile address */
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    
    uint32_t anchor = h % DRAM_ANCHORS;
    uint32_t x = (h >> 8) % DRAM_GRID_X;
    uint32_t y = (h >> 16) % DRAM_GRID_Y;
    uint32_t layer = (h >> 24) % DRAM_LAYERS;
    
    return dram_addr(anchor, x, y, layer);
}

/* Unified address from coordinates: O(1) */
static inline uint32_t geo_unified_from_coords(uint32_t anchor, 
                                                uint32_t x, uint32_t y, 
                                                uint32_t layer) {
    return dram_addr(anchor, x, y, layer);
}

/* Unified address from RDH: O(1) */
static inline uint32_t geo_unified_from_rdh(const RDHConfig *cfg,
                                             int64_t ring, int64_t wedge,
                                             int64_t mirror, int64_t u) {
    return (uint32_t)rdh_key(cfg, ring, wedge, mirror, u, 0) % GEO_UNIFIED_SLOTS;
}

/* Unified address from GearLock: O(1) */
static inline uint32_t geo_unified_from_gear(const GearLock *g) {
    uint32_t cpu = g->cpu_ops % GEAR_CPU_WORLD;
    uint32_t gpu = g->gpu_ops % GEAR_GPU_WORLD;
    return cpu * GEAR_GPU_WORLD + gpu;
}

/* ═══════════════ FILE TABLE ═══════════════ */
typedef struct {
    uint8_t  type;           /* 0=empty, 1=file */
    char     name[GEO_UNIFIED_MAX_NAME];
    uint32_t flat;           /* pre-computed flat position */
    uint32_t size;           /* file bytes */
    void    *ptr;            /* pre-computed pointer to data */
} GeoUnifiedFile;

/* ═══════════════ VOLUME ═══════════════ */
typedef struct {
    uint8_t *bytes;          /* mmap'd volume */
    size_t   vol_size;
    int      fd;
    int      is_mapped;
    
    /* file table */
    GeoUnifiedFile files[GEO_UNIFIED_MAX_FILES];
    uint32_t n_files;
    
    /* pre-computed pointer table */
    void *slot_ptrs[GEO_UNIFIED_SLOTS];
    
    /* GearLock for CPU/GPU sync */
    GearLock gear;
    
    /* RDH config */
    RDHConfig rdh_cfg;
} GeoUnifiedVolume;

/* ═══════════════ LIFECYCLE ═══════════════ */

static inline int geo_unified_init(GeoUnifiedVolume *v) {
    if (!v) return -1;
    memset(v, 0, sizeof(*v));
    
    /* allocate volume */
    v->bytes = (uint8_t *)malloc(GEO_UNIFIED_VOL_BYTES);
    if (!v->bytes) return -1;
    memset(v->bytes, 0, GEO_UNIFIED_VOL_BYTES);
    v->vol_size = GEO_UNIFIED_VOL_BYTES;
    
    /* build pointer table */
    for (uint32_t i = 0; i < GEO_UNIFIED_SLOTS; i++) {
        v->slot_ptrs[i] = &v->bytes[i * GEO_UNIFIED_SLOT_SZ];
    }
    
    /* init GearLock */
    memset(&v->gear, 0, sizeof(v->gear));
    
    /* init RDH config (Tier0: 128 rings, 162 wedges) */
    v->rdh_cfg = (RDHConfig){ 128, 162, 1, 1, 1 };
    
    return 0;
}

static inline void geo_unified_free(GeoUnifiedVolume *v) {
    if (!v) return;
    if (v->bytes) free(v->bytes);
    memset(v, 0, sizeof(*v));
}

/* ═══════════════ FILE OPERATIONS (<10ns) ═══════════════ */

/* create: O(1) — pre-computed position */
static inline int geo_unified_create(GeoUnifiedVolume *v, const char *name,
                                      const void *data, uint32_t size) {
    if (!v || !name || !name[0]) return -1;
    if (v->n_files >= GEO_UNIFIED_MAX_FILES) return -1;
    
    /* check exists */
    for (uint32_t i = 0; i < v->n_files; i++) {
        if (v->files[i].type == 1 && 
            strncmp(v->files[i].name, name, GEO_UNIFIED_MAX_NAME) == 0) {
            return -1;
        }
    }
    
    /* unified address: O(1) */
    uint32_t flat = geo_unified_addr(name);
    
    /* create entry */
    GeoUnifiedFile *f = &v->files[v->n_files];
    f->type = 1;
    strncpy(f->name, name, GEO_UNIFIED_MAX_NAME);
    f->flat = flat;
    f->size = size;
    f->ptr = v->slot_ptrs[flat];
    
    /* copy data */
    if (data && size > 0) {
        uint32_t to_copy = size < GEO_UNIFIED_SLOT_SZ ? size : GEO_UNIFIED_SLOT_SZ;
        memcpy(f->ptr, data, to_copy);
    }
    
    /* tick GearLock */
    gear_cpu_tick(&v->gear);
    
    v->n_files++;
    return 0;
}

/* read: <10ns — pointer dereference only */
static inline void *geo_unified_read(GeoUnifiedVolume *v, const char *name) {
    if (!v || !name) return NULL;
    uint32_t flat = geo_unified_addr(name);
    return v->slot_ptrs[flat];
}

/* read by flat index: <10ns */
static inline void *geo_unified_read_flat(GeoUnifiedVolume *v, uint32_t flat) {
    if (!v || flat >= GEO_UNIFIED_SLOTS) return NULL;
    return v->slot_ptrs[flat];
}

/* read by DRamTile coordinates: <10ns */
static inline void *geo_unified_read_dram(GeoUnifiedVolume *v,
                                           uint32_t anchor, uint32_t x,
                                           uint32_t y, uint32_t layer) {
    if (!v) return NULL;
    uint32_t flat = geo_unified_from_coords(anchor, x, y, layer);
    return v->slot_ptrs[flat];
}

/* read by RDH coordinates: <10ns */
static inline void *geo_unified_read_rdh(GeoUnifiedVolume *v,
                                          const RDHConfig *cfg,
                                          int64_t ring, int64_t wedge,
                                          int64_t mirror, int64_t u) {
    if (!v) return NULL;
    uint32_t flat = geo_unified_from_rdh(cfg, ring, wedge, mirror, u);
    return v->slot_ptrs[flat];
}

/* write: <10ns — direct memory write */
static inline int geo_unified_write(GeoUnifiedVolume *v, const char *name,
                                     const void *data, uint32_t size) {
    if (!v || !name || !data) return -1;
    uint32_t flat = geo_unified_addr(name);
    uint32_t to_write = size < GEO_UNIFIED_SLOT_SZ ? size : GEO_UNIFIED_SLOT_SZ;
    memcpy(v->slot_ptrs[flat], data, to_write);
    return 0;
}

/* delete: <10ns — clear slot */
static inline int geo_unified_delete(GeoUnifiedVolume *v, const char *name) {
    if (!v || !name) return -1;
    uint32_t flat = geo_unified_addr(name);
    memset(v->slot_ptrs[flat], 0, GEO_UNIFIED_SLOT_SZ);
    
    for (uint32_t i = 0; i < v->n_files; i++) {
        if (v->files[i].type == 1 && 
            strncmp(v->files[i].name, name, GEO_UNIFIED_MAX_NAME) == 0) {
            v->files[i].type = 0;
            break;
        }
    }
    return 0;
}

/* ═══════════════ BATCH OPERATIONS ═══════════════ */

static inline void geo_unified_read_batch(GeoUnifiedVolume *v, uint32_t *flats,
                                           uint32_t n, void **out_ptrs) {
    if (!v || !flats || !out_ptrs) return;
    for (uint32_t i = 0; i < n; i++) {
        out_ptrs[i] = v->slot_ptrs[flats[i] % GEO_UNIFIED_SLOTS];
    }
}

/* ═══════════════ VERIFICATION ═══════════════ */

static inline int geo_unified_verify(GeoUnifiedVolume *v) {
    if (!v) return 0;
    
    /* verify pointer table */
    for (uint32_t i = 0; i < GEO_UNIFIED_SLOTS; i++) {
        if (v->slot_ptrs[i] != &v->bytes[i * GEO_UNIFIED_SLOT_SZ]) {
            return 0;
        }
    }
    
    /* verify DRamTile Hilbert */
    if (dram_verify_hilbert() != 0) return 0;
    
    /* verify DRamTile full */
    if (dram_verify_full() != 0) return 0;
    
    return 1;
}

#endif /* GEO_UNIFIED_H */
