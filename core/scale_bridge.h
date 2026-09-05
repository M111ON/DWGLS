/*
 * scale_bridge.h — ONE scale timeline: BFS continuous seeker ⇄ tess gear ring
 * ════════════════════════════════════════════════════════════════════════
 * BRIDGE (2026-09-05): 1 gear tooth (ΔW=1) = 1 semitone = ×2^(1/12)
 *
 *   teeth(s) = −12·log2(s)          ← THE shared continuous coordinate
 *   s(teeth) = 2^(−teeth/12)
 *   W        = teeth mod 144        ← the shared ring (tess native axis)
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

#define SBR_RING              144u   /* teeth per ring = tess scale axis   */
#define SBR_TEETH_PER_OCTAVE  12u    /* 1 octave = 12 semitones = ×2       */
#define SBR_HYPER_BOUND_W     12u    /* last NON-hyperbolic tooth (s=½)    */
#define SBR_RIM_TURN          24u    /* teeth per rim turn = ×4 (q=1)      */
#define SBR_WRAP_DEEP_S       1e-6   /* BFS seeker floor → ring W=95       */

/* ── W → s : EXACT bijection on the ring (s of named tooth w) ─────────── */
static inline double sbr_w_to_scale(uint32_t w)
{
    double r = (double)(w % SBR_RING);
    return exp2(-r / 12.0);
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

/* ── Ring distance (teeth) forward from a to b — the gear Δ the tess side
 * logs: Δ = (b − a) mod 144 ∈ [0,144). Δ=0 is the home tooth (no event). */
static inline uint32_t sbr_step_teeth(uint32_t a, uint32_t b)
{
    return (b + SBR_RING - (a % SBR_RING)) % SBR_RING;
}

/* Magnification ratio s(b)/s(a) — deepening (forward Δ) shrinks s. */
static inline double sbr_step_ratio(uint32_t a, uint32_t b)
{
    return exp2(-(double)sbr_step_teeth(a, b) / 12.0);
}

#endif /* SCALE_BRIDGE_H */
