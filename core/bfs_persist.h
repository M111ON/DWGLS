/*
 * bfs_persist.h — Breathing FS Storage Layer (Phase 1: Production Hardening)
 * ════════════════════════════════════════════════════════════════════
 * Adds to breathing_fs.h:
 *   1) Versioned image format  — fixed-offset portable layout
 *   2) mmap path               — Windows MapViewOfFile + Linux mmap
 *   3) RDH bijection verify    — encode(decode(x)) == x per block
 *   4) Seeker MVCC             — seeker position = version, scale = time
 *
 * IMAGE LAYOUT (all offsets constant — no TOC walk, no var-length):
 *   [0]      header 64B: magic"BIMG" version block_size block_count
 *                        max_files n_files n_blocks_used total_bytes
 *                        scale(f64) seek_pos home_pos delta_count
 *   [64]     files     64 × 49B     (name[32] n_blocks home_block total_bytes strategies[4] valid)
 *   [3200]   owners    144 × u32    (-1 free)
 *   [3776]   meta      144 × 16B    (home_pos current_pos delta strategy scale_at_write payload_size)
 *   [6080]   e_sizes   144 × u16
 *   [6368]   delta_log 256 × u32
 *   [7392]   enc_offs  144 × u32    (payload offset inside data region)
 *   [7968]   data      144 × 512B   (encoded payloads, fixed stride)
 *   [81696]  crc32     4B           (of bytes [0, 81696))
 *   EOF = 82000 B
 *
 * CORE: zero-malloc hot path. All buffers static or caller-owned.
 * KEEP BFSMmapFS OUT OF THE STACK — it embeds BreathingFS (~95 KB).
 */
#ifndef BFS_PERSIST_H
#define BFS_PERSIST_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "breathing_fs.h"

/* ═══════════════ IMAGE GEOMETRY (fixed offsets) ═══════════════ */
#define BFS_IMG_MAGIC     0x474D4942u   /* "BIMG" */
#define BFS_IMG_VERSION   1u
#define BFS_IMG_HEADER_SZ 64u
#define BFS_IMG_FILE_ENT  49u           /* serialized BFSFileEntry size */
#define BFS_IMG_FILES_OFF 64u
#define BFS_IMG_FILES_BYT (BFS_MAX_FILES * BFS_IMG_FILE_ENT)            /* 3136 */
#define BFS_IMG_OWNER_OFF (BFS_IMG_FILES_OFF + BFS_IMG_FILES_BYT)       /* 3200 */
#define BFS_IMG_OWNER_BYT (BFS_BLOCKS * 4u)                             /* 576 */
#define BFS_IMG_META_OFF  (BFS_IMG_OWNER_OFF + BFS_IMG_OWNER_BYT)       /* 3776 */
#define BFS_IMG_META_ENT  16u
#define BFS_IMG_META_BYT  (BFS_BLOCKS * BFS_IMG_META_ENT)               /* 2304 */
#define BFS_IMG_ESIZE_OFF (BFS_IMG_META_OFF + BFS_IMG_META_BYT)         /* 6080 */
#define BFS_IMG_ESIZE_BYT (BFS_BLOCKS * 2u)                             /* 288 */
#define BFS_IMG_DLOG_OFF  (BFS_IMG_ESIZE_OFF + BFS_IMG_ESIZE_BYT)       /* 6368 */
#define BFS_IMG_DLOG_BYT  (256u * 4u)                                   /* 1024 */
#define BFS_IMG_EOFF_OFF  (BFS_IMG_DLOG_OFF + BFS_IMG_DLOG_BYT)         /* 7392 */
#define BFS_IMG_EOFF_BYT  (BFS_BLOCKS * 4u)                             /* 576 */
#define BFS_IMG_DATA_OFF  (BFS_IMG_EOFF_OFF + BFS_IMG_EOFF_BYT)         /* 7968 */
#define BFS_IMG_ENC_MAX   512u
#define BFS_IMG_DATA_BYT  (BFS_BLOCKS * BFS_IMG_ENC_MAX)                /* 73728 */
#define BFS_IMG_CRC_OFF   (BFS_IMG_DATA_OFF + BFS_IMG_DATA_BYT)         /* 81696 */
#define BFS_IMG_SIZE      (BFS_IMG_CRC_OFF + 4u)                        /* 82000 */

/* ═══════════════ MVCC SNAPSHOT ═══════════════ */
#define BFS_MVCC_MAX 8u
typedef struct {
    double   scale;
    uint32_t seek_pos, home_pos, space_size, window;
    uint8_t  hyper;
    uint32_t block_pos[BFS_BLOCKS];   /* per-block current_pos */
    int32_t  block_delta[BFS_BLOCKS]; /* per-block delta */
} BFSMvccSnap;

typedef struct {
    BFSMvccSnap snaps[BFS_MVCC_MAX];
    uint32_t n;
} BFSMvcc;

/* ═══════════════ MMAP HANDLE (holds BreathingFS + mapping) ═══════════════ */
typedef struct {
    BreathingFS fs;              /* TOC loaded from mapping; decode source below */
    const uint8_t *map_ptr;      /* mapped base (NULL when not mapped) */
    size_t  map_size;
    uint32_t enc_off[BFS_BLOCKS]; /* payload offset of block b inside map */
    /* platform handles (opaque) */
    void *h_file, *h_map, *h_view;
    uint32_t crc_stored;
} BFSMmapFS;

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif
#if !defined(_WIN32)
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* ═══════════════ HEADER ACCESSORS ═══════════════ */
static inline uint32_t bfs_img_u32(const uint8_t *m, uint32_t off) {
    return (uint32_t)m[off] | ((uint32_t)m[off+1] << 8) |
           ((uint32_t)m[off+2] << 16) | ((uint32_t)m[off+3] << 24);
}
static inline void bfs_img_wu32(uint8_t *m, uint32_t off, uint32_t v) {
    m[off] = (uint8_t)v; m[off+1] = (uint8_t)(v >> 8);
    m[off+2] = (uint8_t)(v >> 16); m[off+3] = (uint8_t)(v >> 24);
}
static inline uint16_t bfs_img_u16(const uint8_t *m, uint32_t off) {
    return (uint16_t)((uint16_t)m[off] | ((uint16_t)m[off+1] << 8));
}
static inline void bfs_img_wu16(uint8_t *m, uint32_t off, uint16_t v) {
    m[off] = (uint8_t)v; m[off+1] = (uint8_t)(v >> 8);
}
static inline double bfs_img_f64(const uint8_t *m, uint32_t off) {
    double d; memcpy(&d, m + off, 8); return d;
}
static inline void bfs_img_wf64(uint8_t *m, uint32_t off, double v) {
    memcpy(m + off, &v, 8);
}

/* ═══════════════ SERIALIZE BreathingFS → byte buffer ═══════════════
 * dst must hold BFS_IMG_SIZE bytes. Fills all fixed regions + CRC. */
static inline void bfs_img_serialize(BreathingFS *fs, uint8_t *dst)
{
    memset(dst, 0, BFS_IMG_SIZE);
    bfs_img_wu32(dst, 0,  BFS_IMG_MAGIC);
    bfs_img_wu32(dst, 4,  BFS_IMG_VERSION);
    bfs_img_wu32(dst, 8,  BFS_IMG_HEADER_SZ);
    bfs_img_wu32(dst, 12, BFS_SLOTS_BLOCK);
    bfs_img_wu32(dst, 16, BFS_BLOCKS);
    bfs_img_wu32(dst, 20, BFS_MAX_FILES);
    bfs_img_wu32(dst, 24, fs->n_files);
    bfs_img_wu32(dst, 28, fs->n_blocks_used);
    bfs_img_wu32(dst, 32, fs->total_bytes);
    bfs_img_wf64(dst, 40, fs->seeker.scale);
    bfs_img_wu32(dst, 48, fs->seeker.current_pos);
    bfs_img_wu32(dst, 52, fs->seeker.home_pos);
    bfs_img_wu32(dst, 56, fs->delta_count);

    /* files (49B serialized entries) */
    for (uint32_t i = 0; i < BFS_MAX_FILES; i++) {
        uint32_t o = BFS_IMG_FILES_OFF + i * BFS_IMG_FILE_ENT;
        memcpy(dst + o, fs->files[i].name, BFS_MAX_NAME);
        bfs_img_wu32(dst, o + 32, fs->files[i].n_blocks);
        bfs_img_wu32(dst, o + 36, fs->files[i].home_block);
        bfs_img_wu32(dst, o + 40, fs->files[i].total_bytes);
        memcpy(dst + o + 44, fs->files[i].strategies, 4);
        dst[o + 48] = fs->files[i].valid;
    }
    /* owners */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++)
        bfs_img_wu32(dst, BFS_IMG_OWNER_OFF + i * 4, fs->block_owner[i]);
    /* meta (16B serialized) */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        uint32_t o = BFS_IMG_META_OFF + i * BFS_IMG_META_ENT;
        const BFSBlockMeta *bm = &fs->block_meta[i];
        bfs_img_wu32(dst, o + 0,  bm->home_pos);
        bfs_img_wu32(dst, o + 4,  bm->current_pos);
        bfs_img_wu32(dst, o + 8,  (uint32_t)bm->delta);
        dst[o + 12] = bm->strategy;
        dst[o + 13] = bm->scale_at_write;
        bfs_img_wu16(dst, o + 14, bm->payload_size);
    }
    /* encoded sizes + payloads (fixed stride) */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        bfs_img_wu16(dst, BFS_IMG_ESIZE_OFF + i * 2, fs->block_encoded_size[i]);
        uint32_t p_off = BFS_IMG_DATA_OFF + i * BFS_IMG_ENC_MAX;
        if (fs->block_encoded_size[i] > 0) {
            memcpy(dst + p_off, fs->block_encoded[i], fs->block_encoded_size[i]);
            bfs_img_wu32(dst, BFS_IMG_EOFF_OFF + i * 4, p_off);
        } else {
            bfs_img_wu32(dst, BFS_IMG_EOFF_OFF + i * 4, 0);
        }
    }
    /* delta log */
    for (uint32_t i = 0; i < fs->delta_count && i < 256; i++)
        bfs_img_wu32(dst, BFS_IMG_DLOG_OFF + i * 4, fs->delta_log[i]);
    /* CRC over [0, CRC_OFF) */
    uint32_t crc = dyn_crc32(dst, BFS_IMG_CRC_OFF);
    bfs_img_wu32(dst, BFS_IMG_CRC_OFF, crc);
}

/* ═══════════════ PARSE byte buffer → BreathingFS ═══════════════
 * Returns 0 on success; -1 bad magic; -2 bad version; -3 bad geometry;
 * -4 CRC mismatch. Does NOT copy payloads into fs (zero-copy reads). */
static inline int bfs_img_parse(const uint8_t *m, size_t size, BreathingFS *fs,
                                uint32_t *enc_off_out)
{
    if (!m || !fs || size < BFS_IMG_SIZE) return -1;
    if (bfs_img_u32(m, 0) != BFS_IMG_MAGIC) return -1;
    if (bfs_img_u32(m, 4) != BFS_IMG_VERSION) return -2;
    if (bfs_img_u32(m, 12) != BFS_SLOTS_BLOCK || bfs_img_u32(m, 16) != BFS_BLOCKS) return -3;

    uint32_t crc_expected = bfs_img_u32(m, BFS_IMG_CRC_OFF);
    uint32_t crc_actual = dyn_crc32(m, BFS_IMG_CRC_OFF);
    if (crc_actual != crc_expected) return -4;

    memset(fs, 0, sizeof(*fs));
    fs->magic = BFS_MAGIC;
    fs->version = BFS_VERSION;
    fs->n_files = bfs_img_u32(m, 24);
    fs->n_blocks_used = bfs_img_u32(m, 28);
    fs->total_bytes = bfs_img_u32(m, 32);
    fs->delta_count = bfs_img_u32(m, 56);
    fs->seeker.scale = bfs_img_f64(m, 40);
    fs->seeker.current_pos = bfs_img_u32(m, 48);
    fs->seeker.home_pos = bfs_img_u32(m, 52);
    fs->seeker.space_size = (uint32_t)(BFS_TOTAL_SLOTS * fs->seeker.scale);
    if (fs->seeker.space_size < 1) fs->seeker.space_size = 1;
    fs->seeker.window = (uint32_t)((double)BFS_SEEKER_K / fs->seeker.scale);
    fs->seeker.is_hyperbolic = (fs->seeker.window > fs->seeker.space_size) ? 1 : 0;

    /* files */
    for (uint32_t i = 0; i < BFS_MAX_FILES; i++) {
        uint32_t o = BFS_IMG_FILES_OFF + i * BFS_IMG_FILE_ENT;
        memcpy(fs->files[i].name, m + o, BFS_MAX_NAME);
        fs->files[i].n_blocks = bfs_img_u32(m, o + 32);
        fs->files[i].home_block = bfs_img_u32(m, o + 36);
        fs->files[i].total_bytes = bfs_img_u32(m, o + 40);
        memcpy(fs->files[i].strategies, m + o + 44, 4);
        fs->files[i].valid = m[o + 48];
    }
    /* owners */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        fs->block_owner[i] = bfs_img_u32(m, BFS_IMG_OWNER_OFF + i * 4);
        fs->block_meta[i].home_pos = bfs_img_u32(m, BFS_IMG_META_OFF + i * BFS_IMG_META_ENT + 0);
        fs->block_meta[i].current_pos = bfs_img_u32(m, BFS_IMG_META_OFF + i * BFS_IMG_META_ENT + 4);
        fs->block_meta[i].delta = (int32_t)bfs_img_u32(m, BFS_IMG_META_OFF + i * BFS_IMG_META_ENT + 8);
        fs->block_meta[i].strategy = m[BFS_IMG_META_OFF + i * BFS_IMG_META_ENT + 12];
        fs->block_meta[i].scale_at_write = m[BFS_IMG_META_OFF + i * BFS_IMG_META_ENT + 13];
        fs->block_meta[i].payload_size = bfs_img_u16(m, BFS_IMG_META_OFF + i * BFS_IMG_META_ENT + 14);
        fs->block_encoded_size[i] = bfs_img_u16(m, BFS_IMG_ESIZE_OFF + i * 2);
        if (enc_off_out)
            enc_off_out[i] = bfs_img_u32(m, BFS_IMG_EOFF_OFF + i * 4);
    }
    /* payloads: copy into fs->block_encoded so plain bfs_read works after
     * bfs_load_img (zero-copy mmap path uses enc_off + map pointer instead) */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        uint32_t esz = fs->block_encoded_size[i];
        uint32_t p_off = BFS_IMG_DATA_OFF + i * BFS_IMG_ENC_MAX; /* fixed stride */
        if (esz > 0) {
            if (esz > 512 || p_off + esz > BFS_IMG_CRC_OFF) return -4;
            memcpy(fs->block_encoded[i], m + p_off, esz);
        }
    }
    /* delta log */
    for (uint32_t i = 0; i < fs->delta_count && i < 256; i++)
        fs->delta_log[i] = bfs_img_u32(m, BFS_IMG_DLOG_OFF + i * 4);
    return 0;
}

/* ═══════════════ PLAIN FILE SAVE / LOAD (portable, no mmap) ═══════════════ */
static inline int bfs_save_img(const char *path, BreathingFS *fs)
{
    static uint8_t buf[BFS_IMG_SIZE];
    if (!path || !fs) return -1;
    bfs_img_serialize(fs, buf);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, BFS_IMG_SIZE, f);
    fclose(f);
    return (w == BFS_IMG_SIZE) ? 0 : -2;
}

static inline int bfs_load_img(const char *path, BreathingFS *fs)
{
    static uint8_t buf[BFS_IMG_SIZE];
    if (!path || !fs) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, BFS_IMG_SIZE, f);
    fclose(f);
    if (r != BFS_IMG_SIZE) return -2;
    return bfs_img_parse(buf, r, fs, NULL);
}

/* ═══════════════ MMAP OPEN / CLOSE ═══════════════ */
static inline void bfs_mmap_close(BFSMmapFS *mfs); /* fwd decl — used by open on parse failure */

static inline int bfs_mmap_open(const char *path, BFSMmapFS *mfs)
{
    if (!path || !mfs) return -1;
    memset(mfs, 0, sizeof(*mfs));
#if defined(_WIN32)
    HANDLE hf = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return -2;
    DWORD len_low = GetFileSize(hf, NULL);
    if (len_low < BFS_IMG_SIZE) { CloseHandle(hf); return -3; }
    HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!hm) { CloseHandle(hf); return -4; }
    uint8_t *base = (uint8_t *)MapViewOfFile(hm, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!base) { CloseHandle(hm); CloseHandle(hf); return -5; }
    mfs->h_file = hf; mfs->h_map = hm; mfs->h_view = base;
    mfs->map_ptr = base; mfs->map_size = len_low;
#else
    int fd = open(path, O_RDWR);
    if (fd < 0) return -2;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -3; }
    if ((size_t)st.st_size < BFS_IMG_SIZE) { close(fd); return -3; }
    uint8_t *base = (uint8_t *)mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); return -5; }
    mfs->h_file = (void *)(intptr_t)fd;
    mfs->map_ptr = base; mfs->map_size = (size_t)st.st_size;
    mfs->h_map = NULL;
#endif
    int rc = bfs_img_parse(mfs->map_ptr, mfs->map_size, &mfs->fs, mfs->enc_off);
    if (rc != 0) {
        bfs_mmap_close(mfs);
        return rc;
    }
    mfs->crc_stored = bfs_img_u32(mfs->map_ptr, BFS_IMG_CRC_OFF);
    return 0;
}

static inline void bfs_mmap_flush(BFSMmapFS *mfs)
{
    if (!mfs || !mfs->map_ptr) return;
#if defined(_WIN32)
    FlushViewOfFile((void *)mfs->map_ptr, 0);
#else
    msync((void *)mfs->map_ptr, mfs->map_size, MS_SYNC);
#endif
}

static inline void bfs_mmap_close(BFSMmapFS *mfs)
{
    if (!mfs) return;
#if defined(_WIN32)
    if (mfs->h_view) { UnmapViewOfFile(mfs->h_view); mfs->h_view = NULL; }
    if (mfs->h_map)  { CloseHandle((HANDLE)mfs->h_map); mfs->h_map = NULL; }
    if (mfs->h_file) { CloseHandle((HANDLE)mfs->h_file); mfs->h_file = NULL; }
#else
    if (mfs->map_ptr && mfs->map_ptr != MAP_FAILED)
        munmap((void *)mfs->map_ptr, mfs->map_size);
    if (mfs->h_file) { close((int)(intptr_t)mfs->h_file); mfs->h_file = NULL; }
#endif
    mfs->map_ptr = NULL; mfs->map_size = 0;
}

/* ═══════════════ ZERO-COPY READ — decode straight from mapping ═══════════════
 * Coordinate semantics: seeker must be at file's home position for lossless;
 * callers MUST bfs_go_home() first (or invariant delta=0). */
static inline int bfs_mmap_read(const BFSMmapFS *mfs, const char *name,
                                int8_t *out, uint32_t out_size, uint32_t *actual_size)
{
    if (!mfs || !name || !out || !mfs->map_ptr) return -1;
    const BreathingFS *fs = &mfs->fs;
    int file_idx = -1;
    for (uint32_t i = 0; i < fs->n_files; i++) {
        if (fs->files[i].valid && strcmp(fs->files[i].name, name) == 0) {
            file_idx = (int)i; break;
        }
    }
    if (file_idx < 0) return -2;
    const BFSFileEntry *fe = &fs->files[file_idx];
    if (actual_size) *actual_size = fe->total_bytes;
    if (out_size < fe->total_bytes) return -3;

    for (uint32_t b = 0; b < fe->n_blocks; b++) {
        uint32_t bi = fe->home_block + b;
        if (bi >= BFS_BLOCKS) return -4;
        uint32_t p_off = mfs->enc_off[bi];
        if (p_off == 0 || p_off + fs->block_encoded_size[bi] > mfs->map_size)
            return -4;
        /* decode directly from mapped payload — zero-copy */
        DynContainer dc;
        dyn_init(&dc);
        dc.header.strategy = fs->block_meta[bi].strategy;
        dc.header.payload_size = fs->block_encoded_size[bi];
        memcpy(dc.payload, mfs->map_ptr + p_off, dc.header.payload_size);
        dc.header.checksum = dyn_crc32(dc.payload, dc.header.payload_size);
        uint32_t offset = b * BFS_SLOTS_BLOCK;
        uint32_t bsz = BFS_SLOTS_BLOCK;
        if (offset + bsz > fe->total_bytes) bsz = fe->total_bytes - offset;
        int rc = dyn_decode(&dc, out + offset, BFS_SLOTS_BLOCK);
        if (rc != 0) return -5;
    }
    return 0;
}

/* ═══════════════ WRITE-THROUGH — serialize fs INTO the mapping ═══════════════
 * In-place update: no re-open, no temp file. Serializes into the mapped
 * region and flushes. Call after bfs_write / bfs_move_seeker / bfs_go_home
 * when using mmap mode. Returns 0 on success. */
static inline int bfs_mmap_sync(BFSMmapFS *mfs)
{
    if (!mfs || !mfs->map_ptr) return -1;
    bfs_img_serialize(&mfs->fs, (uint8_t *)mfs->map_ptr);
    bfs_mmap_flush(mfs);
    mfs->crc_stored = bfs_img_u32(mfs->map_ptr, BFS_IMG_CRC_OFF);
    return 0;
}

/* ═══════════════ RDH BIJECTION VERIFY ═══════════════
 * For every used block: decode payload → re-encode → compare strategy +
 * payload bytes. encode(decode(x)) == x ⟺ lossless bijection at coordinate.
 * Returns number of blocks verified; negative = catastrophic failure. */
static inline int bfs_rdh_verify_all(const BFSMmapFS *mfs)
{
    if (!mfs || !mfs->map_ptr) return -1;
    const BreathingFS *fs = &mfs->fs;
    uint32_t verified = 0, bad = 0;
    for (uint32_t bi = 0; bi < BFS_BLOCKS; bi++) {
        if (fs->block_owner[bi] == 0xFFFFFFFF) continue;  /* free slot */
        uint32_t p_off = mfs->enc_off[bi];
        uint16_t esz = fs->block_encoded_size[bi];
        if (esz == 0 || p_off == 0 || p_off + esz > mfs->map_size) { bad++; continue; }

        DynContainer dc;
        dyn_init(&dc);
        dc.header.strategy = fs->block_meta[bi].strategy;
        dc.header.payload_size = esz;
        memcpy(dc.payload, mfs->map_ptr + p_off, esz);
        dc.header.checksum = dyn_crc32(dc.payload, esz);
        int8_t raw[BFS_SLOTS_BLOCK];
        int rc = dyn_decode(&dc, raw, BFS_SLOTS_BLOCK);
        if (rc != 0) { bad++; continue; }

        DynContainer re;
        dyn_init(&re);
        rc = dyn_encode(&re, raw, BFS_SLOTS_BLOCK);
        if (rc != 0) { bad++; continue; }
        if (re.header.strategy != fs->block_meta[bi].strategy ||
            re.header.payload_size != esz ||
            memcmp(re.payload, mfs->map_ptr + p_off, esz) != 0) {
            bad++; continue;
        }
        verified++;
    }
    if (bad > 0) return -(int)bad;
    return (int)verified;
}

/* ═══════════════ SEEker MVCC ═══════════════
 * version = snapshot index. scale change = new timeline version.
 * snapshot: capture seeker + per-block current/delta.
 * restore:  apply snapshot → seeker back to version's position. */
static inline void bfs_mvcc_snapshot(BreathingFS *fs, BFSMvcc *m)
{
    if (!fs || !m) return;
    if (m->n >= BFS_MVCC_MAX) { /* shift: drop oldest */
        memmove(m->snaps, m->snaps + 1, (BFS_MVCC_MAX - 1) * sizeof(BFSMvccSnap));
        m->n = BFS_MVCC_MAX - 1;
    }
    BFSMvccSnap *s = &m->snaps[m->n];
    memset(s, 0, sizeof(*s));
    s->scale = fs->seeker.scale;
    s->seek_pos = fs->seeker.current_pos;
    s->home_pos = fs->seeker.home_pos;
    s->space_size = fs->seeker.space_size;
    s->window = fs->seeker.window;
    s->hyper = fs->seeker.is_hyperbolic;
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        s->block_pos[b] = fs->block_meta[b].current_pos;
        s->block_delta[b] = fs->block_meta[b].delta;
    }
    m->n++;
}

static inline int bfs_mvcc_restore(BreathingFS *fs, BFSMvcc *m, uint32_t version)
{
    if (!fs || !m || version >= m->n || version >= BFS_MVCC_MAX) return -1;
    const BFSMvccSnap *s = &m->snaps[version];
    fs->seeker.scale = s->scale;
    fs->seeker.current_pos = s->seek_pos;
    fs->seeker.home_pos = s->home_pos;
    fs->seeker.space_size = s->space_size;
    fs->seeker.window = s->window;
    fs->seeker.is_hyperbolic = s->hyper;
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        fs->block_meta[b].current_pos = s->block_pos[b];
        fs->block_meta[b].delta = s->block_delta[b];
    }
    return 0;
}

#endif /* BFS_PERSIST_H */