/*
 * dwgls_shell.h — Universal Container Shell
 * ════════════════════════════════════════════════════════════════
 *
 * 32 bytes. Fixed. Every DWGLS file starts with this.
 * The shell identifies the container, declares geometry, and validates integrity.
 * The codec module handles everything after byte 32.
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 *
 * BUILD: gcc -O2 -Wall -Icore -o test_shell tests/test_shell.c -lm
 * DEPENDS: none (self-contained)
 * ════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_SHELL_H
#define DWGLS_SHELL_H

#include <stdint.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════════ */

#define DWGLS_SHELL_MAGIC        0x4457474Cu  /* "DWGL" — DWGLS universal */
#define DWGLS_SHELL_VERSION      1u
#define DWGLS_SHELL_SZ           32u
#define DWGLS_TOTAL_SLOTS        20736u       /* 12^4 = 144^2 */

/* ── Codec IDs (registered, not magic-numbered) ──────────────── */
#define CODEC_NONE               0u   /* raw / passthrough */
#define CODEC_KIS_FRAME          1u   /* geo_kis_container: frame+block payload */
#define CODEC_KIS_4D             2u   /* geo_kis_4d_container: 3-axis resolve */
#define CODEC_TESSERACT          3u   /* tesseract_container: 8-octant views */
#define CODEC_GCUBE              4u   /* geo_cube_container: multi-tensor blocks */
#define CODEC_BEAM_ENTROPY       5u   /* beam_entropy_container: BECCoord 8-bit */
#define CODEC_TESS               6u   /* geo_tess_container: stride-37, .tess */
#define CODEC_KIS_CODEC_V6       7u   /* kis_codec_v6: sort+mask+codebook */
#define CODEC_DIAMOND_FIELD      8u   /* geo_diamond_field_v4: 5-path adaptive */
#define CODEC_USER_START         64u  /* user-defined codecs start here */

/* ── Integrity modes ─────────────────────────────────────────── */
#define INTEGRITY_NONE           0u
#define INTEGRITY_CRC32          1u   /* ISO 3309 / ITU-T V.42 */
#define INTEGRITY_CRC64          2u   /* ECMA-182 */
#define INTEGRITY_XXH64          3u   /* xxHash64 (fast, non-crypto) */

/* ════════════════════════════════════════════════════════════════
   SHELL HEADER (32 bytes, packed)
   ════════════════════════════════════════════════════════════════ */

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

/* ════════════════════════════════════════════════════════════════
   CRC-64/ECMA-182
   Polynomial: 0x42F0E1EBA9EA3693, init=F...F, xorout=F...F
   Same algorithm as geo_kis_container.h kis_crc64().
   ════════════════════════════════════════════════════════════════ */

#define DWGLS_CRC64_POLY  UINT64_C(0x42F0E1EBA9EA3693)

static inline uint64_t dwgls_crc64(const uint8_t *data, uint32_t len)
{
    uint64_t crc = UINT64_C(0xFFFFFFFFFFFFFFFF);
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint64_t)data[i] << 56;
        for (int j = 0; j < 8; j++) {
            crc = (crc & (UINT64_C(1) << 63))
                ? ((crc << 1) ^ DWGLS_CRC64_POLY)
                :  (crc << 1);
        }
    }
    return crc ^ UINT64_C(0xFFFFFFFFFFFFFFFF);
}

/* ════════════════════════════════════════════════════════════════
   CRC-32 (ISO 3309 / ITU-T V.42)
   Same algorithm as tesseract_container.h tess_crc32().
   ════════════════════════════════════════════════════════════════ */

#define DWGLS_CRC32_POLY  0xEDB88320u

static inline uint32_t dwgls_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (-(crc & 1u) & DWGLS_CRC32_POLY);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ════════════════════════════════════════════════════════════════
   SHELL FUNCTIONS
   ════════════════════════════════════════════════════════════════ */

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

/* ── shell_compute_checksum ────────────────────────────────────
 * Compute checksum over entire file (shell + payload).
 * The checksum field in the shell is zeroed before computation
 * to avoid chicken-and-egg dependency.
 * payload_ptr must point to the first byte after the shell.
 */
static inline uint64_t dwgls_shell_compute_checksum(
    const DWGLS_Shell *s, const uint8_t *payload_ptr)
{
    /* Copy shell and zero the checksum field for computation */
    DWGLS_Shell tmp;
    memcpy(&tmp, s, sizeof(DWGLS_Shell));
    tmp.checksum = 0;

    /* CRC over shell (all 32 bytes, with checksum=0) */
    uint64_t crc = dwgls_crc64((const uint8_t *)&tmp, DWGLS_SHELL_SZ);
    /* Extend over payload */
    if (payload_ptr && s->payload_size > 0) {
        uint64_t p_crc = dwgls_crc64(payload_ptr, s->payload_size);
        crc ^= p_crc;  /* combine shell + payload checksums */
    }
    return crc;
}

/* ── shell_verify ──────────────────────────────────────────────
 * Verify checksum. payload_ptr points to first byte after shell.
 * Returns 0=ok, -1=checksum mismatch, -2=no integrity mode.
 */
static inline int dwgls_shell_verify(const DWGLS_Shell *s,
                                      const uint8_t *payload_ptr)
{
    if (s->integrity == INTEGRITY_NONE) return -2;
    uint64_t expected = dwgls_shell_compute_checksum(s, payload_ptr);
    return (expected == s->checksum) ? 0 : -1;
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

/* ── shell_print ───────────────────────────────────────────────
 * Debug dump of shell fields.
 */
static inline void dwgls_shell_print(const DWGLS_Shell *s)
{
    (void)s; /* suppress unused warning in release builds */
    /* Actual print done via printf in test code */
}

#endif /* DWGLS_SHELL_H */
