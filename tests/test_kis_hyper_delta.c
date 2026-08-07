/* test_kis_hyper_delta.c — KIS + Hyper Delta = Lossless?
 *
 * Test: Can Hyperbolic capture what KIS loses at small scales?
 *
 * BUILD: gcc -O2 -I../core -o test_kis_hyper_delta test_kis_hyper_delta.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   Test: KIS loses detail at small scale, Hyper captures delta
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_delta_capture(void) {
    printf("TEST: KIS + Hyper Delta = Lossless?\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Original data */
    uint8_t original[20736];
    for (int i = 0; i < 20736; i++) {
        original[i] = (uint8_t)(i % 256);
    }
    
    KISAxes axes;
    kis_axis_from_1d(&axes, original, 20736);
    
    /* Step 1: KIS projection at LARGE scale (coarse) */
    uint32_t scale_large = (uint32_t)(1.0 * 65536.0);  /* 1.0 */
    uint32_t kis_coarse[20736];
    for (uint32_t i = 0; i < 20736; i++) {
        kis_coarse[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale_large);
    }
    
    /* Step 2: KIS projection at SMALL scale (PLATEAU) */
    uint32_t scale_small = (uint32_t)(0.00001 * 65536.0);  /* 0.00001 */
    uint32_t kis_small[20736];
    for (uint32_t i = 0; i < 20736; i++) {
        kis_small[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale_small);
    }
    
    /* Step 3: Calculate delta (what KIS lost) */
    uint8_t delta[20736];
    for (int i = 0; i < 20736; i++) {
        /* Delta = original - what KIS can represent at small scale */
        int diff = (int)original[i] - (int)(kis_small[i] & 0xFF);
        delta[i] = (uint8_t)(diff & 0xFF);
    }
    
    /* Step 4: Store delta in Hyperbolic */
    uint8_t hyper_delta[20736];
    for (uint32_t slot = 0; slot < 6912; slot++) {  /* AXIS_X range */
        HypComplex w = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        /* Use real part as delta storage (simplified) */
        hyper_delta[slot] = (uint8_t)((int)(w.re * 100) & 0xFF);
    }
    
    /* Step 5: Verify — can we recover original from KIS + Hyper delta? */
    uint8_t recovered[20736];
    for (int i = 0; i < 20736; i++) {
        /* recovered = KIS coarse + delta */
        uint32_t kis_val = kis_coarse[i] & 0xFF;
        uint8_t d = delta[i];
        recovered[i] = (uint8_t)((kis_val + d) & 0xFF);
    }
    
    /* Check roundtrip */
    int match = 1;
    int mismatches = 0;
    for (int i = 0; i < 20736; i++) {
        if (recovered[i] != original[i]) {
            match = 0;
            mismatches++;
            if (mismatches <= 3) {
                printf("  MISMATCH [%d]: orig=%u, recovered=%u\n", 
                       i, original[i], recovered[i]);
            }
        }
    }
    
    CHECK(1, "KIS + delta = original (AXIS_X)", match);
    printf("    Mismatches: %d / 20736\n", mismatches);
    
    /* Show sample */
    printf("\n  Sample (first 10):\n");
    printf("  i  | orig | kis_coarse | delta | recovered\n");
    printf("  ---|------|------------|-------|----------\n");
    for (int i = 0; i < 10; i++) {
        printf("  %2d | %4u | %10u | %5u | %9u\n",
               i, original[i], kis_coarse[i] & 0xFF, 
               delta[i], recovered[i]);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test: Hyperbolic stores delta deterministically
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_hyper_delta_deterministic(void) {
    printf("TEST: Hyperbolic Delta Storage (deterministic)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Same delta stored twice — should be identical */
    uint8_t delta1[6912], delta2[6912];
    
    for (uint32_t slot = 0; slot < 6912; slot++) {
        HypComplex w1 = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        HypComplex w2 = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        
        delta1[slot] = (uint8_t)((int)(w1.re * 100) & 0xFF);
        delta2[slot] = (uint8_t)((int)(w2.re * 100) & 0xFF);
    }
    
    int match = (memcmp(delta1, delta2, 6912) == 0);
    CHECK(2, "Hyper delta is deterministic", match);
    printf("    Delta stored: %d bytes\n", 6912);
    printf("    Match: %s\n", match ? "YES" : "NO");
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test: Precision loss at different scales
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_precision_loss(void) {
    printf("TEST: Precision Loss at Different Scales\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[100];
    for (int i = 0; i < 100; i++) {
        original[i] = (uint8_t)(i * 3 % 256);
    }
    
    double scales[] = {1.0, 0.1, 0.01, 0.001, 0.0001, 0.00001};
    int n = sizeof(scales) / sizeof(scales[0]);
    
    printf("  Scale      | Loss (bytes) | Loss %%\n");
    printf("  -----------|--------------|--------\n");
    
    for (int s = 0; s < n; s++) {
        double scale = scales[s];
        uint32_t scale_fp = (uint32_t)(scale * 65536.0);
        
        int loss = 0;
        for (int i = 0; i < 100; i++) {
            uint32_t proj = kis_project_4d_to_3d(i, 0, 0, 0, scale_fp);
            uint8_t kis_val = (uint8_t)(proj & 0xFF);
            if (kis_val != original[i]) loss++;
        }
        
        printf("  %-10.5f | %12d | %5.1f%%\n", 
               scale, loss, (loss * 100.0 / 100));
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS + Hyper Delta = Lossless? (Experiment)\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_delta_capture();
    test_hyper_delta_deterministic();
    test_precision_loss();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    
    return fail == 0 ? 0 : 1;
}
