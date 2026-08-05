/*
 * test_dual_tiling.c — Tests for geo_dual_tiling.h
 *
 * Generates synthetic weights (mix of sparse and dense),
 * splits them into World A / World B, verifies counts,
 * and checks sparsity properties.
 *
 * Compile: gcc -O2 -Wall -o test_dual_tiling test_dual_tiling.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

/* Include the header under test */
#include "../core/geo_dual_tiling.h"

/* Simple deterministic pseudo-random (xorshift32) */
static uint32_t xor_state = 12345;
static float rand_float(void) {
    xor_state ^= xor_state << 13;
    xor_state ^= xor_state >> 17;
    xor_state ^= xor_state << 5;
    return (float)(xor_state & 0xFFFF) / 65535.0f;  /* [0, 1] */
}

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name, expr) do {                                   \
    tests_run++;                                                \
    if (expr) {                                                 \
        tests_passed++;                                         \
        printf("  [PASS] %s\n", name);                          \
    } else {                                                    \
        printf("  [FAIL] %s\n", name);                          \
    }                                                           \
} while(0)

/* ══════════════════════════════════════════════════════════════
   Test 1: Split counts are exactly 28 and 36
   ══════════════════════════════════════════════════════════════ */
static void test_split_counts(void) {
    printf("\n--- Test: Split Counts ---\n");

    float weights[GDP_GRID];
    for (uint32_t i = 0; i < GDP_GRID; i++) weights[i] = rand_float();

    DualTileResult r = geo_dual_tile_split(weights, GDP_GRID);

    TEST("World A count == 28", r.world_a_count == GDP_BORDER);
    TEST("World B count == 36", r.world_b_count == GDP_INNER);
    TEST("28 + 36 == 64", r.world_a_count + r.world_b_count == GDP_GRID);
}

/* ══════════════════════════════════════════════════════════════
   Test 2: All 64 indices covered, no duplicates
   ══════════════════════════════════════════════════════════════ */
static void test_no_overlap(void) {
    printf("\n--- Test: No Overlap ---\n");

    float weights[GDP_GRID];
    for (uint32_t i = 0; i < GDP_GRID; i++) weights[i] = rand_float();

    DualTileResult r = geo_dual_tile_split(weights, GDP_GRID);

    uint8_t seen[GDP_GRID] = {0};
    int ok = 1;
    for (uint32_t i = 0; i < r.world_a_count; i++) {
        uint32_t idx = r.world_a_indices[i];
        if (idx >= GDP_GRID || seen[idx]) { ok = 0; break; }
        seen[idx] = 1;
    }
    for (uint32_t i = 0; i < r.world_b_count; i++) {
        uint32_t idx = r.world_b_indices[i];
        if (idx >= GDP_GRID || seen[idx]) { ok = 0; break; }
        seen[idx] = 1;
    }
    int all_covered = 1;
    for (uint32_t i = 0; i < GDP_GRID; i++) {
        if (!seen[i]) { all_covered = 0; break; }
    }

    TEST("No duplicate indices", ok);
    TEST("All 64 cells covered", all_covered);
}

/* ══════════════════════════════════════════════════════════════
   Test 3: Border cells match GDP_BORDER_IDX exactly
   ══════════════════════════════════════════════════════════════ */
static void test_border_indices_match(void) {
    printf("\n--- Test: Border Indices Match GDP_BORDER_IDX ---\n");

    float weights[GDP_GRID];
    for (uint32_t i = 0; i < GDP_GRID; i++) weights[i] = 1.0f;

    DualTileResult r = geo_dual_tile_split(weights, GDP_GRID);

    /* Sort both arrays for comparison */
    uint32_t a_sorted[GDP_BORDER];
    for (uint32_t i = 0; i < r.world_a_count; i++) a_sorted[i] = r.world_a_indices[i];
    for (uint32_t i = 0; i < r.world_a_count - 1; i++)
        for (uint32_t j = i + 1; j < r.world_a_count; j++)
            if (a_sorted[i] > a_sorted[j]) {
                uint32_t tmp = a_sorted[i]; a_sorted[i] = a_sorted[j]; a_sorted[j] = tmp;
            }

    uint8_t expected_sorted[GDP_BORDER];
    for (uint32_t i = 0; i < GDP_BORDER; i++) expected_sorted[i] = GDP_BORDER_IDX[i];
    for (uint32_t i = 0; i < GDP_BORDER - 1; i++)
        for (uint32_t j = i + 1; j < GDP_BORDER; j++)
            if (expected_sorted[i] > expected_sorted[j]) {
                uint8_t tmp = expected_sorted[i]; expected_sorted[i] = expected_sorted[j]; expected_sorted[j] = tmp;
            }

    int match = 1;
    for (uint32_t i = 0; i < GDP_BORDER; i++) {
        if (a_sorted[i] != (uint32_t)expected_sorted[i]) { match = 0; break; }
    }

    TEST("Border indices match GDP_BORDER_IDX", match);
}

/* ══════════════════════════════════════════════════════════════
   Test 4: Sparsity — border sparser when border is zeroed
   ══════════════════════════════════════════════════════════════ */
static void test_sparsity_border_sparser(void) {
    printf("\n--- Test: Border Sparser When Zeroed ---\n");

    float weights[GDP_GRID];
    /* Fill all with dense values */
    for (uint32_t i = 0; i < GDP_GRID; i++) weights[i] = 0.5f;

    /* Zero out all border cells */
    for (uint32_t i = 0; i < GDP_BORDER; i++) {
        weights[GDP_BORDER_IDX[i]] = 0.0f;
    }

    float sparsity[2];
    geo_dual_tile_sparsity(weights, GDP_GRID, sparsity);

    /* Border should be 100% sparse, inner should be 0% */
    TEST("Border sparsity == 100%", fabsf(sparsity[0] - 1.0f) < 1e-6f);
    TEST("Inner sparsity == 0%",    fabsf(sparsity[1] - 0.0f) < 1e-6f);
    TEST("Border sparser than inner", sparsity[0] > sparsity[1]);
}

/* ══════════════════════════════════════════════════════════════
   Test 5: Sparsity — inner sparser when inner is zeroed
   ══════════════════════════════════════════════════════════════ */
static void test_sparsity_inner_sparser(void) {
    printf("\n--- Test: Inner Sparser When Zeroed ---\n");

    float weights[GDP_GRID];
    for (uint32_t i = 0; i < GDP_GRID; i++) weights[i] = 0.5f;

    /* Zero out inner cells (non-border) */
    for (uint32_t i = 0; i < GDP_GRID; i++) {
        uint8_t is_border = 0;
        for (uint32_t b = 0; b < GDP_BORDER; b++) {
            if (GDP_BORDER_IDX[b] == i) { is_border = 1; break; }
        }
        if (!is_border) weights[i] = 0.0f;
    }

    float sparsity[2];
    geo_dual_tile_sparsity(weights, GDP_GRID, sparsity);

    TEST("Inner sparsity == 100%", fabsf(sparsity[1] - 1.0f) < 1e-6f);
    TEST("Border sparsity == 0%",  fabsf(sparsity[0] - 0.0f) < 1e-6f);
}

/* ══════════════════════════════════════════════════════════════
   Test 6: Sum correctness
   ══════════════════════════════════════════════════════════════ */
static void test_sum_correctness(void) {
    printf("\n--- Test: Sum Correctness ---\n");

    float weights[GDP_GRID];
    for (uint32_t i = 0; i < GDP_GRID; i++) weights[i] = 1.0f;

    DualTileResult r = geo_dual_tile_split(weights, GDP_GRID);

    TEST("World A sum == 28", fabsf(r.world_a_sum - 28.0f) < 1e-5f);
    TEST("World B sum == 36", fabsf(r.world_b_sum - 36.0f) < 1e-5f);
}

/* ══════════════════════════════════════════════════════════════
   Test 7: Edge case — n_weights < 64
   ══════════════════════════════════════════════════════════════ */
static void test_partial_weights(void) {
    printf("\n--- Test: Partial Weights (n=32) ---\n");

    float weights[32];
    for (uint32_t i = 0; i < 32; i++) weights[i] = 1.0f;

    DualTileResult r = geo_dual_tile_split(weights, 32);

    TEST("Count <= 32", r.world_a_count + r.world_b_count <= 32);
    TEST("No overflow", r.world_a_count <= GDP_BORDER && r.world_b_count <= GDP_INNER);
}

/* ══════════════════════════════════════════════════════════════
   Test 8: Demo output (visual verification)
   ══════════════════════════════════════════════════════════════ */
static void test_demo_output(void) {
    printf("\n--- Test: Demo Output ---\n");

    float weights[GDP_GRID];
    for (uint32_t i = 0; i < GDP_GRID; i++) {
        if (i % 4 == 0) weights[i] = 0.0f;   /* 25% sparse */
        else             weights[i] = rand_float();
    }

    printf("\n");
    geo_dual_tile_demo(weights, GDP_GRID);
    TEST("Demo ran without crash", 1);
}

/* ══════════════════════════════════════════════════════════════
   main
   ══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("===============================================================\n");
    printf("  test_dual_tiling.c — geo_dual_tiling.h tests\n");
    printf("===============================================================\n");

    test_split_counts();
    test_no_overlap();
    test_border_indices_match();
    test_sparsity_border_sparser();
    test_sparsity_inner_sparser();
    test_sum_correctness();
    test_partial_weights();
    test_demo_output();

    printf("\n===============================================================\n");
    printf("  Results: %d / %d passed\n", tests_passed, tests_run);
    printf("===============================================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
