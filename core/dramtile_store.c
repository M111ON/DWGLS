#include "dramtile_store.h"

/* ── Anonymous init ── */

int dt_store_init(DRamTileStore *store, size_t min_bytes) {
    memset(store, 0, sizeof(*store));
    dt_enable_lock_privilege();
    size_t cap = min_bytes < 4UL * 1024 * 1024 * 1024
               ? 4UL * 1024 * 1024 * 1024
               : min_bytes;
#ifdef _WIN32
    store->base = (uint8_t*)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    store->is_mmap = (store->base != NULL);
    if (store->base && dt_lock_pages(store->base, cap) == 0)
        store->locked |= DT_LOCKED_BASE;
#else
    store->base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    store->is_mmap = (store->base != MAP_FAILED);
    if (!store->is_mmap) store->base = NULL;
    if (store->base && store->is_mmap && dt_lock_pages(store->base, cap) == 0)
        store->locked |= DT_LOCKED_BASE;
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

/* ── Cold region ── */

int dt_store_init_cold(DRamTileStore *store, size_t max_bytes) {
    if (!store) return -1;
    dt_enable_lock_privilege();
    size_t cap = max_bytes < DT_COLD_DEFAULT ? DT_COLD_DEFAULT : max_bytes;
#ifdef _WIN32
    store->cold_base = (uint8_t*)VirtualAlloc(NULL, cap,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (store->cold_base && dt_lock_pages(store->cold_base, cap) == 0)
        store->locked |= DT_LOCKED_COLD;
#else
    store->cold_base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (store->cold_base == MAP_FAILED) store->cold_base = NULL;
    if (store->cold_base && dt_lock_pages(store->cold_base, cap) == 0)
        store->locked |= DT_LOCKED_COLD;
#endif
    if (!store->cold_base) return -1;
    store->cold_capacity = cap;
    store->cold_used = 0;
    return 0;
}

void dt_cold_rebuild_used(DRamTileStore *store) {
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

int dt_store_init_cold_twin(DRamTileStore *store, const char *filepath, size_t max_bytes) {
    if (!store) return -1;
    dt_enable_lock_privilege();
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
    if (dt_lock_pages(store->cold_base, cap) == 0)
        store->locked |= DT_LOCKED_COLD;
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
    if (dt_lock_pages(store->cold_base, cap) == 0)
        store->locked |= DT_LOCKED_COLD;
#endif
    store->cold_capacity = cap;
    store->cold_used = existing;
    store->is_cold_twin = 1;
    strncpy(store->cold_filepath, filepath, DT_MAX_PATH - 1);
    store->cold_filepath[DT_MAX_PATH - 1] = '\0';
    if (existing > 0) dt_cold_rebuild_used(store);
    return 0;
}

uint8_t *dt_cold_alloc(DRamTileStore *store, size_t sz) {
    if (!store->cold_base) return NULL;
    size_t off = (store->cold_used + 63) & ~63;
    if (off + sz > store->cold_capacity) return NULL;
    store->cold_used = off + sz;
    return store->cold_base + off;
}

/* ── hash probing (linear) ────────────────────────────────
   dt_name_to_addr is not collision-free: two names can land on the
   same slot. Without probing, the second put silently overwrites the
   first entry's hash record (data loss on get). These helpers walk
   slots linearly; entries compare with DT_FLAGS_MASK stripped. */

static int dt_probe_find(DRamTileStore *store, uint32_t addr) {
    uint32_t start = addr % DT_HASH_SLOTS;
    for (uint32_t i = 0; i < DT_HASH_SLOTS; i++) {
        uint32_t s = (start + i) % DT_HASH_SLOTS;
        const DRamTileHashEntry *en = &store->hash[s];
        if (en->size == 0 && en->dram_addr == 0)
            return -1;                          /* empty → not present */
        if (en->size > 0 && (en->dram_addr & ~DT_FLAGS_MASK) == addr)
            return (int)s;
    }
    return -1;
}

/* returns slot to use for insert/overwrite of `addr` with `sz` bytes:
   existing match → its slot · first empty → that slot
   -2 = name exists with different size · -1 = table full */
static int dt_probe_insert(DRamTileStore *store, uint32_t addr, size_t sz) {
    uint32_t start = addr % DT_HASH_SLOTS;
    for (uint32_t i = 0; i < DT_HASH_SLOTS; i++) {
        uint32_t s = (start + i) % DT_HASH_SLOTS;
        DRamTileHashEntry *en = &store->hash[s];
        if (en->size == 0 && en->dram_addr == 0)
            return (int)s;                      /* empty */
        if ((en->dram_addr & ~DT_FLAGS_MASK) == addr)
            return en->size == sz ? (int)s : -2;
    }
    return -1;
}

/* ── put_addr ── */

uint8_t *dt_put_addr(DRamTileStore *store, uint32_t dram_addr, const uint8_t *data, size_t sz) {
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

/* ── Free list ── */

void dt_free_clear(DRamTileStore *store) {
    store->free_count = 0;
}

int dt_free_add(DRamTileStore *store, size_t offset, size_t size) {
    if (!store || size == 0) return 0;
    size_t end = offset + size;
    for (int i = 0; i < store->free_count; i++) {
        size_t fe = store->free_offs[i] + store->free_sizes[i];
        if (end == store->free_offs[i]) {
            store->free_offs[i] = offset;
            store->free_sizes[i] += size;
            return 1;
        }
        if (offset == fe) {
            store->free_sizes[i] += size;
            return 1;
        }
        size_t free_end = fe;
        if (offset >= store->free_offs[i] && offset < free_end) return 0;
        if (offset <= store->free_offs[i] && end > store->free_offs[i]) return 0;
    }
    if (store->free_count >= DT_MAX_FREE) return -1;
    store->free_offs[store->free_count] = offset;
    store->free_sizes[store->free_count] = size;
    store->free_count++;
    return 1;
}

size_t dt_free_take(DRamTileStore *store, size_t sz) {
    int best = -1;
    size_t best_sz = SIZE_MAX;
    for (int i = 0; i < store->free_count; i++) {
        if (store->free_sizes[i] >= sz && store->free_sizes[i] < best_sz) {
            best = i;
            best_sz = store->free_sizes[i];
        }
    }
    if (best < 0) return SIZE_MAX;
    size_t off = store->free_offs[best];
    size_t rem = store->free_sizes[best] - sz;
    size_t rem_off = off + sz;
    int last = store->free_count - 1;
    if (best < last) {
        store->free_offs[best] = store->free_offs[last];
        store->free_sizes[best] = store->free_sizes[last];
    }
    store->free_count--;
    if (rem >= 64) dt_free_add(store, rem_off, rem);
    return off;
}

int dt_free(DRamTileStore *store, const char *name) {
    if (!store || !name) return -1;
    uint32_t addr = dt_name_to_addr(name);
    int s = dt_probe_find(store, addr);
    if (s < 0) return -1;
    uint32_t slot = (uint32_t)s;
    uint32_t entry = store->hash[slot].dram_addr;
    size_t sz = store->hash[slot].size;
    if (entry & DT_BOND_FLAG) {
        if (store->evict_cb) store->evict_cb(name, store->evict_user);
        memset(&store->hash[slot], 0, sizeof(store->hash[slot]));
        store->n_stored--;
        return 0;
    }
    size_t off = store->hash[slot].offset;
    if (store->evict_cb) store->evict_cb(name, store->evict_user);
    memset(&store->hash[slot], 0, sizeof(store->hash[slot]));
    store->n_stored--;
    dt_free_add(store, off, sz);
    return 0;
}

size_t dt_free_bytes(DRamTileStore *store) {
    size_t total = 0;
    for (int i = 0; i < store->free_count; i++)
        total += store->free_sizes[i];
    return total;
}

/* ── put / get ── */

uint8_t *dt_put(DRamTileStore *store, const char *name, const uint8_t *data, size_t sz) {
    uint32_t addr = dt_name_to_addr(name);
    int ps = dt_probe_insert(store, addr, sz);
    if (ps < 0) return NULL;                /* full or size conflict */
    uint32_t slot = (uint32_t)ps;
    if (store->hash[slot].size > 0) {       /* overwrite existing */
        if (store->hash[slot].dram_addr & DT_BOND_FLAG) {
            memcpy(store->cold_base + store->hash[slot].cold_offset, data, sz);
            strncpy(store->hash[slot].name, name, DT_HASH_NAME - 1);
            store->hash[slot].name[DT_HASH_NAME - 1] = '\0';
            return store->cold_base + store->hash[slot].cold_offset;
        }
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
    size_t off = SIZE_MAX;
    if (store->free_count > 0) {
        off = dt_free_take(store, sz);
    }
    if (off == SIZE_MAX) {
        off = (store->used + 63) & ~63;
    }
    int local = (off + sz <= store->capacity);
    if (local) {
        memcpy(store->base + off, data, sz);
        if (off + sz > store->used) store->used = off + sz;
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

uint8_t *kv_compose(DRamTileStore *store, uint32_t slot) {
    uint32_t entry = store->hash[slot].dram_addr;
    if (entry & DT_DELTA_FLAG)
        return store->cold_base + store->hash[slot].cold_offset;
    if (entry & DT_BOND_FLAG)
        return store->cold_base + store->hash[slot].cold_offset;
    return store->kv_base + store->hash[slot].offset;
}

uint8_t *kv_delta_spill(DRamTileStore *store, uint32_t slot,
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

uint8_t *kv_delta_compose_read(DRamTileStore *store,
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

uint8_t *dt_put_kv(DRamTileStore *store, const char *name, const uint8_t *data, size_t sz) {
    if (!store->kv_base) return NULL;
    uint32_t raw_addr = dt_name_to_addr(name);
    int ps = dt_probe_insert(store, raw_addr, sz);
    if (ps < 0) return NULL;
    uint32_t slot = (uint32_t)ps;
    if (store->hash[slot].size > 0) {       /* overwrite existing */
        if (store->hash[slot].dram_addr & DT_BOND_FLAG) {
            memcpy(store->cold_base + store->hash[slot].cold_offset, data, sz);
            return store->cold_base + store->hash[slot].cold_offset;
        }
        memcpy(store->kv_base + store->hash[slot].offset, data, sz);
        return store->kv_base + store->hash[slot].offset;
    }
    uint32_t addr = raw_addr | DT_KV_FLAG;
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

uint8_t *dt_get(DRamTileStore *store, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    int s = dt_probe_find(store, addr);
    if (s < 0) return NULL;
    uint32_t slot = (uint32_t)s;
    uint32_t entry = store->hash[slot].dram_addr;
    if (entry & DT_BOND_FLAG) {
        if (entry & DT_KV_FLAG)
            return kv_compose(store, slot);
        return store->cold_base + store->hash[slot].cold_offset;
    }
    return dt_entry_ptr(store, slot);
}

/* ── Destroy (anonymous) ── */

void dt_store_destroy(DRamTileStore *store) {
    if (!store || !store->base) return;
    if (store->is_twin) {
#ifdef _WIN32
        if (store->locked & DT_LOCKED_BASE) dt_unlock_pages(store->base, store->capacity);
        if (store->base) UnmapViewOfFile(store->base);
        if (store->hMapping) CloseHandle(store->hMapping);
        if (store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
#else
        if (store->locked & DT_LOCKED_BASE) dt_unlock_pages(store->base, store->capacity);
        if (store->base) munmap(store->base, store->capacity);
        if (store->fd >= 0) close(store->fd);
#endif
        goto cleanup;
    }
#ifdef _WIN32
    if (store->locked & DT_LOCKED_BASE) dt_unlock_pages(store->base, store->capacity);
    VirtualFree(store->base, 0, MEM_RELEASE);
#else
    if (store->locked & DT_LOCKED_BASE) dt_unlock_pages(store->base, store->capacity);
    if (store->is_mmap)
        munmap(store->base, store->capacity);
    else
        free(store->base);
#endif
cleanup:
    if (store->kv_base) {
#ifdef _WIN32
        if (store->locked & DT_LOCKED_KV) dt_unlock_pages(store->kv_base, store->kv_capacity);
        VirtualFree(store->kv_base, 0, MEM_RELEASE);
#else
        if (store->locked & DT_LOCKED_KV) dt_unlock_pages(store->kv_base, store->kv_capacity);
        munmap(store->kv_base, store->kv_capacity);
#endif
    }
    if (store->cold_base) {
        if (store->is_cold_twin) {
#ifdef _WIN32
            if (store->locked & DT_LOCKED_COLD) dt_unlock_pages(store->cold_base, store->cold_capacity);
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
#else
            if (store->locked & DT_LOCKED_COLD) dt_unlock_pages(store->cold_base, store->cold_capacity);
            munmap(store->cold_base, store->cold_capacity);
            if (store->cold_fd >= 0) close(store->cold_fd);
#endif
        } else {
#ifdef _WIN32
            if (store->locked & DT_LOCKED_COLD) dt_unlock_pages(store->cold_base, store->cold_capacity);
            VirtualFree(store->cold_base, 0, MEM_RELEASE);
#else
            if (store->locked & DT_LOCKED_COLD) dt_unlock_pages(store->cold_base, store->cold_capacity);
            munmap(store->cold_base, store->cold_capacity);
#endif
        }
    }
    store->locked = 0;
    memset(store, 0, sizeof(*store));
}

/* ── Directory save / load ── */

int dt_store_save_dir(DRamTileStore *store) {
    if (!store->is_twin || !store->base) return -1;
    uint32_t n = 0;
    size_t name_total = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        if (store->hash[i].dram_addr & DT_KV_FLAG) continue;
        if ((store->hash[i].dram_addr & DT_BOND_FLAG) && !store->is_cold_twin) continue;
        name_total += ((strlen(store->hash[i].name) + 7) & ~7);
        n++;
    }
    size_t dir_sz = 16 + (size_t)n * DT_DIR_ENTRY_SZ + name_total + 4;
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
        uint32_t namelen = (uint32_t)strlen(store->hash[i].name);
        memcpy(p, &namelen, 4); p += 4;
        if (namelen > 0) {
            memcpy(p, store->hash[i].name, namelen); p += namelen;
            size_t pad = ((namelen + 7) & ~7) - namelen;
            memset(p, 0, pad); p += pad;
        }
        left--;
    }
    memcpy(p, &dir_off, 4);
    return 0;
}

int dt_store_save_dir_v2(DRamTileStore *store, DtTensorView *views, uint32_t n_views) {
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

int dt_store_load_dir(DRamTileStore *store) {
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
            uint32_t slot = addr % DT_HASH_SLOTS;
            if (namelen > 0 && namelen < DT_NAME_MAX) {
                memcpy(store->hash[slot].name, p, namelen);
                store->hash[slot].name[namelen] = '\0';
            }
            if (namelen > 0) p += (namelen + 7) & ~7;
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

int dt_store_load_views(DRamTileStore *store, DtTensorView **out_views) {
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
        if (namelen > 0 && namelen < DT_NAME_MAX) {
            memcpy(views[i].name, p, namelen);
        }
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

/* ── Twin init ── */

int dt_store_init_twin(DRamTileStore *store, const char *filepath, size_t max_bytes) {
    memset(store, 0, sizeof(*store));
    dt_enable_lock_privilege();
    size_t cap = max_bytes;
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
        /* New file: set to requested capacity */
        LARGE_INTEGER sz; sz.QuadPart = cap;
        SetFilePointerEx(store->hFile, sz, NULL, FILE_BEGIN);
        SetEndOfFile(store->hFile);
    } else {
        /* Existing file: use actual file size (like Linux path) */
        cap = (size_t)fsz;
    }
    store->hMapping = CreateFileMappingA(store->hFile, NULL,
        PAGE_READWRITE, (DWORD)(cap >> 32), (DWORD)cap, NULL);
    if (!store->hMapping) { CloseHandle(store->hFile); return -1; }
    store->base = (uint8_t*)MapViewOfFile(store->hMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, cap);
    if (!store->base) { CloseHandle(store->hMapping); CloseHandle(store->hFile); return -1; }
    if (dt_lock_pages(store->base, cap) == 0)
        store->locked |= DT_LOCKED_BASE;
    store->is_mmap = 1;
#else
    store->fd = open(filepath, O_RDWR | O_CREAT, 0644);
    if (store->fd < 0) return -1;
    struct stat st; fstat(store->fd, &st);
    exists = st.st_size >= 64;
    if (!exists) { if (ftruncate(store->fd, cap) != 0) { close(store->fd); return -1; } }
    else { cap = st.st_size; }
    store->base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_SHARED, store->fd, 0);
    if (store->base == MAP_FAILED) { close(store->fd); return -1; }
    if (dt_lock_pages(store->base, cap) == 0)
        store->locked |= DT_LOCKED_BASE;
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

int dt_store_init_twin_dual(DRamTileStore *store, const char *filepath,
                             size_t max_bytes, float weight_ratio)
{
    memset(store, 0, sizeof(*store));
    dt_enable_lock_privilege();
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
    if (!exists) { LARGE_INTEGER sz; sz.QuadPart = wcap; SetFilePointerEx(store->hFile, sz, NULL, FILE_BEGIN); SetEndOfFile(store->hFile); }
    else if (fsz > wcap) { wcap = (size_t)fsz; }
    store->hMapping = CreateFileMappingA(store->hFile, NULL,
        PAGE_READWRITE, (DWORD)(wcap >> 32), (DWORD)wcap, NULL);
    if (!store->hMapping) { CloseHandle(store->hFile); return -1; }
    store->base = (uint8_t*)MapViewOfFile(store->hMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, wcap);
    if (!store->base) { CloseHandle(store->hMapping); CloseHandle(store->hFile); return -1; }
    if (dt_lock_pages(store->base, wcap) == 0) store->locked |= DT_LOCKED_BASE;
    store->kv_base = (uint8_t*)VirtualAlloc(NULL, kcap,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!store->kv_base) { UnmapViewOfFile(store->base); CloseHandle(store->hMapping); CloseHandle(store->hFile); return -1; }
    if (dt_lock_pages(store->kv_base, kcap) == 0) store->locked |= DT_LOCKED_KV;
    store->is_mmap = 1;
#else
    store->fd = open(filepath, O_RDWR | O_CREAT, 0644);
    if (store->fd < 0) return -1;
    struct stat st; fstat(store->fd, &st);
    exists = st.st_size >= 64;
    if (!exists && ftruncate(store->fd, wcap) != 0) { close(store->fd); return -1; }
    if (exists && (size_t)st.st_size > wcap) wcap = (size_t)st.st_size;
    store->base = (uint8_t*)mmap(NULL, wcap, PROT_READ | PROT_WRITE, MAP_SHARED, store->fd, 0);
    if (store->base == MAP_FAILED) { close(store->fd); return -1; }
    if (dt_lock_pages(store->base, wcap) == 0) store->locked |= DT_LOCKED_BASE;
    store->kv_base = (uint8_t*)mmap(NULL, kcap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (store->kv_base == MAP_FAILED) { munmap(store->base, wcap); close(store->fd); return -1; }
    if (dt_lock_pages(store->kv_base, kcap) == 0) store->locked |= DT_LOCKED_KV;
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

int dt_store_sync(DRamTileStore *store, int async) {
    if (!store->is_twin || !store->base) return -1;
#ifdef _WIN32
    (void)async;
    return FlushViewOfFile(store->base, store->used) ? 0 : -1;
#else
    return msync(store->base, store->used, async ? MS_ASYNC : MS_SYNC);
#endif
}

void dt_store_destroy_twin(DRamTileStore *store) {
    if (!store || !store->base || !store->is_twin) return;
    dt_store_save_dir(store);
    dt_store_sync(store, 0);
#ifdef _WIN32
    if (store->locked & DT_LOCKED_BASE) dt_unlock_pages(store->base, store->capacity);
    UnmapViewOfFile(store->base);
    if (store->hMapping) CloseHandle(store->hMapping);
    if (store->hFile && store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
    if (store->kv_base) {
        if (store->locked & DT_LOCKED_KV) dt_unlock_pages(store->kv_base, store->kv_capacity);
        VirtualFree(store->kv_base, 0, MEM_RELEASE);
    }
    if (store->cold_base) {
        if (store->locked & DT_LOCKED_COLD) dt_unlock_pages(store->cold_base, store->cold_capacity);
        if (store->is_cold_twin) {
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
        } else {
            VirtualFree(store->cold_base, 0, MEM_RELEASE);
        }
    }
#else
    if (store->locked & DT_LOCKED_BASE) dt_unlock_pages(store->base, store->capacity);
    munmap(store->base, store->capacity);
    if (store->kv_base) {
        if (store->locked & DT_LOCKED_KV) dt_unlock_pages(store->kv_base, store->kv_capacity);
        munmap(store->kv_base, store->kv_capacity);
    }
    if (store->cold_base) {
        if (store->locked & DT_LOCKED_COLD) dt_unlock_pages(store->cold_base, store->cold_capacity);
        munmap(store->cold_base, store->cold_capacity);
    }
    if (store->fd >= 0) close(store->fd);
    if (store->is_cold_twin && store->cold_fd >= 0) close(store->cold_fd);
#endif
    store->locked = 0;
    memset(store, 0, sizeof(*store));
}

void dt_store_destroy_twinv(DRamTileStore *store) {
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
            if (idx > 0) dt_store_save_dir_v2(store, views, (uint32_t)idx);
            else dt_store_save_dir(store);
            free(views);
        } else {
            dt_store_save_dir(store);
        }
    } else {
        dt_store_save_dir(store);
    }
    dt_store_sync(store, 0);
#ifdef _WIN32
    if (store->locked & DT_LOCKED_BASE) dt_unlock_pages(store->base, store->capacity);
    UnmapViewOfFile(store->base);
    if (store->hMapping) CloseHandle(store->hMapping);
    if (store->hFile && store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
    if (store->kv_base) {
        if (store->locked & DT_LOCKED_KV) dt_unlock_pages(store->kv_base, store->kv_capacity);
        VirtualFree(store->kv_base, 0, MEM_RELEASE);
    }
    if (store->cold_base) {
        if (store->locked & DT_LOCKED_COLD) dt_unlock_pages(store->cold_base, store->cold_capacity);
        if (store->is_cold_twin) {
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
        } else {
            VirtualFree(store->cold_base, 0, MEM_RELEASE);
        }
    }
#else
    if (store->locked & DT_LOCKED_BASE) dt_unlock_pages(store->base, store->capacity);
    munmap(store->base, store->capacity);
    if (store->kv_base) {
        if (store->locked & DT_LOCKED_KV) dt_unlock_pages(store->kv_base, store->kv_capacity);
        munmap(store->kv_base, store->kv_capacity);
    }
    if (store->cold_base) {
        if (store->locked & DT_LOCKED_COLD) dt_unlock_pages(store->cold_base, store->cold_capacity);
        munmap(store->cold_base, store->cold_capacity);
    }
    if (store->fd >= 0) close(store->fd);
    if (store->is_cold_twin && store->cold_fd >= 0) close(store->cold_fd);
#endif
    store->locked = 0;
    memset(store, 0, sizeof(*store));
}

/* ── View API ── */

uint8_t *dt_putv(DRamTileStore *store, const char *name, uint32_t dtype,
                  int ndim, const uint32_t *shape,
                  const uint8_t *data, size_t sz)
{
    return dt_put(store, name, data, sz);
}

DtTensorView dt_getv(DRamTileStore *store, const char *name) {
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

DtTensorView dt_view(DRamTileStore *store, const char *name,
                      uint32_t dtype, int ndim, const uint32_t *shape)
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
    if (ndim > DT_MAX_NDIM) ndim = DT_MAX_NDIM;
    for (int i = 0; i < ndim; i++)
        v.shape[i] = shape ? shape[i] : 0;
    strncpy(v.name, name, DT_NAME_MAX - 1);
    return v;
}

int dt_store_foreach(DRamTileStore *store, DtTensorCallback callback, void *user) {
    if (!store->is_twin) return -1;
    int count = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        if (store->hash[i].dram_addr & (DT_KV_FLAG | DT_BOND_FLAG)) continue;
        DtTensorView v;
        memset(&v, 0, sizeof(v));
        v.data      = dt_entry_ptr(store, i);
        v.offset    = store->hash[i].offset;
        v.nbytes    = store->hash[i].size;
        v.dram_addr = store->hash[i].dram_addr;
        if (callback && callback(&v, user) != 0)
            break;
        count++;
    }
    return count;
}

size_t dt_store_total_bytes(DRamTileStore *store) {
    size_t total = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        if (store->hash[i].dram_addr & (DT_KV_FLAG | DT_BOND_FLAG)) continue;
        total += store->hash[i].size;
    }
    return total;
}

int dt_store_check_twin(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (sz < (long)DT_FILE_HDR_SZ) return -1;
    return 0;
}

DtTensorView dt_resolve(DRamTileStore *store, uint32_t dram_addr) {
    DtTensorView v;
    memset(&v, 0, sizeof(v));
    uint32_t slot = dram_addr % DT_HASH_SLOTS;
    if ((store->hash[slot].dram_addr & ~DT_FLAGS_MASK) !=
        (dram_addr & ~DT_FLAGS_MASK))
        return v;
    v.data      = dt_routed_ptr(store, slot);
    v.offset    = store->hash[slot].offset;
    v.nbytes    = store->hash[slot].size;
    v.dram_addr = store->hash[slot].dram_addr;
    return v;
}

int dt_migrate_promote_one(DRamTileStore *store, int slot, size_t dir_reserve) {
    if (!(store->hash[slot].dram_addr & DT_BOND_FLAG)) return 0;
    size_t sz = store->hash[slot].size;
    size_t off = (store->used + 63) & ~63;
    size_t usable = (dir_reserve > 0 && store->capacity > dir_reserve)
                  ? store->capacity - dir_reserve : store->capacity;
    if (off + sz > usable) return 0;
    memcpy(store->base + off, store->cold_base + store->hash[slot].cold_offset, sz);
    store->hash[slot].dram_addr &= ~DT_BOND_FLAG;
    store->hash[slot].offset     = off;
    store->hash[slot].cold_offset = 0;
    store->used = off + sz;
    return 1;
}

int dt_migrate_step(DRamTileStore *store, int max_entries,
                     size_t dir_reserve, int promote_newest)
{
    if (!store->cold_base || !store->base || max_entries <= 0) return 0;
    int promoted = 0;
    uint8_t skip[DT_HASH_SLOTS];
    memset(skip, 0, sizeof(skip));
    for (int pass = 0; pass < 2 && promoted < max_entries; pass++) {
        int best_slot = -1;
        uint32_t best_tick = promote_newest ? 0 : UINT32_MAX;
        for (int i = 0; i < DT_HASH_SLOTS; i++) {
            if (skip[i]) continue;
            if (!(store->hash[i].dram_addr & DT_BOND_FLAG)) continue;
            if (store->hash[i].dram_addr & DT_KV_FLAG) continue;
            uint32_t t = store->hash[i].session_tick;
            int better = promote_newest ? (t > best_tick) : (t < best_tick);
            if (best_slot < 0 || better) { best_slot = i; best_tick = t; }
        }
        if (best_slot < 0) break;
        if (dt_migrate_promote_one(store, best_slot, dir_reserve)) promoted++;
        else skip[best_slot] = 1;
    }
    return promoted;
}

int dt_evict_step(DRamTileStore *store, int max_entries) {
    if (!store->cold_base || max_entries <= 0) return 0;
    int evicted = 0;
    for (int pass = 0; pass < max_entries; pass++) {
        int worst_slot = -1;
        uint32_t worst_tick = UINT32_MAX;
        for (int i = 0; i < DT_HASH_SLOTS; i++) {
            if (!(store->hash[i].dram_addr & DT_BOND_FLAG)) continue;
            if (store->hash[i].dram_addr & DT_KV_FLAG) continue;
            uint32_t t = store->hash[i].session_tick;
            if (worst_slot < 0 || t < worst_tick) { worst_slot = i; worst_tick = t; }
        }
        if (worst_slot < 0) break;
        if (store->evict_cb)
            store->evict_cb(store->hash[worst_slot].name, store->evict_user);
        store->hash[worst_slot].dram_addr = 0;
        store->n_stored--;
        evicted++;
    }
    if (evicted > 0) dt_cold_rebuild_used(store);
    return evicted;
}

int dt_cold_make_room(DRamTileStore *store, size_t sz, int max_evict) {
    if (!store->cold_base) return -1;
    size_t off = (store->cold_used + 63) & ~63;
    if (off + sz <= store->cold_capacity) return 0;
    int total = 0;
    int limit = max_evict > 0 ? max_evict : DT_HASH_SLOTS;
    while (total < limit) {
        int n = dt_evict_step(store, 1);
        if (n == 0) return -1;
        total += n;
        off = (store->cold_used + 63) & ~63;
        if (off + sz <= store->cold_capacity) return total;
    }
    return -1;
}