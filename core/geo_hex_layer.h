#pragma once
/*
 * geo_hex_layer.h — hex_codec bridge for GPX4_LAYER_GEO
 *
 * Maps HexTile classification + encoded residuals into Gpx4GeoAddr.sub field
 *
 * sub field layout (14-bit):
 *   [13..12]  tile_type   2b  FLAT=0 SMOOTH=1 GRADIENT=2 EDGE=3
 *   [11.. 8]  xor_diff    4b  avg XOR diff >> 4 (0-15, geometry sensor)
 *   [ 7.. 0]  center_val  8b  center cell raw value (anchor for decode)
 *
 * Usage:
 *   gpx4_geo_hex_encode()  — HexTile → pack into Gpx4GeoAddr.sub
 *   gpx4_geo_hex_decode()  — Gpx4GeoAddr.sub → recover tile_type + center
 *   gpx4_geo_hex_write()   — encode all tiles → write GEOA layer data
 *   gpx4_geo_hex_read()    — read GEOA layer → decode all tiles
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hex_codec.h"
#include "gpx4_container.h"

// ── sub field packing ────────────────────────────────────────

#define GEO_HEX_TYPE_SHIFT   12
#define GEO_HEX_DIFF_SHIFT    8
#define GEO_HEX_TYPE_MASK  0x3u
#define GEO_HEX_DIFF_MASK  0xFu
#define GEO_HEX_VAL_MASK  0xFFu

// type → 2-bit
static inline uint8_t _ghex_type2bit(uint8_t cls) {
    switch(cls) {
        case HENC_FLAT:     return 0;
        case HENC_SMOOTH:   return 1;
        case HENC_GRADIENT: return 2;
        default:            return 3;  // EDGE
    }
}

static inline uint8_t _ghex_bit2type(uint8_t b) {
    static const uint8_t map[4] = {
        HENC_FLAT, HENC_SMOOTH, HENC_GRADIENT, HENC_EDGE
    };
    return map[b & 3];
}

// compute avg XOR diff (same logic as hex_tile_classify internals)
static inline uint8_t _ghex_xor_diff(const HexTile *t) {
    uint8_t center = t->c[6];
    uint32_t sum = 0;
    uint8_t prev = center;
    for (int rp = 0; rp < 6; rp++) {
        uint8_t b = (rp >= 2) ? t->c[rp-2] : center;
        uint8_t cyl = (prev + b) >> 1;
        sum += (uint8_t)(cyl ^ center);
        prev = t->c[rp];
    }
    return (uint8_t)((sum / 6) >> 4) & 0xF;  // 4-bit (0-15)
}

// ── encode: HexTile → sub bits ───────────────────────────────

static inline uint16_t gpx4_geo_hex_encode_sub(const HexTile *t) {
    uint8_t cls   = hex_tile_classify(t);
    uint8_t type2 = _ghex_type2bit(cls);
    uint8_t diff4 = _ghex_xor_diff(t);
    uint8_t cval  = t->c[6];  // center anchor

    return (uint16_t)(
        ((uint16_t)(type2 & GEO_HEX_TYPE_MASK) << GEO_HEX_TYPE_SHIFT) |
        ((uint16_t)(diff4 & GEO_HEX_DIFF_MASK) << GEO_HEX_DIFF_SHIFT) |
        ((uint16_t)(cval  & GEO_HEX_VAL_MASK))
    );
}

// ── decode: sub bits → tile_type + center_val ───────────────

typedef struct {
    uint8_t tile_type;    // HENC_*
    uint8_t xor_diff;     // 0-15 geometry sensor
    uint8_t center_val;   // center cell anchor
} GeoHexInfo;

static inline GeoHexInfo gpx4_geo_hex_decode_sub(uint16_t sub) {
    GeoHexInfo info;
    info.tile_type  = _ghex_bit2type((sub >> GEO_HEX_TYPE_SHIFT) & GEO_HEX_TYPE_MASK);
    info.xor_diff   = (sub >> GEO_HEX_DIFF_SHIFT) & GEO_HEX_DIFF_MASK;
    info.center_val = (uint8_t)(sub & GEO_HEX_VAL_MASK);
    return info;
}

// ── bulk write: n tiles → GEO layer data buffer ─────────────
// pent_id and hilbert come from caller (existing O21 addressing)
// sub field is derived from hex tile classification

static inline void gpx4_geo_hex_write(
        const HexTile *tiles,    // array of n_tiles
        const uint8_t *pent_ids, // pent_id per tile (4-bit, 0-11)
        const uint16_t *hilberts,// hilbert_local per tile (14-bit)
        uint32_t n_tiles,
        uint8_t *out_buf)        // caller alloc: n_tiles * GPX4_GEO_ADDR_SZ
{
    for (uint32_t i = 0; i < n_tiles; i++) {
        uint16_t sub = gpx4_geo_hex_encode_sub(&tiles[i]);
        uint32_t packed =
            ((uint32_t)(pent_ids[i]  & 0xFu)     << 28) |
            ((uint32_t)(hilberts[i]  & 0x3FFFu)  << 14) |
            ((uint32_t)(sub          & 0x3FFFu));
        // big-endian write
        out_buf[i*4+0] = (uint8_t)(packed >> 24);
        out_buf[i*4+1] = (uint8_t)(packed >> 16);
        out_buf[i*4+2] = (uint8_t)(packed >>  8);
        out_buf[i*4+3] = (uint8_t)(packed);
    }
}

// ── bulk read: GEO layer data → GeoHexInfo array ────────────

static inline void gpx4_geo_hex_read(
        const uint8_t *buf,
        uint32_t n_tiles,
        GeoHexInfo *out_info,    // caller alloc: n_tiles
        uint8_t  *out_pents,     // caller alloc: n_tiles (optional, NULL ok)
        uint16_t *out_hilberts)  // caller alloc: n_tiles (optional, NULL ok)
{
    for (uint32_t i = 0; i < n_tiles; i++) {
        uint32_t packed =
            ((uint32_t)buf[i*4+0] << 24) |
            ((uint32_t)buf[i*4+1] << 16) |
            ((uint32_t)buf[i*4+2] <<  8) |
            ((uint32_t)buf[i*4+3]);

        uint16_t sub = (uint16_t)(packed & 0x3FFFu);
        out_info[i] = gpx4_geo_hex_decode_sub(sub);

        if (out_pents)    out_pents[i]    = (uint8_t)(GPX4_GEO_PENT(packed));
        if (out_hilberts) out_hilberts[i] = (uint16_t)(GPX4_GEO_HILBERT(packed));
    }
}

// ── fast type query: skip full decode ───────────────────────
// useful for tile routing without allocating GeoHexInfo

static inline uint8_t gpx4_geo_hex_type(uint32_t packed) {
    uint16_t sub = (uint16_t)(packed & 0x3FFFu);
    return _ghex_bit2type((sub >> GEO_HEX_TYPE_SHIFT) & GEO_HEX_TYPE_MASK);
}

static inline uint8_t gpx4_geo_hex_center(uint32_t packed) {
    return (uint8_t)(packed & GEO_HEX_VAL_MASK);
}
