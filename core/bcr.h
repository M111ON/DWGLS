/*
 * bcr.h — Balanced Crystal Routing (AVL on b_fs address space)
 */
#ifndef BCR_H
#define BCR_H

#include <stdint.h>
#include <stdlib.h>
#include "b_fs_base.h"

#define BCR_N 20736u
#define BCR_SENTINEL 0xFFFFu

typedef struct {
    uint16_t left, right, parent;
    int8_t   height;
    uint16_t data_addr;
} BcrNode;

typedef struct {
    BcrNode nodes[BCR_N];
    uint16_t root;
    uint16_t count;
    uint16_t free_idx;
} BcrTree;

static inline void bcr_init(BcrTree *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->root = BCR_SENTINEL;
    t->free_idx = 0;
    for (uint32_t i = 0; i < BCR_N; i++) {
        t->nodes[i].left = BCR_SENTINEL;
        t->nodes[i].right = BCR_SENTINEL;
        t->nodes[i].parent = BCR_SENTINEL;
        t->nodes[i].height = 0;
        t->nodes[i].data_addr = (uint16_t)i;
    }
}

/* height helper */
static inline int8_t bcr_h(const BcrTree *t, uint16_t idx) {
    if (idx == BCR_SENTINEL) return 0;
    return t->nodes[idx].height;
}

static inline int8_t bcr_bf(const BcrTree *t, uint16_t idx) {
    if (idx == BCR_SENTINEL) return 0;
    return (int8_t)(bcr_h(t, t->nodes[idx].left) - bcr_h(t, t->nodes[idx].right));
}

static inline void bcr_update_height(BcrTree *t, uint16_t idx) {
    if (idx == BCR_SENTINEL) return;
    int8_t lh = bcr_h(t, t->nodes[idx].left);
    int8_t rh = bcr_h(t, t->nodes[idx].right);
    t->nodes[idx].height = (lh > rh ? lh : rh) + 1;
}

/* rotations return new subtree root */
static uint16_t bcr_rotate_right(BcrTree *t, uint16_t y);
static uint16_t bcr_rotate_left(BcrTree *t, uint16_t x);

static uint16_t bcr_rotate_right(BcrTree *t, uint16_t y) {
    uint16_t x = t->nodes[y].left;
    uint16_t t2 = t->nodes[x].right;
    /* perform */
    t->nodes[x].right = y;
    t->nodes[y].left = t2;
    /* parents */
    t->nodes[x].parent = t->nodes[y].parent;
    t->nodes[y].parent = x;
    if (t2 != BCR_SENTINEL) t->nodes[t2].parent = y;
    bcr_update_height(t, y);
    bcr_update_height(t, x);
    return x;
}

static uint16_t bcr_rotate_left(BcrTree *t, uint16_t x) {
    uint16_t y = t->nodes[x].right;
    uint16_t t2 = t->nodes[y].left;
    t->nodes[y].left = x;
    t->nodes[x].right = t2;
    t->nodes[y].parent = t->nodes[x].parent;
    t->nodes[x].parent = y;
    if (t2 != BCR_SENTINEL) t->nodes[t2].parent = x;
    bcr_update_height(t, x);
    bcr_update_height(t, y);
    return y;
}

static uint16_t bcr_balance(BcrTree *t, uint16_t idx) {
    if (idx == BCR_SENTINEL) return BCR_SENTINEL;
    bcr_update_height(t, idx);
    int8_t bf = bcr_bf(t, idx);
    /* LL */
    if (bf > 1 && bcr_bf(t, t->nodes[idx].left) >= 0)
        return bcr_rotate_right(t, idx);
    /* LR */
    if (bf > 1 && bcr_bf(t, t->nodes[idx].left) < 0) {
        t->nodes[idx].left = bcr_rotate_left(t, t->nodes[idx].left);
        return bcr_rotate_right(t, idx);
    }
    /* RR */
    if (bf < -1 && bcr_bf(t, t->nodes[idx].right) <= 0)
        return bcr_rotate_left(t, idx);
    /* RL */
    if (bf < -1 && bcr_bf(t, t->nodes[idx].right) > 0) {
        t->nodes[idx].right = bcr_rotate_right(t, t->nodes[idx].right);
        return bcr_rotate_left(t, idx);
    }
    return idx;
}

/* insert a data_addr (key = data_addr) */
static uint16_t bcr_insert_node(BcrTree *t, uint16_t idx, uint16_t key) {
    if (idx == BCR_SENTINEL) {
        /* use free_idx as new node */
        if (t->free_idx >= BCR_N) return BCR_SENTINEL;
        uint16_t ni = t->free_idx++;
        t->nodes[ni].data_addr = key;
        t->nodes[ni].left = BCR_SENTINEL;
        t->nodes[ni].right = BCR_SENTINEL;
        t->nodes[ni].parent = BCR_SENTINEL;
        t->nodes[ni].height = 1;
        t->count++;
        return ni;
    }
    if (key < t->nodes[idx].data_addr) {
        uint16_t l = bcr_insert_node(t, t->nodes[idx].left, key);
        t->nodes[idx].left = l;
        if (l != BCR_SENTINEL) t->nodes[l].parent = idx;
    } else if (key > t->nodes[idx].data_addr) {
        uint16_t r = bcr_insert_node(t, t->nodes[idx].right, key);
        t->nodes[idx].right = r;
        if (r != BCR_SENTINEL) t->nodes[r].parent = idx;
    } else {
        return idx; /* duplicate */
    }
    return bcr_balance(t, idx);
}

static inline void bcr_insert(BcrTree *t, uint16_t key) {
    if (!t) return;
    if (t->root == BCR_SENTINEL) {
        t->root = bcr_insert_node(t, BCR_SENTINEL, key);
    } else {
        t->root = bcr_insert_node(t, t->root, key);
    }
}

/* build from array (sorts copy) */
static inline void bcr_build(BcrTree *t, const uint16_t *addrs, uint32_t n) {
    if (!t) return;
    bcr_init(t);
    /* insert in given order (AVL balances) */
    for (uint32_t i = 0; i < n && i < BCR_N; i++) {
        bcr_insert(t, addrs[i]);
    }
}

/* search */
static inline uint16_t bcr_search(const BcrTree *t, uint16_t key) {
    uint16_t cur = t ? t->root : BCR_SENTINEL;
    while (cur != BCR_SENTINEL) {
        if (key == t->nodes[cur].data_addr) return cur;
        if (key < t->nodes[cur].data_addr) cur = t->nodes[cur].left;
        else cur = t->nodes[cur].right;
    }
    return BCR_SENTINEL;
}

/* range search [start,end] inclusive */
static inline uint32_t bcr_range_search(const BcrTree *t, uint16_t start, uint16_t end,
                                        uint16_t *out, uint32_t max_out) {
    if (!t || !out || max_out == 0) return 0;
    /* iterative inorder */
    uint32_t cnt = 0;
    /* stack */
    uint16_t stack[64];
    int sp = 0;
    uint16_t cur = t->root;
    while (cur != BCR_SENTINEL || sp > 0) {
        while (cur != BCR_SENTINEL) {
            stack[sp++] = cur;
            if (sp >= 64) break;
            cur = t->nodes[cur].left;
        }
        if (sp == 0) break;
        cur = stack[--sp];
        uint16_t v = t->nodes[cur].data_addr;
        if (v >= start && v <= end) {
            if (cnt < max_out) out[cnt++] = v;
            else break;
        }
        if (v > end) break;
        cur = t->nodes[cur].right;
    }
    return cnt;
}

/* height of tree */
static inline int bcr_height(const BcrTree *t) {
    if (!t || t->root == BCR_SENTINEL) return 0;
    return t->nodes[t->root].height;
}

/* cycle check via DFS */
static inline int bcr_has_cycle(const BcrTree *t) {
    if (!t) return 0;
    static uint8_t color[BCR_N];
    memset(color, 0, BCR_N);
    /* iterative */
    for (uint32_t s = 0; s < BCR_N; s++) {
        if (t->nodes[s].data_addr == 0 && s != 0) continue;
    }
    /* simple: follow parent? Instead check via DFS from root */
    /* We'll do recursive-ish with explicit stack */
    uint16_t st[BCR_N];
    int sp = 0;
    /* mark visited */
    memset(color, 0, BCR_N);
    st[sp++] = t->root;
    /* Use a visited set; if we revisit a node -> cycle */
    while (sp > 0) {
        uint16_t u = st[--sp];
        if (u == BCR_SENTINEL) continue;
        if (color[u]) return 1; /* cycle */
        color[u] = 1;
        uint16_t l = t->nodes[u].left;
        uint16_t r = t->nodes[u].right;
        if (l != BCR_SENTINEL) st[sp++] = l;
        if (r != BCR_SENTINEL) st[sp++] = r;
    }
    return 0;
}

#endif /* BCR_H */