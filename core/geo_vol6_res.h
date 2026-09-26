/* geo_vol6_res.h — sparse residency over the 6x144 volume.
 *
 * Cell = 4 fixed axes (x,y,z,i) + free (j,k): 20736 slots.
 * A BreathingFS lives INSIDE each cell: block j IS axis-j line,
 * slot k IS axis-k. Pool of 8 resident cells (LRU evict, ~350KB each).
 * v1: evict drops (caller spills); sync_bfs pushes data[] through the
 * cell BFS on demand ("cell" file fills all 144 blocks from home 0).
 */
#ifndef GEO_VOL6_RES_H
#define GEO_VOL6_RES_H

#include <stdint.h>
#include <string.h>
#include "geo_vol6.h"
#include "breathing_fs.h"

#define V6RES_POOL   8u
#define V6RES_SLOTS  20736u

/* Spill hook: called with key+bytes+audit BEFORE a dirty cell is dropped.
 * Return 0 ok, nonzero = veto (touch fails loud, cell kept). */
typedef int (*v6res_spill_fn)(const uint8_t key[4],
                              const uint8_t data[V6RES_SLOTS],
                              uint32_t tombs);
/* Fill hook: cold cell attach asks the backing store for prior bytes.
 * Return 0 = restored into data, 1 = no data (keep zeros), <0 = error. */
typedef int (*v6res_fill_fn)(const uint8_t key[4], uint8_t data[V6RES_SLOTS]);

typedef struct {
    uint8_t key[4];               /* x,y,z,i */
    uint8_t data[V6RES_SLOTS];    /* [j*144+k] */
    BreathingFS bfs;
    uint64_t tick;                /* LRU clock */
    uint64_t data_gen;            /* bumped per write */
    uint64_t sync_gen;            /* data_gen at last sync */
    uint32_t last_tombs;          /* audit carried across evict */
    uint8_t dirty;
    uint8_t fresh;                /* set on new attach, cleared on write */
    uint8_t used;
} V6Cell;

typedef struct {
    V6Cell cells[V6RES_POOL];
    uint64_t clock;
    v6res_spill_fn spill;         /* NULL = no spill path */
    v6res_fill_fn fill;           /* NULL = cold cells read as zeros */
} V6Res;

static inline void v6res_init(V6Res *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

static inline int v6res_key_eq(const uint8_t k[4], uint8_t x, uint8_t y,
                               uint8_t z, uint8_t i) {
    return k[0] == x && k[1] == y && k[2] == z && k[3] == i;
}

/* attach-or-hit: evicting LRU on miss when full. Dirty evict spills first
 * (retire-then-free via bfs_delete); no spill path or spill veto → NULL,
 * cell kept, nothing lost. */
static inline V6Cell *v6res_touch(V6Res *r, uint8_t x, uint8_t y,
                                  uint8_t z, uint8_t i) {
    if (!r || x >= V6_SPAN || y >= V6_SPAN || z >= V6_SPAN || i >= V6_SPAN)
        return NULL;
    for (unsigned c = 0; c < V6RES_POOL; c++)
        if (r->cells[c].used && v6res_key_eq(r->cells[c].key, x, y, z, i)) {
            r->cells[c].tick = ++r->clock;
            return &r->cells[c];
        }
    V6Cell *slot = NULL;
    for (unsigned c = 0; c < V6RES_POOL; c++)
        if (!r->cells[c].used) { slot = &r->cells[c]; break; }
    if (!slot) {
        slot = &r->cells[0];
        for (unsigned c = 1; c < V6RES_POOL; c++)
            if (r->cells[c].tick < slot->tick) slot = &r->cells[c];
    }
    if (slot->used && slot->dirty) {
        if (!r->spill) return NULL;   /* fail loud: nowhere to spill */
        slot->last_tombs = slot->bfs.tomb_count;
        if (r->spill(slot->key, slot->data, slot->last_tombs) != 0) return NULL;
        bfs_delete(&slot->bfs, "cell");   /* retire planets, archive tombs */
        slot->last_tombs = slot->bfs.tomb_count;
    }
    {
        uint32_t keep_tombs = slot->used ? slot->bfs.tomb_count : slot->last_tombs;
        uint32_t keep_last = slot->last_tombs;
        if (keep_tombs > keep_last) keep_last = keep_tombs;
        memset(slot, 0, sizeof(*slot));
        slot->last_tombs = keep_last;
    }
    slot->key[0] = x; slot->key[1] = y; slot->key[2] = z; slot->key[3] = i;
    bfs_init(&slot->bfs);
    slot->tick = ++r->clock;
    slot->used = 1;
    slot->fresh = 1;
    return slot;
}

static inline int v6res_write(V6Cell *c, uint8_t j, uint8_t k, uint8_t v) {
    if (!c || !c->used || j >= V6_SPAN || k >= V6_SPAN) return -1;
    c->data[(unsigned)j * V6_SPAN + k] = v;
    c->dirty = 1;
    c->fresh = 0;
    c->data_gen++;
    return 0;
}

static inline int v6res_read(const V6Cell *c, uint8_t j, uint8_t k, uint8_t *out) {
    if (!c || !c->used || j >= V6_SPAN || k >= V6_SPAN || !out) return -1;
    *out = c->data[(unsigned)j * V6_SPAN + k];
    return 0;
}

/* push the whole cell through its inner BFS; block j then holds row j.
 * Redundant syncs skip (no bfs_init → tombs survive). */
static inline int v6res_sync_bfs(V6Cell *c) {
    if (!c || !c->used) return -1;
    if (c->sync_gen == c->data_gen && c->bfs.n_files > 0) return 0;
    bfs_init(&c->bfs);   /* fresh: "cell" takes blocks [0,144) */
    int rc = bfs_write(&c->bfs, "cell", (const int8_t *)c->data, V6RES_SLOTS);
    if (rc == 0) c->sync_gen = c->data_gen;
    return rc;
}

/* ── close discipline: dropping a V6Res without this loses every dirty
 * resident (never evicted, hence never spilled). Returns spilled count,
 * -1 if any spill vetoed (pool kept intact for retry). */
static inline int v6res_close(V6Res *r) {
    if (!r) return -1;
    int n = 0;
    for (unsigned c = 0; c < V6RES_POOL; c++) {
        V6Cell *cell = &r->cells[c];
        if (!cell->used || !cell->dirty) continue;
        if (!r->spill) return -1;
        cell->last_tombs = cell->bfs.tomb_count;
        if (r->spill(cell->key, cell->data, cell->last_tombs) != 0) return -1;
        bfs_delete(&cell->bfs, "cell");
        cell->last_tombs = cell->bfs.tomb_count;
        cell->dirty = 0;
        n++;
    }
    return n;
}

/* ── stripe file API: byte stream over cells via the rank bridge ──
 * write: base rank o, n bytes, spanning cells with auto-touch; cold cells
 *   fill first (spill data wins over zeros), then write.
 * read: same walk; cold cells fill (0 restored / 1 zeros / <0 fail).
 * Both return 0 ok, -1 fail-loud (OOB, no spill path, spill veto, fill error). */

static inline int v6file_cold(V6Res *r, V6Cell *c) {
    if (!c->fresh) return 0;
    c->fresh = 0;
    if (!r->fill) return 0;                 /* cold reads as zeros */
    int rc = r->fill(c->key, c->data);
    if (rc < 0 || rc > 1) return -1;
    return 0;
}

static inline int v6file_write(V6Res *r, uint64_t base, const uint8_t *buf, uint64_t n) {
    if (!r || (!buf && n)) return -1;
    for (uint64_t t = 0; t < n; t++) {
        uint8_t key[4], j, k;
        if (v6_stripe(base + t, key, &j, &k) != 0) return -1;
        V6Cell *c = v6res_touch(r, key[0], key[1], key[2], key[3]);
        if (!c) return -1;
        if (v6file_cold(r, c) != 0) return -1;
        if (v6res_write(c, j, k, buf[t]) != 0) return -1;
    }
    return 0;
}

static inline int v6file_read(V6Res *r, uint64_t base, uint8_t *buf, uint64_t n) {
    if (!r || (!buf && n)) return -1;
    for (uint64_t t = 0; t < n; t++) {
        uint8_t key[4], j, k;
        if (v6_stripe(base + t, key, &j, &k) != 0) return -1;
        V6Cell *c = v6res_touch(r, key[0], key[1], key[2], key[3]);
        if (!c) return -1;
        if (v6file_cold(r, c) != 0) return -1;
        if (v6res_read(c, j, k, &buf[t]) != 0) return -1;
    }
    return 0;
}

#endif /* GEO_VOL6_RES_H */
