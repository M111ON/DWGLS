/*
 * b_fs_wire.h — Final Wiring: single system tying all b_fs pieces together
 * ════════════════════════════════════════════════════════════════════
 * One struct (BfsSystem) owns: storage (BreathingFS) + KV cache + ANN index.
 * Component transforms (fan24/gear/language/18tes) and dimension transfers
 * operate on addresses; KV and ANN store/retrieve bytes through storage.
 * This is the single entry point the rest of DWGLS should use.
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_WIRE_H
#define B_FS_WIRE_H

#include <stdint.h>
#include <string.h>
#include "b_fs_base.h"
#include "b_fs_dim.h"
#include "b_fs_kv.h"
#include "b_fs_ann.h"
#include "b_fs_components.h"
#include "bfs_persist.h"

typedef struct {
    BreathingFS   fs;      /* single backing store              */
    BfsKvCache    kv;      /* KV cache docked on fs             */
    BfsAnnIndex   ann;     /* ANN index docked on fs            */
    int           ready;   /* 1 after init                      */
} BfsSystem;

static inline int bfs_system_init(BfsSystem *sys) {
    if (!sys) return -1;
    memset(sys, 0, sizeof(*sys));
    bfs_init(&sys->fs);
    bfs_kv_init(&sys->kv, &sys->fs);
    bfs_ann_init(&sys->ann, &sys->fs);
    sys->ready = 1;
    return 0;
}

/* Store a KV entry (cold) */
static inline int bfs_system_kv_put(BfsSystem *sys, const char *name,
                                   const uint8_t *data, uint32_t size) {
    if (!sys || !sys->ready) return -1;
    return bfs_kv_put(&sys->kv, name, data, size);
}

/* Retrieve a KV entry */
static inline int bfs_system_kv_get(BfsSystem *sys, const char *name,
                                   uint8_t *out, uint32_t out_size, uint32_t *actual) {
    if (!sys || !sys->ready) return -1;
    return bfs_kv_get(&sys->kv, name, out, out_size, actual);
}

/* Register an ANN entry at a flat address (component-aware) */
static inline int bfs_system_ann_put(BfsSystem *sys, const char *name,
                                    const uint8_t *data, uint32_t size,
                                    uint32_t flat_addr) {
    if (!sys || !sys->ready) return -1;
    return bfs_ann_put(&sys->ann, name, data, size, flat_addr);
}

/* Look up an address (value-blind) */
static inline uint32_t bfs_system_ann_addr(BfsSystem *sys, const char *name) {
    if (!sys || !sys->ready) return 0xFFFFFFFFu;
    return bfs_ann_addr_of(&sys->ann, name);
}

/* Cross-dimension transfer (address only) */
static inline uint32_t bfs_system_transfer(BfsDim src, BfsDim dst, uint32_t addr) {
    return bfs_pair_transfer(src, dst, addr);
}

/* Component: 18tes decode/encode (re-exported) */
static inline void bfs_system_18tes_decode(uint32_t flat, uint32_t *t, uint32_t *c, uint32_t *s) {
    bfc_18tes_decode(flat, t, c, s);
}
static inline uint32_t bfs_system_18tes_encode(uint32_t t, uint32_t c, uint32_t s) {
    return bfc_18tes_encode(t, c, s);
}

/* Component: fan24 CRT */
static inline uint8_t bfs_system_fan24(uint8_t dc, uint8_t dx) {
    return bfc_fan24_crt(dc, dx);
}

/* Component: language identity */
static inline uint32_t bfs_system_language(uint32_t pos) {
    return bfc_language(pos);
}

/* Component: gear tooth */
static inline uint32_t bfs_system_gear_tooth(uint32_t w) {
    return bfc_gear_tooth(w);
}

/* Persistence */
static inline int bfs_system_save(BfsSystem *sys, const char *path) {
    if (!sys || !sys->ready) return -1;
    /* persist ANN index metadata as a special file */
    /* pack: count(4) + entries (name[32]+addr(4)+size(4)) */
    uint8_t meta[4 + BFS_ANN_MAX * 40];
    memset(meta, 0, sizeof(meta));
    uint32_t off = 0;
    memcpy(meta + off, &sys->ann.n, 4); off += 4;
    for (uint32_t i = 0; i < sys->ann.n; i++) {
        memcpy(meta + off, sys->ann.entries[i].name, 32); off += 32;
        memcpy(meta + off, &sys->ann.entries[i].flat_addr, 4); off += 4;
        memcpy(meta + off, &sys->ann.entries[i].size, 4); off += 4;
    }
    bfs_write(&sys->fs, "__ann_index__", (const int8_t*)meta, off);
    return bfs_save_img(path, &sys->fs);
}
static inline int bfs_system_load(BfsSystem *sys, const char *path) {
    if (!sys) return -1;
    memset(sys, 0, sizeof(*sys));
    bfs_init(&sys->fs);
    if (bfs_load_img(path, &sys->fs) != 0) return -1;
    bfs_kv_init(&sys->kv, &sys->fs);
    bfs_ann_init(&sys->ann, &sys->fs);
    /* restore ANN index metadata */
    uint8_t meta[4 + BFS_ANN_MAX * 40];
    uint32_t actual = 0;
    if (bfs_read(&sys->fs, "__ann_index__", (int8_t*)meta, sizeof(meta), &actual) == 0 && actual >= 4) {
        uint32_t n = 0; memcpy(&n, meta, 4);
        if (n > BFS_ANN_MAX) n = BFS_ANN_MAX;
        uint32_t off = 4;
        for (uint32_t i = 0; i < n; i++) {
            if (off + 40 > actual) break;
            BfsAnnEntry *e = &sys->ann.entries[sys->ann.n++];
            memset(e, 0, sizeof(*e));
            memcpy(e->name, meta + off, 32); off += 32;
            memcpy(&e->flat_addr, meta + off, 4); off += 4;
            memcpy(&e->size, meta + off, 4); off += 4;
            e->valid = 1;
        }
        sys->ann.n = n;
    }
    sys->ready = 1;
    return 0;
}

#endif /* B_FS_WIRE_H */
