/*
 * geo_ghost_envelope.h — Block Envelope (§11.6 — DECIDED)
 * ═══════════════════════════════════════════════════════════════════════════
 * §11.6 asked: เสาเข็มต้องประกาศ envelope (w₀, depth k, ขนาดที่ w₀+k) —
 * capacity = Σ envelope ≤ 20736, เกิน = reject deterministic. นโยบาย
 * MAX_EXPANSION_DEPTH (0 = ห้าม, k = ทำได้พอประมาณ) — **ยังไม่ตัดสินใจ**.
 *
 * Decision (2026-08-16): MAX_EXPANSION_DEPTH = envelope_depth(gate) —
 * the deepest k where the MARGINAL step into it still pays (ROI ≥ gate),
 * computed from the SAME model as test_tess_leverage:
 *
 *   fp(k)      = view(k) + residual(k)      ghost footprint at depth k
 *                view(k) = BASE >> k (BASE = 1152, 1 tesseract)
 *                residual(k) = 8k (Δ log ∝ events)
 *   roi_step(k)= (fp(k) − fp(k+1)) / dcost  marginal ROI of ONE more step
 *                dcost = 16 (Δresidual 8 + replay 8 — per block, no ×N)
 *   envelope_depth(gate) = max{ k : roi_step(k−1) ≥ gate }   (monotonic ↓)
 *
 * Curve (matches test_tess_leverage):
 *   roi_step: k=1: 17.5 | 2: 8.5 | 3: 4.0 | 4: 1.75 | 5: 0.625 | 6: 0.0625 | 7+: <0
 *   envelope_depth(1.0) = 5   ← "k 4–5 เหมาะสมที่สุด" (user)
 *   envelope_depth(2.0) = 4   ← conservative (GATE=2 → ห้าม depth 5)
 *   envelope_depth(0.5) = 6   ← aggressive
 *   hard ceiling k=7: fp(k+1) > fp(k) — ลึกไป footprint โตกลับ → never worth
 *
 * Lift decision (ย่อฟรี ขยายจ่าย):
 *   depth = (to_scale > from_scale) ? to_scale − from_scale : 0
 *   requested depth > envelope_depth(gate) → LIFT (ghost) แทนการขยายเสาเข็ม
 *   contraction (to < from) = ฟรี → depth 0 → ไม่ lift
 *
 * The knob = gate: 1.0 (default, cliff ที่ depth 6) / 2.0 (conservative,
 * cliff ที่ depth 5 — "ห้ามตั้งแต่ k=4+1") / 0.5 (aggressive, depth 7).
 *
 * All integer except the ROI ratio (same as test_tess_leverage — the
 * proven numbers 8.5/4.0/1.75/0.625 are the contract).
 */

#ifndef GEO_GHOST_ENVELOPE_H
#define GEO_GHOST_ENVELOPE_H

#include <stdint.h>

/* ── Constants (same model as test_tess_leverage.c) ────────── */
#define GHT_WIN          20736u
#define GHT_CHUNK        144u
#define GHT_FCHUNKS      8u
#define GHT_BASE         (GHT_FCHUNKS * GHT_CHUNK)   /* 1152 — 1 tesseract */
#define GHT_EVENT_SLOT   1u
#define GHT_REPLAY_EVENT GHT_FCHUNKS                /* 8                  */
#define GHT_DCOST        (GHT_REPLAY_EVENT + GHT_EVENT_SLOT * GHT_FCHUNKS) /* 16 */

/* default gate — trained champion (§15.70/15.71): joint training ข้าม 4 GGUF
   จริง (Qwen3/Qwen2.5/LFM/Kokoro) หา kmax=4 (= gate 3.0) — จุด ROI cliff
   (k 4-5 เหมาะสมที่สุด — ตรงกับที่จูนมือเจอ) — ค่าเก่า 1.0 (kmax=5) reject
   ทุกโมเดล (T1.2) */
#define GHT_GATE_DEFAULT 3.0

/* ════════════════════════════════════════════════════════════
   FOOTPRINT MODEL (identical to test_tess_leverage)
   ════════════════════════════════════════════════════════════ */

static inline uint64_t ght_view(uint32_t k) {
    return (k >= 11) ? 0u : (uint64_t)GHT_BASE >> k;
}

static inline uint64_t ght_residual(uint32_t k) {
    return (uint64_t)k * GHT_FCHUNKS * GHT_EVENT_SLOT;   /* = 8k */
}

/* ghost footprint at depth k — view หด B/2ᵏ + residual 8k */
static inline uint64_t ght_fp(uint32_t k) {
    return ght_view(k) + ght_residual(k);
}

/* ════════════════════════════════════════════════════════════
   MARGINAL ROI — one more step of deepening, single block
   ════════════════════════════════════════════════════════════ */

/* roi of step k→k+1: footprint saved / cost.  < 0 = fp grows back
   (hard ceiling — ลึกไปทำให้ก้อนบวมเอง) */
static inline double ght_roi_step(uint32_t k) {
    if (ght_fp(k) <= ght_fp(k + 1)) return -1.0;
    double ben = (double)(ght_fp(k) - ght_fp(k + 1));
    return ben / (double)GHT_DCOST;
}

/* ════════════════════════════════════════════════════════════
   ENVELOPE — the decision (closes §11.6)
   ════════════════════════════════════════════════════════════ */

/* deepest k where the step INTO it (k−1→k) pays ≥ gate.
   Monotonic decreasing curve → first below gate stops the scan. */
static inline uint32_t ght_envelope_depth(double gate) {
    uint32_t best = 0;
    for (uint32_t k = 2; k <= 10; k++) {
        double roi = ght_roi_step(k - 1);
        if (roi < 0.0) break;            /* hard ceiling */
        if (roi < gate) break;           /* cliff — วิญญาณถ่วงกลืนกำไร */
        best = k;
    }
    return best;
}

/* expansion depth on the scale axis:
   ย่อฟรี (to < from) → 0 — contraction never lifts; ขยายจ่าย = ระยะ */
static inline uint32_t ght_scale_depth(uint8_t from_scale, uint8_t to_scale) {
    return (to_scale > from_scale) ? (uint32_t)(to_scale - from_scale) : 0u;
}

/* 1 = requested scale exceeds the block's envelope → must lift */
static inline int ght_needs_lift(double gate, uint8_t from_scale, uint8_t to_scale) {
    return ght_scale_depth(from_scale, to_scale) > ght_envelope_depth(gate);
}

#endif /* GEO_GHOST_ENVELOPE_H */
