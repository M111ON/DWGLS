/*
 * dwgls_codec_tess_oct.h — Tesseract Octant Codec Adapter for DWGLS Vtable
 * ════════════════════════════════════════════════════════════════
 *
 * Wraps tesseract_container.h (4D octant format via Cayley transform)
 * into the DWGLS_Codec vtable interface.
 *
 * Mapping: TessHeader fields → DWGLS_Shell fields
 *   TessHeader.n_cubes        → shell.total_slots / HYP_AXIS_SLOTS
 *   TessHeader.scale_factor   → shell.scale_factor
 *   TessHeader.formula        → ctx.user_data[0]
 *   TessHeader.checksum       → CRC-32 integrity
 *
 * resolve(): slot → octant + offset.
 *   axis = slot / HYP_AXIS_SLOTS (0=X, 1=Y, 2=Z)
 *   local = slot % HYP_AXIS_SLOTS
 *   Formula: LINEAR=direct, CAYLEY=hyperbolic transform, SPIRAL=angle rotation
 *   Returns resolved data address via tess_resolve().
 *
 * Layout: 8 octants from sign combinations of 3 KIS axes.
 *   octant(s) = sign_x | (sign_y << 1) | (sign_z << 2)
 *
 * Payload layout (after DWGLS_Shell):
 *   TessHeader        [32B]   — magic "TES4", octant config, CRC-32
 *   address_map       [N*4B]  — slot → resolved data address
 *   data              [N*1B]  — weight data (N = n_cubes × HYP_AXIS_SLOTS)
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -fsyntax-only core/dwgls_codec_tess_oct.h
 * DEPENDS: dwgls_shell.h, dwgls_codec.h, tesseract_container.h,
 *          hyperbolic_seek.h, geo_kis_projection.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_TESS_OCT_H
#define DWGLS_CODEC_TESS_OCT_H

#include "dwgls_shell.h"
#include "dwgls_codec.h"
#include "tesseract_container.h"
#include <string.h>
#include <math.h>

/* ════════════════════════════════════════════════════════════════
   OVERHEAD CONSTANTS
   ════════════════════════════════════════════════════════════════ */

/* TessHeader is 32B packed */
#define TESS_OCT_CODEC_HEADER_SZ  32u

/* ctx->user_data indices for Tesseract metadata */
#define TESS_OCT_CTX_FORMULA      0u  /* 0=LINEAR, 1=CAYLEY, 2=SPIRAL */
#define TESS_OCT_CTX_N_CUBES      1u  /* number of octants/cubes (0 = default 8) */
#define TESS_OCT_CTX_RESERVED0    2u
#define TESS_OCT_CTX_RESERVED1    3u

/* ════════════════════════════════════════════════════════════════
   CODEC VTABLE IMPLEMENTATION
   ════════════════════════════════════════════════════════════════ */

/* ── tess_oct_info ───────────────────────────────────────────────
 * Return codec metadata.
 */
static DWGLS_CodecInfo dwgls_tess_oct_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "tesseract_oct";
    info.codec_id    = CODEC_TESSERACT;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_RANDOM_ACCESS
                     | CODEC_FLAG_COMPRESSED
                     | CODEC_FLAG_DERIVED_VIEWS;
    return info;
}

/* ── tess_oct_encode ─────────────────────────────────────────────
 * Encode raw weight bytes → Tesseract octant payload.
 *
 * Builds the address_map via tess_resolve, stores weight data.
 *
 * dst layout:
 *   [TessHeader 32B][address_map N*4B][data N*1B]
 *
 * N = n_cubes × HYP_AXIS_SLOTS (default: 8 × 6912 = 55296)
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t dwgls_tess_oct_encode(const void *src, uint32_t n_elems,
                                     const DWGLS_CodecCtx *ctx,
                                     void *dst, uint32_t dst_cap)
{
    if (!src || !dst || n_elems == 0) return -1;

    /* Derive formula and n_cubes from user_data */
    uint32_t formula = ctx->user_data[TESS_OCT_CTX_FORMULA];
    uint32_t n_cubes = ctx->user_data[TESS_OCT_CTX_N_CUBES];
    if (n_cubes == 0) n_cubes = TESS_N_OCTANTS;  /* default: 8 octants */

    uint32_t total_slots = n_cubes * HYP_AXIS_SLOTS;
    if (n_elems > total_slots) n_elems = total_slots;

    /* Build header */
    TessHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic        = TESS_MAGIC;
    hdr.version      = TESS_VERSION;
    hdr.n_cubes      = n_cubes;
    hdr.scale_factor = ctx->scale_factor;
    hdr.formula      = formula;
    hdr.checksum     = 0;
    hdr.reserved[0]  = 0;
    hdr.reserved[1]  = 0;

    /* Compute address map via tess_resolve */
    uint32_t *address_map = (uint32_t *)malloc(total_slots * sizeof(uint32_t));
    if (!address_map) return -1;

    for (uint32_t i = 0; i < total_slots; i++) {
        uint32_t local = i % HYP_AXIS_SLOTS;
        address_map[i] = tess_resolve(local, formula, ctx->scale_factor)
                       + (i / HYP_AXIS_SLOTS) * HYP_AXIS_SLOTS;
    }

    /* Compute CRC-32 checksum over input data */
    const uint8_t *input = (const uint8_t *)src;
    hdr.checksum = tess_crc32(input, n_elems);

    /* Compute total payload size */
    uint32_t payload_sz = TESS_OCT_CODEC_HEADER_SZ
                        + total_slots * sizeof(uint32_t)
                        + total_slots * sizeof(uint8_t);
    if (dst_cap < payload_sz) {
        free(address_map);
        return -1;
    }

    /* Pack into dst */
    uint8_t *out = (uint8_t *)dst;
    uint32_t off = 0;

    memcpy(out + off, &hdr, TESS_OCT_CODEC_HEADER_SZ);
    off += TESS_OCT_CODEC_HEADER_SZ;

    memcpy(out + off, address_map, total_slots * sizeof(uint32_t));
    off += total_slots * sizeof(uint32_t);

    /* Copy weight data through address map (encode step) */
    uint8_t *data_out = out + off;
    memset(data_out, 0, total_slots * sizeof(uint8_t));
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t addr = address_map[i] % total_slots;
        data_out[addr] = input[i];
    }
    off += total_slots * sizeof(uint8_t);

    free(address_map);
    return (int32_t)off;
}

/* ── tess_oct_decode ─────────────────────────────────────────────
 * Decode Tesseract octant payload → raw weight bytes.
 *
 * src layout:
 *   [TessHeader 32B][address_map N*4B][data N*1B]
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t dwgls_tess_oct_decode(const void *src, uint32_t src_len,
                                     const DWGLS_CodecCtx *ctx,
                                     void *dst, uint32_t dst_cap)
{
    (void)ctx;
    if (!src || !dst || src_len < TESS_OCT_CODEC_HEADER_SZ) return -1;

    const uint8_t *in = (const uint8_t *)src;
    TessHeader hdr;
    memcpy(&hdr, in, TESS_OCT_CODEC_HEADER_SZ);

    if (hdr.magic != TESS_MAGIC) return -1;

    uint32_t total_slots = hdr.n_cubes * HYP_AXIS_SLOTS;
    uint32_t map_bytes = total_slots * sizeof(uint32_t);
    uint32_t data_bytes = total_slots * sizeof(uint8_t);

    if (src_len < TESS_OCT_CODEC_HEADER_SZ + map_bytes + data_bytes)
        return -1;

    const uint32_t *addr_map = (const uint32_t *)(in + TESS_OCT_CODEC_HEADER_SZ);
    const uint8_t *data = in + TESS_OCT_CODEC_HEADER_SZ + map_bytes;

    uint32_t max_out = dst_cap / sizeof(uint8_t);
    uint8_t *out = (uint8_t *)dst;

    /* Reconstruct: for each slot, look up data via address map */
    for (uint32_t s = 0; s < total_slots && s < max_out; s++) {
        uint32_t addr = addr_map[s] % total_slots;
        out[s] = data[addr];
    }

    return (int32_t)(total_slots < max_out ? total_slots : max_out);
}

/* ── tess_oct_payload_size ───────────────────────────────────────
 * Compute Tesseract octant payload size without encoding.
 *
 * Returns header(32) + address_map(N*4) + data(N*1).
 * N = n_cubes × HYP_AXIS_SLOTS.
 */
static uint32_t dwgls_tess_oct_payload_size(uint32_t n_elems,
                                             const DWGLS_CodecCtx *ctx)
{
    (void)n_elems;
    uint32_t n_cubes = ctx->user_data[TESS_OCT_CTX_N_CUBES];
    if (n_cubes == 0) n_cubes = TESS_N_OCTANTS;

    uint32_t total_slots = n_cubes * HYP_AXIS_SLOTS;

    return TESS_OCT_CODEC_HEADER_SZ
         + total_slots * sizeof(uint32_t)
         + total_slots * sizeof(uint8_t);
}

/* ── tess_oct_verify ─────────────────────────────────────────────
 * Verify Tesseract octant payload integrity.
 *
 * Checks: magic, n_cubes, CRC-32.
 *
 * Returns 0=ok, negative=corrupt.
 */
static int dwgls_tess_oct_verify(const void *src, uint32_t src_len)
{
    if (!src || src_len < TESS_OCT_CODEC_HEADER_SZ) return -1;

    const uint8_t *in = (const uint8_t *)src;
    TessHeader hdr;
    memcpy(&hdr, in, TESS_OCT_CODEC_HEADER_SZ);

    /* Magic check */
    if (hdr.magic != TESS_MAGIC) return -2;

    /* n_cubes must be at most 8 (8 octants in 4D tesseract) */
    if (hdr.n_cubes == 0 || hdr.n_cubes > TESS_N_OCTANTS) return -3;

    /* Formula must be valid */
    if (hdr.formula > TESS_FORMULA_SPIRAL) return -4;

    uint32_t total_slots = hdr.n_cubes * HYP_AXIS_SLOTS;

    /* Minimum payload: header + full address map + data */
    uint32_t min_payload = TESS_OCT_CODEC_HEADER_SZ
                         + total_slots * sizeof(uint32_t)
                         + total_slots * sizeof(uint8_t);
    if (src_len < min_payload) return -5;

    /* CRC-32 integrity check */
    const uint8_t *data = in + TESS_OCT_CODEC_HEADER_SZ
                        + total_slots * sizeof(uint32_t);
    uint32_t computed = tess_crc32(data, total_slots);
    if (computed != hdr.checksum) return -6;

    return 0;
}

/* ── tess_oct_resolve ────────────────────────────────────────────
 * Address mapping: slot (0..20735) → resolved data address.
 *
 * Core geometric mapping for tesseract octants:
 *   1. Determine octant from slot (sign bits on 3 axes)
 *   2. Apply formula (LINEAR/CAYLEY/SPIRAL) to map within octant
 *   3. Return resolved address in data payload space
 *
 * Returns resolved address, or UINT32_MAX on error.
 */
static uint32_t dwgls_tess_oct_resolve(uint32_t slot,
                                       const DWGLS_CodecCtx *ctx)
{
    if (slot >= HYP_KIS_SLOTS) return UINT32_MAX;

    uint32_t formula = ctx->user_data[TESS_OCT_CTX_FORMULA];

    /* tess_resolve handles the formula-based address mapping */
    return tess_resolve(slot, formula, ctx->scale_factor);
}

/* ════════════════════════════════════════════════════════════════
   VTABLE INSTANCE
   ════════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_TESSERACT = {
    .info         = dwgls_tess_oct_info,
    .encode       = dwgls_tess_oct_encode,
    .decode       = dwgls_tess_oct_decode,
    .payload_size = dwgls_tess_oct_payload_size,
    .verify       = dwgls_tess_oct_verify,
    .resolve      = dwgls_tess_oct_resolve,
};

#endif /* DWGLS_CODEC_TESS_OCT_H */
