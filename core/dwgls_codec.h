/* ═══════════════════════════════════════════════════════════════════════════
 * dwgls_codec.h — Swappable Codec Interface for DWGLS Shell
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Every codec implements the same 6 functions. The shell calls them.
 * Codecs don't call each other. Stateless: encode/decode are pure functions.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef DWGLS_CODEC_H
#define DWGLS_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "dwgls_shell.h"

/* ═══════════════════════════════════════════════════════════════
   CODEC CONTEXT (optional, for codecs that need state)
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_slots;     /* 20736 or codec-specific */
    uint32_t scale_factor;    /* fixed-point scale */
    uint32_t x_slots;         /* axis decomposition (0 = use default) */
    uint32_t y_slots;
    uint32_t z_slots;
    uint32_t user_data[4];    /* codec-specific parameters */
} DWGLS_CodecCtx;

/* ═══════════════════════════════════════════════════════════════
   CODEC INFO
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;         /* "tess", "gcube", etc. */
    uint8_t     codec_id;     /* CODEC_* constant */
    uint8_t     min_version;  /* minimum shell version needed */
    uint32_t    flags;        /* capability flags (see below) */
} DWGLS_CodecInfo;

/* ═══════════════════════════════════════════════════════════════
   CODEC FLAGS
   ═══════════════════════════════════════════════════════════════ */

#define CODEC_FLAG_MULTI_TENSOR   (1u << 0)  /* supports multiple tensors */
#define CODEC_FLAG_MMAP_FRIENDLY  (1u << 1)  /* payload is mmap-able */
#define CODEC_FLAG_SEQUENTIAL     (1u << 2)  /* optimized for seq access */
#define CODEC_FLAG_RANDOM_ACCESS  (1u << 3)  /* optimized for random access */
#define CODEC_FLAG_COMPRESSED     (1u << 4)  /* payload may be smaller */
#define CODEC_FLAG_DERIVED_VIEWS  (1u << 5)  /* generates views at runtime */

/* ═══════════════════════════════════════════════════════════════
   CODEC VTABLE
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    /* ── Metadata ──────────────────────────────────────────── */
    DWGLS_CodecInfo (*info)(void);

    /* ── Encode: raw weights → codec payload ──────────────────
     *   src:      raw weight data
     *   n_elems:  number of elements
     *   ctx:      codec context (scale, axes, user_data)
     *   dst:      output buffer (caller-allocated)
     *   dst_cap:  capacity of dst in bytes
     *   Returns:  bytes written, or negative on error
     */
    int32_t (*encode)(const void *src, uint32_t n_elems,
                      const DWGLS_CodecCtx *ctx,
                      void *dst, uint32_t dst_cap);

    /* ── Decode: codec payload → raw weights ──────────────────
     *   src:      codec payload (starts after DWGLS_Shell)
     *   src_len:  payload bytes
     *   ctx:      codec context
     *   dst:      output buffer (caller-allocated)
     *   dst_cap:  capacity of dst in bytes
     *   Returns:  bytes written (raw), or negative on error
     */
    int32_t (*decode)(const void *src, uint32_t src_len,
                      const DWGLS_CodecCtx *ctx,
                      void *dst, uint32_t dst_cap);

    /* ── Size: compute encoded size without encoding ──────────
     *   n_elems:  number of elements
     *   ctx:      codec context
     *   Returns:  payload size in bytes, 0 if unknown
     */
    uint32_t (*payload_size)(uint32_t n_elems,
                              const DWGLS_CodecCtx *ctx);

    /* ── Verify: check payload integrity ──────────────────────
     *   src:      codec payload
     *   src_len:  payload bytes
     *   Returns:  0=ok, negative=corrupt
     */
    int (*verify)(const void *src, uint32_t src_len);

    /* ── Resolve: address mapping (for codecs with geometry) ──
     *   slot:     input slot (0..20735)
     *   ctx:      codec context (scale, formula params)
     *   Returns:  resolved address in payload space
     */
    uint32_t (*resolve)(uint32_t slot, const DWGLS_CodecCtx *ctx);

} DWGLS_CodecVtable;

/* ═══════════════════════════════════════════════════════════════
   BUILT-IN CODEC REGISTRY (compile-time, no malloc)
   ═══════════════════════════════════════════════════════════════ */

/* Each codec defines a static const vtable */
extern const DWGLS_CodecVtable DWGLS_CODEC_RAW;
extern const DWGLS_CodecVtable DWGLS_CODEC_TESS;

/* ── Lookup by codec_id ──────────────────────────────────────── */
static inline const DWGLS_CodecVtable* dwgls_codec_find(uint8_t codec_id)
{
    /* Static array — no malloc, O(N) scan but N ≤ 16 */
    static const DWGLS_CodecVtable *registry[] = {
        &DWGLS_CODEC_RAW,           /* 0 */
        NULL,                       /* 1 - KIS_FRAME (not implemented) */
        NULL,                       /* 2 - KIS_4D (not implemented) */
        NULL,                       /* 3 - TESSERACT (not implemented) */
        NULL,                       /* 4 - GCUBE (not implemented) */
        NULL,                       /* 5 - BEAM_ENTROPY (not implemented) */
        &DWGLS_CODEC_TESS,          /* 6 - TESS */
        NULL,                       /* 7 - KIS_V6 (not implemented) */
        NULL,                       /* 8 - DIAMOND_FIELD (not implemented) */
    };
    if (codec_id < sizeof(registry)/sizeof(registry[0]))
        return registry[codec_id];
    return NULL;  /* unknown codec — user must register externally */
}

/* ═══════════════════════════════════════════════════════════════
   RAW CODEC (passthrough / identity)
   ═══════════════════════════════════════════════════════════════ */

static inline DWGLS_CodecInfo raw_info(void)
{
    return (DWGLS_CodecInfo){
        .name = "raw",
        .codec_id = CODEC_NONE,
        .min_version = 1,
        .flags = CODEC_FLAG_MMAP_FRIENDLY | CODEC_FLAG_SEQUENTIAL | CODEC_FLAG_RANDOM_ACCESS,
    };
}

static inline int32_t raw_encode(const void *src, uint32_t n_elems,
                                  const DWGLS_CodecCtx *ctx,
                                  void *dst, uint32_t dst_cap)
{
    (void)ctx;
    uint32_t cell_size = ctx ? ctx->user_data[0] : 4;
    uint32_t need = n_elems * cell_size;
    if (dst_cap < need) return -1;
    memcpy(dst, src, need);
    return (int32_t)need;
}

static inline int32_t raw_decode(const void *src, uint32_t src_len,
                                  const DWGLS_CodecCtx *ctx,
                                  void *dst, uint32_t dst_cap)
{
    (void)ctx;
    uint32_t cell_size = ctx ? ctx->user_data[0] : 4;
    uint32_t n_elems = src_len / cell_size;
    uint32_t need = n_elems * cell_size;
    if (dst_cap < need) return -1;
    memcpy(dst, src, need);
    return (int32_t)need;
}

static inline uint32_t raw_payload_size(uint32_t n_elems,
                                         const DWGLS_CodecCtx *ctx)
{
    uint32_t cell_size = ctx ? ctx->user_data[0] : 4;
    return n_elems * cell_size;
}

static inline int raw_verify(const void *src, uint32_t src_len)
{
    (void)src; (void)src_len;
    return 0;  /* raw always passes */
}

static inline uint32_t raw_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    (void)ctx;
    return slot;  /* identity mapping */
}

const DWGLS_CodecVtable DWGLS_CODEC_RAW = {
    .info         = raw_info,
    .encode       = raw_encode,
    .decode       = raw_decode,
    .payload_size = raw_payload_size,
    .verify       = raw_verify,
    .resolve      = raw_resolve,
};

#endif /* DWGLS_CODEC_H */