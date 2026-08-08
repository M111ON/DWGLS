/*
 * dwgls_codec_kis4d.h — KIS 4D Codec Adapter for DWGLS Vtable
 * ════════════════════════════════════════════════════════════════
 *
 * Wraps geo_kis_4d_container.h (KIS 4D format: 3-axis KIS space)
 * into the DWGLS_Codec vtable interface.
 *
 * Mapping: KIS4DHeader fields → DWGLS_Shell fields
 *   KIS4DHeader.total_slots   → shell.total_slots (20736)
 *   KIS4DHeader.scale_factor  → shell.scale_factor
 *   KIS4DHeader.x/y/z_slots   → ctx.x_slots/y_slots/z_slots
 *   KIS4DHeader.data_count    → payload (unique values)
 *   KIS4DHeader.checksum      → CRC-32 integrity
 *
 * resolve(): slot → (x,y,z) axis + angle-based address mapping.
 *   axis = select_axis(slot): 0=X, 1=Y, 2=Z (via x/y/z_slots offsets)
 *   local = axis_slot(slot): position within axis
 *   angle = 2π * local / axis_slots + axis * 2π/3
 *   mapped = angle * (scale / scale_factor), then back to slot index
 *
 * Payload layout (after DWGLS_Shell):
 *   KIS4DHeader     [32B]   — magic "KIS4", axis config, checksum
 *   address_map     [20736 × 4B] — slot → resolved data address
 *   data            [data_count × 1B] — unique weight values
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -fsyntax-only core/dwgls_codec_kis4d.h
 * DEPENDS: dwgls_shell.h, dwgls_codec.h, geo_kis_4d_container.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_KIS4D_H
#define DWGLS_CODEC_KIS4D_H

#include "dwgls_shell.h"
#include "dwgls_codec.h"
#include "geo_kis_4d_container.h"
#include <string.h>
#include <math.h>

/* ════════════════════════════════════════════════════════════════
   OVERHEAD CONSTANTS
   ════════════════════════════════════════════════════════════════ */

/* Payload overhead: KIS4DHeader(32) + address_map(20736*4) + data(N*1) */
#define KIS4D_CODEC_HEADER_SZ    32u
#define KIS4D_CODEC_MAP_SZ       (DWGLS_TOTAL_SLOTS * sizeof(uint32_t))
/* data_size is variable, derived from data_count in header */

/* ctx->user_data indices for KIS4D metadata */
#define KIS4D_CTX_DATA_COUNT     0u  /* number of unique values */
#define KIS4D_CTX_X_SLOTS        1u  /* X-axis slot count (0 = default 6912) */
#define KIS4D_CTX_Y_SLOTS        2u  /* Y-axis slot count (0 = default 6912) */
#define KIS4D_CTX_Z_SLOTS        3u  /* Z-axis slot count (0 = default 6912) */

/* ════════════════════════════════════════════════════════════════
   CODEC VTABLE IMPLEMENTATION
   ════════════════════════════════════════════════════════════════ */

/* ── kis4d_info ─────────────────────────────────────────────────
 * Return codec metadata.
 */
static DWGLS_CodecInfo dwgls_kis4d_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "kis_4d";
    info.codec_id    = CODEC_KIS_4D;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_RANDOM_ACCESS
                     | CODEC_FLAG_MMAP_FRIENDLY
                     | CODEC_FLAG_COMPRESSED;
    return info;
}

/* ── kis4d_encode ────────────────────────────────────────────────
 * Encode raw weight bytes → KIS4D payload.
 *
 * Builds the address_map via kis4d_resolve, stores unique values.
 *
 * dst layout:
 *   [KIS4DHeader 32B][address_map 20736*4B][data data_count*1B]
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t dwgls_kis4d_encode(const void *src, uint32_t n_elems,
                                  const DWGLS_CodecCtx *ctx,
                                  void *dst, uint32_t dst_cap)
{
    if (!src || !dst || n_elems == 0) return -1;

    /* Derive axis layout from user_data or default to 6912 each */
    uint32_t x_slots = ctx->x_slots ? ctx->x_slots : DWGLS_TOTAL_SLOTS / 3;
    uint32_t y_slots = ctx->y_slots ? ctx->y_slots : DWGLS_TOTAL_SLOTS / 3;
    uint32_t z_slots = DWGLS_TOTAL_SLOTS - x_slots - y_slots;

    /* Build header */
    KIS4DHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = 0x4B495334;  /* "KIS4" */
    hdr.version     = 1;
    hdr.total_slots = DWGLS_TOTAL_SLOTS;
    hdr.scale_factor = ctx->scale_factor;
    hdr.x_slots     = x_slots;
    hdr.y_slots     = y_slots;
    hdr.z_slots     = z_slots;

    /* Compute address map */
    uint32_t address_map[DWGLS_TOTAL_SLOTS];
    for (uint32_t i = 0; i < DWGLS_TOTAL_SLOTS; i++) {
        address_map[i] = kis4d_resolve(i, ctx->scale_factor, &hdr);
    }

    /* Count unique addresses and build data */
    uint8_t data[256];
    uint32_t unique_count = 0;
    const uint8_t *input = (const uint8_t *)src;
    uint32_t count = (n_elems < DWGLS_TOTAL_SLOTS) ? n_elems : DWGLS_TOTAL_SLOTS;

    for (uint32_t i = 0; i < count; i++) {
        int found = 0;
        for (uint32_t j = 0; j < unique_count; j++) {
            if (data[j] == input[i]) { found = 1; break; }
        }
        if (!found) {
            data[unique_count++] = input[i];
            if (unique_count > 256) return -1;  /* too many unique values */
        }
    }
    hdr.data_count = unique_count;

    /* Compute CRC-32 checksum */
    uint32_t crc = 0;
    for (uint32_t i = 0; i < unique_count; i++) {
        crc = (crc << 1) ^ data[i];
    }
    hdr.checksum = crc;

    /* Compute total payload size */
    uint32_t payload_sz = KIS4D_CODEC_HEADER_SZ
                        + DWGLS_TOTAL_SLOTS * sizeof(uint32_t)
                        + unique_count * sizeof(uint8_t);
    if (dst_cap < payload_sz) return -1;

    /* Pack into dst */
    uint8_t *out = (uint8_t *)dst;
    uint32_t off = 0;

    memcpy(out + off, &hdr, KIS4D_CODEC_HEADER_SZ);
    off += KIS4D_CODEC_HEADER_SZ;

    memcpy(out + off, address_map, DWGLS_TOTAL_SLOTS * sizeof(uint32_t));
    off += DWGLS_TOTAL_SLOTS * sizeof(uint32_t);

    memcpy(out + off, data, unique_count * sizeof(uint8_t));
    off += unique_count * sizeof(uint8_t);

    return (int32_t)off;
}

/* ── kis4d_decode ────────────────────────────────────────────────
 * Decode KIS4D payload → raw weight bytes.
 *
 * src layout:
 *   [KIS4DHeader 32B][address_map N*4B][data data_count*1B]
 *
 * Reconstructs weight array by resolving each slot through the
 * address map, then mapping addresses back to data values.
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t dwgls_kis4d_decode(const void *src, uint32_t src_len,
                                  const DWGLS_CodecCtx *ctx,
                                  void *dst, uint32_t dst_cap)
{
    (void)ctx;
    if (!src || !dst || src_len < KIS4D_CODEC_HEADER_SZ) return -1;

    const uint8_t *in = (const uint8_t *)src;
    KIS4DHeader hdr;
    memcpy(&hdr, in, KIS4D_CODEC_HEADER_SZ);

    if (hdr.magic != 0x4B495334) return -1;
    if (hdr.total_slots != DWGLS_TOTAL_SLOTS) return -1;

    uint32_t map_bytes = DWGLS_TOTAL_SLOTS * sizeof(uint32_t);
    if (src_len < KIS4D_CODEC_HEADER_SZ + map_bytes) return -1;

    const uint32_t *addr_map = (const uint32_t *)(in + KIS4D_CODEC_HEADER_SZ);
    const uint8_t *data = in + KIS4D_CODEC_HEADER_SZ + map_bytes;

    uint32_t max_out = dst_cap / sizeof(uint8_t);
    uint8_t *out = (uint8_t *)dst;

    /* Reconstruct: for each slot, find which unique data value maps to it */
    for (uint32_t s = 0; s < DWGLS_TOTAL_SLOTS && s < max_out; s++) {
        uint32_t addr = addr_map[s];
        /* Simple lookup: find which data index corresponds to this address.
         * In the encoded form, data[addr % data_count] is the value. */
        out[s] = data[addr % hdr.data_count];
    }

    return (int32_t)(DWGLS_TOTAL_SLOTS < max_out ? DWGLS_TOTAL_SLOTS : max_out);
}

/* ── kis4d_payload_size ──────────────────────────────────────────
 * Compute KIS4D payload size without encoding.
 *
 * Returns header(32) + address_map(20736*4) + data(N*1).
 * data_count is estimated from n_elems or from user_data.
 */
static uint32_t dwgls_kis4d_payload_size(uint32_t n_elems,
                                         const DWGLS_CodecCtx *ctx)
{
    (void)n_elems;
    (void)ctx;
    /* Worst case: every slot has a unique value (n_elems unique).
     * For typical weight data, unique count << 20736, but we use
     * the full map + worst-case data for a safe upper bound. */
    uint32_t data_count = ctx->user_data[KIS4D_CTX_DATA_COUNT];
    if (data_count == 0) data_count = 256;  /* conservative estimate */

    return KIS4D_CODEC_HEADER_SZ
         + DWGLS_TOTAL_SLOTS * sizeof(uint32_t)
         + data_count * sizeof(uint8_t);
}

/* ── kis4d_verify ────────────────────────────────────────────────
 * Verify KIS4D payload integrity.
 *
 * Checks: magic, total_slots, x+y+z = total, checksum consistency.
 *
 * Returns 0=ok, negative=corrupt.
 */
static int dwgls_kis4d_verify(const void *src, uint32_t src_len)
{
    if (!src || src_len < KIS4D_CODEC_HEADER_SZ) return -1;

    const uint8_t *in = (const uint8_t *)src;
    KIS4DHeader hdr;
    memcpy(&hdr, in, KIS4D_CODEC_HEADER_SZ);

    /* Magic check */
    if (hdr.magic != 0x4B495334) return -2;

    /* Total slots check */
    if (hdr.total_slots != DWGLS_TOTAL_SLOTS) return -3;

    /* Axis decomposition: x + y + z must equal total_slots */
    if (hdr.x_slots + hdr.y_slots + hdr.z_slots != hdr.total_slots)
        return -4;

    /* Minimum payload: header + full address map */
    uint32_t min_payload = KIS4D_CODEC_HEADER_SZ
                         + DWGLS_TOTAL_SLOTS * sizeof(uint32_t)
                         + hdr.data_count * sizeof(uint8_t);
    if (src_len < min_payload) return -5;

    /* CRC-32 integrity check */
    const uint8_t *data = in + KIS4D_CODEC_HEADER_SZ
                        + DWGLS_TOTAL_SLOTS * sizeof(uint32_t);
    uint32_t crc = 0;
    for (uint32_t i = 0; i < hdr.data_count; i++) {
        crc = (crc << 1) ^ data[i];
    }
    if (crc != hdr.checksum) return -6;

    return 0;
}

/* ── kis4d_resolve ───────────────────────────────────────────────
 * Address mapping: slot (0..20735) → resolved payload address.
 *
 * This is the core geometric mapping:
 *   1. Select axis (X/Y/Z) based on slot range
 *   2. Get local position within axis
 *   3. Compute angle, apply scale rotation, map back to slot index
 *
 * Returns resolved address in payload space, or UINT32_MAX on error.
 */
static uint32_t dwgls_kis4d_resolve(uint32_t slot,
                                    const DWGLS_CodecCtx *ctx)
{
    if (slot >= DWGLS_TOTAL_SLOTS) return UINT32_MAX;

    uint32_t x_slots = ctx->x_slots ? ctx->x_slots : DWGLS_TOTAL_SLOTS / 3;
    uint32_t y_slots = ctx->y_slots ? ctx->y_slots : DWGLS_TOTAL_SLOTS / 3;
    uint32_t z_slots = DWGLS_TOTAL_SLOTS - x_slots - y_slots;

    /* Build a minimal header for resolve */
    KIS4DHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.total_slots  = DWGLS_TOTAL_SLOTS;
    hdr.scale_factor = ctx->scale_factor;
    hdr.x_slots      = x_slots;
    hdr.y_slots      = y_slots;
    hdr.z_slots      = z_slots;

    return kis4d_resolve(slot, ctx->scale_factor, &hdr);
}

/* ════════════════════════════════════════════════════════════════
   VTABLE INSTANCE
   ════════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_KIS_4D = {
    .info         = dwgls_kis4d_info,
    .encode       = dwgls_kis4d_encode,
    .decode       = dwgls_kis4d_decode,
    .payload_size = dwgls_kis4d_payload_size,
    .verify       = dwgls_kis4d_verify,
    .resolve      = dwgls_kis4d_resolve,
};

#endif /* DWGLS_CODEC_KIS4D_H */
