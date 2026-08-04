/*
 * geo_goldberg_lut.h — Goldberg Dodecahedron Bipolar Pair LUT (stub)
 * ════════════════════════════════════════════════════════════════════
 * 12 pentagon faces → 6 bipolar pairs × 2 poles
 * Pairs derived from TRING_CPAIR: face f ↔ face f+6 (mod 12)
 *   pair_id = face_id % 6   (0..5)
 *   pole    = face_id / 6   (0=positive ring1, 1=negative ring2)
 */
#ifndef GEO_GOLDBERG_LUT_H
#define GEO_GOLDBERG_LUT_H
#include <stdint.h>

/* GB_PEN_TO_PAIR[face_id] → bipolar pair index 0..5 */
static const uint8_t GB_PEN_TO_PAIR[12] = {
    0, 1, 2, 3, 4, 5,   /* ring1: faces 0..5  */
    0, 1, 2, 3, 4, 5,   /* ring2: faces 6..11 */
};

/* GB_PEN_POLE[face_id] → 0=positive (ring1), 1=negative (ring2) */
static const uint8_t GB_PEN_POLE[12] = {
    0, 0, 0, 0, 0, 0,   /* ring1 */
    1, 1, 1, 1, 1, 1,   /* ring2 */
};

#endif /* GEO_GOLDBERG_LUT_H */
