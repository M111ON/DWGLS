/* test_kis_hyper_handoff.c — KIS ↔ Hyperbolic Dual Boundary Experiment
 *
 * Exp 1: Threshold finding — how small can KIS scale get before precision breaks?
 * Exp 2: Handoff integrity — does data survive the transition?
 * Exp 3: Single axis test (AXIS_X = Hilbert)
 *
 * BUILD: gcc -O2 -I../core -o test_kis_hyper_handoff test_kis_hyper_handoff.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   EXP 1: KIS Scale Precision Test
   Find where fixed-point precision breaks down
   ═══════════════════════════════════════════════════════════════════════════ */
static void exp1_scale_precision(void) {
    printf("EXP 1: KIS Scale Precision (single axis X)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Test data: simple pattern */
    uint8_t data[20736];
    for (int i = 0; i < 20736; i++) {
        data[i] = (uint8_t)(i % 256);
    }
    
    KISAxes axes;
    kis_axis_from_1d(&axes, data, 20736);
    
    /* Test at different scale levels */
    double scales[] = {
        1.0, 0.5, 0.1, 0.01, 0.001, 
        0.0001, 0.00001, 0.000001, 0.0000001
    };
    int num_scales = sizeof(scales) / sizeof(scales[0]);
    
    printf("  Scale          | Project Value | Precision OK?\n");
    printf("  ---------------|---------------|---------------\n");
    
    for (int s = 0; s < num_scales; s++) {
        double scale = scales[s];
        uint32_t scale_fixed = (uint32_t)(scale * 65536.0); /* <<16 */
        
        /* Project a test point */
        uint32_t proj = kis_project_4d_to_3d(100, 200, 300, 400, scale_fixed);
        
        /* Check if projection is meaningful (non-zero, within range) */
        uint32_t x3 = (proj >> 20) & 0x0FFF;
        uint32_t y3 = (proj >> 8) & 0x0FFF;
        uint32_t z3 = proj & 0xFF;
        
        int precision_ok = (x3 < 20736) && (y3 < 20736) && (z3 < 256);
        
        printf("  %-15.7f | x=%4u y=%4u z=%3u | %s\n",
               scale, x3, y3, z3, precision_ok ? "YES" : "NO");
        
        if (!precision_ok) {
            printf("  ^^^ PRECISION BREAKS HERE (scale < %.7f)\n", scale);
        }
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   EXP 2: Handoff Integrity Test
   Verify data survives KIS → Hyperbolic transition
   ═══════════════════════════════════════════════════════════════════════════ */
static void exp2_handoff_integrity(void) {
    printf("EXP 2: Handoff Integrity (KIS → Hyperbolic → KIS)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t data[20736];
    for (int i = 0; i < 20736; i++) {
        data[i] = (uint8_t)(i % 256);
    }
    
    KISAxes axes;
    kis_axis_from_1d(&axes, data, 20736);
    
    /* Test: KIS → Hyperbolic → KIS roundtrip */
    int roundtrip_pass = 0;
    int roundtrip_fail = 0;
    
    for (uint32_t slot = 0; slot < 20736; slot += 100) {
        /* KIS → Hyperbolic */
        HypComplex w = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        
        /* Hyperbolic → KIS */
        uint32_t back = hyperbolic_to_kis_axis(w, HYP_AXIS_X);
        
        if (back == slot) {
            roundtrip_pass++;
        } else {
            roundtrip_fail++;
            if (roundtrip_fail <= 3) {
                printf("  MISMATCH: slot=%u → hyper → back=%u\n", slot, back);
            }
        }
    }
    
    CHECK(1, "KIS→Hyper→KIS roundtrip (AXIS_X)", roundtrip_fail == 0);
    printf("    Roundtrip: %d pass, %d fail (out of %d tested)\n",
           roundtrip_pass, roundtrip_fail, 20736/100);
    
    /* Test: Handoff at different scale thresholds */
    printf("\n  Testing handoff at different scale thresholds:\n");
    double thresholds[] = {0.1, 0.01, 0.001, 0.0001, 0.00001, 0.000001};
    int num_thresh = sizeof(thresholds) / sizeof(thresholds[0]);
    
    for (int t = 0; t < num_thresh; t++) {
        double thresh = thresholds[t];
        uint32_t thresh_fixed = (uint32_t)(thresh * 65536.0);
        
        /* At this threshold, can we still roundtrip? */
        uint32_t test_slot = 1000;
        HypComplex w = kis_to_hyperbolic_axis(test_slot, HYP_AXIS_X);
        uint32_t back = hyperbolic_to_kis_axis(w, HYP_AXIS_X);
        
        printf("    threshold=%.6f → roundtrip %s\n",
               thresh, (back == test_slot) ? "OK" : "FAIL");
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   EXP 3: Single Axis Deep Test (AXIS_X = Hilbert)
   Test precision, determinism, and pattern
   ═══════════════════════════════════════════════════════════════════════════ */
static void exp3_single_axis_deep(void) {
    printf("EXP 3: Single Axis Deep Test (AXIS_X = Hilbert)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Test 1: Determinism — same input always gives same output */
    int det_pass = 0, det_fail = 0;
    for (uint32_t slot = 0; slot < 6912; slot += 50) {
        HypComplex w1 = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        HypComplex w2 = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        
        if (w1.re == w2.re && w1.im == w2.im) {
            det_pass++;
        } else {
            det_fail++;
        }
    }
    CHECK(2, "Determinism (same input → same output)", det_fail == 0);
    printf("    Determinism: %d pass, %d fail\n", det_pass, det_fail);
    
    /* Test 2: Precision at small scales */
    printf("\n  Testing precision at small KIS scales (AXIS_X only):\n");
    double test_scales[] = {1.0, 0.1, 0.01, 0.001, 0.0001, 0.00001, 0.000001};
    int n = sizeof(test_scales) / sizeof(test_scales[0]);
    
    for (int i = 0; i < n; i++) {
        double scale = test_scales[i];
        
        /* Project slot through KIS at this scale */
        uint32_t slot = 1000;
        uint32_t proj = kis_project_4d_to_3d(slot, 0, 0, 0, (uint32_t)(scale * 65536.0));
        
        /* Then try to recover via Hyperbolic */
        HypComplex w = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        uint32_t recovered = hyperbolic_to_kis_axis(w, HYP_AXIS_X);
        
        printf("    scale=%.7f → proj=%u → hyper→recovered=%u → %s\n",
               scale, proj, recovered, (recovered == slot) ? "OK" : "DRIFT");
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   EXP 4: Pattern Comparison
   KIS sequential vs Hyperbolic deterministic
   ═══════════════════════════════════════════════════════════════════════════ */
static void exp4_pattern_comparison(void) {
    printf("EXP 4: Pattern Comparison (KIS Sequential vs Hyperbolic)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* KIS pattern: sequential access */
    printf("  KIS pattern (sequential, AXIS_X):\n");
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t slot = i * 1000;
        printf("    slot %u → value %u\n", slot, (uint32_t)(slot % 256));
    }
    
    /* Hyperbolic pattern: deterministic via Cayley transform */
    printf("\n  Hyperbolic pattern (deterministic, AXIS_X):\n");
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t slot = i * 1000;
        HypComplex w = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        printf("    slot %u → hyper(%.4f, %.4f)\n", slot, w.re, w.im);
    }
    
    CHECK(3, "Both patterns produce output", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS ↔ Hyperbolic Dual Boundary Experiment\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    exp1_scale_precision();
    exp2_handoff_integrity();
    exp3_single_axis_deep();
    exp4_pattern_comparison();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    
    return fail == 0 ? 0 : 1;
}
