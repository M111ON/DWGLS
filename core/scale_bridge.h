/*
 * scale_bridge.h — ONE scale timeline: BFS continuous seeker ⇄ tess gear ring
 * ═══════════════════════════════════════════════════════════════════════════
 * BRIDGE (2026-09-05): 1 gear tooth (ΔW=1) = 1 semitone = ×2^(1/12)
 *
 *   teeth(s) = −12·log2(s)          ← THE shared continuous coordinate
 *   s(teeth) = 2^(−teeth/12)
 *   W        = teeth mod 144        ← the shared ring (tess native axis)
 *
 * NEW: W = position identity on 6 axes (xyz square + ijk triangle)
 *      from geo_box_axes.h — floor = 0, position ≥ 0
 *      Each axis has its own W ring; seeker moves along its axis ring.
 *      Scale function is EXACTLY s = 2^(−W/12) for all axes — bijection preserved.
 *      Axis differences: hyperbolic boundary offset, step distance.
 *
 * Named window: W ∈ [0,144) covers s ∈ [2^(−143/12), 1]  (≈ [2.06e-4, 1]).
 * Deeper/expanded scales wrap the ring — doctrinally the ∞-loop:
 * "∞ ← contraction ← 0 ← expansion → ∞ · enter anywhere".
 *
 * ALIGNMENTS (all verified by tests/test_scale_bridge.c — oracle = math,
 * never this file):
 *   W=0    ↔ s=1.0      home — gear home tooth (Δ=0 emits nothing)
 *   W=12   ↔ s=0.5      EXACT BFS hyperbolic boundary: window=space ⇔ s=½
 *                       (5184/s = 20736·s ⇔ s² = ¼); hyper ⇔ W > 12
 *   ΔW=12  (1 octave)   = ×2        (base-2, timeline-first)
 *   ΔW=24  (1 rim turn) = ×4 = q=1  (fan24 rim, 24 teeth/rim)
 *   144 teeth           = 12 octaves = ×4096 = 4^6 = one full ring
 *   BFS floor s=1e-6    → teeth −12·log2(1e-6) = 239.18 → round 239
 *                        → ring W = 239 mod 144 = 95 (deep wrap)
 *   s=2.0 (expansion)   → teeth −12 → ring W = −12 mod 144 = 132
 *
 * Quantization: s→W is nearest-tooth; max ratio error = 2^(±1/24) ≈ ±2.9%
 * (half semitone). W→s is exact; s→W→s roundtrip is exact on the grid.
 *
 * Zero hash, zero lookup, zero malloc — pure math, int + libm only.
 * LUT ห้าม: scale เป็นฟังก์ชัน (ฟรี) — ไม่ใช่ตาราง address
 */
#ifndef SCALE_BRIDGE_H
#define SCALE_BRIDGE_H

#include <stdint.h>
#include <math.h>
#include "geo_box_axes.h"

#define SBR_RING              144u   /* teeth per ring = tess scale axis   */
#define SBR_TEETH_PER_OCTAVE  12u    /* 1 octave = 12 semitones = ×2       */
#define SBR_HYPER_BOUND_W     12u    /* last NON-hyperbolic tooth (s=½)    */
#define SBR_RIM_TURN          24u    /* teeth per rim turn = ×4 (q=1)      */
#define SBR_WRAP_DEEP_S       1e-6   /* BFS seeker floor → ring W=95       */

/* ═══════════════ AXIS-AWARE W (position identity) ═══════════════ */

/*
 * Each axis has its own scale ring. W = position mod 144.
 * Position comes from GBA_Address (floor=0, monotonic along axis).
 * This replaces the old single-ring model with 6 parallel rings.
 * Scale is IDENTICAL across axes: s = 2^(-W/12) — exact bijection.
 * Axis differences: hyperbolic boundary, step functions.
 */

/* ── W → s : EXACT bijection on the ring (s of named tooth w) ─────────── */
static inline double sbr_w_to_scale(uint32_t w)
{
    double r = (double)(w % SBR_RING);
    return exp2(-r / 12.0);
}

static inline uint32_t sbr_position_to_w(uint64_t position)
{
    return (uint32_t)(position % SBR_RING);
}

static inline uint32_t sbr_gba_to_w(GBA_Address a)
{
    return sbr_position_to_w(a.position);
}

/* Scale is IDENTICAL for all axes — exact bijection W ↔ s */
static inline double sbr_gba_to_scale(GBA_Address a)
{
    return sbr_w_to_scale(sbr_gba_to_w(a));
}

/* ── s → teeth : the unbounded continuous shared coordinate ─────────────
 * s ≤ 0 or NaN → +INFINITY (degenerate; use sbr_scale_to_ring for the
 * safe ring view, which maps degenerate input to the deepest tooth). */
static inline double sbr_scale_to_teeth(double s)
{
    if (!(s > 0.0)) return INFINITY;
    return -12.0 * log2(s);
}

/* ── s → W : nearest-tooth ring position. Total on (0,∞) — deep scales
 * wrap (BFS floor 1e-6 → 95), expansions wrap too (2.0 → 132). */
static inline uint32_t sbr_scale_to_ring(double s)
{
    if (!(s > 0.0)) return SBR_RING - 1u;          /* degenerate → deepest tooth */
    double t = -12.0 * log2(s);
    long long w = (t >= 0.0) ? (long long)(t + 0.5) : (long long)(t - 0.5);
    w %= (long long)SBR_RING;
    if (w < 0) w += (long long)SBR_RING;
    return (uint32_t)w;
}

/* ── Hyperbolic predicate (true BFS definition: window = 5184/s > space) ──
 * window > space ⇔ 5184/s > 20736·s ⇔ s² < ¼ ⇔ s < 0.5 (strict boundary at
 * W=12, so hyper starts at W=13). */
static inline int sbr_is_hyperbolic_scale(double s)
{
    return (s > 0.0 && s < 0.5) ? 1 : 0;
}

/* Ring-tooth view: hyperbolic iff the tooth is deeper than the boundary. */
static inline int sbr_w_is_hyperbolic(uint32_t w)
{
    return (w % SBR_RING) > SBR_HYPER_BOUND_W;
}

/* Axis-aware hyperbolic: triangle axes have boundary at W=18 (12+6) */
static inline int sbr_gba_is_hyperbolic(GBA_Address a)
{
    uint32_t w = sbr_gba_to_w(a);
    uint32_t bound = gba_is_triangle_axis(a.axis) ? (SBR_HYPER_BOUND_W + 6) : SBR_HYPER_BOUND_W;
    return w > bound;
}

/* ── Ring distance (teeth) forward from a to b — the gear Δ the tess side
 * logs: Δ = (b − a) mod 144 ∈ [0,144). Δ=0 is the home tooth (no event). */
static inline uint32_t sbr_step_teeth(uint32_t a, uint32_t b)
{
    return (b + SBR_RING - (a % SBR_RING)) % SBR_RING;
}

static inline uint32_t sbr_gba_step_teeth(GBA_Address a, GBA_Address b)
{
    if (a.axis != b.axis) return SBR_RING;  /* different axis = max distance */
    return sbr_step_teeth(sbr_gba_to_w(a), sbr_gba_to_w(b));
}

/* Magnification ratio s(b)/s(a) — deepening (forward Δ) shrinks s. */
static inline double sbr_step_ratio(uint32_t a, uint32_t b)
{
    return exp2(-(double)sbr_step_teeth(a, b) / 12.0);
}

static inline double sbr_gba_step_ratio(GBA_Address a, GBA_Address b)
{
    if (a.axis != b.axis) return 0.0;
    return sbr_step_ratio(sbr_gba_to_w(a), sbr_gba_to_w(b));
}

/* ── TESS Header helpers: scale_factor is fixed-point uint32_t (s × 65536) ──
 * W=0 → s=1.0 → scale_factor=65536; W=12 → s=0.5 → scale_factor=32768. */
static inline uint32_t sbr_w_to_scale_factor(uint32_t w)
{
    return (uint32_t)(sbr_w_to_scale(w) * 65536.0 + 0.5);
}

static inline uint32_t sbr_scale_factor_to_w(uint32_t sf)
{
    double s = (double)sf / 65536.0;
    return sbr_scale_to_ring(s);
}

/* Axis-aware scale factor for TESS Header — uses standard scale (bijection) */
static inline uint32_t sbr_gba_to_scale_factor(GBA_Address a)
{
    return sbr_w_to_scale_factor(sbr_gba_to_w(a));
}

/* ── SEEKER STATE (continuous) ═══════════════ */

typedef struct {
    GBA_Address gba;          /* current axis + position + local */
    double      s;            /* continuous scale */
    uint32_t    teeth;        /* continuous teeth (unbounded) */
    int         direction;    /* +1 = expansion (forward), -1 = contraction */
} SBR_Seeker;

static inline SBR_Seeker sbr_seeker_make(GBA_Address a, int direction)
{
    SBR_Seeker sk;
    sk.gba = a;
    sk.teeth = (double)a.position * 12.0 / 144.0;  /* approximate */
    sk.s = sbr_gba_to_scale(a);
    sk.direction = direction;
    return sk;
}

static inline SBR_Seeker sbr_seeker_step(SBR_Seeker sk, int64_t delta_teeth)
{
    sk.teeth += (double)delta_teeth;
    if (sk.teeth < 0) sk.teeth = 0;
    sk.s = exp2(-sk.teeth / 12.0);
    sk.gba.position = (uint64_t)(sk.teeth / 12.0 * 144.0);
    return sk;
}

static inline int sbr_seeker_verify(void)
{
    /* Original ring verification */
    for (uint32_t w = 0; w < SBR_RING; w++) {
        double s = sbr_w_to_scale(w);
        uint32_t rt = sbr_scale_to_ring(s);
        if (rt != w) return -1;
    }

    /* Axis-aware verification: W and scale are standard, only hyperbolic differs */
    for (uint32_t axis = 0; axis < GBA_AXIS_COUNT; axis++) {
        for (uint64_t pos = 0; pos < 200; pos++) {
            GBA_Address a = gba_make(axis, pos, 0);
            uint32_t w = sbr_gba_to_w(a);
            if (w != pos % SBR_RING) return -2;

            double s = sbr_gba_to_scale(a);
            if (s <= 0.0) return -3;

            /* Round-trip: position → W → scale → ring W (standard scale = exact) */
            uint32_t rt = sbr_scale_to_ring(s);
            if (rt != w) return -4;
        }
    }

    /* Hyperbolic boundary */
    if (sbr_is_hyperbolic_scale(0.5)) return -5;  /* 0.5 is boundary, NOT hyperbolic */
    if (!sbr_is_hyperbolic_scale(0.49)) return -6;  /* 0.49 IS hyperbolic */

    return 0;
}

#endif /* SCALE_BRIDGE_H */