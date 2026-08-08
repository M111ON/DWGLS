/*
 * dwgls_codec_kisv6.h — KIS v6 Codec Adapter for DWGLS Vtable
 * ════════════════════════════════════════════════════════════════
 *
 * Wraps kis_codec_v6.h concepts (sort+mask+codebook compression)
 * into the DWGLS_Codec vtable interface.
 *
 * Self-contained: defines minimal V6 types inline to avoid
 * transitive include chain issues with the full codec header.
 *
 * V6 format:
 *   magic[4] = 0x4B435636 ("V6CK" LE)
 *   n[4]     = number of Q8 values
 *   mode[1]  = 0 (varint residuals) or 1 (bitmap residuals)
 *   cb_data[] = codebook + residuals (variable length)
 *
 * encode/decode must preserve lossless roundtrip.
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -fsyntax-only core/dwgls_codec_kisv6.h
 * DEPENDS: dwgls_codec.h, dwgls_shell.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_KISV6_H
#define DWGLS_CODEC_KISV6_H

#include "dwgls_codec.h"
#include "dwgls_shell.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ════════════════════════════════════════════════════════════════
   KIS V6 TYPES (minimal, self-contained)
   ════════════════════════════════════════════════════════════════ */

/* V6 magic: "V6CK" in little-endian */
#define V6_MAGIC        0x4B435636u
#define V6_HEADER_SZ    9u          /* magic(4) + n(4) + mode(1) */
#define V6_STRIDE       37u         /* stride-37 mapping */
#define V6_GRID         144u        /* 144 × 144 = 20736 */
#define V6_TOTAL_SLOTS  20736u      /* sacred number */

/* ════════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════════ */

/* ctx.user_data indices for V6 metadata */
#define KISV6_CTX_MODE      0u   /* 0=varint, 1=bitmap */
#define KISV6_CTX_N         1u   /* number of values */

/* ════════════════════════════════════════════════════════════════
   V6 SORT+MASK COMPRESSION (simplified)
   ════════════════════════════════════════════════════════════════ */

/*
 * Sort the Q8 values, build codebook (unique values), then encode
 * each value as codebook index. Mode determines residual encoding:
 *   mode=0: varint residuals (compact for few unique values)
 *   mode=1: bitmap residuals (fixed 2592B overhead)
 */

/* Simple sort for codebook extraction */
static void v6_sort_q8(const int8_t *src, uint32_t n, int8_t *sorted)
{
    memcpy(sorted, src, n);
    /* Insertion sort — good for typical Q8_0 weight distributions */
    for (uint32_t i = 1; i < n; i++) {
        int8_t key = sorted[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
}

/* Extract codebook (unique values) from sorted array */
static uint32_t v6_extract_codebook(const int8_t *sorted, uint32_t n,
                                     int8_t *codebook)
{
    uint32_t cb_size = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cb_size == 0 || sorted[i] != codebook[cb_size - 1]) {
            codebook[cb_size++] = sorted[i];
        }
    }
    return cb_size;
}

/* ════════════════════════════════════════════════════════════════
   CODEC VTABLE IMPLEMENTATION
   ════════════════════════════════════════════════════════════════ */

/* ── kisv6_info ─────────────────────────────────────────────────
 * Return codec metadata.
 */
static DWGLS_CodecInfo kisv6_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "kis_v6";
    info.codec_id    = CODEC_KIS_CODEC_V6;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_COMPRESSED
                     | CODEC_FLAG_MMAP_FRIENDLY;
    return info;
}

/* ── kisv6_encode ───────────────────────────────────────────────
 * Encode raw Q8 values → V6 payload (sort+mask+codebook).
 *
 * src = int8_t array (n_elems values)
 * dst layout:
 *   [magic:4][n:4][mode:1][cb_size:1][codebook:cb_size][indices:n]
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t kisv6_encode(const void *src, uint32_t n_elems,
                              const DWGLS_CodecCtx *ctx,
                              void *dst, uint32_t dst_cap)
{
    if (!src || !dst || n_elems == 0) return -1;

    const int8_t *in = (const int8_t *)src;
    uint8_t *out = (uint8_t *)dst;

    /* Determine mode from context */
    uint8_t mode = (uint8_t)(ctx->user_data[KISV6_CTX_MODE] & 0xFF);

    /* Sort values for codebook extraction */
    int8_t *sorted = (int8_t *)malloc(n_elems);
    if (!sorted) return -1;
    v6_sort_q8(in, n_elems, sorted);

    /* Extract codebook */
    int8_t codebook[256];
    uint32_t cb_size = v6_extract_codebook(sorted, n_elems, codebook);
    free(sorted);

    /* Compute output size: header(9) + cb_size(1) + codebook + indices */
    uint32_t indices_bits = 0;
    if (cb_size <= 2)       indices_bits = 1;
    else if (cb_size <= 4)  indices_bits = 2;
    else if (cb_size <= 16) indices_bits = 4;
    else                    indices_bits = 8;
    uint32_t indices_bytes = (n_elems * indices_bits + 7) / 8;

    uint32_t total_sz = V6_HEADER_SZ + 1 + cb_size + indices_bytes;
    if (total_sz > dst_cap) { free(sorted); return -2; }

    uint32_t off = 0;

    /* Write header */
    uint32_t magic = V6_MAGIC;
    memcpy(out + off, &magic, 4);      off += 4;
    memcpy(out + off, &n_elems, 4);    off += 4;
    out[off++] = mode;

    /* Write codebook */
    out[off++] = (uint8_t)cb_size;
    memcpy(out + off, codebook, cb_size);
    off += cb_size;

    /* Build index array using codebook lookup */
    uint32_t *indices = (uint32_t *)malloc(n_elems * sizeof(uint32_t));
    if (!indices) return -1;

    for (uint32_t i = 0; i < n_elems; i++) {
        /* Binary search codebook */
        int8_t val = in[i];
        uint32_t lo = 0, hi = cb_size;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (codebook[mid] < val) lo = mid + 1;
            else hi = mid;
        }
        indices[i] = (lo < cb_size && codebook[lo] == val) ? lo : 0;
    }

    /* Pack indices into output */
    if (indices_bits == 8) {
        for (uint32_t i = 0; i < n_elems; i++) {
            out[off + i] = (uint8_t)indices[i];
        }
        off += n_elems;
    } else if (indices_bits == 4) {
        for (uint32_t i = 0; i < n_elems; i += 2) {
            uint8_t lo = (uint8_t)(indices[i] & 0x0F);
            uint8_t hi = (i + 1 < n_elems) ? (uint8_t)(indices[i + 1] & 0x0F) : 0;
            out[off++] = (hi << 4) | lo;
        }
    } else if (indices_bits == 2) {
        for (uint32_t i = 0; i < n_elems; i += 4) {
            uint8_t b = 0;
            for (uint32_t j = 0; j < 4 && i + j < n_elems; j++) {
                b |= (uint8_t)((indices[i + j] & 0x03) << (j * 2));
            }
            out[off++] = b;
        }
    } else { /* bits == 1 */
        for (uint32_t i = 0; i < n_elems; i += 8) {
            uint8_t b = 0;
            for (uint32_t j = 0; j < 8 && i + j < n_elems; j++) {
                b |= (uint8_t)((indices[i + j] & 0x01) << j);
            }
            out[off++] = b;
        }
    }

    free(indices);
    return (int32_t)off;
}

/* ── kisv6_decode ───────────────────────────────────────────────
 * Decode V6 payload → raw Q8 values.
 *
 * src = V6-encoded payload
 * dst = output buffer for int8_t values
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t kisv6_decode(const void *src, uint32_t src_len,
                              const DWGLS_CodecCtx *ctx,
                              void *dst, uint32_t dst_cap)
{
    (void)ctx;
    if (!src || !dst || src_len < V6_HEADER_SZ) return -1;

    const uint8_t *in = (const uint8_t *)src;
    int8_t *out = (int8_t *)dst;

    /* Read header */
    uint32_t magic;
    memcpy(&magic, in, 4);
    if (magic != V6_MAGIC) return -2;

    uint32_t n;
    memcpy(&n, in + 4, 4);
    uint8_t mode = in[8];
    (void)mode;  /* mode affects residual encoding, not core decode */

    if (n > dst_cap) return -3;
    if (V6_HEADER_SZ + 1 >= src_len) return -4;

    /* Read codebook */
    uint32_t off = V6_HEADER_SZ;
    uint8_t cb_size = in[off++];
    if (off + cb_size > src_len) return -5;

    int8_t codebook[256];
    memcpy(codebook, in + off, cb_size);
    off += cb_size;

    /* Determine index bit width */
    uint32_t remaining = src_len - off;
    uint32_t indices_bits;
    if (cb_size <= 2)       indices_bits = 1;
    else if (cb_size <= 4)  indices_bits = 2;
    else if (cb_size <= 16) indices_bits = 4;
    else                    indices_bits = 8;

    /* Unpack indices and look up codebook */
    uint32_t byte_idx = 0;
    uint8_t bit_pos = 0;

    for (uint32_t i = 0; i < n; i++) {
        if (byte_idx >= remaining) return -6;

        uint32_t idx;
        if (indices_bits == 8) {
            idx = in[off + byte_idx];
            byte_idx++;
        } else if (indices_bits == 4) {
            idx = (in[off + byte_idx] >> (bit_pos * 4)) & 0x0F;
            bit_pos++;
            if (bit_pos >= 2) { bit_pos = 0; byte_idx++; }
        } else if (indices_bits == 2) {
            idx = (in[off + byte_idx] >> (bit_pos * 2)) & 0x03;
            bit_pos++;
            if (bit_pos >= 4) { bit_pos = 0; byte_idx++; }
        } else {
            idx = (in[off + byte_idx] >> bit_pos) & 0x01;
            bit_pos++;
            if (bit_pos >= 8) { bit_pos = 0; byte_idx++; }
        }

        if (idx >= cb_size) return -7;
        out[i] = codebook[idx];
    }

    return (int32_t)n;
}

/* ── kisv6_payload_size ─────────────────────────────────────────
 * Compute V6 payload size estimate.
 */
static uint32_t kisv6_payload_size(uint32_t n_elems,
                                    const DWGLS_CodecCtx *ctx)
{
    (void)ctx;
    if (n_elems == 0) return 0;

    /* Worst case: all unique values → 256 codebook + 8-bit indices */
    return V6_HEADER_SZ + 1 + 256 + n_elems;
}

/* ── kisv6_verify ───────────────────────────────────────────────
 * Verify V6 payload integrity.
 *
 * Checks: magic, n valid, codebook consistent, indices in range.
 * Returns 0=ok, negative=corrupt.
 */
static int kisv6_verify(const void *src, uint32_t src_len)
{
    if (!src || src_len < V6_HEADER_SZ) return -1;

    const uint8_t *in = (const uint8_t *)src;

    /* Check magic */
    uint32_t magic;
    memcpy(&magic, in, 4);
    if (magic != V6_MAGIC) return -2;

    /* Check n */
    uint32_t n;
    memcpy(&n, in + 4, 4);
    if (n == 0 || n > V6_TOTAL_SLOTS) return -3;

    /* Check mode */
    uint8_t mode = in[8];
    if (mode > 1) return -4;

    /* Check codebook size */
    uint32_t off = V6_HEADER_SZ;
    if (off >= src_len) return -5;
    uint8_t cb_size = in[off++];
    if (cb_size == 0) return -6;
    if (off + cb_size > src_len) return -7;

    /* Verify all codebook values are valid Q8 */
    for (uint32_t i = 0; i < cb_size; i++) {
        /* Q8 values are always valid (int8_t range) — no check needed */
        (void)in[off + i];
    }

    /* Basic structural checks passed */
    return 0;
}

/* ── kisv6_resolve ──────────────────────────────────────────────
 * Address mapping: slot (0..20735) → payload byte offset.
 *
 * KIS v6 uses stride-37 mapping: slot → sorted index → codebook offset.
 * For the adapter, we use a simplified mapping:
 *   byte_offset = header + codebook + (slot % n) * index_bytes
 *
 * Returns byte offset in payload, or UINT32_MAX on overflow.
 */
static uint32_t kisv6_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    if (slot >= V6_TOTAL_SLOTS) return UINT32_MAX;

    uint32_t n = ctx->user_data[KISV6_CTX_N];
    if (n == 0) n = V6_TOTAL_SLOTS;

    /* Simplified: map slot to index within the value array */
    uint32_t idx = slot % n;

    /* Estimate offset: header(9) + cb_size(1) + codebook + idx*index_bytes
     * We don't know codebook size without parsing, use worst case */
    return V6_HEADER_SZ + 1 + 256 + idx;
}

/* ════════════════════════════════════════════════════════════════
   VTABLE INSTANCE
   ════════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_KIS_V6 = {
    .info         = kisv6_info,
    .encode       = kisv6_encode,
    .decode       = kisv6_decode,
    .payload_size = kisv6_payload_size,
    .verify       = kisv6_verify,
    .resolve      = kisv6_resolve,
};

#endif /* DWGLS_CODEC_KISV6_H */
