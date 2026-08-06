/*
 * hex_tile.h — Hex Tile Classification & Encoding
 *
 * 7 cells per tile (6 ring + 1 center)
 * Classification: FLAT, TRIPLET_FLAT, GRADIENT, EDGE
 * Encoding: predict center, store residuals
 */

#ifndef HEX_TILE_H
#define HEX_TILE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define HEX_CELLS    7
#define HEX_CENTER   6
#define HEX_RING     6

#define HENC_FLAT         0x00
#define HENC_TRIPLET_FLAT 0x01
#define HENC_GRADIENT     0x02
#define HENC_EDGE         0x03

static const uint8_t HEX_TRIPLETS[6][3] = {
    {0,1,6},{1,2,6},{2,3,6},{3,4,6},{4,5,6},{5,0,6}
};

typedef struct { uint8_t c[HEX_CELLS]; } HexTile;

static inline int _hex_triplet_flat(const HexTile *t, int ti) {
    return t->c[HEX_TRIPLETS[ti][0]] == t->c[HEX_TRIPLETS[ti][1]] &&
           t->c[HEX_TRIPLETS[ti][1]] == t->c[HEX_TRIPLETS[ti][2]];
}

static inline uint8_t _hex_predict(const HexTile *t) {
    for (int i = 0; i < 6; i++)
        if (_hex_triplet_flat(t, i)) return t->c[HEX_TRIPLETS[i][0]];
    uint8_t r[6];
    for (int i = 0; i < 6; i++) r[i] = t->c[i];
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 6; j++)
            if (r[j] < r[i]) { uint8_t x = r[i]; r[i] = r[j]; r[j] = x; }
    return (r[2] + r[3]) >> 1;
}

static inline uint8_t _hex_classify(const HexTile *t) {
    int allsame = 1;
    for (int i = 1; i < HEX_CELLS; i++)
        if (t->c[i] != t->c[0]) { allsame = 0; break; }
    if (allsame) return HENC_FLAT;
    for (int i = 0; i < 6; i++)
        if (_hex_triplet_flat(t, i)) return HENC_TRIPLET_FLAT;
    uint8_t mn = 255, mx = 0;
    for (int i = 0; i < HEX_CELLS; i++) {
        if (t->c[i] < mn) mn = t->c[i];
        if (t->c[i] > mx) mx = t->c[i];
    }
    return (mx - mn > 32) ? HENC_EDGE : HENC_GRADIENT;
}

static inline uint8_t hex_tile_classify(const HexTile *t) {
    return _hex_classify(t);
}

static inline int hex_tile_encode(const HexTile *t, uint8_t *dst) {
    uint8_t type = _hex_classify(t);
    dst[0] = type;
    if (type == HENC_FLAT) {
        dst[1] = t->c[0];
        return 2;
    }
    uint8_t pred = _hex_predict(t);
    dst[1] = pred;
    for (int i = 0; i < HEX_CELLS; i++)
        dst[2 + i] = (uint8_t)((t->c[i] - pred + 128) & 0xFF);
    return 9;
}

static inline int hex_tile_decode(const uint8_t *src, size_t src_len, HexTile *t) {
    if (src_len < 2) return 0;
    if (src[0] == HENC_FLAT) {
        for (int i = 0; i < HEX_CELLS; i++) t->c[i] = src[1];
        return 2;
    }
    if (src_len < 9) return 0;
    uint8_t pred = src[1];
    for (int i = 0; i < HEX_CELLS; i++)
        t->c[i] = (uint8_t)((src[2 + i] - 128 + pred) & 0xFF);
    return 9;
}

#endif /* HEX_TILE_H */
