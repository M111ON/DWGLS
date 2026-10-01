/* geo_tower_ladder.h — intra-tower ladder addressing for ONE geo_jump tower.
 *
 * Lineage: decoded from the POGLS V4.2 artifact (3d_stack_144.html), which
 * predates geo_jump. The artifact's "3 towers x 48" are NOT geo_jump towers —
 * they are the 3 GEO_BLOCKs (48 slots each) inside a SINGLE GEO_TOWER (144).
 * Each block carries its own 16-slot residual: 3 blocks x (48 active + 16
 * residual) = 3 x 64 = 192 slots total = 1 tower-full (144 active + 48 res).
 * There is no 144-tess hierarchy above this: the whole 192 lives flat inside
 * one jump unit, so a balloon/haunt table over it needs no cross-tower route.
 *
 * The three active layers are ONE curve under three symmetries, not three
 * curves: HL_L1 = rot90ccw(HL_L0), HL_L2 = mirX(HL_L1). Each layer therefore
 * visits the same 16 cells of the 4x4; the layer index is what separates them.
 * Composition rule holds: undo a layer view by its own inverse symmetry.
 *
 * Peano (HL_PEANO) is the alternative engine. It is a boustrophedon, not a
 * space-filling curve: steps 11->12 is a 3-cell jump (0,0)->(3,3). It is
 * supplied as-is because the artifact's engine switch selects between them, and
 * the discontinuity is a property of the curve, not a defect here.
 *
 * Residual zone (16 per tower, 48 total): addressed as (rung, corner),
 * rung 0..3 = the four horizontal planes around the 3 layers
 * (below L0, L0|L1, L1|L2, above L2), corner 0..3 = the 4 corners of the 4x4.
 * 4 x 4 = 16, so the residual zone is a bijection. The artifact derived these
 * from float (-0.8 / 3.8) and produced 13 distinct positions out of 48 slots;
 * this replaces that with integer (rung, corner) so every slot is reachable
 * and unique.
 *
 * Pointer arithmetic is modulo TL_TOTAL, never a bitmask. 192 is not a power
 * of two: ANDing with 191 (0xBF) clears bit 6, leaving slots 64..127
 * unreachable and corrupting 128 of 192 shift-lefts. Modulo is exact.
 *
 * Replay log is LIFO (per the breath-log contract: unwind in reverse order).
 * Jump is the deterministic MOD-37 stride, not a random number: gcd(37,192)=1,
 * so the map is a bijection, and the same pointer replays identically.
 *
 * Header-only, integer-only, stdint only. No build step.
 */
#ifndef GEO_TOWER_LADDER_H
#define GEO_TOWER_LADDER_H

#include <stdint.h>

#define TL_TOWERS            3u   /* towers in the vertical stack          */
#define TL_SLOTS_PER_TOWER   64u  /* 48 active + 16 residual               */
#define TL_TOTAL            192u  /* 3 * 64; not a power of two            */
#define TL_TOTAL_MASK_LOST  191u  /* 192-1: a bound, NOT an AND-mask       */

#define TL_ACTIVE_PER_TOWER  48u  /* 3 layers * 16 steps                   */
#define TL_RESIDUAL_PER_TOWER 16u /* 4 rungs * 4 corners                   */
#define TL_LAYERS             3u
#define TL_STEPS             16u  /* cells per layer (4x4)                 */
#define TL_SIDE               4u
#define TL_RUNGS              4u   /* horizontal planes around 3 layers     */
#define TL_CORNERS            4u
#define TL_REWIND_CAP       972u   /* replay log depth                      */

_Static_assert(TL_ACTIVE_PER_TOWER + TL_RESIDUAL_PER_TOWER == TL_SLOTS_PER_TOWER,
               "48+16 must be 64 per tower");
_Static_assert(TL_TOWERS * TL_SLOTS_PER_TOWER == TL_TOTAL, "3*64 must be 192");
_Static_assert(TL_LAYERS * TL_STEPS == TL_ACTIVE_PER_TOWER, "3*16 must be 48");
_Static_assert(TL_RUNGS * TL_CORNERS == TL_RESIDUAL_PER_TOWER, "4*4 must be 16");
_Static_assert(TL_TOTAL == 192u && (TL_TOTAL & (TL_TOTAL - 1u)) != 0u,
               "192 is deliberately not a power of two");

/* Lane ids. Lane 0-2 are the three Hilbert views, lane 3 is Peano. */
#define TL_LANE_HILBERT  0u
#define TL_LANE_HILBERT1 1u
#define TL_LANE_HILBERT2 2u
#define TL_LANE_PEANO    3u
#define TL_LANES         4u

/* Cell packing: x in bits 0-1, y in bits 2-3. */
#define TL_CELL(x, y)   ((uint8_t)(((x) & 0x3u) | (((y) & 0x3u) << 2)))
#define TL_CELL_X(c)    ((uint32_t)((c) & 0x3u))
#define TL_CELL_Y(c)    ((uint32_t)((c) >> 2))

/* ── Active ladder tables (one curve, three views) ─────────────── */
static const uint8_t TL_HILBERT_L0[16] = {
    0x00, 0x04, 0x05, 0x01, 0x02, 0x03, 0x07, 0x06,
    0x0A, 0x0B, 0x0F, 0x0E, 0x0D, 0x09, 0x08, 0x0C
};
static const uint8_t TL_HILBERT_L1[16] = {
    0x0C, 0x0D, 0x09, 0x08, 0x04, 0x00, 0x01, 0x05,
    0x06, 0x02, 0x03, 0x07, 0x0B, 0x0A, 0x0E, 0x0F
};
static const uint8_t TL_HILBERT_L2[16] = {
    0x0F, 0x0E, 0x0A, 0x0B, 0x07, 0x03, 0x02, 0x06,
    0x05, 0x01, 0x00, 0x04, 0x08, 0x09, 0x0D, 0x0C
};
static const uint8_t TL_PEANO[16] = {
    0x00, 0x04, 0x08, 0x09, 0x05, 0x01, 0x02, 0x06,
    0x0A, 0x0B, 0x07, 0x03, 0x0F, 0x0E, 0x0D, 0x0C
};

static const uint8_t *const TL_LANE[TL_LANES] = {
    TL_HILBERT_L0, TL_HILBERT_L1, TL_HILBERT_L2, TL_PEANO
};

/* Corner cells, in the same packing as a 4x4 cell. */
static const uint8_t TL_CORNER_CELL[4] = { 0x00, 0x03, 0x0F, 0x0C };

/* ── Slot decode (total slot 0..191) ───────────────────────────── */
static inline uint32_t tl_tower(uint32_t slot) {
    return (slot % TL_TOTAL) / TL_SLOTS_PER_TOWER;
}
static inline uint32_t tl_local(uint32_t slot) {
    return (slot % TL_TOTAL) % TL_SLOTS_PER_TOWER;
}
static inline uint32_t tl_is_residual(uint32_t slot) {
    return tl_local(slot) >= TL_ACTIVE_PER_TOWER ? 1u : 0u;
}
static inline uint32_t tl_active_slot(uint32_t tower, uint32_t layer, uint32_t step) {
    return (tower % TL_TOWERS) * TL_SLOTS_PER_TOWER
         + (layer % TL_LAYERS) * TL_STEPS
         + (step % TL_STEPS);
}
static inline uint32_t tl_residual_slot(uint32_t tower, uint32_t rung, uint32_t corner) {
    return (tower % TL_TOWERS) * TL_SLOTS_PER_TOWER
         + TL_ACTIVE_PER_TOWER
         + (rung % TL_RUNGS) * TL_CORNERS
         + (corner % TL_CORNERS);
}
/* layer and step within the 48 active slots of a tower */
static inline uint32_t tl_layer(uint32_t slot) { return (tl_local(slot) % TL_ACTIVE_PER_TOWER) / TL_STEPS; }
static inline uint32_t tl_step(uint32_t slot)  { return (tl_local(slot) % TL_ACTIVE_PER_TOWER) % TL_STEPS; }

/* residual (rung, corner) within the 16 residual slots of a tower */
static inline uint32_t tl_rung(uint32_t slot) {
    return ((tl_local(slot) - TL_ACTIVE_PER_TOWER) % TL_RESIDUAL_PER_TOWER) / TL_CORNERS;
}
static inline uint32_t tl_corner(uint32_t slot) {
    return ((tl_local(slot) - TL_ACTIVE_PER_TOWER) % TL_RESIDUAL_PER_TOWER) % TL_CORNERS;
}

/* ── Coordinate read (slot -> address, no float, no projection) ─── */
/* Cell of an active slot in the given lane. Layer 0..2 always reads the
 * Hilbert views; the Peano engine replaces the whole ladder, so lane 3 is
 * only meaningful for layer 0. */
static inline uint8_t tl_cell(uint32_t slot, uint32_t lane) {
    return TL_LANE[lane % TL_LANES][tl_step(slot)];
}
/* Cell of a residual slot: corner cell lifted onto its rung. */
static inline uint8_t tl_residual_cell(uint32_t slot) {
    return TL_CORNER_CELL[tl_corner(slot)];
}

/* Symmetry generators, exposed so a view can be inverted (composition rule). */
static inline uint8_t tl_rot90ccw(uint8_t c) { return TL_CELL(TL_CELL_Y(c), 3u - TL_CELL_X(c)); }
static inline uint8_t tl_mirx(uint8_t c)     { return TL_CELL(3u - TL_CELL_X(c), TL_CELL_Y(c)); }

/* ── Pointer walk (modulo, never a bitmask) ────────────────────── */
static inline uint32_t tl_slot(uint32_t p) { return p % TL_TOTAL; }
static inline uint32_t tl_shift(uint32_t p, int32_t delta) {
    return (uint32_t)(((int32_t)tl_slot(p) + delta) % (int32_t)TL_TOTAL
                      + (int32_t)TL_TOTAL) % (int32_t)TL_TOTAL;
}
/* MOD-37 stride: gcd(37,192)=1 so this is a bijection on 192 slots. */
static inline uint32_t tl_jump(uint32_t p) {
    return (tl_slot(p) * 37u) % TL_TOTAL;
}
/* next active slot of the same tower, else the first active slot of the next.
 * Walks on the local index (0..47), NOT on (layer, step): step wraps at 16, so
 * incrementing step alone would fall back to layer 0 and never leave it. */
static inline uint32_t tl_next_active(uint32_t slot) {
    uint32_t t = tl_tower(slot);
    uint32_t l = (tl_local(slot) % TL_ACTIVE_PER_TOWER) + 1u;
    if (l >= TL_ACTIVE_PER_TOWER) return tl_active_slot(t + 1u, 0u, 0u);
    return tl_active_slot(t, l / TL_STEPS, l % TL_STEPS);
}

/* ── Replay log (LIFO, bounded at 972) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t head;   /* frames written                          */
    uint16_t count;  /* frames held, <= TL_REWIND_CAP          */
    uint16_t slot[TL_REWIND_CAP];   /* slot AFTER the move         */
    uint16_t prev[TL_REWIND_CAP];   /* slot BEFORE the move        */
} TlLog;

static inline void tl_log_reset(TlLog *g) { g->head = 0; g->count = 0; }

static inline void tl_log_push(TlLog *g, uint32_t from, uint32_t to) {
    uint16_t i = (uint16_t)(g->head % TL_REWIND_CAP);
    g->slot[i] = (uint16_t)tl_slot(to);
    g->prev[i] = (uint16_t)tl_slot(from);
    g->head = (uint16_t)(g->head + 1u);
    g->count = (uint16_t)(g->count < TL_REWIND_CAP ? g->count + 1u : TL_REWIND_CAP);
}

/* Undo the most recent move. Returns 0 when the log is empty. */
static inline uint32_t tl_log_rewind(TlLog *g) {
    if (g->count == 0u) return 0u;
    g->head = (uint16_t)(g->head ? g->head - 1u : 0u);
    g->count = (uint16_t)(g->count - 1u);
    return g->prev[g->head % TL_REWIND_CAP];
}

#endif /* GEO_TOWER_LADDER_H */
