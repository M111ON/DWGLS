/* ═══════════════════════════════════════════════════════════════════════════
 * codec_tess.h — TESS Codec (stride-37, 8-octant, .tess format)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Wraps the existing geo_tess_container.h logic as a DWGLS codec.
 * This is the PRIMARY codec — the one .tess files use.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef CODEC_TESS_H
#define CODEC_TESS_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "dwgls_codec.h"
#include "geo_tess_container.h"

/* ═══════════════════════════════════════════════════════════════
   TESS CODEC INFO
   ═══════════════════════════════════════════════════════════════ */

static inline DWGLS_CodecInfo tess_info(void)
{
    return (DWGLS_CodecInfo){
        .name = "tess",
        .codec_id = CODEC_TESS,
        .min_version = 1,
        .flags = CODEC_FLAG_MMAP_FRIENDLY | CODEC_FLAG_RANDOM_ACCESS
               | CODEC_FLAG_DERIVED_VIEWS,
    };
}

/* ═══════════════════════════════════════════════════════════════
   TESS ENCODE: raw weights → TESS payload (Header + Formula + CubeData)
   ═══════════════════════════════════════════════════════════════ */

static inline int32_t tess_encode(const void *src, uint32_t n_elems,
                                   const DWGLS_CodecCtx *ctx,
                                   void *dst, uint32_t dst_cap)
{
    if (!src || !dst || n_elems == 0) return -1;

    uint32_t cell_size = ctx ? ctx->user_data[0] : TESS_CELL_F32;
    uint32_t total_slots = ctx ? ctx->total_slots : TESS_TOTAL_SLOTS;
    uint32_t scale_factor = ctx ? ctx->scale_factor : 65536u;

    /* TESS always stores 20736 cells regardless of input size */
    uint32_t cube_bytes = total_slots * cell_size;
    uint32_t payload_size = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;

    if (dst_cap < payload_size) return -1;

    uint8_t *p = (uint8_t *)dst;

    /* ── 1. Write TESS_Header ── */
    TESS_Header hdr;
    tess_header_init(&hdr, ctx ? ctx->user_data[1] : TESS_GGML_F32, cell_size);
    hdr.scale_factor = scale_factor;
    hdr.x_slots = ctx ? ctx->x_slots : TESS_X_SLOTS;
    hdr.y_slots = ctx ? ctx->y_slots : TESS_Y_SLOTS;
    hdr.z_slots = ctx ? ctx->z_slots : TESS_Z_SLOTS;

    memcpy(p, &hdr, TESS_HEADER_SIZE);
    p += TESS_HEADER_SIZE;

    /* ── 2. Write TESS_Formula ── */
    TESS_Formula fml;
    tess_formula_init(&fml);
    fml.mirror_axis_x = hdr.x_slots;
    fml.mirror_axis_y = hdr.y_slots;
    fml.mirror_axis_z = hdr.z_slots;
    fml.stride_seed = TESS_STRIDE_37;

    memcpy(p, &fml, TESS_FORMULA_SIZE);
    p += TESS_FORMULA_SIZE;

    /* ── 3. Scatter raw data through stride-37 into CubeData ── */
    uint8_t *cube_data = p;
    memset(cube_data, 0, cube_bytes);

    const uint8_t *src_bytes = (const uint8_t *)src;
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = tess_stride_scatter(i);
        if (slot >= total_slots) slot = i % total_slots;
        uint32_t dst_off = slot * cell_size;
        if (dst_off + cell_size <= cube_bytes) {
            memcpy(cube_data + dst_off, src_bytes + i * cell_size, cell_size);
        }
    }
    p += cube_bytes;

    /* ── 4. Compute and append CRC64 over CubeData ── */
    uint64_t cube_crc = tess_crc64(cube_data, cube_bytes);
    memcpy(p, &cube_crc, TESS_CRC_SIZE);
    p += TESS_CRC_SIZE;

    /* Update header with cube checksum */
    ((TESS_Header *)dst)->cube_checksum = cube_crc;

    return (int32_t)(p - (uint8_t *)dst);
}

/* ═══════════════════════════════════════════════════════════════
   TESS DECODE: TESS payload → raw weights
   ═══════════════════════════════════════════════════════════════ */

static inline int32_t tess_decode(const void *src, uint32_t src_len,
                                   const DWGLS_CodecCtx *ctx,
                                   void *dst, uint32_t dst_cap)
{
    if (!src || src_len < TESS_HEADER_SIZE + TESS_FORMULA_SIZE + TESS_CRC_SIZE) return -1;
    if (!dst) return -1;

    const uint8_t *p = (const uint8_t *)src;

    /* Read header */
    const TESS_Header *hdr = (const TESS_Header *)p;
    p += TESS_HEADER_SIZE;

    int v = tess_header_validate(hdr);
    if (v != 0) return v;

    /* Read formula (optional but expected) */
    p += TESS_FORMULA_SIZE;

    uint32_t cell_size = hdr->cell_size;
    uint32_t total_slots = hdr->total_slots;
    uint32_t cube_bytes = total_slots * cell_size;

    if (src_len < TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE)
        return -2;

    const uint8_t *cube_data = p;
    p += cube_bytes;

    /* Verify CRC64 */
    uint64_t stored_crc;
    memcpy(&stored_crc, p, TESS_CRC_SIZE);
    uint64_t computed_crc = tess_crc64(cube_data, cube_bytes);
    if (computed_crc != stored_crc) return -3;

    /* Gather: read from same scatter positions used during encode */
    uint32_t n_elems = (ctx && ctx->user_data[2]) ? ctx->user_data[2] : 0;
    uint32_t n_out = n_elems ? n_elems : (dst_cap / cell_size);
    uint8_t *dst_bytes = (uint8_t *)dst;

    for (uint32_t i = 0; i < n_out; i++) {
        uint32_t slot = tess_stride_scatter(i);
        if (slot >= total_slots) slot = i % total_slots;
        uint32_t src_off = slot * cell_size;
        if (src_off + cell_size <= cube_bytes) {
            memcpy(dst_bytes + i * cell_size, cube_data + src_off, cell_size);
        }
    }

    return (int32_t)(n_out * cell_size);
}

/* ═══════════════════════════════════════════════════════════════
   TESS PAYLOAD SIZE
   ═══════════════════════════════════════════════════════════════ */

static inline uint32_t tess_payload_size(uint32_t n_elems,
                                          const DWGLS_CodecCtx *ctx)
{
    (void)n_elems;  /* TESS always stores full 20736 cells */
    uint32_t cell_size = ctx ? ctx->user_data[0] : TESS_CELL_F32;
    uint32_t total_slots = ctx ? ctx->total_slots : TESS_TOTAL_SLOTS;
    return TESS_HEADER_SIZE + TESS_FORMULA_SIZE + (total_slots * cell_size) + TESS_CRC_SIZE;
}

/* ═══════════════════════════════════════════════════════════════
   TESS VERIFY
   ════════════════════════════════════════════════════════════════ */

static inline int tess_verify(const void *src, uint32_t src_len)
{
    if (!src || src_len < TESS_HEADER_SIZE + TESS_FORMULA_SIZE + TESS_CRC_SIZE) return -1;

    const TESS_Header *hdr = (const TESS_Header *)src;
    int v = tess_header_validate(hdr);
    if (v != 0) return v;

    uint32_t cell_size = hdr->cell_size;
    uint32_t total_slots = hdr->total_slots;
    uint32_t cube_bytes = total_slots * cell_size;
    uint32_t expected = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes + TESS_CRC_SIZE;

    if (src_len < expected) return -2;

    const uint8_t *cube_data = (const uint8_t *)src + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
    uint64_t stored_crc;
    memcpy(&stored_crc, (const uint8_t *)src + TESS_HEADER_SIZE + TESS_FORMULA_SIZE + cube_bytes, TESS_CRC_SIZE);
    uint64_t computed_crc = tess_crc64(cube_data, cube_bytes);

    return (computed_crc == stored_crc) ? 0 : -3;
}

/* ═══════════════════════════════════════════════════════════════
   TESS RESOLVE: slot → payload address (stride-37 scatter)
   ═══════════════════════════════════════════════════════════════ */

static inline uint32_t codec_tess_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    (void)ctx;
    return tess_stride_scatter(slot);
}

/* ═══════════════════════════════════════════════════════════════
   TESS CODEC VTABLE
   ═══════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_TESS = {
    .info         = tess_info,
    .encode       = tess_encode,
    .decode       = tess_decode,
    .payload_size = tess_payload_size,
    .verify       = tess_verify,
    .resolve      = codec_tess_resolve,
};

#endif /* CODEC_TESS_H */