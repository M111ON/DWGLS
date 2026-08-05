#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

#include "geo_dram_tile.h"

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

static inline uint32_t dt_name_to_addr(const char *name) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t anchor = h % DRAM_ANCHORS;
    uint32_t x = (h >> 8) % DRAM_GRID_X;
    uint32_t y = (h >> 16) % DRAM_GRID_Y;
    uint32_t layer = (h >> 24) % DRAM_LAYERS;
    return dram_addr(anchor, x, y, layer);
}

static inline uint32_t dt_name_to_rdh(const char *name) {
    if (!name || name[0] == '\0') return 0;
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
        uint32_t th = 2166136261u;
        for (const char *q = p; *q; q++)
            th = (th ^ (uint8_t)*q) * 16777619u;
        wedge = th % DRAM_ANCHORS;
    } else {
        uint32_t h = 2166136261u;
        for (const char *p = name; *p; p++)
            h = (h ^ (uint8_t)*p) * 16777619u;
        wedge = h % DRAM_ANCHORS;
        ring = 0;
    }
    uint32_t anchor = wedge;
    uint32_t x      = ring & 7u;
    uint32_t y      = (ring >> 3) & 7u;
    uint32_t l      = (ring >> 6) & 1u;
    return dram_addr(anchor, x, y, l);
}

static inline int dt_store_init(DRamTileStore *store, size_t min_bytes) {
    memset(store, 0, sizeof(*store));
    size_t cap = min_bytes < 4UL * 1024 * 1024 * 1024
               ? 4UL * 1024 * 1024 * 1024
               : min_bytes;
#ifdef _WIN32
    store->base = (uint8_t*)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    store->is_mmap = (store->base != NULL);
#else
    store->base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    store->is_mmap = (store->base != MAP_FAILED);
    if (!store->is_mmap) store->base = NULL;
#endif
    if (!store->base) {
        store->base = (uint8_t*)malloc(cap);
        if (!store->base) return -1;
        store->is_mmap = 0;
    }
    store->capacity = cap;
    store->used = 0;
    return 0;
}

static inline int dt_store_init_cold(DRamTileStore *store, size_t max_bytes) {
    if (!store) return -1;
    size_t cap = max_bytes < DT_COLD_DEFAULT ? DT_COLD_DEFAULT : max_bytes;
#ifdef _WIN32
    store->cold_base = (uint8_t*)VirtualAlloc(NULL, cap,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    store->cold_base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (store->cold_base == MAP_FAILED) store->cold_base = NULL;
#endif
    if (!store->cold_base) return -1;
    store->cold_capacity = cap;
    store->cold_used = 0;
    return 0;
}

static inline void dt_cold_rebuild_used(DRamTileStore *store) {
    if (!store->cold_base) return;
    size_t max_off = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr & DT_BOND_FLAG) {
            size_t end = (size_t)store->hash[i].cold_offset + store->hash[i].size;
            if (end > max_off) max_off = end;
        }
    }
    store->cold_used = max_off;
}

static inline int dt_store_init_cold_twin(DRamTileStore *store,
                                           const char *filepath,
                                           size_t max_bytes)
{
    if (!store) return -1;
    size_t cap = max_bytes < DT_COLD_DEFAULT ? DT_COLD_DEFAULT : max_bytes;
    size_t existing = 0;
#ifdef _WIN32
    store->hColdFile = CreateFileA(filepath,
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (store->hColdFile == INVALID_HANDLE_VALUE) return -1;
    DWORD hi; DWORD lo = GetFileSize(store->hColdFile, &hi);
    existing = (uint64_t)hi << 32 | lo;
    if (existing == 0) {
        LARGE_INTEGER sz; sz.QuadPart = cap;
        SetFilePointerEx(store->hColdFile, sz, NULL, FILE_BEGIN);
        SetEndOfFile(store->hColdFile);
    }
    store->hColdMapping = CreateFileMappingA(store->hColdFile, NULL,
        PAGE_READWRITE, (DWORD)(cap >> 32), (DWORD)cap, NULL);
    if (!store->hColdMapping) { CloseHandle(store->hColdFile); return -1; }
    store->cold_base = (uint8_t*)MapViewOfFile(store->hColdMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, cap);
    if (!store->cold_base) {
        CloseHandle(store->hColdMapping); CloseHandle(store->hColdFile);
        return -1;
    }
#else
    store->cold_fd = open(filepath, O_RDWR | O_CREAT, 0644);
    if (store->cold_fd < 0) return -1;
    struct stat st; fstat(store->cold_fd, &st);
    existing = (size_t)st.st_size;
    if (existing == 0 && ftruncate(store->cold_fd, cap) != 0) {
        close(store->cold_fd); return -1;
    }
    if (existing > 0) cap = existing;
    store->cold_base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, store->cold_fd, 0);
    if (store->cold_base == MAP_FAILED) { close(store->cold_fd); return -1; }
#endif
    store->cold_capacity = cap;
    store->cold_used = existing;
    store->is_cold_twin = 1;
    strncpy(store->cold_filepath, filepath, DT_MAX_PATH - 1);
    store->cold_filepath[DT_MAX_PATH - 1] = '\0';
    if (existing > 0) dt_cold_rebuild_used(store);
    return 0;
}

static inline uint8_t *dt_cold_alloc(DRamTileStore *store, size_t sz) {
    if (!store->cold_base) return NULL;
    size_t off = (store->cold_used + 63) & ~63;
    if (off + sz > store->cold_capacity) return NULL;
    store->cold_used = off + sz;
    return store->cold_base + off;
}

static inline uint8_t *dt_entry_ptr(DRamTileStore *store, uint32_t slot) {
    if (store->hash[slot].dram_addr & DT_KV_FLAG)
        return store->kv_base + store->hash[slot].offset;
    return store->base + store->hash[slot].offset;
}

static inline uint8_t *dt_put_addr(DRamTileStore *store,
                                    uint32_t dram_addr,
                                    const uint8_t *data, size_t sz)
{
    uint32_t addr = dram_addr & 0x1FFFFFFFu;
    if (addr >= DRAM_FULL) return NULL;
    uint32_t slot = addr % DT_HASH_SLOTS;
    if (store->hash[slot].dram_addr == addr && store->hash[slot].size > 0) {
        if (store->hash[slot].size != sz) return NULL;
        memcpy(dt_entry_ptr(store, slot), data, sz);
        return dt_entry_ptr(store, slot);
    }
    store->session_tick++;
    store->hash[slot].name[0] = '\0';
    size_t off = (store->used + 63) & ~63;
    int local = (off + sz <= store->capacity);
    if (local) {
        memcpy(store->base + off, data, sz);
        store->used = off + sz;
        store->hash[slot].dram_addr = addr;
        store->hash[slot].offset    = off;
    } else {
        uint8_t *cold_ptr = dt_cold_alloc(store, sz);
        if (!cold_ptr) return NULL;
        memcpy(cold_ptr, data, sz);
        store->hash[slot].dram_addr = addr | DT_BOND_FLAG;
        store->hash[slot].offset    = 0;
        store->hash[slot].cold_offset = (uint32_t)(cold_ptr - store->cold_base);
        store->hash[slot].session_tick = store->session_tick;
        off = 0;
    }
    store->hash[slot].size = sz;
    store->n_stored++;
    return local ? (store->base + off) : (store->cold_base + store->hash[slot].cold_offset);
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

static inline uint8_t *dt_put(DRamTileStore *store,
                               const char *name,
                               const uint8_t *data, size_t sz) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if (store->hash[slot].dram_addr == addr) {
        /* unused */         size_t off = store->hash[slot].offset;
        if (store->hash[slot].size != sz) return NULL;
        if (store->hash[slot].session_tick > 0)
            fprintf(stderr, "[dt_put] WARNING: overwriting slot %u '%s' — "
                    "ensure no active SID swap reads from this mmap\n",
                    slot, name);
        memcpy(dt_entry_ptr(store, slot), data, sz);
        strncpy(store->hash[slot].name, name, DT_HASH_NAME - 1);
        store->hash[slot].name[DT_HASH_NAME - 1] = '\0';
        return dt_entry_ptr(store, slot);
    }
    store->session_tick++;
    strncpy(store->hash[slot].name, name, DT_HASH_NAME - 1);
    store->hash[slot].name[DT_HASH_NAME - 1] = '\0';
    size_t off = (store->used + 63) & ~63;
    int local = (off + sz <= store->capacity);
    if (local) {
        memcpy(store->base + off, data, sz);
        store->used = off + sz;
        store->hash[slot].dram_addr = addr;
        store->hash[slot].offset    = off;
    } else {
        uint8_t *cold_ptr = dt_cold_alloc(store, sz);
        if (!cold_ptr) return NULL;
        memcpy(cold_ptr, data, sz);
        store->hash[slot].dram_addr = addr | DT_BOND_FLAG;
        store->hash[slot].offset    = 0;
        store->hash[slot].cold_offset = (uint32_t)(cold_ptr - store->cold_base);
        store->hash[slot].session_tick = store->session_tick;
        off = 0;
    }
    store->hash[slot].size = sz;
    store->n_stored++;
    return local ? (store->base + off) : (store->cold_base + store->hash[slot].cold_offset);
}

static inline uint8_t *kv_compose(DRamTileStore *store, uint32_t slot) {
    uint32_t entry = store->hash[slot].dram_addr;
    if (entry & DT_DELTA_FLAG)
        return store->cold_base + store->hash[slot].cold_offset;
    if (entry & DT_BOND_FLAG)
        return store->cold_base + store->hash[slot].cold_offset;
    return store->kv_base + store->hash[slot].offset;
}

static inline uint8_t *kv_delta_spill(DRamTileStore *store, uint32_t slot,
                                       const uint8_t *fresh_data, size_t sz)
{
    uint8_t *floor0 = dt_cold_alloc(store, sz);
    if (!floor0) return NULL;
    memcpy(floor0, store->kv_base + store->hash[slot].offset, sz);
    store->session_tick++;
    uint8_t *floor1 = store->kv_base + store->hash[slot].offset;
    for (size_t i = 0; i < sz; i++)
        floor1[i] = fresh_data[i] ^ floor0[i];
    uint32_t flags = DT_KV_FLAG | DT_BOND_FLAG | DT_DELTA_FLAG;
    store->hash[slot].dram_addr   |= flags;
    store->hash[slot].cold_offset  = (uint32_t)(floor0 - store->cold_base);
    store->hash[slot].session_tick = store->session_tick;
    return floor0;
}

static inline uint8_t *kv_delta_compose_read(DRamTileStore *store,
                                              uint32_t slot,
                                              uint8_t *dst, size_t sz)
{
    uint8_t *floor0 = store->cold_base + store->hash[slot].cold_offset;
    uint8_t *floor1 = store->kv_base  + store->hash[slot].offset;
    if (dst != floor0)
        memcpy(dst, floor0, sz);
    for (size_t i = 0; i < sz; i++)
        dst[i] ^= floor1[i];
    return dst;
}

static inline uint8_t *dt_put_kv(DRamTileStore *store,
                                  const char *name,
                                  const uint8_t *data, size_t sz)
{
    if (!store->kv_base) return NULL;
    uint32_t raw_addr = dt_name_to_addr(name);
    uint32_t addr = raw_addr | DT_KV_FLAG;
    uint32_t slot = addr % DT_HASH_SLOTS;
    if (store->hash[slot].dram_addr == addr) {
        if (store->hash[slot].size != sz) return NULL;
        if (store->hash[slot].dram_addr & DT_BOND_FLAG) {
            memcpy(store->cold_base + store->hash[slot].cold_offset, data, sz);
            return store->cold_base + store->hash[slot].cold_offset;
        }
        memcpy(store->kv_base + store->hash[slot].offset, data, sz);
        return store->kv_base + store->hash[slot].offset;
    }
    size_t off = (store->kv_used + 63) & ~63;
    if (off + sz <= store->kv_capacity) {
        memcpy(store->kv_base + off, data, sz);
        store->kv_used = off + sz;
        store->hash[slot].dram_addr = addr;
        store->hash[slot].offset    = off;
        store->hash[slot].size      = sz;
        store->n_stored++;
        return store->kv_base + off;
    }
    if (!store->cold_base) return NULL;
    uint8_t *cold_ptr = dt_cold_alloc(store, sz);
    if (!cold_ptr) return NULL;
    memcpy(cold_ptr, data, sz);
    store->session_tick++;
    store->hash[slot].dram_addr = addr | DT_BOND_FLAG;
    store->hash[slot].offset    = 0;
    store->hash[slot].cold_offset = (uint32_t)(cold_ptr - store->cold_base);
    store->hash[slot].size      = sz;
    store->hash[slot].session_tick = store->session_tick;
    store->n_stored++;
    return cold_ptr;
}

static inline int dt_is_kv(DRamTileStore *store, uint32_t slot) {
    return (store->hash[slot].dram_addr & DT_KV_FLAG) != 0;
}

static inline uint8_t *dt_get(DRamTileStore *store, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    uint32_t entry = store->hash[slot].dram_addr;
    if ((entry & ~DT_FLAGS_MASK) != addr)
        return NULL;
    if (entry & DT_BOND_FLAG) {
        if (entry & DT_KV_FLAG)
            return kv_compose(store, slot);
        return store->cold_base + store->hash[slot].cold_offset;
    }
    return dt_entry_ptr(store, slot);
}

static inline size_t dt_get_size(DRamTileStore *store, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if ((store->hash[slot].dram_addr & ~DT_FLAGS_MASK) == addr)
        return store->hash[slot].size;
    return 0;
}

static inline void dt_store_destroy(DRamTileStore *store) {
    if (!store || !store->base) return;
    if (store->is_twin) {
#ifdef _WIN32
        if (store->base) UnmapViewOfFile(store->base);
        if (store->hMapping) CloseHandle(store->hMapping);
        if (store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
#else
        if (store->base) munmap(store->base, store->capacity);
        if (store->fd >= 0) close(store->fd);
#endif
        goto cleanup;
    }
#ifdef _WIN32
    VirtualFree(store->base, 0, MEM_RELEASE);
#else
    if (store->is_mmap)
        munmap(store->base, store->capacity);
    else
        free(store->base);
#endif
cleanup:
    if (store->kv_base) {
#ifdef _WIN32
        VirtualFree(store->kv_base, 0, MEM_RELEASE);
#else
        munmap(store->kv_base, store->kv_capacity);
#endif
    }
    if (store->cold_base) {
        if (store->is_cold_twin) {
#ifdef _WIN32
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
#else
            munmap(store->cold_base, store->cold_capacity);
            if (store->cold_fd >= 0) close(store->cold_fd);
#endif
        } else {
#ifdef _WIN32
            VirtualFree(store->cold_base, 0, MEM_RELEASE);
#else
            munmap(store->cold_base, store->cold_capacity);
#endif
        }
    }
    memset(store, 0, sizeof(*store));
}

typedef struct {
    uint32_t dram_addr;
    uint64_t offset;
    uint64_t size;
    uint32_t dtype;
    int32_t  ndim;
    uint32_t shape[DT_MAX_NDIM];
    uint32_t namelen;
} DtDirEntryV2;

static inline int dt_store_save_dir(DRamTileStore *store) {
    if (!store->is_twin || !store->base) return -1;
    uint32_t n = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        if (store->hash[i].dram_addr & DT_KV_FLAG) continue;
        if ((store->hash[i].dram_addr & DT_BOND_FLAG) && !store->is_cold_twin) continue;
        n++;
    }
    size_t dir_sz = 16 + (size_t)n * DT_DIR_ENTRY_SZ + 4;
    if (dir_sz + 64 > store->capacity) return -1;
    size_t dir_off = store->capacity - dir_sz;
    uint8_t *p = store->base + dir_off;
    memcpy(p, "TDI2", 4); p += 4;
    memcpy(p, &n, 4);     p += 4;
    memcpy(p, &store->used, 8); p += 8;
    uint32_t left = n;
    for (int i = 0; i < DT_HASH_SLOTS && left > 0; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        uint32_t raw = store->hash[i].dram_addr;
        if (raw & DT_KV_FLAG) continue;
        if ((raw & DT_BOND_FLAG) && !store->is_cold_twin) continue;
        uint32_t addr = raw & ~(DT_KV_FLAG | DT_BOND_FLAG);
        uint64_t off  = (uint64_t)store->hash[i].offset;
        uint64_t sz   = (uint64_t)store->hash[i].size;
        uint32_t dtype = 0;
        uint32_t shape[DT_MAX_NDIM] = {0};
        if (raw & DT_BOND_FLAG) {
            dtype = DT_COLD_BIT;
            shape[3] = store->hash[i].cold_offset;
        }
        memcpy(p, &addr, 4); p += 4;
        memcpy(p, &off,  8); p += 8;
        memcpy(p, &sz,   8); p += 8;
        memcpy(p, &dtype, 4); p += 4;
        uint32_t ndim = 0;
        memcpy(p, &ndim, 4); p += 4;
        memcpy(p, shape, sizeof(uint32_t) * DT_MAX_NDIM); p += sizeof(uint32_t) * DT_MAX_NDIM;
        uint32_t namelen = 0;
        memcpy(p, &namelen, 4); p += 4;
        left--;
    }
    memcpy(p, &dir_off, 4);
    return 0;
}

static inline int dt_store_save_dir_v2(DRamTileStore *store,
                                        DtTensorView *views, uint32_t n_views)
{
    if (!store->is_twin || !store->base) return -1;
    uint32_t n = n_views;
    size_t name_total = 0;
    for (uint32_t i = 0; i < n; i++)
        name_total += ((strlen(views[i].name) + 7) & ~7);
    size_t dir_sz = 16 + (size_t)n * DT_DIR_ENTRY_SZ + name_total + 4;
    if (dir_sz + 64 > store->capacity) return -1;
    size_t dir_off = store->capacity - dir_sz;
    uint8_t *p = store->base + dir_off;
    memcpy(p, "TDI2", 4); p += 4;
    memcpy(p, &n, 4);     p += 4;
    memcpy(p, &store->used, 8); p += 8;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = views[i].dram_addr;
        uint64_t off  = (uint64_t)views[i].offset;
        uint64_t sz   = (uint64_t)views[i].nbytes;
        uint32_t dtype = views[i].dtype;
        int32_t  ndim  = views[i].ndim;
        uint32_t namelen = (uint32_t)strlen(views[i].name);
        memcpy(p, &addr, 4); p += 4;
        memcpy(p, &off,  8); p += 8;
        memcpy(p, &sz,   8); p += 8;
        memcpy(p, &dtype, 4); p += 4;
        memcpy(p, &ndim,  4); p += 4;
        memcpy(p, views[i].shape, sizeof(uint32_t) * DT_MAX_NDIM);
        p += sizeof(uint32_t) * DT_MAX_NDIM;
        memcpy(p, &namelen, 4); p += 4;
        memcpy(p, views[i].name, namelen); p += namelen;
        size_t pad = ((namelen + 7) & ~7) - namelen;
        memset(p, 0, pad); p += pad;
    }
    memcpy(p, &dir_off, 4);
    return 0;
}

static inline int dt_store_load_dir(DRamTileStore *store) {
    if (!store->base || store->capacity < 12) return -1;
    uint32_t dir_off;
    memcpy(&dir_off, store->base + store->capacity - 4, 4);
    if (dir_off >= store->capacity - 16) return -1;
    uint8_t *p = store->base + dir_off;
    char magic[5] = {0};
    memcpy(magic, p, 4); p += 4;
    uint32_t n;
    memcpy(&n, p, 4); p += 4;
    if (n > DT_HASH_SLOTS) return -1;
    memcpy(&store->used, p, 8); p += 8;
    memset(store->hash, 0, sizeof(store->hash));
    store->n_stored = 0;
    if (strcmp(magic, "TDI2") == 0) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t addr, dtype, namelen;
            int32_t  ndim;
            uint64_t off, sz;
            uint32_t shape[DT_MAX_NDIM] = {0};
            memcpy(&addr, p, 4); p += 4;
            memcpy(&off,  p, 8); p += 8;
            memcpy(&sz,   p, 8); p += 8;
            memcpy(&dtype, p, 4); p += 4;
            memcpy(&ndim,  p, 4); p += 4;
            memcpy(shape, p, sizeof(uint32_t) * DT_MAX_NDIM);
            p += sizeof(uint32_t) * DT_MAX_NDIM;
            memcpy(&namelen, p, 4); p += 4;
            if (namelen > 0) p += (namelen + 7) & ~7;
            uint32_t slot = addr % DT_HASH_SLOTS;
            store->hash[slot].dram_addr = addr;
            if (dtype & DT_COLD_BIT) {
                store->hash[slot].dram_addr |= DT_BOND_FLAG;
                store->hash[slot].cold_offset = shape[3];
            }
            store->hash[slot].offset    = (size_t)off;
            store->hash[slot].size      = (size_t)sz;
            store->n_stored++;
        }
    } else if (strcmp(magic, "TDIR") == 0) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t addr; size_t off, sz;
            memcpy(&addr, p, 4); p += 4;
            memcpy(&off,  p, 8); p += 8;
            memcpy(&sz,   p, 8); p += 8;
            uint32_t slot = addr % DT_HASH_SLOTS;
            store->hash[slot].dram_addr = addr;
            store->hash[slot].offset    = off;
            store->hash[slot].size      = sz;
            store->n_stored++;
        }
    } else {
        return -1;
    }
    return 0;
}

static inline int dt_store_load_views(DRamTileStore *store,
                                       DtTensorView **out_views)
{
    *out_views = NULL;
    if (!store->base || store->capacity < 12) return -1;
    uint32_t dir_off;
    memcpy(&dir_off, store->base + store->capacity - 4, 4);
    if (dir_off >= store->capacity - 16) return -1;
    uint8_t *p = store->base + dir_off;
    char magic[5] = {0};
    memcpy(magic, p, 4); p += 4;
    if (strcmp(magic, "TDI2") != 0) return -1;
    uint32_t n;
    memcpy(&n, p, 4); p += 4;
    if (n > DT_HASH_SLOTS) return -1;
    uint64_t data_used;
    memcpy(&data_used, p, 8); p += 8;
    DtTensorView *views = (DtTensorView*)calloc(n, sizeof(DtTensorView));
    if (!views) return -1;
    memset(store->hash, 0, sizeof(store->hash));
    store->n_stored = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr, dtype, namelen;
        int32_t  ndim;
        uint64_t off, sz;
        memcpy(&addr, p, 4); p += 4;
        memcpy(&off,  p, 8); p += 8;
        memcpy(&sz,   p, 8); p += 8;
        memcpy(&dtype, p, 4); p += 4;
        memcpy(&ndim,  p, 4); p += 4;
        memcpy(views[i].shape, p, sizeof(uint32_t) * DT_MAX_NDIM);
        p += sizeof(uint32_t) * DT_MAX_NDIM;
        memcpy(&namelen, p, 4); p += 4;
        views[i].data      = store->base + (size_t)off;
        views[i].offset    = (size_t)off;
        views[i].nbytes    = (size_t)sz;
        views[i].dram_addr = addr;
        views[i].dtype     = dtype;
        views[i].ndim      = (int)ndim;
        memset(views[i].name, 0, DT_NAME_MAX);
        if (namelen > 0 && namelen < DT_NAME_MAX)
            memcpy(views[i].name, p, namelen);
        if (namelen > 0) p += (namelen + 7) & ~7;
        uint32_t slot = addr % DT_HASH_SLOTS;
        store->hash[slot].dram_addr = addr;
        store->hash[slot].offset    = (size_t)off;
        store->hash[slot].size      = (size_t)sz;
        store->n_stored++;
    }
    *out_views = views;
    return (int)n;
}

static inline int dt_store_init_twin(DRamTileStore *store,
                                      const char *filepath,
                                      size_t max_bytes)
{
    memset(store, 0, sizeof(*store));
    size_t cap = max_bytes;
    int exists = 0;
#ifdef _WIN32
    store->hFile = CreateFileA(filepath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (store->hFile == INVALID_HANDLE_VALUE) return -1;
    DWORD hi; DWORD lo = GetFileSize(store->hFile, &hi);
    exists = ((uint64_t)hi << 32 | lo) >= 64;
    if (!exists) {
        LARGE_INTEGER sz;
        sz.QuadPart = cap;
        SetFilePointerEx(store->hFile, sz, NULL, FILE_BEGIN);
        SetEndOfFile(store->hFile);
    }
    store->hMapping = CreateFileMappingA(store->hFile, NULL,
        PAGE_READWRITE, (DWORD)(cap >> 32), (DWORD)cap, NULL);
    if (!store->hMapping) { CloseHandle(store->hFile); return -1; }
    store->base = (uint8_t*)MapViewOfFile(store->hMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, cap);
    if (!store->base) { CloseHandle(store->hMapping); CloseHandle(store->hFile); return -1; }
    store->is_mmap = 1;
#else
    int flags = O_RDWR | O_CREAT;
    store->fd = open(filepath, flags, 0644);
    if (store->fd < 0) return -1;
    struct stat st; fstat(store->fd, &st);
    exists = st.st_size >= 64;
    if (!exists) { if (ftruncate(store->fd, cap) != 0) { close(store->fd); return -1; } }
    else { cap = st.st_size; }
    store->base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, store->fd, 0);
    if (store->base == MAP_FAILED) { close(store->fd); return -1; }
    store->is_mmap = 1;
#endif
    store->capacity = cap;
    store->is_twin   = 1;
    strncpy(store->filepath, filepath, DT_MAX_PATH - 1);
    store->filepath[DT_MAX_PATH - 1] = '\0';
    if (exists) {
        if (dt_store_load_dir(store) != 0) {
            memset(store->hash, 0, sizeof(store->hash));
            store->n_stored = 0; store->used = 0;
        }
    }
    return 0;
}

static inline int dt_store_init_twin_dual(DRamTileStore *store,
                                           const char *filepath,
                                           size_t max_bytes,
                                           float weight_ratio)
{
    memset(store, 0, sizeof(*store));
    if (weight_ratio <= 0.0f) weight_ratio = 0.5f;
    if (weight_ratio >= 1.0f) weight_ratio = 0.9f;
    size_t total = max_bytes < 4UL * 1024 * 1024 * 1024
                  ? 4UL * 1024 * 1024 * 1024 : max_bytes;
    size_t wcap = ((size_t)(total * weight_ratio)) & ~4095u;
    size_t kcap = (total - wcap) & ~4095u;
    if (kcap < (1u << 20)) kcap = 1u << 20;
    int exists = 0;
#ifdef _WIN32
    store->hFile = CreateFileA(filepath,
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (store->hFile == INVALID_HANDLE_VALUE) return -1;
    DWORD hi; DWORD lo = GetFileSize(store->hFile, &hi);
    uint64_t fsz = (uint64_t)hi << 32 | lo;
    exists = fsz >= 64;
    if (!exists) {
        LARGE_INTEGER sz; sz.QuadPart = wcap;
        SetFilePointerEx(store->hFile, sz, NULL, FILE_BEGIN);
        SetEndOfFile(store->hFile);
    } else if (fsz > wcap) { wcap = (size_t)fsz; }
    store->hMapping = CreateFileMappingA(store->hFile, NULL,
        PAGE_READWRITE, (DWORD)(wcap >> 32), (DWORD)wcap, NULL);
    if (!store->hMapping) { CloseHandle(store->hFile); return -1; }
    store->base = (uint8_t*)MapViewOfFile(store->hMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, wcap);
    if (!store->base) { CloseHandle(store->hMapping); CloseHandle(store->hFile); return -1; }
    store->kv_base = (uint8_t*)VirtualAlloc(NULL, kcap,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!store->kv_base) {
        UnmapViewOfFile(store->base); CloseHandle(store->hMapping); CloseHandle(store->hFile);
        return -1;
    }
    store->is_mmap = 1;
#else
    store->fd = open(filepath, O_RDWR | O_CREAT, 0644);
    if (store->fd < 0) return -1;
    struct stat st; fstat(store->fd, &st);
    exists = st.st_size >= 64;
    if (!exists && ftruncate(store->fd, wcap) != 0) { close(store->fd); return -1; }
    if (exists && (size_t)st.st_size > wcap) wcap = (size_t)st.st_size;
    store->base = (uint8_t*)mmap(NULL, wcap, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, store->fd, 0);
    if (store->base == MAP_FAILED) { close(store->fd); return -1; }
    store->kv_base = (uint8_t*)mmap(NULL, kcap, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (store->kv_base == MAP_FAILED) { munmap(store->base, wcap); close(store->fd); return -1; }
    store->is_mmap = 1;
#endif
    store->capacity        = wcap;
    store->kv_capacity     = kcap;
    store->weight_boundary = wcap;
    store->is_twin         = 1;
    strncpy(store->filepath, filepath, DT_MAX_PATH - 1);
    store->filepath[DT_MAX_PATH - 1] = '\0';
    if (exists) {
        if (dt_store_load_dir(store) != 0) {
            memset(store->hash, 0, sizeof(store->hash));
            store->n_stored = 0; store->used = 0;
        }
    }
    return 0;
}

static inline int dt_store_sync(DRamTileStore *store, int async) {
    if (!store->is_twin || !store->base) return -1;
#ifdef _WIN32
    (void)async;
    return FlushViewOfFile(store->base, store->used) ? 0 : -1;
#else
    return msync(store->base, store->used,
                 async ? MS_ASYNC : MS_SYNC);
#endif
}

static inline void dt_store_destroy_twin(DRamTileStore *store) {
    if (!store || !store->base || !store->is_twin) return;
    dt_store_save_dir(store);
    dt_store_sync(store, 0);
#ifdef _WIN32
    UnmapViewOfFile(store->base);
    if (store->hMapping) CloseHandle(store->hMapping);
    if (store->hFile && store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
    if (store->kv_base) VirtualFree(store->kv_base, 0, MEM_RELEASE);
    if (store->cold_base) {
        if (store->is_cold_twin) {
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
        } else { VirtualFree(store->cold_base, 0, MEM_RELEASE); }
    }
#else
    munmap(store->base, store->capacity);
    if (store->kv_base) munmap(store->kv_base, store->kv_capacity);
    if (store->cold_base) {
        if (store->is_cold_twin)
            munmap(store->cold_base, store->cold_capacity);
        else
            munmap(store->cold_base, store->cold_capacity);
    }
    if (store->fd >= 0) close(store->fd);
    if (store->is_cold_twin && store->cold_fd >= 0) close(store->cold_fd);
#endif
    memset(store, 0, sizeof(*store));
}

static inline void dt_store_destroy_twinv(DRamTileStore *store) {
    if (!store || !store->base || !store->is_twin) return;
    uint32_t n = store->n_stored;
    DtTensorView *views = NULL;
    if (n > 0) {
        views = (DtTensorView*)calloc(n, sizeof(DtTensorView));
        if (views) {
            int idx = 0;
            for (int i = 0; i < DT_HASH_SLOTS && idx < (int)n; i++) {
                if (store->hash[i].dram_addr == 0) continue;
                if (store->hash[i].dram_addr & (DT_KV_FLAG | DT_BOND_FLAG)) continue;
                views[idx].data      = store->base + store->hash[i].offset;
                views[idx].offset    = store->hash[i].offset;
                views[idx].nbytes    = store->hash[i].size;
                views[idx].dram_addr = store->hash[i].dram_addr;
                idx++;
            }
            if (idx > 0)
                dt_store_save_dir_v2(store, views, (uint32_t)idx);
            else
                dt_store_save_dir(store);
            free(views);
        } else { dt_store_save_dir(store); }
    } else { dt_store_save_dir(store); }
    dt_store_sync(store, 0);
#ifdef _WIN32
    UnmapViewOfFile(store->base);
    if (store->hMapping) CloseHandle(store->hMapping);
    if (store->hFile && store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
    if (store->kv_base) VirtualFree(store->kv_base, 0, MEM_RELEASE);
    if (store->cold_base) {
        if (store->is_cold_twin) {
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
        } else { VirtualFree(store->cold_base, 0, MEM_RELEASE); }
    }
#else
    munmap(store->base, store->capacity);
    if (store->kv_base) munmap(store->kv_base, store->kv_capacity);
    if (store->cold_base) munmap(store->cold_base, store->cold_capacity);
    if (store->fd >= 0) close(store->fd);
    if (store->is_cold_twin && store->cold_fd >= 0) close(store->cold_fd);
#endif
    memset(store, 0, sizeof(*store));
}

static inline uint8_t *dt_putv(DRamTileStore *store,
                                const char *name,
                                uint32_t dtype,
                                int ndim,
                                const uint32_t *shape,
                                const uint8_t *data, size_t sz)
{
    return dt_put(store, name, data, sz);
}

static inline DtTensorView dt_getv(DRamTileStore *store, const char *name) {
    DtTensorView v;
    memset(&v, 0, sizeof(v));
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if ((store->hash[slot].dram_addr & ~DT_FLAGS_MASK) != addr) return v;
    v.data      = dt_routed_ptr(store, slot);
    v.offset    = store->hash[slot].offset;
    v.nbytes    = store->hash[slot].size;
    v.dram_addr = store->hash[slot].dram_addr;
    strncpy(v.name, name, DT_NAME_MAX - 1);
    return v;
}

static inline DtTensorView dt_view(DRamTileStore *store,
                                    const char *name,
                                    uint32_t dtype,
                                    int ndim,
                                    const uint32_t *shape)
{
    DtTensorView v;
    memset(&v, 0, sizeof(v));
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if ((store->hash[slot].dram_addr & ~DT_FLAGS_MASK) != addr) return v;
    v.data      = dt_routed_ptr(store, slot);
    v.offset    = store->hash[slot].offset;
    v.nbytes    = store->hash[slot].size;
    v.dram_addr = store->hash[slot].dram_addr;
    v.dtype     = dtype;
    v.ndim      = ndim;
    memcpy(v.shape, shape, sizeof(uint32_t) * (size_t)(ndim > 0 ? ndim : 1));
    strncpy(v.name, name, DT_NAME_MAX - 1);
    return v;
}

static inline int dt_store_putv(DRamTileStore *store, DtTensorView *view) {
    uint8_t *p = dt_put(store, view->name, view->data, view->nbytes);
    if (!p) return -1;
    view->data = p;
    return 0;
}

static inline int dt_store_foreach(DRamTileStore *store,
                                    DtTensorCallback cb, void *user)
{
    if (!store || !cb) return -1;
    int count = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        DtTensorView v;
        memset(&v, 0, sizeof(v));
        v.data      = dt_routed_ptr(store, i);
        v.offset    = store->hash[i].offset;
        v.nbytes    = store->hash[i].size;
        v.dram_addr = store->hash[i].dram_addr;
        strncpy(v.name, store->hash[i].name, DT_NAME_MAX - 1);
        if (cb(&v, user)) break;
        count++;
    }
    return count;
}

static inline size_t dt_store_total_bytes(DRamTileStore *store) {
    return store->used + store->kv_used + store->cold_used;
}
