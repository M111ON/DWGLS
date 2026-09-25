/* test_frustum_trit.c — frustum_trit.h verification */
#include <stdio.h>
#include <string.h>
#include "../core/frustum_trit.h"

int main(void)
{
    int pass = 0, fail = 0;

    /* T1: TRIT_MOD constant */
    if (TRIT_MOD == 27u) {
        printf("[PASS] T1: TRIT_MOD == 27\n"); pass++;
    } else {
        printf("[FAIL] T1: TRIT_MOD = %u (expected 27)\n", TRIT_MOD); fail++;
    }

    /* T2: GEAR_MESH = COSET_COUNT * FACE_COUNT */
    if (GEAR_MESH == 54u) {
        printf("[PASS] T2: GEAR_MESH == 54\n"); pass++;
    } else {
        printf("[FAIL] T2: GEAR_MESH = %u (expected 54)\n", GEAR_MESH); fail++;
    }

    /* T3: TritAddr decomposition for all 27 trits */
    {
        int t3_ok = 1;
        for (uint8_t t = 0u; t < TRIT_MOD; t++) {
            TritAddr a;
            trit_decompose(t, &a);
            if (a.trit != t || a.coset != t / 6 || a.face != t % 6 ||
                a.level != t % 4 || a.letter != t % 26) {
                printf("[FAIL] T3: trit %u decomposition mismatch\n", t); fail++; t3_ok = 0; break;
            }
        }
        if (t3_ok) { printf("[PASS] T3: All 27 trits decompose correctly\n"); pass++; }
    }

    /* T4: TritAddr recomposition */
    {
        int t4_ok = 1;
        for (uint8_t t = 0u; t < TRIT_MOD; t++) {
            TritAddr a;
            trit_decompose(t, &a);
            uint8_t t2 = trit_compose(&a);
            if (t2 != t) {
                printf("[FAIL] T4: trit %u recompose = %u\n", t, t2); fail++; t4_ok = 0; break;
            }
        }
        if (t4_ok) { printf("[PASS] T4: All 27 trits recompose losslessly\n"); pass++; }
    }

    /* T5: Slope fingerprint */
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    uint32_t addr = 0x12345678u;
    uint64_t slope = trit_slope(seed, addr);
    if (slope == (seed ^ (uint64_t)addr)) {
        printf("[PASS] T5: slope = seed ^ addr\n"); pass++;
    } else {
        printf("[FAIL] T5: slope mismatch\n"); fail++;
    }

    printf("\n=== frustum_trit: %d PASS, %d FAIL ===\n", pass, fail);
    return fail ? 1 : 0;
}