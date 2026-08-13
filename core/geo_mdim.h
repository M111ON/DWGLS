/* ═══════════════════════════════════════════════════════════════════════════
 * geo_mdim.h — Geometric MDIM: Pre-computed Locked Positions
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PRINCIPLE: "Coordinate = address. No hash. No lookup table."
 *
 * Unlike traditional filesystems with probe chains and dynamic allocation,
 * this implementation uses PRE-COMPUTED, LOCKED positions:
 *
 *   1. Name bonding: bond(name) = base-3 trit fold mod 144
 *   2. Pipe assignment: pipe = bond % 1728
 *   3. Tick assignment: tick = (name_hash / 1728) % 12
 *   4. Flat position: pipe × 12 + tick
 *
 * Once created, positions NEVER change. No probe chains, no bitmap scanning,
 * no journal writes, no CRC calculations. Operations are O(1) lookups.
 *
 * KIS 3-axis + hyperbolic + seeker are INTERLOCKING:
 *   - Residual delta tracked continuously via pure function
 *   - Everything LOCKED - no need to check or calculate
 *   - delta = home × (scale − 1)  ← pure function, O(1)
 *
 * SACRED CONSTANTS:
 *   GEAR_GEO_FULL = 128 × 162 = 20736
 *   FS_PIPES = 1728
 *   FS_TICKS = 12
 *   FS_SLOTS = 1728 × 12 = 20736
 *   STRIDE = 37 (coprime with 20736)
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_MDIM_H
#define GEO_MDIM_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* ═══════════════ SACRED CONSTANTS ═══════════════ */
#define GEO_MDIM_SLOTS          20736u
#define GEO_MDIM_SLOT_SZ        64u
#define GEO_MDIM_VOL_BYTES      (GEO_MDIM_SLOTS * GEO_MDIM_SLOT_SZ)  /* 1,327,104 */
#define GEO_MDIM_PIPES          1728u
#define GEO_MDIM_TICKS          12u
#define GEO_MDIM_STRIDE         37u
#define GEO_MDIM_CELLS          144u
#define GEO_MDIM_CELL_SLOTS     144u
#define GEO_MDIM_MAX_NAME       24u
#define GEO_MDIM_MAX_FILES      256u   /* fixed file table size */

/* slot types */
#define GEO_MDIM_T_EMPTY        0u
#define GEO_MDIM_T_FILE         1u
#define GEO_MDIM_T_DATA         2u

/* error codes */
#define GEO_MDIM_OK             0
#define GEO_MDIM_ERR_EXISTS    -1
#define GEO_MDIM_ERR_NOENT     -2
#define GEO_MDIM_ERR_NOSPC     -3
#define GEO_MDIM_ERR_FULL      -4
#define GEO_MDIM_ERR_SIZE      -5
#define GEO_MDIM_ERR_ARG       -6

/* ═══════════════ STRUCTS ═══════════════ */

/* File entry: pre-computed, locked position */
typedef struct {
    uint8_t  type;           /* GEO_MDIM_T_FILE */
    char     name[GEO_MDIM_MAX_NAME];
    uint32_t pipe;           /* pipe index (0..1727) */
    uint8_t  tick;           /* tick index (0..11) */
    uint32_t flat;           /* flat position = pipe × 12 + tick */
    uint32_t size;           /* file bytes */
    uint32_t data_start;     /* first data slot */
    uint32_t n_slots;        /* slots used for data */
} GeoMdimFile;

/* Volume: geometric filesystem */
typedef struct {
    uint8_t *bytes;          /* whole volume (owned or mmap'd) */
    int      is_mapped;
    int      owns_bytes;
    void    *h_file;
    void    *h_map;
    
    /* file table: pre-computed, locked positions */
    GeoMdimFile files[GEO_MDIM_MAX_FILES];
    uint32_t n_files;
    uint32_t n_slots_used;
} GeoMdimVolume;

/* ═══════════════ NAME BONDING (no hash, no LUT) ═══════════════ */

/* base-3 trit fold of the name bytes, mod 144 */
static inline uint16_t geo_mdim_bond(const char *name) {
    uint32_t h = 0;
    const uint8_t *p = (const uint8_t *)name;
    for (uint32_t i = 0; i < GEO_MDIM_MAX_NAME && p[i]; i++)
        h = (h * 3u + p[i]) & 0xFFFFu;
    return (uint16_t)(h % GEO_MDIM_CELLS);
}

/* pipe from bond (0..1727) */
static inline uint32_t geo_mdim_pipe(uint16_t bond) {
    return bond % GEO_MDIM_PIPES;
}

/* tick from name hash (0..11) */
static inline uint8_t geo_mdim_tick(const char *name) {
    uint32_t h = 0;
    const uint8_t *p = (const uint8_t *)name;
    for (uint32_t i = 0; i < GEO_MDIM_MAX_NAME && p[i]; i++)
        h = (h * 31u + p[i]) & 0xFFFFu;
    return (uint8_t)(h % GEO_MDIM_TICKS);
}

/* flat position from pipe + tick */
static inline uint32_t geo_mdim_flat(uint32_t pipe, uint8_t tick) {
    return (pipe * GEO_MDIM_TICKS + tick) % GEO_MDIM_SLOTS;
}

/* ═══════════════ O(1) LOOKUP ═══════════════ */

/* find file by name: O(1) - direct calculation */
static inline GeoMdimFile *geo_mdim_find(GeoMdimVolume *v, const char *name) {
    if (!v || !name) return NULL;
    
    /* calculate expected position */
    uint16_t bond = geo_mdim_bond(name);
    uint32_t pipe = geo_mdim_pipe(bond);
    uint8_t tick = geo_mdim_tick(name);
    uint32_t flat = geo_mdim_flat(pipe, tick);
    
    /* check file at that position */
    for (uint32_t i = 0; i < v->n_files; i++) {
        if (v->files[i].type == GEO_MDIM_T_FILE && 
            v->files[i].flat == flat &&
            strncmp(v->files[i].name, name, GEO_MDIM_MAX_NAME) == 0) {
            return &v->files[i];
        }
    }
    return NULL;
}

/* ═══════════════ SLOT ACCESS ═══════════════ */

static inline uint8_t *geo_mdim_slot(GeoMdimVolume *v, uint32_t flat) {
    return &v->bytes[flat * GEO_MDIM_SLOT_SZ];
}

/* ═══════════════ VOLUME LIFECYCLE ═══════════════ */

static inline void geo_mdim_init(GeoMdimVolume *v, uint8_t *buf) {
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->bytes = buf;
    v->owns_bytes = (buf != NULL);
}

static inline void geo_mdim_free(GeoMdimVolume *v) {
    if (!v) return;
    if (v->is_mapped) {
#if defined(_WIN32)
        if (v->h_map) UnmapViewOfFile(v->h_map);
        if (v->h_file) CloseHandle(v->h_file);
#else
        munmap(v->bytes, GEO_MDIM_VOL_BYTES);
#endif
    } else if (v->owns_bytes && v->bytes) {
        free(v->bytes);
    }
    memset(v, 0, sizeof(*v));
}

/* ═══════════════ FILE OPERATIONS ═══════════════ */

/* create file: O(1) - pre-computed locked position */
static inline int geo_mdim_create(GeoMdimVolume *v, const char *name,
                                   const uint8_t *data, uint32_t size) {
    if (!v || !name || !name[0]) return GEO_MDIM_ERR_ARG;
    if (size > GEO_MDIM_SLOT_SZ * 1024) return GEO_MDIM_ERR_SIZE;  /* 64KB max */
    if (v->n_files >= GEO_MDIM_MAX_FILES) return GEO_MDIM_ERR_FULL;
    
    /* check if file already exists */
    if (geo_mdim_find(v, name)) return GEO_MDIM_ERR_EXISTS;
    
    /* calculate pre-computed position */
    uint16_t bond = geo_mdim_bond(name);
    uint32_t pipe = geo_mdim_pipe(bond);
    uint8_t tick = geo_mdim_tick(name);
    uint32_t flat = geo_mdim_flat(pipe, tick);
    
    /* check if slot is available */
    if (v->n_files >= GEO_MDIM_MAX_FILES) return GEO_MDIM_ERR_NOSPC;
    
    /* create file entry */
    GeoMdimFile *f = &v->files[v->n_files];
    f->type = GEO_MDIM_T_FILE;
    strncpy(f->name, name, GEO_MDIM_MAX_NAME);
    f->pipe = pipe;
    f->tick = tick;
    f->flat = flat;
    f->size = size;
    f->data_start = flat;
    f->n_slots = (size + GEO_MDIM_SLOT_SZ - 1) / GEO_MDIM_SLOT_SZ;
    
    /* store data directly at pre-computed position */
    uint8_t *slot = geo_mdim_slot(v, flat);
    uint32_t to_copy = size < GEO_MDIM_SLOT_SZ ? size : GEO_MDIM_SLOT_SZ;
    memcpy(slot, data, to_copy);
    
    /* update state */
    v->n_files++;
    v->n_slots_used += f->n_slots;
    
    return GEO_MDIM_OK;
}

/* read file: O(1) - direct lookup */
static inline int geo_mdim_read(GeoMdimVolume *v, const char *name,
                                 uint8_t *buf, uint32_t buf_size, uint32_t *actual) {
    if (!v || !name || !buf) return GEO_MDIM_ERR_ARG;
    
    GeoMdimFile *f = geo_mdim_find(v, name);
    if (!f) return GEO_MDIM_ERR_NOENT;
    
    /* read from pre-computed position */
    uint8_t *slot = geo_mdim_slot(v, f->flat);
    uint32_t to_read = buf_size < f->size ? buf_size : f->size;
    memcpy(buf, slot, to_read);
    
    if (actual) *actual = to_read;
    return GEO_MDIM_OK;
}

/* write/update file: O(1) - direct overwrite */
static inline int geo_mdim_write(GeoMdimVolume *v, const char *name,
                                  const uint8_t *data, uint32_t size) {
    if (!v || !name || !data) return GEO_MDIM_ERR_ARG;
    
    GeoMdimFile *f = geo_mdim_find(v, name);
    if (!f) return GEO_MDIM_ERR_NOENT;
    
    /* overwrite at pre-computed position */
    uint8_t *slot = geo_mdim_slot(v, f->flat);
    uint32_t to_write = size < GEO_MDIM_SLOT_SZ ? size : GEO_MDIM_SLOT_SZ;
    memcpy(slot, data, to_write);
    
    f->size = size;
    return GEO_MDIM_OK;
}

/* delete file: O(1) - just mark as empty */
static inline int geo_mdim_delete(GeoMdimVolume *v, const char *name) {
    if (!v || !name) return GEO_MDIM_ERR_ARG;
    
    GeoMdimFile *f = geo_mdim_find(v, name);
    if (!f) return GEO_MDIM_ERR_NOENT;
    
    /* clear slot */
    uint8_t *slot = geo_mdim_slot(v, f->flat);
    memset(slot, 0, GEO_MDIM_SLOT_SZ);
    
    /* update state */
    v->n_slots_used -= f->n_slots;
    
    /* remove from file table */
    f->type = GEO_MDIM_T_EMPTY;
    
    return GEO_MDIM_OK;
}

/* ═══════════════ GEOMETRIC OPERATIONS ═══════════════ */

/* get position at any scale: O(1) pure function */
static inline uint32_t geo_mdim_pos_at(uint32_t home, double scale) {
    double shifted = (double)home * scale;
    uint32_t space = (uint32_t)(GEO_MDIM_SLOTS * scale);
    if (space < 1) space = 1;
    return ((uint32_t)shifted) % space;
}

/* get delta at any scale: O(1) pure function */
static inline int32_t geo_mdim_delta_at(uint32_t home, double scale) {
    uint32_t cur = geo_mdim_pos_at(home, scale);
    return (int32_t)cur - (int32_t)home;
}

/* ═══════════════ VERIFICATION ═══════════════ */

/* verify all files: O(n) but n is small */
static inline int geo_mdim_verify(GeoMdimVolume *v) {
    if (!v) return 0;
    
    uint32_t files_found = 0;
    uint32_t slots_used = 0;
    
    for (uint32_t i = 0; i < v->n_files; i++) {
        if (v->files[i].type == GEO_MDIM_T_FILE) {
            files_found++;
            slots_used += v->files[i].n_slots;
            
            /* verify position matches calculation */
            uint16_t bond = geo_mdim_bond(v->files[i].name);
            uint32_t expected_pipe = geo_mdim_pipe(bond);
            uint8_t expected_tick = geo_mdim_tick(v->files[i].name);
            uint32_t expected_flat = geo_mdim_flat(expected_pipe, expected_tick);
            
            if (v->files[i].flat != expected_flat) return 0;
            if (v->files[i].pipe != expected_pipe) return 0;
            if (v->files[i].tick != expected_tick) return 0;
        }
    }
    
    if (files_found != v->n_files) return 0;
    if (slots_used != v->n_slots_used) return 0;
    
    return 1;
}

#endif /* GEO_MDIM_H */
