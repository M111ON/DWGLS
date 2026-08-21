/*
 * diamond_shell_v2.h — 3D Shell + real fold_fibo_intersect discriminator
 * ════════════════════════════════════════════════════════════════════════
 *
 * v1 → v2 changes:
 *   rotation discriminator: entropy proxy → fold_fibo_intersect() popcount
 *   classification:         entropy threshold → fibo_intersect popcount
 *   batch rotation:         entropy sum → fibo_intersect XOR accumulation
 *
 * Why fibo_intersect as discriminator:
 *   fold_fibo_intersect = AND of 4 rotated copies of chunk data
 *   bits that survive = geometric constants of THIS data
 *   high popcount = data has strong geometric structure → better alignment
 *   low popcount  = data is noisy/flat → rotation doesn't help much
 *
 *   rotation scan: apply each of 6 3D orientations to chunk,
 *   build DiamondBlock per orientation, pick highest fibo_intersect
 *   popcount = most geometrically aligned orientation
 *
 * Encode flags (same as v1, compatible):
 *   SHELL_FLAG_FLAT   (0): 2B  — fibo_isect == 0 (no structure)
 *   SHELL_FLAG_SPARSE (1): 9B  — fibo_isect popcount <= SPARSE_THRESH
 *   SHELL_FLAG_DENSE  (2): 17B — fibo_isect popcount > SPARSE_THRESH
 *   SHELL_FLAG_BATCH  (3): 5B  — belongs to batch
 * ════════════════════════════════════════════════════════════════════════
 */

#ifndef DIAMOND_SHELL_V2_H
#define DIAMOND_SHELL_V2_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* pogls_fold.h provides: DiamondBlock, fold_block_init,
 * fold_build_quad_mirror, fold_fibo_intersect, fold_xor_audit */
#include "pogls_fold.h"

/* ── constants ─────────────────────────────────────────── */

#define SHELL_CHUNK_SZ      64u
#define SHELL_ROT_STATES     6u

#define SHELL_L0_CHUNKS      1u
#define SHELL_L1_CHUNKS      8u
#define SHELL_L2_CHUNKS     64u
#define SHELL_L3_CHUNKS    512u

#define SHELL_FIBO_SIG      17u
#define SHELL_FIBO_FLUSH   144u
#define SHELL_FIBO_SNAP    720u

/*
 * SPARSE_THRESH: fibo_intersect popcount
 * <= this → sparse (few geometric constants → encode as seed)
 *  > this → dense  (many geometric constants → store raw diff)
 *  == 0   → flat   (no structure at all → skip)
 */
#define SHELL_SPARSE_THRESH  4u

#define SHELL_FLAG_FLAT      0u
#define SHELL_FLAG_SPARSE    1u
#define SHELL_FLAG_DENSE     2u
#define SHELL_FLAG_BATCH     3u

/* ── types ─────────────────────────────────────────────── */

typedef struct {
    uint8_t  flag;
    uint8_t  layer;
    uint8_t  best_rot;
    uint8_t  fibo_phase;
    uint64_t fibo_isect;    /* fold_fibo_intersect of best-rotation block */
    uint8_t  isect_pc;      /* popcount(fibo_isect) */
    uint64_t diff_a;        /* first 8B of rotated chunk (XOR diff) */
    uint64_t diff_b;        /* next  8B */
    uint64_t seed;          /* FNV hash for sparse */
    uint32_t batch_id;
    uint32_t chunk_z;
} ShellChunkResult;

typedef struct {
    uint8_t  layer;
    uint32_t capacity;
    uint32_t count;
    uint32_t batch_id;
    uint8_t  best_rot;
    uint64_t batch_isect;   /* XOR of all fibo_isect in batch */
    uint8_t *cube;
} ShellBatch;

typedef struct {
    uint64_t n_chunks;
    uint64_t n_flat;
    uint64_t n_sparse;
    uint64_t n_dense;
    uint64_t n_batch;
    uint64_t enc_bytes;
    uint64_t raw_bytes;
    uint64_t rot_wins[SHELL_ROT_STATES];
    uint64_t isect_total;   /* sum of all fibo_isect popcounts */
    double   ratio;
} ShellMetrics;

/* ── internal helpers ───────────────────────────────────── */

static inline uint64_t _shell_fnv64(const uint8_t *d, int n)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

static inline uint8_t _shell_fibo_phase(uint32_t z)
{
    if (z > 0 && z % SHELL_FIBO_SNAP  == 0) return 3;
    if (z > 0 && z % SHELL_FIBO_FLUSH == 0) return 2;
    if (z > 0 && z % SHELL_FIBO_SIG   == 0) return 1;
    return 0;
}

/*
 * _shell_rotate64: re-index 64B chunk as 4×4×4 cube
 * 6 orientations = 6 face-forward rotations
 */
static inline void _shell_rotate64(uint8_t out[64],
                                    const uint8_t in[64],
                                    uint8_t rot)
{
    for (uint8_t z = 0; z < 4; z++) {
        for (uint8_t y = 0; y < 4; y++) {
            for (uint8_t x = 0; x < 4; x++) {
                uint8_t sx, sy, sz;
                switch (rot % SHELL_ROT_STATES) {
                    case 0: sx=x;   sy=y;   sz=z;   break;
                    case 1: sx=y;   sy=z;   sz=x;   break;
                    case 2: sx=z;   sy=x;   sz=y;   break;
                    case 3: sx=x;   sy=z;   sz=3-y; break;
                    case 4: sx=z;   sy=y;   sz=3-x; break;
                    case 5: sx=3-y; sy=x;   sz=z;   break;
                    default: sx=x; sy=y; sz=z; break;
                }
                out[z*16 + y*4 + x] = in[sz*16 + sy*4 + sx];
            }
        }
    }
}

/*
 * _shell_chunk_to_block: load 64B rotated chunk into DiamondBlock
 *
 * Mapping:
 *   chunk[0..7]  → core.raw         (primary 8B)
 *   core.invert  = ~core.raw        (XOR invariant)
 *   quad_mirror  = fold_build_quad_mirror() from core.raw
 *   chunk[8..63] → ignored for fibo_intersect (quad_mirror uses core only)
 *
 * face_id  = rot (0..5 maps to 6 frustum faces)
 * engine_id = chunk_z & 0x7F (timeline position, low 7 bits)
 * vector_pos = FNV of chunk bytes 8..11 (spatial hash)
 * fibo_gear = 1 (G1 direct — single chunk mode)
 */
static inline DiamondBlock _shell_chunk_to_block(const uint8_t rotbuf[64],
                                                   uint8_t  rot,
                                                   uint32_t chunk_z)
{
    DiamondBlock db;
    memset(&db, 0, sizeof(db));

    /* load first 8B as core */
    memcpy(&db.core.raw, rotbuf, 8);
    db.invert = ~db.core.raw;

    /* override core fields to embed geometry context */
    uint8_t face_id    = rot & 0x1F;
    uint8_t engine_id  = (uint8_t)(chunk_z & 0x7F);
    uint32_t vpos      = (uint32_t)_shell_fnv64(rotbuf + 8, 4) & 0xFFFFFF;
    uint8_t fibo_gear  = 1u;
    uint8_t quad_flags = (uint8_t)(rotbuf[16] ^ rotbuf[32]); /* spatial XOR */

    /* rebuild core with geometry context embedded */
    db.core.raw = ((uint64_t)(face_id   & 0x1F) << 59)
                | ((uint64_t)(engine_id & 0x7F) << 52)
                | ((uint64_t)(vpos & 0xFFFFFF)  << 28)
                | ((uint64_t)(fibo_gear  & 0x0F)<< 24)
                | ((uint64_t)(quad_flags & 0xFF)<< 16)
                | (db.core.raw & 0xFFFF); /* keep low 16b as data residue */
    db.invert = ~db.core.raw;

    fold_build_quad_mirror(&db);
    return db;
}

/*
 * _chunk_to_pure_block: content-only DiamondBlock from first 8B
 * NO geometry metadata override — pure fold_fibo_intersect on real data
 * This fixes Bug 1 from HANDOFF: metadata was polluting 75% of core.raw
 * (only 16-bit data residue survived → fibo_intersect was meaningless)
 */
static inline DiamondBlock _chunk_to_pure_block(const uint8_t chunk[8])
{
    DiamondBlock db;
    memset(&db, 0, sizeof(db));
    memcpy(&db.core.raw, chunk, 8);
    db.invert = ~db.core.raw;
    fold_build_quad_mirror(&db);
    return db;
}

/* ══════════════════════════════════════════════════════════
 * shell_classify_chunk_v2()
 *
 * v2: uses fold_fibo_intersect as rotation discriminator
 *     + pure-content block for accurate content analysis
 *
 * For each of 6 rotations:
 *   1. rotate chunk bytes (3D re-index)
 *   2. build PURE DiamondBlock from first 8B (no metadata override)
 *   3. compute fold_fibo_intersect → popcount
 *   4. pick rotation with HIGHEST popcount
 *      (= most geometric structure preserved = best codec alignment)
 *
 * Classify by isect popcount (PURE content, not metadata-contaminated):
 *   == 0              → FLAT   (2B encode)
 *   1..SPARSE_THRESH  → SPARSE (10B: seed)
 *   > SPARSE_THRESH   → DENSE  (66B: full rotated chunk)
 * ══════════════════════════════════════════════════════════ */
static inline ShellChunkResult shell_classify_chunk_v2(
    const uint8_t *chunk,
    uint32_t       chunk_z,
    uint32_t       batch_id)
{
    ShellChunkResult r;
    memset(&r, 0, sizeof(r));
    r.layer      = 0;
    r.batch_id   = batch_id;
    r.chunk_z    = chunk_z;
    r.fibo_phase = _shell_fibo_phase(chunk_z);

    uint8_t  rotbuf[64];
    uint8_t  best_buf[64];
    uint64_t best_isect = 0;
    uint8_t  best_rot   = 0;
    int      best_pc    = -1;

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        _shell_rotate64(rotbuf, chunk, rot);

        /* v3 fix: use PURE content block (no metadata override)
         * _shell_chunk_to_block() overwrites 75% of core.raw with
         * geometry context (face_id, engine_id, vpos, fibo_gear, quad_flags),
         * leaving only 16-bit data residue. fold_fibo_intersect on that
         * contaminated block tells us about GEOMETRY CONTEXT, not CONTENT.
         *
         * _chunk_to_pure_block() uses first 8B as-is → fibo_intersect
         * reflects REAL content structure → accurate rotation selection. */
        DiamondBlock db = _chunk_to_pure_block(rotbuf);

        /* pure block always passes XOR audit (invert = ~core.raw) */
        uint64_t isect = fold_fibo_intersect(&db);
        int pc = __builtin_popcountll(isect);

        if (pc > best_pc) {
            best_pc    = pc;
            best_isect = isect;
            best_rot   = rot;
            memcpy(best_buf, rotbuf, 64);
        }
    }

    r.best_rot   = best_rot;
    r.fibo_isect = best_isect;
    r.isect_pc   = (uint8_t)(best_pc < 0 ? 0 : best_pc);

    memcpy(&r.diff_a, best_buf,     8);
    memcpy(&r.diff_b, best_buf + 8, 8);

    /* classify */
    if (r.isect_pc == 0) {
        r.flag = SHELL_FLAG_FLAT;
    } else if (r.isect_pc <= SHELL_SPARSE_THRESH) {
        r.flag = SHELL_FLAG_SPARSE;
        r.seed = _shell_fnv64(best_buf, 64);
    } else {
        r.flag = SHELL_FLAG_DENSE;
        r.seed = _shell_fnv64(best_buf, 64);
    }

    return r;
}

static inline uint32_t shell_encode_size_v2(const ShellChunkResult *r)
{
    switch (r->flag) {
        case SHELL_FLAG_FLAT:   return 2u;
        case SHELL_FLAG_SPARSE: return 66u;  /* full rotated 64B + 2B header */
        case SHELL_FLAG_DENSE:  return 66u;  /* same — lossless minimum */
        case SHELL_FLAG_BATCH:  return 5u;   /* batch ID + z-index */
        default:                return 66u;
    }
}

/* ── batch API ──────────────────────────────────────────── */

static inline ShellBatch *shell_batch_new_v2(uint8_t layer, uint32_t batch_id)
{
    static const uint32_t caps[] = {
        SHELL_L0_CHUNKS, SHELL_L1_CHUNKS, SHELL_L2_CHUNKS, SHELL_L3_CHUNKS
    };
    uint8_t l = layer < 4 ? layer : 3;
    ShellBatch *b = (ShellBatch *)calloc(1, sizeof(ShellBatch));
    b->layer    = l;
    b->capacity = caps[l];
    b->batch_id = batch_id;
    b->cube     = (uint8_t *)calloc(b->capacity * SHELL_CHUNK_SZ, 1);
    return b;
}

static inline void shell_batch_free_v2(ShellBatch *b)
{
    if (b) { free(b->cube); free(b); }
}

static inline int shell_batch_push_v2(ShellBatch *b, const uint8_t *chunk)
{
    if (b->count >= b->capacity) return 1;
    memcpy(b->cube + b->count * SHELL_CHUNK_SZ, chunk, SHELL_CHUNK_SZ);
    b->count++;
    return (b->count >= b->capacity) ? 1 : 0;
}

/*
 * shell_batch_flush_v2()
 * Batch rotation: XOR-accumulate fibo_isect across all chunks per rotation
 * pick rotation where accumulated isect has highest popcount
 * = rotation where geometric constants are most consistent across batch
 */
static inline uint8_t shell_batch_flush_v2(ShellBatch *b,
                                             uint64_t *out_batch_isect)
{
    uint8_t  rotbuf[64];
    uint64_t best_acc = 0;
    uint8_t  best_rot = 0;
    int      best_pc  = -1;

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        uint64_t acc_isect = 0;
        for (uint32_t i = 0; i < b->count; i++) {
            _shell_rotate64(rotbuf, b->cube + i * SHELL_CHUNK_SZ, rot);

            /* v3: use pure content DiamondBlock for accurate fibo_intersect */
            DiamondBlock db = _chunk_to_pure_block(rotbuf);

            acc_isect ^= fold_fibo_intersect(&db);
        }
        int pc = __builtin_popcountll(acc_isect);
        if (pc > best_pc) {
            best_pc  = pc;
            best_rot = rot;
            best_acc = acc_isect;
        }
    }

    b->best_rot      = best_rot;
    b->batch_isect   = best_acc;
    if (out_batch_isect) *out_batch_isect = best_acc;
    return best_rot;
}

/* ══════════════════════════════════════════════════════════
 * shell_run_pipeline_v2()
 * Full pipeline with fibo_intersect-based classification
 * ══════════════════════════════════════════════════════════ */
static inline ShellMetrics shell_run_pipeline_v2(
    const uint8_t *data,
    uint64_t       n_chunks,
    uint8_t        layer)
{
    ShellMetrics m;
    memset(&m, 0, sizeof(m));
    m.n_chunks  = n_chunks;
    m.raw_bytes = n_chunks * SHELL_CHUNK_SZ;

    if (layer == 0) {
        for (uint64_t i = 0; i < n_chunks; i++) {
            ShellChunkResult r = shell_classify_chunk_v2(
                data + i * SHELL_CHUNK_SZ, (uint32_t)i, 0);
            uint32_t esz = shell_encode_size_v2(&r);
            m.enc_bytes    += esz;
            m.isect_total  += r.isect_pc;
            m.rot_wins[r.best_rot]++;
            switch (r.flag) {
                case SHELL_FLAG_FLAT:   m.n_flat++;   break;
                case SHELL_FLAG_SPARSE: m.n_sparse++; break;
                case SHELL_FLAG_DENSE:  m.n_dense++;  break;
                default: break;
            }
        }
    } else {
        uint32_t batch_id = 0;
        ShellBatch *batch = shell_batch_new_v2(layer, batch_id);

        for (uint64_t i = 0; i < n_chunks; i++) {
            int full = shell_batch_push_v2(batch,
                            data + i * SHELL_CHUNK_SZ);
            if (full) {
                uint64_t bisect = 0;
                uint8_t  br     = shell_batch_flush_v2(batch, &bisect);
                m.rot_wins[br]++;
                m.isect_total += (uint64_t)__builtin_popcountll(bisect);
                m.enc_bytes   += 4u + batch->count * 5u;
                m.n_batch     += batch->count;
                shell_batch_free_v2(batch);
                batch = shell_batch_new_v2(layer, ++batch_id);
            }
        }
        if (batch->count > 0) {
            uint64_t bisect = 0;
            uint8_t  br     = shell_batch_flush_v2(batch, &bisect);
            m.rot_wins[br]++;
            m.isect_total += (uint64_t)__builtin_popcountll(bisect);
            m.enc_bytes   += 4u + batch->count * 5u;
            m.n_batch     += batch->count;
        }
        shell_batch_free_v2(batch);
    }

    m.ratio = m.enc_bytes > 0
        ? (double)m.raw_bytes / (double)m.enc_bytes : 0.0;
    return m;
}

#endif /* DIAMOND_SHELL_V2_H */
