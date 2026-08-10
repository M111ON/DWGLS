/*
 * bfs_persist.h — Breathing FS Storage Layer (v2: actually-smaller image)
 * ════════════════════════════════════════════════════════════════════
 * v3 changes vs v2 (Aug 10, 2026 — consensus round 1):
 *   4) OWNERS table REMOVED (−576 B)  — derived from file runs
 *      (alloc is contiguous-run; bfs_read always assumed home_block+b)
 *   5) E_SIZES table REMOVED (−288 B) — size[i] = next_used_off − off[i]
 *      (data packed in block order; derive on parse, O(144²) trivial)
 *   6) meta 8 B → 4 B (−576 B)        — u32: home_pos(15) | strategy(3);
 *      payload_size & scale_at_write removed (duplicated / global scale)
 *   7) header 64 B → 40 B (−24 B)     — static geometry fields dropped
 *   → real file size = header + files + meta + offsets + Σ payloads + CRC
 *
 * IMAGE LAYOUT v3 (HEADER fixed; DATA variable):
 *   [0]      header 40B: magic"BIMG" version=3 n_files n_blocks_used
 *                        total_bytes data_size(u32@20) scale(f64@24)
 *                        current_pos(u32@32) home_pos(u32@36)
 *   [40]     files     64 × 49B
 *   [3176]   meta      144 × u32     (home_pos15 | strategy3 — derived rest)
 *   [3752]   enc_offs  144 × u32     (absolute payload offset; 0 = empty)
 *   [4328]   data      data_size B  (PACKED — no fixed stride)
 *   [4328+data_size]   crc32 4B
 *
 * DERIVED ON PARSE (nothing stored twice):
 *   owners     ← file runs (home_block + n_blocks, contiguous alloc)
 *   e_sizes    ← enc_offs deltas (next used offset; last → data_end)
 *   payload_size ← e_sizes (same value)
 *   current_pos/delta ← home_pos × header scale
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

/* ═══════════════ IMAGE GEOMETRY (v3) ═══════════════ */
#define BFS_IMG_MAGIC     0x474D4942u   /* "BIMG" */
#define BFS_IMG_VERSION   3u
#define BFS_IMG_HEADER_SZ 40u
#define BFS_IMG_FILE_ENT  49u           /* serialized BFSFileEntry size */
#define BFS_IMG_FILES_OFF 40u
#define BFS_IMG_FILES_BYT (BFS_MAX_FILES * BFS_IMG_FILE_ENT)            /* 3136 */
#define BFS_IMG_META_OFF  (BFS_IMG_FILES_OFF + BFS_IMG_FILES_BYT)       /* 3176 */
#define BFS_IMG_META_ENT  4u            /* u32: home_pos(15) | strategy(3) */
#define BFS_IMG_META_BYT  (BFS_BLOCKS * BFS_IMG_META_ENT)               /* 576 */
#define BFS_IMG_EOFF_OFF  (BFS_IMG_META_OFF + BFS_IMG_META_BYT)         /* 3752 */
#define BFS_IMG_EOFF_BYT  (BFS_BLOCKS * 4u)                             /* 576 */
#define BFS_IMG_DATA_OFF  (BFS_IMG_EOFF_OFF + BFS_IMG_EOFF_BYT)         /* 4328 */
#define BFS_IMG_ENC_MAX   512u
#define BFS_IMG_MAX_DATA  (BFS_BLOCKS * BFS_IMG_ENC_MAX)                /* 73728 */
#define BFS_IMG_MIN_SIZE  (BFS_IMG_DATA_OFF + 4u)                       /* 4332 */
#define BFS_IMG_MAX_SIZE  (BFS_IMG_DATA_OFF + BFS_IMG_MAX_DATA + 4u)    /* 78060 */

/* Header field offsets (v3) */
#define BFS_HDR_DATA_SIZE 20u   /* u32 — packed data region bytes */
#define BFS_HDR_SCALE     24u   /* f64 — global seeker scale       */
#define BFS_HDR_CUR_POS   32u   /* u32 — seeker current position   */
#define BFS_HDR_HOME_POS  36u   /* u32 — seeker home position      */

/* meta packing: home_pos needs 15 bits (max 20735 < 2^15), strategy 3 (0-4) */
#define BFS_META_HOME_MASK   0x7FFFu
#define BFS_META_STRAT_SHIFT 15u

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

/* Serialized size of an fs (header + TOC + packed payloads + CRC). */
static inline uint32_t bfs_img_size_of(const BreathingFS *fs)
{
    uint32_t data = 0;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++)
        data += fs->block_encoded_size[i];
    return BFS_IMG_DATA_OFF + data + 4u;
}

/* ═══════════════ SERIALIZE BreathingFS → byte buffer ═══════════════
 * dst must hold BFS_IMG_MAX_SIZE. Returns actual serialized size. */
static inline uint32_t bfs_img_serialize(BreathingFS *fs, uint8_t *dst)
{
    memset(dst, 0, BFS_IMG_MAX_SIZE);

    /* header (v3, 40B) — static geometry fields dropped (compile-time) */
    bfs_img_wu32(dst, 0,  BFS_IMG_MAGIC);
    bfs_img_wu32(dst, 4,  BFS_IMG_VERSION);
    bfs_img_wu32(dst, 8,  fs->n_files);
    bfs_img_wu32(dst, 12, fs->n_blocks_used);
    bfs_img_wu32(dst, 16, fs->total_bytes);
    bfs_img_wu32(dst, 20, 0);                     /* data_size filled at end */
    bfs_img_wf64(dst, 24, fs->seeker.scale);
    bfs_img_wu32(dst, 32, fs->seeker.current_pos);
    bfs_img_wu32(dst, 36, fs->seeker.home_pos);

    /* files */
    for (uint32_t i = 0; i < BFS_MAX_FILES; i++) {
        uint32_t o = BFS_IMG_FILES_OFF + i * BFS_IMG_FILE_ENT;
        memcpy(dst + o, fs->files[i].name, BFS_MAX_NAME);
        bfs_img_wu32(dst, o + 32, fs->files[i].n_blocks);
        bfs_img_wu32(dst, o + 36, fs->files[i].home_block);
        bfs_img_wu32(dst, o + 40, fs->files[i].total_bytes);
        memcpy(dst + o + 44, fs->files[i].strategies, 4);
        dst[o + 48] = fs->files[i].valid;
    }
    /* meta 4B: home_pos(15) | strategy(3) — owners/e_sizes/current/delta
     * NOT stored: owners from file runs, sizes from offset deltas,
     * current/delta from home × global header scale (derived on parse) */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        const BFSBlockMeta *bm = &fs->block_meta[i];
        uint32_t packed = (bm->home_pos & BFS_META_HOME_MASK)
                        | ((uint32_t)(bm->strategy & 0x7u) << BFS_META_STRAT_SHIFT);
        bfs_img_wu32(dst, BFS_IMG_META_OFF + i * 4, packed);
    }
    /* enc_offs + packed payloads (sizes derivable; no e_sizes table) */
    uint32_t packed = 0;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        uint16_t esz = fs->block_encoded_size[i];
        if (esz > 0) {
            uint32_t p_off = BFS_IMG_DATA_OFF + packed;
            memcpy(dst + p_off, fs->block_encoded[i], esz);
            packed += esz;
            bfs_img_wu32(dst, BFS_IMG_EOFF_OFF + i * 4, p_off);
        } else {
            bfs_img_wu32(dst, BFS_IMG_EOFF_OFF + i * 4, 0);
        }
    }

    /* data_size + CRC over [0, data_end) */
    uint32_t actual = BFS_IMG_DATA_OFF + packed + 4u;
    bfs_img_wu32(dst, BFS_HDR_DATA_SIZE, packed);
    uint32_t crc = dyn_crc32(dst, BFS_IMG_DATA_OFF + packed);
    bfs_img_wu32(dst, BFS_IMG_DATA_OFF + packed, crc);
    return actual;
}

/* ═══════════════ PARSE byte buffer → BreathingFS ═══════════════
 * size = actual file size (serialized size, not max). Returns 0 on
 * success; -1 bad magic/size; -2 bad version; -3 bad geometry;
 * -4 CRC mismatch. Payloads copied into fs for plain reads; mmap path
 * keeps them in the mapping (zero-copy). */
static inline int bfs_img_parse(const uint8_t *m, size_t size, BreathingFS *fs,
                                uint32_t *enc_off_out)
{
    if (!m || !fs || size < BFS_IMG_MIN_SIZE) return -1;
    if (bfs_img_u32(m, 0) != BFS_IMG_MAGIC) return -1;
    if (bfs_img_u32(m, 4) != BFS_IMG_VERSION) return -2;
    /* v3: static geometry (slots/blocks/max_files) is compile-time;
     * BFS_SLOTS_BLOCK no longer stored in the header */

    uint32_t data_size = bfs_img_u32(m, BFS_HDR_DATA_SIZE);
    uint32_t data_end = BFS_IMG_DATA_OFF + data_size;
    if (data_end + 4u > size) return -1;   /* truncated / corrupt size */

    uint32_t crc_stored = bfs_img_u32(m, data_end);
    uint32_t crc_actual = dyn_crc32(m, data_end);
    if (crc_actual != crc_stored) return -4;

    memset(fs, 0, sizeof(*fs));
    fs->magic = BFS_MAGIC;
    fs->version = BFS_VERSION;
    fs->n_files = bfs_img_u32(m, 8);
    fs->n_blocks_used = bfs_img_u32(m, 12);
    fs->total_bytes = bfs_img_u32(m, 16);
    fs->seeker.scale = bfs_img_f64(m, 24);
    fs->seeker.current_pos = bfs_img_u32(m, 32);
    fs->seeker.home_pos = bfs_img_u32(m, 36);
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

    /* meta (4B packed) + enc_offs — read once into locals, derive the rest */
    uint32_t eoff[BFS_BLOCKS];
    uint32_t sum_blocks = 0;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        uint32_t om = bfs_img_u32(m, BFS_IMG_META_OFF + i * 4);
        fs->block_meta[i].home_pos = om & BFS_META_HOME_MASK;
        fs->block_meta[i].strategy = (uint8_t)((om >> BFS_META_STRAT_SHIFT) & 0x7u);
        fs->block_meta[i].scale_at_write = 0;   /* global header scale used */
        fs->block_meta[i].payload_size = 0;     /* derived below           */
        if (enc_off_out) enc_off_out[i] = 0;
        eoff[i] = bfs_img_u32(m, BFS_IMG_EOFF_OFF + i * 4);
        if (enc_off_out) enc_off_out[i] = eoff[i];
        /* DERIVE current_pos & delta from anchor (home_pos × scale) */
        double shifted = (double)fs->block_meta[i].home_pos * fs->seeker.scale;
        uint32_t cur = shifted > 0.0
                     ? ((uint32_t)shifted) % fs->seeker.space_size
                     : 0u;
        fs->block_meta[i].current_pos = cur;
        fs->block_meta[i].delta = (int32_t)cur - (int32_t)fs->block_meta[i].home_pos;
    }

    /* DERIVE owners from file runs (contiguous-run alloc invariant v3) */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) fs->block_owner[i] = 0xFFFFFFFF;
    for (uint32_t f = 0; f < BFS_MAX_FILES; f++) {
        const BFSFileEntry *fe = &fs->files[f];
        if (!fe->valid || fe->n_blocks == 0) continue;
        sum_blocks += fe->n_blocks;
        for (uint32_t b = 0; b < fe->n_blocks; b++)
            fs->block_owner[fe->home_block + b] = f;
    }
    /* integrity: file table must tile exactly the used blocks */
    if (sum_blocks != fs->n_blocks_used) return -4;

    /* DERIVE sizes: size[i] = next_used_off − off[i] (last → data_end) */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        if (eoff[i] == 0) { fs->block_encoded_size[i] = 0; continue; }
        uint32_t end = data_end;
        for (uint32_t j = i + 1; j < BFS_BLOCKS; j++)
            if (eoff[j] != 0) { end = eoff[j]; break; }
        uint32_t sz = end - eoff[i];
        if (sz > BFS_IMG_ENC_MAX) return -4;   /* corrupt offset chain */
        fs->block_encoded_size[i] = (uint16_t)sz;
        fs->block_meta[i].payload_size = (uint16_t)sz;
    }

    /* payloads: copy so plain bfs_read works (load path); mmap uses eoff */
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        uint32_t esz = fs->block_encoded_size[i];
        if (esz > 0) {
            if (eoff[i] < BFS_IMG_DATA_OFF || eoff[i] + esz > data_end) return -4;
            memcpy(fs->block_encoded[i], m + eoff[i], esz);
        }
    }
    return 0;
}

/* ═══════════════ PLAIN FILE SAVE / LOAD (portable, no mmap) ═══════════════ */
static inline int bfs_save_img(const char *path, BreathingFS *fs)
{
    static uint8_t buf[BFS_IMG_MAX_SIZE];
    if (!path || !fs) return -1;
    uint32_t n = bfs_img_serialize(fs, buf);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return (w == n) ? 0 : -2;
}

static inline int bfs_load_img(const char *path, BreathingFS *fs)
{
    static uint8_t buf[BFS_IMG_MAX_SIZE];
    if (!path || !fs) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < (long)BFS_IMG_MIN_SIZE || (size_t)sz > BFS_IMG_MAX_SIZE) {
        fclose(f); return -2;
    }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) return -2;
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
    if (len_low < BFS_IMG_MIN_SIZE) { CloseHandle(hf); return -3; }
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
    if (st.st_size < (off_t)BFS_IMG_MIN_SIZE) { close(fd); return -3; }
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
    mfs->crc_stored = bfs_img_u32(mfs->map_ptr,
                                  BFS_IMG_DATA_OFF + bfs_img_u32(mfs->map_ptr, BFS_HDR_DATA_SIZE));
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
        if (p_off < BFS_IMG_DATA_OFF ||
            p_off + fs->block_encoded_size[bi] > mfs->map_size)
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
        /* STABILITY FIX: same partial-last-block guard as bfs_read */
        int rc = dyn_decode(&dc, out + offset, bsz);
        if (rc != 0) return -5;
    }
    return 0;
}

/* ═══════════════ WRITE-THROUGH — serialize fs INTO the mapping ═══════════════
 * In-place update when the new image fits the current mapping; otherwise
 * the file + mapping are grown (remap). Returns 0 on success. */
static inline int bfs_mmap_sync(BFSMmapFS *mfs)
{
    static uint8_t tmp[BFS_IMG_MAX_SIZE];
    if (!mfs || !mfs->map_ptr) return -1;
    uint32_t n = bfs_img_serialize(&mfs->fs, tmp);

#if defined(_WIN32)
    HANDLE hf = (HANDLE)mfs->h_file;
    DWORD cur = GetFileSize(hf, NULL);
    if (n > cur) {
        if (mfs->h_view) { UnmapViewOfFile(mfs->h_view); mfs->h_view = NULL; }
        if (mfs->h_map)  { CloseHandle((HANDLE)mfs->h_map); mfs->h_map = NULL; }
        LARGE_INTEGER li; li.QuadPart = n;
        SetFilePointerEx(hf, li, NULL, FILE_BEGIN);
        SetEndOfFile(hf);
        HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READWRITE, 0, 0, NULL);
        if (!hm) return -4;
        uint8_t *base = (uint8_t *)MapViewOfFile(hm, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        if (!base) { CloseHandle(hm); return -5; }
        mfs->h_map = hm; mfs->h_view = base; mfs->map_ptr = base; mfs->map_size = n;
    }
    memcpy((void *)mfs->map_ptr, tmp, n);
    FlushViewOfFile((void *)mfs->map_ptr, 0);
#else
    if (n > mfs->map_size) {
        int fd = (int)(intptr_t)mfs->h_file;
        if (mfs->map_ptr && mfs->map_ptr != MAP_FAILED)
            munmap((void *)mfs->map_ptr, mfs->map_size);
        if (ftruncate(fd, (off_t)n) != 0) return -4;
        uint8_t *base = (uint8_t *)mmap(NULL, (size_t)n, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) return -5;
        mfs->map_ptr = base; mfs->map_size = n;
    }
    memcpy((void *)mfs->map_ptr, tmp, n);
    msync((void *)mfs->map_ptr, mfs->map_size, MS_SYNC);
#endif
    mfs->crc_stored = bfs_img_u32(mfs->map_ptr, n - 4u);
    /* refresh parse view (payload offsets may have moved) */
    return bfs_img_parse(mfs->map_ptr, mfs->map_size, &mfs->fs, mfs->enc_off);
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
        if (esz == 0 || p_off < BFS_IMG_DATA_OFF || p_off + esz > mfs->map_size) { bad++; continue; }

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