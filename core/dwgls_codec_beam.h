/*
 * dwgls_codec_beam.h — Beam Entropy Codec Adapter for DWGLS Vtable
 * ════════════════════════════════════════════════════════════════
 *
 * Wraps beam_entropy_container.h concepts (BECCoord 8-bit navigation)
 * into the DWGLS_Codec vtable interface.
 *
 * Self-contained: defines minimal BeamEntropy types inline to avoid
 * transitive include chain issues with the full container header.
 *
 * Mapping: BeamEntropyContainer → DWGLS_Shell fields
 *   BecSlot.data[64]   → payload (raw byte data, lossless)
 *   BecSlot.flags      → shell integrity hint
 *   BEC_FIELD_W=144    → geometry (144×144 field)
 *
 * Payload layout (after DWGLS_Shell):
 *   occupied_count : uint32_t  (number of active slots)
 *   slot_data[]    : variable  (occupied slots, each 65 bytes: coord + data[64])
 *
 * resolve() maps slot (0..20735) → beam coordinate.
 *
 * No separate magic (runtime-only container).
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -fsyntax-only core/dwgls_codec_beam.h
 * DEPENDS: dwgls_codec.h, dwgls_shell.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_BEAM_H
#define DWGLS_CODEC_BEAM_H

#include "dwgls_codec.h"
#include "dwgls_shell.h"
#include <string.h>
#include <stdint.h>

/* ════════════════════════════════════════════════════════════════
   BEAM ENTROPY TYPES (minimal, self-contained)
   ════════════════════════════════════════════════════════════════ */

/* BECCoord: 8-bit navigation coordinate
 * Upper nibble = zone (0..15), Lower nibble = position (0..15) */
typedef uint8_t BECCoord;

/* Beam field dimensions: 144 × 144 = 20736 slots */
#define BEC_FIELD_W       144u
#define BEC_FIELD_SLOTS   20736u

/* Slot payload: 64 bytes of data + 1 byte flags = 65 bytes */
#define BEC_SLOT_DATA_SZ  64u
#define BEC_SLOT_FLAGS_SZ 1u

/* ════════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════════ */

/* ctx.user_data indices for beam metadata */
#define BEAM_CTX_OCCUPIED   0u   /* total occupied slots */

/* Each slot in payload: 1 byte coord + 64 bytes data = 65 bytes */
#define BEAM_SLOT_PAYLOAD_SZ  65u

/* ════════════════════════════════════════════════════════════════
   BEAM ADDRESSING (stride-37 mapping)
   ════════════════════════════════════════════════════════════════ */

/* Convert param_index (0..20735) to BECCoord */
static inline BECCoord bec_coord_from_param(uint32_t param_index)
{
    uint32_t zone = (param_index / BEC_FIELD_W) & 0x0F;
    uint32_t pos  = (param_index % BEC_FIELD_W) & 0x0F;
    return (BECCoord)((zone << 4) | pos);
}

/* Convert param_index to payload byte offset (after occupied_count header) */
static inline uint32_t bec_param_to_offset(uint32_t param_index)
{
    return sizeof(uint32_t) + param_index * BEAM_SLOT_PAYLOAD_SZ;
}

/* ════════════════════════════════════════════════════════════════
   CODEC VTABLE IMPLEMENTATION
   ════════════════════════════════════════════════════════════════ */

/* ── beam_info ──────────────────────────────────────────────────
 * Return codec metadata.
 */
static DWGLS_CodecInfo beam_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "beam_entropy";
    info.codec_id    = CODEC_BEAM_ENTROPY;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_RANDOM_ACCESS
                     | CODEC_FLAG_MMAP_FRIENDLY;
    return info;
}

/* ── beam_encode ────────────────────────────────────────────────
 * Encode raw byte data → Beam Entropy payload.
 *
 * Each byte maps to a BECCoord (zone<<4 | position).
 * Data is stored as occupied_count + per-slot (coord + data[64]).
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t beam_encode(const void *src, uint32_t n_elems,
                            const DWGLS_CodecCtx *ctx,
                            void *dst, uint32_t dst_cap)
{
    (void)ctx;
    if (!src || !dst || n_elems == 0) return -1;

    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;
    uint32_t off = 0;

    /* Write occupied count */
    if (off + sizeof(uint32_t) > dst_cap) return -1;
    uint32_t occupied = n_elems;
    memcpy(out + off, &occupied, sizeof(uint32_t));
    off += sizeof(uint32_t);

    /* Encode each byte with its BECCoord */
    for (uint32_t i = 0; i < n_elems; i++) {
        if (off + BEAM_SLOT_PAYLOAD_SZ > dst_cap) return -2;

        /* Compute BECCoord for this param_index */
        BECCoord coord = bec_coord_from_param(i);
        out[off++] = coord;

        /* Store the data byte at the start, zero-pad the rest */
        out[off] = in[i];
        memset(out + off + 1, 0, BEC_SLOT_DATA_SZ - 1);
        off += BEC_SLOT_DATA_SZ;
    }

    return (int32_t)off;
}

/* ── beam_decode ────────────────────────────────────────────────
 * Decode Beam Entropy payload → raw byte data.
 *
 * Reads occupied_count, then per-slot (coord + data[64]).
 * Returns the first byte of each slot's data.
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t beam_decode(const void *src, uint32_t src_len,
                            const DWGLS_CodecCtx *ctx,
                            void *dst, uint32_t dst_cap)
{
    (void)ctx;
    if (!src || !dst || src_len < sizeof(uint32_t)) return -1;

    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;

    /* Read occupied count */
    uint32_t occupied;
    memcpy(&occupied, in, sizeof(uint32_t));

    uint32_t off = sizeof(uint32_t);
    uint32_t written = 0;

    for (uint32_t i = 0; i < occupied && written < dst_cap; i++) {
        if (off + BEAM_SLOT_PAYLOAD_SZ > src_len) return -2;

        /* Skip coord byte */
        off++;

        /* Write the data byte (first byte of slot data) */
        out[written++] = in[off];
        off += BEC_SLOT_DATA_SZ;
    }

    return (int32_t)written;
}

/* ── beam_payload_size ──────────────────────────────────────────
 * Compute Beam Entropy payload size.
 */
static uint32_t beam_payload_size(uint32_t n_elems,
                                   const DWGLS_CodecCtx *ctx)
{
    (void)ctx;
    return (uint32_t)sizeof(uint32_t) + n_elems * BEAM_SLOT_PAYLOAD_SZ;
}

/* ── beam_verify ────────────────────────────────────────────────
 * Verify Beam Entropy payload integrity.
 *
 * Checks: occupied count matches available slots, coord values valid.
 * Returns 0=ok, negative=corrupt.
 */
static int beam_verify(const void *src, uint32_t src_len)
{
    if (!src || src_len < sizeof(uint32_t)) return -1;

    const uint8_t *in = (const uint8_t *)src;

    /* Read occupied count */
    uint32_t occupied;
    memcpy(&occupied, in, sizeof(uint32_t));

    /* Sanity check: occupied must fit in available space */
    uint32_t available = (src_len - sizeof(uint32_t)) / BEAM_SLOT_PAYLOAD_SZ;
    if (occupied > available) return -2;

    /* Verify each coord is valid (upper nibble < 16, lower nibble < 16) */
    uint32_t off = sizeof(uint32_t);
    for (uint32_t i = 0; i < occupied; i++) {
        if (off + BEAM_SLOT_PAYLOAD_SZ > src_len) return -3;
        BECCoord coord = in[off];
        uint8_t zone = (coord >> 4) & 0x0F;
        uint8_t pos  = coord & 0x0F;
        if (zone > 15 || pos > 15) return -4;
        off += BEAM_SLOT_PAYLOAD_SZ;
    }

    return 0;
}

/* ── beam_resolve ───────────────────────────────────────────────
 * Address mapping: slot (0..20735) → payload byte offset.
 *
 * Beam uses stride-37 mapping: slot → param_index → byte offset.
 *
 * Returns byte offset in payload, or UINT32_MAX on overflow.
 */
static uint32_t beam_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    if (slot >= BEC_FIELD_SLOTS) return UINT32_MAX;

    /* Map slot to payload offset */
    uint32_t param_index = slot % BEC_FIELD_SLOTS;
    return bec_param_to_offset(param_index);
}

/* ════════════════════════════════════════════════════════════════
   VTABLE INSTANCE
   ════════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_BEAM_ENTROPY = {
    .info         = beam_info,
    .encode       = beam_encode,
    .decode       = beam_decode,
    .payload_size = beam_payload_size,
    .verify       = beam_verify,
    .resolve      = beam_resolve,
};

#endif /* DWGLS_CODEC_BEAM_H */
