/*
 * b_fs_ann.h — Phase 4: ANN Index on b_fs
 * ════════════════════════════════════════════════════════════════════
 * ANN index entries are stored in BFS (name -> payload bytes). Search
 * returns addresses (flat slots), never payload meaning — value-blindness
 * preserved (MAP not COMPRESS). The index is just (name, flat_addr).
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_ANN_H
#define B_FS_ANN_H

#include <stdint.h>
#include <string.h>
#include "b_fs_base.h"

#define BFS_ANN_MAX 256u

typedef struct {
    char     name[32];
    uint32_t flat_addr;   /* address in the 20736 field */
    uint32_t size;        /* payload size in BFS        */
    int      valid;
} BfsAnnEntry;

typedef struct {
    BreathingFS *fs;
    BfsAnnEntry  entries[BFS_ANN_MAX];
    uint32_t     n;
} BfsAnnIndex;

static inline void bfs_ann_init(BfsAnnIndex *idx, BreathingFS *fs) {
    if (!idx) return;
    memset(idx, 0, sizeof(*idx));
    idx->fs = fs;
}

/* Register a payload under a name at a given flat address. */
static inline int bfs_ann_put(BfsAnnIndex *idx, const char *name,
                             const uint8_t *data, uint32_t size,
                             uint32_t flat_addr) {
    if (!idx || !idx->fs || !name || !data || size == 0) return -1;
    if (idx->n >= BFS_ANN_MAX) return -2;
    if (bfs_write(idx->fs, name, (const int8_t*)data, size) != 0) return -3;
    BfsAnnEntry *e = &idx->entries[idx->n++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, 31);
    e->flat_addr = flat_addr % BFS_PAIR_N;
    e->size = size;
    e->valid = 1;
    return 0;
}

/* Lookup address by name (returns flat_addr or 0xFFFFFFFF if missing). */
static inline uint32_t bfs_ann_addr_of(const BfsAnnIndex *idx, const char *name) {
    if (!idx || !name) return 0xFFFFFFFFu;
    for (uint32_t i = 0; i < idx->n; i++)
        if (idx->entries[i].valid && strcmp(idx->entries[i].name, name) == 0)
            return idx->entries[i].flat_addr;
    return 0xFFFFFFFFu;
}

/* Fetch payload by name (search result -> address -> bytes). */
static inline int bfs_ann_get(const BfsAnnIndex *idx, const char *name,
                             uint8_t *out, uint32_t out_size, uint32_t *actual) {
    if (!idx || !idx->fs || !name || !out) return -1;
    return bfs_read(idx->fs, name, (int8_t*)out, out_size, actual);
}

/* Count of valid entries. */
static inline uint32_t bfs_ann_count(const BfsAnnIndex *idx) {
    return idx ? idx->n : 0;
}

#endif /* B_FS_ANN_H */
