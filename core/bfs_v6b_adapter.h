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

/* Table-driven CRC32 (polynomial 0xEDB88320) — values bit-identical to the
 * former bit-by-bit loop, ~10x faster. The v6b checksum field is write-only
 * metadata (stored, never compared), but encode + every read path recompute
 * it, so per-byte cost dominates full-model streaming (card #46 real proof). */
static uint32_t v6b_crc32_tab[256];
static int v6b_crc32_tab_ready = 0;

static inline void v6b_crc32_tab_init(void) {
    if (v6b_crc32_tab_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (V6B_DC_CRC_POLY & (-(int32_t)(c & 1)));
        v6b_crc32_tab[i] = c;
    }
    v6b_crc32_tab_ready = 1;
}

static inline uint32_t v6b_dc_crc32(const uint8_t *data, uint32_t len) {
    v6b_crc32_tab_init();
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++)
        crc = v6b_crc32_tab[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
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
