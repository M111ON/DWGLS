/*
 * diamond_shell_codec.h — Diamond Shell v3 Codec
 * ═══════════════════════════════════════════════════════
 * Geometric lossless codec using fold_fibo_intersect on pure content.
 *
 * Key fix vs v2:
 *   1. PURE content DiamondBlock (no metadata override)
 *   2. pc-based classification (fibo_intersect popcount)
 *   3. Accurate wire format: FLAT=2B, SPARSE=66B, DENSE=66B
 *   4. fibo_intersect exposed for flow/dedup in pipeline
 *
 * Wire format (per chunk):
 *   FLAT   [flag:1B][rot:1B]                  = 2B
 *   SPARSE [flag:1B][rot:1B][rotated:64B]      = 66B
 *   DENSE  [flag:1B][rot:1B][rotated:64B]      = 66B
 *
 * The rotated chunk is stored for SPARSE and DENSE because:
 *   - 8B seed is not reversible (FNV is one-way)
 *   - Need full 64B for lossless reconstruction
 *   - Actual compression comes from FLOW/BATCH grouping above codec layer
 *
 * Flow encoding protocol (upper layer — pipeline_glue):
 *   When consecutive chunks share stable fibo_intersect:
 *   → emit flow header with shared invariant once
 *   → store per-chunk delta (typically 2-8B)
 *   → FLOW reconstruction: base + delta × N
 */

#ifndef DIAMOND_SHELL_CODEC_H
#define DIAMOND_SHELL_CODEC_H

#include <stdint.h>
#include <string.h>
#include "diamond_shell_v2.h"

/* ── inverse rotation ───────────────────────────────────────────── */
static inline void _shell_inverse_rotate64(uint8_t out[64],
                                            const uint8_t in[64],
                                            uint8_t rot)
{
    uint8_t tmp[64];
    memset(tmp, 0, 64);
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
                tmp[sz*16 + sy*4 + sx] = in[z*16 + y*4 + x];
            }
        }
    }
    memcpy(out, tmp, 64);
}

/* ── encode one chunk ───────────────────────────────────────────── */
static inline uint32_t shell_encode_chunk(uint8_t *buf,
                                           const ShellChunkResult *r,
                                           const uint8_t rotbuf[64])
{
    buf[0] = r->flag;
    buf[1] = r->best_rot;

    switch (r->flag) {
        case SHELL_FLAG_FLAT:
            return 2;

        case SHELL_FLAG_SPARSE:
        case SHELL_FLAG_DENSE:
            /* store full 64B rotated → guaranteed lossless */
            memcpy(buf + 2, rotbuf, 64);
            return 66;

        default:
            return 2;
    }
}

static inline uint32_t shell_encode_size_lossless(const ShellChunkResult *r)
{
    return (r->flag == SHELL_FLAG_FLAT) ? 2 : 66;
}

/* ── stream encode ──────────────────────────────────────────────── */
static inline uint64_t shell_stream_encode(const uint8_t *data,
                                            uint64_t       n_chunks,
                                            uint8_t       *out)
{
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        const uint8_t *chunk = data + i * SHELL_CHUNK_SZ;

        /* classify using v3 pure-content DiamondBlock */
        uint8_t rotbuf[64];
        uint8_t best_buf[64];
        uint64_t best_isect = 0;
        uint8_t  best_rot   = 0;
        int      best_pc    = -1;

        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, chunk, rot);
            DiamondBlock db = _chunk_to_pure_block(rotbuf);
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) {
                best_pc    = pc;
                best_isect = isect;
                best_rot   = rot;
                memcpy(best_buf, rotbuf, 64);
            }
        }

        ShellChunkResult r;
        int chunk_is_zero = 1;
        for (uint32_t z = 0; z < SHELL_CHUNK_SZ; z++) {
            if (chunk[z]) { chunk_is_zero = 0; break; }
        }

        memset(&r, 0, sizeof(r));
        r.best_rot   = best_rot;
        r.fibo_isect = best_isect;
        r.isect_pc   = (uint8_t)(best_pc < 0 ? 0 : best_pc);
        r.flag = chunk_is_zero ? SHELL_FLAG_FLAT
               : (r.isect_pc <= SHELL_SPARSE_THRESH) ? SHELL_FLAG_SPARSE
               : SHELL_FLAG_DENSE;

        pos += shell_encode_chunk(out + pos, &r, best_buf);
    }
    return pos;
}

/* ── stream decode ──────────────────────────────────────────────── */
static inline uint64_t shell_stream_decode(const uint8_t *in,
                                            uint64_t       n_chunks,
                                            uint8_t       *out)
{
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        uint8_t flag = in[pos];
        uint8_t rot  = in[pos + 1];
        pos += 2;

        uint8_t *chunk_out = out + i * SHELL_CHUNK_SZ;

        if (flag == SHELL_FLAG_FLAT) {
            memset(chunk_out, 0, SHELL_CHUNK_SZ);
        } else {
            uint8_t rotbuf[64];
            memcpy(rotbuf, in + pos, 64);
            pos += 64;
            _shell_inverse_rotate64(chunk_out, rotbuf, rot);
        }
    }
    return pos;
}

#endif /* DIAMOND_SHELL_CODEC_H */
