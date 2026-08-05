/* ═══════════════════════════════════════════════════════════════════════════
 * test_cell_classify.c — Tests for geo_cell_classify.h
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "../core/geo_cell_classify.h"
#include <stdlib.h>

int main(void) {
    int all_pass = 1;

    /* ── Test 1: Verify classify for generations 0-5 ─────────────── */
    printf("\n=== Test 1: verify_cell_classify (gen 0-5) ===\n");
    if (!verify_cell_classify(5)) all_pass = 0;

    /* ── Test 2: Spot-check specific classifications ──────────────── */
    printf("\n=== Test 2: Spot-check classifications ===\n");
    {
        int spot_ok = 1;
        struct { uint8_t g, f; uint16_t s; uint8_t expect; } cases[] = {
            {0, 0, 0, 0},  /* III */
            {0, 1, 0, 0},  /* III (gen even, face odd→ bit1=1? no: face=1&1=1, slot=0&1=0 → 010=2) */
            {1, 0, 0, 4},  /* DII */
            {1, 1, 1, 7},  /* DDD */
            {2, 0, 1, 1},  /* IID */
            {3, 3, 5, 7},  /* DDD */
        };
        /* Recalculate expected for test case index 1: gen=0(f0), face=1(f1), slot=0(f0) → (0<<2)|(1<<1)|0 = 2 = IDI */
        cases[1].expect = 2;  /* IDI */

        for (int i = 0; i < 6; i++) {
            GeoCubeAddr addr = geo_cube_addr(cases[i].g, cases[i].f, cases[i].s);
            uint8_t ct = geo_cell_classify(addr);
            const char *name = geo_cell_classify_name(ct);
            if (ct != cases[i].expect) {
                printf("  FAIL: gen=%u face=%u slot=%u → %s (%u), expected %u\n",
                       cases[i].g, cases[i].f, cases[i].s, name, ct, cases[i].expect);
                spot_ok = 0;
            } else {
                printf("  PASS: gen=%u face=%u slot=%u → %s (%u)\n",
                       cases[i].g, cases[i].f, cases[i].s, name, ct);
            }
        }
        if (!spot_ok) all_pass = 0;
    }

    /* ── Test 3: Generate 1000 fake weights, classify, histogram ──── */
    printf("\n=== Test 3: 1000 random weight classification histogram ===\n");
    {
        float weights[1000];
        for (int i = 0; i < 1000; i++) {
            weights[i] = (float)rand() / (float)RAND_MAX;
        }
        geo_cell_classify_stats(weights, 1000, 5);
    }

    /* ── Test 4: All 8 cell types are reachable ──────────────────── */
    printf("\n=== Test 4: All 8 cell types reachable ===\n");
    {
        uint8_t seen[8] = {0};
        uint32_t max_gen = 7;
        for (uint32_t g = 0; g <= max_gen; g++) {
            uint16_t spf = slots_per_face(g);
            for (uint8_t f = 0; f < CUBE_ADDR_FACES; f++) {
                for (uint16_t s = 0; s < spf; s++) {
                    GeoCubeAddr addr = geo_cube_addr(g, f, s);
                    uint8_t ct = geo_cell_classify(addr);
                    seen[ct] = 1;
                }
            }
        }
        int all_seen = 1;
        for (uint8_t ct = 0; ct < 8; ct++) {
            if (!seen[ct]) {
                printf("  FAIL: cell type %s (%u) not reachable\n",
                       geo_cell_classify_name(ct), ct);
                all_seen = 0;
            }
        }
        if (all_seen) {
            printf("  PASS: all 8 cell types reachable\n");
        } else {
            all_pass = 0;
        }
    }

    /* ── Summary ──────────────────────────────────────────────────── */
    printf("\n===============================================================\n");
    if (all_pass) {
        printf("  ALL TESTS PASSED\n");
    } else {
        printf("  SOME TESTS FAILED\n");
    }
    printf("===============================================================\n");

    return all_pass ? 0 : 1;
}
