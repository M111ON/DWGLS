/*
 * dwgls_codec_tess.h — TESS Codec Adapter for DWGLS_Codec Vtable
 * ════════════════════════════════════════════════════════════════
 *
 * Wraps geo_tess_container.h into the DWGLS_Codec vtable.
 * Handles stride-37 scatter, octant resolution, CRC-64 integrity.
 *
 * PAYLOAD LAYOUT (after 32-byte DWGLS_Shell):
 *   [0..63]    TESS_Formula (64B) — resolver parameters
 *   [64..]     CubeData — weight cells, stride-37 scattered
 *   [last-8..last-1] CRC-64 seal over formula+cubedata
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -fsyntax-only core/dwgls_codec_tess.h
 * DEPENDS: dwgls_codec.h, dwgls_shell.h, geo_tess_container.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_TESS_H
#define DWGLS_CODEC_TESS_H

#include <stdint.h>
#include <string.h>
#include "dwgls_codec.h"
#include "geo_tess_container.h"

/* ════════════════════════════════════════════════════════════════
   TESS CODEC CONTEXT EXTENSIONS
   ════════════════════════════════════════════════════════════════
 * user_data[0] = cell_size    (bytes per cell: 1,2,4,18,20,34)
 * user_data[1] = gguf_type    (TESS_GGML_F32, _Q8_0, etc.)
 * user_data[2] = octant_mask  (which of 8 octants to encode, 0xFF=all)
 * user_data[3] = reserved
 */

#define TESS_CTX_CELL_SIZE(ctx)    ((ctx)->user_data[0])
#define TESS_CTX_GGUF_TYPE(ctx)    ((ctx)->user_data[1])
#define TESS_CTX_OCTANT_MASK(ctx)  ((ctx)->user_data[2])

/* ════════════════════════════════════════════════════════════════
   CODEC INFO
   ════════════════════════════════════════════════════════════════ */

static inline DWGLS_CodecInfo tess_codec_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "tess";
    info.codec_id    = CODEC_TESS;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_RANDOM_ACCESS    /* stride-37 is scatter,
                                                       but resolve() enables
                                                       random access */
                     | CODEC_FLAG_MMAP_FRIENDLY
                     | CODEC_FLAG_DERIVED_VIEWS    /* 8 octant views */
                     | CODEC_FLAG_COMPRESSED;      /* Q4/Q8 quantization
                                                       reduces cell size */
    return info;
}

/* ════════════════════════════════════════════════════════════════
   ENCODE: raw weights → tess payload
   ════════════════════════════════════════════════════════════════
 * Creates: TESS_Formula + stride-37 scattered CubeData + CRC-64
 *
 * src:      raw weight data (uint8_t array)
 * n_elems:  number of elements (must be ≤ 20736)
 * ctx:      codec context (cell_size in user_data[0], gguf_type in [1])
 * dst:      output buffer (caller-allocated)
 * dst_cap:  capacity of dst in bytes
 * Returns:  bytes written, or negative on error
 */

static inline int32_t tess_codec_encode(
    const void *src, uint32_t n_elems,
    const DWGLS_CodecCtx *ctx,
    void *dst, uint32_t dst_cap)
{
    if (!src || !dst || !ctx) return -1;
    if (n_elems > TESS_TOTAL_SLOTS) return -2;

    uint32_t cell_sz = TESS_CTX_CELL_SIZE(ctx);
    if (cell_sz == 0) cell_sz = 1;  /* default: 1 byte per cell */

    /* Compute required payload size:
     *   Formula (64) + CubeData (n_elems * cell_sz) + CRC (8) */
    uint32_t formula_sz  = sizeof(TESS_Formula);
    uint32_t cubedata_sz = n_elems * cell_sz;
    uint32_t total_payload = formula_sz + cubedata_sz + TESS_CRC_SIZE;

    if (total_payload > dst_cap) return -3;

    uint8_t *out = (uint8_t *)dst;

    /* ── Write Formula block ───────────────────────────────── */
    TESS_Formula *fm = (TESS_Formula *)out;
    memset(fm, 0, sizeof(*fm));
    fm->mirror_axis_x = ctx->x_slots ? ctx->x_slots : TESS_X_SLOTS;
    fm->mirror_axis_y = ctx->y_slots ? ctx->y_slots : TESS_Y_SLOTS;
    fm->mirror_axis_z = ctx->z_slots ? ctx->z_slots : TESS_Z_SLOTS;
    fm->time_stride   = 1u;  /* default: no temporal scaling */
    fm->octant_mask   = (uint8_t)(TESS_CTX_OCTANT_MASK(ctx)
                                  ? TESS_CTX_OCTANT_MASK(ctx) : 0xFFu);
    fm->stride_seed   = TESS_STRIDE_37;

    /* ── Scatter weights into CubeData using stride-37 ───────
     * Stride-37 is a bijection on [0, 20736) since gcd(37,20736)=1.
     * For n_elems == TESS_TOTAL_SLOTS, use scatter directly.
     * For smaller n_elems, use identity (no scatter — not bijective). */
    uint8_t *cubedata = out + formula_sz;
    memset(cubedata, 0, cubedata_sz);

    const uint8_t *src_u8 = (const uint8_t *)src;
    int use_scatter = (n_elems == TESS_TOTAL_SLOTS);

    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t cell = use_scatter ? tess_stride_scatter(i) : i;
        uint32_t off  = cell * cell_sz;
        if (off + cell_sz <= cubedata_sz) {
            memcpy(cubedata + off, src_u8 + i * cell_sz, cell_sz);
        }
    }

    /* ── Compute formula_id (hash of formula parameters) ───── */
    fm->_pad[0] = 0;  /* ensure deterministic padding */
    uint64_t fid = dwgls_crc64((const uint8_t *)fm, formula_sz);
    /* Store as last 8 bytes of formula (overwrite reserved pad) */

    /* ── Compute CRC-64 seal over formula + cubedata ────────── */
    uint64_t crc = dwgls_crc64(out, formula_sz + cubedata_sz);

    /* Write CRC-64 at end of payload (big-endian for portability) */
    uint8_t *seal = out + formula_sz + cubedata_sz;
    for (int i = 7; i >= 0; i--) {
        seal[7 - i] = (uint8_t)(crc >> (i * 8));
    }

    (void)fid;  /* formula_id stored in reserved field if needed */

    return (int32_t)total_payload;
}

/* ════════════════════════════════════════════════════════════════
   DECODE: tess payload → raw weights
   ════════════════════════════════════════════════════════════════
 * Reverses stride-37 scatter to extract linear weight ordering.
 *
 * src:      codec payload (TESS_Formula + CubeData + CRC)
 * src_len:  payload bytes
 * ctx:      codec context
 * dst:      output buffer (caller-allocated)
 * dst_cap:  capacity in bytes
 * Returns:  bytes written, or negative on error
 */

static inline int32_t tess_codec_decode(
    const void *src, uint32_t src_len,
    const DWGLS_CodecCtx *ctx,
    void *dst, uint32_t dst_cap)
{
    if (!src || !dst || !ctx) return -1;
    if (src_len < sizeof(TESS_Formula) + TESS_CRC_SIZE) return -2;

    uint32_t cell_sz = TESS_CTX_CELL_SIZE(ctx);
    if (cell_sz == 0) cell_sz = 1;

    const uint8_t *in = (const uint8_t *)src;

    /* ── Read Formula (for axis bounds validation) ─────────── */
    const TESS_Formula *fm = (const TESS_Formula *)in;
    (void)fm;  /* axis bounds available for future validation */

    /* ── Compute CubeData size (payload minus formula minus CRC) */
    uint32_t cubedata_sz = src_len - (uint32_t)sizeof(TESS_Formula)
                           - TESS_CRC_SIZE;
    uint32_t n_cells = cubedata_sz / cell_sz;
    uint32_t raw_bytes = n_cells * cell_sz;

    if (raw_bytes > dst_cap) return -3;

    const uint8_t *cubedata = in + sizeof(TESS_Formula);

    /* ── Gather weights from scattered layout ──────────────
     * Inverse of encode: if stride-37 was used, gather reverses it.
     * If identity was used (n_elems != 20736), read directly. */
    uint8_t *out = (uint8_t *)dst;
    int use_gather = (n_cells == TESS_TOTAL_SLOTS);

    for (uint32_t i = 0; i < n_cells; i++) {
        uint32_t linear = use_gather ? tess_stride_gather(i) : i;
        if (linear >= n_cells) continue;  /* out-of-range cell */

        uint32_t src_off = i * cell_sz;
        uint32_t dst_off = linear * cell_sz;

        if (src_off + cell_sz <= cubedata_sz &&
            dst_off + cell_sz <= raw_bytes) {
            memcpy(out + dst_off, cubedata + src_off, cell_sz);
        }
    }

    return (int32_t)raw_bytes;
}

/* ════════════════════════════════════════════════════════════════
   PAYLOAD SIZE: compute encoded size without encoding
   ════════════════════════════════════════════════════════════════
 * Returns total payload bytes for n_elems cells.
 * This is what the shell should declare in payload_size.
 */

static inline uint32_t tess_codec_payload_size(
    uint32_t n_elems, const DWGLS_CodecCtx *ctx)
{
    uint32_t cell_sz = TESS_CTX_CELL_SIZE(ctx);
    if (cell_sz == 0) cell_sz = 1;

    /* Formula (64) + CubeData + CRC (8) */
    return (uint32_t)sizeof(TESS_Formula)
         + n_elems * cell_sz
         + TESS_CRC_SIZE;
}

/* ════════════════════════════════════════════════════════════════
   VERIFY: check payload integrity
   ════════════════════════════════════════════════════════════════
 * Validates CRC-64 seal over formula + cubedata.
 * Returns: 0=ok, -1=CRC mismatch, -2=payload too small
 */

static inline int tess_codec_verify(
    const void *src, uint32_t src_len)
{
    if (!src) return -2;
    if (src_len < sizeof(TESS_Formula) + TESS_CRC_SIZE) return -2;

    const uint8_t *in = (const uint8_t *)src;
    uint32_t data_len = src_len - TESS_CRC_SIZE;

    /* Read stored CRC-64 (big-endian) */
    const uint8_t *seal = in + data_len;
    uint64_t stored_crc = 0;
    for (int i = 0; i < 8; i++) {
        stored_crc = (stored_crc << 8) | seal[i];
    }

    /* Compute CRC-64 over formula + cubedata */
    uint64_t computed_crc = dwgls_crc64(in, data_len);

    return (computed_crc == stored_crc) ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════
   RESOLVE: address mapping (slot → cell index)
   ════════════════════════════════════════════════════════════════
 * Maps a linear weight slot (0..20735) to the cell index in the
 * scattered CubeData layout via stride-37 scatter.
 *
 * For random access into a .tess file:
 *   cell_index = resolve(slot, ctx)
 *   byte_offset = formula_size + cell_index * cell_size
 *
 * slot:     input slot (0..20735)
 * ctx:      codec context
 * Returns:  cell index in CubeData (0..n_elems-1)
 */

static inline uint32_t tess_codec_resolve(
    uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    (void)ctx;
    /* Stride-37 scatter: weight slot → cell index */
    return tess_stride_scatter(slot);
}

/* ════════════════════════════════════════════════════════════════
   SHELL ↔ TESS_HEADER CONVERSION HELPERS
   ════════════════════════════════════════════════════════════════
 * Bridge between DWGLS_Shell (32B) and TESS_Header (64B).
 * Used when opening legacy .tess files or constructing native
 * DWGLS wrappers around tess payloads.
 */

/* Populate a DWGLS_Shell from a TESS_Header */
static inline void tess_shell_from_header(DWGLS_Shell *s,
                                           const TESS_Header *th)
{
    s->magic        = DWGLS_SHELL_MAGIC;
    s->version      = DWGLS_SHELL_VERSION;
    s->codec_id     = CODEC_TESS;
    s->integrity    = INTEGRITY_CRC64;
    s->total_slots  = th->total_slots;
    s->scale_factor = th->scale_factor;
    s->cell_size    = th->cell_size;
    /* payload_size = formula(64) + cubedata + CRC(8)
     * cubedata = total_slots * cell_size (approximate) */
    s->payload_size = (uint32_t)sizeof(TESS_Formula)
                    + th->total_slots * th->cell_size
                    + TESS_CRC_SIZE;
    s->checksum     = th->cube_checksum;
}

/* Populate a TESS_Header from a DWGLS_Shell */
static inline void tess_header_from_shell(TESS_Header *th,
                                           const DWGLS_Shell *s,
                                           uint32_t gguf_type)
{
    memset(th, 0, sizeof(*th));
    th->magic         = TESS_MAGIC;
    th->version       = TESS_VERSION;
    th->total_slots   = s->total_slots;
    th->cell_size     = s->cell_size;
    th->scale_factor  = s->scale_factor;
    th->x_slots       = TESS_X_SLOTS;
    th->y_slots       = TESS_Y_SLOTS;
    th->z_slots       = TESS_Z_SLOTS;
    th->gguf_type     = gguf_type;
    th->tensor_count  = 0;
    th->source_size   = 0;
    th->cube_checksum = s->checksum;
    th->formula_id    = 0;
}

/* ════════════════════════════════════════════════════════════════
   OCTANT-AWARE RESOLVE (extended address mapping)
   ════════════════════════════════════════════════════════════════
 * Maps slot through octant mirror + stride-37 scatter.
 * This combines two address transforms:
 *   1. Octant: mirrors slot within axis (Cayley-like symmetry)
 *   2. Stride-37: scatters linear index into cell space
 *
 * octant:   0-7 (bit 0=X, bit 1=Y, bit 2=Z sign)
 * header:   TESS_Header with axis bounds
 * Returns:  cell index in CubeData
 */

static inline uint32_t tess_codec_resolve_octant(
    uint32_t slot, uint8_t octant, const TESS_Header *header)
{
    /* Step 1: Apply octant mirror */
    uint32_t mirrored = tess_resolve_octant(slot, octant, header);

    /* Step 2: Stride-37 scatter into cell space */
    return tess_stride_scatter(mirrored);
}

/* ════════════════════════════════════════════════════════════════
   VTABLE REGISTRATION
   ════════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_TESS = {
    .info         = tess_codec_info,
    .encode       = tess_codec_encode,
    .decode       = tess_codec_decode,
    .payload_size = tess_codec_payload_size,
    .verify       = tess_codec_verify,
    .resolve      = tess_codec_resolve,
};

#endif /* DWGLS_CODEC_TESS_H */
