/*
 * kv_page_rdh.h — RDH-addressed KV page store
 *
 * Address = (ring, wedge, mirror, u, v) → deterministic slot key
 *   ring   = attention layer index
 *   wedge  = head group (0=all heads, expandable to per-head)
 *   mirror = K(0) / V(1)
 *   u      = position range index (page_start / 128)
 *   v      = 0 (reserved)
 *
 * Slot key = pure integer formula (5 params) — no array lookup, no collision.
 * Each page = 1 layer × (K or V) × 128 tokens × all heads.
 *
 * vs kv_page_store.h (flat page_id 0..255):
 *   - Address has geometric meaning: (layer, K/V, pos_range)
 *   - Slot = formula, not page_id × slot_size
 *   - Evict per-layer, not all-layers-at-once
 *   - Deterministic: reconstruct address from (ring,wedge,mirror,u) without table
 */

#ifndef KV_PAGE_RDH_H
#define KV_PAGE_RDH_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "rdh_addr.h"
#include "../geopixel/binary_shell_codec.h"

/* ── RDH dimension constants ───────────────────────────────────
 * These are the "5 parameters" of the RDH addressing system,
 * reparameterized for KV cache:
 *   ring   = layer index (0..63, max attention layers)
 *   wedge  = head group (1 = all heads, expandable to per-head)
 *   mirror = K(0) or V(1)
 *   u      = position range index (0..255, 128 tokens each)
 *   v      = reserved (0)
 */
#define KV_RDH_N_RINGS     64        /* max attention layers */
#define KV_RDH_N_WEDGES    1         /* head groups (expand later) */
#define KV_RDH_N_MIRROR    2         /* K=0, V=1 */
#define KV_RDH_MAX_U       256       /* max position ranges (32K ctx / 128) */
#define KV_RDH_N_V         1         /* reserved */

#define KV_RDH_PAGE_SIZE   128       /* tokens per page */
#define KV_RDH_SLOT_META   16        /* header bytes before compressed data */
#define KV_RDH_MAGIC       0x52444856  /* "VHDR" little-endian */

/* ── Slot metadata structure ────────────────────────────────────
 * Stored at start of each mmap slot.
 */
typedef struct {
    uint32_t magic;          /* KV_RDH_MAGIC */
    uint32_t comp_size;      /* compressed data size */
    uint32_t orig_size;      /* uncompressed size */
    uint32_t layer;          /* layer index (ring) */
    uint32_t direction;      /* 0=K, 1=V (mirror) */
    uint32_t pos_start;      /* token position start */
    uint32_t lru_tick;       /* last access tick */
} __attribute__((packed)) KVRdhSlotMeta;

/* ── RDH page store ─────────────────────────────────────────────*/
typedef struct {
    /* mmap backing */
    uint8_t *mmap_base;
    size_t   mmap_capacity;
    size_t   mmap_used;
    int      is_mmap;

    /* Slot geometry */
    size_t   slot_size;         /* bytes per slot (aligned) */
    size_t   slot_capacity;     /* total slots available */

    /* Per-layer KV info (from llama) */
    void    *k_tensors[KV_RDH_N_RINGS];
    void    *v_tensors[KV_RDH_N_RINGS];
    uint8_t *k_data[KV_RDH_N_RINGS];
    uint8_t *v_data[KV_RDH_N_RINGS];
    size_t   k_nb1[KV_RDH_N_RINGS];    /* bytes per token position for K */
    size_t   v_nb1[KV_RDH_N_RINGS];    /* bytes per token position for V */
    size_t   k_size[KV_RDH_N_RINGS];
    size_t   v_size[KV_RDH_N_RINGS];
    int      n_embd_k[KV_RDH_N_RINGS];
    int      layer_id[KV_RDH_N_RINGS];
    int      n_attn_layers;
    int      n_ctx;

    /* Page tracking — RDH (ring, 0, mirror, u) = key
     * No page_valid[] array — slot validity = (magic == KV_RDH_MAGIC) */
    uint32_t lru_tick;                  /* global LRU counter */
    int      n_pages_per_layer;         /* n_ctx / KV_PAGE_SIZE */
    int      n_evicted;                 /* count of evicted slots */
    size_t   total_snap_bytes;
    size_t   total_orig_bytes;

    /* Eviction tracking (bitmap of evicted slots: 1 = evicted) */
    uint8_t *evicted_map;               /* bitmap of evicted pages */
    size_t   evicted_map_size;          /* bytes in bitmap */

    /* Stats */
    int      enabled;
    int      n_slots;                   /* total RDH slots used */
} KVPageRDH;

/* ── RDH config instance for KV page ───────────────────────────*/
static inline const RDHConfig* kv_rdh_config(void) {
    static const RDHConfig cfg = { KV_RDH_N_RINGS, KV_RDH_N_WEDGES,
                                   KV_RDH_N_MIRROR, KV_RDH_MAX_U, KV_RDH_N_V };
    return &cfg;
}

/* ── RDH key via rdh_addr.h ────────────────────────────────────*/
static inline int kv_rdh_key(int ring, int wedge, int mirror, int u) {
    return (int)rdh_key(kv_rdh_config(), ring, wedge, mirror, u, 0);
}

static inline int kv_rdh_ring(int key) {
    int64_t r, w, m, u;
    rdh_decompose(kv_rdh_config(), key, &r, &w, &m, &u);
    return (int)r;
}
static inline int kv_rdh_mirror(int key) {
    int64_t r, w, m, u;
    rdh_decompose(kv_rdh_config(), key, &r, &w, &m, &u);
    return (int)m;
}
static inline int kv_rdh_u(int key) {
    int64_t r, w, m, u;
    rdh_decompose(kv_rdh_config(), key, &r, &w, &m, &u);
    return (int)u;
}
static inline int kv_rdh_wedge(int key) {
    int64_t r, w, m, u;
    rdh_decompose(kv_rdh_config(), key, &r, &w, &m, &u);
    return (int)w;
}

/* Maximum RDH slot count */
static inline int kv_rdh_max_slots(void) {
    return KV_RDH_N_RINGS * KV_RDH_N_WEDGES * KV_RDH_N_MIRROR * KV_RDH_MAX_U;
}

/* ── Slot pointer ───────────────────────────────────────────────*/
static inline uint8_t *kv_rdh_slot_ptr(KVPageRDH *s, int key) {
    return s->mmap_base + (size_t)key * s->slot_size;
}

/* ── Bitmap helpers for eviction tracking ───────────────────────*/
static inline int kv_rdh_evicted_get(KVPageRDH *s, int key) {
    return (s->evicted_map[key >> 3] >> (key & 7)) & 1;
}
static inline void kv_rdh_evicted_set(KVPageRDH *s, int key, int val) {
    if (val)
        s->evicted_map[key >> 3] |=  (uint8_t)(1u << (key & 7));
    else
        s->evicted_map[key >> 3] &= ~(uint8_t)(1u << (key & 7));
}

/* ── Init ───────────────────────────────────────────────────────*/
static inline int kv_rdh_init(KVPageRDH *s, int n_ctx) {
    memset(s, 0, sizeof(*s));
    s->n_ctx = n_ctx;
    s->n_pages_per_layer = n_ctx / KV_RDH_PAGE_SIZE;
    if (s->n_pages_per_layer > KV_RDH_MAX_U)
        s->n_pages_per_layer = KV_RDH_MAX_U;
    s->n_slots = kv_rdh_max_slots();
    s->enabled = 0;

    fprintf(stderr, "[kv-rdh] init: n_ctx=%d, pages_per_layer=%d, "
        "rdh_slots=%d\n", n_ctx, s->n_pages_per_layer, s->n_slots);
    return 0;
}

/* ── mmap init ──────────────────────────────────────────────────*/
static inline int kv_rdh_mmap_init(KVPageRDH *s, size_t capacity) {
#ifdef _WIN32
    s->mmap_base = (uint8_t*)VirtualAlloc(NULL, capacity,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s->is_mmap = (s->mmap_base != NULL);
#else
    s->mmap_base = (uint8_t*)mmap(NULL, capacity,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    s->is_mmap = (s->mmap_base != MAP_FAILED);
    if (!s->is_mmap) s->mmap_base = NULL;
#endif
    if (!s->mmap_base) {
        fprintf(stderr, "[kv-rdh] mmap failed for %zu bytes, malloc fallback\n", capacity);
        s->mmap_base = (uint8_t*)malloc(capacity);
        s->is_mmap = 0;
    }
    if (!s->mmap_base) return -1;
    s->mmap_capacity = capacity;
    s->mmap_used = 0;
    return 0;
}

/* ── Register layers ────────────────────────────────────────────*/
static inline void kv_rdh_register_layers(KVPageRDH *s,
    void **k_tensors, void **v_tensors,
    uint8_t **k_data, uint8_t **v_data,
    size_t *k_nb1, size_t *v_nb1,
    size_t *k_size, size_t *v_size,
    int *n_embd_k, int *layer_id,
    int n_layers)
{
    if (n_layers > KV_RDH_N_RINGS) n_layers = KV_RDH_N_RINGS;
    s->n_attn_layers = n_layers;

    for (int i = 0; i < n_layers; i++) {
        s->k_tensors[i] = k_tensors[i];
        s->v_tensors[i] = v_tensors[i];
        s->k_data[i]    = k_data[i];
        s->v_data[i]    = v_data[i];
        s->k_nb1[i]     = k_nb1[i];
        s->v_nb1[i]     = v_nb1[i];
        s->k_size[i]    = k_size[i];
        s->v_size[i]    = v_size[i];
        s->n_embd_k[i]  = n_embd_k[i];
        s->layer_id[i]  = layer_id[i];
    }

    /* Slot size = max across layers: K page + V page per slot
     * One slot = (K or V) data for KV_PAGE_SIZE tokens */
    size_t max_layer_bytes = 0;
    for (int i = 0; i < n_layers; i++) {
        size_t k_page = (size_t)KV_RDH_PAGE_SIZE * s->k_nb1[i];
        size_t v_page = (size_t)KV_RDH_PAGE_SIZE * s->v_nb1[i];
        size_t total = k_page > v_page ? k_page : v_page;
        if (total > max_layer_bytes) max_layer_bytes = total;
    }
    s->slot_size = max_layer_bytes + KV_RDH_SLOT_META;
    /* Align to cache line */
    s->slot_size = (s->slot_size + 63) & ~(size_t)63;

    /* Total mmap = all slots × slot_size + 25% headroom */
    size_t total_mmap = (size_t)s->n_slots * s->slot_size;
    total_mmap += total_mmap / 4;
    if (total_mmap < 4u * 1024 * 1024)
        total_mmap = 4u * 1024 * 1024;

    if (kv_rdh_mmap_init(s, total_mmap) != 0) {
        fprintf(stderr, "[kv-rdh] mmap init failed!\n");
        return;
    }

    /* Eviction bitmap */
    s->evicted_map_size = ((size_t)s->n_slots + 7) / 8;
    s->evicted_map = (uint8_t*)calloc(s->evicted_map_size, 1);
    if (!s->evicted_map) {
        fprintf(stderr, "[kv-rdh] evicted_map alloc failed!\n");
        return;
    }

    s->total_orig_bytes = (size_t)s->n_pages_per_layer * max_layer_bytes * 2 * n_layers;
    s->enabled = 1;

    fprintf(stderr, "[kv-rdh] registered %d rings (layers), "
        "pages_per_layer=%d, slot_size=%zu, mmap=%zu MB, "
        "evicted_map=%zu bytes\n",
        n_layers, s->n_pages_per_layer, s->slot_size,
        total_mmap / (1024 * 1024), s->evicted_map_size);
}

/* ── Snapshot one RDH page: (ring, wedge, mirror, u) → compress → slot ──*/
static inline int kv_rdh_snapshot(KVPageRDH *s, int ring, int wedge, int mirror, int u) {
    if (!s->enabled) return -1;
    if (ring < 0 || ring >= s->n_attn_layers) return -1;
    if (wedge < 0 || wedge >= KV_RDH_N_WEDGES) return -1;
    if (mirror < 0 || mirror >= KV_RDH_N_MIRROR) return -1;
    if (u < 0 || u >= s->n_pages_per_layer) return -1;

    int key = kv_rdh_key(ring, wedge, mirror, u);
    uint8_t *slot = kv_rdh_slot_ptr(s, key);
    KVRdhSlotMeta *meta = (KVRdhSlotMeta *)slot;

    /* Skip if already valid */
    if (meta->magic == KV_RDH_MAGIC) return 0;

    int start_pos = u * KV_RDH_PAGE_SIZE;
    int end_pos   = start_pos + KV_RDH_PAGE_SIZE;
    if (end_pos > s->n_ctx) end_pos = s->n_ctx;
    int page_tokens = end_pos - start_pos;
    if (page_tokens <= 0) return -1;

    /* Get source data: K or V page for this layer */
    uint8_t *src_base = (mirror == 0) ? s->k_data[ring] : s->v_data[ring];
    size_t   nb1 = (mirror == 0) ? s->k_nb1[ring] : s->v_nb1[ring];
    size_t   page_bytes = (size_t)page_tokens * nb1;

    /* Compress */
    void *comp = NULL;
    size_t comp_size = 0;

    uint64_t n_chunks = (page_bytes + 63) / 64;
    uint64_t max_enc  = n_chunks * 70 + 16;
    uint8_t *enc = (uint8_t *)malloc((size_t)max_enc);
    if (!enc) return -1;

    size_t enc_pos = 16;
    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        size_t off = (size_t)(ci * 64);
        size_t remain = page_bytes > off ? page_bytes - off : 0;
        uint8_t chunk64[64];
        memset(chunk64, 0, 64);
        memcpy(chunk64, src_base + start_pos * nb1 + off,
               remain < 64 ? remain : 64);
        enc_pos += bin_encode_chunk(enc + enc_pos, chunk64, &(BinChunkResult){0});
    }

    *(uint32_t *)(enc + 0)  = KV_RDH_MAGIC;
    *(uint64_t *)(enc + 4)  = (uint64_t)page_bytes;
    *(uint32_t *)(enc + 12) = (uint32_t)n_chunks;

    size_t total = enc_pos;
    double ratio = (double)page_bytes / (double)(total > 16 ? total - 16 : 1);

    if (total >= page_bytes || ratio < 1.1) {
        free(enc);
        uint8_t *raw_out = (uint8_t *)malloc(16 + page_bytes);
        if (!raw_out) return -1;
        *(uint32_t *)(raw_out + 0) = KV_RDH_MAGIC;
        *(uint64_t *)(raw_out + 4) = (uint64_t)page_bytes;
        *(uint32_t *)(raw_out + 12) = 0;
        memcpy(raw_out + 16, src_base + start_pos * nb1, page_bytes);
        comp = raw_out;
        comp_size = 16 + page_bytes;
    } else {
        comp = enc;
        comp_size = total;
    }

    /* Check slot room */
    if (KV_RDH_SLOT_META + comp_size > s->slot_size) {
        free(comp);
        return -1;
    }

    /* Write slot metadata + compressed data */
    meta->magic     = KV_RDH_MAGIC;
    meta->comp_size = (uint32_t)comp_size;
    meta->orig_size = (uint32_t)page_bytes;
    meta->layer     = (uint32_t)ring;
    meta->direction = (uint32_t)mirror;
    meta->pos_start = (uint32_t)start_pos;
    meta->lru_tick  = ++s->lru_tick;

    memcpy(slot + sizeof(KVRdhSlotMeta), comp, comp_size);
    free(comp);

    s->total_snap_bytes += comp_size;
    kv_rdh_evicted_set(s, key, 0);

    fprintf(stderr, "[kv-rdh] snap ring=%d mirror=%s u=%d [pos %d..%d]: "
        "orig=%zu comp=%zu ratio=%.2fx\n",
        ring, mirror ? "V" : "K", u, start_pos, end_pos - 1,
        page_bytes, comp_size, ratio);
    return 0;
}

/* ── Evict one RDH page: zero live cache ────────────────────────*/
static inline int kv_rdh_evict(KVPageRDH *s, int ring, int wedge, int mirror, int u) {
    if (!s->enabled) return -1;
    if (ring < 0 || ring >= s->n_attn_layers) return -1;
    if (u < 0 || u >= s->n_pages_per_layer) return -1;

    int key = kv_rdh_key(ring, wedge, mirror, u);

    /* Snapshot first if not already */
    uint8_t *slot = kv_rdh_slot_ptr(s, key);
    KVRdhSlotMeta *meta = (KVRdhSlotMeta *)slot;
    if (meta->magic != KV_RDH_MAGIC) {
        if (kv_rdh_snapshot(s, ring, wedge, mirror, u) != 0) return -1;
    }

    /* Skip if already evicted */
    if (kv_rdh_evicted_get(s, key)) return 0;

    /* Zero out positions in live KV cache */
    int start_pos = u * KV_RDH_PAGE_SIZE;
    int end_pos   = start_pos + KV_RDH_PAGE_SIZE;
    if (end_pos > s->n_ctx) end_pos = s->n_ctx;
    int page_tokens = end_pos - start_pos;

    uint8_t *base = (mirror == 0) ? s->k_data[ring] : s->v_data[ring];
    size_t   nb1  = (mirror == 0) ? s->k_nb1[ring] : s->v_nb1[ring];
    memset(base + start_pos * nb1, 0, (size_t)page_tokens * nb1);

    kv_rdh_evicted_set(s, key, 1);
    s->n_evicted++;

    fprintf(stderr, "[kv-rdh] evict ring=%d mirror=%s u=%d\n",
        ring, mirror ? "V" : "K", u);
    return 0;
}

/* ── Restore one RDH page: decompress → write back to live cache ──*/
static inline int kv_rdh_restore(KVPageRDH *s, int ring, int wedge, int mirror, int u) {
    if (!s->enabled) return -1;
    if (ring < 0 || ring >= s->n_attn_layers) return -1;

    int key = kv_rdh_key(ring, wedge, mirror, u);
    uint8_t *slot = kv_rdh_slot_ptr(s, key);
    KVRdhSlotMeta *meta = (KVRdhSlotMeta *)slot;

    if (meta->magic != KV_RDH_MAGIC) return -1;
    if (!kv_rdh_evicted_get(s, key)) return 0;

    /* Decompress: data format = [4B magic][8B orig_size][4B n_chunks][chunks/raw] */
    const uint8_t *comp_data = slot + sizeof(KVRdhSlotMeta);
    size_t comp_buf_size = meta->comp_size;

    uint32_t cmagic = *(const uint32_t *)(comp_data + 0);
    if (cmagic != KV_RDH_MAGIC) return -1;

    size_t orig_size = (size_t)(*(const uint64_t *)(comp_data + 4));
    uint32_t n_chunks = *(const uint32_t *)(comp_data + 12);
    void *dec = NULL;
    size_t dec_size = 0;

    if (n_chunks == 0) {
        /* Uncompressed path */
        if (16 + orig_size > comp_buf_size) return -1;
        dec = malloc(orig_size);
        if (!dec) return -1;
        memcpy(dec, comp_data + 16, orig_size);
        dec_size = orig_size;
    } else {
        /* Binary Shell decode path */
        dec = malloc(orig_size > 0 ? orig_size : 1);
        if (!dec) return -1;
        size_t dp = 0, cp = 16;
        while (dp < orig_size && cp < comp_buf_size) {
            uint8_t chunk_out[64];
            uint32_t consumed = bin_decode_chunk(comp_data + cp, chunk_out);
            if (!consumed) { free(dec); return -1; }
            size_t copy = (orig_size - dp) > 64 ? 64 : (orig_size - dp);
            memcpy((uint8_t*)dec + dp, chunk_out, copy);
            dp += copy;
            cp += (size_t)consumed;
        }
        dec_size = dp;
    }

    if (dec_size < meta->orig_size) {
        free(dec);
        return -1;
    }

    /* Write back to live KV cache */
    int start_pos = u * KV_RDH_PAGE_SIZE;
    int end_pos   = start_pos + KV_RDH_PAGE_SIZE;
    if (end_pos > s->n_ctx) end_pos = s->n_ctx;
    int page_tokens = end_pos - start_pos;

    uint8_t *base = (mirror == 0) ? s->k_data[ring] : s->v_data[ring];
    size_t   nb1  = (mirror == 0) ? s->k_nb1[ring] : s->v_nb1[ring];
    memcpy(base + start_pos * nb1, dec, (size_t)page_tokens * nb1);

    free(dec);
    kv_rdh_evicted_set(s, key, 0);
    s->n_evicted--;
    meta->lru_tick = ++s->lru_tick;

    return 0;
}

/* ── Helper: snapshot/evict/restore entire layer ────────────────*/
static inline int kv_rdh_snapshot_layer(KVPageRDH *s, int ring) {
    int errs = 0;
    for (int m = 0; m < KV_RDH_N_MIRROR; m++)
        for (int u = 0; u < s->n_pages_per_layer; u++)
            if (kv_rdh_snapshot(s, ring, 0, m, u) != 0) errs++;
    return errs;
}

/* ── Snapshot all ───────────────────────────────────────────────*/
static inline int kv_rdh_snapshot_all(KVPageRDH *s) {
    int errs = 0;
    for (int r = 0; r < s->n_attn_layers; r++)
        for (int m = 0; m < KV_RDH_N_MIRROR; m++)
            for (int u = 0; u < s->n_pages_per_layer; u++)
                if (kv_rdh_snapshot(s, r, 0, m, u) != 0) errs++;
    fprintf(stderr, "[kv-rdh] snapshot all: %d slots, errs=%d\n",
        s->n_attn_layers * KV_RDH_N_MIRROR * s->n_pages_per_layer, errs);
    return errs == 0 ? 0 : -1;
}

/* ── LRU: find oldest evictable slot ────────────────────────────*/
static inline int kv_rdh_find_lru(KVPageRDH *s) {
    int best_key = -1;
    uint32_t best_tick = UINT32_MAX;
    for (int key = 0; key < s->n_slots; key++) {
        if (kv_rdh_evicted_get(s, key)) continue;
        uint8_t *slot = kv_rdh_slot_ptr(s, key);
        KVRdhSlotMeta *meta = (KVRdhSlotMeta *)slot;
        if (meta->magic != KV_RDH_MAGIC) continue;
        if (meta->lru_tick < best_tick) {
            best_tick = meta->lru_tick;
            best_key = key;
        }
    }
    return best_key;
}

/* ── LRU: evict oldest N pages (by key = specific layer/mirror/u) ──*/
static inline int kv_rdh_evict_oldest(KVPageRDH *s, int n) {
    int evicted = 0;
    for (int i = 0; i < n; i++) {
        int key = kv_rdh_find_lru(s);
        if (key < 0) break;
        int ring   = kv_rdh_ring(key);
        int mirror = kv_rdh_mirror(key);
        int u      = kv_rdh_u(key);
        if (kv_rdh_evict(s, ring, 0, mirror, u) == 0) evicted++;
    }
    return evicted;
}

/* ── Evict all pages of a specific layer (ring) ──────────────── */
static inline int kv_rdh_evict_ring(KVPageRDH *s, int ring) {
    int evicted = 0;
    for (int m = 0; m < KV_RDH_N_MIRROR; m++)
        for (int u = 0; u < s->n_pages_per_layer; u++)
            if (kv_rdh_evict(s, ring, 0, m, u) == 0) evicted++;
    return evicted;
}

/* ── Access page (restore if evicted, update LRU) ───────────────*/
static inline int kv_rdh_access(KVPageRDH *s, int ring, int wedge, int mirror, int u) {
    if (!s->enabled) return -1;
    int key = kv_rdh_key(ring, wedge, mirror, u);
    if (kv_rdh_evicted_get(s, key)) {
        return kv_rdh_restore(s, ring, wedge, mirror, u);
    }
    uint8_t *slot = kv_rdh_slot_ptr(s, key);
    KVRdhSlotMeta *meta = (KVRdhSlotMeta *)slot;
    if (meta->magic == KV_RDH_MAGIC)
        meta->lru_tick = ++s->lru_tick;
    return 0;
}

/* ── Status ─────────────────────────────────────────────────────*/
static inline void kv_rdh_print_status(const KVPageRDH *s) {
    double ratio = s->total_snap_bytes > 0 ?
        (double)s->total_orig_bytes / (double)s->total_snap_bytes : 0;
    fprintf(stderr, "[kv-rdh] status: %d layers, %d pages/layer, "
        "%d evicted, orig=%zu snap=%zu ratio=%.2fx, slot=%zu mmap=%zu\n",
        s->n_attn_layers, s->n_pages_per_layer, s->n_evicted,
        s->total_orig_bytes, s->total_snap_bytes, ratio,
        s->slot_size, s->mmap_capacity);
}

/* ── Destroy ────────────────────────────────────────────────────*/
static inline void kv_rdh_destroy(KVPageRDH *s) {
    free(s->evicted_map);
    s->evicted_map = NULL;
    if (s->mmap_base) {
#ifdef _WIN32
        VirtualFree(s->mmap_base, 0, MEM_RELEASE);
#else
        if (s->is_mmap)
            munmap(s->mmap_base, s->mmap_capacity);
        else
            free(s->mmap_base);
#endif
        s->mmap_base = NULL;
    }
    fprintf(stderr, "[kv-rdh] destroyed\n");
}

#endif /* KV_PAGE_RDH_H */
