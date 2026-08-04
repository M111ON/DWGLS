/*
 * geo_diamond_field.h — Diamond Field v3 (Shell + Slot + Tring)
 *                       + Classifier v2 + Sparse + Batch mode
 *
 * Core concept:
 *   block  = geometry slot (bit in shell)
 *   data   = Tring timeline (external, tick-indexed, variable-size)
 *   delete = clear flag (data survives until GC)
 *   reshape= remap slots to new shell level
 *   batch  = group K chunks → 5B/chunk (L1=8, L2=64, L3=512)
 *
 * Shell scale: level n → size = 2n+1 → slots = (2n+1)^3
 *   n=0: 1    n=1: 27   n=2: 125  n=3: 343
 *   n=4: 729  n=5: 1331 n=6: 2197 n=7: 3375  n=8: 4913
 *
 * Classifier: score = popcnt(isect) + uniq_count - var_norm
 * Sparse: n <= 2 → [count][pos,val]... format
 * Batch:  group K chunks → shared header + base + adaptive bitmask diffs (≤12 diffs)
 *
 * depends: tring.h, pogls_fold.h
 */
#ifndef GEO_DIAMOND_FIELD_H
#define GEO_DIAMOND_FIELD_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "tring.h"
#include "pogls_fold.h"

/* ── constants ────────────────────────────────────────────────────── */
#define SHELL_MAX_LEVEL   8u
#define SHELL_MAX_SLOTS   4913u          /* 17^3, level 8 */
#define SHELL_IDX_BITS    17u            /* 131072/level: 8192 base × 16 expansion */
#define SHELL_IDX_MASK    ((1u << SHELL_IDX_BITS) - 1u)  /* 0x1FFFF */
#define TRING_MAX_TICKS   (1u << 20)     /* 1M ticks, caller can extend */
#define SLOT_NULL         0xFFFFFFFFu

/* sparse encoding threshold: n <= SPARSE_MAX_LEVEL → sparse store */
#define SPARSE_MAX_LEVEL  2u

/* batch constants */
#define BATCH_L1          8u
#define BATCH_L2         64u
#define BATCH_L3        512u
#define BATCH_MAX_DIFF   12u            /* max differing bytes from base */
#define SHELL_ROT_STATES  6u            /* 6 face-forward 3D orientations */

/* forward declarations */
static inline void shell_hilbert_invert(uint64_t *invert, uint8_t n);
static inline uint64_t chunk_geometric_hash(const uint8_t chunk[64]);
static inline void _batch_hilbert_d2xy_n(int n, int d, int *x, int *y);
static inline uint8_t _batch_rotation_scan(const uint8_t *chunks, uint32_t K);

/* ── Shell ─────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  level;
    uint8_t  size;           /* = 2n+1                            */
    uint16_t slot_count;     /* = size^3 (max 4913)               */
    uint16_t occupied;       /* fast full-shell detection         */
    uint16_t _pad;
    uint64_t flags[77];      /* ceil(4913/64) = 77 words = 616B   */
} Shell;

static inline uint16_t shell_size(uint8_t n)   { return (uint16_t)(2u*n + 1u); }
static inline uint16_t shell_slots(uint8_t n)  { uint16_t s = shell_size(n); return (uint16_t)(s*s*s); }

static inline void shell_init(Shell *sh, uint8_t n) {
    memset(sh, 0, sizeof(*sh));
    sh->level      = n;
    sh->size       = (uint8_t)shell_size(n);
    sh->slot_count = shell_slots(n);
}

static inline void  shell_set(Shell *sh, uint16_t idx)
    { if (!(sh->flags[idx >> 6] & (1ULL << (idx & 63u)))) { sh->flags[idx >> 6] |= (1ULL << (idx & 63u)); sh->occupied++; } }
static inline void  shell_clr(Shell *sh, uint16_t idx)
    { if (sh->flags[idx >> 6] & (1ULL << (idx & 63u))) { sh->flags[idx >> 6] &= ~(1ULL << (idx & 63u)); sh->occupied--; } }
static inline int   shell_get(const Shell *sh, uint16_t idx)
    { return (int)((sh->flags[idx >> 6] >> (idx & 63u)) & 1u); }
static inline int   shell_any(const Shell *sh)
    { for (int i=0;i<77;i++) if (sh->flags[i]) return 1; return 0; }
static inline int   shell_full(const Shell *sh)
    { return sh->occupied >= sh->slot_count; }

/* ── Slot ──────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t x, y, z;
    uint16_t idx;
} Slot;

static inline Slot slot_from_idx(uint16_t idx, uint8_t size) {
    Slot s; s.idx = idx;
    s.z = (uint16_t)(idx / ((uint16_t)size * size));
    s.y = (uint16_t)((idx / size) % size);
    s.x = (uint16_t)(idx % size);
    return s;
}
static inline uint16_t slot_to_idx(uint16_t x, uint16_t y, uint16_t z, uint8_t size) {
    return (uint16_t)(x + y*(uint16_t)size + z*(uint16_t)size*(uint16_t)size);
}

static inline uint32_t slot_global(uint8_t n, uint16_t local_idx) {
    return ((uint32_t)n << SHELL_IDX_BITS) | (local_idx & SHELL_IDX_MASK);
}
static inline uint8_t  global_level(uint32_t gidx) { return (uint8_t)(gidx >> SHELL_IDX_BITS); }
static inline uint16_t global_local(uint32_t gidx) { return (uint16_t)(gidx & SHELL_IDX_MASK); }

/* ── Level classifier v2 ─────────────────────────────────────────── */
static inline int shell_classify_score(const uint8_t chunk[64]) {
    const DiamondBlock *b = (const DiamondBlock *)chunk;
    uint64_t isect = fold_fibo_intersect(b);
    int pc = __builtin_popcountll(isect);
    uint16_t hist[256] = {0};
    uint32_t sum = 0; int uniq = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t v = chunk[i];
        if (hist[v] == 0) uniq++;
        hist[v]++; sum += v;
    }
    uint8_t mean = (uint8_t)(sum / 64);
    uint32_t abs_dev = 0;
    for (int i = 0; i < 64; i++) {
        int d = (int)chunk[i] - mean;
        abs_dev += d < 0 ? (uint32_t)(-d) : (uint32_t)d;
    }
    int var_norm = (int)(abs_dev / 128);
    return pc + uniq - var_norm;
}

static const int8_t SCORE_THRESH[9] = { 2, 6, 12, 20, 30, 40, 50, 60, 120 };

static inline uint8_t shell_classify_level(const uint8_t chunk[64]) {
    int score = shell_classify_score(chunk);
    for (uint8_t n = 0; n <= SHELL_MAX_LEVEL; n++)
        if (score <= SCORE_THRESH[n]) return n;
    return SHELL_MAX_LEVEL;
}

/* ── map chunk → slot index ───────────────────────────────────────── */
#define FNV64_OFFSET 14695981039346656037ULL
#define FNV64_PRIME  1099511628211ULL

static inline uint64_t _fnv64(const uint8_t *data, uint32_t len) {
    uint64_t h = FNV64_OFFSET;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ data[i]) * FNV64_PRIME;
    return h;
}

static inline uint16_t chunk_to_slot_idx(const uint8_t chunk[64], uint8_t n) {
    uint16_t cap = shell_slots(n);
    if (cap == 0) return 0;
    return (uint16_t)(_fnv64(chunk, 64) % cap);
}

/* ── Index — slot → tick mapping ──────────────────────────────────── */
#define INDEX_SIZE ((9u << SHELL_IDX_BITS))   /* 9*131072 = 1,179,648 entries */

/* ── ×16 sub-slot slope: XOR-fold chunk → 4-bit (apex pattern) ── */
static inline uint8_t _x16_slope(const uint8_t chunk[64]) {
    uint64_t fold = 0;
    for (int i = 0; i < 64; i += 8) {
        uint64_t w; memcpy(&w, chunk + i, 8); fold ^= w;
    }
    fold ^= fold >> 32;
    fold ^= fold >> 16;
    fold ^= fold >> 8;
    return (uint8_t)(fold & 0xFu);
}

typedef struct {
    uint32_t *tick;  /* heap-allocated, INDEX_SIZE entries, SLOT_NULL if empty */
} SlotIndex;

static inline int sidx_init(SlotIndex *si) {
    si->tick = (uint32_t *)malloc(INDEX_SIZE * sizeof(uint32_t));
    if (!si->tick) return -1;
    memset(si->tick, 0xFF, INDEX_SIZE * sizeof(uint32_t));
    return 0;
}
static inline void sidx_destroy(SlotIndex *si) {
    free(si->tick); si->tick = NULL;
}
static inline void    sidx_set(SlotIndex *si, uint32_t gidx, uint32_t tick)
    { if (si->tick && gidx < INDEX_SIZE) si->tick[gidx] = tick; }
static inline uint32_t sidx_get(const SlotIndex *si, uint32_t gidx)
    { return (si->tick && gidx < INDEX_SIZE) ? si->tick[gidx] : SLOT_NULL; }
static inline void    sidx_clear(SlotIndex *si, uint32_t gidx)
    { if (si->tick && gidx < INDEX_SIZE) si->tick[gidx] = SLOT_NULL; }

/* batch sidx pack/unpack: non-batch = tick directly; batch = bit31|(tick<<8)|pos */
#define BATCH_SIDX_FLAG (1u << 31)

static inline uint32_t sidx_pack_batch(uint32_t tick, uint8_t pos) {
    return BATCH_SIDX_FLAG | (tick << 8) | pos;
}
static inline int      sidx_is_batch(uint32_t v)   { return (v & BATCH_SIDX_FLAG) != 0; }
static inline uint32_t sidx_batch_tick(uint32_t v) { return (v >> 8) & 0x7FFFFF; }
static inline uint8_t  sidx_batch_pos(uint32_t v)  { return (uint8_t)(v & 0xFF); }

/* ── Flow state: carry path/seed/rot across chunks ──────────────── */
/*
 * Encoding paths: 0=sparse, 1=bitpack, 2=LZ77, 3=LZ+Hilbert, 4=raw
 * Flow memory biases decisions toward previous path when data is similar.
 */
#define FLOW_PATH_SPARSE     0
#define FLOW_PATH_BITPACK    1
#define FLOW_PATH_LZ77       2
#define FLOW_PATH_LZ_HILBERT 3
#define FLOW_PATH_RAW        4
#define FLOW_WINDOW_MIN      8u
#define FLOW_WINDOW_MAX     32u
#define FLOW_LOCK_STREAK     4u    /* after N same-path chunks, lock path */
#define FLOW_HASH_SIMILAR    6u    /* top 6 bits of hash must match for bias */

typedef struct {
    uint8_t  prev_path;       /* last encoding path used */
    uint64_t prev_seed;       /* last chunk geometric hash */
    uint8_t  prev_rot;        /* last rotation used */
    uint8_t  path_streak;     /* consecutive chunks with same path */
    uint8_t  locked_path;     /* if <5, force this path for next chunks */
    uint8_t  window_path_scores[5]; /* accumulated scores per path in window */
    uint8_t  window_pos;      /* current position in window */
} FlowState;

static inline void flow_init(FlowState *fs) {
    memset(fs, 0, sizeof(*fs));
    fs->locked_path = 5; /* 5 = no lock */
}

/* ── DiamondField ──────────────────────────────────────────────────── */
typedef struct {
    Shell     shell[9];
    Tring     tring;
    SlotIndex sidx;
    uint8_t   batch_prev_rot;  /* last rotation used for batch pruning */
    uint8_t   x16;             /* ×16 capacity mode (0/1)             */
    FlowState flow;            /* flow-aware routing state */
} DiamondField;

static inline int dfield_init(DiamondField *df, uint32_t tring_cap) {
    for (uint8_t n = 0; n <= SHELL_MAX_LEVEL; n++)
        shell_init(&df->shell[n], n);
    if (sidx_init(&df->sidx) != 0) return -1;
    df->batch_prev_rot = 0;
    df->x16 = 0;
    flow_init(&df->flow);
    return tring_init(&df->tring, tring_cap ? tring_cap : TRING_MAX_TICKS);
}

static inline void dfield_free(DiamondField *df) {
    tring_destroy(&df->tring);
    sidx_destroy(&df->sidx);
}

static inline void dfield_set_x16(DiamondField *df, int val) {
    df->x16 = val ? 1u : 0u;
}

/* ── Sparse encode / decode ───────────────────────────────────────── */
static inline uint32_t sparse_encode(uint8_t out[66], const uint8_t chunk[64]) {
    uint8_t idx[64], val[64]; int count = 0;
    for (int i = 0; i < 64; i++) { if (chunk[i]) { idx[count]=(uint8_t)i; val[count]=chunk[i]; count++; } }
    if (count == 0) { out[0] = 0; return 1; }
    if (1 + 2 * count >= 64) return 0;
    out[0] = (uint8_t)count;
    for (int i = 0; i < count; i++) { out[1+i*2]=idx[i]; out[1+i*2+1]=val[i]; }
    return (uint32_t)(1 + count * 2);
}

static inline void sparse_decode(uint8_t out[64], const uint8_t *in, uint32_t size) {
    memset(out, 0, 64);
    if (!in || size < 1) return;
    int count = (int)in[0]; if (count > 31) count = 31;
    for (int i = 0; i < count; i++) { uint8_t p = in[1+i*2]; if (p < 64) out[p] = in[1+i*2+1]; }
}

/* ── DENSE encode / decode (v2-style, 17B, lossless for DiamondBlock data) ─ */
/*
 * For chunks with strong geometric structure (n ≥ 3, seed_pc ≥ 8):
 *   store [rot:1B][core:16B] = 17B
 *   core = bytes 0-15 of chunk (Core Slot + Invert region)
 *   Decode: reconstruct DiamondBlock from bytes 0-7 → full 64B via quad_mirror
 *   Lossless when bytes 16-63 match DiamondBlock structure (geometric data)
 *   For non-geometric data: bytes 16-63 are regenerated → lossy
 *
 * Use in geometric pipeline where caller guarantees DiamondBlock-compatible data.
 */
#define DENSE_NODE_SIZE  17u

static inline void dense_encode(uint8_t out[DENSE_NODE_SIZE], const uint8_t chunk[64], uint8_t rot) {
    out[0] = rot;              /* 0-5 */
    memcpy(out + 1, chunk, 16); /* bytes 0-15 of chunk (not rotated) */
}

/* Decode DENSE: reconstruct 64B from 16B core + rotation */
/* Produces lossless output only for DiamondBlock-structured data */
static inline void dense_decode(uint8_t out[64], const uint8_t dense[DENSE_NODE_SIZE]) {
    uint8_t rot = dense[0];
    /* Reconstruct DiamondBlock from bytes 0-7 */
    DiamondBlock db;
    memset(&db, 0, sizeof(db));
    memcpy(&db.core.raw, dense + 1, 8);
    db.invert = ~db.core.raw;
    fold_build_quad_mirror(&db);
    /* bytes 8-15: use stored (preserves original invert if valid) */
    memcpy(((uint8_t*)&db) + 8, dense + 1 + 8, 8);
    memcpy(out, &db, 64);
    /* Apply inverse rotation if needed (rot != 0 means caller must inverse_rotate) */
    (void)rot; /* caller's responsibility to inverse_rotate out[] by rot */
}

/* ── Batch adaptive bitmask diff ──────────────────────────────────── */
/*
 * Wire format per chunk: [mask:8B][values:count*1B]
 *   mask    = 64-bit bitmask: bit i set → byte i differs from base
 *   values  = new byte values for each set bit (in position order)
 *   Returns 8 + count bytes, or 0 if count > BATCH_MAX_DIFF (fallback)
 */
static inline uint32_t batch_encode_diff(uint8_t out[], const uint8_t base[64], const uint8_t chunk[64]) {
    uint64_t mask = 0;
    uint8_t vals[64];
    int count = 0;
    for (int i = 0; i < 64; i++) {
        if (chunk[i] != base[i]) {
            mask |= (1ULL << i);
            vals[count++] = chunk[i];
        }
    }
    if (count > BATCH_MAX_DIFF) return 0;
    memcpy(out, &mask, 8);
    memcpy(out + 8, vals, (size_t)count);
    return 8 + (uint32_t)count;
}

/* Reconstruct chunk from base + bitmask diff. diff must be valid. */
static inline void batch_decode_diff(uint8_t out[64], const uint8_t base[64], const uint8_t diff_data[]) {
    memcpy(out, base, 64);
    uint64_t mask;
    memcpy(&mask, diff_data, 8);
    const uint8_t *vals = diff_data + 8;
    int vi = 0;
    for (int i = 0; i < 64; i++)
        if (mask & (1ULL << i)) out[i] = vals[vi++];
}

/* Given batch_data (tring node content), find byte offset of diff for chunk pos */
static inline uint32_t batch_diff_offset(const uint8_t *batch_data, uint8_t pos) {
    uint8_t flags = batch_data[3];
    uint32_t base_off = 4 + ((flags & 1) ? 8 : 0); /* skip header + optional seed */
    uint32_t off = base_off + 64; /* skip base */
    for (uint8_t j = 0; j < pos; j++) {
        uint64_t mask;
        memcpy(&mask, batch_data + off, 8);
        off += 8 + __builtin_popcountll(mask);
    }
    return off;
}

/* Compute base chunk as byte-wise median across N chunks */
static inline void batch_compute_base(uint8_t base[64], const uint8_t *chunks, uint32_t count, uint32_t stride) {
    /* for each byte position, pick median value */
    uint8_t vals[512]; /* max BATCH_L3 = 512 */
    for (int pos = 0; pos < 64; pos++) {
        for (uint32_t i = 0; i < count; i++) vals[i] = chunks[i * stride + pos];
        /* simple median: sort partial */
        for (uint32_t i = 0; i < count; i++)
            for (uint32_t j = i + 1; j < count; j++)
                if (vals[i] > vals[j]) { uint8_t t = vals[i]; vals[i] = vals[j]; vals[j] = t; }
        base[pos] = vals[count / 2];
    }
}

/* ── Entropy encoding: LZ77 + bitpack (self-contained) ──────────── */
#define ENTROPY_LZ_TAG      0xFD
#define ENTROPY_BITPACK_TAG 0xFC
#define ENTROPY_STRUCT_PC   16

static inline uint32_t lz77_compress(const uint8_t in[64], uint8_t out[64]) {
    uint32_t ipos = 0, opos = 0;
    while (ipos < 64 && opos + 8 < 64) {
        uint32_t flag_pos = opos++;
        uint8_t flags = 0;
        uint32_t ops = 0;
        for (int bit = 0; bit < 8 && ipos < 64; bit++, ops++) {
            uint32_t max_off = ipos < 255 ? ipos : 255;
            uint32_t best_len = 1, best_off = 0;
            for (uint32_t off = 1; off <= max_off; off++) {
                uint32_t len = 0;
                while (ipos + len < 64 && len < 17 && in[ipos+len]==in[ipos+len-off]) len++;
                if (len > best_len) { best_len = len; best_off = off; }
            }
            if (best_len >= 3) {
                flags |= (uint8_t)(1 << bit);
                if (opos + 2 > 63) return 0;
                out[opos++] = (uint8_t)(best_off);
                out[opos++] = (uint8_t)(best_len - 2);
                ipos += best_len;
            } else {
                if (opos + 1 > 63) return 0;
                out[opos++] = in[ipos++];
            }
        }
        out[flag_pos] = flags;
        if (ops == 0 || ipos >= 64) break;
    }
    return ipos >= 64 ? opos : 0;
}

static inline uint32_t lz77_decompress(const uint8_t in[], uint32_t in_sz, uint8_t out[64]) {
    uint32_t ipos = 0, opos = 0;
    while (ipos < in_sz && opos < 64) {
        if (ipos + 1 > in_sz) return 0;
        uint8_t flags = in[ipos++];
        for (int bit = 0; bit < 8 && ipos <= in_sz && opos < 64; bit++) {
            if (flags & (1 << bit)) {
                if (ipos + 2 > in_sz) return 0;
                uint8_t off = in[ipos++];
                uint8_t len = (uint8_t)(in[ipos++] + 2);
                if (off > opos) return 0;

                uint32_t src = opos - off;
                uint32_t end = opos + len;
                if (end > 64) end = 64;

                if (off >= 8) {
                    /* Word-aligned copy: 8 bytes at a time */
                    while (opos + 8 <= end) {
                        uint64_t word;
                        memcpy(&word, out + src, 8);
                        memcpy(out + opos, &word, 8);
                        opos += 8;
                        src += 8;
                    }
                }
                /* Byte tail (handles overlap when off < 8) */
                while (opos < end) {
                    out[opos] = out[src];
                    opos++;
                    src++;
                }
            } else { out[opos++] = in[ipos++]; }
        }
    }
    return opos;
}

static inline uint32_t bitpack_encode(const uint8_t in[64], uint8_t out[64]) {
    memset(out, 0, 64);
    uint8_t hist[256] = {0}; int uniq = 0;
    for (int i = 0; i < 64; i++) { if (hist[in[i]] == 0) uniq++; hist[in[i]]++; }
    int nbits = 0;
    if (uniq == 1) nbits = 0;
    else if (uniq <= 2) nbits = 1;
    else if (uniq <= 4) nbits = 2;
    else if (uniq <= 16) nbits = 4;
    else return 0;
    uint8_t lut[16]; int li = 0;
    for (int i = 0; i < 256 && li < uniq; i++) if (hist[i]) lut[li++] = (uint8_t)i;
    uint32_t opos = 0;
    out[opos++] = (uint8_t)nbits; out[opos++] = (uint8_t)uniq;
    for (int i = 0; i < uniq && opos < 64; i++) out[opos++] = lut[i];
    if (nbits > 0) {
        uint32_t bitpos = 0;
        for (int i = 0; i < 64; i++) {
            int idx = 0; for (int j = 0; j < uniq; j++) if (lut[j]==in[i]) { idx=j; break; }
            for (int b = 0; b < nbits; b++, bitpos++)
                if (idx & (1 << b)) out[opos + (bitpos>>3)] |= (uint8_t)(1 << (bitpos&7));
        }
        opos += (bitpos + 7) / 8;
    }
    return opos < 64 ? opos : 0;
}

static inline uint32_t bitpack_decode(const uint8_t in[], uint32_t in_sz, uint8_t out[64]) {
    if (in_sz < 2) return 0;
    uint8_t nbits = in[0], uniq = in[1];
    if (uniq == 0 || uniq > 16 || in_sz < (uint32_t)(2 + uniq)) return 0;
    uint8_t lut[16]; for (int i = 0; i < uniq; i++) lut[i] = in[2 + i];
    if (nbits == 0) { memset(out, lut[0], 64); return 64; }
    uint32_t ipos = 2 + uniq, bitpos = 0;
    for (int i = 0; i < 64; i++) {
        int idx = 0;
        for (int b = 0; b < nbits; b++, bitpos++) {
            if (ipos + (bitpos>>3) >= in_sz) return 0;
            if (in[ipos + (bitpos>>3)] & (1 << (bitpos&7))) idx |= (1 << b);
        }
        if (idx >= uniq) return 0;
        out[i] = lut[idx];
    }
    return 64;
}

/* ── Hilbert reorder pre-transform (8×8 Hilbert curve) ──────────── */
#define ENTROPY_LZ_HILBERT_TAG 0xFB

/* forward declaration (defined later with batch helpers) */
static inline void _batch_hilbert_d2xy_n(int n, int d, int *x, int *y);

static inline void _hilbert_reorder(uint8_t out[64], const uint8_t in[64], uint8_t offset) {
    int n = 8; /* 8×8 grid for 64B chunk */
    for (int d = 0; d < 64; d++) {
        int x, y;
        _batch_hilbert_d2xy_n(n, (d + offset * 16) % 64, &x, &y);
        out[y * n + x] = in[d];
    }
}
static inline void _hilbert_unreorder(uint8_t out[64], const uint8_t in[64], uint8_t offset) {
    uint8_t tmp[64]; int n = 8;
    for (int d = 0; d < 64; d++) {
        int x, y;
        _batch_hilbert_d2xy_n(n, (d + offset * 16) % 64, &x, &y);
        tmp[d] = in[y * n + x];
    }
    memcpy(out, tmp, 64);
}

/* ── Fast entropy estimator (cheap, no compression trials) ──────── */
/*
 * Returns: 0=low entropy (try all methods), 1=medium (skip Hilbert), 2=high (raw only)
 * Cost: ~64 byte ops — much cheaper than trying 7 compression methods.
 */
static inline int _chunk_entropy_class(const uint8_t chunk[64]) {
    uint32_t sum = 0, abs_dev = 0;
    uint16_t hist[16] = {0}; /* coarse 16-bin histogram */
    int uniq = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t v = chunk[i];
        sum += v;
        int bin = v >> 4; /* 16 bins */
        if (hist[bin] == 0) uniq++;
        hist[bin]++;
    }
    uint8_t mean = (uint8_t)(sum >> 6);
    for (int i = 0; i < 64; i++) {
        int d = (int)chunk[i] - mean;
        abs_dev += d < 0 ? (uint32_t)(-d) : (uint32_t)d;
    }
    /* High entropy: many unique bins + high deviation */
    if (uniq > 10 && abs_dev > 1800) return 2; /* raw only */
    if (uniq > 6  && abs_dev > 1200) return 1; /* skip Hilbert */
    return 0;                                   /* try all */
}

/* ── ENCODE with adaptive method selection ───────────────────────── */
/*
 * For each chunk, try ALL methods and pick the smallest output:
 *   sparse → bitpack → LZ77 → LZ77+Hilbert(4 offsets) → raw(64B)
 * Early-exit: entropy estimator skips expensive trials for high-entropy data.
 */
static inline uint32_t dfield_encode(DiamondField *df,
                                       const uint8_t  chunk[64],
                                       uint8_t       *out_level)
{
    uint8_t n = shell_classify_level(chunk);
    if (out_level) *out_level = n;
    uint16_t local_idx = chunk_to_slot_idx(chunk, n);
    uint32_t gidx      = slot_global(n, local_idx);
    while (n <= SHELL_MAX_LEVEL) {
        uint16_t cap_orig = df->shell[n].slot_count;
        if (!df->x16 && shell_full(&df->shell[n])) { n++; continue; }
        int found = 0;
        if (df->x16) {
            uint32_t base = (uint32_t)(_fnv64(chunk, 64) % cap_orig);
            uint8_t slope = _x16_slope(chunk);
            for (int s = 0; s < 16; s++) {
                uint32_t eff = base * 16u + ((uint32_t)(slope + (uint8_t)s) & 0xFu);
                uint32_t gp = slot_global(n, (uint16_t)eff);
                if (sidx_get(&df->sidx, gp) == SLOT_NULL) { local_idx = (uint16_t)eff; gidx = gp; found = 1; break; }
            }
        } else {
            uint32_t cap = cap_orig;
            uint32_t probe = chunk_to_slot_idx(chunk, n);
            for (uint32_t i = 0; i < cap; i++) {
                uint16_t idx = (uint16_t)probe;
                int occupied = shell_get(&df->shell[n], idx) ||
                               (sidx_get(&df->sidx, slot_global(n, idx)) != SLOT_NULL);
                if (!occupied) { local_idx = idx; gidx = slot_global(n, idx); found = 1; break; }
                probe = (probe + 1u) % cap;
            }
        }
        if (found) { if (out_level) *out_level = n; break; }
        n++;
    }
    if (n > SHELL_MAX_LEVEL) return SLOT_NULL;

    /* Fast entropy check: skip expensive trials for high-entropy data */
    int eclass = _chunk_entropy_class(chunk);

    /* Adaptive: try all methods, pick smallest */
    uint8_t best_buf[68];    /* room for tag + offset + max compressed */
    uint32_t best_sz = 64;   /* default: raw 64B */
    uint32_t tick;

    if (eclass == 2) {
        /* High entropy: store raw immediately */
        tick = tring_push(&df->tring, chunk, 64);
        if (tick == UINT32_MAX) return SLOT_NULL;
        if (!df->x16 || local_idx < df->shell[n].slot_count) shell_set(&df->shell[n], local_idx);
        sidx_set(&df->sidx, gidx, tick);
        return gidx;
    }

    /* 1. try sparse */
    {
        uint8_t tbuf[66];
        uint32_t ssz = sparse_encode(tbuf, chunk);
        if (ssz > 0 && ssz < best_sz) {
            best_sz = ssz;
            best_buf[0] = 0;
            memcpy(best_buf + 1, tbuf, ssz);
        }
    }

    /* 2. try bitpack */
    {
        uint8_t tbuf[64];
        uint32_t bsz = bitpack_encode(chunk, tbuf);
        if (bsz > 0 && bsz + 1 < best_sz) {
            best_sz = bsz + 1;
            best_buf[0] = ENTROPY_BITPACK_TAG;
            memcpy(best_buf + 1, tbuf, bsz);
        }
    }

    /* 3. try LZ77 */
    {
        uint8_t tbuf[64];
        uint32_t bsz = lz77_compress(chunk, tbuf);
        if (bsz > 0 && bsz + 1 < best_sz) {
            best_sz = bsz + 1;
            best_buf[0] = ENTROPY_LZ_TAG;
            memcpy(best_buf + 1, tbuf, bsz);
        }
    }

    /* 4. try LZ77 + Hilbert (4 offsets) — skip for medium entropy or if LZ77 failed */
    if (eclass == 0 && best_sz < 64) {
        uint8_t rebuf[64], hbuf[66];
        for (int off = 0; off < 4; off++) {
            _hilbert_reorder(rebuf, chunk, (uint8_t)off);
            uint32_t bsz = lz77_compress(rebuf, hbuf + 2);
            if (bsz > 0 && bsz + 2 < best_sz) {
                best_sz = bsz + 2;
                best_buf[0] = ENTROPY_LZ_HILBERT_TAG;
                best_buf[1] = (uint8_t)off;
                memcpy(best_buf + 2, hbuf + 2, bsz);
            }
        }
    }

    /* store */
    if (best_buf[0] == 0 && best_sz < 64) {
        /* sparse (no tag) */
        tick = tring_push(&df->tring, best_buf + 1, best_sz);
    } else if (best_sz < 64) {
        /* entropy: tag + data */
        tick = tring_push(&df->tring, best_buf, best_sz);
    } else {
        tick = tring_push(&df->tring, chunk, 64);
    }
    if (tick == UINT32_MAX) return SLOT_NULL;
    if (!df->x16 || local_idx < df->shell[n].slot_count) shell_set(&df->shell[n], local_idx);
    sidx_set(&df->sidx, gidx, tick);
    return gidx;
}

/* ── ENCODE WITH FLOW-AWARE ROUTING + CONFIDENCE + MEMORY ───────── */
/*
 * Replaces dfield_encode for streaming workloads. Features:
 *   - Flow memory: bias toward previous path when chunk hash is similar
 *   - Confidence-based: pick path with largest gap to second-best
 *   - Path locking: after FLOW_LOCK_STREAK same-path chunks, lock path
 *   - Window scoring: track path distribution over FLOW_WINDOW_MAX chunks
 */
static inline uint32_t dfield_encode_flow(DiamondField *df,
                                            const uint8_t  chunk[64],
                                            uint8_t       *out_level)
{
    FlowState *fs = &df->flow;
    uint64_t cur_seed = chunk_geometric_hash(chunk);

    /* Flow memory: check similarity to previous chunk */
    uint64_t hash_diff = cur_seed ^ fs->prev_seed;
    int similar = (hash_diff >> (64 - FLOW_HASH_SIMILAR)) == 0;

    /* If path is locked, skip trials and use locked path */
    if (fs->locked_path < 5) {
        uint8_t n = shell_classify_level(chunk);
        if (out_level) *out_level = n;
        uint16_t local_idx = chunk_to_slot_idx(chunk, n);
        uint32_t gidx      = slot_global(n, local_idx);
        while (n <= SHELL_MAX_LEVEL) {
            uint16_t cap_orig = df->shell[n].slot_count;
            if (!df->x16 && shell_full(&df->shell[n])) { n++; continue; }
            int found = 0;
            if (df->x16) {
                uint32_t base = (uint32_t)(_fnv64(chunk, 64) % cap_orig);
                uint8_t slope = _x16_slope(chunk);
                for (int s = 0; s < 16; s++) {
                    uint32_t eff = base * 16u + ((uint32_t)(slope + (uint8_t)s) & 0xFu);
                    uint32_t gp = slot_global(n, (uint16_t)eff);
                    if (sidx_get(&df->sidx, gp) == SLOT_NULL) { local_idx = (uint16_t)eff; gidx = gp; found = 1; break; }
                }
            } else {
                uint32_t cap = cap_orig;
                uint32_t probe = chunk_to_slot_idx(chunk, n);
                for (uint32_t i = 0; i < cap; i++) {
                    uint16_t idx = (uint16_t)probe;
                    int occupied = shell_get(&df->shell[n], idx) ||
                                   (sidx_get(&df->sidx, slot_global(n, idx)) != SLOT_NULL);
                    if (!occupied) { local_idx = idx; gidx = slot_global(n, idx); found = 1; break; }
                    probe = (probe + 1u) % cap;
                }
            }
            if (found) break;
            n++;
        }
        if (n > SHELL_MAX_LEVEL) return SLOT_NULL;

        uint8_t buf[68]; uint32_t sz = 0; uint32_t tick;
        switch (fs->locked_path) {
            case FLOW_PATH_SPARSE:
                sz = sparse_encode(buf + 1, chunk);
                if (sz > 0) { buf[0] = 0; } else { sz = 64; memcpy(buf, chunk, 64); }
                break;
            case FLOW_PATH_BITPACK: {
                uint8_t t[64]; uint32_t b = bitpack_encode(chunk, t);
                if (b > 0) { buf[0]=ENTROPY_BITPACK_TAG; memcpy(buf+1,t,b); sz=b+1; }
                else { sz=64; memcpy(buf,chunk,64); }
                break; }
            case FLOW_PATH_LZ77: {
                uint8_t t[64]; uint32_t b = lz77_compress(chunk, t);
                if (b > 0) { buf[0]=ENTROPY_LZ_TAG; memcpy(buf+1,t,b); sz=b+1; }
                else { sz=64; memcpy(buf,chunk,64); }
                break; }
            case FLOW_PATH_LZ_HILBERT: {
                /* Skip Hilbert for high-entropy data even if locked */
                if (_chunk_entropy_class(chunk) == 2) { sz=64; memcpy(buf,chunk,64); break; }
                uint8_t re[64], t[66]; uint32_t best=65; uint8_t boff=0;
                for (int o=0;o<4;o++) { _hilbert_reorder(re,chunk,(uint8_t)o);
                    uint32_t b=lz77_compress(re,t+2); if(b>0&&b+2<best){best=b+2;boff=(uint8_t)o;} }
                if (best<65) { buf[0]=ENTROPY_LZ_HILBERT_TAG; buf[1]=boff; memcpy(buf+2,t+2,best-2); sz=best; }
                else { sz=64; memcpy(buf,chunk,64); }
                break; }
            default: sz=64; memcpy(buf,chunk,64); break;
        }
        if (sz < 64 && buf[0] == 0) tick = tring_push(&df->tring, buf+1, sz);
        else if (sz < 64) tick = tring_push(&df->tring, buf, sz);
        else tick = tring_push(&df->tring, chunk, 64);
        if (tick == UINT32_MAX) return SLOT_NULL;
        if (!df->x16 || local_idx < df->shell[n].slot_count) shell_set(&df->shell[n], local_idx);
        sidx_set(&df->sidx, gidx, tick);
        fs->prev_seed = cur_seed;
        return gidx;
    }

    /* Normal path: try all methods, compute sizes */
    uint32_t sizes[5] = {65, 65, 65, 65, 64};
    uint8_t  bufs[5][68];

    int eclass = _chunk_entropy_class(chunk);

    if (eclass == 2) {
        /* High entropy: skip all trials, store raw */
        sizes[4] = 64;
        goto flow_pick_path;
    }

    /* 0. sparse */
    { uint32_t s = sparse_encode(bufs[0]+1, chunk);
      if (s > 0 && s < 64) { bufs[0][0]=0; sizes[0]=s; } }

    /* 1. bitpack */
    { uint8_t t[64]; uint32_t b = bitpack_encode(chunk, t);
      if (b > 0 && b+1 < 64) { bufs[1][0]=ENTROPY_BITPACK_TAG; memcpy(bufs[1]+1,t,b); sizes[1]=b+1; } }

    /* 2. LZ77 */
    { uint8_t t[64]; uint32_t b = lz77_compress(chunk, t);
      if (b > 0 && b+1 < 64) { bufs[2][0]=ENTROPY_LZ_TAG; memcpy(bufs[2]+1,t,b); sizes[2]=b+1; } }

    /* 3. LZ+Hilbert (best offset) — skip for medium entropy or if LZ77 failed */
    if (eclass == 0 && sizes[2] < 64) {
        uint8_t re[64], t[66]; uint32_t best=65; uint8_t boff=0;
        for (int o=0;o<4;o++) { _hilbert_reorder(re,chunk,(uint8_t)o);
            uint32_t b=lz77_compress(re,t+2); if(b>0&&b+2<best){best=b+2;boff=(uint8_t)o;} }
        if (best<65) { bufs[3][0]=ENTROPY_LZ_HILBERT_TAG; bufs[3][1]=boff; memcpy(bufs[3]+2,t+2,best-2); sizes[3]=best; } }

    /* 4. raw: sizes[4]=64, no buf needed */

flow_pick_path:

    /* Flow memory bias: boost similar path by 10% */
    if (similar && fs->prev_path < 5) {
        sizes[fs->prev_path] = sizes[fs->prev_path] * 9 / 10;
    }

    /* Confidence-based: pick path with largest gap to second-best */
    int best_path = 0, second_path = 1;
    for (int i = 1; i < 5; i++) {
        if (sizes[i] < sizes[best_path]) { second_path = best_path; best_path = i; }
        else if (sizes[i] < sizes[second_path]) second_path = i;
    }

    /* Update flow state */
    if (best_path == (int)fs->prev_path) {
        fs->path_streak++;
        if (fs->path_streak >= FLOW_LOCK_STREAK) fs->locked_path = (uint8_t)best_path;
    } else {
        fs->path_streak = 1;
        fs->locked_path = 5;
    }
    fs->window_path_scores[best_path]++;
    fs->window_pos++;
    if (fs->window_pos >= FLOW_WINDOW_MAX) {
        memset(fs->window_path_scores, 0, sizeof(fs->window_path_scores));
        fs->window_pos = 0;
    }
    fs->prev_path = (uint8_t)best_path;
    fs->prev_seed = cur_seed;

    /* Slot allocation (same as dfield_encode) */
    uint8_t n = shell_classify_level(chunk);
    if (out_level) *out_level = n;
    uint16_t local_idx = chunk_to_slot_idx(chunk, n);
    uint32_t gidx      = slot_global(n, local_idx);
    while (n <= SHELL_MAX_LEVEL) {
        uint16_t cap_orig = df->shell[n].slot_count;
        if (!df->x16 && shell_full(&df->shell[n])) { n++; continue; }
        int found = 0;
        if (df->x16) {
            uint32_t base = (uint32_t)(_fnv64(chunk, 64) % cap_orig);
            uint8_t slope = _x16_slope(chunk);
            for (int s = 0; s < 16; s++) {
                uint32_t eff = base * 16u + ((uint32_t)(slope + (uint8_t)s) & 0xFu);
                uint32_t gp = slot_global(n, (uint16_t)eff);
                if (sidx_get(&df->sidx, gp) == SLOT_NULL) { local_idx = (uint16_t)eff; gidx = gp; found = 1; break; }
            }
        } else {
            uint32_t cap = cap_orig;
            uint32_t probe = chunk_to_slot_idx(chunk, n);
            for (uint32_t i = 0; i < cap; i++) {
                uint16_t idx = (uint16_t)probe;
                int occupied = shell_get(&df->shell[n], idx) ||
                               (sidx_get(&df->sidx, slot_global(n, idx)) != SLOT_NULL);
                if (!occupied) { local_idx = idx; gidx = slot_global(n, idx); found = 1; break; }
                probe = (probe + 1u) % cap;
            }
        }
        if (found) break;
        n++;
    }
    if (n > SHELL_MAX_LEVEL) return SLOT_NULL;

    /* Store with best path */
    uint32_t tick;
    if (best_path == 4) {
        tick = tring_push(&df->tring, chunk, 64);
    } else if (sizes[best_path] < 64 && bufs[best_path][0] == 0) {
        tick = tring_push(&df->tring, bufs[best_path] + 1, sizes[best_path]);
    } else if (sizes[best_path] < 64) {
        tick = tring_push(&df->tring, bufs[best_path], sizes[best_path]);
    } else {
        tick = tring_push(&df->tring, chunk, 64);
    }
    if (tick == UINT32_MAX) return SLOT_NULL;
    if (!df->x16 || local_idx < df->shell[n].slot_count) shell_set(&df->shell[n], local_idx);
    sidx_set(&df->sidx, gidx, tick);
    return gidx;
}


/* ── Hilbert XOR-fold helpers for batch ───────────────────────────── */
/*
 * hilbert_d2xy: Hilbert distance → (x,y) for 8×8 grid (order 3)
 * Applied to 64B chunk treated as 8×8 spatial layout.
 */
/* Generalized Hilbert d2xy for any power-of-2 grid (n = 2..32) */
static inline void _batch_hilbert_d2xy_n(int n, int d, int *x, int *y) {
    int rx, ry, s, t = d;
    *x = *y = 0;
    for (s = 1; s < n; s *= 2) {
        rx = 1 & (t / 2); ry = 1 & (t ^ rx);
        if (ry == 0) {
            if (rx == 1) { *x = s-1 - *x; *y = s-1 - *y; }
            int tmp = *x; *x = *y; *y = tmp;
        }
        *x += s * rx; *y += s * ry; t /= 4;
    }
}

/* 8×8 grid (backward compat) */
static inline void _batch_hilbert_d2xy(int d, int *x, int *y) {
    _batch_hilbert_d2xy_n(8, d, x, y);
}

/*
 * _batch_hilbert_xorfold: XOR-fold K chunks using Hilbert offset scan
 *
 * For each of 4 offsets {0, K/4, K/2, 3K/4}:
 *   re-order chunk sequence by Hilbert curve with phase offset
 *   hash-fold first 8B of each chunk → 64-bit accumulator
 *   Uses FNV-mix to avoid XOR cancellation on symmetric data
 *
 * Returns: best_offset (highest popcount acc = most stable structure bits)
 * out_isect: fold result for best offset = batch geometric seed
 *
 * Cost: O(K × 4) — 4× cheaper than 6-rotation scan per chunk
 */
/* Find nearest power-of-2 grid dimension covering K cells */
static inline int _hilbert_grid_dim(uint32_t K) {
    int n = 1; while ((uint32_t)(n * n) < K) n *= 2;
    return n;
}

static inline uint8_t _batch_hilbert_xorfold(const uint8_t *chunks, uint32_t K,
                                               uint64_t *out_isect)
{
    int n = _hilbert_grid_dim(K);        /* grid size (power of 2) */
    uint32_t offsets[4] = {0, K/4, K/2, 3*K/4};
    uint64_t best_acc = 0; uint8_t best_off = 0; int best_pc = -1;

    for (int oi = 0; oi < 4; oi++) {
        uint64_t acc = 0x9e3779b97f4a7c15ULL;
        for (uint32_t pos = 0; pos < K; pos++) {
            uint32_t d = (pos + offsets[oi]) % K;
            int hx, hy;
            _batch_hilbert_d2xy_n(n, (int)d, &hx, &hy);
            uint32_t src_idx = (uint32_t)(hy * n + hx);
            if (src_idx >= K) src_idx = d;  /* off-grid fallback */
            uint64_t word;
            memcpy(&word, chunks + src_idx * 64, 8);
            acc ^= word;
            acc *= 1099511628211ULL;
            acc ^= acc >> 33;
        }
        int pc = __builtin_popcountll(acc);
        if (pc > best_pc) { best_pc = pc; best_acc = acc; best_off = (uint8_t)oi; }
    }
    if (out_isect) *out_isect = best_acc;
    return best_off;
}

/* ── rotation scan pruning: fast pass + borderline fallback ──────── */
#define ROT_FAST_PASS_ACCEPT 20   /* fast pass score >= this → accept immediately */
#define ROT_FAST_PASS_BORDER 12   /* fast pass score < this → low structure, skip full scan */

/* Fast pass: score a single rotation */
static inline int _batch_rotation_fast_score(const uint8_t *chunks, uint32_t K, uint8_t rot) {
    uint32_t stride = (K > 64) ? K / 64 : 1;
    uint8_t  rotbuf[64];
    uint64_t acc = 0;

    for (uint32_t i = 0; i < K; i += stride) {
        const uint8_t *c = chunks + i * 64;
        for (uint8_t z=0;z<4;z++) for (uint8_t y=0;y<4;y++) for (uint8_t x=0;x<4;x++) {
            uint8_t sx,sy,sz2;
            switch(rot%6){
                case 0:sx=x;sy=y;sz2=z;break; case 1:sx=y;sy=z;sz2=x;break;
                case 2:sx=z;sy=x;sz2=y;break; case 3:sx=x;sy=z;sz2=3-y;break;
                case 4:sx=z;sy=y;sz2=3-x;break; default:sx=3-y;sy=x;sz2=z;break;
            }
            rotbuf[z*16+y*4+x]=c[sz2*16+sy*4+sx];
        }
        uint64_t word; memcpy(&word, rotbuf, 8);
        acc ^= word;
    }
    return __builtin_popcountll(acc);
}

/* Full 6-rotation scan (used when fast pass is borderline) */
static inline uint8_t _batch_rotation_scan(const uint8_t *chunks, uint32_t K) {
    uint32_t stride = (K > 64) ? K / 64 : 1;
    uint8_t  rotbuf[64];
    uint8_t  best_rot = 0; int best_pc = -1;

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        uint64_t acc = 0;
        for (uint32_t i = 0; i < K; i += stride) {
            const uint8_t *c = chunks + i * 64;
            for (uint8_t z=0;z<4;z++) for (uint8_t y=0;y<4;y++) for (uint8_t x=0;x<4;x++) {
                uint8_t sx,sy,sz2;
                switch(rot%6){
                    case 0:sx=x;sy=y;sz2=z;break; case 1:sx=y;sy=z;sz2=x;break;
                    case 2:sx=z;sy=x;sz2=y;break; case 3:sx=x;sy=z;sz2=3-y;break;
                    case 4:sx=z;sy=y;sz2=3-x;break; default:sx=3-y;sy=x;sz2=z;break;
                }
                rotbuf[z*16+y*4+x]=c[sz2*16+sy*4+sx];
            }
            uint64_t word; memcpy(&word, rotbuf, 8);
            acc ^= word;
        }
        int pc = __builtin_popcountll(acc);
        if (pc > best_pc) { best_pc = pc; best_rot = rot; }
    }
    return best_rot;
}

/* Pruned rotation: fast pass (1 rot) → borderline → full scan (6 rot) */
static inline uint8_t _batch_rotation_pruned(const uint8_t *chunks, uint32_t K,
                                                uint64_t seed_pc_hint, uint8_t prev_rot) {
    (void)seed_pc_hint; /* replaced by fast-pass scoring */
    uint8_t fast_rot = (prev_rot < SHELL_ROT_STATES) ? prev_rot : 0;
    int fast_pc = _batch_rotation_fast_score(chunks, K, fast_rot);

    if (fast_pc >= ROT_FAST_PASS_ACCEPT) return fast_rot;  /* good enough */
    if (fast_pc < ROT_FAST_PASS_BORDER)  return fast_rot;  /* low structure, rot doesn't matter */
    return _batch_rotation_scan(chunks, K);                /* borderline → full scan */
}

/* ── Pre-LZ reshape: reorder chunks by Hilbert walk for compression boost ── */
/*
 * Reorders K chunks so that Hilbert-walked slots come first (in walk order),
 * invert slots come last. This improves LZ77 locality by placing similar
 * chunks adjacent to each other.
 *
 * out_perm[i] = original index of chunk that should go at position i.
 * Caller reorders chunks using this permutation before encoding.
 */
static inline void _batch_reshape_for_lz(uint32_t out_perm[], uint32_t K, uint8_t n) {
    uint32_t slots = shell_slots(n);
    uint64_t invert[78] = {0};
    shell_hilbert_invert(invert, n);

    uint32_t walk_pos = 0, invert_pos = K;
    for (uint32_t i = 0; i < K; i++) {
        uint32_t slot = i % slots;
        int is_invert = (invert[slot >> 6] >> (slot & 63)) & 1;
        if (is_invert) {
            out_perm[--invert_pos] = i;
        } else {
            out_perm[walk_pos++] = i;
        }
    }
}

/* ── ENCODE BATCH (adaptive bitmask diff + Hilbert XOR-fold + rotation) */
/*
 * v4 additions vs v3:
 *   - Hilbert XOR-fold: compute batch geometric seed (8B) from K chunks
 *   - Rotation scan: find best_rot for batch (not hardcoded 0)
 *   - flags byte: bit0=hilbert_seed_valid (structure_ratio >= threshold)
 *
 * Wire format (variable size):
 *   [layer:1B][best_rot:1B][count:1B][flags:1B] = 4B header  (unchanged)
 *   [hilbert_seed:8B]                             = batch XOR-fold seed (NEW)
 *   [base_chunk:64B]                              = base (full 64B)
 *   [mask0:8B][vals0:popcnt(mask0)*1B]            = diff for chunk 0
 *   ...
 *
 * flags bit0=1: hilbert_seed present and valid (decoder can use as quick hash)
 * flags bit1=0: reserved
 *
 * Adaptive: up to BATCH_MAX_DIFF (12) differing bytes per chunk.
 * If any chunk exceeds BATCH_MAX_DIFF, returns 0 (don't batch).
 *
 * Returns number of chunks encoded, or 0 on error/fallback.
 */
static inline uint32_t dfield_encode_batch(DiamondField *df,
                                              const uint8_t *chunks,  /* K × 64B */
                                              uint32_t       K,
                                              uint8_t        layer,   /* 0=L1,1=L2,2=L3 */
                                              uint32_t       gidxs_out[])
{
    if (K == 0 || K > BATCH_L3) return 0;
    uint32_t cap = layer == 0 ? BATCH_L1 : layer == 1 ? BATCH_L2 : BATCH_L3;
    if (K > cap) K = cap;

    /* Early exit: high-entropy data won't batch (diffs > BATCH_MAX_DIFF) */
    if (_chunk_entropy_class(chunks) == 2) return 0;

    /* 0. Pre-LZ reshape: reorder chunks by Hilbert walk for compression boost */
    uint8_t n_class = shell_classify_level(chunks);
    const uint8_t *work = chunks;
    uint32_t perm[512];
    uint8_t reordered[512 * 64];
    _batch_reshape_for_lz(perm, K, n_class);
    for (uint32_t i = 0; i < K; i++)
        memcpy(reordered + i * 64, chunks + perm[i] * 64, 64);
    work = reordered;

    /* 1. Hilbert XOR-fold → batch geometric seed + best_offset */
    uint64_t hilbert_seed = 0;
    _batch_hilbert_xorfold(work, K, &hilbert_seed);
    int seed_pc = __builtin_popcountll(hilbert_seed);
    /* structure_valid: seed has enough stable bits (>= 8 of 64) */
    uint8_t seed_valid = (seed_pc >= 8) ? 1u : 0u;

    /* 2. Rotation scan → best_rot for batch (pruned if seed stable) */
    uint8_t best_rot = _batch_rotation_pruned(work, K, (uint64_t)seed_pc, df->batch_prev_rot);

    /* 3. classify level from first chunk (representative) */
    uint8_t n = n_class;

    /* 4. compute base + pre-encode all diffs */
    uint8_t base[64];
    batch_compute_base(base, work, K, 64);

    uint8_t diff_cache[512][12];  /* max 512 × (8+12) = 10KB */
    uint32_t diff_sizes[512];
    uint32_t total_diff_size = 0;
    int all_pass = 1;
    for (uint32_t i = 0; i < K; i++) {
        diff_sizes[i] = batch_encode_diff(diff_cache[i], base, work + i * 64);
        if (diff_sizes[i] == 0) { all_pass = 0; break; }
        total_diff_size += diff_sizes[i];
    }
    if (!all_pass) return 0;  /* fallback: don't batch */

    /* 5. build wire format (seed conditional on flags) */
    uint32_t hdr_sz = 4 + (seed_valid ? 8 : 0); /* header + optional seed */
    uint32_t wire_size = hdr_sz + 64 + total_diff_size;
    uint8_t wire[4 + 8 + 64 + 512 * (8 + BATCH_MAX_DIFF)]; /* max L3 */
    wire[0] = (uint8_t)layer;
    wire[1] = best_rot;
    df->batch_prev_rot = best_rot;
    wire[2] = (uint8_t)K;
    wire[3] = seed_valid;        /* bit0=1 → seed present after header */
    if (seed_valid)
        memcpy(wire + 4, &hilbert_seed, 8);
    memcpy(wire + hdr_sz, base, 64); /* base at hdr_sz */
    uint32_t woff = hdr_sz + 64;
    for (uint32_t i = 0; i < K; i++) {
        memcpy(wire + woff, diff_cache[i], diff_sizes[i]);
        woff += diff_sizes[i];
    }

    uint32_t batch_tick = tring_push(&df->tring, wire, wire_size);
    if (batch_tick == UINT32_MAX) return 0;

    /* 6. for each chunk: map + probe slot + set shell + sidx */
    uint32_t encoded = 0;
    for (uint32_t i = 0; i < K; i++) {
        const uint8_t *chunk = work + i * 64;
        uint8_t probe_n = n;
        uint16_t local_idx;
        uint32_t gidx;
        while (probe_n <= SHELL_MAX_LEVEL) {
            uint16_t cap_orig = df->shell[probe_n].slot_count;
            if (!df->x16 && shell_full(&df->shell[probe_n])) { probe_n++; continue; }
            int found = 0;
            if (df->x16) {
                uint32_t base = (uint32_t)(_fnv64(chunk, 64) % cap_orig);
                uint8_t slope = _x16_slope(chunk);
                for (int s = 0; s < 16; s++) {
                    uint32_t eff = base * 16u + ((uint32_t)(slope + (uint8_t)s) & 0xFu);
                    uint32_t gp = slot_global(probe_n, (uint16_t)eff);
                    if (sidx_get(&df->sidx, gp) == SLOT_NULL) { local_idx = (uint16_t)eff; gidx = gp; found = 1; break; }
                }
            } else {
                uint32_t cap = cap_orig;
                uint32_t probe = chunk_to_slot_idx(chunk, probe_n);
                for (uint32_t j = 0; j < cap; j++) {
                    uint16_t idx = (uint16_t)probe;
                    int occupied = shell_get(&df->shell[probe_n], idx) ||
                                   (sidx_get(&df->sidx, slot_global(probe_n, idx)) != SLOT_NULL);
                    if (!occupied) { local_idx = idx; gidx = slot_global(probe_n, idx); found = 1; break; }
                    probe = (probe + 1u) % cap;
                }
            }
            if (found) break;
            probe_n++;
        }
        if (probe_n > SHELL_MAX_LEVEL) break;

        uint32_t packed = sidx_pack_batch(batch_tick, (uint8_t)i);
        if (!df->x16 || local_idx < df->shell[probe_n].slot_count) shell_set(&df->shell[probe_n], local_idx);
        sidx_set(&df->sidx, gidx, packed);
        gidxs_out[encoded++] = gidx;
    }
    return encoded;
}

/* ── DECODE (single, handles batch and non-batch) ──────────────────── */
static inline int dfield_decode(const DiamondField *df,
                                 uint32_t            gidx,
                                 uint8_t             out[64])
{
    uint8_t  n   = global_level(gidx);
    uint16_t idx = global_local(gidx);
    if (n > SHELL_MAX_LEVEL) return -1;
    if (idx < df->shell[n].slot_count && !shell_get(&df->shell[n], idx)) return -1;

    uint32_t sidx_val = sidx_get(&df->sidx, gidx);
    if (sidx_val == SLOT_NULL) return -1;

    if (!sidx_is_batch(sidx_val)) {
        /* non-batch: direct tick */
        uint32_t stored_size;
        const uint8_t *data = tring_read(&df->tring, sidx_val, &stored_size);
        if (!data) return -1;
        if (stored_size < 64) {
            if (stored_size > 0 && data[0] == ENTROPY_LZ_HILBERT_TAG) {
                uint8_t tmp[64];
                uint8_t off = (stored_size > 1) ? data[1] : 0;
                lz77_decompress(data + 2, stored_size - 2, tmp);
                _hilbert_unreorder(out, tmp, off);
            } else if (stored_size > 0 && data[0] == ENTROPY_LZ_TAG)
                lz77_decompress(data + 1, stored_size - 1, out);
            else if (stored_size > 0 && data[0] == ENTROPY_BITPACK_TAG)
                bitpack_decode(data + 1, stored_size - 1, out);
            else if (stored_size == DENSE_NODE_SIZE)
                dense_decode(out, data);
            else
                sparse_decode(out, data, stored_size);
        } else memcpy(out, data, 64);
        return 0;
    }

    /* batch: base offset depends on flags (conditional seed) */
    uint32_t batch_tick = sidx_batch_tick(sidx_val);
    uint8_t  batch_pos  = sidx_batch_pos(sidx_val);
    uint32_t sz;
    const uint8_t *batch_data = tring_read(&df->tring, batch_tick, &sz);
    if (!batch_data) return -1;

    uint8_t flags = batch_data[3];
    uint32_t base_off = 4 + ((flags & 1) ? 8 : 0);
    uint32_t doff = batch_diff_offset(batch_data, batch_pos);
    if (doff + 8 > sz) return -1;
    batch_decode_diff(out, batch_data + base_off, batch_data + doff);
    return 0;
}

/* ── DELETE O(1) ────────────────────────────────────────────────────── */
static inline void dfield_delete(DiamondField *df, uint32_t gidx) {
    uint8_t n   = global_level(gidx);
    uint16_t idx = global_local(gidx);
    if (n > SHELL_MAX_LEVEL) return;
    shell_clr(&df->shell[n], idx);
}

/* ── GC callback ──────────────────────────────────────────────────── */
static inline int _dfield_tick_referenced(uint32_t tick, void *ctx) {
    DiamondField *df = (DiamondField *)ctx;
    for (uint32_t gidx = 0; gidx < INDEX_SIZE; gidx++) {
        uint32_t sidx_val = sidx_get(&df->sidx, gidx);
        if (sidx_val == SLOT_NULL) continue;
        uint32_t entry_tick = sidx_is_batch(sidx_val) ? sidx_batch_tick(sidx_val) : sidx_val;
        if (entry_tick != tick) continue;
        uint8_t  n   = global_level(gidx);
        uint16_t idx = global_local(gidx);
        if (n <= SHELL_MAX_LEVEL && (idx >= df->shell[n].slot_count || shell_get(&df->shell[n], idx)))
            return 1;
    }
    return 0;
}

static inline uint32_t dfield_gc(DiamondField *df) {
    return tring_gc_scan(&df->tring, _dfield_tick_referenced, df);
}

/* ── Hilbert walk → invert mask for atomic reshape ───────────────── */
/*
 * Adaptive walk: G = ceil(slots/360), L = 3, S = 120
 * Produces bitmask: invert[i] = 1 → slot i is NOT covered by walk
 * mask must have at least (slots+63)/64 elements.
 */
#define WALK_LANES 3u
#define WALK_STEPS 120u

static inline void shell_hilbert_invert(uint64_t *invert, uint8_t n) {
    uint32_t slots = shell_slots(n);
    uint32_t words = (slots + 63) / 64;
    memset(invert, 0, words * 8);
    /* walk all slots — any slot hit is NOT invert */
    int G = (int)((slots + WALK_LANES * WALK_STEPS - 1) / (WALK_LANES * WALK_STEPS));
    if (G < 1) G = 1;
    int gd = 1; while (gd * gd < (int)WALK_STEPS) gd *= 2;
    if (gd < 2) gd = 2;

    for (int g = 0; g < G; g++) {
        for (uint32_t l = 0; l < WALK_LANES; l++) {
            uint32_t base = (uint32_t)g * (WALK_LANES * WALK_STEPS) + l * WALK_STEPS;
            for (uint32_t step = 0; step < WALK_STEPS; step++) {
                int hx, hy;
                _batch_hilbert_d2xy_n(gd, (int)(step % (uint32_t)(gd * gd)), &hx, &hy);
                uint32_t local = (uint32_t)(hy * gd + hx) % WALK_STEPS;
                if (step >= (uint32_t)(gd * gd))
                    local = (local ^ (uint32_t)gd) % WALK_STEPS;
                uint32_t slot = base + local;
                if (slot < slots) {
                    invert[slot >> 6] |= (1ULL << (slot & 63));
                }
            }
        }
    }
    /* now invert mask = NOT walked: complement within slot range */
    for (uint32_t w = 0; w < words; w++) invert[w] = ~invert[w];
    /* clear bits beyond slot count */
    if (slots & 63) invert[words - 1] &= ((1ULL << (slots & 63)) - 1);
}

/* ── RESHAPE (atomic: only remap invert slots ~7%, walked preserves idx) ─ */
static inline uint32_t dfield_reshape(DiamondField *df, uint8_t n_old, uint8_t n_new) {
    if (n_old > SHELL_MAX_LEVEL || n_new > SHELL_MAX_LEVEL) return 0;
    uint16_t cap_old = df->shell[n_old].slot_count;
    uint16_t cap_new = df->shell[n_new].slot_count;
    uint32_t remapped = 0;

    /* build invert mask: only these slots need data remap */
    uint64_t invert[78] = {0};  /* max 4913 slots → 77 words */
    shell_hilbert_invert(invert, n_old);

    for (uint16_t idx = 0; idx < cap_old; idx++) {
        if (!shell_get(&df->shell[n_old], idx)) continue;
        uint32_t gidx_old = slot_global(n_old, idx);
        uint32_t sidx_val = sidx_get(&df->sidx, gidx_old);
        if (sidx_val == SLOT_NULL) continue;

        uint16_t new_idx;
        int is_invert = (invert[idx >> 6] >> (idx & 63)) & 1;

        if (is_invert) {
            /* invert slot: need full remap (read chunk → re-hash → probe) */
            uint32_t tick;
            uint8_t  batch_pos;
            int      is_batch;
            if (sidx_is_batch(sidx_val)) {
                tick = sidx_batch_tick(sidx_val);
                batch_pos = sidx_batch_pos(sidx_val);
                is_batch = 1;
            } else { tick = sidx_val; batch_pos = 0; is_batch = 0; }

            uint8_t chunk[64];
            if (is_batch) {
                uint32_t sz; const uint8_t *bd = tring_read(&df->tring, tick, &sz);
                if (!bd) continue;
                uint32_t doff = batch_diff_offset(bd, batch_pos);
                if (doff + 8 > sz) continue;
                uint8_t bf = bd[3]; uint32_t bo = 4 + ((bf & 1) ? 8 : 0);
                batch_decode_diff(chunk, bd + bo, bd + doff);
            } else {
                uint32_t sz; const uint8_t *raw = tring_read(&df->tring, tick, &sz);
                if (!raw) continue;
                if (sz < 64) {
                    if (sz > 0 && raw[0] == ENTROPY_LZ_HILBERT_TAG) {
                        uint8_t tmp[64]; uint8_t off = (sz>1)?raw[1]:0;
                        lz77_decompress(raw+2, sz-2, tmp);
                        _hilbert_unreorder(chunk, tmp, off);
                    } else if (sz > 0 && raw[0] == ENTROPY_LZ_TAG)
                        lz77_decompress(raw+1, sz-1, chunk);
                    else if (sz > 0 && raw[0] == ENTROPY_BITPACK_TAG)
                        bitpack_decode(raw+1, sz-1, chunk);
                    else if (sz == DENSE_NODE_SIZE) dense_decode(chunk, raw);
                    else sparse_decode(chunk, raw, sz);
                } else memcpy(chunk, raw, 64);
            }
            /* re-probe in new shell */
            new_idx = chunk_to_slot_idx(chunk, n_new);
            for (uint16_t p = 0; p < cap_new; p++) {
                if (!shell_get(&df->shell[n_new], new_idx)) break;
                new_idx = (uint16_t)((new_idx + 1u) % cap_new);
            }
        } else {
            /* walked slot: preserve same idx in new shell */
            new_idx = idx;
            if (new_idx >= cap_new) continue; /* should not happen */
        }

        uint32_t gidx_new = slot_global(n_new, new_idx);
        shell_clr(&df->shell[n_old], idx);
        sidx_clear(&df->sidx, gidx_old);
        shell_set(&df->shell[n_new], new_idx);
        sidx_set(&df->sidx, gidx_new, sidx_val);
        remapped++;
    }
    return remapped;
}

/* ── Per-chunk geometric hash — proxy for batch seed ────────────── */
/*
 * Uses fold_fibo_intersect (geometric structure stability) + FNV-mix
 * to produce a repeatable geometric fingerprint per chunk.
 * Chunks with same geometric_hash share structure → batch well together.
 */
static inline uint64_t chunk_geometric_hash(const uint8_t chunk[64]) {
    const DiamondBlock *b = (const DiamondBlock *)chunk;
    uint64_t isect = fold_fibo_intersect(b);
    isect ^= isect >> 33;
    isect *= 1099511628211ULL;
    return isect;
}

/* ── Entropy encoding: LZ77 + bitpack (self-contained, no external deps) ─ */
/* ── Fingerprint — seed-based grouping key ──────────────────────── */
/*
 * Groups chunks by (geometric_hash ^ coarse_rot_class).
 * Chunks with similar geometric structure (same seed proxy) and
 * similar rotation preference land in same bucket → batch succeeds.
 */
static inline uint8_t shell_fingerprint(const uint8_t chunk[64], uint8_t num_buckets) {
    uint64_t gh = chunk_geometric_hash(chunk);
    uint8_t rot_class = (uint8_t)((gh >> 40) & 3); /* coarse rotation class 0-3 */
    return (uint8_t)((gh ^ (gh >> 16) ^ rot_class) % num_buckets);
}

/* ── ENCODE WINDOWED — group by fingerprint, batch per group ────── */
static inline uint32_t dfield_encode_windowed(DiamondField *df,
                                               const uint8_t *chunks,
                                               uint32_t       window_size,
                                               uint8_t        num_buckets,
                                               uint32_t       gidxs_out[])
{
    if (window_size == 0) return 0;
    if (window_size > 256) window_size = 256;
    if (num_buckets == 0) num_buckets = 8;

    /* Fast path: if first chunk is high-entropy, skip grouping entirely */
    if (_chunk_entropy_class(chunks) == 2) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < window_size; i++) {
            uint32_t g = dfield_encode(df, chunks + i * 64, NULL);
            if (g != SLOT_NULL) gidxs_out[total++] = g;
        }
        return total;
    }

    uint8_t fps[256];
    for (uint32_t i = 0; i < window_size; i++)
        fps[i] = shell_fingerprint(chunks + i * 64, num_buckets);

    uint8_t  bucket_fp[256]; uint32_t bucket_cnt[256] = {0};
    uint32_t num_buckets_used = 0;
    for (uint32_t i = 0; i < window_size; i++) {
        uint8_t fp = fps[i];
        uint32_t b;
        for (b = 0; b < num_buckets_used; b++) if (bucket_fp[b] == fp) break;
        if (b == num_buckets_used) { if (b >= 256) break; bucket_fp[b] = fp; num_buckets_used++; }
        bucket_cnt[b]++;
    }

    uint32_t bucket_chunks[256][256] = {{0}};
    uint32_t bucket_idx[256] = {0};
    for (uint32_t i = 0; i < window_size; i++) {
        uint8_t fp = fps[i];
        uint32_t b;
        for (b = 0; b < num_buckets_used; b++) if (bucket_fp[b] == fp) break;
        bucket_chunks[b][bucket_idx[b]++] = i;
    }

    /* encode each bucket, map results back to original position */
    uint32_t gidx_map[256];
    memset(gidx_map, 0xFF, sizeof(gidx_map)); /* SLOT_NULL fill */
    for (uint32_t b = 0; b < num_buckets_used; b++) {
        uint32_t cnt = bucket_cnt[b];
        if (cnt < 2) {
            for (uint32_t j = 0; j < cnt; j++) {
                uint32_t ci = bucket_chunks[b][j];
                gidx_map[ci] = dfield_encode(df, chunks + ci * 64, NULL);
            }
        } else {
            uint8_t temp[256 * 64];
            for (uint32_t j = 0; j < cnt; j++) {
                uint32_t ci = bucket_chunks[b][j];
                memcpy(temp + j * 64, chunks + ci * 64, 64);
            }
            uint32_t gb[256];
            uint8_t bl = cnt >= 64 ? 1 : (cnt >= 8 ? 0 : 0);
            uint32_t e = dfield_encode_batch(df, temp, cnt, bl, gb);
            if (e > 0) {
                for (uint32_t j = 0; j < e; j++) {
                    uint32_t ci = bucket_chunks[b][j];
                    gidx_map[ci] = gb[j];
                }
                for (uint32_t j = e; j < cnt; j++) {
                    uint32_t ci = bucket_chunks[b][j];
                    gidx_map[ci] = dfield_encode(df, chunks + ci * 64, NULL);
                }
            } else {
                for (uint32_t j = 0; j < cnt; j++) {
                    uint32_t ci = bucket_chunks[b][j];
                    gidx_map[ci] = dfield_encode(df, chunks + ci * 64, NULL);
                }
            }
        }
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < window_size; i++)
        if (gidx_map[i] != SLOT_NULL)
            gidxs_out[total++] = gidx_map[i];
    return total;
}

/* ── FLAT STREAMING MODE — bypass shell/slot for bulk data ────── */
/*
 * For large files (2GB+), shell indexing is impractical (~31M slots needed).
 * Flat mode encodes each chunk adaptively and stores sequentially in tring.
 * No dedup, no geometric indexing — just sequential compressed storage.
 *
 * Returns: tick (sequential chunk number), UINT32_MAX on failure.
 * Decode: use dfield_decode_flat() with the returned tick.
 */
static inline uint32_t dfield_encode_flat(DiamondField *df,
                                            const uint8_t chunk[64])
{
    int eclass = _chunk_entropy_class(chunk);

    /* Fast path: high-entropy → raw store */
    if (eclass == 2)
        return tring_push(&df->tring, chunk, 64);

    /* Adaptive: try all methods, pick smallest */
    uint8_t best_buf[68];
    uint32_t best_sz = 64;

    /* sparse */
    { uint8_t t[66]; uint32_t s = sparse_encode(t, chunk);
      if (s > 0 && s < best_sz) { best_sz = s; best_buf[0] = 0; memcpy(best_buf+1, t, s); } }

    /* bitpack */
    { uint8_t t[64]; uint32_t b = bitpack_encode(chunk, t);
      if (b > 0 && b+1 < best_sz) { best_sz = b+1; best_buf[0] = ENTROPY_BITPACK_TAG; memcpy(best_buf+1, t, b); } }

    /* LZ77 */
    { uint8_t t[64]; uint32_t b = lz77_compress(chunk, t);
      if (b > 0 && b+1 < best_sz) { best_sz = b+1; best_buf[0] = ENTROPY_LZ_TAG; memcpy(best_buf+1, t, b); } }

    /* LZ+Hilbert — only if LZ77 compressed */
    if (eclass == 0 && best_sz < 64) {
        uint8_t re[64], t[66];
        for (int o=0;o<4;o++) { _hilbert_reorder(re,chunk,(uint8_t)o);
            uint32_t b=lz77_compress(re,t+2); if(b>0&&b+2<best_sz){best_sz=b+2;best_buf[0]=ENTROPY_LZ_HILBERT_TAG;best_buf[1]=(uint8_t)o;memcpy(best_buf+2,t+2,b);} }
    }

    /* Store */
    if (best_buf[0] == 0 && best_sz < 64)
        return tring_push(&df->tring, best_buf+1, best_sz);
    if (best_sz < 64)
        return tring_push(&df->tring, best_buf, best_sz);
    return tring_push(&df->tring, chunk, 64);
}

/* Decode a flat-mode chunk by tick (shell/sidx NOT required) */
static inline int dfield_decode_flat(const DiamondField *df,
                                       uint32_t tick,
                                       uint8_t out[64])
{
    uint32_t sz;
    const uint8_t *data = tring_read(&df->tring, tick, &sz);
    if (!data) return -1;

    if (sz >= 64) { memcpy(out, data, 64); return 0; }
    if (sz == 0) return -1;

    if (data[0] == ENTROPY_LZ_HILBERT_TAG && sz >= 2) {
        uint8_t tmp[64]; uint8_t off = data[1];
        if (lz77_decompress(data+2, sz-2, tmp) < 64) return -1;
        _hilbert_unreorder(out, tmp, off); return 0;
    }
    if (data[0] == ENTROPY_LZ_TAG) {
        if (lz77_decompress(data+1, sz-1, out) < 64) return -1; return 0;
    }
    if (data[0] == ENTROPY_BITPACK_TAG) {
        if (bitpack_decode(data+1, sz-1, out) < 64) return -1; return 0;
    }
    if (sz == DENSE_NODE_SIZE) { dense_decode(out, data); return 0; }
    sparse_decode(out, data, sz);
    return 0;
}

#endif /* GEO_DIAMOND_FIELD_H */
