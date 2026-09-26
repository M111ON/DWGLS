/* bfs_wangate.h — wang+tantrix block-edge gate for BFS.
 * Chain rule: tile[i].entry = tile[i-1].exit (laid to connect, like tantrix
 * tiles on a table); tile.exit = xor-fold of the block's ENCODED bytes
 * (the bytes reads consume — same substrate the planet watches).
 * verify: exit recompute catches TAMPER (-1); tantrix_connects (the wang
 * edge match) catches REORDER/BREAK (-2). Sidecar tape travels with the fs.
 */
#ifndef BFS_WANGATE_H
#define BFS_WANGATE_H

#include <stdint.h>
#include <string.h>
#include "breathing_fs.h"
#include "lc_tantrix.h"

#define BWT_N 144u

typedef struct {
    TantrixTile tiles[BWT_N];
    uint8_t has[BWT_N];
} BWTape;

static inline void bwt_init(BWTape *t) {
    if (t) memset(t, 0, sizeof(*t));
}

static inline uint8_t bwt_fold(const BreathingFS *fs, uint32_t bi) {
    uint8_t x = 0;
    uint16_t n = fs->block_encoded_size[bi];
    for (uint16_t i = 0; i < n; i++) x ^= fs->block_encoded[bi][i];
    return (uint8_t)(x & 0x3u);
}

static const BFSFileEntry *bwt_find(const BreathingFS *fs, const char *name) {
    for (uint32_t i = 0; i < fs->n_files; i++)
        if (fs->files[i].valid && strncmp(fs->files[i].name, name, BFS_MAX_NAME) == 0)
            return &fs->files[i];
    return NULL;
}

/* lay the chain after bfs_write. Returns blocks sealed, -1 no file, -2 no tape. */
static inline int bwt_seal(const BreathingFS *fs, BWTape *t, const char *name) {
    const BFSFileEntry *e;
    if (!fs || !t || !name) return -2;
    e = bwt_find(fs, name);
    if (!e) return -1;
    uint8_t prev_exit = 0;   /* anchor */
    for (uint32_t b = 0; b < e->n_blocks; b++) {
        uint32_t bi = e->home_block + b;
        if (bi >= BWT_N) return -1;
        uint8_t exit = bwt_fold(fs, bi);
        t->tiles[bi] = tantrix_make(prev_exit, exit, (uint8_t)(b & 0x3u),
                                    TANTRIX_CLASS_NORMAL);
        t->has[bi] = 1;
        prev_exit = exit;
    }
    return (int)e->n_blocks;
}

/* 0 clean, -1 TAMPER (content moved), -2 BREAK (chain cut), -3 no file/tape. */
static inline int bwt_verify(const BreathingFS *fs, const BWTape *t, const char *name) {
    const BFSFileEntry *e;
    if (!fs || !t || !name) return -3;
    e = bwt_find(fs, name);
    if (!e) return -3;
    uint8_t prev_exit = 0;
    TantrixTile prev = tantrix_make(0, 0, 0, TANTRIX_CLASS_NORMAL);
    for (uint32_t b = 0; b < e->n_blocks; b++) {
        uint32_t bi = e->home_block + b;
        if (bi >= BWT_N || !t->has[bi]) return -2;
        if (bwt_fold(fs, bi) != tantrix_exit(t->tiles[bi])) return -1;
        if (tantrix_entry(t->tiles[bi]) != prev_exit) return -2;
        if (b > 0 && !tantrix_connects(prev, t->tiles[bi])) return -2;
        prev = t->tiles[bi];
        prev_exit = tantrix_exit(t->tiles[bi]);
    }
    return 0;
}

/* gated read: DROP (refuse) on any gate failure — never silent garbage. */
static inline int bwt_read(const BreathingFS *fs, const BWTape *t, const char *name,
                           int8_t *out, uint32_t out_size, uint32_t *actual) {
    int v = bwt_verify(fs, t, name);
    if (v != 0) return v - 10;   /* -11 tamper, -12 break, -13 missing */
    return bfs_read(fs, name, out, out_size, actual);
}

#endif /* BFS_WANGATE_H */
