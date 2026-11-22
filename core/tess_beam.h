/* tess_beam.h — Beam Addressing integrated into tess_resolver.
 *
 * "Coordinate IS the data. No hash. No storage. No collision."
 *
 * Beam addressing maps weight values to coordinates (beam length = weight value).
 * Base-12 structure: 12⁴ = 20736 slots (same as TESS_TOTAL_SLOTS).
 *
 * Two timers:
 *   frame_seek: 1440 cycle (geom) — face/slot/phase/ico
 *   beam_timer: 20736 slots (spine) — pipe/tick/slot mapping
 *
 * Three views (same data):
 *   View 1: frame_seek (geom)  — face/slot/phase/ico
 *   View 2: fibo_spine (spine) — pipe/tick/bridge/residual
 *   View 3: spherical (coord)  — azimuth/elevation/length
 *
 * BeamCode = 8-bit storage: zone (upper nibble) + position (lower nibble) = 256 = Q8
 * BeamCoord = runtime transient (capo_id, param_index, abs_value, sign) — never stored
 *
 * Two-sided: positive (ceiling, +) + negative (ground, -). Carries both.
 * Lazy evaluation: only compute when pointer reads.
 *
 * Integration: beam addressing (weight→coord) → tess_resolver (coord→slot) → jump module (navigation)
 */
#ifndef TESS_BEAM_H
#define TESS_BEAM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "tess_resolver.h"
#include "tess_jump.h"

/* ── BeamCode: 8-bit storage (Q8 = 256 values) ─────────────────── */
typedef uint8_t BeamCode;

/* BeamCode encode: weight (int8) → code (uint8) */
static inline BeamCode beam_code_from_weight(int8_t w) {
    return (BeamCode)(w + 128);  /* -128..127 → 0..255 */
}

/* BeamCode decode: code (uint8) → weight (int8) */
static inline int8_t beam_weight_from_code(BeamCode code) {
    return (int8_t)(code - 128);  /* 0..255 → -128..127 */
}

/* BeamCode zone/position split */
static inline uint8_t beam_code_zone(BeamCode code) { return code >> 4; }      /* 0..15 */
static inline uint8_t beam_code_pos(BeamCode code)  { return code & 0x0F; }    /* 0..15 */

/* ── BeamCoord: runtime transient (never stored) ───────────────── */
typedef struct {
    uint32_t capo_id;      /* parameter partition (0..255) */
    uint32_t param_index;  /* position in weight array */
    uint32_t abs_value;    /* weight magnitude (cached) */
    uint8_t  sign;         /* polarity: 1=ceiling(+), 0=ground(-) */
} BeamCoord;

/* ── Weight → BeamCoord (runtime, O(1)) ────────────────────────── */
static inline BeamCoord beam_weight_to_coord(uint32_t capo_id, uint32_t param_index, int8_t weight) {
    BeamCoord c;
    c.capo_id = capo_id;
    c.param_index = param_index;
    c.abs_value = (weight < 0) ? (uint32_t)(-weight) : (uint32_t)weight;
    c.sign = (weight >= 0) ? 1 : 0;  /* 1=ceiling(+), 0=ground(-) */
    return c;
}

/* ── BeamCoord → weight (verify) ───────────────────────────────── */
static inline int8_t beam_coord_to_weight(const BeamCoord *c) {
    int8_t w = (int8_t)c->abs_value;
    return c->sign ? w : (int8_t)(-w);
}

/* ── BeamCoord → slot index (12⁴ = 20736) ────────────────────── */
/* slot = capo_id * 1000000 + param_index (mod 20736) — simplified */
/* In production: use beam_timer for precise slot mapping */
static inline uint32_t beam_to_slot(const BeamCoord *c) {
    /* Use param_index mod 20736 as slot (deterministic) */
    return c->param_index % TESS_TOTAL_SLOTS;
}

/* ── BeamCoord → JumpResult (bridge to jump module) ────────────── */
static inline JumpResult beam_to_jump(JumpContext *ctx, const BeamCoord *c) {
    /* Set pointer to beam's slot */
    ctx->pointer_pos = beam_to_slot(c);
    /* Set side based on sign */
    ctx->side = c->sign ? 1 : -1;
    /* Execute jump (Hilbert outer → ... → snap) */
    return jump_execute(ctx, JUMP_HILBERT, c->abs_value);
}

/* ── Beam addressing verify ────────────────────────────────────── */
static inline int beam_addressing_verify(void) {
    /* T1: BeamCode roundtrip (Q8 full range) */
    for (int32_t w = -128; w <= 127; w++) {
        BeamCode code = beam_code_from_weight((int8_t)w);
        int32_t r = beam_weight_from_code(code);
        if (r != w) return -1;
    }
    /* T2: BeamCoord roundtrip */
    for (int w = -127; w <= 127; w++) {
        BeamCoord c = beam_weight_to_coord(0, 42, (int8_t)w);
        int8_t r = beam_coord_to_weight(&c);
        if (r != (int8_t)w) return -2;
    }
    /* T3: BeamCode zone/position split */
    {
        BeamCode code = beam_code_from_weight(100);
        uint8_t zone = beam_code_zone(code);
        uint8_t pos = beam_code_pos(code);
        if (zone != 14 || pos != 4) return -3;  /* 100+128=228 → zone=14, pos=4 */
    }
    /* T4: beam_to_slot in range */
    {
        BeamCoord c = beam_weight_to_coord(0, 100, 50);
        uint32_t slot = beam_to_slot(&c);
        if (slot >= TESS_TOTAL_SLOTS) return -4;
    }
    /* T5: two-sided (positive + negative) */
    {
        BeamCoord pos = beam_weight_to_coord(0, 10, 50);
        BeamCoord neg = beam_weight_to_coord(0, 10, -50);
        if (pos.sign != 1) return -5;
        if (neg.sign != 0) return -6;
    }
    return 0;
}

#endif /* TESS_BEAM_H */
