/*
 * tring.h — Tring: Timeline Ring for variable-size data
 *
 * Core idea:
 *   tick     = monotonic ID (time axis)
 *   TringNode = {tick, size, data[]} — variable size, heap allocated
 *   tring[]  = sparse pointer array indexed by tick
 *
 * Visibility is external — tring doesn't know about shell flags.
 * GC must be triggered by caller after dfield_delete().
 *
 * Memory layout per node:
 *   [tick:8B][size:4B][_pad:4B][data:size bytes]
 *   → header = 16B, data follows immediately
 *   → total alloc = 16 + size bytes per node
 *
 * Fixed 64B mode (legacy/codec path):
 *   tring_push64() / tring_read64() — zero-copy fast path
 *   data pointer is always 64B aligned within the node
 *
 * Design invariants:
 *   - tick never reused (monotonic, no wrap assumed < UINT32_MAX)
 *   - tring[tick] == NULL means free (released or never written)
 *   - size == 0 is valid (marker/tombstone node, no data bytes)
 *   - caller owns visibility; tring owns lifetime
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Node ──────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t tick;    /* which tick this node belongs to              */
    uint32_t size;    /* data length in bytes (0 = marker only)       */
    uint32_t _pad;    /* align data[] to 8B boundary                  */
    uint8_t  data[];  /* flexible array — actual bytes follow header  */
} TringNode;

/* node alloc size */
static inline size_t tring_node_alloc_size(uint32_t data_size) {
    return sizeof(TringNode) + data_size;
}

/* ── Tring ─────────────────────────────────────────────────────────── */
typedef struct {
    TringNode **nodes;      /* sparse array [0..capacity-1]           */
    uint32_t    capacity;   /* allocated slots (power-of-2 preferred) */
    uint32_t    next_tick;  /* next tick to assign                     */
    uint32_t    live_count; /* nodes currently non-NULL                */
    uint32_t    _pad;
} Tring;

/* ── init / free ───────────────────────────────────────────────────── */
static inline int tring_init(Tring *t, uint32_t cap) {
    t->nodes      = (TringNode **)calloc(cap, sizeof(TringNode *));
    if (!t->nodes) return -1;
    t->capacity   = cap;
    t->next_tick  = 0;
    t->live_count = 0;
    t->_pad       = 0;
    return 0;
}

static inline void tring_destroy(Tring *t) {
    if (!t->nodes) return;
    for (uint32_t i = 0; i < t->capacity; i++) {
        if (t->nodes[i]) { free(t->nodes[i]); t->nodes[i] = NULL; }
    }
    free(t->nodes);
    t->nodes      = NULL;
    t->capacity   = 0;
    t->next_tick  = 0;
    t->live_count = 0;
}

/* ── push — variable size ─────────────────────────────────────────── */
/*
 * Copies data into a new TringNode and stores at nodes[tick].
 * data may be NULL if size == 0 (marker node).
 * Returns tick assigned, UINT32_MAX on failure.
 */
static inline uint32_t tring_push(Tring *t, const void *data, uint32_t size) {
    if (t->next_tick >= t->capacity) return UINT32_MAX;
    TringNode *node = (TringNode *)malloc(tring_node_alloc_size(size));
    if (!node) return UINT32_MAX;
    node->tick = t->next_tick;
    node->size = size;
    node->_pad = 0;
    if (size && data) memcpy(node->data, data, size);
    t->nodes[t->next_tick] = node;
    t->live_count++;
    return t->next_tick++;
}

/* ── push64 — fixed 64B fast path (legacy/codec) ─────────────────── */
static inline uint32_t tring_push64(Tring *t, const uint8_t chunk[64]) {
    return tring_push(t, chunk, 64);
}

/* ── read ──────────────────────────────────────────────────────────── */
/*
 * Returns pointer to node's data (not a copy), NULL if tick is free.
 * Pointer valid until tring_release(tick) or tring_destroy().
 * out_size: if non-NULL, receives size of data (0 for marker nodes).
 */
static inline const uint8_t *tring_read(const Tring *t,
                                          uint32_t tick,
                                          uint32_t *out_size)
{
    if (tick >= t->capacity || !t->nodes[tick]) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    TringNode *n = t->nodes[tick];
    if (out_size) *out_size = n->size;
    return (n->size > 0) ? n->data : NULL;
}

/* read64: asserts size == 64, returns pointer or NULL */
static inline const uint8_t *tring_read64(const Tring *t, uint32_t tick) {
    uint32_t sz;
    const uint8_t *p = tring_read(t, tick, &sz);
    return (p && sz == 64) ? p : NULL;
}

/* ── node metadata (without reading data) ─────────────────────────── */
static inline int      tring_alive(const Tring *t, uint32_t tick) {
    return (tick < t->capacity && t->nodes[tick] != NULL);
}
static inline uint32_t tring_size_of(const Tring *t, uint32_t tick) {
    if (!tring_alive(t, tick)) return 0;
    return t->nodes[tick]->size;
}

/* ── release (GC primitive) ───────────────────────────────────────── */
/*
 * Frees the node at tick. Safe to call on already-free tick.
 * Does NOT clear any shell flags — that's the caller's job.
 */
static inline void tring_release(Tring *t, uint32_t tick) {
    if (tick >= t->capacity || !t->nodes[tick]) return;
    free(t->nodes[tick]);
    t->nodes[tick] = NULL;
    if (t->live_count) t->live_count--;
}

/* ── GC: sweep by reference bitmap ───────────────────────────────── */
/*
 * Caller provides a bitmap of ticks that are still referenced by
 * at least one visible shell slot. Any tick NOT in the bitmap is freed.
 *
 * bitmap: array of uint64_t, bit [tick] = 1 means "keep".
 * bitmap_words: ceil(t->capacity / 64).
 *
 * Returns number of ticks freed.
 *
 * Why bitmap instead of scan-all-slots:
 *   GC cost = O(capacity/64) words instead of O(INDEX_SIZE × live_ticks).
 *   Caller builds bitmap during delete or via shell sweep — one pass.
 */
static inline uint32_t tring_gc_bitmap(Tring *t,
                                        const uint64_t *bitmap,
                                        uint32_t bitmap_words)
{
    uint32_t freed = 0;
    for (uint32_t i = 0; i < t->capacity; i++) {
        if (!t->nodes[i]) continue;                    /* already free */
        uint32_t word = i >> 6;
        uint64_t bit  = 1ULL << (i & 63u);
        int referenced = (word < bitmap_words) && (bitmap[word] & bit);
        if (!referenced) {
            tring_release(t, i);
            freed++;
        }
    }
    return freed;
}

/* ── GC: simple scan (no bitmap, O(n²) — for small datasets / tests) */
/*
 * ref_fn: callback — returns 1 if tick is still referenced, 0 if not.
 * ctx: opaque pointer passed to ref_fn.
 */
typedef int (*tring_ref_fn)(uint32_t tick, void *ctx);

static inline uint32_t tring_gc_scan(Tring *t, tring_ref_fn ref_fn, void *ctx)
{
    uint32_t freed = 0;
    for (uint32_t i = 0; i < t->next_tick; i++) {
        if (!t->nodes[i]) continue;
        if (!ref_fn(i, ctx)) {
            tring_release(t, i);
            freed++;
        }
    }
    return freed;
}

/* ── stats ─────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t live_count;     /* non-NULL nodes                       */
    uint32_t next_tick;      /* next tick (= total pushes ever)      */
    uint64_t total_bytes;    /* sum of all live node data sizes       */
    uint32_t max_size;       /* largest live node data size           */
    uint32_t min_size;       /* smallest live node data size (excl 0)*/
} TringStats;

static inline TringStats tring_stats(const Tring *t) {
    TringStats s = {0, t->next_tick, 0, 0, UINT32_MAX};
    for (uint32_t i = 0; i < t->next_tick; i++) {
        if (!t->nodes[i]) continue;
        s.live_count++;
        s.total_bytes += t->nodes[i]->size;
        if (t->nodes[i]->size > s.max_size) s.max_size = t->nodes[i]->size;
        if (t->nodes[i]->size > 0 && t->nodes[i]->size < s.min_size)
            s.min_size = t->nodes[i]->size;
    }
    if (s.min_size == UINT32_MAX) s.min_size = 0;
    return s;
}
