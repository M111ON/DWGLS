/*
 * hyperbolic_seek.h — KIS ↔ Hyperbolic Teleport Seek (3-Axis)
 *
 * Cayley transform maps KIS (unit disk) ↔ Hyperbolic (upper half-plane)
 * 3 axes: X (Hilbert), Y (Peano), Z (Metatron)
 * Each axis has its own infinity point and Cayley transform
 *
 * BUILD: gcc -O2 -I. -o test_hyperbolic test_hyperbolic.c -lm
 */

#ifndef HYPERBOLIC_SEEK_H
#define HYPERBOLIC_SEEK_H

#include <stdint.h>
#include <math.h>

#define HYP_KIS_SLOTS    20736u
#define HYP_AXIS_SLOTS   6912u   /* 20736 / 3 */
#define HYP_INFINITY_IDX  3456u   /* HYP_AXIS_SLOTS / 2 */
#define HYP_PI           3.14159265358979323846

/* 3 axes: X=Hilbert, Y=Peano, Z=Metatron */
#define HYP_AXIS_X  0u
#define HYP_AXIS_Y  1u
#define HYP_AXIS_Z  2u

/* axis = slot / HYP_AXIS_SLOTS — each axis OWNS a contiguous 6912-slot band
 * (axis 0: [0,6912), axis 1: [6912,13824), axis 2: [13824,20736)).
 * Roundtrip kis_to_hyperbolic_axis()/hyperbolic_to_kis_axis() is bijective
 * ONLY when the same axis is passed both ways; passing slot % 3 is a
 * semantic bug (that is a phase, not the owning axis). */
static inline uint8_t hyperbolic_axis_of(uint32_t slot) {
    return (uint8_t)(slot / HYP_AXIS_SLOTS);
}

/* ═══════════════════════════════════════════════════════
   Complex number
   ═══════════════════════════════════════════════════════ */
typedef struct {
    double re, im;
} HypComplex;

static inline HypComplex hyp_mul(HypComplex a, HypComplex b) {
    return (HypComplex){
        a.re * b.re - a.im * b.im,
        a.re * b.im + a.im * b.re
    };
}

static inline HypComplex hyp_div(HypComplex a, HypComplex b) {
    double denom = b.re * b.re + b.im * b.im;
    if (denom < 1e-15) return (HypComplex){1e15, 0};
    return (HypComplex){
        (a.re * b.re + a.im * b.im) / denom,
        (a.im * b.re - a.re * b.im) / denom
    };
}

static inline double hyp_abs2(HypComplex a) {
    return a.re * a.re + a.im * a.im;
}

/* ═══════════════════════════════════════════════════════
   KIS → Hyperbolic (Cayley Transform) — per axis
   ═══════════════════════════════════════════════════════ */
static inline HypComplex kis_to_hyperbolic_axis(uint32_t slot, uint8_t axis) {
    /* Map slot to angle within axis */
    uint32_t axis_slot = slot % HYP_AXIS_SLOTS;
    double angle = 2.0 * HYP_PI * (double)axis_slot / (double)HYP_AXIS_SLOTS;
    
    /* Phase rotation: each axis offset by 120° */
    angle += (double)axis * 2.0 * HYP_PI / 3.0;
    
    HypComplex z = {cos(angle), sin(angle)};
    HypComplex one = {1.0, 0.0};
    HypComplex i_unit = {0.0, 1.0};
    
    /* w = i(1-z)/(1+z) */
    return hyp_mul(i_unit, hyp_div(
        (HypComplex){one.re - z.re, one.im - z.im},
        (HypComplex){one.re + z.re, one.im + z.im}
    ));
}

/* ═══════════════════════════════════════════════════════
   Hyperbolic → KIS (Inverse Cayley) — per axis
   ═══════════════════════════════════════════════════════ */
static inline uint32_t hyperbolic_to_kis_axis(HypComplex w, uint8_t axis) {
    HypComplex i_unit = {0.0, 1.0};
    HypComplex numer = (HypComplex){i_unit.re - w.re, i_unit.im - w.im};
    HypComplex denom = (HypComplex){i_unit.re + w.re, i_unit.im + w.im};
    HypComplex z = hyp_div(numer, denom);
    
    double angle = atan2(z.im, z.re);
    if (angle < 0) angle += 2.0 * HYP_PI;
    
    /* Remove axis offset */
    angle -= (double)axis * 2.0 * HYP_PI / 3.0;
    if (angle < 0) angle += 2.0 * HYP_PI;
    
    uint32_t slot = (uint32_t)(angle * (double)HYP_AXIS_SLOTS / (2.0 * HYP_PI) + 0.5);
    return (slot % HYP_AXIS_SLOTS) + axis * HYP_AXIS_SLOTS;
}

/* ═══════════════════════════════════════════════════════
   Teleport Seek (3-axis)
   ═══════════════════════════════════════════════════════ */
static inline uint32_t teleport_seek(uint32_t src, uint32_t dst) {
    uint8_t axis = hyperbolic_axis_of(dst);
    if (axis > 2) axis = 2;
    HypComplex w = kis_to_hyperbolic_axis(dst, axis);
    return hyperbolic_to_kis_axis(w, axis);
}

/* ═══════════════════════════════════════════════════════
   Three-Phase Balance
   ═══════════════════════════════════════════════════════ */
typedef struct {
    uint32_t kis_active[3];   /* per-axis KIS active */
    uint32_t hyper_active[3]; /* per-axis Hyper active */
    uint32_t total[3];        /* per-axis total */
} DualBalance3;

static inline DualBalance3 dual_balance_3axis(uint32_t step) {
    DualBalance3 b;
    uint8_t phase = step % 3;
    
    /* 60/25/15 split per phase */
    static const uint32_t LOAD[3][3] = {
        {60, 25, 15},  /* phase 0: X dominant */
        {25, 60, 15},  /* phase 1: Y dominant */
        {15, 25, 60},  /* phase 2: Z dominant */
    };
    
    for (uint8_t a = 0; a < 3; a++) {
        b.kis_active[a] = HYP_AXIS_SLOTS * LOAD[phase][a] / 100;
        b.hyper_active[a] = HYP_AXIS_SLOTS - b.kis_active[a];
        b.total[a] = b.kis_active[a] + b.hyper_active[a];
    }
    
    return b;
}

/* ═══════════════════════════════════════════════════════
   Self-test
   ═══════════════════════════════════════════════════════ */
static inline int hyperbolic_selftest(void) {
    int pass = 0, fail = 0;
    
    /* Test 1: EXHAUSTIVE roundtrip — all 20736 slots, axis = owner band.
     * Proves bijectivity: kis_to_hyperbolic_axis then back == original. */
    for (uint32_t slot = 0; slot < HYP_KIS_SLOTS; slot++) {
        uint8_t a = hyperbolic_axis_of(slot);
        HypComplex w = kis_to_hyperbolic_axis(slot, a);
        uint32_t back = hyperbolic_to_kis_axis(w, a);
        if (back == slot) { pass++; } else { fail++; }
    }
    
    /* Test 2: per-axis bijectivity — each band roundtrips within itself */
    for (uint8_t a = 0; a < 3; a++) {
        for (uint32_t s = 0; s < HYP_AXIS_SLOTS; s++) {
            uint32_t slot = a * HYP_AXIS_SLOTS + s;
            HypComplex w = kis_to_hyperbolic_axis(slot, a);
            uint32_t back = hyperbolic_to_kis_axis(w, a);
            if (back == slot) { pass++; } else { fail++; }
        }
    }
    
    /* Test 3: Three-phase balance */
    for (uint32_t s = 0; s < 100; s++) {
        DualBalance3 b = dual_balance_3axis(s);
        uint32_t sum = 0;
        for (uint8_t a = 0; a < 3; a++) sum += b.total[a];
        if (sum == HYP_KIS_SLOTS) { pass++; } else { fail++; }
    }
    
    return fail == 0 ? 0 : -1;
}

#endif /* HYPERBOLIC_SEEK_H */
