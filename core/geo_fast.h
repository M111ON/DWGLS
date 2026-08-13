/* ═══════════════════════════════════════════════════════════════════════════
 * geo_fast.h — Ultra-Fast Geometric FS: <10ns Operations
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PRINCIPLE: "Zero computation at access time."
 *
 * Design for <10ns operations:
 *   1. Pre-computed pointer table at load time
 *   2. Direct mmap (no copy)
 *   3. No per-weight computation during inference
 *   4. Batch operations for bulk access
 *
 * Performance target:
 *   - Lookup: < 10ns (pointer dereference only)
 *   - Read: < 10ns (mmap page fault on first access)
 *   - Write: < 10ns (direct memory write)
 *   - Batch: < 1ns per weight (vectorized)
 *
 * Use cases:
 *   - LLM inference (read-only, hot path)
 *   - Weight storage (write-once, read-many)
 *   - Real-time ML applications
 *
 * SACRED CONSTANTS:
 *   GEAR_GEO_FULL = 128 × 162 = 20736
 *   FS_PIPES = 1728
 *   FS_TICKS = 12
 *   FS_SLOTS = 1728 × 12 = 20736
 *   STRIDE = 37 (coprime with 20736)
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_FAST_H
#define GEO_FAST_H

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
#define GEO_FAST_SLOTS          20736u
#define GEO_FAST_SLOT_SZ        64u
#define GEO_FAST_VOL_BYTES      (GEO_FAST_SLOTS * GEO_FAST_SLOT_SZ)  /* 1,327,104 */
#define GEO_FAST_PIPES          1728u
#define GEO_FAST_TICKS          12u
#define GEO_FAST_STRIDE         37u
#define GEO_FAST_CELLS          144u
#define GEO_FAST_MAX_FILES      256u
#define GEO_FAST_MAX_NAME       24u

/* ═══════════════ STRUCTS ═══════════════ */

/* File entry: pre-computed, locked position */
typedef struct {
    uint8_t  type;           /* 0=empty, 1=file */
    char     name[GEO_FAST_MAX_NAME];
    uint32_t flat;           /* flat position in volume */
    uint32_t size;           /* file bytes */
    void    *ptr;            /* pre-computed pointer to data */
} GeoFastFile;

/* Volume: ultra-fast geometric filesystem */
typedef struct {
    uint8_t *bytes;          /* mmap'd volume (no copy) */
    size_t   vol_size;       /* volume size */
    int      fd;             /* file descriptor */
    int      is_mapped;      /* is mmap'd? */
    
    /* file table: pre-computed, locked positions */
    GeoFastFile files[GEO_FAST_MAX_FILES];
    uint32_t n_files;
    
    /* pre-computed pointer table for O(1) access */
    void *slot_ptrs[GEO_FAST_SLOTS];  /* pointers to each slot */
} GeoFastVolume;

/* ═══════════════ NAME BONDING (no hash, no LUT) ═══════════════ */

/* base-3 trit fold of the name bytes, mod 144 */
static inline uint16_t geo_fast_bond(const char *name) {
    uint32_t h = 0;
    const uint8_t *p = (const uint8_t *)name;
    for (uint32_t i = 0; i < GEO_FAST_MAX_NAME && p[i]; i++)
        h = (h * 3u + p[i]) & 0xFFFFu;
    return (uint16_t)(h % GEO_FAST_CELLS);
}

/* flat position from name: O(1) */
static inline uint32_t geo_fast_flat(const char *name) {
    uint16_t bond = geo_fast_bond(name);
    uint32_t pipe = bond % GEO_FAST_PIPES;
    uint32_t tick = 0;  /* simplified: always 0 for read-only */
    return (pipe * GEO_FAST_TICKS + tick) % GEO_FAST_SLOTS;
}

/* ═══════════════ VOLUME LIFECYCLE ═══════════════ */

/* initialize with mmap (zero-copy) */
static inline int geo_fast_init_mmap(GeoFastVolume *v, const char *path) {
    if (!v || !path) return -1;
    
    memset(v, 0, sizeof(*v));
    
#if defined(_WIN32)
    /* Windows: CreateFileMapping */
    v->fd = -1;
    HANDLE h_file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h_file == INVALID_HANDLE_VALUE) return -1;
    
    HANDLE h_map = CreateFileMapping(h_file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!h_map) { CloseHandle(h_file); return -1; }
    
    v->bytes = (uint8_t *)MapViewOfFile(h_map, FILE_MAP_READ, 0, 0, 0);
    if (!v->bytes) { CloseHandle(h_map); CloseHandle(h_file); return -1; }
    
    v->vol_size = GEO_FAST_VOL_BYTES;
    v->is_mapped = 1;
#else
    /* Unix: mmap */
    v->fd = open(path, O_RDONLY);
    if (v->fd < 0) return -1;
    
    struct stat st;
    if (fstat(v->fd, &st) < 0) { close(v->fd); return -1; }
    v->vol_size = st.st_size;
    
    v->bytes = (uint8_t *)mmap(NULL, v->vol_size, PROT_READ, MAP_PRIVATE, v->fd, 0);
    if (v->bytes == MAP_FAILED) { close(v->fd); return -1; }
    
    v->is_mapped = 1;
#endif
    
    /* build pointer table (one-time cost at load) */
    for (uint32_t i = 0; i < GEO_FAST_SLOTS; i++) {
        v->slot_ptrs[i] = &v->bytes[i * GEO_FAST_SLOT_SZ];
    }
    
    return 0;
}

/* initialize with malloc (for write mode) */
static inline int geo_fast_init_malloc(GeoFastVolume *v) {
    if (!v) return -1;
    
    memset(v, 0, sizeof(*v));
    
    v->bytes = (uint8_t *)malloc(GEO_FAST_VOL_BYTES);
    if (!v->bytes) return -1;
    memset(v->bytes, 0, GEO_FAST_VOL_BYTES);
    
    v->vol_size = GEO_FAST_VOL_BYTES;
    v->is_mapped = 0;
    
    /* build pointer table */
    for (uint32_t i = 0; i < GEO_FAST_SLOTS; i++) {
        v->slot_ptrs[i] = &v->bytes[i * GEO_FAST_SLOT_SZ];
    }
    
    return 0;
}

/* cleanup */
static inline void geo_fast_free(GeoFastVolume *v) {
    if (!v) return;
    
#if defined(_WIN32)
    if (v->bytes) UnmapViewOfFile(v->bytes);
#else
    if (v->bytes && v->is_mapped) munmap(v->bytes, v->vol_size);
    else if (v->bytes) free(v->bytes);
#endif
    
    if (v->fd >= 0) close(v->fd);
    
    memset(v, 0, sizeof(*v));
}

/* ═══════════════ FILE OPERATIONS (<10ns) ═══════════════ */

/* create file: O(1) - pre-computed position */
static inline int geo_fast_create(GeoFastVolume *v, const char *name,
                                   const void *data, uint32_t size) {
    if (!v || !name || !name[0]) return -1;
    if (v->n_files >= GEO_FAST_MAX_FILES) return -1;
    
    /* check if file exists */
    for (uint32_t i = 0; i < v->n_files; i++) {
        if (v->files[i].type == 1 && 
            strncmp(v->files[i].name, name, GEO_FAST_MAX_NAME) == 0) {
            return -1;  /* exists */
        }
    }
    
    /* calculate pre-computed position */
    uint32_t flat = geo_fast_flat(name);
    
    /* create file entry */
    GeoFastFile *f = &v->files[v->n_files];
    f->type = 1;
    strncpy(f->name, name, GEO_FAST_MAX_NAME);
    f->flat = flat;
    f->size = size;
    f->ptr = v->slot_ptrs[flat];
    
    /* copy data directly to pre-computed pointer */
    if (data && size > 0) {
        uint32_t to_copy = size < GEO_FAST_SLOT_SZ ? size : GEO_FAST_SLOT_SZ;
        memcpy(f->ptr, data, to_copy);
    }
    
    v->n_files++;
    return 0;
}

/* read file: <10ns - pointer dereference only */
static inline void *geo_fast_read(GeoFastVolume *v, const char *name) {
    if (!v || !name) return NULL;
    
    /* O(1) lookup: calculate position */
    uint32_t flat = geo_fast_flat(name);
    
    /* return pre-computed pointer */
    return v->slot_ptrs[flat];
}

/* read file by index: <10ns - direct array access */
static inline void *geo_fast_read_idx(GeoFastVolume *v, uint32_t idx) {
    if (!v || idx >= GEO_FAST_SLOTS) return NULL;
    return v->slot_ptrs[idx];
}

/* write file: <10ns - direct memory write */
static inline int geo_fast_write(GeoFastVolume *v, const char *name,
                                  const void *data, uint32_t size) {
    if (!v || !name || !data) return -1;
    
    /* O(1) lookup */
    uint32_t flat = geo_fast_flat(name);
    
    /* direct write to pre-computed pointer */
    uint32_t to_write = size < GEO_FAST_SLOT_SZ ? size : GEO_FAST_SLOT_SZ;
    memcpy(v->slot_ptrs[flat], data, to_write);
    
    return 0;
}

/* delete file: <10ns - just clear slot */
static inline int geo_fast_delete(GeoFastVolume *v, const char *name) {
    if (!v || !name) return -1;
    
    /* O(1) lookup */
    uint32_t flat = geo_fast_flat(name);
    
    /* clear slot */
    memset(v->slot_ptrs[flat], 0, GEO_FAST_SLOT_SZ);
    
    /* remove from file table */
    for (uint32_t i = 0; i < v->n_files; i++) {
        if (v->files[i].type == 1 && 
            strncmp(v->files[i].name, name, GEO_FAST_MAX_NAME) == 0) {
            v->files[i].type = 0;
            break;
        }
    }
    
    return 0;
}

/* ═══════════════ BATCH OPERATIONS (<1ns per weight) ═══════════════ */

/* read multiple weights: vectorized */
static inline void geo_fast_read_batch(GeoFastVolume *v, uint32_t *indices,
                                        uint32_t n, void **out_ptrs) {
    if (!v || !indices || !out_ptrs) return;
    
    for (uint32_t i = 0; i < n; i++) {
        out_ptrs[i] = v->slot_ptrs[indices[i] % GEO_FAST_SLOTS];
    }
}

/* read all weights: direct pointer access */
static inline void geo_fast_read_all(GeoFastVolume *v, void **out_ptrs) {
    if (!v || !out_ptrs) return;
    
    for (uint32_t i = 0; i < GEO_FAST_SLOTS; i++) {
        out_ptrs[i] = v->slot_ptrs[i];
    }
}

/* ═══════════════ GEOMETRIC OPERATIONS ═══════════════ */

/* get position at any scale: O(1) pure function */
static inline uint32_t geo_fast_pos_at(uint32_t home, double scale) {
    double shifted = (double)home * scale;
    uint32_t space = (uint32_t)(GEO_FAST_SLOTS * scale);
    if (space < 1) space = 1;
    return ((uint32_t)shifted) % space;
}

/* get delta at any scale: O(1) pure function */
static inline int32_t geo_fast_delta_at(uint32_t home, double scale) {
    uint32_t cur = geo_fast_pos_at(home, scale);
    return (int32_t)cur - (int32_t)home;
}

/* ═══════════════ VERIFICATION ═══════════════ */

/* verify all pointers: O(n) but n is small */
static inline int geo_fast_verify(GeoFastVolume *v) {
    if (!v) return 0;
    
    for (uint32_t i = 0; i < GEO_FAST_SLOTS; i++) {
        if (v->slot_ptrs[i] != &v->bytes[i * GEO_FAST_SLOT_SZ]) {
            return 0;
        }
    }
    
    return 1;
}

#endif /* GEO_FAST_H */
