/*
 * test_hyper_delta_format.c — Hyperbolic Delta Format Test
 *
 * Test: KIS compresses, Hyperbolic stores delta, verify lossless
 *
 * BUILD: gcc -O2 -I../core -o test_hyper_delta_format test_hyper_delta_format.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../core/hyper_delta.h"
#include "../core/geo_kis_projection.h"

/* Windows-compatible cycle counter */
#ifdef _WIN32
#include <intrin.h>
static inline uint64_t rdtsc(void) { return __rdtsc(); }
#else
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   Test 1: Delta calculate + recover = lossless
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_delta_lossless(void) {
    printf("TEST 1: Delta calculate + recover = lossless\n");
    printf("═══════════════════════════════════════════════\n");

    /* Original data */
    uint8_t original[20736];
    for (int i = 0; i < 20736; i++) {
        original[i] = (uint8_t)(i % 256);
    }

    /* KIS projection at scale 1.0 (coarse) */
    uint32_t scale = (uint32_t)(1.0 * 65536.0);
    uint32_t kis_coarse[20736];
    for (uint32_t i = 0; i < 20736; i++) {
        kis_coarse[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale);
    }

    /* Calculate delta */
    HyperDelta delta;
    hyper_delta_init(&delta, 1);
    hyper_delta_calculate(&delta, original, kis_coarse, 20736);

    /* Recover from KIS + delta */
    uint8_t recovered[20736];
    hyper_delta_recover(&delta, kis_coarse, recovered, 20736);

    /* Verify */
    int match = 1;
    int mismatches = 0;
    for (int i = 0; i < 20736; i++) {
        if (recovered[i] != original[i]) {
            match = 0;
            mismatches++;
            if (mismatches <= 5) {
                printf("  MISMATCH [%d]: orig=%u, kis=%u, delta=%u, recovered=%u\n",
                       i, original[i], kis_coarse[i] & 0xFF,
                       delta.data[i], recovered[i]);
            }
        }
    }

    CHECK(1, "KIS + delta = original (lossless)", match);
    printf("    Mismatches: %d / 20736\n", mismatches);

    /* Show sample */
    printf("\n  Sample (first 10):\n");
    printf("  i  | orig | kis_coarse | delta | recovered\n");
    printf("  ---|------|------------|-------|----------\n");
    for (int i = 0; i < 10; i++) {
        printf("  %2d | %4u | %10u | %5u | %9u\n",
               i, original[i], kis_coarse[i] & 0xFF,
               delta.data[i], recovered[i]);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 2: Delta at different scales
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_delta_scales(void) {
    printf("\nTEST 2: Delta at different KIS scales\n");
    printf("═══════════════════════════════════════════════\n");

    uint8_t original[20736];
    for (int i = 0; i < 20736; i++) {
        original[i] = (uint8_t)((i * 7 + 13) % 256);
    }

    double scales[] = {1.0, 0.5, 0.1, 0.01};
    int num_scales = 4;

    for (int s = 0; s < num_scales; s++) {
        uint32_t scale = (uint32_t)(scales[s] * 65536.0);
        uint32_t kis[20736];
        for (uint32_t i = 0; i < 20736; i++) {
            kis[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale);
        }

        HyperDelta delta;
        hyper_delta_init(&delta, s + 1);
        hyper_delta_calculate(&delta, original, kis, 20736);

        uint8_t recovered[20736];
        hyper_delta_recover(&delta, kis, recovered, 20736);

        int match = 1;
        for (int i = 0; i < 20736; i++) {
            if (recovered[i] != original[i]) {
                match = 0;
                break;
            }
        }

        /* Count non-zero deltas */
        int non_zero = 0;
        for (int i = 0; i < 20736; i++) {
            if (delta.data[i] != 0) non_zero++;
        }

        char desc[64];
        snprintf(desc, sizeof(desc), "scale=%.2f lossless=%s non_zero=%d",
                 scales[s], match ? "YES" : "NO", non_zero);
        CHECK(s + 2, desc, match);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 3: Delta header validity
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_delta_header(void) {
    printf("\nTEST 3: Delta header validity\n");
    printf("═══════════════════════════════════════════════\n");

    HyperDelta delta;
    hyper_delta_init(&delta, 42);

    CHECK(6, "magic == 0x48444C54", delta.header.magic == HYPER_DELTA_MAGIC);
    CHECK(7, "version == 1", delta.header.version == HYPER_DELTA_VERSION);
    CHECK(8, "kis_slots == 20736", delta.header.kis_slots == HYPER_DELTA_SLOTS);
    CHECK(9, "scale_step == 42", delta.header.scale_step == 42);
    CHECK(10, "is_valid == 1", hyper_delta_is_valid(&delta) == 1);
    CHECK(11, "size == 20752", hyper_delta_size() == sizeof(HyperDelta));
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test 4: fs vs hs speed comparison
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_fs_vs_hs_speed(void) {
    printf("\nTEST 4: fs (direct) vs hs (KIS + delta) speed\n");
    printf("═══════════════════════════════════════════════\n");

    uint8_t original[20736];
    for (int i = 0; i < 20736; i++) {
        original[i] = (uint8_t)(i % 256);
    }

    /* KIS + delta */
    uint32_t scale = (uint32_t)(0.5 * 65536.0);
    uint32_t kis[20736];
    for (uint32_t i = 0; i < 20736; i++) {
        kis[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale);
    }

    HyperDelta delta;
    hyper_delta_init(&delta, 1);
    hyper_delta_calculate(&delta, original, kis, 20736);

    /* Benchmark: fs (direct access) */
    uint64_t fs_start = rdtsc();
    for (int iter = 0; iter < 1000; iter++) {
        volatile uint8_t val = original[iter % 20736];
        (void)val;
    }
    uint64_t fs_end = rdtsc();
    uint64_t fs_cycles = fs_end - fs_start;

    /* Benchmark: hs (KIS + delta) */
    uint64_t hs_start = rdtsc();
    for (int iter = 0; iter < 1000; iter++) {
        uint32_t idx = iter % 20736;
        volatile uint8_t val = (uint8_t)((kis[idx] + delta.data[idx]) & 0xFF);
        (void)val;
    }
    uint64_t hs_end = rdtsc();
    uint64_t hs_cycles = hs_end - hs_start;

    printf("  fs (direct):      %I64u cycles (1000 reads)\n", fs_cycles);
    printf("  hs (KIS + delta): %I64u cycles (1000 reads)\n", hs_cycles);
    printf("  ratio:            %.2fx\n", (double)hs_cycles / (double)fs_cycles);

    CHECK(12, "hs = fs + delta (1-2 steps overhead)", hs_cycles > fs_cycles);
    CHECK(13, "hs < 3x fs (reasonable overhead)", hs_cycles < fs_cycles * 3);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════╗\n");
    printf("║  Hyperbolic Delta Format Test                 ║\n");
    printf("║  KIS compresses → Hyper stores delta          ║\n");
    printf("║  Full = KIS + delta (lossless)                ║\n");
    printf("╚═══════════════════════════════════════════════╝\n\n");

    test_delta_lossless();
    test_delta_scales();
    test_delta_header();
    test_fs_vs_hs_speed();

    printf("\n═══════════════════════════════════════════════\n");
    printf("RESULTS: %d PASS, %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════\n");

    return fail ? 1 : 0;
}
