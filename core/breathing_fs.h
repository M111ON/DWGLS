/*
 * breathing_fs.h — Geometric File System via Breathing Seeker
 * ════════════════════════════════════════════════════════════════════
 * CORE: Compression = Space Movement
 *   Write:  seeker อยู่ position X → วาง data ที่ X
 *   Read:   seeker ต้องอยู่ X เท่านั้น → อ่าน lossless
 *   Delta:  ทุกการขยับมีผลกับ compression
 * SPACE: 20736 slots = 144 cubes × 144 slots
 * SACRED: 20736, 1728, 144, 12, 18
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef BREATHING_FS_H
#define BREATHING_FS_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "dwgls_dynamic_codec.h"

#define BFS_MAGIC          0x42524548u
#define BFS_VERSION        1u
#define BFS_TOTAL_SLOTS    20736u
#define BFS_BLOCKS         144u
#define BFS_SLOTS_BLOCK    144u
#define BFS_SEEKER_K       5184u
#define BFS_MAX_FILES      64u
#define BFS_MAX_NAME       32u

/* ═══════════════ BREATHING SEEKER ═══════════════ */
typedef struct {
    double   scale;
    uint32_t space_size;
    uint32_t window;
    uint32_t current_pos;
    uint32_t home_pos;
    uint8_t  is_hyperbolic;
} BreathingSeeker;

static inline void seeker_init(BreathingSeeker *s) {
    if (!s) return;
    s->scale = 1.0; s->space_size = BFS_TOTAL_SLOTS; s->window = BFS_SEEKER_K;
    s->current_pos = 0; s->home_pos = 0; s->is_hyperbolic = 0;
}

static inline void seeker_scale(BreathingSeeker *s, double ns) {
    if (!s || ns <= 0.0) return;
    s->scale = ns;
    s->space_size = (uint32_t)(BFS_TOTAL_SLOTS * ns);
    if (s->space_size < 1) s->space_size = 1;
    s->window = (uint32_t)((double)BFS_SEEKER_K / ns);
    s->is_hyperbolic = (s->window > s->space_size) ? 1 : 0;
    if (s->current_pos >= s->space_size)
        s->current_pos %= s->space_size;
}

static inline void seeker_advance(BreathingSeeker *s) {
    if (!s) return;
    s->current_pos += BFS_SLOTS_BLOCK;
    if (s->current_pos >= s->space_size) s->current_pos %= s->space_size;
}

static inline int32_t seeker_delta(const BreathingSeeker *s) {
    return s ? (int32_t)s->current_pos - (int32_t)s->home_pos : 0;
}

static inline int seeker_is_home(const BreathingSeeker *s) {
    return (s && s->current_pos == s->home_pos) ? 1 : 0;
}

static inline void seeker_print(const BreathingSeeker *s) {
    if (!s) return;
    printf("  Seeker: scale=%.4f space=%u window=%u pos=%u home=%u delta=%d %s\n",
           s->scale, s->space_size, s->window, s->current_pos, s->home_pos,
           seeker_delta(s), s->is_hyperbolic ? "[HYPERBOLIC]" : "");
}

/* ═══════════════ FILE ENTRY ═══════════════ */
typedef struct {
    char     name[BFS_MAX_NAME];
    uint32_t n_blocks;
    uint32_t home_block;
    uint32_t total_bytes;
    uint8_t  strategies[4];
    uint8_t  valid;
} BFSFileEntry;

/* ═══════════════ BLOCK METADATA ═══════════════ */
typedef struct {
    uint32_t home_pos;
    uint32_t current_pos;
    int32_t  delta;
    uint8_t  strategy;
    uint8_t  scale_at_write;
    uint16_t payload_size;
} BFSBlockMeta;

/* ═══════════════ FILE SYSTEM ═══════════════ */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_files;
    uint32_t n_blocks_used;
    uint32_t total_bytes;
    BreathingSeeker seeker;
    BFSFileEntry files[BFS_MAX_FILES];
    uint32_t block_owner[BFS_BLOCKS];
    BFSBlockMeta block_meta[BFS_BLOCKS];
    int8_t block_data[BFS_BLOCKS][BFS_SLOTS_BLOCK];
    uint8_t block_encoded[BFS_BLOCKS][512];
    uint16_t block_encoded_size[BFS_BLOCKS];
    uint32_t delta_log[256];
    uint32_t delta_count;
} BreathingFS;

/* ═══════════════ INIT ═══════════════ */
static inline void bfs_init(BreathingFS *fs) {
    if (!fs) return;
    memset(fs, 0, sizeof(*fs));
    fs->magic = BFS_MAGIC;
    fs->version = BFS_VERSION;
    seeker_init(&fs->seeker);
    for (uint32_t i = 0; i < BFS_BLOCKS; i++)
        fs->block_owner[i] = 0xFFFFFFFF;
}

/* ═══════════════ WRITE ═══════════════ */
static inline int bfs_write(BreathingFS *fs, const char *name,
                             const int8_t *data, uint32_t size)
{
    if (!fs || !name || !data || size == 0) return -1;
    if (fs->n_files >= BFS_MAX_FILES) return -2;
    uint32_t n_blocks = (size + BFS_SLOTS_BLOCK - 1) / BFS_SLOTS_BLOCK;
    if (n_blocks > BFS_BLOCKS - fs->n_blocks_used) return -3;

    uint32_t blocks[BFS_BLOCKS];
    uint32_t found = 0;
    for (uint32_t i = 0; i < BFS_BLOCKS && found < n_blocks; i++)
        if (fs->block_owner[i] == 0xFFFFFFFF) blocks[found++] = i;

    BFSFileEntry *fe = &fs->files[fs->n_files];
    memset(fe, 0, sizeof(*fe));
    strncpy(fe->name, name, BFS_MAX_NAME - 1);
    fe->n_blocks = n_blocks;
    fe->home_block = blocks[0];
    fe->total_bytes = size;
    fe->valid = 1;

    for (uint32_t b = 0; b < n_blocks; b++) {
        uint32_t bi = blocks[b];
        uint32_t offset = b * BFS_SLOTS_BLOCK;
        uint32_t bsz = BFS_SLOTS_BLOCK;
        if (offset + bsz > size) bsz = size - offset;

        memset(fs->block_data[bi], 0, BFS_SLOTS_BLOCK);
        memcpy(fs->block_data[bi], data + offset, bsz);

        BFSBlockMeta *bm = &fs->block_meta[bi];
        bm->home_pos = fs->seeker.current_pos;
        bm->current_pos = fs->seeker.current_pos;
        bm->delta = 0;
        bm->scale_at_write = (uint8_t)(fs->seeker.scale * 100);

        /* Use static DynContainer — no stack/heap overflow */
        DynContainer _bfs_dc;
        dyn_init(&_bfs_dc);
        int rc = dyn_encode(&_bfs_dc, fs->block_data[bi], BFS_SLOTS_BLOCK);
        if (rc == 0) {
            bm->strategy = _bfs_dc.header.strategy;
            bm->payload_size = _bfs_dc.header.payload_size;
            memcpy(fs->block_encoded[bi], _bfs_dc.payload, _bfs_dc.header.payload_size);
            fs->block_encoded_size[bi] = _bfs_dc.header.payload_size;
            if (_bfs_dc.header.strategy < 4)
                fe->strategies[_bfs_dc.header.strategy]++;
        }

        fs->block_owner[bi] = fs->n_files;
        seeker_advance(&fs->seeker);
    }

    fs->n_files++;
    fs->n_blocks_used += n_blocks;
    fs->total_bytes += size;
    return 0;
}

/* ═══════════════ READ ═══════════════ */
static inline int bfs_read(const BreathingFS *fs, const char *name,
                            int8_t *out, uint32_t out_size, uint32_t *actual_size)
{
    if (!fs || !name || !out) return -1;

    int file_idx = -1;
    for (uint32_t i = 0; i < fs->n_files; i++) {
        if (fs->files[i].valid && strcmp(fs->files[i].name, name) == 0) {
            file_idx = (int)i;
            break;
        }
    }
    if (file_idx < 0) return -2;

    const BFSFileEntry *fe = &fs->files[file_idx];
    if (actual_size) *actual_size = fe->total_bytes;
    if (out_size < fe->total_bytes) return -3;

    for (uint32_t b = 0; b < fe->n_blocks; b++) {
        uint32_t bi = fe->home_block + b;
        if (bi >= BFS_BLOCKS) return -4;

        /* Use static DynContainer — no stack/heap overflow */
        DynContainer _bfs_dc;
        dyn_init(&_bfs_dc);
        _bfs_dc.header.strategy = fs->block_meta[bi].strategy;
        _bfs_dc.header.payload_size = fs->block_encoded_size[bi];
        memcpy(_bfs_dc.payload, fs->block_encoded[bi], _bfs_dc.header.payload_size);
        _bfs_dc.header.checksum = dyn_crc32(_bfs_dc.payload, _bfs_dc.header.payload_size);

        uint32_t offset = b * BFS_SLOTS_BLOCK;
        uint32_t bsz = BFS_SLOTS_BLOCK;
        if (offset + bsz > fe->total_bytes) bsz = fe->total_bytes - offset;

        int rc = dyn_decode(&_bfs_dc, out + offset, BFS_SLOTS_BLOCK);
        if (rc != 0) return -5;
    }
    return 0;
}

/* ═══════════════ DELTA OPS ═══════════════ */
static inline void bfs_move_seeker(BreathingFS *fs, double new_scale) {
    if (!fs) return;
    seeker_scale(&fs->seeker, new_scale);
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs->block_owner[b] != 0xFFFFFFFF) {
            BFSBlockMeta *bm = &fs->block_meta[b];
            uint32_t shifted = (uint32_t)((double)bm->home_pos * fs->seeker.scale);
            bm->current_pos = shifted % fs->seeker.space_size;
            bm->delta = (int32_t)bm->current_pos - (int32_t)bm->home_pos;
        }
    }
    if (fs->delta_count < 256)
        fs->delta_log[fs->delta_count++] = fs->seeker.current_pos;
}

static inline void bfs_go_home(BreathingFS *fs) {
    if (!fs) return;
    seeker_scale(&fs->seeker, 1.0);
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs->block_owner[b] != 0xFFFFFFFF) {
            fs->block_meta[b].current_pos = fs->block_meta[b].home_pos;
            fs->block_meta[b].delta = 0;
        }
    }
}

static inline void bfs_delta_stats(const BreathingFS *fs) {
    if (!fs) return;
    uint32_t non_zero = 0;
    int32_t max_d = 0;
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs->block_owner[b] != 0xFFFFFFFF) {
            int32_t d = fs->block_meta[b].delta;
            if (d != 0) non_zero++;
            if (d < 0) d = -d;
            if (d > max_d) max_d = d;
        }
    }
    printf("  Deltas: %u non-zero / %u blocks | max |delta| = %d\n",
           non_zero, fs->n_blocks_used, max_d);
}

/* ═══════════════ VERIFY ═══════════════ */
static inline int bfs_verify_file(const BreathingFS *fs, const char *name,
                                   const int8_t *original, uint32_t size)
{
    if (!fs || !name || !original) return -1;
    int8_t *recon = (int8_t *)malloc(size);
    if (!recon) return -1;
    uint32_t actual = 0;
    int rc = bfs_read(fs, name, recon, size, &actual);
    if (rc != 0) { free(recon); return rc; }
    if (actual != size) { free(recon); return -6; }
    int match = (memcmp(original, recon, size) == 0);
    free(recon);
    return match ? 0 : -7;
}

/* ═══════════════ PRINT ═══════════════ */
static inline void bfs_print_dir(const BreathingFS *fs) {
    if (!fs) return;
    printf("  Files: %u | Blocks: %u / %u | Bytes: %u\n",
           fs->n_files, fs->n_blocks_used, BFS_BLOCKS, fs->total_bytes);
    for (uint32_t i = 0; i < fs->n_files; i++) {
        const BFSFileEntry *fe = &fs->files[i];
        if (!fe->valid) continue;
        printf("    %-20s %3u blocks  %6u bytes  S=%u C=%u D=%u R=%u\n",
               fe->name, fe->n_blocks, fe->total_bytes,
               fe->strategies[0], fe->strategies[1], fe->strategies[2], fe->strategies[3]);
    }
}

#endif /* BREATHING_FS_H */
