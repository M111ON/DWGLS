/*
 * b_fs_kv.h — Phase 3: KV Cache on b_fs
 * ════════════════════════════════════════════════════════════════════
 * KV cache entries are stored as BFS files (name -> bytes). BFS is the
 * single backing store; hot RAM cache is an optional twin layer.
 *   put : bfs_write (cold, addressable, lossless)
 *   get : bfs_read
 *   twin: optional hot pointer; sync parks hot -> cold
 * No malloc in hot path beyond BFS's own (caller-owned buffers).
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_KV_H
#define B_FS_KV_H

#include <stdint.h>
#include <string.h>
#include "b_fs_base.h"

typedef struct {
    BreathingFS *fs;     /* backing store (cold)        */
    BfsTwin      twin;   /* optional hot view           */
    uint32_t     n_puts;
    uint32_t     n_gets;
    uint32_t     n_miss;
} BfsKvCache;

static inline void bfs_kv_init(BfsKvCache *c, BreathingFS *fs) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->fs = fs;
    bfs_twin_init(&c->twin, NULL, NULL, 0);
}

static inline int bfs_kv_put(BfsKvCache *c, const char *name,
                             const uint8_t *data, uint32_t size) {
    if (!c || !c->fs || !name || !data || size == 0) return -1;
    int rc = bfs_write(c->fs, name, (const int8_t*)data, size);
    if (rc == 0) {
        c->n_puts++;
        /* park hot twin if present */
        if (c->twin.hot && c->twin.cold && c->twin.size >= size)
            memcpy(c->twin.cold, data, size);
    }
    return rc;
}

static inline int bfs_kv_get(BfsKvCache *c, const char *name,
                             uint8_t *out, uint32_t out_size, uint32_t *actual) {
    if (!c || !c->fs || !name || !out) return -1;
    c->n_gets++;
    int rc = bfs_read(c->fs, name, (int8_t*)out, out_size, actual);
    if (rc != 0) c->n_miss++;
    return rc;
}

static inline int bfs_kv_has(BfsKvCache *c, const char *name) {
    if (!c || !c->fs) return 0;
    /* scan file table */
    for (uint32_t i = 0; i < c->fs->n_files; i++)
        if (c->fs->files[i].valid && strcmp(c->fs->files[i].name, name) == 0)
            return 1;
    return 0;
}

#endif /* B_FS_KV_H */
