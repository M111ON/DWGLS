/*
 * dwgls_codec_gcube.h — GCube Codec Adapter
 * ════════════════════════════════════════════════════════════════
 *
 * Wraps geo_cube_container.h (GCube format) into the DWGLS_Codec vtable.
 * Maps the GCubeFileHeader + TensorIndexEntry → DWGLS_Shell mapping.
 *
 * PAYLOAD LAYOUT (starts after 32-byte DWGLS_Shell):
 *   GCubeFileHeader   [64B]   — magic "GCB\0", version, tensor count
 *   GCubeTensorEntry  [80B]   — per-tensor metadata (name, dims, blocks)
 *   Block Data        [N×64B] — DiamondBlock payloads, zero-padded
 *   CRC-32            [4B]    — integrity over (header + index + blocks)
 *
 * CONVENTION for ctx->user_data:
 *   [0] = packed (n_dims << 8) | dtype
 *   [1] = dims[0]
 *   [2] = dims[1]
 *   [3] = dims[2]
 *   (dims[3] inferred: n_elems / (dims[0]*dims[1]*dims[2]))
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -fsyntax-only core/dwgls_codec_gcube.h
 * DEPENDS: dwgls_shell.h, dwgls_codec.h, geo_cube_container.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_GCUBE_H
#define DWGLS_CODEC_GCUBE_H

#include "dwgls_shell.h"
#include "dwgls_codec.h"
#include "geo_cube_container.h"
#include <string.h>

/* ════════════════════════════════════════════════════════════════
   OVERHEAD CONSTANTS
   ════════════════════════════════════════════════════════════════ */

/* Payload overhead: GCubeFileHeader(64) + GCubeTensorEntry(80) + CRC32(4) */
#define GCUBE_CODEC_OVERHEAD  (GCUBE_FILE_HDR_SZ + GCUBE_TENSOR_HDR_SZ + 4u)

/* ════════════════════════════════════════════════════════════════
   HELPER: Compute block count for data_size bytes
   ════════════════════════════════════════════════════════════════ */

static inline uint32_t dwgls_gcube_block_count(uint32_t data_size)
{
    uint32_t n = data_size / GCUBE_BLOCK_SZ;
    if (data_size % GCUBE_BLOCK_SZ) n++;
    if (n == 0) n = 1;
    return n;
}

/* ════════════════════════════════════════════════════════════════
   INFO
   ════════════════════════════════════════════════════════════════ */

static DWGLS_CodecInfo dwgls_gcube_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "gcube";
    info.codec_id    = CODEC_GCUBE;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_MULTI_TENSOR
                     | CODEC_FLAG_MMAP_FRIENDLY
                     | CODEC_FLAG_RANDOM_ACCESS;
    return info;
}

/* ════════════════════════════════════════════════════════════════
   ENCODE: raw weight bytes → GCube payload
   ════════════════════════════════════════════════════════════════
 *
 * Packs a single-tensor GCube binary into dst:
 *   [GCubeFileHeader 64B][GCubeTensorEntry 80B][blocks N*64B][CRC32 4B]
 *
 * src     = raw weight data (n_elems bytes)
 * n_elems = number of weight bytes
 * ctx     = codec context; user_data packs tensor metadata
 * dst     = output buffer (caller-allocated)
 * dst_cap = capacity of dst
 * Returns = total bytes written, or negative on error
 */
static int32_t dwgls_gcube_encode(const void *src, uint32_t n_elems,
                             const DWGLS_CodecCtx *ctx,
                             void *dst, uint32_t dst_cap)
{
    if (!src || !dst) return -1;
    if (n_elems == 0) return 0;

    /* Unpack tensor metadata from ctx->user_data */
    uint8_t  n_dims = (uint8_t)(ctx->user_data[0] & 0xFF);
    uint8_t  dtype  = (uint8_t)((ctx->user_data[0] >> 8) & 0xFF);
    if (n_dims == 0) n_dims = 1;

    uint32_t n_blocks = dwgls_gcube_block_count(n_elems);
    uint32_t blk_bytes = n_blocks * GCUBE_BLOCK_SZ;

    /* Total payload = FileHeader(64) + TensorEntry(80) + blocks + CRC(4) */
    uint32_t total = GCUBE_CODEC_OVERHEAD + blk_bytes;

    if (total > dst_cap) return -2;

    uint8_t *out = (uint8_t *)dst;
    uint32_t off = 0;

    /* ── GCubeFileHeader (64B) ──────────────────────────────── */
    GCubeFileHeader *hdr = (GCubeFileHeader *)(out + off);
    memset(hdr, 0, GCUBE_FILE_HDR_SZ);
    memcpy(hdr->magic, GCUBE_MAGIC, 4);
    hdr->version      = GCUBE_VERSION;
    hdr->flags        = 0;
    hdr->n_tensors    = 1;
    hdr->total_blocks = n_blocks;
    hdr->total_weights = n_elems;
    /* model_name left as zeros (empty string) */
    off += GCUBE_FILE_HDR_SZ;

    /* ── GCubeTensorEntry (80B) ─────────────────────────────── */
    GCubeTensorEntry *ent = (GCubeTensorEntry *)(out + off);
    memset(ent, 0, GCUBE_TENSOR_HDR_SZ);
    /* name: "tensor_0" */
    strncpy(ent->name, "tensor_0", GCUBE_MAX_NAME - 1);
    ent->n_dims    = n_dims;
    ent->dtype     = dtype;
    ent->n_elems   = n_elems;
    ent->data_size = n_elems;
    ent->block_start = 0;
    ent->block_count = n_blocks;

    /* Fill dims from ctx->user_data[1..3] + infer dims[3] */
    ent->dims[0] = ctx->user_data[1] ? ctx->user_data[1] : n_elems;
    ent->dims[1] = ctx->user_data[2];
    ent->dims[2] = ctx->user_data[3];
    if (n_dims >= 4 && ent->dims[0] > 0 && ent->dims[1] > 0 && ent->dims[2] > 0) {
        ent->dims[3] = n_elems / (ent->dims[0] * ent->dims[1] * ent->dims[2]);
    }
    off += GCUBE_TENSOR_HDR_SZ;

    /* ── Block Data (n_blocks × 64B) ───────────────────────── */
    memcpy(out + off, src, n_elems);
    /* Zero-pad last block if needed */
    uint32_t valid = n_elems % GCUBE_BLOCK_SZ;
    if (valid > 0) {
        memset(out + off + n_elems, 0, GCUBE_BLOCK_SZ - valid);
    }
    off += blk_bytes;

    /* ── CRC-32 (4B) ───────────────────────────────────────── */
    uint32_t crc = gcube_crc32(out, off);  /* CRC over header+index+blocks */
    memcpy(out + off, &crc, 4);
    off += 4;

    return (int32_t)off;
}

/* ════════════════════════════════════════════════════════════════
   DECODE: GCube payload → raw weight bytes
   ════════════════════════════════════════════════════════════════
 *
 * Parses GCube payload and extracts the first tensor's block data.
 * Verifies CRC-32 integrity before returning data.
 *
 * src     = GCube payload (header + index + blocks + CRC)
 * src_len = payload bytes
 * ctx     = codec context (unused, for interface compliance)
 * dst     = output buffer for raw weight bytes
 * dst_cap = capacity of dst
 * Returns = bytes written to dst, or negative on error
 */
static int32_t dwgls_gcube_decode(const void *src, uint32_t src_len,
                             const DWGLS_CodecCtx *ctx,
                             void *dst, uint32_t dst_cap)
{
    (void)ctx;
    if (!src || !dst) return -1;
    if (src_len < GCUBE_CODEC_OVERHEAD) return -3;  /* too small for header+CRC */

    const uint8_t *in = (const uint8_t *)src;

    /* ── Parse GCubeFileHeader ──────────────────────────────── */
    const GCubeFileHeader *hdr = (const GCubeFileHeader *)in;
    if (memcmp(hdr->magic, GCUBE_MAGIC, 4) != 0) return -4;
    if (hdr->version != GCUBE_VERSION) return -5;
    if (hdr->n_tensors == 0) return -6;

    /* ── CRC-32 verification ────────────────────────────────── */
    uint32_t data_len = src_len - 4;  /* everything except the CRC */
    uint32_t computed_crc = gcube_crc32(in, data_len);
    uint32_t stored_crc;
    memcpy(&stored_crc, in + data_len, 4);
    if (computed_crc != stored_crc) return -7;

    /* ── Parse first tensor entry ───────────────────────────── */
    uint32_t idx_off = GCUBE_FILE_HDR_SZ;
    if (src_len < idx_off + GCUBE_TENSOR_HDR_SZ) return -8;

    const GCubeTensorEntry *ent = (const GCubeTensorEntry *)(in + idx_off);

    /* ── Extract block data ─────────────────────────────────── */
    uint32_t blk_off = GCUBE_FILE_HDR_SZ + GCUBE_TENSOR_HDR_SZ;
    uint32_t blk_bytes = ent->block_count * GCUBE_BLOCK_SZ;

    if (src_len < blk_off + blk_bytes) return -9;
    if (ent->data_size > dst_cap) return -10;

    memcpy(dst, in + blk_off, ent->data_size);
    return (int32_t)ent->data_size;
}

/* ════════════════════════════════════════════════════════════════
   PAYLOAD SIZE: compute GCube payload size without encoding
   ════════════════════════════════════════════════════════════════
 *
 * Returns the exact payload size in bytes for n_elems of raw data.
 * Payload = GCubeFileHeader(64) + TensorEntry(80) + blocks + CRC(4)
 */
static uint32_t dwgls_gcube_payload_size(uint32_t n_elems,
                                    const DWGLS_CodecCtx *ctx)
{
    (void)ctx;
    if (n_elems == 0) return 0;
    uint32_t n_blocks = dwgls_gcube_block_count(n_elems);
    return GCUBE_CODEC_OVERHEAD + n_blocks * GCUBE_BLOCK_SZ;
}

/* ════════════════════════════════════════════════════════════════
   VERIFY: check payload integrity (CRC-32)
   ════════════════════════════════════════════════════════════════
 *
 * Validates GCube magic, version, and CRC-32 checksum.
 * Returns 0=ok, negative=corrupt/invalid.
 */
static int dwgls_gcube_verify(const void *src, uint32_t src_len)
{
    if (!src) return -1;
    if (src_len < GCUBE_CODEC_OVERHEAD) return -2;

    const uint8_t *in = (const uint8_t *)src;

    /* Magic check */
    if (memcmp(in, GCUBE_MAGIC, 4) != 0) return -3;

    /* Version check */
    const GCubeFileHeader *hdr = (const GCubeFileHeader *)in;
    if (hdr->version != GCUBE_VERSION) return -4;

    /* CRC-32 check */
    uint32_t data_len = src_len - 4;
    uint32_t computed = gcube_crc32(in, data_len);
    uint32_t stored;
    memcpy(&stored, in + data_len, 4);
    return (computed == stored) ? 0 : -5;
}

/* ════════════════════════════════════════════════════════════════
   RESOLVE: address mapping (slot → payload byte offset)
   ════════════════════════════════════════════════════════════════
 *
 * Maps a logical slot index (0..n_elems-1) to the byte offset
 * within the GCube payload where that weight resides.
 *
 * The block data begins at offset GCUBE_FILE_HDR_SZ + GCUBE_TENSOR_HDR_SZ
 * within the payload. Each slot maps linearly into the block region.
 *
 * slot     = logical weight index
 * ctx      = codec context (n_elems can be derived from ctx->user_data)
 * Returns  = byte offset from payload start, or UINT32_MAX if out of range
 */
static uint32_t dwgls_gcube_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    /* Block data starts after header + tensor index in the payload */
    uint32_t data_base = GCUBE_FILE_HDR_SZ + GCUBE_TENSOR_HDR_SZ;

    /* Bounds check: slot must be within n_elems (use total_slots as proxy) */
    uint32_t n_elems = ctx->total_slots;
    if (n_elems == 0) n_elems = DWGLS_TOTAL_SLOTS;  /* sacred default */
    if (slot >= n_elems) return UINT32_MAX;

    /* Linear address within block data region */
    return data_base + slot;
}

/* ════════════════════════════════════════════════════════════════
   VTABLE INSTANCE
   ════════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_GCUBE = {
    .info         = dwgls_gcube_info,
    .encode       = dwgls_gcube_encode,
    .decode       = dwgls_gcube_decode,
    .payload_size = dwgls_gcube_payload_size,
    .verify       = dwgls_gcube_verify,
    .resolve      = dwgls_gcube_resolve,
};

#endif /* DWGLS_CODEC_GCUBE_H */
