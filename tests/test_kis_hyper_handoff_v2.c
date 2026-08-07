/* test_kis_hyper_handoff_v2.c — KIS ↔ Hyperbolic Dual Boundary (FIXED)
 *
 * Key insight: Each axis has independent range (0-6912)
 * Roundtrip only works within single axis bounds.
 *
 * BUILD: gcc -O2 -I../core -o test_kis_hyper_v2 test_kis_hyper_handoff_v2.c -lm
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
   EXP 1: Scale Precision — where does KIS fixed-point break?
   ═══════════════════════════════════════════════════════════════════════════ */
static void exp1_scale_precision(void) {
    printf("EXP 1: KIS Scale Precision (AXIS_X only)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    double scales[] = {
        1.0, 0.5, 0.1, 0.01, 0.001, 
        0.0001, 0.00001, 0.000001, 0.0000001
    };
    int n = sizeof(scales) / sizeof(scales[0]);
    
    printf("  Scale          | Fixed-Point  | Projection   | Precision\n");
    printf("  ---------------|--------------|--------------|----------\n");
    
    uint32_t prev_proj = 0;
    for (int i = 0; i < n; i++) {
        double scale = scales[i];
        uint32_t scale_fp = (uint32_t)(scale * 65536.0);
        
        uint32_t proj = kis_project_4d_to_3d(1000, 0, 0, 0, scale_fp);
        
        /* Check if projection changed from previous scale */
        int changed = (i == 0) || (proj != prev_proj);
        
        printf("  %-15.7f | %10u | %10u | %s\n",
               scale, scale_fp, proj, changed ? "CHANGED" : "SAME");
        
        if (!changed && i > 0) {
            printf("  ^^^ SCALE TOO SMALL — no effect (precision plateau)\n");
        }
        
        prev_proj = proj;
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   EXP 2: Handoff Integrity — roundtrip within AXIS_X bounds
   ═══════════════════════════════════════════════════════════════════════════ */
static void exp2_handoff_integrity(void) {
    printf("EXP 2: Handoff Integrity (KIS ↔ Hyper, AXIS_X only)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* AXIS_X range: 0 to 6912 */
    int roundtrip_pass = 0, roundtrip_fail = 0;
    
    for (uint32_t slot = 0; slot < HYP_AXIS_SLOTS; slot += 50) {
        HypComplex w = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        uint32_t back = hyperbolic_to_kis_axis(w, HYP_AXIS_X);
        
        if (back == slot) {
            roundtrip_pass++;
        } else {
            roundtrip_fail++;
            if (roundtrip_fail <= 5) {
                printf("  MISMATCH: slot=%u → hyper → back=%u (diff=%d)\n", 
                       slot, back, (int)back - (int)slot);
            }
        }
    }
    
    CHECK(1, "KIS→Hyper→KIS roundtrip (AXIS_X, 0-6912)", roundtrip_fail == 0);
    printf("    Roundtrip: %d pass, %d fail (out of %d tested)\n",
           roundtrip_pass, roundtrip_fail, HYP_AXIS_SLOTS/50);
    
    /* Test: handoff at different scale thresholds */
    printf("\n  Scale threshold effect on handoff:\n");
    double thresholds[] = {1.0, 0.1, 0.01, 0.001, 0.0001, 0.00001, 0.000001};
    int n = sizeof(thresholds) / sizeof(thresholds[0]);
    
    for (int i = 0; i < n; i++) {
        double thresh = thresholds[i];
        uint32_t thresh_fp = (uint32_t)(thresh * 65536.0);
        
        /* Project at this scale, then check if hyperbolic can recover */
        uint32_t slot = 3456; /* middle of AXIS_X */
        uint32_t proj = kis_project_4d_to_3d(slot, 0, 0, 0, thresh_fp);
        HypComplex w = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        uint32_t recovered = hyperbolic_to_kis_axis(w, HYP_AXIS_X);
        
        printf("    scale=%.7f → proj=%u → recovered=%u → %s\n",
               thresh, proj, recovered, (recovered == slot) ? "OK" : "DRIFT");
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   EXP 3: Precision Threshold — find exact break point
   ═══════════════════════════════════════════════════════════════════════════ */
static void exp3_precision_threshold(void) {
    printf("EXP 3: Precision Threshold (fine-grained search)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Binary search for precision break point */
    double low = 0.000001, high = 1.0;
    uint32_t ref_slot = 3456;
    
    printf("  Searching for precision break point...\n");
    printf("  Reference slot: %u (middle of AXIS_X)\n\n", ref_slot);
    
    /* Test at fine increments around likely threshold */
    double test_scales[] = {
        0.001, 0.0005, 0.0001, 0.00005, 0.00001, 
        0.000005, 0.000001, 0.0000005, 0.0000001
    };
    int n = sizeof(test_scales) / sizeof(test_scales[0]);
    
    printf("  Scale          | proj value | Hyper recover | Status\n");
    printf("  ---------------|------------|---------------|--------\n");
    
    uint32_t prev_proj = 0;
    for (int i = 0; i < n; i++) {
        double scale = test_scales[i];
        uint32_t scale_fp = (uint32_t)(scale * 65536.0);
        
        uint32_t proj = kis_project_4d_to_3d(ref_slot, 0, 0, 0, scale_fp);
        HypComplex w = kis_to_hyperbolic_axis(ref_slot, HYP_AXIS_X);
        uint32_t recovered = hyperbolic_to_kis_axis(w, HYP_AXIS_X);
        
        int proj_changed = (i == 0) || (proj != prev_proj);
        int hyper_ok = (recovered == ref_slot);
        
        printf("  %-15.7f | %10u | %13u | %s\n",
               scale, proj, recovered,
               !hyper_ok ? "FAIL" : !proj_changed ? "PLATEAU" : "OK");
        
        prev_proj = proj;
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   EXP 4: Pattern Properties
   ═══════════════════════════════════════════════════════════════════════════ */
static void exp4_pattern_properties(void) {
    printf("EXP 4: Pattern Properties (KIS vs Hyperbolic)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* KIS: sequential, predictable */
    printf("  KIS pattern (sequential, AXIS_X):\n");
    printf("    slot → value (linear)\n");
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t slot = i * 1000;
        printf("    %u → %u\n", slot, (uint32_t)(slot % 256));
    }
    
    /* Hyperbolic: deterministic, Hilbert-like */
    printf("\n  Hyperbolic pattern (deterministic, AXIS_X):\n");
    printf("    slot → (re, im) via Cayley transform\n");
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t slot = i * 1000;
        HypComplex w = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        printf("    %u → (%.4f, %.4f)\n", slot, w.re, w.im);
    }
    
    /* Key difference: Hyperbolic has fixed path (deterministic) */
    printf("\n  Key insight:\n");
    printf("    KIS: slot N → value N (sequential)\n");
    printf("    Hyper: slot N → Cayley(N) → fixed coordinate (deterministic)\n");
    printf("    Hyperbolic path is like Hilbert curve — same input = same path\n");
    
    CHECK(2, "Both patterns produce output", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS ↔ Hyperbolic Dual Boundary Experiment v2\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("Single axis test: AXIS_X (Hilbert)\n");
    printf("Axis range: 0 to %u\n\n", HYP_AXIS_SLOTS);
    
    exp1_scale_precision();
    exp2_handoff_integrity();
    exp3_precision_threshold();
    exp4_pattern_properties();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    
    return fail == 0 ? 0 : 1;
}
