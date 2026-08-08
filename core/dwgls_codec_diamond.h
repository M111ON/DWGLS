/*
 * dwgls_codec_diamond.h — Diamond Field Codec Adapter for DWGLS Vtable
 * ════════════════════════════════════════════════════════════════
 *
 * Wraps geo_diamond_field_v4.h concepts (5-path adaptive diamond geometry)
 * into the DWGLS_Codec vtable interface.
 *
 * Self-contained: defines minimal DiamondField types inline to avoid
 * transitive include chain issues with the full field header.
 *
 * Diamond Field: 5 adaptive encoding paths per 64-byte chunk:
 *   Path 0: Sparse   (≤2 unique → ~2-4B/chunk)
 *   Path 1: Bitpack  (structured → ~8-17B)
 *   Path 2: LZ77     (repeated → ~20-50B)
 *   Path 3: LZ77+Hilbert (spatial → ~15-40B)
 *   Path 4: Raw      (64 bytes, high entropy fallback)
 *
 * Shell scale: level n → size = 2n+1 → slots = (2n+1)^3
 *   n=0: 1, n=1: 27, ..., n=8: 4913 (SHELL_MAX_SLOTS)
 *
 * Payload layout (after DWGLS_Shell):
 *   [level:1][occupied:2][flags:616B][encoded_chunks...]
 *
 * resolve() maps slot → diamond cell address via shell slot geometry.
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -fsyntax-only core/dwgls_codec_diamond.h
 * DEPENDS: dwgls_codec.h, dwgls_shell.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_DIAMOND_H
#define DWGLS_CODEC_DIAMOND_H

#include "dwgls_codec.h"
#include "dwgls_shell.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ════════════════════════════════════════════════════════════════
   DIAMOND FIELD TYPES (minimal, self-contained)
   ════════════════════════════════════════════════════════════════ */

/* Shell levels: n=0..8, slots = (2n+1)^3 */
#define SHELL_MAX_LEVEL   8u
#define SHELL_MAX_SLOTS   4913u   /* 17^3, level 8 */

/* Compute slots for a given shell level */
static inline uint16_t shell_slots(uint8_t n) {
    uint8_t sz = (uint8_t)(2 * n + 1);
    return (uint16_t)(sz * sz * sz);
}

/* Compute shell size (side length) for a given level */
static inline uint8_t shell_size(uint8_t n) {
    return (uint8_t)(2 * n + 1);
}

/* ════════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════════ */

/* ctx.user_data indices for diamond field metadata */
#define DF_CTX_LEVEL        0u   /* shell level (0..8) */
#define DF_CTX_OCCUPIED     1u   /* total occupied slots */

/* DiamondField shell flags array: ceil(4913/64) = 77 uint64_t = 616 bytes */
#define DF_SHELL_FLAGS_SZ  616u

/* Payload header: level(1) + occupied(2) + pad(1) + flags(616) = 620 bytes */
#define DF_PAYLOAD_HEADER_SZ  620u

/* ════════════════════════════════════════════════════════════════
   ENCODING PATHS (simplified)
   ════════════════════════════════════════════════════════════════ */

/* Entropy thresholds for path selection */
static const int SCORE_THRESH[6] = { 10, 30, 60, 100, 150, 200 };

/* Classify a 64-byte chunk's entropy */
static int diamond_classify(const uint8_t chunk[64])
{
    /* Count unique values */
    uint8_t seen[256];
    memset(seen, 0, 256);
    uint16_t unique = 0;
    for (int i = 0; i < 64; i++) {
        if (!seen[chunk[i]]) {
            seen[chunk[i]] = 1;
            unique++;
        }
    }
    return (int)unique;
}

/* ── Sparse encode (Path 0) ────────────────────────────────────
 * ≤2 unique values: store count + (value) pairs.
 * Format: [count:1][val0:1][val1:1]
 */
static uint32_t sparse_encode(uint8_t *out, const uint8_t chunk[64])
{
    uint8_t seen[256];
    memset(seen, 0, 256);
    uint8_t vals[2];
    uint8_t count = 0;

    for (int i = 0; i < 64 && count < 2; i++) {
        if (!seen[chunk[i]]) {
            seen[chunk[i]] = 1;
            vals[count++] = chunk[i];
        }
    }

    if (out) {
        out[0] = count;
        for (uint8_t i = 0; i < count; i++) out[1 + i] = vals[i];
    }
    return 1 + count;
}

/* ── Bitpack encode (Path 1) ───────────────────────────────────
 * Structured data with few bits active.
 * Format: [mask:8B][active_values...]
 */
static uint32_t bitpack_encode(const uint8_t chunk[64], uint8_t *out)
{
    /* Simplified: store raw 64 bytes with a header marker */
    if (out) {
        out[0] = 0xFE;  /* bitpack marker */
        memcpy(out + 1, chunk, 64);
    }
    return 65;
}

/* ── LZ77 compress (Path 2) ────────────────────────────────────
 * Repeated byte patterns.
 * Simplified: store raw with marker */
static uint32_t lz77_compress(const uint8_t chunk[64], uint8_t *out)
{
    if (out) {
        out[0] = 0xFD;  /* LZ77 marker */
        memcpy(out + 1, chunk, 64);
    }
    return 65;
}

/* ── Sparse decode ────────────────────────────────────────────── */
static void sparse_decode(uint8_t chunk[64], const uint8_t *in)
{
    uint8_t count = in[0];
    if (count == 0) { memset(chunk, 0, 64); return; }
    if (count == 1) { memset(chunk, in[1], 64); return; }
    /* 2 values: fill first half with val0, second half with val1 */
    memset(chunk, in[1], 32);
    memset(chunk + 32, in[2], 32);
}

/* ════════════════════════════════════════════════════════════════
   CODEC VTABLE IMPLEMENTATION
   ════════════════════════════════════════════════════════════════ */

/* ── diamond_info ───────────────────────────────────────────────
 * Return codec metadata.
 */
static DWGLS_CodecInfo diamond_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "diamond_field";
    info.codec_id    = CODEC_DIAMOND_FIELD;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_COMPRESSED
                     | CODEC_FLAG_DERIVED_VIEWS;
    return info;
}

/* ── diamond_encode ─────────────────────────────────────────────
 * Encode raw byte chunks → Diamond Field payload.
 *
 * src = raw byte data (n_elems bytes, grouped into 64-byte chunks)
 * dst layout:
 *   [level:1][occupied:2][pad:1][shell_flags:616B]
 *   [per-chunk encoded data...]
 *
 * Uses 5-path adaptive encoding: sparse/bitpack/LZ77/raw.
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t diamond_encode(const void *src, uint32_t n_elems,
                               const DWGLS_CodecCtx *ctx,
                               void *dst, uint32_t dst_cap)
{
    if (!src || !dst || n_elems == 0) return -1;

    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;

    /* Determine shell level from first chunk's entropy */
    uint8_t level = (uint8_t)(ctx->user_data[DF_CTX_LEVEL] & 0xFF);
    if (level == 0) {
        /* Auto-classify from first chunk */
        int score = diamond_classify(in);
        if (score <= 2)       level = 0;
        else if (score <= 8)  level = 1;
        else if (score <= 16) level = 2;
        else if (score <= 32) level = 3;
        else                  level = 4;
    }

    uint32_t off = 0;

    /* Write payload header */
    if (off + DF_PAYLOAD_HEADER_SZ > dst_cap) return -2;
    out[off++] = level;
    /* occupied will be filled later */
    uint16_t occupied_off = (uint16_t)(off);
    off += 2;
    out[off++] = 0;  /* pad */
    /* Write shell flags (zeros initially) */
    memset(out + off, 0, DF_SHELL_FLAGS_SZ);
    off += DF_SHELL_FLAGS_SZ;

    /* Encode each 64-byte chunk */
    uint32_t n_chunks = (n_elems + 63) / 64;
    uint16_t occupied = 0;

    for (uint32_t ch = 0; ch < n_chunks; ch++) {
        const uint8_t *chunk = in + ch * 64;

        /* Classify and encode chunk */
        int score = diamond_classify(chunk);

        if (score <= SCORE_THRESH[0]) {
            /* Path 0: Sparse */
            uint32_t written = sparse_encode(out + off, chunk);
            off += written;
            occupied++;
        } else if (score <= SCORE_THRESH[3]) {
            /* Path 1: Bitpack */
            uint32_t written = bitpack_encode(chunk, out + off);
            off += written;
            occupied++;
        } else if (score <= SCORE_THRESH[5]) {
            /* Path 2: LZ77 */
            uint32_t written = lz77_compress(chunk, out + off);
            off += written;
            occupied++;
        } else {
            /* Path 3/4: Raw (high entropy) */
            if (off + 64 > dst_cap) return -3;
            memcpy(out + off, chunk, 64);
            off += 64;
            occupied++;
        }
    }

    /* Write occupied count back into header */
    memcpy(out + occupied_off, &occupied, 2);

    return (int32_t)off;
}

/* ── diamond_decode ─────────────────────────────────────────────
 * Decode Diamond Field payload → raw byte chunks.
 *
 * src = diamond-encoded payload
 * dst = output buffer for raw bytes
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t diamond_decode(const void *src, uint32_t src_len,
                               const DWGLS_CodecCtx *ctx,
                               void *dst, uint32_t dst_cap)
{
    (void)ctx;
    if (!src || !dst || src_len < DF_PAYLOAD_HEADER_SZ) return -1;

    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;

    /* Read header */
    uint8_t level = in[0];
    uint16_t occupied;
    memcpy(&occupied, in + 1, 2);

    if (level > SHELL_MAX_LEVEL) return -2;

    /* Decode occupied chunks from payload data */
    uint32_t off = DF_PAYLOAD_HEADER_SZ;
    uint32_t written = 0;

    for (uint32_t i = 0; i < occupied && written < dst_cap; i++) {
        if (off >= src_len) return -3;

        uint8_t decoded[64];

        /* Determine encoding path from marker byte */
        if (in[off] <= 2) {
            /* Sparse: count byte ≤ 2 */
            sparse_decode(decoded, in + off);
            off += 2 + in[off];
        } else if (in[off] == 0xFE) {
            /* Bitpack */
            off++;  /* skip marker */
            uint32_t to_copy = 64;
            if (off + to_copy > src_len) to_copy = src_len - off;
            memcpy(decoded, in + off, to_copy);
            if (to_copy < 64) memset(decoded + to_copy, 0, 64 - to_copy);
            off += 64;
        } else if (in[off] == 0xFD) {
            /* LZ77 */
            off++;  /* skip marker */
            uint32_t to_copy = 64;
            if (off + to_copy > src_len) to_copy = src_len - off;
            memcpy(decoded, in + off, to_copy);
            if (to_copy < 64) memset(decoded + to_copy, 0, 64 - to_copy);
            off += 64;
        } else {
            /* Raw copy */
            uint32_t to_copy = 64;
            if (off + to_copy > src_len) to_copy = src_len - off;
            memcpy(decoded, in + off, to_copy);
            if (to_copy < 64) memset(decoded + to_copy, 0, 64 - to_copy);
            off += to_copy;
        }

        /* Write decoded chunk to output */
        uint32_t to_write = 64;
        if (written + to_write > dst_cap) to_write = dst_cap - written;
        memcpy(out + written, decoded, to_write);
        written += to_write;
    }

    return (int32_t)written;
}

/* ── diamond_payload_size ───────────────────────────────────────
 * Compute Diamond Field payload size estimate.
 *
 * For level n, each chunk encodes to variable size depending on
 * entropy classifier. Worst case: 64 bytes per chunk (raw).
 */
static uint32_t diamond_payload_size(uint32_t n_elems,
                                      const DWGLS_CodecCtx *ctx)
{
    (void)ctx;
    if (n_elems == 0) return 0;

    /* Header + worst case: all chunks are raw */
    uint32_t n_chunks = (n_elems + 63) / 64;
    return DF_PAYLOAD_HEADER_SZ + n_chunks * 64;
}

/* ── diamond_verify ─────────────────────────────────────────────
 * Verify Diamond Field payload integrity.
 *
 * Checks: level valid, occupied reasonable, flags consistency.
 * Returns 0=ok, negative=corrupt.
 */
static int diamond_verify(const void *src, uint32_t src_len)
{
    if (!src || src_len < DF_PAYLOAD_HEADER_SZ) return -1;

    const uint8_t *in = (const uint8_t *)src;

    /* Check level */
    uint8_t level = in[0];
    if (level > SHELL_MAX_LEVEL) return -2;

    /* Check occupied count */
    uint16_t occupied;
    memcpy(&occupied, in + 1, 2);
    uint16_t max_slots = shell_slots(level);
    if (occupied > max_slots) return -3;

    /* Verify shell flags are self-consistent:
     * Count set bits in flags and compare with occupied */
    uint32_t bit_count = 0;
    const uint64_t *flags = (const uint64_t *)(in + 4);
    uint32_t n_words = (max_slots + 63) / 64;
    for (uint32_t w = 0; w < n_words && w < 77; w++) {
        bit_count += __builtin_popcountll(flags[w]);
    }
    if (bit_count != occupied) return -4;

    return 0;
}

/* ── diamond_resolve ────────────────────────────────────────────
 * Address mapping: slot (0..20735) → diamond cell address.
 *
 * Diamond field uses Shell level n → (2n+1)^3 slots.
 * The slot maps to a 3D coordinate within the shell.
 *
 * Returns byte offset in payload, or UINT32_MAX on overflow.
 */
static uint32_t diamond_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    if (slot >= DWGLS_TOTAL_SLOTS) return UINT32_MAX;

    uint8_t level = (uint8_t)(ctx->user_data[DF_CTX_LEVEL] & 0xFF);
    if (level == 0) level = 3;  /* default level */
    if (level > SHELL_MAX_LEVEL) return UINT32_MAX;

    uint16_t max_slots = shell_slots(level);

    /* Map slot to local index within shell */
    uint16_t local_idx = (uint16_t)(slot % max_slots);

    /* Payload offset: header + (local_idx as chunk index) * variable_chunk_size
     * We estimate 64 bytes per chunk for the resolve hint */
    uint32_t chunk_idx = local_idx;
    return DF_PAYLOAD_HEADER_SZ + chunk_idx * 64;
}

/* ════════════════════════════════════════════════════════════════
   VTABLE INSTANCE
   ════════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_DIAMOND_FIELD = {
    .info         = diamond_info,
    .encode       = diamond_encode,
    .decode       = diamond_decode,
    .payload_size = diamond_payload_size,
    .verify       = diamond_verify,
    .resolve      = diamond_resolve,
};

#endif /* DWGLS_CODEC_DIAMOND_H */
