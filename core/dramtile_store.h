#pragma once
#ifndef DRAMTILE_STORE_H
#define DRAMTILE_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #ifndef _GNU_SOURCE
  #define _GNU_SOURCE
  #endif
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

#ifdef _WIN32
static inline int dt_enable_lock_privilege(void) {
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return -1;
    if (!LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &luid)) {
        CloseHandle(hToken); return -1;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        CloseHandle(hToken); return -1;
    }
    CloseHandle(hToken);
    return 0;
}
static inline int dt_lock_pages(void *addr, size_t size) {
    if (!addr || size == 0) return -1;
    return VirtualLock(addr, size) ? 0 : -1;
}
static inline int dt_unlock_pages(void *addr, size_t size) {
    if (!addr || size == 0) return -1;
    return VirtualUnlock(addr, size) ? 0 : -1;
}
#else
static inline int dt_enable_lock_privilege(void) { return 0; }
static inline int dt_lock_pages(void *addr, size_t size) {
    if (!addr || size == 0) return -1;
    return mlock(addr, size);
}
static inline int dt_unlock_pages(void *addr, size_t size) {
    if (!addr || size == 0) return -1;
    return munlock(addr, size);
}
#endif

#define DT_LOCKED_BASE     0x01u
#define DT_LOCKED_KV       0x02u
#define DT_LOCKED_COLD     0x04u

#include "geo_dram_tile.h"
#include "rdh_capture.h"

#define DT_HASH_SLOTS   512
#define DT_FILE_MAGIC   0x544E4957
#define DT_DIR_MAGIC    0x52494454
#define DT_DIR2_MAGIC   0x32494454
#define DT_FILE_VER     1
#define DT_MAX_PATH     260
#define DT_FILE_HDR_SZ  64
#define DT_MAX_NDIM     4
#define DT_NAME_MAX     256
#define DT_HASH_NAME    48
#define DT_DIR_ENTRY_SZ 48
#define DT_COLD_DEFAULT (256UL << 20)
#define DT_MAX_FREE     512

typedef enum {
    DT_F32 = 0,
    DT_F16 = 1,
    DT_I32 = 2,
    DT_I8  = 3,
    DT_Q40 = 4,
    DT_Q80 = 5,
} DtDataType;

typedef struct {
    uint32_t dram_addr;
    size_t   offset;
    size_t   size;
    char     name[DT_HASH_NAME];
    uint32_t cold_offset;
    uint32_t session_tick;
} DRamTileHashEntry;

typedef struct {
    uint8_t          *base;
    size_t            capacity;
    size_t            used;
    int               is_mmap;
    DRamTileHashEntry hash[DT_HASH_SLOTS];
    uint32_t          n_stored;
    char              filepath[DT_MAX_PATH];
    int               is_twin;
    size_t            weight_boundary;
    size_t            kv_capacity;
    size_t            kv_used;
    uint8_t          *kv_base;
    uint8_t          *cold_base;
    size_t            cold_capacity;
    size_t            cold_used;
    char              cold_filepath[DT_MAX_PATH];
    int               is_cold_twin;
    uint32_t          session_tick;
    void            (*evict_cb)(const char *name, void *user);
    void             *evict_user;
    size_t            free_offs[DT_MAX_FREE];
    size_t            free_sizes[DT_MAX_FREE];
    int               free_count;
#define DT_KV_FLAG    0x80000000u
#define DT_BOND_FLAG  0x40000000u
#define DT_DELTA_FLAG 0x20000000u
#define DT_FLAGS_MASK (DT_KV_FLAG | DT_BOND_FLAG | DT_DELTA_FLAG)
#define DT_COLD_BIT   0x80000000u
#ifdef _WIN32
    HANDLE            hColdFile;
    HANDLE            hColdMapping;
#else
    int               cold_fd;
#endif
#ifdef _WIN32
    HANDLE            hFile;
    HANDLE            hMapping;
#else
    int               fd;
#endif
    uint8_t           locked;
} DRamTileStore;

typedef struct {
    uint8_t  *data;
    size_t    offset;
    size_t    nbytes;
    uint32_t  dram_addr;
    uint32_t  dtype;
    int       ndim;
    uint32_t  shape[6];
    char      name[DT_NAME_MAX];
} DtTensorView;

typedef int (*DtTensorCallback)(DtTensorView *view, void *user);

typedef struct {
    uint32_t dram_addr;
    uint64_t offset;
    uint64_t size;
    uint32_t dtype;
    int32_t  ndim;
    uint32_t shape[DT_MAX_NDIM];
    uint32_t namelen;
} DtDirEntryV2;

static inline uint32_t dt_name_to_addr(const char *name) {
    if (!name || name[0] == '\0') return 0;
    static const RDHConfig tier0 = RDH_TIER0;
    uint32_t ring = 0, wedge = 0;
    if (name[0] == 'b' && name[1] == 'l' && name[2] == 'k' && name[3] == '.') {
        uint32_t layer = 0;
        const char *p = name + 4;
        while (*p >= '0' && *p <= '9') {
            layer = layer * 10 + (uint32_t)(*p - '0');
            p++;
        }
        ring = layer & 0x7Fu;
        if (*p == '.') p++;
        size_t tlen = strlen(p);
        if (tlen > 0) {
            int64_t flat = rdh_capture((const uint8_t*)p, tlen, &tier0);
            int64_t rr, ww, mm, uu;
            rdh_decompose(&tier0, flat, &rr, &ww, &mm, &uu);
            wedge = (uint32_t)(ww % DRAM_ANCHORS);
        }
    } else {
        size_t len = strlen(name);
        int64_t flat = rdh_capture((const uint8_t*)name, len, &tier0);
        int64_t rr, ww, mm, uu;
        rdh_decompose(&tier0, flat, &rr, &ww, &mm, &uu);
        ring = (uint32_t)((int64_t)(rr & 0x7F));
        wedge = (uint32_t)(ww % DRAM_ANCHORS);
    }
    uint32_t anchor = wedge;
    uint32_t x      = ring & 7u;
    uint32_t y      = (ring >> 3) & 7u;
    uint32_t l      = (ring >> 6) & 1u;
    return dram_addr(anchor, x, y, l);
}

static inline int dt_is_kv(DRamTileStore *store, uint32_t slot) {
    return (store->hash[slot].dram_addr & DT_KV_FLAG) != 0;
}

static inline uint8_t *dt_entry_ptr(DRamTileStore *store, uint32_t slot) {
    if (store->hash[slot].dram_addr & DT_KV_FLAG)
        return store->kv_base + store->hash[slot].offset;
    return store->base + store->hash[slot].offset;
}

static inline uint8_t *dt_routed_ptr(DRamTileStore *store, uint32_t slot) {
    uint32_t entry = store->hash[slot].dram_addr;
    if (entry & DT_BOND_FLAG) {
        if (entry & DT_DELTA_FLAG)
            return store->cold_base + store->hash[slot].cold_offset;
        return store->cold_base + store->hash[slot].cold_offset;
    }
    return dt_entry_ptr(store, slot);
}

static inline size_t dt_get_size(DRamTileStore *store, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t start = addr % DT_HASH_SLOTS;
    for (uint32_t i = 0; i < DT_HASH_SLOTS; i++) {
        uint32_t s = (start + i) % DT_HASH_SLOTS;
        const DRamTileHashEntry *en = &store->hash[s];
        if (en->size == 0 && en->dram_addr == 0) return 0;
        if (en->size > 0 && (en->dram_addr & ~DT_FLAGS_MASK) == addr)
            return en->size;
    }
    return 0;
}

/* ── declarations (implemented in dramtile_store.c) ── */
int dt_store_init(DRamTileStore *store, size_t min_bytes);
int dt_store_init_cold(DRamTileStore *store, size_t max_bytes);
void dt_cold_rebuild_used(DRamTileStore *store);
int dt_store_init_cold_twin(DRamTileStore *store, const char *filepath, size_t max_bytes);
uint8_t *dt_cold_alloc(DRamTileStore *store, size_t sz);
uint8_t *dt_put_addr(DRamTileStore *store, uint32_t dram_addr, const uint8_t *data, size_t sz);
void dt_free_clear(DRamTileStore *store);
int dt_free_add(DRamTileStore *store, size_t offset, size_t size);
size_t dt_free_take(DRamTileStore *store, size_t sz);
int dt_free(DRamTileStore *store, const char *name);
size_t dt_free_bytes(DRamTileStore *store);
uint8_t *dt_put(DRamTileStore *store, const char *name, const uint8_t *data, size_t sz);
uint8_t *kv_compose(DRamTileStore *store, uint32_t slot);
uint8_t *kv_delta_spill(DRamTileStore *store, uint32_t slot, const uint8_t *fresh_data, size_t sz);
uint8_t *kv_delta_compose_read(DRamTileStore *store, uint32_t slot, uint8_t *dst, size_t sz);
uint8_t *dt_put_kv(DRamTileStore *store, const char *name, const uint8_t *data, size_t sz);
uint8_t *dt_get(DRamTileStore *store, const char *name);
void dt_store_destroy(DRamTileStore *store);
int dt_store_save_dir(DRamTileStore *store);
int dt_store_save_dir_v2(DRamTileStore *store, DtTensorView *views, uint32_t n_views);
int dt_store_load_dir(DRamTileStore *store);
int dt_store_load_views(DRamTileStore *store, DtTensorView **out_views);
int dt_store_init_twin(DRamTileStore *store, const char *filepath, size_t max_bytes);
int dt_store_init_twin_dual(DRamTileStore *store, const char *filepath, size_t max_bytes, float weight_ratio);
int dt_store_sync(DRamTileStore *store, int async);
void dt_store_destroy_twin(DRamTileStore *store);
void dt_store_destroy_twinv(DRamTileStore *store);
uint8_t *dt_putv(DRamTileStore *store, const char *name, uint32_t dtype, int ndim, const uint32_t *shape, const uint8_t *data, size_t sz);
DtTensorView dt_getv(DRamTileStore *store, const char *name);
DtTensorView dt_view(DRamTileStore *store, const char *name, uint32_t dtype, int ndim, const uint32_t *shape);
int dt_store_foreach(DRamTileStore *store, DtTensorCallback callback, void *user);
size_t dt_store_total_bytes(DRamTileStore *store);
int dt_store_check_twin(const char *filepath);
DtTensorView dt_resolve(DRamTileStore *store, uint32_t dram_addr);
int dt_migrate_promote_one(DRamTileStore *store, int slot, size_t dir_reserve);
int dt_migrate_step(DRamTileStore *store, int max_entries, size_t dir_reserve, int promote_newest);
int dt_evict_step(DRamTileStore *store, int max_entries);
int dt_cold_make_room(DRamTileStore *store, size_t sz, int max_evict);

#endif /* DRAMTILE_STORE_H */