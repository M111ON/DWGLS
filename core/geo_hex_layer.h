/*
 * geo_hex_layer.h — hex_codec bridge for GPX4_LAYER_GEO
 *
 * Maps HexTile classification + encoded residuals into Gpx4GeoAddr.sub field
 */

#ifndef GEO_HEX_LAYER_H
#define GEO_HEX_LAYER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hex_tile.h"

/* ── sub field packing ──────────────────────────────────────── */
#define GEO_HEX_TYPE_SHIFT   12
#define GEO_HEX_DIFF_SHIFT    8
#define GEO_HEX_TYPE_MASK  0x3u
#define GEO_HEX_DIFF_MASK  0xFu
#define GEO_HEX_VAL_MASK  0xFFu

static inline uint8_t _ghex_type2bit(uint8_t cls) {
    switch(cls) {
        case HENC_FLAT:     return 0;
        case HENC_TRIPLET_FLAT:   return 1;
        case HENC_GRADIENT: return 2;
        default:            return 3;
    }
}

static inline uint8_t _ghex_bit2type(uint8_t b) {
    static const uint8_t map[4] = {
        HENC_FLAT, HENC_TRIPLET_FLAT, HENC_GRADIENT, HENC_EDGE
    };
    return map[b & 3];
}

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
    return (uint8_t)((sum / 6) >> 4) & 0xF;
}

/* ── encode: HexTile → sub bits ─────────────────────────────── */
static inline uint16_t gpx4_geo_hex_encode_sub(const HexTile *t) {
    uint8_t cls   = hex_tile_classify(t);
    uint8_t type2 = _ghex_type2bit(cls);
    uint8_t diff4 = _ghex_xor_diff(t);
    uint8_t cval  = t->c[6];

    return (uint16_t)(
        ((uint16_t)(type2 & GEO_HEX_TYPE_MASK) << GEO_HEX_TYPE_SHIFT) |
        ((uint16_t)(diff4 & GEO_HEX_DIFF_MASK) << GEO_HEX_DIFF_SHIFT) |
        ((uint16_t)(cval  & GEO_HEX_VAL_MASK))
    );
}

/* ── decode: sub bits → tile_type + center_val ─────────────── */
typedef struct {
    uint8_t tile_type;
    uint8_t xor_diff;
    uint8_t center_val;
} GeoHexInfo;

static inline GeoHexInfo gpx4_geo_hex_decode_sub(uint16_t sub) {
    GeoHexInfo info;
    info.tile_type  = _ghex_bit2type((sub >> GEO_HEX_TYPE_SHIFT) & GEO_HEX_TYPE_MASK);
    info.xor_diff   = (sub >> GEO_HEX_DIFF_SHIFT) & GEO_HEX_DIFF_MASK;
    info.center_val = (uint8_t)(sub & GEO_HEX_VAL_MASK);
    return info;
}

/* ── fast type query ─────────────────────────────────────── */
static inline uint8_t gpx4_geo_hex_type(uint32_t packed) {
    uint16_t sub = (uint16_t)(packed & 0x3FFFu);
    return _ghex_bit2type((sub >> GEO_HEX_TYPE_SHIFT) & GEO_HEX_TYPE_MASK);
}

static inline uint8_t gpx4_geo_hex_center(uint32_t packed) {
    return (uint8_t)(packed & GEO_HEX_VAL_MASK);
}

#endif /* GEO_HEX_LAYER_H */
