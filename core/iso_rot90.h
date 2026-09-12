/*
 * iso_rot90.h — Hexagonal generator (order-5 → order-6) on 144 slots
 * ══════════════════════════════════════════════════════════════════════
 *
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
 * Operation (swap digit pair across swapped bases):
 *   x' = C*4 + A · y' = D*4 + B
 *
 * ── THE CHAIN ──────────────────────────────────────────────────────
 *
 *   order-5 (this operation)
 *       ↓  5+1 = 6
 *   order-6 (hexagonal symmetry)
 *       ↓
 *   hexagonal lattice = universal container:
 *       order-2 (rhombus edge / I)
 *       order-3 (triangle vertex / Y)
 *       order-4 (square inscribed)
 *       order-5 (icosa via vertex tile)
 *       order-6 (hexagon itself)
 *
 *   Sync points:
 *     LCM(4, 6) = 12      → 12 slots per axis (natural square base)
 *     LCM(5, 6) = 30      → icosa/dodeca edges
 *     LCM(4, 5, 6) = 60   → |A5| (icosahedral rotation group)
 *
 *   Honeycomb growth: 1 → 7 → 19 → 37 → 61 = 6k+1
 *   7th roots of unity = irreducible generator of order-6
 *     (prime order → no subgroup ambiguity)
 *
 * ── 36-VIEW DERIVATION ────────────────────────────────────────────
 *
 *   5 rotations + 1 base = 6 orientations per face
 *   6 faces × 6 orientations = 36 views
 *   36 × 4 (D4) = 144 = 12²
 *   144² = 20736 = latent space
 *
 *   If order-4: 4+1=5 → 5²=25 → 25×4=100 ≠ 144. Chain breaks.
 *   Order-5 is necessary for 36-view design.
 *
 * ── PROPERTIES ─────────────────────────────────────────────────────
 *
 *   - Bijection on [0,11]²  (per-axis: base-3/base-4 digit transpose)
 *   - Inverse: undo the view flip (given w = sq*4+tri, recover tri*3+sq)
 *   - NOT self-inverse: bases differ, so T∘T ≠ identity
 *   - Order = 5 (LCM of bases 3,4) → generates hexagonal (order-6)
 *   - Corners {0,11} fixed points on both maps
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

/*
 * iso_hex5 — order-5 generator: (v = tri×3 + sq) → (sq×4 + tri)
 *
 * Order 5 (not 4!). Derived from icosahedron 5-fold symmetry.
 * Generates 6 orientations per face (5 rotations + 1 base).
 * 6 faces × 6 orientations = 36 views.
 * 36 × 4 (D4) = 144 = dodeca faces² = natural square.
 *
 * Honeycomb = geometric realization of order-6.
 * Rhombus unit cell = order-2 × order-3 component.
 * Triangle = order-3 vertex component.
 */
static inline int32_t iso_hex5(int32_t v) {
    int32_t tri, sq;
    iso_digits(v, &tri, &sq);
    return sq * ISO_TRI_PART + tri;          /* swap digits across bases */
}

static inline iso_pt iso_hex5_pt(iso_pt p) {
    iso_pt r;
    r.x = iso_hex5(p.x);
    r.y = iso_hex5(p.y);
    return r;
}

/*
 * iso_hex5_inv — inverse of iso_hex5
 *   given w = sq*4 + tri  ->  recover v = tri*3 + sq
 */
static inline int32_t iso_hex5_inv(int32_t w) {
    int32_t sq = w / ISO_TRI_PART;           /* was the base-4 low digit  */
    int32_t tri = w % ISO_TRI_PART;          /* was the base-3 low digit  */
    return tri * ISO_SQ_PART + sq;
}

static inline iso_pt iso_hex5_inv_pt(iso_pt p) {
    iso_pt r;
    r.x = iso_hex5_inv(p.x);
    r.y = iso_hex5_inv(p.y);
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

/* full round: slot → hex5 → slot' */
static inline int32_t iso_hex5_slot(int32_t s) {
    iso_pt p = iso_unslot(s);
    return iso_slot(iso_hex5_pt(p).x, iso_hex5_pt(p).y);
}

/* full round: slot → hex5_inv → slot' */
static inline int32_t iso_hex5_inv_slot(int32_t s) {
    iso_pt p = iso_unslot(s);
    return iso_slot(iso_hex5_inv_pt(p).x, iso_hex5_inv_pt(p).y);
}

/* ═══════════════════════════════════════════════════════════════════════
   Backward-compat aliases (old names)
   ═══════════════════════════════════════════════════════════════════════ */
#define iso_rot90_axis      iso_hex5
#define iso_rot90           iso_hex5_pt
#define iso_rot90_slot      iso_hex5_slot
#define iso_rot270_axis     iso_hex5_inv
#define iso_rot270          iso_hex5_inv_pt
#define iso_rot270_slot     iso_hex5_inv_slot
#define iso_base_transpose      iso_hex5
#define iso_base_transpose_pt   iso_hex5_pt
#define iso_base_transpose_slot iso_hex5_slot
#define iso_base_transpose_inv      iso_hex5_inv
#define iso_base_transpose_inv_pt   iso_hex5_inv_pt
#define iso_base_transpose_inv_slot iso_hex5_inv_slot

#endif /* ISO_ROT90_H */
