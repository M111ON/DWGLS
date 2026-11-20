/*
 * saturn_ring.h — Saturn-ring store: fixed radii + ratio needle (2026-09-17)
 * ═══════════════════════════════════════════════════════════════════════════
 * Owner's design (analogical: Saturn + vinyl player).
 *
 * v2 (this revision): ring PAYLOAD slots live in VRAM on GPU 1 (preferred)
 * via cudaMalloc; metadata (valid/ticks/keys) stays host-side (tiny, O(N)).
 * Fixed capacity is enforced by slot count — no bump, no holes, no compact.
 * Upload path: cudaMemcpy H2D on place, D2H on find (into per-ring staging).
 *
 * Requires: CUDA runtime (compile with -I<cuda>/include -L<cuda>/lib/x64 -lcudart).
 * Falls back to device 0 if GPU 1 is unavailable.
 */
#ifndef SATURN_RING_H
#define SATURN_RING_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <cuda_runtime.h>

#define SATURN_MAGIC   0x5352544Eu  /* "SRTN" */
#define SATURN_VERSION 2u

/* one ring: N uniform slots, fixed forever after init (payload in VRAM) */
typedef struct {
    uint8_t  *d_slots;    /* device VRAM: N * slot_sz bytes */
    uint8_t  *valid;      /* host: N flags */
    uint32_t *ticks;      /* host: N last-access ticks */
    uint32_t *keys;       /* host: N name hashes */
    uint8_t  *h_staging;  /* host: slot_sz bytes, D2H read cache */
    uint32_t  n_slots;
    uint32_t  slot_sz;
    uint32_t  n_valid;
    uint64_t  n_place;
    int       device;
} SaturnRing;

typedef struct {
    uint32_t   magic;
    SaturnRing hot;        /* ring0 */
    SaturnRing cold;       /* ring1 */
    uint32_t   tick;
    uint32_t   hot_weight; /* ratio needle: hot placements per cycle */
    uint32_t   cold_weight;
    /* config (for reset/recovery) */
    uint32_t   cfg_hot_slots;
    uint32_t   cfg_cold_slots;
    uint32_t   cfg_slot_sz;
    uint32_t   cfg_hot_w;
    uint32_t   cfg_cold_w;
    int        faulted;
    /* mirror window (zones 1..mirror_n are DRAM∩VRAM mirrored) */
    uint32_t   mirror_n;
    uint8_t   *mirror_slots;  /* host: mirror_n * slot_sz */
    uint8_t   *mirror_valid;  /* host: mirror_n */
    uint32_t  *mirror_ticks;  /* host: mirror_n */
    uint32_t  *mirror_keys;   /* host: mirror_n */
    uint64_t   n_promote;
    uint64_t   n_demote;
    uint64_t   n_evict;
    uint64_t   n_hot_routed;   /* quota accounting (logical placements) */
    uint64_t   n_cold_routed;
    int        device;
} SaturnStore;

static inline uint32_t saturn_hash(const char *name) {
    uint32_t h = 0x811c9dc5u;
    while (*name) { h ^= (uint8_t)(*name++); h *= 0x01000193u; }
    return h;
}

static inline int saturn_ring_init(SaturnRing *r, uint32_t n_slots, uint32_t slot_sz, int device) {
    memset(r, 0, sizeof(*r));
    r->device = device;
    r->n_slots = n_slots;
    r->slot_sz = slot_sz;
    r->h_staging = (uint8_t *)malloc(slot_sz);
    r->valid = (uint8_t *)calloc(n_slots, 1);
    r->ticks = (uint32_t *)calloc(n_slots, sizeof(uint32_t));
    r->keys  = (uint32_t *)calloc(n_slots, sizeof(uint32_t));
    if (!r->h_staging || !r->valid || !r->ticks || !r->keys) {
        free(r->h_staging); free(r->valid); free(r->ticks); free(r->keys);
        memset(r, 0, sizeof(*r));
        return -1;
    }
    cudaSetDevice(device);
    cudaError_t e = cudaMalloc((void **)&r->d_slots, (size_t)n_slots * slot_sz);
    if (e != cudaSuccess) {
        free(r->h_staging); free(r->valid); free(r->ticks); free(r->keys);
        memset(r, 0, sizeof(*r));
        return -1;
    }
    cudaMemset(r->d_slots, 0, (size_t)n_slots * slot_sz);
    return 0;
}

static inline void saturn_ring_free(SaturnRing *r) {
    if (r->d_slots) { cudaSetDevice(r->device); cudaFree(r->d_slots); }
    free(r->h_staging);
    free(r->valid);
    free(r->ticks);
    free(r->keys);
    memset(r, 0, sizeof(*r));
}

static inline int saturn_init(SaturnStore *s, uint32_t hot_slots, uint32_t cold_slots,
                              uint32_t slot_sz, uint32_t hot_w, uint32_t cold_w) {
    memset(s, 0, sizeof(*s));
    s->magic = SATURN_MAGIC;
    int ndev = 0;
    cudaGetDeviceCount(&ndev);
    s->device = (ndev >= 2) ? 1 : 0;
    if (saturn_ring_init(&s->hot, hot_slots, slot_sz, s->device) != 0) return -1;
    if (saturn_ring_init(&s->cold, cold_slots, slot_sz, s->device) != 0) {
        saturn_ring_free(&s->hot);
        return -1;
    }
    s->hot_weight = hot_w ? hot_w : 3;
    s->cold_weight = cold_w ? cold_w : 1;
    s->cfg_hot_slots = hot_slots;
    s->cfg_cold_slots = cold_slots;
    s->cfg_slot_sz = slot_sz;
    s->cfg_hot_w = s->hot_weight;
    s->cfg_cold_w = s->cold_weight;
    s->faulted = 0;
    s->mirror_n = (hot_slots >= 4) ? 4 : hot_slots;
    if (s->mirror_n > 0) {
        s->mirror_slots = (uint8_t *)malloc((size_t)s->mirror_n * slot_sz);
        s->mirror_valid = (uint8_t *)calloc(s->mirror_n, 1);
        s->mirror_ticks = (uint32_t *)calloc(s->mirror_n, sizeof(uint32_t));
        s->mirror_keys  = (uint32_t *)calloc(s->mirror_n, sizeof(uint32_t));
        if (!s->mirror_slots || !s->mirror_valid || !s->mirror_ticks || !s->mirror_keys) {
            free(s->mirror_slots); free(s->mirror_valid);
            free(s->mirror_ticks); free(s->mirror_keys);
            s->mirror_slots = NULL; s->mirror_valid = NULL;
            s->mirror_ticks = NULL; s->mirror_keys = NULL;
            s->mirror_n = 0;
        }
    }
    return 0;
}

static inline void saturn_free(SaturnStore *s) {
    saturn_ring_free(&s->hot);
    saturn_ring_free(&s->cold);
    free(s->mirror_slots);
    free(s->mirror_valid);
    free(s->mirror_ticks);
    free(s->mirror_keys);
    s->mirror_slots = NULL; s->mirror_valid = NULL;
    s->mirror_ticks = NULL; s->mirror_keys = NULL;
    s->mirror_n = 0;
    s->magic = 0;
}

/* reset/recovery: rebuild store from saved config (loses data, clears fault) */
static inline int saturn_reset(SaturnStore *s) {
    if (!s->cfg_hot_slots) return -1;
    saturn_free(s);
    int r = saturn_init(s, s->cfg_hot_slots, s->cfg_cold_slots,
                        s->cfg_slot_sz, s->cfg_hot_w, s->cfg_cold_w);
    if (r == 0) s->faulted = 0;
    else s->faulted = 1;
    return r;
}

/* place into ring: free slot preferred, else replace lowest-tick (fixed capacity) */
static inline int saturn_ring_place(SaturnStore *s, SaturnRing *r,
                                    uint32_t key, const uint8_t *data, uint32_t sz) {
    if (sz > r->slot_sz) return -1;
    cudaSetDevice(r->device);
    int free_idx = -1;
    uint32_t start = key % r->n_slots;
    for (uint32_t i = 0; i < r->n_slots; i++) {
        uint32_t cand = (start + i) % r->n_slots;
        if (!r->valid[cand]) { free_idx = (int)cand; break; }
    }
    uint32_t sl;
    if (free_idx >= 0) {
        sl = (uint32_t)free_idx;
        r->n_valid++;
    } else {
        sl = 0;
        for (uint32_t i = 1; i < r->n_slots; i++)
            if (r->ticks[i] < r->ticks[sl]) sl = i;
        s->n_evict++;
    }
    if (cudaMemcpy(r->d_slots + (size_t)sl * r->slot_sz, data, sz, cudaMemcpyHostToDevice) != cudaSuccess) {
        s->faulted = 1;
        return -1;
    }
    if (sz < r->slot_sz)
        cudaMemset(r->d_slots + (size_t)sl * r->slot_sz + sz, 0, r->slot_sz - sz);
    r->valid[sl] = 1;
    r->keys[sl] = key;
    r->ticks[sl] = ++s->tick;
    r->n_place++;
    return (free_idx >= 0) ? 0 : 1;
}

/* find: linear probe from key slot, key-verified; D2H into staging on hit */
static inline uint8_t *saturn_ring_find(SaturnStore *s, SaturnRing *r, uint32_t key) {
    cudaSetDevice(r->device);
    uint32_t start = key % r->n_slots;
    for (uint32_t i = 0; i < r->n_slots; i++) {
        uint32_t sl = (start + i) % r->n_slots;
        if (r->valid[sl] && r->keys[sl] == key) {
            r->ticks[sl] = ++s->tick;
            if (cudaMemcpy(r->h_staging, r->d_slots + (size_t)sl * r->slot_sz,
                           r->slot_sz, cudaMemcpyDeviceToHost) != cudaSuccess) {
                s->faulted = 1;
                return NULL;
            }
            return r->h_staging;
        }
    }
    return NULL;
}

/* needle: promote = place hot; on overflow demote lowest-tick hot -> cold first */
static inline int saturn_promote(SaturnStore *s, const char *name,
                                 const uint8_t *data, uint32_t sz) {
    uint32_t key = saturn_hash(name);
    SaturnRing *hot = &s->hot;
    /* already in hot? update in place */
    uint32_t start = key % hot->n_slots;
    for (uint32_t i = 0; i < hot->n_slots; i++) {
        uint32_t sl = (start + i) % hot->n_slots;
        if (hot->valid[sl] && hot->keys[sl] == key) {
            cudaSetDevice(hot->device);
            if (cudaMemcpy(hot->d_slots + (size_t)sl * hot->slot_sz, data, sz,
                           cudaMemcpyHostToDevice) != cudaSuccess) return -1;
            if (sz < hot->slot_sz)
                cudaMemset(hot->d_slots + (size_t)sl * hot->slot_sz + sz, 0,
                           hot->slot_sz - sz);
            hot->ticks[sl] = ++s->tick;
            s->n_promote++;
            return 0;
        }
    }
    /* overflow: demote lowest-tick hot -> cold, then place */
    if (hot->n_valid >= hot->n_slots && hot->n_slots > 0) {
        uint32_t lo = 0;
        for (uint32_t i = 1; i < hot->n_slots; i++)
            if (hot->ticks[i] < hot->ticks[lo]) lo = i;
        cudaSetDevice(hot->device);
        if (cudaMemcpy(hot->h_staging, hot->d_slots + (size_t)lo * hot->slot_sz,
                       hot->slot_sz, cudaMemcpyDeviceToHost) == cudaSuccess) {
            saturn_ring_place(s, &s->cold, hot->keys[lo], hot->h_staging,
                              hot->slot_sz);
            s->n_cold_routed++;
            s->n_demote++;
        }
        hot->valid[lo] = 0;
        if (hot->n_valid > 0) hot->n_valid--;
    }
    int r = saturn_ring_place(s, hot, key, data, sz);
    if (r < 0) return -1;
    s->n_hot_routed++;
    s->n_promote++;
    return r;
}

static inline uint8_t *saturn_get(SaturnStore *s, const char *name) {
    uint32_t key = saturn_hash(name);
    uint8_t *p = saturn_ring_find(s, &s->hot, key);
    if (p) return p;
    return saturn_ring_find(s, &s->cold, key);
}

/* device pointer for GPU consumers (no copy); NULL if absent */
static inline uint8_t *saturn_get_devptr(SaturnStore *s, const char *name) {
    uint32_t key = saturn_hash(name);
    SaturnRing *rings[2] = { &s->hot, &s->cold };
    for (int ri = 0; ri < 2; ri++) {
        SaturnRing *r = rings[ri];
        uint32_t start = key % r->n_slots;
        for (uint32_t i = 0; i < r->n_slots; i++) {
            uint32_t sl = (start + i) % r->n_slots;
            if (r->valid[sl] && r->keys[sl] == key)
                return r->d_slots + (size_t)sl * r->slot_sz;
        }
    }
    return NULL;
}

static inline int saturn_demote_cold(SaturnStore *s, const char *name,
                                     const uint8_t *data, uint32_t sz) {
    uint32_t key = saturn_hash(name);
    int r = saturn_ring_place(s, &s->cold, key, data, sz);
    if (r < 0) return -1;
    s->n_cold_routed++;
    s->n_demote++;
    return r;
}

/* thread-safe read: copy slot D2H into caller buffer (no shared staging) */
static inline int saturn_read(SaturnStore *s, const char *name,
                             uint8_t *dst, uint32_t sz) {
    if (!dst) return -1;
    uint32_t key = saturn_hash(name);
    SaturnRing *rings[2] = { &s->hot, &s->cold };
    for (int ri = 0; ri < 2; ri++) {
        SaturnRing *r = rings[ri];
        if (sz > r->slot_sz) continue;
        uint32_t start = key % r->n_slots;
        for (uint32_t i = 0; i < r->n_slots; i++) {
            uint32_t sl = (start + i) % r->n_slots;
            if (r->valid[sl] && r->keys[sl] == key) {
                cudaSetDevice(r->device);
                r->ticks[sl] = ++s->tick;
                if (cudaMemcpy(dst, r->d_slots + (size_t)sl * r->slot_sz, sz,
                               cudaMemcpyDeviceToHost) != cudaSuccess)
                    return -1;
                return 0;
            }
        }
    }
    return -1;
}

/* mirror window: snapshot zones 1..mirror_n (hot slots) to DRAM */
static inline int saturn_mirror_snapshot(SaturnStore *s) {
    if (s->mirror_n == 0 || !s->mirror_slots) return -1;
    SaturnRing *hot = &s->hot;
    cudaSetDevice(hot->device);
    for (uint32_t i = 0; i < s->mirror_n; i++) {
        if (hot->valid[i]) {
            if (cudaMemcpy(s->mirror_slots + (size_t)i * hot->slot_sz,
                           hot->d_slots + (size_t)i * hot->slot_sz,
                           hot->slot_sz, cudaMemcpyDeviceToHost) != cudaSuccess)
                return -1;
        }
        s->mirror_valid[i] = hot->valid[i];
        s->mirror_ticks[i] = hot->ticks[i];
        s->mirror_keys[i]  = hot->keys[i];
    }
    return 0;
}

/* sync-back: restore zones 1..mirror_n from DRAM mirror (zone 4 baseline) */
static inline int saturn_mirror_sync_back(SaturnStore *s) {
    if (s->mirror_n == 0 || !s->mirror_slots) return -1;
    SaturnRing *hot = &s->hot;
    cudaSetDevice(hot->device);
    for (uint32_t i = 0; i < s->mirror_n; i++) {
        if (s->mirror_valid[i]) {
            if (cudaMemcpy(hot->d_slots + (size_t)i * hot->slot_sz,
                           s->mirror_slots + (size_t)i * hot->slot_sz,
                           hot->slot_sz, cudaMemcpyHostToDevice) != cudaSuccess)
                return -1;
        }
        hot->valid[i] = s->mirror_valid[i];
        hot->ticks[i] = s->mirror_ticks[i];
        hot->keys[i]  = s->mirror_keys[i];
    }
    /* recompute n_valid for mirrored prefix */
    uint32_t nv = 0;
    for (uint32_t i = 0; i < hot->n_slots; i++) if (hot->valid[i]) nv++;
    hot->n_valid = nv;
    return 0;
}

/* carry fault recovery: sync-back to mirror line, then re-promote carry zones upward */
static inline int saturn_carry_recover(SaturnStore *s) {
    SaturnRing *hot = &s->hot;
    /* save carry-zone entries (key + data) before dropping */
    uint32_t ncarry = hot->n_slots - s->mirror_n;
    uint32_t *saved_keys = NULL;
    uint8_t  *saved_data = NULL;
    uint32_t  nsaved = 0;
    if (ncarry > 0) {
        saved_keys = (uint32_t *)malloc(ncarry * sizeof(uint32_t));
        saved_data = (uint8_t *)malloc((size_t)ncarry * hot->slot_sz);
        if (saved_keys && saved_data) {
            cudaSetDevice(hot->device);
            for (uint32_t i = s->mirror_n; i < hot->n_slots; i++) {
                if (hot->valid[i]) {
                    if (cudaMemcpy(saved_data + (size_t)nsaved * hot->slot_sz,
                                   hot->d_slots + (size_t)i * hot->slot_sz,
                                   hot->slot_sz, cudaMemcpyDeviceToHost) == cudaSuccess) {
                        saved_keys[nsaved] = hot->keys[i];
                        nsaved++;
                    }
                }
            }
        }
    }
    if (saturn_mirror_sync_back(s) != 0) {
        free(saved_keys); free(saved_data);
        return -1;
    }
    /* drop carry zones */
    for (uint32_t i = s->mirror_n; i < hot->n_slots; i++) {
        hot->valid[i] = 0;
        hot->ticks[i] = 0;
        hot->keys[i]  = 0;
    }
    uint32_t nv = 0;
    for (uint32_t i = 0; i < hot->n_slots; i++) if (hot->valid[i]) nv++;
    hot->n_valid = nv;
    /* cold staging dropped */
    for (uint32_t i = 0; i < s->cold.n_slots; i++) {
        s->cold.valid[i] = 0; s->cold.ticks[i] = 0; s->cold.keys[i] = 0;
    }
    s->cold.n_valid = 0;
    /* re-promote saved carry entries upward */
    for (uint32_t i = 0; i < nsaved; i++) {
        /* find name? we only have key; promote by key-derived? need name.
           Use a synthetic name from key is not reversible. Instead place directly. */
        /* place directly into hot (free carry slot expected) */
        uint32_t key = saved_keys[i];
        /* find free slot */
        int free_idx = -1;
        for (uint32_t j = s->mirror_n; j < hot->n_slots; j++) {
            if (!hot->valid[j]) { free_idx = (int)j; break; }
        }
        if (free_idx < 0) {
            /* hot full: demote lowest-tick among carry+mirror? use promote path */
            free_idx = 0;
            for (uint32_t j = 1; j < hot->n_slots; j++)
                if (hot->ticks[j] < hot->ticks[free_idx]) free_idx = (int)j;
        }
        uint32_t sl = (uint32_t)free_idx;
        cudaSetDevice(hot->device);
        cudaMemcpy(hot->d_slots + (size_t)sl * hot->slot_sz,
                   saved_data + (size_t)i * hot->slot_sz,
                   hot->slot_sz, cudaMemcpyHostToDevice);
        hot->valid[sl] = 1;
        hot->keys[sl] = key;
        hot->ticks[sl] = ++s->tick;
        if (hot->n_valid < hot->n_slots) hot->n_valid++;
        s->n_hot_routed++;
        s->n_promote++;
    }
    free(saved_keys); free(saved_data);
    return 0;
}

/* invariants */
static inline int saturn_verify(SaturnStore *s) {
    if (s->magic != SATURN_MAGIC) return -1;
    if (!s->hot.d_slots || !s->cold.d_slots) return -4;
    if (s->hot.n_valid > s->hot.n_slots) return -2;
    if (s->cold.n_valid > s->cold.n_slots) return -3;
    return 0;
}

#endif /* SATURN_RING_H */
