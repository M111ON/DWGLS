/* ═══════════════════════════════════════════════════════════════════════════
 * dwgls_shell.h — Universal Container Shell for DWGLS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 32 bytes. Fixed. Every DWGLS file starts with this.
 * The shell identifies the container, declares geometry, and validates integrity.
 * The codec module handles everything after byte 32.
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef DWGLS_SHELL_H
#define DWGLS_SHELL_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define DWGLS_SHELL_MAGIC    0x4457474Cu  /* "DWGL" — DWGLS universal */
#define DWGLS_SHELL_VERSION  1u
#define DWGLS_SHELL_SZ       32u

/* ── Codec IDs (registered, not magic-numbered) ──────────────── */
#define CODEC_NONE           0u   /* raw / passthrough */
#define CODEC_KIS_FRAME      1u   /* geo_kis_container: frame+block payload */
#define CODEC_KIS_4D         2u   /* geo_kis_4d_container: 3-axis resolve */
#define CODEC_TESSERACT      3u   /* tesseract_container: 8-octant views */
#define CODEC_GCUBE          4u   /* geo_cube_container: multi-tensor blocks */
#define CODEC_BEAM_ENTROPY   5u   /* beam_entropy_container: BECCoord 8-bit */
#define CODEC_TESS           6u   /* geo_tess_container: stride-37, .tess */
#define CODEC_KIS_CODEC_V6   7u   /* kis_codec_v6: sort+mask+codebook */
#define CODEC_DIAMOND_FIELD  8u   /* geo_diamond_field_v4: 5-path adaptive */
#define CODEC_USER_START     64u  /* user-defined codecs start here */

/* ── Integrity modes ─────────────────────────────────────────── */
#define INTEGRITY_NONE       0u
#define INTEGRITY_CRC32      1u   /* ISO 3309 / ITU-T V.42 */
#define INTEGRITY_CRC64      2u   /* ECMA-182 */
#define INTEGRITY_XXH64      3u   /* xxHash64 (fast, non-crypto) */

/* ═══════════════════════════════════════════════════════════════
   SHELL HEADER (32 bytes packed)
   ═══════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    /* ── Identification (8 bytes) ────────────────────────────── */
    uint32_t magic;           /* 0x4457474C = "DWGL"               */
    uint16_t version;         /* shell format version (1)           */
    uint8_t  codec_id;        /* CODEC_* enum — what's inside       */
    uint8_t  integrity;       /* INTEGRITY_* — how to validate      */

    /* ── Geometry (8 bytes) ──────────────────────────────────── */
    uint32_t total_slots;     /* 20736 (sacred) or 0 if unknown     */
    uint32_t scale_factor;    /* fixed-point: scale × 65536         */

    /* ── Layout (8 bytes) ────────────────────────────────────── */
    uint32_t payload_size;    /* bytes after shell (codec-dependent) */
    uint32_t cell_size;       /* bytes per cell (1, 2, 4, 18, 34)   */

    /* ── Seal (8 bytes) ──────────────────────────────────────── */
    uint64_t checksum;        /* CRC/CRC64/XXH64 of shell+payload   */
} DWGLS_Shell;
#pragma pack(pop)

/* ═══════════════════════════════════════════════════════════════
   CRC-64/ECMA (polynomial 0x42F0E1EBA9EA3693, init=F...F, xorout=F...F)
   ═══════════════════════════════════════════════════════════════ */

#define DWGLS_CRC64_POLY  UINT64_C(0x42F0E1EBA9EA3693)

static inline uint64_t dwgls_crc64(const uint8_t *data, uint32_t len)
{
    uint64_t crc = UINT64_C(0xFFFFFFFFFFFFFFFF);  /* ECMA: init */
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint64_t)data[i] << 56;
        for (int j = 0; j < 8; j++) {
            crc = (crc & (UINT64_C(1) << 63))
                ? ((crc << 1) ^ DWGLS_CRC64_POLY)
                :  (crc << 1);
        }
    }
    return crc ^ UINT64_C(0xFFFFFFFFFFFFFFFF);  /* ECMA: xorout */
}

/* ═══════════════════════════════════════════════════════════════
   SHELL FUNCTIONS
   ═══════════════════════════════════════════════════════════════ */

/* ── shell_init ────────────────────────────────────────────────
 * Initialize shell from parameters. Zeroes reserved fields.
 */
static inline void dwgls_shell_init(DWGLS_Shell *s,
                                     uint8_t codec,
                                     uint32_t total_slots,
                                     uint32_t scale,
                                     uint32_t payload_sz,
                                     uint32_t cell_sz,
                                     uint8_t integrity_mode)
{
    s->magic        = DWGLS_SHELL_MAGIC;
    s->version      = DWGLS_SHELL_VERSION;
    s->codec_id     = codec;
    s->integrity    = integrity_mode;
    s->total_slots  = total_slots;
    s->scale_factor = scale;
    s->payload_size = payload_sz;
    s->cell_size    = cell_sz;
    s->checksum     = 0;  /* computed after payload is written */
}

/* ── shell_validate ────────────────────────────────────────────
 * Check magic + version. Returns 0=ok, -1=bad magic, -2=bad version.
 */
static inline int dwgls_shell_validate(const DWGLS_Shell *s)
{
    if (s->magic != DWGLS_SHELL_MAGIC) return -1;
    if (s->version > DWGLS_SHELL_VERSION) return -2;
    return 0;
}

/* ── shell_total_size ──────────────────────────────────────────
 * Total file size = shell(32) + payload.
 */
static inline uint32_t dwgls_shell_total_size(const DWGLS_Shell *s)
{
    return DWGLS_SHELL_SZ + s->payload_size;
}

/* ── shell_codec_name ──────────────────────────────────────────
 * Human-readable codec name.
 */
static inline const char* dwgls_shell_codec_name(uint8_t codec)
{
    switch (codec) {
        case CODEC_NONE:          return "raw";
        case CODEC_KIS_FRAME:     return "kis_frame";
        case CODEC_KIS_4D:        return "kis_4d";
        case CODEC_TESSERACT:     return "tesseract";
        case CODEC_GCUBE:         return "gcube";
        case CODEC_BEAM_ENTROPY:  return "beam_entropy";
        case CODEC_TESS:          return "tess";
        case CODEC_KIS_CODEC_V6:  return "kis_v6";
        case CODEC_DIAMOND_FIELD: return "diamond_field";
        default:                  return "user_defined";
    }
}

/* ── shell_compute_checksum ────────────────────────────────────
 * Compute checksum over shell (with checksum=0) + payload.
 * Returns the checksum value.
 */
static inline uint64_t dwgls_shell_compute_checksum(const DWGLS_Shell *s,
                                                     const void *payload)
{
    uint64_t crc = UINT64_C(0xFFFFFFFFFFFFFFFF);

    /* Hash shell with checksum field zeroed */
    const uint8_t *p = (const uint8_t *)s;
    for (uint32_t i = 0; i < DWGLS_SHELL_SZ; i++) {
        uint8_t byte = (i >= 24 && i < 32) ? 0 : p[i];  /* zero out checksum field (bytes 24-31) */
        crc ^= (uint64_t)byte << 56;
        for (int j = 0; j < 8; j++) {
            crc = (crc & (UINT64_C(1) << 63))
                ? ((crc << 1) ^ DWGLS_CRC64_POLY)
                :  (crc << 1);
        }
    }

    /* Hash payload */
    if (payload && s->payload_size) {
        p = (const uint8_t *)payload;
        for (uint32_t i = 0; i < s->payload_size; i++) {
            crc ^= (uint64_t)p[i] << 56;
            for (int j = 0; j < 8; j++) {
                crc = (crc & (UINT64_C(1) << 63))
                    ? ((crc << 1) ^ DWGLS_CRC64_POLY)
                    :  (crc << 1);
            }
        }
    }

    return crc ^ UINT64_C(0xFFFFFFFFFFFFFFFF);
}

/* ── shell_verify_integrity ────────────────────────────────────
 * Verify shell magic + checksum over entire file (shell + payload).
 * Returns 0 on pass, negative on error:
 *   -1: bad magic
 *   -2: bad version
 *   -3: checksum mismatch
 *   -4: buffer too short
 */
static inline int dwgls_shell_verify_integrity(const DWGLS_Shell *s,
                                                const void *payload,
                                                uint32_t payload_len)
{
    if (payload_len < s->payload_size) return -4;

    int v = dwgls_shell_validate(s);
    if (v != 0) return v;

    /* Compute checksum over shell (with zeroed checksum) + payload */
    uint64_t crc = UINT64_C(0xFFFFFFFFFFFFFFFF);
    const uint8_t *p = (const uint8_t *)s;

    for (uint32_t i = 0; i < DWGLS_SHELL_SZ; i++) {
        uint8_t byte = (i >= 24 && i < 32) ? 0 : p[i];  /* zero out checksum field */
        crc ^= (uint64_t)byte << 56;
        for (int j = 0; j < 8; j++) {
            crc = (crc & (UINT64_C(1) << 63))
                ? ((crc << 1) ^ DWGLS_CRC64_POLY)
                :  (crc << 1);
        }
    }

    p = (const uint8_t *)payload;
    for (uint32_t i = 0; i < s->payload_size; i++) {
        crc ^= (uint64_t)p[i] << 56;
        for (int j = 0; j < 8; j++) {
            crc = (crc & (UINT64_C(1) << 63))
                ? ((crc << 1) ^ DWGLS_CRC64_POLY)
                :  (crc << 1);
        }
    }
    crc ^= UINT64_C(0xFFFFFFFFFFFFFFFF);

    if (crc != s->checksum) return -3;
    return 0;
}

#endif /* DWGLS_SHELL_H */