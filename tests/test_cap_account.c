/* test_cap_account.c — Field Capacity Accounting (§11.6)
 * ═══════════════════════════════════════════════════════════════════════════
 * Σ block envelopes ≤ 20736, เกิน = reject deterministic (ไม่ silent).
 *
 *   T1  envelope size = fp(k) — ตรงกับ leverage model (fp 0..7)
 *   T2  admit sequence fills the window — used/load/remaining สอดคล้อง
 *   T3  over capacity → REJECT — counted, used ไม่เปลี่ยน (ไม่ silent)
 *   T4  deterministic — sequence เดิมรัน 2 รอบ → verdict เดิมทุกตัว
 *   T5  beyond envelope → LIFT (deflect, 0 field cost) — §15.32
 *   T6  contraction (ย่อฟรี) → depth 0 → admit ที่ราคา fp(0)
 *   T7  capacity peak consistency: 199 blocks @ depth 4 (cap 199 — ค่า
 *      เดียวกับ test_tess_leverage T2); ตัวที่ 200 → REJECT
 *   T8  rejects after full — explicit + deterministic (ไม่ silent)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-cap_account tests/test_cap_account.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../core/geo_cap_account.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════
   T1 — envelope size = fp(k) (leverage model contract)
   ═══════════════════════════════════════════════════════════════ */
static void test_envelope_size(void) {
    uint64_t expect[8] = { 1152, 584, 304, 168, 104, 76, 66, 65 };
    int ok = 1;
    for (uint32_t k = 0; k <= 7; k++)
        if (cap_envelope_size(k) != expect[k]) ok = 0;
    CHECK(1, "envelope size fp(k) == {1152,584,304,168,104,76,66,65}", ok);
    CHECK(1, "depth 0 = 1 tesseract (1152) — 18 tes consistency",
          cap_envelope_size(0) == GHT_BASE);
    CHECK(1, "hard ceiling: fp(8) > fp(7) — ลึกไปบวมเอง",
          cap_envelope_size(8) > cap_envelope_size(7));
}

/* ═══════════════════════════════════════════════════════════════
   T2 — admit fills the window; T3 — reject on overflow
   ═══════════════════════════════════════════════════════════════ */
static void test_admit_reject(void) {
    CapAccount a; cap_init(&a);

    /* fill with depth-4 blocks: fp(4)=104, cap = 199.38 → 199 fit */
    int verdict = CAP_ADMIT;
    for (uint32_t i = 0; i < 199; i++) {
        verdict = cap_admit(&a, 1.0, 0, 4);   /* depth 4 ≤ envelope 5 */
        if (verdict != CAP_ADMIT) break;
    }
    CHECK(2, "199 blocks @ depth 4 admitted (cap 199 — leverage peak)",
          verdict == CAP_ADMIT && a.blocks == 199);
    CHECK(2, "used == 199×104 = 20696 ≤ 20736", a.used == 20696);
    CHECK(2, "remaining == 40", cap_remaining(&a) == 40);
    CHECK(2, "load ≈ 0.9981", fabs(cap_load(&a) - 20696.0 / 20736.0) < 1e-12);

    /* 200th → over capacity → REJECT, counted, used unchanged */
    verdict = cap_admit(&a, 1.0, 0, 4);
    CHECK(3, "200th @ depth 4 → REJECT", verdict == CAP_REJECT);
    CHECK(3, "reject counted (ไม่ silent)", a.rejects == 1);
    CHECK(3, "used unchanged after reject", a.used == 20696);
    CHECK(3, "blocks unchanged after reject", a.blocks == 199);
}

/* ═══════════════════════════════════════════════════════════════
   T4 — determinism: same sequence → same verdicts
   ═══════════════════════════════════════════════════════════════ */
static void test_determinism(void) {
    CapAccount x, y; cap_init(&x); cap_init(&y);
    /* mixed sequence: depths 2,4,5,2,3,6,1,4,5,3 (w0=0) */
    uint8_t seq[10] = { 2, 4, 5, 2, 3, 6, 1, 4, 5, 3 };
    int same = 1;
    for (uint32_t i = 0; i < 10; i++) {
        int vx = cap_admit(&x, 1.0, 0, seq[i]);
        int vy = cap_admit(&y, 1.0, 0, seq[i]);
        if (vx != vy) same = 0;
    }
    CHECK(4, "same sequence → same verdict (pure + replay ได้)", same);
    CHECK(4, "state identical after replay",
          x.used == y.used && x.blocks == y.blocks &&
          x.rejects == y.rejects && x.lifts == y.lifts);
}

/* ═══════════════════════════════════════════════════════════════
   T5 — beyond envelope → LIFT (deflect, 0 field cost)
   ═══════════════════════════════════════════════════════════════ */
static void test_lift_deflect(void) {
    CapAccount a; cap_init(&a);

    /* depth 5 (≤ envelope 5) → ADMIT */
    CHECK(5, "depth 5 @GATE=1 → ADMIT", cap_admit(&a, 1.0, 0, 5) == CAP_ADMIT);
    /* depth 6 (> envelope 5) → LIFT — not even counted against capacity */
    CHECK(5, "depth 6 @GATE=1 → LIFT", cap_admit(&a, 1.0, 0, 6) == CAP_LIFT);
    CHECK(5, "lift counted, capacity untouched",
          a.lifts == 1 && a.used == 76 && a.blocks == 1);
    /* strict gate: depth 5 now beyond envelope 4 → LIFT */
    CHECK(5, "GATE=2: depth 5 → LIFT (cliff ขยับ)", cap_admit(&a, 2.0, 0, 5) == CAP_LIFT);
}

/* ═══════════════════════════════════════════════════════════════
   T6 — contraction (ย่อฟรี) → depth 0 → cheapest
   ═══════════════════════════════════════════════════════════════ */
static void test_contraction(void) {
    CapAccount a; cap_init(&a);
    CHECK(6, "contraction 10→2 → ADMIT at fp(0)", cap_admit(&a, 1.0, 10, 2) == CAP_ADMIT);
    CHECK(6, "cost = fp(0) = 1152", a.used == 1152);
    CHECK(6, "same scale → depth 0 → ADMIT", cap_admit(&a, 1.0, 3, 3) == CAP_ADMIT);
}

/* ═══════════════════════════════════════════════════════════════
   T7 — rejects explicit + deterministic even when full
   ═══════════════════════════════════════════════════════════════ */
static void test_full_reject(void) {
    CapAccount a; cap_init(&a);
    for (uint32_t i = 0; i < 199; i++) cap_admit(&a, 1.0, 0, 4);

    /* mixed attempts while full — every one REJECT (explicit), never silent */
    uint8_t seq[5] = { 0, 1, 3, 5, 4 };
    uint32_t r1 = 0;
    for (uint32_t i = 0; i < 5; i++)
        if (cap_admit(&a, 1.0, 0, seq[i]) == CAP_REJECT) r1++;

    /* rerun on a fresh account → same reject count (deterministic) */
    CapAccount b; cap_init(&b);
    for (uint32_t i = 0; i < 199; i++) cap_admit(&b, 1.0, 0, 4);
    uint32_t r2 = 0;
    for (uint32_t i = 0; i < 5; i++)
        if (cap_admit(&b, 1.0, 0, seq[i]) == CAP_REJECT) r2++;

    CHECK(7, "all 5 attempts rejected while full", r1 == 5);
    CHECK(7, "deterministic across fresh accounts", r1 == r2);
    CHECK(7, "reject count recorded — never silent", b.rejects == 5);
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Field Capacity Accounting (§11.6) — Σ envelope ≤ 20736\n");
    printf("════════════════════════════════════════════════════════\n\n");

    test_envelope_size();
    test_admit_reject();
    test_determinism();
    test_lift_deflect();
    test_contraction();
    test_full_reject();

    printf("\n════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("════════════════════════════════════════════════════════\n");

    return fail == 0 ? 0 : 1;
}
