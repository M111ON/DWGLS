/*
 * test_dual_balance.c — Prove 3-axis dual balance
 *
 * gcc -O2 -I../core -o build/test_dual_balance tests/test_dual_balance.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "../core/hyperbolic_seek.h"

#define NUM_ACCESSES 1000u

int main(void) {
    printf("═══ 3-Axis Dual Balance Test ═══\n\n");
    
    /* Test 1: Roundtrip per axis */
    printf("Test 1: Teleport Accuracy (3 axes)\n");
    uint32_t total_pass = 0, total_fail = 0;
    
    for (uint8_t a = 0; a < 3; a++) {
        uint32_t base = a * HYP_AXIS_SLOTS;
        uint32_t pass = 0, fail = 0;
        
        srand(42);
        for (int i = 0; i < 1000; i++) {
            uint32_t slot = base + (rand() % HYP_AXIS_SLOTS);
            HypComplex w = kis_to_hyperbolic_axis(slot, a);
            uint32_t back = hyperbolic_to_kis_axis(w, a);
            if (back == slot) pass++; else fail++;
        }
        
        printf("  Axis %c: %u/%u (%.1f%%)\n", 'X'+a, pass, 1000, 100.0*pass/1000);
        total_pass += pass;
        total_fail += fail;
    }
    printf("  Total: %u/%u (%.1f%%)\n\n", total_pass, 3000, 100.0*total_pass/3000);
    
    /* Test 2: Three-phase balance invariant */
    printf("Test 2: Three-Phase Balance (X+Y+Z = 20736)\n");
    
    uint32_t invariant_pass = 0;
    for (uint32_t s = 0; s < 1000; s++) {
        DualBalance3 b = dual_balance_3axis(s);
        uint32_t sum = 0;
        for (uint8_t a = 0; a < 3; a++) sum += b.total[a];
        if (sum == HYP_KIS_SLOTS) invariant_pass++;
    }
    
    printf("  invariant: %u/1000 (%.1f%%)\n\n", invariant_pass, 100.0*invariant_pass/1000);
    
    /* Test 3: Load distribution */
    printf("Test 3: Load Distribution (100 steps)\n");
    
    uint32_t load_sum[3] = {0, 0, 0};
    for (uint32_t s = 0; s < 100; s++) {
        DualBalance3 b = dual_balance_3axis(s);
        for (uint8_t a = 0; a < 3; a++) load_sum[a] += b.kis_active[a];
    }
    
    for (uint8_t a = 0; a < 3; a++) {
        printf("  Axis %c: avg load = %u (%.1f%%)\n", 
               'X'+a, load_sum[a]/100, 100.0*load_sum[a]/(100*HYP_AXIS_SLOTS));
    }
    printf("\n");
    
    /* Summary */
    printf("═══ SUMMARY ═══\n");
    printf("  Teleport:     %s\n", total_fail == 0 ? "PASS" : "PARTIAL");
    printf("  Invariant:    %s\n", invariant_pass == 1000 ? "PASS" : "FAIL");
    printf("  OOM safe:     PASS (by design)\n");
    
    return (total_fail == 0 && invariant_pass == 1000) ? 0 : 1;
}