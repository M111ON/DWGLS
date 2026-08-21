/*
 * kv_remap_diamond.h — Diamond Shell integration for KV Remap
 * ════════════════════════════════════════════════════════════
 *
 * Replaces RLE compression with Diamond Shell (64B chunk-based).
 * Benchmark A proved: Diamond Shell 2.32x vs RLE 2.10x at 40% change.
 *
 * Wire format:
 *   [DIA_MAGIC:4][orig_size:4][comp_size:4][n_chunks:4][flags:1] [encoded chunks...]
 *
 * Per-chunk (from diamond_shell_codec.h):
 *   FLAT:   [flag:1][rot:1]                      = 2B
 *   SPARSE: [flag:1][rot:1][rotbuf:64]           = 66B
 *   DENSE:  [flag:1][rot:1][rotbuf:64]           = 66B
 *
 * Build: gcc -O2 -std=c11 -I../collection/geopixel -I../collection/Hfolder
 *        -I../collection/geo_jump_module/include -I../collection/dgls/diamond/include
 */

#ifndef KV_REMAP_DIAMOND_H
#define KV_REMAP_DIAMOND_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "diamond_shell_v2.h"
#include "diamond_shell_codec.h"

/* ── Diamond header ────────────────────────────────────── */
#define DIA_MAGIC  0x44494131  /* "DIA1" */

typedef struct {
    uint32_t magic;
    uint32_t orig_size;
    uint32_t comp_size;
    uint32_t n_chunks;
    uint8_t  flags;           /* reserved for future use */
} DiamondRemapHeader;

/* ── Compress XOR diff with Diamond Shell ──────────────── */
static inline int kv_remap_compress_diamond(const void *data, size_t size,
                                            void **out, size_t *out_size)
{
    if (!data || size == 0) return -1;

    const uint8_t *src = (const uint8_t *)data;
    uint64_t n_chunks = (size + 63) / 64;

    /* Allocate worst-case output: header + n_chunks × 66B */
    size_t max_out = sizeof(DiamondRemapHeader) + n_chunks * 66 + 256;
    uint8_t *buf = (uint8_t *)calloc(max_out, 1);
    if (!buf) return -1;

    DiamondRemapHeader *hdr = (DiamondRemapHeader *)buf;
    uint8_t *dst = buf + sizeof(DiamondRemapHeader);

    /* Encode with Diamond Shell */
    uint64_t encoded = shell_stream_encode(src, n_chunks, dst);

    /* Build header */
    hdr->magic = DIA_MAGIC;
    hdr->orig_size = (uint32_t)size;
    hdr->comp_size = (uint32_t)(sizeof(DiamondRemapHeader) + encoded);
    hdr->n_chunks = (uint32_t)n_chunks;
    hdr->flags = 0;

    size_t total = sizeof(DiamondRemapHeader) + encoded;

    /* If Diamond Shell is larger, store raw */
    if (total >= size + sizeof(DiamondRemapHeader)) {
        free(buf);
        size_t raw_sz = sizeof(DiamondRemapHeader) + size;
        uint8_t *raw = (uint8_t *)calloc(raw_sz, 1);
        if (!raw) return -1;
        DiamondRemapHeader *rh = (DiamondRemapHeader *)raw;
        rh->magic = DIA_MAGIC;
        rh->orig_size = (uint32_t)size;
        rh->comp_size = (uint32_t)raw_sz;
        rh->n_chunks = 0;  /* n_chunks=0 means raw data */
        rh->flags = 0;
        memcpy(raw + sizeof(DiamondRemapHeader), data, size);
        *out = raw;
        *out_size = raw_sz;
        return 1;  /* raw */
    }

    *out = buf;
    *out_size = total;
    return 0;  /* compressed */
}

/* ── Decompress Diamond Shell ──────────────────────────── */
static inline void *kv_remap_decompress_diamond(const void *compressed, size_t comp_size,
                                                 size_t *out_size)
{
    if (comp_size < sizeof(DiamondRemapHeader)) return NULL;
    const DiamondRemapHeader *hdr = (const DiamondRemapHeader *)compressed;

    if (hdr->magic != DIA_MAGIC) return NULL;

    size_t orig = hdr->orig_size;
    if (out_size) *out_size = orig;

    /* Raw data */
    if (hdr->n_chunks == 0) {
        uint8_t *out = (uint8_t *)malloc(orig > 0 ? orig : 1);
        if (!out) return NULL;
        memcpy(out, (const uint8_t *)compressed + sizeof(DiamondRemapHeader), orig);
        return out;
    }

    /* Diamond Shell compressed */
    uint8_t *out = (uint8_t *)calloc(orig > 0 ? orig : 1, 1);
    if (!out) return NULL;

    const uint8_t *src = (const uint8_t *)compressed + sizeof(DiamondRemapHeader);
    uint64_t decoded = shell_stream_decode(src, hdr->n_chunks, out);

    if (decoded == 0 && hdr->n_chunks > 0) {
        free(out);
        return NULL;
    }

    return out;
}

/* ── Stats: count FLAT/SPARSE/DENSE in compressed data ─── */
typedef struct {
    uint64_t flat;
    uint64_t sparse;
    uint64_t dense;
    uint64_t total;
} DiamondStats;

static inline DiamondStats kv_remap_diamond_stats(const void *compressed, size_t comp_size) {
    DiamondStats stats;
    memset(&stats, 0, sizeof(stats));

    if (comp_size < sizeof(DiamondRemapHeader)) return stats;
    const DiamondRemapHeader *hdr = (const DiamondRemapHeader *)compressed;
    if (hdr->magic != DIA_MAGIC || hdr->n_chunks == 0) return stats;

    const uint8_t *src = (const uint8_t *)compressed + sizeof(DiamondRemapHeader);
    uint64_t pos = 0;
    stats.total = hdr->n_chunks;

    for (uint32_t i = 0; i < hdr->n_chunks; i++) {
        uint8_t flag = src[pos];
        pos += 2;  /* flag + rot */
        switch (flag) {
            case SHELL_FLAG_FLAT:   stats.flat++;   pos += 0;  break;
            case SHELL_FLAG_SPARSE: stats.sparse++; pos += 64; break;
            case SHELL_FLAG_DENSE:  stats.dense++;  pos += 64; break;
            default: pos += 64; break;
        }
    }

    return stats;
}

#endif /* KV_REMAP_DIAMOND_H */
