/*
 * geo_kis_container.h — KIS Container Format for Adaptive Storage
 * ════════════════════════════════════════════════════════════════
 *
 * Binary container for serializing adaptive-encoded weight data
 * on the kis-timeline (stride-37 Fibo 1440).
 *
 * Layout:
 *   Header[24B] + Payload[variable] + CRC[8B]
 *
 * Header (24 bytes, packed):
 *   magic      uint64_t   0x4B4953004B4953  ("KIS\0KIS")
 *   version    uint8_t    1
 *   tier       uint8_t    0..3
 *   entropy    uint8_t    0..255
 *   frame_cnt  uint8_t    number of frame slots
 *   block_cnt  uint16_t   number of DiamondBlocks
 *   weight_cnt uint32_t   total weight count
 *   reserved   uint32_t   0
 *
 * Payload (variable):
 *   frame_slots[frame_cnt] : uint16_t each (enc values)
 *   blocks[block_cnt]      : 64 floats each (DiamondBlocks)
 *
 * CRC tail (8 bytes):
 *   uint64_t CRC-64/ECMA over header + payload
 *
 * Geometry-data separation:
 *   Frame operations live in geometry space (geo_frame_seek.h).
 *   Container serialization lives in data space (this header).
 *
 * No malloc. No float. Stateless O(1).
 * ════════════════════════════════════════════════════════════════
 */

#ifndef GEO_KIS_CONTAINER_H
#define GEO_KIS_CONTAINER_H

#include <stdint.h>
#include <string.h>
#include "geo_frame_seek.h"
#include "geo_adaptive_store.h"

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define KIS_MAGIC           UINT64_C(0x4B4953004B4953)  /* "KIS\0KIS" LE */
#define KIS_VERSION         1
#define KIS_HEADER_SZ       24u
#define KIS_CRC_SZ           8u
#define KIS_BLOCK_WORDS      64u   /* floats per DiamondBlock */
#define KIS_MAX_FRAMES      89u   /* max frame slots (from AdaptiveStore) */
#define KIS_MAX_BLOCKS     768u   /* 12 × 64 from AdaptiveStore */

/* CRC-64/ECMA: polynomial 0x42F0E1EBA9EA3693, init=F...F, xorout=F...F */
#define KIS_CRC64_POLY      UINT64_C(0x42F0E1EBA9EA3693)

/* ══════════════════════════════════════════════════════════════
   HEADER STRUCT
   ══════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    uint64_t  magic;       /* must be KIS_MAGIC                  */
    uint8_t   version;     /* must be KIS_VERSION                */
    uint8_t   tier;        /* 0..3                               */
    uint8_t   entropy;     /* 0..255                             */
    uint8_t   frame_cnt;   /* number of frame slots in payload  */
    uint16_t  block_cnt;   /* number of DiamondBlocks in payload */
    uint16_t  _pad0;       /* alignment padding (always 0)       */
    uint32_t  weight_cnt;  /* total weight count                 */
    uint32_t  reserved;    /* 0                                  */
} KisHeader;
#pragma pack(pop)

/* ══════════════════════════════════════════════════════════════
   CRC-64/ECMA
   ══════════════════════════════════════════════════════════════ */

static inline uint64_t kis_crc64(const uint8_t *data, uint32_t len)
{
    uint64_t crc = UINT64_C(0xFFFFFFFFFFFFFFFF);  /* ECMA: init */
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint64_t)data[i] << 56;
        for (int j = 0; j < 8; j++) {
            crc = (crc & (UINT64_C(1) << 63))
                ? ((crc << 1) ^ KIS_CRC64_POLY)
                :  (crc << 1);
        }
    }
    return crc ^ UINT64_C(0xFFFFFFFFFFFFFFFF);  /* ECMA: xorout */
}

/* ══════════════════════════════════════════════════════════════
   CONTAINER FUNCTIONS
   ══════════════════════════════════════════════════════════════ */

/* ── kis_container_init ─────────────────────────────────────
 * Init container header from AdaptiveStore state.
 * Copies tier, entropy, frame_count, block_count, total_weight_count.
 */
static inline void kis_container_init(KisHeader *hdr,
                                      const AdaptiveStore *as)
{
    hdr->magic       = KIS_MAGIC;
    hdr->version     = KIS_VERSION;
    hdr->tier        = as->tier;
    hdr->entropy     = as->entropy_score;
    hdr->frame_cnt   = as->frame_count;
    hdr->block_cnt   = as->block_count;
    hdr->weight_cnt  = as->total_weight_count;
    hdr->reserved    = 0;
    hdr->_pad0       = 0;
}

/* ── kis_container_size ─────────────────────────────────────
 * Compute total serialized size in bytes.
 *   header(24) + frame_slots(frame_cnt × 2) + blocks(block_cnt × 256) + crc(8)
 */
static inline uint32_t kis_container_size(const KisHeader *hdr)
{
    uint32_t payload = (uint32_t)hdr->frame_cnt * sizeof(uint16_t)
                     + (uint32_t)hdr->block_cnt * KIS_BLOCK_WORDS * sizeof(float);
    return KIS_HEADER_SZ + payload + KIS_CRC_SZ;
}

/* ── kis_container_serialize ────────────────────────────────
 * Serialize: write header + payload to buffer.
 * buf_cap must be >= kis_container_size(hdr).
 * CRC64 is appended at the tail.
 * Returns total bytes written, or -1 on insufficient capacity.
 */
static inline int kis_container_serialize(const KisHeader *hdr,
                                          const uint16_t *frame_slots,
                                          const float *blocks,
                                          uint8_t *buf,
                                          uint32_t buf_cap)
{
    uint32_t total = kis_container_size(hdr);
    if (buf_cap < total) return -1;

    uint32_t off = 0;

    /* header */
    memcpy(buf + off, hdr, KIS_HEADER_SZ);
    off += KIS_HEADER_SZ;

    /* frame slots */
    uint32_t fsz = (uint32_t)hdr->frame_cnt * sizeof(uint16_t);
    memcpy(buf + off, frame_slots, fsz);
    off += fsz;

    /* blocks */
    uint32_t bsz = (uint32_t)hdr->block_cnt * KIS_BLOCK_WORDS * sizeof(float);
    memcpy(buf + off, blocks, bsz);
    off += bsz;

    /* CRC64 over header + payload */
    uint64_t crc = kis_crc64(buf, off);
    memcpy(buf + off, &crc, sizeof(crc));
    off += sizeof(crc);

    return (int)off;
}

/* ── kis_container_deserialize ──────────────────────────────
 * Deserialize: read header from buffer.
 * Validates magic and version.
 * Returns 0 on success, -1 on bad magic, -2 on bad version,
 * -3 on buffer too short.
 */
static inline int kis_container_deserialize(KisHeader *hdr,
                                            const uint8_t *buf,
                                            uint32_t buf_len)
{
    if (buf_len < KIS_HEADER_SZ) return -3;

    memcpy(hdr, buf, KIS_HEADER_SZ);

    if (hdr->magic != KIS_MAGIC) return -1;
    if (hdr->version != KIS_VERSION) return -2;
    return 0;
}

/* ── kis_container_verify ───────────────────────────────────
 * Verify: check magic + CRC integrity.
 * Returns 0 on pass, -1 on bad magic, -2 on bad version,
 * -3 on CRC mismatch, -4 on buffer too short.
 */
static inline int kis_container_verify(const uint8_t *buf,
                                       uint32_t buf_len)
{
    if (buf_len < KIS_HEADER_SZ + KIS_CRC_SZ) return -4;

    KisHeader hdr;
    memcpy(&hdr, buf, KIS_HEADER_SZ);

    if (hdr.magic != KIS_MAGIC) return -1;
    if (hdr.version != KIS_VERSION) return -2;

    /* compute expected total size to find CRC offset */
    uint32_t total = kis_container_size(&hdr);
    if (buf_len < total) return -4;

    /* CRC covers header + payload (everything before the last 8 bytes) */
    uint32_t crc_region = total - KIS_CRC_SZ;
    uint64_t computed = kis_crc64(buf, crc_region);

    uint64_t stored;
    memcpy(&stored, buf + crc_region, sizeof(stored));

    if (computed != stored) return -3;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   ROUND-TRIP HELPERS
   ══════════════════════════════════════════════════════════════ */

/* ── kis_extract_frame_slots ────────────────────────────────
 * Get pointer to frame slots within a serialized buffer.
 * Caller must have verified integrity first.
 */
static inline const uint16_t *kis_extract_frame_slots(const uint8_t *buf)
{
    return (const uint16_t *)(buf + KIS_HEADER_SZ);
}

/* ── kis_extract_blocks ─────────────────────────────────────
 * Get pointer to DiamondBlocks within a serialized buffer.
 * Caller must have verified integrity first.
 */
static inline const float *kis_extract_blocks(const KisHeader *hdr,
                                              const uint8_t *buf)
{
    uint32_t frame_data_sz = (uint32_t)hdr->frame_cnt * sizeof(uint16_t);
    return (const float *)(buf + KIS_HEADER_SZ + frame_data_sz);
}

#endif /* GEO_KIS_CONTAINER_H */
