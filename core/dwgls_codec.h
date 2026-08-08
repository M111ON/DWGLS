/*
 * dwgls_codec.h — Swappable Codec Interface
 * ════════════════════════════════════════════════════════════════
 *
 * Every codec registers a DWGLS_Codec vtable.
 * The shell dispatches through the vtable based on codec_id.
 * Codecs are stateless: encode/decode are pure functions.
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -o test_codec tests/test_codec.c -lm
 * DEPENDS: dwgls_shell.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_CODEC_H
#define DWGLS_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "dwgls_shell.h"

/* ════════════════════════════════════════════════════════════════
   CODEC CONTEXT
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_slots;     /* 20736 or codec-specific */
    uint32_t scale_factor;    /* fixed-point scale × 65536 */
    uint32_t x_slots;         /* axis decomposition (0 = use default) */
    uint32_t y_slots;
    uint32_t z_slots;
    uint32_t user_data[4];    /* codec-specific parameters */
} DWGLS_CodecCtx;

/* ── Default context for 20736 3-axis layout ─────────────────── */
static inline DWGLS_CodecCtx dwgls_ctx_default(uint32_t scale)
{
    DWGLS_CodecCtx ctx;
    ctx.total_slots  = DWGLS_TOTAL_SLOTS;
    ctx.scale_factor = scale;
    ctx.x_slots      = DWGLS_TOTAL_SLOTS / 3;  /* 6912 */
    ctx.y_slots      = DWGLS_TOTAL_SLOTS / 3;  /* 6912 */
    ctx.z_slots      = DWGLS_TOTAL_SLOTS - ctx.x_slots - ctx.y_slots; /* 6912 */
    memset(ctx.user_data, 0, sizeof(ctx.user_data));
    return ctx;
}

/* ════════════════════════════════════════════════════════════════
   CODEC INFO
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;         /* "tess", "gcube", etc. */
    uint8_t     codec_id;     /* CODEC_* constant */
    uint8_t     min_version;  /* minimum shell version needed */
    uint32_t    flags;        /* capability flags (see below) */
} DWGLS_CodecInfo;

/* ── Codec Flags ─────────────────────────────────────────────── */
#define CODEC_FLAG_MULTI_TENSOR   (1u << 0)  /* supports multiple tensors */
#define CODEC_FLAG_MMAP_FRIENDLY  (1u << 1)  /* payload is mmap-able */
#define CODEC_FLAG_SEQUENTIAL     (1u << 2)  /* optimized for seq access */
#define CODEC_FLAG_RANDOM_ACCESS  (1u << 3)  /* optimized for random access */
#define CODEC_FLAG_COMPRESSED     (1u << 4)  /* payload may be smaller */
#define CODEC_FLAG_DERIVED_VIEWS  (1u << 5)  /* generates views at runtime */

/* ════════════════════════════════════════════════════════════════
   CODEC VTABLE
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    /* ── Metadata ────────────────────────────────────────────── */
    DWGLS_CodecInfo (*info)(void);

    /* ── Encode: raw weights → codec payload ────────────────────
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

    /* ── Decode: codec payload → raw weights ────────────────────
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

    /* ── Size: compute encoded size without encoding ────────────
     *   n_elems:  number of elements
     *   ctx:      codec context
     *   Returns:  payload size in bytes, 0 if unknown
     */
    uint32_t (*payload_size)(uint32_t n_elems,
                             const DWGLS_CodecCtx *ctx);

    /* ── Verify: check payload integrity ────────────────────────
     *   src:      codec payload
     *   src_len:  payload bytes
     *   Returns:  0=ok, negative=corrupt
     */
    int (*verify)(const void *src, uint32_t src_len);

    /* ── Resolve: address mapping (for codecs with geometry) ────
     *   slot:     input slot (0..20735)
     *   ctx:      codec context (scale, formula params)
     *   Returns:  resolved address in payload space
     */
    uint32_t (*resolve)(uint32_t slot, const DWGLS_CodecCtx *ctx);

} DWGLS_CodecVtable;

/* ════════════════════════════════════════════════════════════════
   CODEC REGISTRY (static, no malloc)
   ════════════════════════════════════════════════════════════════ */

/* ── Built-in codec vtables (defined in codec_*.h files) ──────── */
extern const DWGLS_CodecVtable DWGLS_CODEC_RAW;
/* extern const DWGLS_CodecVtable DWGLS_CODEC_KIS_FRAME;  */
/* extern const DWGLS_CodecVtable DWGLS_CODEC_KIS_4D;     */
/* extern const DWGLS_CodecVtable DWGLS_CODEC_TESSERACT;  */
/* extern const DWGLS_CodecVtable DWGLS_CODEC_GCUBE;      */
/* extern const DWGLS_CodecVtable DWGLS_CODEC_BEAM_ENTROPY;*/
/* extern const DWGLS_CodecVtable DWGLS_CODEC_TESS;       */
/* extern const DWGLS_CodecVtable DWGLS_CODEC_KIS_V6;     */
/* extern const DWGLS_CodecVtable DWGLS_CODEC_DIAMOND_FIELD;*/

/* ── Lookup by codec_id ────────────────────────────────────────
 * Returns NULL if codec_id is not registered.
 * O(N) scan but N ≤ 16 — trivial cost.
 */
static inline const DWGLS_CodecVtable* dwgls_codec_find(uint8_t codec_id)
{
    /* Static array — compile-time known, no malloc */
    static const DWGLS_CodecVtable *registry[] = {
        &DWGLS_CODEC_RAW,           /* 0 = CODEC_NONE */
    };
    /* TODO: register more codecs as they are implemented */
    const uint32_t n = sizeof(registry) / sizeof(registry[0]);
    if (codec_id < n)
        return registry[codec_id];
    return NULL;
}

/* ════════════════════════════════════════════════════════════════
   RAW CODEC (passthrough, codec_id = 0)
   ════════════════════════════════════════════════════════════════ */

static DWGLS_CodecInfo raw_info(void)
{
    DWGLS_CodecInfo info;
    info.name        = "raw";
    info.codec_id    = CODEC_NONE;
    info.min_version = 1;
    info.flags       = CODEC_FLAG_MMAP_FRIENDLY | CODEC_FLAG_RANDOM_ACCESS;
    return info;
}

static int32_t raw_encode(const void *src, uint32_t n_elems,
                           const DWGLS_CodecCtx *ctx,
                           void *dst, uint32_t dst_cap)
{
    (void)ctx;
    uint32_t bytes = n_elems * sizeof(uint8_t);
    if (bytes > dst_cap) return -1;
    memcpy(dst, src, bytes);
    return (int32_t)bytes;
}

static int32_t raw_decode(const void *src, uint32_t src_len,
                           const DWGLS_CodecCtx *ctx,
                           void *dst, uint32_t dst_cap)
{
    (void)ctx;
    if (src_len > dst_cap) return -1;
    memcpy(dst, src, src_len);
    return (int32_t)src_len;
}

static uint32_t raw_payload_size(uint32_t n_elems,
                                  const DWGLS_CodecCtx *ctx)
{
    (void)ctx;
    return n_elems * sizeof(uint8_t);
}

static int raw_verify(const void *src, uint32_t src_len)
{
    (void)src; (void)src_len;
    return 0;  /* raw data is always "valid" */
}

static uint32_t raw_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
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

/* ════════════════════════════════════════════════════════════════
   UNIVERSAL OPEN (auto-detect shell or legacy format)
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    DWGLS_Shell              shell;
    const DWGLS_CodecVtable *codec;
    const uint8_t           *payload;    /* pointer into mmap or fread buffer */
    uint32_t                 payload_len;
} DWGLS_File;

/* ── dwgls_open ────────────────────────────────────────────────
 * Open a DWGLS file from a memory buffer.
 * Auto-detects DWGLS shell and legacy formats.
 * Returns 0=ok, negative=error.
 */
static inline int dwgls_open(DWGLS_File *f, const void *data, uint32_t len)
{
    if (!f || !data || len < DWGLS_SHELL_SZ) return -1;

    /* Try DWGLS shell first */
    const DWGLS_Shell *shell = (const DWGLS_Shell *)data;
    if (dwgls_shell_validate(shell) == 0) {
        memcpy(&f->shell, shell, sizeof(DWGLS_Shell));
        f->codec = dwgls_codec_find(shell->codec_id);
        f->payload = (const uint8_t *)data + DWGLS_SHELL_SZ;
        f->payload_len = shell->payload_size;
        return f->codec ? 0 : -3;  /* unknown codec */
    }

    /* Legacy format detection via magic sniffing */
    /* Use base pointer to avoid -Warray-bounds on small test buffers */
    const uint8_t *base = (const uint8_t *)data;
    if (len >= 4) {
        const uint32_t magic32 = *(const uint32_t *)base;

        switch (magic32) {
            /* .tess format: magic = 0x54455353 ("TESS") */
            case 0x54455353u: {
                f->shell.magic        = DWGLS_SHELL_MAGIC;
                f->shell.version      = DWGLS_SHELL_VERSION;
                f->shell.codec_id     = CODEC_TESS;
                f->shell.integrity    = INTEGRITY_CRC64;
                f->shell.total_slots  = DWGLS_TOTAL_SLOTS;
                f->shell.scale_factor = 65536u;  /* default scale=1.0 */
                f->shell.cell_size    = 1u;      /* will be updated from TESS_Header */
                f->payload            = base + 64; /* skip TESS_Header */
                f->payload_len        = len - 64;
                f->shell.payload_size = f->payload_len;
                f->codec = dwgls_codec_find(CODEC_TESS);
                return f->codec ? 0 : -3;
            }

            /* .gcube format: magic = 0x00424347 ("GCB\0") */
            case 0x00424347u: {
                f->shell.magic        = DWGLS_SHELL_MAGIC;
                f->shell.version      = DWGLS_SHELL_VERSION;
                f->shell.codec_id     = CODEC_GCUBE;
                f->shell.integrity    = INTEGRITY_CRC32;
                f->shell.total_slots  = DWGLS_TOTAL_SLOTS;
                f->shell.scale_factor = 65536u;
                f->payload            = base + 64; /* skip GCubeFileHeader */
                f->payload_len        = len - 64;
                f->shell.payload_size = f->payload_len;
                f->codec = dwgls_codec_find(CODEC_GCUBE);
                return f->codec ? 0 : -3;
            }

            default:
                break;
        }

        /* Check for 8-byte magic (KIS) */
        if (len >= 8) {
            const uint64_t magic64 = *(const uint64_t *)data;
            if (magic64 == UINT64_C(0x4B4953004B4953)) {
                /* "KIS\0KIS" — geo_kis_container */
                f->shell.magic        = DWGLS_SHELL_MAGIC;
                f->shell.version      = DWGLS_SHELL_VERSION;
                f->shell.codec_id     = CODEC_KIS_FRAME;
                f->shell.integrity    = INTEGRITY_CRC64;
                f->shell.total_slots  = DWGLS_TOTAL_SLOTS;
                f->shell.scale_factor = 65536u;
                f->payload            = (const uint8_t *)data + 24; /* skip KisHeader */
                f->payload_len        = len - 24;
                f->shell.payload_size = f->payload_len;
                f->codec = dwgls_codec_find(CODEC_KIS_FRAME);
                return f->codec ? 0 : -3;
            }
        }
    }

    return -2;  /* unrecognized format */
}

#endif /* DWGLS_CODEC_H */
