/*
 * bcr_mm.h — Multi-Map BCR (Inverted Index) with zero-malloc posting lists
 */
#ifndef BCR_MM_H
#define BCR_MM_H

#include <stdint.h>
#include <string.h>
#include "b_fs_base.h"

#define BCR_MM_N 20736u
#define BCR_MM_SENTINEL 0xFFFFu
#define BCR_MM_MAX_POOL 65536u
#define BCR_MM_MAX_NODES BCR_MM_N

typedef struct {
    uint16_t key;
    uint16_t head_idx;
    uint16_t tail_idx;
    uint16_t count;
    int16_t  left, right;
    int8_t   height;
} BcrMmNode;

typedef struct {
    BcrMmNode nodes[BCR_MM_MAX_NODES];
    uint16_t  posting_pool[BCR_MM_MAX_POOL];   /* doc_addr per slot */
    uint16_t  pool_next[BCR_MM_MAX_POOL];     /* next slot in list (SENTINEL=none) */
    uint8_t   pool_used[BCR_MM_MAX_POOL];     /* 1 if slot occupied */
    uint32_t  pool_count;                     /* number of used slots */
    uint16_t  root;
    uint16_t  node_count;
    uint16_t  free_node;
} BcrMmTree;

/* Reuse collision primitive from residual_space.h (pure arithmetic, no malloc) */
static inline uint32_t _rs_hash(uint64_t bond_key, uint32_t mask) {
    uint64_t h = bond_key;
    h ^= h >> 33;
    h *= UINT64_C(0xFF51AFD7ED558CCD);
    h ^= h >> 33;
    return (uint32_t)(h & (uint64_t)mask);
}

static inline void bcr_mm_init(BcrMmTree *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->root = BCR_MM_SENTINEL;
    t->pool_count = 0;
    t->node_count = 0;
    t->free_node = 0;
    for (uint32_t i = 0; i < BCR_MM_MAX_NODES; i++) {
        t->nodes[i].left = BCR_MM_SENTINEL;
        t->nodes[i].right = BCR_MM_SENTINEL;
        t->nodes[i].height = 0;
        t->nodes[i].count = 0;
        t->nodes[i].head_idx = BCR_MM_SENTINEL;
        t->nodes[i].tail_idx = BCR_MM_SENTINEL;
    }
}

static inline int8_t bcr_mm_h(const BcrMmTree *t, uint16_t idx) {
    if (idx == BCR_MM_SENTINEL) return 0;
    return t->nodes[idx].height;
}

static inline int8_t bcr_mm_bf(const BcrMmTree *t, uint16_t idx) {
    if (idx == BCR_MM_SENTINEL) return 0;
    return (int8_t)(bcr_mm_h(t, t->nodes[idx].left) - bcr_mm_h(t, t->nodes[idx].right));
}

static inline void bcr_mm_update_height(BcrMmTree *t, uint16_t idx) {
    if (idx == BCR_MM_SENTINEL) return;
    int8_t lh = bcr_mm_h(t, t->nodes[idx].left);
    int8_t rh = bcr_mm_h(t, t->nodes[idx].right);
    t->nodes[idx].height = (lh > rh ? lh : rh) + 1;
}

/* rotations */
static uint16_t bcr_mm_rot_right(BcrMmTree *t, uint16_t y);
static uint16_t bcr_mm_rot_left(BcrMmTree *t, uint16_t x);

static uint16_t bcr_mm_rot_right(BcrMmTree *t, uint16_t y) {
    uint16_t x = t->nodes[y].left;
    uint16_t t2 = t->nodes[x].right;
    t->nodes[x].right = y;
    t->nodes[y].left = t2;
    bcr_mm_update_height(t, y);
    bcr_mm_update_height(t, x);
    return x;
}

static uint16_t bcr_mm_rot_left(BcrMmTree *t, uint16_t x) {
    uint16_t y = t->nodes[x].right;
    uint16_t t2 = t->nodes[y].left;
    t->nodes[y].left = x;
    t->nodes[x].right = t2;
    bcr_mm_update_height(t, x);
    bcr_mm_update_height(t, y);
    return y;
}

static uint16_t bcr_mm_balance(BcrMmTree *t, uint16_t idx) {
    if (idx == BCR_MM_SENTINEL) return BCR_MM_SENTINEL;
    bcr_mm_update_height(t, idx);
    int8_t bf = bcr_mm_bf(t, idx);
    if (bf > 1 && bcr_mm_bf(t, t->nodes[idx].left) >= 0)
        return bcr_mm_rot_right(t, idx);
    if (bf > 1 && bcr_mm_bf(t, t->nodes[idx].left) < 0) {
        t->nodes[idx].left = bcr_mm_rot_left(t, t->nodes[idx].left);
        return bcr_mm_rot_right(t, idx);
    }
    if (bf < -1 && bcr_mm_bf(t, t->nodes[idx].right) <= 0)
        return bcr_mm_rot_left(t, idx);
    if (bf < -1 && bcr_mm_bf(t, t->nodes[idx].right) > 0) {
        t->nodes[idx].right = bcr_mm_rot_right(t, t->nodes[idx].right);
        return bcr_mm_rot_left(t, idx);
    }
    return idx;
}

/* find empty slot in posting pool via hash + linear probing (reuse _rs_hash) */
static inline uint32_t bcr_mm_find_slot(BcrMmTree *t, uint16_t k, uint16_t da) {
    uint32_t mask = BCR_MM_MAX_POOL - 1;
    uint32_t slot = _rs_hash(((uint64_t)k << 16) | da, mask);
    while (t->pool_used[slot]) {
        slot = (slot + 1) & mask;
    }
    return slot;
}

/* insert key+doc_addr */
static uint16_t bcr_mm_insert_node(BcrMmTree *t, uint16_t idx, uint16_t key, uint16_t doc_addr) {
    if (idx == BCR_MM_SENTINEL) {
        if (t->free_node >= BCR_MM_MAX_NODES) return BCR_MM_SENTINEL;
        if (t->pool_count >= BCR_MM_MAX_POOL) return BCR_MM_SENTINEL;
        uint32_t slot = bcr_mm_find_slot(t, key, doc_addr);
        uint16_t ni = t->free_node++;
        t->nodes[ni].key = key;
        t->nodes[ni].left = BCR_MM_SENTINEL;
        t->nodes[ni].right = BCR_MM_SENTINEL;
        t->nodes[ni].height = 1;
        t->nodes[ni].head_idx = (uint16_t)slot;
        t->nodes[ni].tail_idx = (uint16_t)slot;
        t->nodes[ni].count = 1;
        t->posting_pool[slot] = doc_addr;
        t->pool_next[slot] = BCR_MM_SENTINEL;
        t->pool_used[slot] = 1;
        t->pool_count++;
        t->node_count++;
        return ni;
    }
    if (key == t->nodes[idx].key) {
        /* append to posting list via hash + probe */
        if (t->pool_count >= BCR_MM_MAX_POOL) return idx;
        uint32_t slot = bcr_mm_find_slot(t, key, doc_addr);
        t->posting_pool[slot] = doc_addr;
        t->pool_next[slot] = BCR_MM_SENTINEL;
        t->pool_used[slot] = 1;
        t->pool_count++;
        /* link tail -> slot */
        t->pool_next[t->nodes[idx].tail_idx] = (uint16_t)slot;
        t->nodes[idx].tail_idx = (uint16_t)slot;
        t->nodes[idx].count++;
        return idx;
    }
    if (key < t->nodes[idx].key) {
        uint16_t l = bcr_mm_insert_node(t, t->nodes[idx].left, key, doc_addr);
        t->nodes[idx].left = l;
    } else {
        uint16_t r = bcr_mm_insert_node(t, t->nodes[idx].right, key, doc_addr);
        t->nodes[idx].right = r;
    }
    return bcr_mm_balance(t, idx);
}

static inline void bcr_mm_insert(BcrMmTree *t, uint16_t key, uint16_t doc_addr) {
    if (!t) return;
    if (t->root == BCR_MM_SENTINEL) {
        t->root = bcr_mm_insert_node(t, BCR_MM_SENTINEL, key, doc_addr);
    } else {
        t->root = bcr_mm_insert_node(t, t->root, key, doc_addr);
    }
}

/* search key -> copy posting list to out_buffer, return count */
static inline uint32_t bcr_mm_search(const BcrMmTree *t, uint16_t key, uint16_t *out, uint32_t max_out) {
    if (!t || !out || max_out == 0) return 0;
    uint16_t cur = t->root;
    while (cur != BCR_MM_SENTINEL) {
        if (key == t->nodes[cur].key) {
            uint32_t cnt = 0;
            uint16_t i = t->nodes[cur].head_idx;
            while (i != BCR_MM_SENTINEL) {
                if (cnt < max_out) out[cnt++] = t->posting_pool[i];
                i = t->pool_next[i];
            }
            return cnt;
        }
        if (key < t->nodes[cur].key) cur = t->nodes[cur].left;
        else cur = t->nodes[cur].right;
    }
    return 0;
}

/* intersect two sorted? arrays of addresses (BCR list and HNSW list) */
static inline uint32_t bcr_mm_intersect(const uint16_t *a, uint32_t na,
                                       const uint16_t *b, uint32_t nb,
                                       uint16_t *out, uint32_t max_out) {
    /* assumes both sorted ascending; if not, sort first. We'll sort copies. */
    /* simple O(na*nb) for small, but implement two-pointer after sort */
    /* For correctness, do nested for small lists */
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < na && cnt < max_out; i++) {
        for (uint32_t j = 0; j < nb; j++) {
            if (a[i] == b[j]) {
                /* check duplicate in out */
                int dup = 0;
                for (uint32_t k = 0; k < cnt; k++) if (out[k] == a[i]) { dup = 1; break; }
                if (!dup) out[cnt++] = a[i];
                break;
            }
        }
    }
    return cnt;
}

static inline int bcr_mm_height(const BcrMmTree *t) {
    if (!t || t->root == BCR_MM_SENTINEL) return 0;
    return t->nodes[t->root].height;
}

#endif /* BCR_MM_H */