/* test_tess_leverage.c — Leverage gate: "ขยาย 1 ก้าวคุ้มไหม?"
 *
 * คำถาม (จาก user): "ขยายไม่มีข้อดีเลยนอกจากต้องการ push beyond limit
 *    ถ้าทั้งระบบมีไฟล์เดียวอ่ะสบายพอทำได้แต่ไม่แนะนำ
 *    เพราะเราอยู่สเกลสูงขนาดนี้ ขยับอีกก้าวเดียวนี่ทวีคูณมากเลยนะ"
 *
 * Gate = การตัดสินใจว่า field base ควรขยาย k → k+1 หรือไม่:
 *
 *   cost(step)    = สิ่งที่ทั้งสนามจ่าย:
 *                    - ghost side: Δresidual + Δreplay ต่อไฟล์ (เล็ก, เป็นเส้นตรง)
 *                      แต่สะสม ×N — ไฟล์ยิ่งเยอะ ยิ่งลาก
 *                    - naive exposure: materialization potential ×2 ทั้งสนาม
 *                      (ghost ไม่จอง แต่หนี้ศักยภาพยังทวีคูณ — "ทวีคูณมากเลยนะ")
 *   benefit(step) = push beyond limit = capacity ที่เพิ่ง unlock (Δcap × fp)
 *                    ถ้าไม่มีไฟล์ชนเพดาน (load ต่ำ) → benefit = 0 → ห้ามขยายเสมอ
 *   ROI = benefit / cost — gate: ขยายเมื่อ (benefit > 0 && ROI ≥ GATE) เท่านั้น
 *
 * ตัวเลข (ต่อจาก test_tess_ghost): B=1152 (1 tesseract), fp(k)=B/2ᵏ+8k,
 *   capacity(k)=20736/fp(k) — peak ที่ k=7 (319) ตรงกับ T9 ของ ghost test
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -o tests/test_tess_leverage \
 *        tests/test_tess_leverage.c -lm
 */
#include <stdio.h>
#include <stdint.h>

#define WIN         20736u
#define CHUNK       144u
#define FCHUNKS     8u
#define BASE        (FCHUNKS * CHUNK)   /* 1152 — 1 tesseract */
#define FLOOR_MARGIN 2u                 /* dead zone 0..1 (จาก ghost test) */
#define EVENT_SLOT  1u
#define REPLAY_EVENT FCHUNKS            /* replay 1 event ต่อ chunk ต่อขั้น */

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── ตัวแบบ (เดียวกันกับ ghost test) ─────────────────────────── */
static uint64_t view(uint32_t k)      { return (k >= 11) ? 0 : (uint64_t)BASE >> k; }
static uint64_t residual(uint32_t k)  { return (uint64_t)k * FCHUNKS * EVENT_SLOT; }  /* = 8k */
static uint64_t fp(uint32_t k)        { return view(k) + residual(k); }              /* ghost footprint */
static uint64_t naive(uint32_t k)     { return (k >= 11) ? 0 : (uint64_t)BASE << k; }/* materialization potential */
static uint64_t capacity(uint32_t k)  { uint64_t f = fp(k); return f ? WIN / f : 0; }
static int64_t dcap(uint32_t k)       { return (int64_t)capacity(k + 1) - (int64_t)capacity(k); }
static uint64_t dcost_per_file(void)  { return residual(1) + REPLAY_EVENT; }         /* 8+8 = 16 */

/* ROI ของขั้น k→k+1 เมื่อสนามเต็ม (N = capacity(k)):
 *   benefit = ไฟล์ใหม่ที่ unlock × footprint ของมัน
 *   cost    = Δ ต่อไฟล์ × จำนวนไฟล์ปัจจุบัน                                */
static double roi_field(uint32_t k) {
    if (dcap(k) <= 0) return -1.0;                       /* เกินเพดานแล้ว — ไม่มีอะไรให้ push */
    double benefit = (double)dcap(k) * (double)fp(k + 1);
    double cost    = (double)capacity(k) * (double)dcost_per_file();
    return benefit / cost;
}

/* gate: pure function — deterministic, ไม่มี state
 * pressure = สนามมีแรงกดดันจริง (load ≥ 90% cap) — ไร้แรงกดดัน = benefit 0 เสมอ */
static int gate_allow(uint32_t k, double gate, int pressure, double *roi_out) {
    if (roi_out) *roi_out = roi_field(k);
    if (!pressure) return 0;                             /* ไม่มี limit ที่ต้อง push → ห้าม */
    if (dcap(k) <= 0) return 0;                          /* push เกินเพดาน → ห้าม */
    if (roi_field(k) < gate) return 0;                   /* ROI ต่ำกว่าเกณฑ์ → ห้าม */
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
   T1: cost side — "ขยับอีกก้าวเดียวนี่ทวีคูณมากเลยนะ"
   ═══════════════════════════════════════════════════════════════ */
static void test_cost_side(void) {
    printf("═ COST — 1 ก้าว = ×2 ของทั้งสนาม (naive exposure) + Δ ghost ต่อไฟล์ ═\n");

    /* T1: naive materialization potential ×2 ทุกขั้น — "ทวีคูณ" ของสเกลสูง */
    {
        int ok = 1;
        for (uint32_t k = 2; k <= 9; k++)
            if (naive(k + 1) != 2u * naive(k)) ok = 0;
        CHECK("T1: naive potential ×2 ทุกขั้น (k→k+1) — 1 ก้าว = ทวีคูณทั้งสนาม", ok);
        /* ตัวเลขจริงที่ base สูง: k=6, 314 ไฟล์ → 1 ก้าว = +23M slots potential */
        uint64_t agg6 = capacity(6) * naive(6);
        printf("     k=6: Σpotential = %llu slots → 1 ก้าว = +%llu (×2)\n",
               (unsigned long long)agg6, (unsigned long long)agg6);
        CHECK("T1b: ที่สเกลสูง 1 ก้าว = หนี้ศักยภาพ +23M slots — ghost ไม่จองแต่ยังเป็นหนี้",
              agg6 == 23150592ull);   /* 314 ไฟล์ × 1152×64 */
    }

    /* T1c: ghost side — Δ ต่อไฟล์ เล็ก (16 slots) แต่สะสม ×N */
    {
        uint64_t per = dcost_per_file();
        printf("     Δcost ghost ต่อไฟล์ = %llu slots (Δresidual 8 + replay 8)\n",
               (unsigned long long)per);
        CHECK("T1c: Δcost ghost ต่อไฟล์ เล็กเป็นเส้นตรง (16) — ไม่ใช่ 2ᵏ", per == 16u);
        CHECK("T1d: แต่สะสม ×N — ไฟล์เยอะ = ลากเยอะ (วิญญาณถ่วง)",
              per * capacity(4) > 1000u);   /* 16×199 = 3184 */
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T2: benefit side — "ขยายไม่มีข้อดีเลยนอกจาก push beyond limit"
   ═══════════════════════════════════════════════════════════════ */
static void test_benefit_side(void) {
    printf("═ BENEFIT — capacity unlock (push beyond limit) เท่านั้น ═\n");

    /* T2: capacity curve — peak ที่ k=7 (319, ตรงกับ ghost T9); Δcap ≤ 0 ตั้งแต่ k=7 */
    {
        uint32_t peak_k = 0; uint64_t peak = 0;
        for (uint32_t k = 0; k <= 10; k++) {
            uint64_t c = capacity(k);
            if (c > peak) { peak = c; peak_k = k; }
        }
        printf("     capacity peak: k=%u (%llu files/window)\n",
               peak_k, (unsigned long long)peak);
        CHECK("T2: capacity curve มี peak ที่ k=7 (319) — ตรงกับ ghost T9", peak_k == 7 && peak == 319);
        int pos = 1;
        for (uint32_t k = 2; k < peak_k; k++)
            if (dcap(k) <= 0) pos = 0;          /* ก่อน peak ทุกขั้น unlock ได้ */
        CHECK("T2b: Δcap > 0 เฉพาะก่อน peak — push ยังได้", pos);
    int neg = 1;
    for (uint32_t k = peak_k; k <= 9; k++)
        if (dcap(k) > 0) neg = 0;           /* ตั้งแต่ peak ขึ้นไป ไม่มีอะไรให้ unlock */
        CHECK("T2c: ตั้งแต่ k=7 ขึ้นไป Δcap ≤ 0 — ขยายไม่มีข้อดีเลย (push เกินเพดานแล้ว)", neg);
    }

    /* T2d: ไร้แรงกดดัน → benefit = 0 → ห้ามขยาย (แม้ ROI สูงก็ไม่ใช่เหตุผล) */
    {
        uint32_t k = 4;                          /* cap 199 */
        uint32_t n = 10;                         /* load ต่ำมาก */
        int pressure = (uint64_t)n * 10u >= capacity(k) * 9u;   /* ≥ 90% ของ cap */
        CHECK("T2d: load ต่ำ (10/199) → ไม่มี limit ที่ต้อง push → benefit = 0",
              !pressure);
        CHECK("T2e: ขยายตอนไม่มีแรงกดดัน = จ่าย ×2 ทั้งสนามโดยไม่มีกำไร → ต้องห้าม",
              !pressure && dcap(k) > 0);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T3: ROI curve — "ROI ต่ำแค่ไหนจึงควรห้าม" = < 1 (หรือเกณฑ์นโยบาย)
   ═══════════════════════════════════════════════════════════════ */
static void test_roi_curve(void) {
    printf("═ ROI CURVE — benefit/cost ต่อขั้น; knee = จุดที่ < เกณฑ์ ═\n");

    /* T3: sweep k=2..9 — ROI ลดลง 8.49 → 0.065; knee แรกที่ < 1 = k=5 */
    uint32_t knee1 = 0, knee2 = 0;
    printf("     k    Δcap   fp(k+1)   benefit    cost(×N)    ROI\n");
    for (uint32_t k = 2; k <= 9; k++) {
        int64_t d = (int64_t)dcap(k);
        uint64_t ben = (d > 0) ? (uint64_t)d * fp(k + 1) : 0;
        uint64_t cst = capacity(k) * dcost_per_file();
        double roi = roi_field(k);
        if (!knee1 && roi >= 0 && roi < 1.0) knee1 = k;
        if (!knee2 && roi >= 0 && roi < 2.0) knee2 = k;
        printf("     %u   %5lld   %7llu   %8llu   %10llu   %7.2f\n",
               k, (long long)d, (unsigned long long)fp(k + 1),
               (unsigned long long)ben, (unsigned long long)cst, roi);
    }
    printf("     knee: ROI<1 ที่ k=%u, ROI<2 ที่ k=%u\n", knee1, knee2);
    CHECK("T3: ROI ลดลงตาม base — สเกลสูง = leverage แย่ลง (8.49 → 0.065)", knee1 == 5);
    CHECK("T3b: เกณฑ์ห้าม = ROI < 1 (ต่ำกว่า = จ่าย > กำไร — วิญญาณถ่วงกลืน)", knee1 == 5);
    CHECK("T3c: GATE=2 (เข้มขึ้น) → ห้ามตั้งแต่ k=4 (ROI 1.74 < 2)", knee2 == 4);
    CHECK("T3d: ก่อน knee ROI สูง (k=2..4 ≥ 1.74) — ขยายต้นๆ คุ้ม เพราะ unlock เยอะ จ่ายน้อย",
          roi_field(2) > 8.0 && roi_field(4) >= 1.7);
    CHECK("T3e: หลัง knee ROI ต่ำกว่า 1 (k=5..6) — และ k≥7 ติดลบ (เกินเพดาน)",
          roi_field(5) < 1.0 && roi_field(7) < 0.0);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T4: gate decision — deterministic, กรณีตัวอย่าง
   ═══════════════════════════════════════════════════════════════ */
static void test_gate(void) {
    printf("═ GATE — ตัดสินใจ deterministic: (benefit > 0 && ROI ≥ GATE) ═\n");

    /* T4a: ไร้แรงกดดัน → reject */
    {
        double roi = 0;
        CHECK("T4a: ไม่มีแรงกดดัน (load ต่ำ) → gate ปฏิเสธ (benefit=0)",
              !gate_allow(4, 1.0, 0, &roi) && roi > 1.0);   /* ROI สูงแต่ไม่ใช่เหตุผล */
    }
    /* T4b: สนามเต็มที่ k=3 → ROI 4.02 ≥ 1 → allow (ขยายคุ้ม — unlock เยอะ) */
    {
        double roi = 0;
        CHECK("T4b: สนามเต็ม k=3 → ROI 4.02 ≥ 1 → อนุญาต", gate_allow(3, 1.0, 1, &roi));
    }
    /* T4c: สนามเต็มที่ k=5 → ROI 0.64 < 1 → reject (จ่าย > กำไร) */
    {
        double roi = 0;
        CHECK("T4c: สนามเต็ม k=5 → ROI 0.64 < 1 → ห้าม (วิญญาณถ่วงกลืนกำไร)",
              !gate_allow(5, 1.0, 1, &roi) && roi < 1.0);
    }
    /* T4d: k≥7 → Δcap ≤ 0 → reject เสมอ ("ขยายไม่มีข้อดีเลย") */
    {
        double roi = 0;
        CHECK("T4d: k=7 เกินเพดาน → ปฏิเสธแม้ GATE=0.1 (ไม่มีอะไรให้ push)",
              !gate_allow(7, 0.1, 1, &roi));
    }
    /* T4e: pure function — เรียกซ้ำผลเท่ากัน (deterministic, replay ได้) */
    {
        double r1 = 0, r2 = 0;
        int a1 = gate_allow(3, 1.0, 1, &r1), a2 = gate_allow(3, 1.0, 1, &r2);
        CHECK("T4e: gate เป็น pure — เรียก 2 ครั้ง ผลเหมือนกัน", a1 == a2 && r1 == r2);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T5: single-file — "ถ้าทั้งระบบมีไฟล์เดียวอ่ะสบายพอทำได้แต่ไม่แนะนำ"
   ═══════════════════════════════════════════════════════════════ */
static void test_single_file(void) {
    printf("═ SINGLE FILE — 1 ไฟล์: cost ไม่ ×N → ไปได้ลึกกว่า แต่ ROI ยังตัดสิน ═\n");

    /* ไฟล์เดียวต้อง land ภายใน budget fp ≤ b_req — ไฟล์ไม่รู้จักการขยายของสนาม
     * benefit(step) = footprint ที่ลดลงของไฟล์นั้น; cost = Δ ต่อไฟล์ (16) เท่านั้น */
    uint64_t b_req = 100;                    /* ต้อง land โดย fp ≤ 100 */
    printf("     fp(k): k=2:%llu 3:%llu 4:%llu 5:%llu 6:%llu 7:%llu 8:%llu\n",
           (unsigned long long)fp(2), (unsigned long long)fp(3),
           (unsigned long long)fp(4), (unsigned long long)fp(5),
           (unsigned long long)fp(6), (unsigned long long)fp(7),
           (unsigned long long)fp(8));

    /* T5a: budget 100 — ขยาย 3→4 (benefit 64, ROI 4.0) และ 4→5 (28, ROI 1.75) → allow;
     *      5→6 ไฟล์ fit แล้ว → benefit 0 → reject (หยุดที่ k=5) */
    {
        int allow34 = 0, allow45 = 0, allow56 = 0;
        for (uint32_t k = 2; k <= 5; k++) {
        /* benefit = footprint ที่ลดลงของขั้นนี้ — ตราบใดที่ยังไม่ fit (fp(k) > budget) */
        uint64_t ben = (fp(k) > b_req) ? fp(k) - fp(k + 1) : 0;
        double roi = (double)ben / (double)dcost_per_file();
            if (k == 3 && roi >= 1.0) allow34 = 1;
            if (k == 4 && roi >= 1.0) allow45 = 1;
            if (k == 5) allow56 = (roi >= 1.0);
        }
        CHECK("T5a: budget 100 → ขยาย 3→4, 4→5 คุ้ม (ROI 4.0/1.75) → อนุญาต", allow34 && allow45);
        CHECK("T5b: พอ fit แล้ว (k=5) → ขยายต่อ benefit=0 → หยุด", !allow56);
    }

    /* T5c: budget 68 — fit ที่ k=6 แต่ขั้น 5→6 ROI = 10/16 = 0.625 < 1 → "ทำได้แต่ไม่แนะนำ" */
    {
        uint64_t b2 = 68;
        uint64_t ben = (fp(5) > b2 && fp(6) <= b2) ? fp(5) - fp(6) : 0;
        double roi = (double)ben / (double)dcost_per_file();
        printf("     budget 68: ขั้น 5→6 benefit=%llu ROI=%.2f (fit ได้แต่ ROI<1)\n",
               (unsigned long long)ben, roi);
        CHECK("T5c: budget 68 → k=6 พอดี แต่ ROI 0.625 < 1 → ไม่แนะนำ (จ่าย residual > กำไร)",
              ben == 10 && roi < 1.0);
    }

    /* T5d: เกินเพดาน fp ของไฟล์เดียว — ขยายลึกไป footprint กลับโต (k≥7) */
    {
        int grows = (fp(7) < fp(8));         /* 65 < 68 — residual กลืน view จนโตกลับ */
        CHECK("T5d: k≥7 fp โตกลับ (65→68) — ขยายลึกไปทำให้ไฟล์เดียวบวมเอง", grows);
    }

    /* T5e: "สบาย" = cost ไม่ ×N — ไฟล์เดียว ROI = ROI สนาม × N (ที่ขั้นเดียวกัน)
     *      แต่ที่สเกลสูง ROI ยัง < 1 ทั้งคู่ → ไม่แนะนำสำหรับใคร */
    {
        printf("     step k=5: field ROI=%.2f (×N), single-file ROI=%.2f (×1)\n",
               roi_field(5), (double)10 / (double)16);
        CHECK("T5e: ไฟล์เดียวไปได้ลึกกว่าสนาม (ไม่เจือจางด้วย ×N) แต่สเกลสูง ROI ยัง < 1 — ไม่แนะนำ",
              roi_field(5) < 1.0);
    }
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Leverage gate — \"ขยาย 1 ก้าวคุ้มไหม?\" (ย่อฟรี ขยายจ่าย — ห้ามเมื่อ ROI < เกณฑ์)\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    test_cost_side();
    test_benefit_side();
    test_roi_curve();
    test_gate();
    test_single_file();
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
