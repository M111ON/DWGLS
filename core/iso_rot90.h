/*
 * iso_rot90.h — Triangle(iso ijk) ↔ Square(xyz) bridge on 144 slots
 * ══════════════════════════════════════════════════════════════════════
 * Core identity (user's equation):
 *
 *   (4×4) × (3×3) = 144 = 12×12
 *
 * One cell = 144 slots, two views:
 *   square view : x,y ∈ [0,11]           (xyz grid)
 *   iso view    : tri part A,B ∈ [0,3]   (4×4 triangle macro)
 *                 sq  part C,D ∈ [0,2]   (3×3 square micro)
 *   x = A*3 + C · y = B*3 + D
 *
 * rot90 ("หมุน isometric 90° — ปลายแหลมต่อกันเป็น square"):
 *   swap the digit pair across swapped bases:
 *   x' = C*4 + A · y' = D*4 + B
 *
 * Properties (all pure int, no table, no hash):
 *   - rot90: bijection on [0,11]²  (per-axis: base-3/base-4 digit transpose)
 *   - rot270 = rot90⁻¹ (mutual inverses — like physical 90°/270° turns;
 *     NOT self-inverse: bases differ, so rot90∘rot90 ≠ identity)
 *   - corners {0,11} fixed points on both maps
 *
 * Composes with existing halves of Core Law 128×162 = 20736:
 *   fold   : slot → FRAME_ICO anchor      (geo_dram_tile.h: 81×2 poles)
 *   unfold : anchor → hilbert_8x8 × layer (=128)
 */
#ifndef ISO_ROT90_H
#define ISO_ROT90_H

#include <stdint.h>

#define ISO_SIDE      12                    /* 12×12 = 144 slots per cell */
#define ISO_TRI_PART  4                     /* (4×4) triangle macro       */
#define ISO_SQ_PART   3                     /* (3×3) square micro         */
#define ISO_SLOTS     (ISO_SIDE * ISO_SIDE) /* 144 */

typedef struct { int32_t x, y; } iso_pt;

static inline void iso_digits(int32_t v, int32_t *tri, int32_t *sq) {
    *tri = v / ISO_SQ_PART;      /* 0..3 */
    *sq  = v % ISO_SQ_PART;      /* 0..2 */
}

static inline int32_t iso_rot90_axis(int32_t v) {
    int32_t tri, sq;
    iso_digits(v, &tri, &sq);
    return sq * ISO_TRI_PART + tri;          /* swap digits across bases */
}

static inline iso_pt iso_rot90(iso_pt p) {
    iso_pt r;
    r.x = iso_rot90_axis(p.x);
    r.y = iso_rot90_axis(p.y);
    return r;
}

/* rot270 = rot90⁻¹ : undo the view flip
 *   given w = sq*4 + tri  ->  recover v = tri*3 + sq */
static inline int32_t iso_rot270_axis(int32_t w) {
    int32_t sq = w / ISO_TRI_PART;           /* was the base-4 low digit  */
    int32_t tri = w % ISO_TRI_PART;          /* was the base-3 low digit  */
    return tri * ISO_SQ_PART + sq;
}

static inline iso_pt iso_rot270(iso_pt p) {
    iso_pt r;
    r.x = iso_rot270_axis(p.x);
    r.y = iso_rot270_axis(p.y);
    return r;
}

static inline int32_t iso_slot(int32_t x, int32_t y) {
    return y * ISO_SIDE + x;
}

static inline iso_pt iso_unslot(int32_t s) {
    iso_pt p;
    p.x = s % ISO_SIDE;
    p.y = s / ISO_SIDE;
    return p;
}

/* full round: slot → rot90 → slot' */
static inline int32_t iso_rot90_slot(int32_t s) {
    iso_pt p = iso_unslot(s);
    return iso_slot(iso_rot90(p).x, iso_rot90(p).y);
}

/* full round: slot → rot270 → slot' */
static inline int32_t iso_rot270_slot(int32_t s) {
    iso_pt p = iso_unslot(s);
    return iso_slot(iso_rot270(p).x, iso_rot270(p).y);
}

#endif /* ISO_ROT90_H */
