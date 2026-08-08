/*
 * dwgls_codec_kis.h — KIS Codec Adapter for DWGLS Vtable
 * ════════════════════════════════════════════════════════════════
 *
 * Wraps geo_kis_container.h (KIS format: frame+block payload)
 * into the DWGLS_Codec vtable interface.
 *
 * Mapping: kis_header_t fields → DWGLS_Shell fields
 *   KisHeader.tier       → ctx.user_data[0]
 *   KisHeader.entropy    → ctx.user_data[1]
 *   KisHeader.frame_cnt  → ctx.user_data[2]
 *   KisHeader.block_cnt  → ctx.user_data[3]
 *   KisHeader.weight_cnt → shell.total_slots
 *
 * Payload layout (after DWGLS_Shell):
 *   frame_slots[frame_cnt] : uint16_t each  (enc values)
 *   blocks[block_cnt]      : 64 floats each (DiamondBlocks)
 *
 * The shell CRC-64 covers the entire file; the codec payload
 * itself carries no additional checksum (it inherits integrity
 * from the DWGLS_Shell layer).
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -fsyntax-only core/dwgls_codec_kis.h
 * DEPENDS: dwgls_codec.h, dwgls_shell.h, geo_kis_container.h
 *          geo_adaptive_store.h, geo_frame_seek.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_KIS_H
#define DWGLS_CODEC_KIS_H

#include "dwgls_codec.h"
#include "geo_kis_container.h"
#include "geo_adaptive_store.h"

/* ════════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════════ */

/* ctx.user_data indices for KIS metadata */
#define KIS_CTX_TIER        0u
#define KIS_CTX_ENTROPY     1u
#define KIS_CTX_FRAME_CNT   2u
#define KIS_CTX_BLOCK_CNT   3u

/* ════════════════════════════════════════════════════════════════
   KIS → DWGLS_SHELL MAPPING
   ════════════════════════════════════════════════════════════════ */

/*
 * kis_to_shell: populate a DWGLS_Shell from KisHeader + context.
 * The shell wraps the KIS payload in DWGLS universal format.
 */
static inline void kis_to_shell(const KisHeader *kis,
                                const DWGLS_CodecCtx *ctx,
                                DWGLS_Shell *shell)
{
    /* Geometry: KIS uses the full 20736-sacred slot space */
    uint32_t payload = kis_container_size(kis) - KIS_HEADER_SZ;
    /* Subtract the legacy CRC (8B) — shell handles integrity */
    if (payload >= KIS_CRC_SZ) payload -= KIS_CRC_SZ;

    dwgls_shell_init(shell,
                     CODEC_KIS_FRAME,
                     kis->weight_cnt ? kis->weight_cnt : DWGLS_TOTAL_SLOTS,
                     ctx->scale_factor,
                     payload,
                     sizeof(float),   /* cell_size: floats are 4 bytes */
                     INTEGRITY_CRC64);
}

/* ════════════════════════════════════════════════════════════════
   CODEC VTABLE IMPLEMENTATION
   ════════════════════════════════════════════════════════════════ */

/* ── kis_info ───────────────────────────────────────────────────
 * Return codec metadata.
 */
static DWGLS_CodecInfo kis_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "kis_frame";
    info.codec_id    = CODEC_KIS_FRAME;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_RANDOM_ACCESS
                     | CODEC_FLAG_MMAP_FRIENDLY;
    return info;
}

/* ── kis_encode ─────────────────────────────────────────────────
 * Encode raw float weights → KIS payload (frame_slots + blocks).
 *
 * ctx->user_data[KIS_CTX_TIER]     = tier 0..3 (0=auto from entropy)
 * ctx->user_data[KIS_CTX_ENTROPY]  = entropy score 0..255
 *
 * If tier==0 and entropy==0, defaults to tier 0 (structured).
 *
 * dst layout:
 *   [frame_cnt × uint16_t]  frame slot encs
 *   [block_cnt × 64 × float] DiamondBlocks
 *
 * Returns bytes written, or -1 on error.
 */
static int32_t kis_encode(const void *src, uint32_t n_elems,
                           const DWGLS_CodecCtx *ctx,
                           void *dst, uint32_t dst_cap)
{
    if (!src || !ctx || !dst || n_elems == 0) return -1;

    const float *weights = (const float *)src;

    /* Derive tier and entropy from user_data or defaults */
    uint8_t tier     = (uint8_t)(ctx->user_data[KIS_CTX_TIER] & 0xFF);
    uint8_t entropy  = (uint8_t)(ctx->user_data[KIS_CTX_ENTROPY] & 0xFF);

    /* Default: tier 0 structured */
    if (tier == 0 && entropy == 0) {
        tier    = 0;
        entropy = 32;  /* low-entropy structured data */
    }

    /* Build AdaptiveStore from weight data */
    AdaptiveStore as;
    adaptive_init(&as);

    /* Use time = 0 as the canonical entry point */
    int rc = adaptive_write(&as, 0, weights, (int)n_elems, entropy);
    if (rc != 0) return -1;

    /* Compute payload size: frame_slots + DiamondBlocks (no KIS header, no CRC) */
    uint32_t frame_data_sz = (uint32_t)as.frame_count * sizeof(uint16_t);
    uint32_t block_data_sz = (uint32_t)as.block_count * ADPT_BLOCK_WORDS * sizeof(float);
    uint32_t total_payload = frame_data_sz + block_data_sz;

    if (dst_cap < total_payload) return -1;

    uint8_t *out = (uint8_t *)dst;
    uint32_t off = 0;

    /* Write frame slots */
    memcpy(out + off, as.frames, frame_data_sz);
    off += frame_data_sz;

    /* Write DiamondBlocks */
    memcpy(out + off, as.blocks, block_data_sz);
    off += block_data_sz;

    return (int32_t)off;
}

/* ── kis_decode ─────────────────────────────────────────────────
 * Decode KIS payload → raw float weights.
 *
 * src layout:
 *   [frame_cnt × uint16_t]  frame slot encs
 *   [block_cnt × 64 × float] DiamondBlocks
 *
 * frame_cnt and block_cnt are derived from dst_cap and
 * the payload structure. We iterate blocks until weight capacity.
 *
 * Returns bytes written (raw weights), or -1 on error.
 */
static int32_t kis_decode(const void *src, uint32_t src_len,
                           const DWGLS_CodecCtx *ctx,
                           void *dst, uint32_t dst_cap)
{
    if (!src || !dst || src_len == 0 || dst_cap == 0) return -1;
    (void)ctx;  /* decode doesn't need context for reconstruction */

    const uint8_t *in = (const uint8_t *)src;
    float *out = (float *)dst;
    uint32_t max_floats = dst_cap / sizeof(float);

    /* We need to figure out frame_cnt from the payload.
     * Strategy: try frame_cnt = 1..KIS_MAX_FRAMES, check if
     * block_data fits in remaining src_len.
     * block_cnt = frame_cnt × 12 (ADPT_EDGES_PER_FRAME)
     * block_data = block_cnt × 64 × 4 bytes
     */
    uint32_t frame_cnt = 0;
    uint32_t frame_data_sz = 0;

    for (uint8_t fc = 1; fc <= KIS_MAX_FRAMES; fc++) {
        uint32_t fdsz = (uint32_t)fc * sizeof(uint16_t);
        uint16_t bc = (uint16_t)(fc * ADPT_EDGES_PER_FRAME);
        uint32_t bdsz = (uint32_t)bc * KIS_BLOCK_WORDS * sizeof(float);
        if (fdsz + bdsz <= src_len) {
            frame_cnt       = fc;
            frame_data_sz   = fdsz;
        }
    }

    if (frame_cnt == 0) return -1;  /* couldn't fit any valid layout */

    /* frame_slots are at the start of src but used only for geometry reference;
     * weight reconstruction reads from blocks only. */

    /* Read DiamondBlocks */
    const float *blocks = (const float *)(in + frame_data_sz);

    /* Reconstruct weights: round-robin read from blocks */
    uint32_t wi = 0;
    uint32_t bi = 0;
    for (uint32_t fi = 0; fi < frame_cnt && wi < max_floats; fi++) {
        for (uint16_t ei = 0; ei < ADPT_EDGES_PER_FRAME && wi < max_floats; ei++) {
            const float *blk = blocks + bi * KIS_BLOCK_WORDS;
            for (uint32_t k = 0; k < KIS_BLOCK_WORDS && wi < max_floats; k++) {
                out[wi++] = blk[k];
            }
            bi++;
        }
    }

    /* Return number of bytes written */
    return (int32_t)(wi * sizeof(float));
}

/* ── kis_payload_size ───────────────────────────────────────────
 * Compute KIS payload size without encoding.
 *
 * From ctx->user_data we derive frame_cnt and block_cnt.
 * If user_data not set, defaults to tier 0 → 1 frame, 12 blocks.
 *
 * Returns payload size in bytes (frame_slots + blocks).
 */
static uint32_t kis_payload_size(uint32_t n_elems,
                                  const DWGLS_CodecCtx *ctx)
{
    (void)n_elems;  /* weight count doesn't change layout */

    uint8_t tier = (uint8_t)(ctx->user_data[KIS_CTX_TIER] & 0xFF);
    uint8_t entropy = (uint8_t)(ctx->user_data[KIS_CTX_ENTROPY] & 0xFF);

    /* Derive frame count from tier */
    uint8_t frame_cnt;
    if (tier == 0 && entropy == 0) {
        frame_cnt = 1;  /* default tier 0 */
    } else {
        frame_cnt = adaptive_frame_count(tier);
    }

    uint16_t block_cnt = (uint16_t)(frame_cnt * ADPT_EDGES_PER_FRAME);

    uint32_t frame_data_sz = (uint32_t)frame_cnt * sizeof(uint16_t);
    uint32_t block_data_sz = (uint32_t)block_cnt * KIS_BLOCK_WORDS * sizeof(float);

    return frame_data_sz + block_data_sz;
}

/* ── kis_verify ─────────────────────────────────────────────────
 * Verify KIS payload integrity.
 *
 * Checks: frame/block count consistency, block_cnt == frame_cnt × 12,
 * and that the payload size matches the declared counts.
 *
 * Returns 0=ok, negative=corrupt.
 */
static int kis_verify(const void *src, uint32_t src_len)
{
    if (!src || src_len == 0) return -1;

    /* Minimum payload: 1 frame slot (2B) + 1 block (256B) */
    if (src_len < sizeof(uint16_t) + KIS_BLOCK_WORDS * sizeof(float))
        return -2;

    /* Find best-fitting frame_cnt (same logic as decode) */
    uint8_t best_fc = 0;
    for (uint8_t fc = 1; fc <= KIS_MAX_FRAMES; fc++) {
        uint32_t fdsz = (uint32_t)fc * sizeof(uint16_t);
        uint16_t bc = (uint16_t)(fc * ADPT_EDGES_PER_FRAME);
        uint32_t bdsz = (uint32_t)bc * KIS_BLOCK_WORDS * sizeof(float);
        if (fdsz + bdsz <= src_len) {
            best_fc = fc;
        }
    }

    if (best_fc == 0) return -3;

    /* Verify structural invariant */
    uint16_t block_cnt = (uint16_t)(best_fc * ADPT_EDGES_PER_FRAME);
    uint32_t frame_data_sz = (uint32_t)best_fc * sizeof(uint16_t);
    uint32_t block_data_sz = (uint32_t)block_cnt * KIS_BLOCK_WORDS * sizeof(float);
    uint32_t expected = frame_data_sz + block_data_sz;

    if (expected != src_len) return -4;  /* trailing/missing bytes */

    /* Validate frame slot encs are in range [0, 1439] */
    const uint16_t *slots = (const uint16_t *)src;
    for (uint8_t fi = 0; fi < best_fc; fi++) {
        if (slots[fi] >= FRAME_CYCLE) return -5;
    }

    return 0;
}

/* ── kis_resolve ────────────────────────────────────────────────
 * Address mapping: slot (0..20735) → payload byte offset.
 *
 * The slot decomposes into:
 *   frame_idx = slot / (12 × 64)  [which DiamondBlock frame]
 *   edge_idx  = (slot / 64) % 12  [which edge within frame]
 *   float_idx = slot % 64         [which float within block]
 *
 * Payload offset = frame_slots_size + (frame_idx × 12 + edge_idx) × 256
 *                  + float_idx × 4
 *
 * We need ctx->user_data[KIS_CTX_FRAME_CNT] to know frame_cnt.
 * If not set, assumes tier 0 (1 frame).
 *
 * Returns byte offset in payload, or UINT32_MAX on overflow.
 */
static uint32_t kis_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    if (slot >= DWGLS_TOTAL_SLOTS) return UINT32_MAX;

    /* Derive frame_cnt: explicit user_data override, else from tier */
    uint8_t frame_cnt = (uint8_t)(ctx->user_data[KIS_CTX_FRAME_CNT] & 0xFF);
    if (frame_cnt == 0) {
        uint8_t tier = (uint8_t)(ctx->user_data[KIS_CTX_TIER] & 0xFF);
        uint8_t entropy = (uint8_t)(ctx->user_data[KIS_CTX_ENTROPY] & 0xFF);
        frame_cnt = (tier == 0 && entropy == 0) ? 1 : adaptive_frame_count(tier);
    }

    uint16_t total_blocks = (uint16_t)(frame_cnt * ADPT_EDGES_PER_FRAME);

    /* Decompose slot into frame/edge/float indices */
    uint32_t block_idx = slot / KIS_BLOCK_WORDS;  /* which block globally */
    uint32_t float_idx = slot % KIS_BLOCK_WORDS;  /* which float in block */

    /* Wrap block_idx if it exceeds allocated blocks (modular mapping) */
    if (block_idx >= total_blocks) {
        block_idx = block_idx % total_blocks;
    }

    /* Payload offset: frame_slots + block_data */
    uint32_t frame_data_sz = (uint32_t)frame_cnt * sizeof(uint16_t);
    uint32_t offset = frame_data_sz + block_idx * KIS_BLOCK_WORDS * sizeof(float)
                    + float_idx * sizeof(float);

    return offset;
}

/* ════════════════════════════════════════════════════════════════
   VTABLE INSTANCE
   ════════════════════════════════════════════════════════════════ */

const DWGLS_CodecVtable DWGLS_CODEC_KIS_FRAME = {
    .info         = kis_info,
    .encode       = kis_encode,
    .decode       = kis_decode,
    .payload_size = kis_payload_size,
    .verify       = kis_verify,
    .resolve      = kis_resolve,
};

#endif /* DWGLS_CODEC_KIS_H */
