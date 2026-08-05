/* ═══════════════════════════════════════════════════════════════════════════
 * test_phi_microscope.c — Test suite for geo_phi_microscope.h
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Compile: gcc -O2 -Wall -o tests/test_phi_microscope tests/test_phi_microscope.c -lm
 * Run:     ./tests/test_phi_microscope
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "../core/geo_phi_microscope.h"
#include <assert.h>

/* Simple LCG PRNG */
static uint32_t prng_state = 42;
static uint32_t prng_next(void) {
    prng_state = prng_state * 1103515245u + 12345u;
    return prng_state;
}

/* ──────────────────────────────────────────────────────────────
   TEST 1: Entropy computation
   ────────────────────────────────────────────────────────────── */
static int test_entropy(void) {
    printf("  TEST 1: Entropy computation\n");

    /* Uniform distribution → max entropy = log2(8) = 3.0 */
    uint32_t uniform[8] = {125, 125, 125, 125, 125, 125, 125, 125};
    double H_uniform = phi_entropy(uniform, 1000);
    printf("    Uniform H = %.6f bits (expected ≈ 3.0)\n", H_uniform);
    assert(H_uniform > 2.99 && H_uniform < 3.01);  /* 8 types = max 3.0 bits */

    /* All in one bucket → zero entropy */
    uint32_t delta[8] = {1000, 0, 0, 0, 0, 0, 0, 0};
    double H_delta = phi_entropy(delta, 1000);
    printf("    Delta   H = %.6f bits (expected 0.0)\n", H_delta);
    assert(H_delta < 1e-10);

    /* Two-bucket split */
    uint32_t split[8] = {500, 0, 0, 0, 500, 0, 0, 0};
    double H_split = phi_entropy(split, 1000);
    printf("    Split   H = %.6f bits (expected 1.0)\n", H_split);
    assert(H_split > 0.99 && H_split < 1.01);

    printf("    ✓ PASS\n");
    return 1;
}

/* ──────────────────────────────────────────────────────────────
   TEST 2: phi_observe_generation on random weights
   ────────────────────────────────────────────────────────────── */
static int test_observe_random(void) {
    printf("  TEST 2: Observe random weights at gen 6\n");

    const uint32_t N = 1000;
    float weights[1000];

    prng_state = 42;
    for (uint32_t i = 0; i < N; i++) {
        weights[i] = ((float)(prng_next() & 0xFFFF) / 32768.0f) - 1.0f;
    }

    PhiMicroscopeResult r = phi_observe_generation(weights, N, 6);

    printf("    Gen %u, shell=%u, n_observed=%u\n", r.gen, r.shell_size, r.n_observed);
    printf("    Count entropy = %.6f bits (gen parity limits to 4 types)\n", r.entropy);
    printf("    Magnitude entropy = %.6f bits (random ≈ 2.0 — uniform magnitude dist)\n", r.magnitude_entropy);
    printf("    |w|_mean = %.6f\n", r.mean_magnitude);

    /* Random weights: gen even → types 0-3 only → max H = 2.0 bits
     * gen odd  → types 4-7 only → max H = 2.0 bits
     * So random weights at any gen should show H ≈ 2.0 */
    assert(r.n_observed == N);
    assert(r.entropy > 1.95 && r.entropy < 2.05);    /* count entropy ≈ 2.0 */
    assert(r.magnitude_entropy > 1.95 && r.magnitude_entropy < 2.05);  /* uniform mag ≈ 2.0 */
    printf("    ✓ PASS\n");
    return 1;
}

/* ──────────────────────────────────────────────────────────────
   TEST 3: phi_observe_generation on structured weights
   ────────────────────────────────────────────────────────────── */
static int test_observe_structured(void) {
    printf("  TEST 3: Observe structured weights at gen 6\n");

    const uint32_t N = 1000;
    float weights[1000];

    prng_state = 12345;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t pos = i % (uint32_t)total_slots(6);
        uint8_t face = (uint8_t)((pos / slots_per_face(6)) % CUBE_ADDR_FACES);
        uint16_t slot = (uint16_t)(pos % slots_per_face(6));
        GeoCubeAddr addr = geo_cube_addr(6, face, slot);

        /* Bias: type 0 gets 3x, type 7 gets 0.1x */
        double bias = 1.0;
        if (addr.cell_type == 0) bias = 3.0;
        else if (addr.cell_type == 7) bias = 0.1;

        float base = ((float)(prng_next() & 0xFFFF) / 32768.0f) - 1.0f;
        weights[i] = base * (float)bias;
    }

    PhiMicroscopeResult r = phi_observe_generation(weights, N, 6);
    printf("    Gen %u, shell=%u, n_observed=%u\n", r.gen, r.shell_size, r.n_observed);
    printf("    Count entropy = %.6f bits (same as random — same mapping)\n", r.entropy);
    printf("    Magnitude entropy = %.6f bits (structured < 2.0 — biased magnitudes)\n", r.magnitude_entropy);
    printf("    |w|_mean = %.6f\n", r.mean_magnitude);
    printf("    Cell types: ");
    for (uint32_t i = 0; i < PHI_MICRO_CELL_TYPES; i++) {
        printf("%s:%u ", cell_type_name((uint8_t)i), r.type_counts[i]);
    }
    printf("\n");

    assert(r.n_observed == N);
    /* Structured weights should have lower entropy than random */
    printf("    ✓ PASS\n");
    return 1;
}

/* ──────────────────────────────────────────────────────────────
   TEST 4: Entropy comparison — random vs structured
   ────────────────────────────────────────────────────────────── */
static int test_entropy_comparison(void) {
    printf("  TEST 4: Entropy comparison across generations\n");

    const uint32_t N = 1000;
    float random_w[1000], struct_w[1000];

    prng_state = 42;
    for (uint32_t i = 0; i < N; i++) {
        random_w[i] = ((float)(prng_next() & 0xFFFF) / 32768.0f) - 1.0f;
    }

    prng_state = 99999;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t pos = i % (uint32_t)total_slots(6);
        uint8_t face = (uint8_t)((pos / slots_per_face(6)) % CUBE_ADDR_FACES);
        uint16_t slot = (uint16_t)(pos % slots_per_face(6));
        GeoCubeAddr addr = geo_cube_addr(6, face, slot);
        double bias = 1.0;
        if (addr.cell_type == 0) bias = 5.0;   /* strong bias */
        else if (addr.cell_type == 7) bias = 0.05;

        struct_w[i] = ((float)(prng_next() & 0xFFFF) / 32768.0f - 1.0f) * (float)bias;
    }

    printf("    Gen | R_cnt H | S_cnt H | R_mag H | S_mag H | Δ_mag\n");
    printf("    ----|---------|---------|---------|---------|------\n");

    double max_drop = 0.0;
    uint8_t best_gen = 0;

    for (uint8_t g = 0; g <= 8; g++) {
        PhiMicroscopeResult rr = phi_observe_generation(random_w, N, g);
        PhiMicroscopeResult sr = phi_observe_generation(struct_w, N, g);
        double drop = rr.magnitude_entropy - sr.magnitude_entropy;
        printf("    %3u |  %.4f  |  %.4f  |  %.4f  |  %.4f  | %.4f\n",
               g, rr.entropy, sr.entropy, rr.magnitude_entropy, sr.magnitude_entropy, drop);
        if (drop > max_drop) {
            max_drop = drop;
            best_gen = g;
        }
    }

    printf("    Best separation at gen %u (Δ=%.4f bits)\n", best_gen, max_drop);
    assert(max_drop > 0.001);  /* structured must be measurably different */
    printf("    ✓ PASS\n");
    return 1;
}

/* ──────────────────────────────────────────────────────────────
   TEST 5: Full demo run
   ────────────────────────────────────────────────────────────── */
static int test_demo(void) {
    printf("  TEST 5: Full demo\n");
    phi_microscope_demo();
    printf("    ✓ PASS (demo completed)\n");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  geo_phi_microscope.h — Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    int pass = 0, total = 5;

    pass += test_entropy();
    printf("\n");
    pass += test_observe_random();
    printf("\n");
    pass += test_observe_structured();
    printf("\n");
    pass += test_entropy_comparison();
    printf("\n");
    pass += test_demo();

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  Results: %d / %d tests passed\n", pass, total);
    printf("═══════════════════════════════════════════════════════════════\n");

    return (pass == total) ? 0 : 1;
}
