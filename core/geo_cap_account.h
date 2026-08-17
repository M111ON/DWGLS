/*
 * geo_cap_account.h — Field Capacity Accounting (§11.6)
 * ═══════════════════════════════════════════════════════════════════════════
 * §11.6: เสาเข็มต้องประกาศ envelope (w₀, depth k, ขนาดที่ w₀+k) —
 *   capacity = Σ envelope ≤ 20736, เกิน = reject deterministic (ไม่ silent).
 *
 * This layer closes the accounting half of §11.6 (the depth bound half is
 * §15.32 / geo_ghost_envelope.h):
 *
 *   - envelope size of a block at depth k = ght_fp(k) — the ghost
 *     footprint (view หด B/2ᵏ + residual 8k) — what the block ACTUALLY
 *     reserves in the field (§15.2: ghost ไม่จอง 2ᵏ; capacity(k)=WIN/fp(k)
 *     ตรงกับ test_tess_leverage เป๊ะ).
 *   - admission is PURE + explicit: same sequence → same verdict, and a
 *     reject is COUNTED (never silent).  Three verdicts:
 *
 *       CAP_LIFT   — requested depth > envelope_depth(gate) → deflect to
 *                    the ghost store (§15.32 auto-lift) — consumes ZERO
 *                    field capacity (overcommitment หายตั้งแต่ต้นทาง).
 *       CAP_REJECT — within envelope but Σ would exceed 20736 → ชนขอบ —
 *                    deterministic reject, counted.
 *       CAP_ADMIT  — placed; used += fp(k).
 *
 *   - contraction (ย่อฟรี) → depth 0 → cheapest envelope fp(0) = 1152
 *     (1 tesseract — "18 tes" consistency: ≤ 18 blocks at base).
 *
 * All header-only, static inline, int math (gate is the only double —
 * same as the envelope module).
 */

#ifndef GEO_CAP_ACCOUNT_H
#define GEO_CAP_ACCOUNT_H

#include <stdint.h>
#include <string.h>
#include "geo_ghost_envelope.h"

#define CAP_WIN GHT_WIN   /* 20736 */

/* ════════════════════════════════════════════════════════════
   TRAINED DEFAULT PLACEMENT RULE (§15.70/15.71 — unified champion)
   ──
   Evolution (joint training, fitness = Σ per-model cost) หา rule set
   เดียวที่ 0 rejects บน 4 GGUF จริงพร้อมกัน (Qwen3 9248 · Qwen2.5 13872 ·
   LFM 16184 · Kokoro 0 field) — เท่ากับ per-model optimum ของแต่ละตัว และ
   ผ่าน rotation theorem (gcd(115,144)=1: ทุก L|144 rotates + uniform):

     stride 115 · offset 115 (ย้าย rank-0 scale > kmax → ทั้งก้อนเข้า ghost)
     gate 3.0 (kmax=4, GHT_GATE_DEFAULT) · orbit 1 · chunk 262144

   stride/offset = กฎการวาง w = (stride·rank + offset) % 144 —
   chunk = ขนาด block ต่อการวาง — ใช้เป็นค่าเริ่มต้นของ placement chain
   (field_trainer / cap_chain_scan — เดิม 37,0,1.0,16K reject ทุกโมเดล T1.2)
   ════════════════════════════════════════════════════════════ */
#define CAP_RULE_STRIDE 115u
#define CAP_RULE_OFFSET 115u
#define CAP_RULE_GATE   GHT_GATE_DEFAULT   /* 3.0 → kmax 4 */
#define CAP_RULE_ORBIT  1u
#define CAP_RULE_CHUNK  262144u

/* ── trained scale resolver — THE single rule (placement AND read) ──
   block/rank → เป้าหมาย scale บนแกน 144: w = (stride·rank + offset) % 144
   — ใช้ที่เดียว: วาง (chain) · admit (gate) · อ่าน (ghost_read_rule) —
   ถ้าอ่านกับวางใช้กฎคนละตัว → bond ต่าง → thaw fail โดย construction
   (จาก_scale เป็นส่วนหนึ่งของ address — เสาเข็มห้ามขยับ) */
static inline uint8_t cap_rule_scale(uint32_t rank) {
    return (uint8_t)(((uint64_t)CAP_RULE_STRIDE * rank + CAP_RULE_OFFSET) % 144u);
}

/* ── verdicts ─────────────────────────────────────────────── */
#define CAP_ADMIT   1     /* placed — capacity consumed          */
#define CAP_REJECT  0     /* over capacity — deterministic reject */
#define CAP_LIFT   (-1)   /* beyond envelope — ghost store instead */

typedef struct {
    uint64_t used;        /* Σ envelope (slots) */
    uint32_t blocks;      /* admitted blocks    */
    uint32_t rejects;     /* over-capacity rejects (explicit, never silent) */
    uint32_t lifts;       /* beyond-envelope deflects to ghost store */
} CapAccount;

static inline void cap_init(CapAccount *a) {
    if (!a) return;
    memset(a, 0, sizeof(*a));
}

/* declared reservation of a block at depth k — ghost footprint fp(k) */
static inline uint64_t cap_envelope_size(uint32_t k) {
    return ght_fp(k);
}

/* remaining field slots */
static inline uint64_t cap_remaining(const CapAccount *a) {
    if (!a) return 0;
    return (a->used < CAP_WIN) ? CAP_WIN - a->used : 0u;
}

/* field load 0..1 */
static inline double cap_load(const CapAccount *a) {
    if (!a) return 0.0;
    return (double)a->used / (double)CAP_WIN;
}

/* ════════════════════════════════════════════════════════════
   ADMISSION — deterministic, explicit reject
   ════════════════════════════════════════════════════════════
   Block born at w0 requests to_scale.  Verdict:
     depth > envelope_depth(gate)  → CAP_LIFT   (ghost, 0 field cost)
     used + fp(k) > 20736          → CAP_REJECT (counted)
     else                          → CAP_ADMIT  (used += fp(k))      */
static inline int cap_admit(CapAccount *a, double gate,
                            uint8_t w0, uint8_t to_scale) {
    if (!a) return CAP_REJECT;

    uint32_t k = ght_scale_depth(w0, to_scale);
    if (k > ght_envelope_depth(gate)) {
        a->lifts++;
        return CAP_LIFT;                    /* §15.32 — ghost store */
    }

    uint64_t env = cap_envelope_size(k);
    if (a->used + env > CAP_WIN) {
        a->rejects++;                       /* ไม่ silent */
        return CAP_REJECT;
    }

    a->used += env;
    a->blocks++;
    return CAP_ADMIT;
}

#endif /* GEO_CAP_ACCOUNT_H */
