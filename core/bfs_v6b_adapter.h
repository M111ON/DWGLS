/* ═══════════════════════════════════════════════════════════════════════════
 * bfs_v6b_adapter.h — v6b codec adapter for breathing_fs (block-level API)
 * ═══════════════════════════════════════════════════════════════════════════
 * Drop-in replacement for DynContainer in breathing_fs.h.
 * Provides: v6b_dc_init, v6b_dc_encode, v6b_dc_decode.
 * Self-contained CRC32 (no dependency on dwgls_dynamic_codec.h).
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef BFS_V6B_ADAPTER_H
#define BFS_V6B_ADAPTER_H

#include "kis_codec_v6b.h"

#define V6B_DC_MAX_ENC  2048u
#define V6B_DC_CRC_POLY 0xEDB88320u

static inline uint32_t v6b_dc_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (V6B_DC_CRC_POLY & (-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFF;
}

typedef struct {
    uint8_t  payload[V6B_DC_MAX_ENC];
    uint32_t payload_size;
    uint32_t strategy;
    uint32_t checksum;
} V6bContainer;

static inline void v6b_dc_init(V6bContainer *dc) {
    if (!dc) return;
    dc->payload_size = 0;
    dc->strategy = 99;
    dc->checksum = 0;
}

static inline int v6b_dc_encode(V6bContainer *dc, const int8_t *data, uint32_t size) {
    if (!dc || !data || size == 0) return -1;
    if (size > V6B_SLOTS) size = V6B_SLOTS;

    v6b_stream_t st = {0};
    if (v6b_init(&st, V6B_Q8) != 0) return -2;
    if (v6b_collect(&st, data, size) != 0) { v6b_free(&st); return -3; }

    uint32_t hdr = v6b_header(&st, dc->payload, V6B_DC_MAX_ENC);
    if (hdr == 0) { v6b_free(&st); return -4; }

    uint32_t off = hdr;
    while (1) {
        uint32_t room = V6B_DC_MAX_ENC - off;
        uint32_t emitted = v6b_emit(&st, dc->payload + off, room);
        if (emitted == 0) break;
        off += emitted;
        if (off >= V6B_DC_MAX_ENC) { v6b_free(&st); return -5; }
    }

    dc->payload_size = off;
    if (off == hdr) { v6b_free(&st); return -6; }
    dc->strategy = 0;   /* v6b container — single codec, strategy 0 */
    dc->checksum = v6b_dc_crc32(dc->payload, dc->payload_size);
    v6b_free(&st);
    return 0;
}

static inline int v6b_dc_decode(const V6bContainer *dc, int8_t *out, uint32_t out_size) {
    if (!dc || !out) return -1;
    uint32_t got = v6b_decode_all(dc->payload, dc->payload_size,
                                   (uint8_t *)out, out_size);
    return (got == out_size) ? 0 : -2;
}

#endif /* BFS_V6B_ADAPTER_H */
