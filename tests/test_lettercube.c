/* test_lettercube.c — Test LetterCube Chain addressing.
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_lettercube tests/test_lettercube.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tess_resolver.h"
#include "tess_lettercube.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== LetterCube Chain Test ===\n\n");

    /* Test 1: Init (onion mode) */
    printf("Test 1: Init (onion mode)\n");
    {
        LCLetterCube lc;
        lc_init(&lc, 2);  /* onion mode */
        CHECK(lc.n_cubes == 1, "starts with 1 cube");
        CHECK(lc.mode == 2, "mode = onion");
        /* 8 mini-cubes at octant positions */
        for (int i = 0; i < LC_MINI; i++) {
            /* each pos is ±1 */
            if (lc.cubes[i].pos[0] == 0 || lc.cubes[i].pos[1] == 0 || lc.cubes[i].pos[2] == 0) {
                printf("  FAIL: mini %d has zero pos\n", i);
                fail++;
                goto done;
            }
        }
        pass++;
        printf("  PASS: 8 mini-cubes at octant positions\n");
    }

    /* Test 2: Weight → letter pair */
    printf("\nTest 2: Weight -> letter pair\n");
    {
        int ok = 1;
        for (int w = -127; w <= 127; w++) {
            uint8_t pair = lc_weight_to_pair((int8_t)w);
            if (pair >= LC_PAIRS) { ok = 0; break; }
        }
        CHECK(ok, "all weights -> valid letter pair (0..23)");
    }

    /* Test 3: Letter pair → slot */
    printf("\nTest 3: Letter pair -> slot (20736)\n");
    {
        int ok = 1;
        for (uint8_t p = 0; p < LC_PAIRS; p++) {
            for (uint32_t l = 0; l < 36; l++) {
                uint32_t slot = lc_pair_to_slot(p, l);
                if (slot >= TESS_TOTAL_SLOTS) { ok = 0; break; }
            }
            if (!ok) break;
        }
        CHECK(ok, "all (pair, layer) -> valid slot");
        /* 24 * 864 = 20736 */
        CHECK(LC_PAIRS * 864 == TESS_TOTAL_SLOTS, "24 * 864 = 20736");
    }

    /* Test 4: Weight → slot (via letter pair) */
    printf("\nTest 4: Weight -> slot (via letter pair)\n");
    {
        uint32_t slot = lc_weight_to_slot(42, 0);
        CHECK(slot < TESS_TOTAL_SLOTS, "weight=42, layer=0 -> valid slot");
        printf("  INFO: weight=42 -> slot=%u\n", slot);
    }

    /* Test 5: Link check (row mode) */
    printf("\nTest 5: Link check (row mode = always linked)\n");
    {
        LCLetterCube lc;
        lc_init(&lc, 0);  /* row mode */
        int linked = lc_link_check(&lc, 0, 1, 0);
        CHECK(linked == 1, "row mode: always linked");
    }

    /* Test 6: Link check (onion mode) */
    printf("\nTest 6: Link check (onion mode = always linked)\n");
    {
        LCLetterCube lc;
        lc_init(&lc, 2);  /* onion mode */
        int linked = lc_link_check(&lc, 0, 1, 0);
        CHECK(linked == 1, "onion mode: always linked");
    }

    /* Test 7: Onion layer */
    printf("\nTest 7: Onion layer (nested)\n");
    {
        int layer = lc_onion_layer(0, 36);
        CHECK(layer >= 0 && layer < 36, "layer in range [0,35]");
    }

    /* Test 8: Verify (all checks) */
    printf("\nTest 8: lc_verify (all checks)\n");
    {
        int rc = lc_verify();
        CHECK(rc == 0, "lc_verify() = 0 (all pass)");
    }

    /* Test 9: Letter pool (A-Z, a-z = 52) */
    printf("\nTest 9: Letter pool (52 letters)\n");
    {
        CHECK(LC_LETTERS == 52, "52 letters (A-Z + a-z)");
        CHECK(LC_PAIRS == 24, "24 letter pairs");
        CHECK(LC_MINI == 8, "8 mini-cubes");
        CHECK(LC_AXES == 3, "3 axes");
    }

    /* Test 10: Integration with resolver */
    printf("\nTest 10: Integration with tess_resolver\n");
    {
        uint32_t slot = lc_weight_to_slot(42, 5);
        uint32_t cell = tess_slot_to_cell(slot);
        CHECK(slot < TESS_TOTAL_SLOTS, "slot in range");
        CHECK(cell < TESS_TOTAL_SLOTS, "cell in range (stride-37)");
        printf("  INFO: weight=42, layer=5 -> slot=%u -> cell=%u\n", slot, cell);
    }

    done:
    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
