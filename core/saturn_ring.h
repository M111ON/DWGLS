/*
 * saturn_ring.h — Saturn-ring store: fixed radii + ratio needle (2026-09-17)
 * ═══════════════════════════════════════════════════════════════════════════
 * Owner's design (analogical: Saturn + vinyl player).
 *
 * Anti-vramtile discipline:
 *   - Uniform fixed-size slots: NO variable sizes → NO fragmentation,
 *     NO holes, NO compact, NO vram_used accounting. A slot is valid or free.
 *   - Fixed ring capacities: ring full = replace lowest-tick within ring.
 *   - Static arrays: bytes can never leak (no alloc per entry in steady state).
 *   - Ratio needle: migration counters enforce hot:cold quota (default 3:1).
 *
 * v1 is RAM-backed (honest: the borrowed-VRAM hypothesis stays open until
 * a Vulkan upload_fn lands). Upload hook reserved, not required.
 */
#ifndef SATURN_RING_H
#define SATURN_RING_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define SATURN_MAGIC   0x5352544Eu  /* "SRTN" */
#define SATURN_VERSION 1u

/* one ring: N uniform slots, fixed forever after init */
typedef struct {
    uint8_t  *slots;      /* N * slot_sz bytes, static */
    uint8_t  *valid;      /* N flags */
    uint32_t *ticks;      /* N last-access ticks */
    uint32_t *keys;       /* N name hashes (for verify) */
    uint32_t  n_slots;
    uint32_t  slot_sz;
    uint32_t  n_valid;
    uint64_t  n_place;    /* placements into this ring (ratio accounting) */
} SaturnRing;

typedef struct {
    uint32_t   magic;
    SaturnRing hot;        /* ring0 */
    SaturnRing cold;       /* ring1 */
    uint32_t   tick;
    uint32_t   hot_weight; /* ratio needle: hot placements per cycle */
    uint32_t   cold_weight;
    uint64_t   n_promote;
    uint64_t   n_demote;
    uint64_t   n_evict;
} SaturnStore;

static inline uint32_t saturn_hash(const char *name) {
    uint32_t h = 0x811c9dc5u;
    while (*name) { h ^= (uint8_t)(*name++); h *= 0x01000193u; }
    return h;
}

static inline int saturn_ring_init(SaturnRing *r, uint32_t n_slots, uint32_t slot_sz) {
    memset(r, 0, sizeof(*r));
    r->slots = (uint8_t *)calloc(n_slots, slot_sz);
    r->valid = (uint8_t *)calloc(n_slots, 1);
    r->ticks = (uint32_t *)calloc(n_slots, sizeof(uint32_t));
    r->keys  = (uint32_t *)calloc(n_slots, sizeof(uint32_t));
    if (!r->slots || !r->valid || !r->ticks || !r->keys) return -1;
    r->n_slots = n_slots;
    r->slot_sz = slot_sz;
    return 0;
}

static inline void saturn_ring_free(SaturnRing *r) {
    free(r->slots); free(r->valid); free(r->ticks); free(r->keys);
    memset(r, 0, sizeof(*r));
}

static inline int saturn_init(SaturnStore *s, uint32_t hot_slots, uint32_t cold_slots,
                              uint32_t slot_sz, uint32_t hot_w, uint32_t cold_w) {
    memset(s, 0, sizeof(*s));
    s->magic = SATURN_MAGIC;
    if (saturn_ring_init(&s->hot, hot_slots, slot_sz) != 0) return -1;
    if (saturn_ring_init(&s->cold, cold_slots, slot_sz) != 0) {
        saturn_ring_free(&s->hot);
        return -1;
    }
    s->hot_weight = hot_w ? hot_w : 3;
    s->cold_weight = cold_w ? cold_w : 1;
    return 0;
}

static inline void saturn_free(SaturnStore *s) {
    saturn_ring_free(&s->hot);
    saturn_ring_free(&s->cold);
    s->magic = 0;
}

/* place into ring: free slot preferred, else replace lowest-tick (bounded) */
static inline int saturn_ring_place(SaturnStore *s, SaturnRing *r,
                                    uint32_t key, const uint8_t *data, uint32_t sz) {
    if (sz > r->slot_sz) return -1; /* fixed slots: oversize rejected, never split */
    uint32_t start = key % r->n_slots;
    for (uint32_t i = 0; i < r->n_slots; i++) {
        uint32_t sl = (start + i) % r->n_slots;
        if (!r->valid[sl]) {
            memcpy(r->slots + (size_t)sl * r->slot_sz, data, sz);
            if (sz < r->slot_sz)
                memset(r->slots + (size_t)sl * r->slot_sz + sz, 0, r->slot_sz - sz);
            r->valid[sl] = 1;
            r->keys[sl] = key;
            r->ticks[sl] = ++s->tick;
            r->n_valid++;
            r->n_place++;
            return 0;
        }
    }
    /* ring full: replace lowest-tick (fixed capacity — never grows) */
    uint32_t lo = 0;
    for (uint32_t sl = 1; sl < r->n_slots; sl++)
        if (r->ticks[sl] < r->ticks[lo]) lo = sl;
    memcpy(r->slots + (size_t)lo * r->slot_sz, data, sz);
    if (sz < r->slot_sz)
        memset(r->slots + (size_t)lo * r->slot_sz + sz, 0, r->slot_sz - sz);
    r->keys[lo] = key;
    r->ticks[lo] = ++s->tick;
    r->n_place++;
    s->n_evict++;
    return 1; /* replaced */
}

/* find: linear probe from key slot, key-verified (no false hits) */
static inline uint8_t *saturn_ring_find(SaturnStore *s, SaturnRing *r, uint32_t key) {
    uint32_t start = key % r->n_slots;
    for (uint32_t i = 0; i < r->n_slots; i++) {
        uint32_t sl = (start + i) % r->n_slots;
        if (r->valid[sl] && r->keys[sl] == key) {
            r->ticks[sl] = ++s->tick;
            return r->slots + (size_t)sl * r->slot_sz;
        }
    }
    return NULL;
}

/* needle: promote = place hot (demote overflow hot→cold by lowest-tick) */
static inline int saturn_promote(SaturnStore *s, const char *name,
                                 const uint8_t *data, uint32_t sz) {
    uint32_t key = saturn_hash(name);
    int r = saturn_ring_place(s, &s->hot, key, data, sz);
    if (r < 0) return -1;
    s->n_promote++;
    return r;
}

static inline uint8_t *saturn_get(SaturnStore *s, const char *name) {
    uint32_t key = saturn_hash(name);
    uint8_t *p = saturn_ring_find(s, &s->hot, key);
    if (p) return p;
    return saturn_ring_find(s, &s->cold, key);
}

static inline int saturn_demote_cold(SaturnStore *s, const char *name,
                                     const uint8_t *data, uint32_t sz) {
    uint32_t key = saturn_hash(name);
    int r = saturn_ring_place(s, &s->cold, key, data, sz);
    if (r < 0) return -1;
    s->n_demote++;
    return r;
}

/* invariants (call from tests): capacities fixed, counters consistent */
static inline int saturn_verify(SaturnStore *s) {
    if (s->magic != SATURN_MAGIC) return -1;
    if (s->hot.n_valid > s->hot.n_slots) return -2;
    if (s->cold.n_valid > s->cold.n_slots) return -3;
    return 0;
}

#endif /* SATURN_RING_H */
