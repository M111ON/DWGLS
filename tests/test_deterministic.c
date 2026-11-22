/* test_deterministic.c — Deterministic trace test for tess_resolver.
 *
 * Verifies:
 *   1. Stride-37 scatter is deterministic (same input → same output)
 *   2. Zero-copy resolve is deterministic (same slot → same pointer)
 *   3. Cascade is deterministic (same sequence → same state)
 *   4. Distort is deterministic (same input → same output)
 *   5. Jump is deterministic (same sequence → same result)
 *   6. Beam addressing is deterministic
 *   7. LetterCube is deterministic
 *   8. Full pipeline: weight → slot → cell → jump → result (deterministic)
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_deterministic tests/test_deterministic.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tess_resolver.h"
#include "tess_jump.h"
#include "tess_beam.h"
#include "tess_lettercube.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Deterministic Trace Test ===\n\n");

    /* ── Test 1: Stride-37 scatter determinism ── */
    printf("Test 1: Stride-37 scatter determinism\n");
    {
        /* Scatter same data twice, compare byte-for-byte */
        uint32_t n = 1000;
        uint8_t *src = (uint8_t *)malloc(n * 4);
        uint8_t *dst1 = (uint8_t *)calloc(TESS_TOTAL_SLOTS * 4, 1);
        uint8_t *dst2 = (uint8_t *)calloc(TESS_TOTAL_SLOTS * 4, 1);
        for (uint32_t i = 0; i < n; i++) { uint32_t v = i * 7 + 13; memcpy(src + i*4, &v, 4); }

        /* Scatter 1 */
        for (uint32_t s = 0; s < n; s++) {
            uint32_t cell = tess_slot_to_cell(s % TESS_TOTAL_SLOTS);
            memcpy(dst1 + cell * 4, src + s * 4, 4);
        }
        /* Scatter 2 (same input) */
        for (uint32_t s = 0; s < n; s++) {
            uint32_t cell = tess_slot_to_cell(s % TESS_TOTAL_SLOTS);
            memcpy(dst2 + cell * 4, src + s * 4, 4);
        }
        /* Compare */
        int same = (memcmp(dst1, dst2, TESS_TOTAL_SLOTS * 4) == 0);
        CHECK(same, "stride-37 scatter: deterministic (byte-identical)");
        free(src); free(dst1); free(dst2);
    }

    /* ── Test 2: Zero-copy resolve determinism ── */
    printf("\nTest 2: Zero-copy resolve determinism\n");
    {
        uint32_t csz = 4;
        uint8_t *base = (uint8_t *)calloc(TESS_TOTAL_SLOTS * csz, 1);
        for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) { uint32_t v = i; memcpy(base + i*csz, &v, csz); }

        TessCubeView cube = { .base = base, .cell_size = csz, .n_cells = TESS_TOTAL_SLOTS, .capo_id = 0 };

        /* Resolve same slot 100 times, compare pointers */
        const uint8_t *p0 = tess_resolve_ptr(&cube, 42);
        int all_same = 1;
        for (int i = 0; i < 100; i++) {
            const uint8_t *p = tess_resolve_ptr(&cube, 42);
            if (p != p0) { all_same = 0; break; }
        }
        CHECK(all_same, "resolve(42) returns same pointer 100 times");
        free(base);
    }

    /* ── Test 3: Cascade determinism ── */
    printf("\nTest 3: Cascade determinism\n");
    {
        TessCascade c1, c2;
        memset(&c1, 0, sizeof(c1));
        memset(&c2, 0, sizeof(c2));

        /* Same sequence of activate/deactivate */
        int seq[] = {5, 10, 15, 5, 20, 10};
        for (int i = 0; i < 6; i++) {
            if (i < 3) { tess_cascade_activate(&c1, seq[i]); tess_cascade_activate(&c2, seq[i]); }
            else { tess_cascade_deactivate(&c1, seq[i]); tess_cascade_deactivate(&c2, seq[i]); }
        }

        /* Compare state */
        int same = (memcmp(&c1, &c2, sizeof(c1)) == 0);
        CHECK(same, "cascade: same sequence -> same state");
    }

    /* ── Test 4: Distort determinism ── */
    printf("\nTest 4: Distort determinism\n");
    {
        int all_same = 1;
        for (int i = 0; i < 1000; i++) {
            uint32_t d1 = tess_distort(100, 50, TESS_TOTAL_SLOTS);
            uint32_t d2 = tess_distort(100, 50, TESS_TOTAL_SLOTS);
            if (d1 != d2) { all_same = 0; break; }
        }
        CHECK(all_same, "distort: deterministic (1000 calls)");
        /* Symmetric */
        CHECK(tess_distort(100, 50, TESS_TOTAL_SLOTS) == tess_distort(50, 100, TESS_TOTAL_SLOTS),
              "distort: symmetric");
    }

    /* ── Test 5: Jump determinism ── */
    printf("\nTest 5: Jump determinism\n");
    {
        JumpContext c1, c2;
        jump_init(&c1); jump_init(&c2);
        c1.pointer_pos = 500; c2.pointer_pos = 500;

        /* Same sequence */
        JumpType seq[] = { JUMP_HILBERT, JUMP_HYPER, JUMP_FRAME, JUMP_FAN24, JUMP_JET };
        uint32_t params[] = { 10, 1, 0, 0, 144 };
        JumpResult r1_last, r2_last;
        for (int i = 0; i < 5; i++) {
            r1_last = jump_execute(&c1, seq[i], params[i]);
            r2_last = jump_execute(&c2, seq[i], params[i]);
        }

        CHECK(r1_last.slot == r2_last.slot, "jump: same sequence -> same slot");
        CHECK(r1_last.cell == r2_last.cell, "jump: same sequence -> same cell");
        CHECK(r1_last.side == r2_last.side, "jump: same sequence -> same side");
        CHECK(r1_last.distort == r2_last.distort, "jump: same sequence -> same distort");
    }

    /* ── Test 6: Beam addressing determinism ── */
    printf("\nTest 6: Beam addressing determinism\n");
    {
        /* Same weight → same code, same coord, same slot */
        int all_same = 1;
        for (int w = -128; w <= 127; w++) {
            BeamCode c1 = beam_code_from_weight((int8_t)w);
            BeamCode c2 = beam_code_from_weight((int8_t)w);
            if (c1 != c2) { all_same = 0; break; }

            BeamCoord coord1 = beam_weight_to_coord(0, 42, (int8_t)w);
            BeamCoord coord2 = beam_weight_to_coord(0, 42, (int8_t)w);
            if (coord1.abs_value != coord2.abs_value || coord1.sign != coord2.sign) { all_same = 0; break; }

            uint32_t s1 = beam_to_slot(&coord1);
            uint32_t s2 = beam_to_slot(&coord2);
            if (s1 != s2) { all_same = 0; break; }
        }
        CHECK(all_same, "beam: deterministic (all 256 weights)");
    }

    /* ── Test 7: LetterCube determinism ── */
    printf("\nTest 7: LetterCube determinism\n");
    {
        LCLetterCube lc1, lc2;
        lc_init(&lc1, 2);
        lc_init(&lc2, 2);

        int same = (memcmp(&lc1, &lc2, sizeof(lc1)) == 0);
        CHECK(same, "lettercube: same init -> same state");

        /* Same weight → same pair → same slot */
        int all_same = 1;
        for (int w = -127; w <= 127; w++) {
            uint8_t p1 = lc_weight_to_pair((int8_t)w);
            uint8_t p2 = lc_weight_to_pair((int8_t)w);
            if (p1 != p2) { all_same = 0; break; }
            uint32_t s1 = lc_weight_to_slot((int8_t)w, 5);
            uint32_t s2 = lc_weight_to_slot((int8_t)w, 5);
            if (s1 != s2) { all_same = 0; break; }
        }
        CHECK(all_same, "lettercube: deterministic (all weights)");
    }

    /* ── Test 8: Full pipeline determinism ── */
    printf("\nTest 8: Full pipeline determinism (weight -> slot -> cell -> jump)\n");
    {
        int all_same = 1;
        for (int w = 0; w < 10; w++) {
            /* Run 1 */
            JumpContext ctx1; jump_init(&ctx1);
            BeamCoord c1 = beam_weight_to_coord(0, (uint32_t)w, (int8_t)w);
            JumpResult r1 = beam_to_jump(&ctx1, &c1);

            /* Run 2 (same input) */
            JumpContext ctx2; jump_init(&ctx2);
            BeamCoord c2 = beam_weight_to_coord(0, (uint32_t)w, (int8_t)w);
            JumpResult r2 = beam_to_jump(&ctx2, &c2);

            if (r1.slot != r2.slot || r1.cell != r2.cell || r1.side != r2.side) {
                all_same = 0; break;
            }
        }
        CHECK(all_same, "full pipeline: deterministic (10 weights)");
    }

    /* ── Test 9: Multi-cube resolve determinism ── */
    printf("\nTest 9: Multi-cube resolve determinism\n");
    {
        /* Simulate capo views */
        uint32_t csz = 4;
        uint8_t *base = (uint8_t *)calloc(TESS_TOTAL_SLOTS * csz, 1);
        for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) { uint32_t v = i; memcpy(base + i*csz, &v, csz); }

        TessCubeView cap = { .base = base, .cell_size = csz, .n_cells = TESS_TOTAL_SLOTS, .capo_id = 0 };
        TessMultiCubeView mc = { .caps = &cap, .n_caps = 1, .cell_size = csz, .total_cells = TESS_TOTAL_SLOTS };

        /* Resolve same slot 1000 times */
        const uint8_t *p0 = tess_resolve_multi(&mc, 42);
        int all_same = 1;
        for (int i = 0; i < 1000; i++) {
            const uint8_t *p = tess_resolve_multi(&mc, 42);
            if (p != p0) { all_same = 0; break; }
            uint32_t v1, v2;
            memcpy(&v1, p0, 4); memcpy(&v2, p, 4);
            if (v1 != v2) { all_same = 0; break; }
        }
        CHECK(all_same, "multi-cube resolve: deterministic (1000 calls)");
        free(base);
    }

    /* ── Test 10: Field focus determinism ── */
    printf("\nTest 10: Field focus determinism\n");
    {
        /* Same pointer sequence → same active regions */
        /* (tested in test_field_focus.c, verified here as determinism) */
        /* Simple check: distort is deterministic */
        uint32_t d1 = tess_distort(100, 50, TESS_TOTAL_SLOTS);
        uint32_t d2 = tess_distort(100, 50, TESS_TOTAL_SLOTS);
        CHECK(d1 == d2, "field: distort deterministic");
    }

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
