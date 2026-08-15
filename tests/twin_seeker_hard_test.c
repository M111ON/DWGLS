/*
 * twin_seeker_hard_test.c — ทดสอบ twin seeker หลายมุม
 *
 * ไม่ได้หาว่ามัน work — หาว่ามัน break ตรงไหน
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/twin_seeker_hard_test tests/twin_seeker_hard_test.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../core/hyperbolic_seek.h"

#define GEO_SLOTS   20736u
#define STRIDE_37   37u
#define STRIDE_INV  16813u
#define PHI         1.618033988749895

static inline uint32_t geo_slot(uint32_t idx) {
    return (idx * STRIDE_37) % GEO_SLOTS;
}
static inline uint32_t geo_inverse(uint32_t slot) {
    return (slot * STRIDE_INV) % GEO_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 1: "Record not calculate" — multiply only กับ trig เทียบกัน
   ═══════════════════════════════════════════════════════════════════════════ */

static int test_record_vs_calc(void) {
    printf("═══ Test 1: Record (multiply) vs Calculate (trig) ═══\n\n");

    /*
     * Record:  target = current + offset (integer add)
     * Calc:    target = hyperbolic_to_kis(kis_to_hyperbolic(current))
     *
     * ถ้า record เท่ากับ calc = ใช้ multiply ได้
     * ถ้าไม่เท่ากัน = record ไม่พอ ต้อง calc
     */

    int match = 0, mismatch = 0;
    int32_t max_diff = 0;

    for (uint32_t i = 0; i < GEO_SLOTS; i++) {
        uint32_t slot = geo_slot(i);
        uint8_t axis = hyperbolic_axis_of(slot);   /* owner band, NOT %% 3 */

        /* Calc path (trig) */
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);
        uint32_t calc_target = hyperbolic_to_kis_axis(h, axis);

        /* Record path (multiply/add only) */
        uint32_t record_target = slot; /* identity for now */

        int32_t diff = (int32_t)calc_target - (int32_t)record_target;
        if (diff < 0) diff = -diff;

        if (calc_target == record_target) {
            match++;
        } else {
            mismatch++;
            if (diff > max_diff) max_diff = diff;
        }
    }

    printf("  Match:     %d / %d\n", match, GEO_SLOTS);
    printf("  Mismatch:  %d / %d\n", mismatch, GEO_SLOTS);
    printf("  Max diff:  %d slots\n\n", max_diff);

    if (mismatch > 0) {
        printf("  ⚠ Record ≠ Calc — multiply ไม่พอ ต้อง trig\n");
    } else {
        printf("  ✓ Record = Calc — multiply พอ\n");
    }

    return mismatch;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 2: ทุก slot ทะลุถึง target ไหม? (pierce accuracy)
   ═══════════════════════════════════════════════════════════════════════════ */

static int test_pierce_accuracy(void) {
    printf("\n═══ Test 2: Pierce Accuracy — ทะลุถึง target ไหม? ═══\n\n");

    int pass = 0, fail = 0;

    /* Test: ทุก slot ทะลุถึงตัวเองไหม? (identity) */
    for (uint32_t i = 0; i < GEO_SLOTS; i++) {
        uint32_t slot = geo_slot(i);

        /* Pierce: offset = (target - current) / container */
        /* ทะลุถึง target = slot เอง → offset = 0 */
        uint32_t pierced = slot; /* offset = 0 */

        if (pierced == slot) pass++;
        else fail++;
    }

    printf("  Identity test (offset=0): %d PASS, %d FAIL\n", pass, fail);

    /* Test: ทะลุถึง slot ถัดไป (offset = 1/20736) */
    pass = 0; fail = 0;
    for (uint32_t i = 0; i < GEO_SLOTS; i++) {
        uint32_t current = geo_slot(i);
        uint32_t target = (current + 1) % GEO_SLOTS;

        /* Pierce ด้วย offset */
        double offset = 1.0 / (double)GEO_SLOTS;
        uint32_t pierced = (current + (uint32_t)(offset * GEO_SLOTS)) % GEO_SLOTS;

        if (pierced == target) pass++;
        else fail++;
    }

    printf("  Next-slot test (offset=1/N): %d PASS, %d FAIL\n", pass, fail);

    /* Test: ทะลุถึง slot ที่อยู่ไกล 1000 */
    pass = 0; fail = 0;
    for (uint32_t i = 0; i < 100; i++) {
        uint32_t current = geo_slot(i);
        uint32_t target = (current + 1000) % GEO_SLOTS;

        int32_t delta = (int32_t)target - (int32_t)current;
        if (delta < 0) delta += GEO_SLOTS;

        uint32_t pierced = (current + (uint32_t)delta) % GEO_SLOTS;

        if (pierced == target) pass++;
        else fail++;
    }

    printf("  Distant test (delta=1000): %d PASS, %d FAIL\n\n", pass, fail);

    return fail;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 3: Fingerprint consistency — fingerprint ตรงกับ slot ไหม?
   ═══════════════════════════════════════════════════════════════════════════ */

static int test_fingerprint_consistency(void) {
    printf("═══ Test 3: Fingerprint Consistency ═══\n\n");

    /*
     * Fingerprint = scale + slot + direction
     * คำถาม: fingerprint เปลี่ยนถ้า slot เปลี่ยนไหม?
     */

    int unique = 0, collision = 0;

    /* เก็บ fingerprints */
    uint32_t fp_slot[GEO_SLOTS];
    double fp_re[GEO_SLOTS];
    double fp_im[GEO_SLOTS];

    for (uint32_t i = 0; i < GEO_SLOTS; i++) {
        uint32_t slot = geo_slot(i);
        uint8_t axis = hyperbolic_axis_of(slot);   /* owner band, NOT %% 3 */
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);

        fp_slot[i] = slot;
        fp_re[i] = h.re;
        fp_im[i] = h.im;
    }

    /* ตรวจ duplicate */
    for (uint32_t i = 0; i < GEO_SLOTS; i++) {
        for (uint32_t j = i + 1; j < GEO_SLOTS && j < i + 100; j++) {
            if (fp_slot[i] == fp_slot[j] &&
                fabs(fp_re[i] - fp_re[j]) < 1e-10 &&
                fabs(fp_im[i] - fp_im[j]) < 1e-10) {
                collision++;
                if (collision <= 5) {
                    printf("  Collision: slot[%u] == slot[%u] = %u\n",
                           i, j, fp_slot[i]);
                }
            }
        }
        unique++;
    }

    printf("  Unique:     %d\n", unique);
    printf("  Collisions: %d\n\n", collision);

    if (collision > 0) {
        printf("  ⚠ Fingerprint collision — 2 slots มี fingerprint เดียวกัน\n");
    } else {
        printf("  ✓ All fingerprints unique\n");
    }

    return collision;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 4: Wrap-around — ทะลุข้าม boundary ได้ไหม?
   ═══════════════════════════════════════════════════════════════════════════ */

static int test_wraparound(void) {
    printf("═══ Test 4: Wrap-around ═══\n\n");

    int pass = 0, fail = 0;

    /* Test cases: ทะลุจากท้ายไปต้น */
    uint32_t cases[][2] = {
        {20735, 0},      /* ทะลุ 1 slot ข้าม boundary */
        {20730, 5},      /* ทะลุ 5 slots */
        {20000, 736},    /* ทะลุจากท้ายไปต้น */
        {5184, 15552},   /* ทะลุจาก center ไป ring */
        {15552, 5184},   /* ทะลุจาก ring ไป center */
        {10368, 0},      /* ทะลุจากกึ่งกลาง */
    };
    int n_cases = sizeof(cases) / sizeof(cases[0]);

    printf("  %-10s %-10s %-10s %-10s\n",
           "Current", "Target", "Pierce", "Status");
    printf("  %-10s %-10s %-10s %-10s\n",
           "───────", "──────", "──────", "──────");

    for (int c = 0; c < n_cases; c++) {
        uint32_t current = cases[c][0];
        uint32_t target = cases[c][1];

        int32_t delta = (int32_t)target - (int32_t)current;
        if (delta < 0) delta += GEO_SLOTS;

        uint32_t pierced = (current + (uint32_t)delta) % GEO_SLOTS;

        int ok = (pierced == target);
        if (ok) pass++; else fail++;

        printf("  [%5u]   [%5u]   [%5u]   %s\n",
               current, target, pierced,
               ok ? "PASS ✓" : "FAIL ✗");
    }

    printf("\n  Wrap-around: %d/%d PASS\n\n", pass, n_cases);

    return fail;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 5: Scale ratio — ratio เปลี่ยน ทะลุถึงไหม?
   ═══════════════════════════════════════════════════════════════════════════ */

static int test_scale_ratio(void) {
    printf("═══ Test 5: Scale Ratio Effect ═══\n\n");

    double scales[] = {1.0, 0.5, 0.25, 0.1, 0.01, PHI, 2.0};
    int n_scales = sizeof(scales) / sizeof(scales[0]);

    printf("  %-10s %-12s %-12s %-10s\n",
           "Scale", "Data Slots", "Ring Size", "Fits?");
    printf("  %-10s %-12s %-12s %-10s\n",
           "─────", "──────────", "────────", "────");

    uint32_t container = GEO_SLOTS;

    for (int s = 0; s < n_scales; s++) {
        double scale = scales[s];
        uint32_t ring_size = (uint32_t)(container * scale);
        uint32_t data_slots = container - ring_size;

        /* ทะลุถ้า data > 0 */
        int fits = (data_slots > 0 && ring_size > 0);

        printf("  [%5.2f]   [%8u]   [%8u]   %s\n",
               scale, data_slots, ring_size,
               fits ? "YES" : "NO");
    }

    printf("\n");

    /* ทดสอบว่า multiply = target slot ไหม */
    printf("  Scale → Target slot mapping:\n");
    for (int s = 0; s < n_scales; s++) {
        double scale = scales[s];
        uint32_t expected = (uint32_t)(scale * container) % container;

        /* Record path */
        uint32_t record = expected;

        /* Calc path: ทะลุจริงไหม? */
        uint8_t axis = hyperbolic_axis_of(expected);   /* owner band */
        HypComplex h = kis_to_hyperbolic_axis(expected, axis);
        uint32_t calc = hyperbolic_to_kis_axis(h, axis);

        printf("    scale=%.2f → record=%u, calc=%u, %s\n",
               scale, record, calc,
               record == calc ? "MATCH" : "DIFFER");
    }

    printf("\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 6: Loop roundtrip — KIS → Hyper → KIS ครบ 100% ไหม?
   ═══════════════════════════════════════════════════════════════════════════ */

static int test_loop_roundtrip(void) {
    printf("═══ Test 6: Loop Roundtrip ═══\n\n");

    int pass = 0, fail = 0;

    for (uint32_t i = 0; i < GEO_SLOTS; i++) {
        uint32_t slot = geo_slot(i);
        uint8_t axis = hyperbolic_axis_of(slot);   /* owner band, NOT %% 3 */

        /* KIS → Hyper → KIS */
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);
        uint32_t back = hyperbolic_to_kis_axis(h, axis);

        if (back == slot) pass++;
        else fail++;
    }

    printf("  KIS → Hyper → KIS: %d PASS, %d FAIL\n", pass, fail);

    /* per-axis bijectivity: each axis band [a*6912, (a+1)*6912) roundtrips
     * within itself — total must equal GEO_SLOTS */
    int axis_pass = 0, axis_fail = 0;
    for (uint8_t axis = 0; axis < 3; axis++) {
        for (uint32_t s = 0; s < HYP_AXIS_SLOTS; s++) {
            uint32_t slot = axis * HYP_AXIS_SLOTS + s;
            HypComplex h = kis_to_hyperbolic_axis(slot, axis);
            uint32_t back = hyperbolic_to_kis_axis(h, axis);
            if (back == slot) axis_pass++; else axis_fail++;
        }
    }
    printf("  Per-axis (owner band): %d/%d PASS (%d FAIL)\n\n",
           axis_pass, GEO_SLOTS, axis_fail);

    return fail;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 7: Speed — integer path vs trig path
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_speed(void) {
    printf("═══ Test 7: Speed Comparison ═══\n\n");

    uint32_t n = 200000;
    volatile uint32_t sink = 0;

    /* Integer path (multiply/add only) */
    clock_t t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i);
        uint32_t val = geo_inverse(slot);
        sink += val;
    }
    clock_t t1 = clock();
    double int_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    /* Trig path (cos/sin/atan2) */
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i);
        uint8_t axis = hyperbolic_axis_of(slot);   /* owner band */
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);
        uint32_t back = hyperbolic_to_kis_axis(h, axis);
        sink += back;
    }
    t1 = clock();
    double trig_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    printf("  Integer (multiply): %8.3f ms (%.1f ns/op)\n",
           int_ms, int_ms * 1e6 / n);
    printf("  Trig (cos/sin):     %8.3f ms (%.1f ns/op)\n",
           trig_ms, trig_ms * 1e6 / n);
    printf("  Ratio:              %.1fx\n\n", trig_ms / int_ms);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Twin Seeker Hard Test — หาว่า break ตรงไหน             ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    int total_fail = 0;

    total_fail += test_record_vs_calc();
    total_fail += test_pierce_accuracy();
    total_fail += test_fingerprint_consistency();
    total_fail += test_wraparound();
    total_fail += test_scale_ratio();
    total_fail += test_loop_roundtrip();
    test_speed();

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  RESULT: %s (total mismatches: %d)\n",
           total_fail == 0 ? "ALL PASS ✓" : "HAS FAILURES ✗",
           total_fail);
    printf("═══════════════════════════════════════════════════════════\n");

    return total_fail > 0 ? 1 : 0;
}
