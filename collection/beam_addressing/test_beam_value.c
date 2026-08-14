/*
 * test_beam_value.c — Standalone test for beam_value.c + FGLS integration
 * ═══════════════════════════════════════════════════════════════════
 *
 * Tests both:
 *   1. RUNTIME: BeamCoord (transient, with param_index for navigation)
 *   2. STORAGE: BeamCode (8-bit, 2-level hierarchy, zone×position)
 *
 * Compile: gcc -O2 -I../core test_beam_value.c -o test_beam_value.exe
 * Run: ./test_beam_value.exe
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static double now_sec(void) {
    static LARGE_INTEGER freq = {0};
    static int init = 0;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}
#else
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

#include "beam_value.c"

/* ══════════════════════════════════════════════════════════════
   TEST SUITE
   ══════════════════════════════════════════════════════════════ */

static int test_beam_code_storage(void)
{
    printf("=== Test 1: BeamCode Storage (8-bit) ===\n");
    int failed = 0;

    /* T1: weight → code → weight roundtrip for full Q8 range */
    {
        int ok = 1;
        for (int32_t w = -128; w <= 127; w++) {
            BeamCode code = beam_code_from_weight(w);
            int32_t r = beam_weight_from_code(code);
            if (r != w) { ok = 0; break; }
        }
        printf("  T1: Q8 full range roundtrip %s\n", ok ? "PASS" : "FAIL");
        if (!ok) failed++;
    }

    /* T2: BeamCode is 8-bit (1 byte) */
    {
        int ok = (sizeof(BeamCode) == 1);
        printf("  T2: sizeof(BeamCode)=%zu %s\n", sizeof(BeamCode), ok ? "PASS" : "FAIL");
        if (!ok) failed++;
    }

    /* T3: Each Q8 value maps to unique 8-bit code */
    {
        int codes[256] = {0};
        int dup = 0;
        for (int32_t w = -128; w <= 127; w++) {
            BeamCode c = beam_code_from_weight(w);
            codes[c]++;
            if (codes[c] > 1) { dup = 1; break; }
        }
        printf("  T3: unique mapping [%d dup] %s\n", dup, dup ? "FAIL" : "PASS");
        if (dup) failed++;

        /* Verify zone/position split */
        int zp_ok = 1;
        for (int i = 0; i < 256; i++) {
            BeamCode c = (BeamCode)i;
            uint8_t zone = beam_code_zone(c);
            uint8_t pos = beam_code_pos(c);
            if (zone > 15 || pos > 15) { zp_ok = 0; break; }
            if ((zone << 4 | pos) != i) { zp_ok = 0; break; }
        }
        printf("  T3b: zone/position correct %s\n", zp_ok ? "PASS" : "FAIL");
        if (!zp_ok) failed++;
    }

    /* T4: Zero weight */
    {
        BeamCode c = beam_code_from_weight(0);
        int32_t r = beam_weight_from_code(c);
        printf("  T4: weight=0 -> code=%u -> recovered=%d %s\n",
               c, r, (r == 0) ? "PASS" : "FAIL");
        if (r != 0) failed++;
    }

    /* T5: Edge cases */
    {
        BeamCode c1 = beam_code_from_weight(127);
        BeamCode c2 = beam_code_from_weight(-128);
        int32_t r1 = beam_weight_from_code(c1);
        int32_t r2 = beam_weight_from_code(c2);
        printf("  T5: max=127 -> %d %s, min=-128 -> %d %s\n",
               r1, (r1 == 127) ? "PASS" : "FAIL",
               r2, (r2 == -128) ? "PASS" : "FAIL");
        if (r1 != 127 || r2 != -128) failed++;
    }

    /* T6: BeamCoord → BeamCode conversion */
    {
        BeamCoord rc = beam_weight_to_coord(0, 42, 100);
        BeamCode code = beam_coord_to_code(rc);
        int32_t w = beam_weight_from_code(code);
        printf("  T6: coord->code->weight 100 -> %d %s\n",
               w, (w == 100) ? "PASS" : "FAIL");
        if (w != 100) failed++;
    }

    /* T7: BeamCode → BeamCoord conversion */
    {
        BeamCode code = beam_code_from_weight(-50);
        BeamCoord c = beam_code_to_coord(code, 0, 100);
        int32_t w = beam_coord_to_weight(c);
        printf("  T7: code->coord->weight -50 -> %d %s (param=%u)\n",
               w, (w == -50) ? "PASS" : "FAIL", c.param_index);
        if (w != -50 || c.param_index != 100) failed++;
    }

    /* T8: Batch code store + roundtrip */
    {
        int32_t weights[] = {10, -20, 30, -40, 50, -60, 70, -80, 90, -100};
        BeamCode codes[10];
        uint32_t stored = beam_store_codes(weights, 10, codes, 10);
        int ok = stored == 10 && beam_verify_code_roundtrip(weights, codes, 10);
        printf("  T8: batch codes (%u stored) %s\n", stored, ok ? "PASS" : "FAIL");
        if (!ok) failed++;
    }

    printf("  BeamCode tests: %d/8 failed\n\n", failed);
    return failed;
}

static int test_beam_value_core(void)
{
    printf("=== Test 2: Beam Value Core (Runtime) ===\n");
    int failed = 0;

    /* T9: roundtrip positive */
    {
        BeamCoord c = beam_weight_to_coord(0, 0, 100);
        int32_t r = beam_coord_to_weight(c);
        printf("  T9: weight=100 -> coord=(%u,%u,%u,%u) -> recovered=%d %s\n",
               c.capo_id, c.param_index, c.abs_value, c.sign, r,
               (r == 100) ? "PASS" : "FAIL");
        if (r != 100) failed++;
    }

    /* T10: roundtrip negative */
    {
        BeamCoord c = beam_weight_to_coord(0, 1, -50);
        int32_t r = beam_coord_to_weight(c);
        printf("  T10: weight=-50 -> coord=(%u,%u,%u,%u) -> recovered=%d %s\n",
               c.capo_id, c.param_index, c.abs_value, c.sign, r,
               (r == -50) ? "PASS" : "FAIL");
        if (r != -50) failed++;
    }

    /* T11: roundtrip zero */
    {
        BeamCoord c = beam_weight_to_coord(0, 2, 0);
        int32_t r = beam_coord_to_weight(c);
        printf("  T11: weight=0 -> coord=(%u,%u,%u,%u) -> recovered=%d %s\n",
               c.capo_id, c.param_index, c.abs_value, c.sign, r,
               (r == 0) ? "PASS" : "FAIL");
        if (r != 0) failed++;
    }

    /* T12: Q8 range */
    {
        BeamCoord c1 = beam_weight_to_coord(0, 3, 127);
        int32_t r1 = beam_coord_to_weight(c1);
        BeamCoord c2 = beam_weight_to_coord(0, 4, -128);
        int32_t r2 = beam_coord_to_weight(c2);
        printf("  T12: Q8 max=127 -> %d %s, Q8 min=-128 -> %d %s\n",
               r1, (r1 == 127) ? "PASS" : "FAIL",
               r2, (r2 == -128) ? "PASS" : "FAIL");
        if (r1 != 127 || r2 != -128) failed++;
    }

    printf("  Core tests: %d/4 failed\n\n", failed);
    return failed;
}

static int test_fibo_tick_integration(void)
{
    printf("=== Test 3: Fibo Tick Integration ===\n");
    int failed = 0;

    /* T13: slot index within range */
    for (uint32_t i = 0; i < 100; i++) {
        BeamCoord c = beam_weight_to_coord(0, i, (int32_t)(i % 128));
        uint32_t slot = beam_to_fibo_slot(c);
        if (slot >= FT_GEO_FULL) {
            printf("  T13 FAIL: param=%u slot=%u >= %u\n", i, slot, FT_GEO_FULL);
            failed++;
            break;
        }
    }
    if (failed == 0) printf("  T13: slot index 0..99 within range PASS\n");

    /* T14: flower ID within range */
    for (uint32_t i = 0; i < 100; i++) {
        BeamCoord c = beam_weight_to_coord(0, i, (int32_t)(i % 128));
        uint16_t flower = beam_to_fibo_flower(c);
        if (flower >= FT_FLOWERS_FULL) {
            printf("  T14 FAIL: param=%u flower=%u >= %u\n", i, flower, FT_FLOWERS_FULL);
            failed++;
            break;
        }
    }
    if (failed == 0) printf("  T14: flower ID 0..99 within range PASS\n");

    /* T15: texture valid */
    for (uint32_t i = 0; i < 100; i++) {
        BeamCoord c = beam_weight_to_coord(0, i, (int32_t)(i % 128));
        uint8_t tex = beam_to_fibo_texture(c);
        if (tex != P5H_TEX_INNER && tex != P5H_TEX_OUTER && tex != 0) {
            printf("  T15 FAIL: param=%u texture=%u invalid\n", i, tex);
            failed++;
            break;
        }
    }
    if (failed == 0) printf("  T15: texture 0..99 valid PASS\n");

    printf("  Fibo tick tests: %d/3 failed\n\n", failed > 0 ? 3 : 0);
    return failed;
}

static int test_frame_seek_integration(void)
{
    printf("=== Test 4: Frame Seek Integration ===\n");
    int failed = 0;

    /* T16: DualFrame within range */
    for (uint32_t i = 0; i < 100; i++) {
        BeamCoord c = beam_weight_to_coord(0, i, (int32_t)(i % 128));
        DualFrame f = beam_to_frame(c);
        if (f.face > 11 || f.slot > 119) {
            printf("  T16 FAIL: param=%u face=%u slot=%u\n", i, f.face, f.slot);
            failed++;
            break;
        }
    }
    if (failed == 0) printf("  T16: DualFrame 0..99 within range PASS\n");

    /* T17: frame range with entropy */
    for (uint32_t i = 0; i < 100; i++) {
        BeamCoord c = beam_weight_to_coord(0, i, (int32_t)(i % 128));
        FrameRange fr = beam_to_frame_range(c, 2);
        if (fr.span > 3) {
            printf("  T17 FAIL: param=%u span=%u\n", i, fr.span);
            failed++;
            break;
        }
    }
    if (failed == 0) printf("  T17: frame range span valid PASS\n");

    printf("  Frame seek tests: %d/2 failed\n\n", failed > 0 ? 2 : 0);
    return failed;
}

static int test_spherical_integration(void)
{
    printf("=== Test 5: Spherical Integration ===\n");
    int failed = 0;

    /* T18: azimuth/elevation within range */
    for (uint32_t i = 0; i < 100; i++) {
        BeamCoord c = beam_weight_to_coord(0, i, (int32_t)(i % 128));
        uint16_t az, el;
        beam_to_spherical(c, &az, &el);
        if (az >= 360 || el >= 360) {
            printf("  T18 FAIL: param=%u az=%u el=%u\n", i, az, el);
            failed++;
            break;
        }
    }
    if (failed == 0) printf("  T18: spherical 0..99 within range PASS\n");

    /* T19: 5D coordinate */
    for (uint32_t i = 0; i < 100; i++) {
        BeamCoord c = beam_weight_to_coord(0, i, (int32_t)(i % 128));
        BeamCoord5D c5 = beam_to_5d(c);
        if (c5.azimuth >= 360 || c5.elevation >= 360) {
            printf("  T19 FAIL: param=%u\n", i);
            failed++;
            break;
        }
    }
    if (failed == 0) printf("  T19: 5D coordinate 0..99 valid PASS\n");

    printf("  Spherical tests: %d/2 failed\n\n", failed > 0 ? 2 : 0);
    return failed;
}

static int test_batch_operations(void)
{
    printf("=== Test 6: Batch Operations ===\n");
    int failed = 0;

    /* T20: batch BeamCoord store + verify */
    int32_t weights[] = {10, -20, 30, -40, 50, -60, 70, -80, 90, -100};
    BeamCoord coords[10];
    uint32_t stored = beam_store_weights(weights, 10, coords, 10);

    if (stored != 10) {
        printf("  T20 FAIL: BeamCoord stored=%u expected=10\n", stored);
        failed++;
    } else if (!beam_verify_roundtrip(weights, coords, 10)) {
        printf("  T20 FAIL: BeamCoord roundtrip failed\n");
        failed++;
    } else {
        printf("  T20: batch BeamCoord store + verify PASS\n");
    }

    /* T21: batch BeamCode store + verify */
    BeamCode codes[10];
    stored = beam_store_codes(weights, 10, codes, 10);
    if (stored != 10) {
        printf("  T21 FAIL: BeamCode stored=%u expected=10\n", stored);
        failed++;
    } else if (!beam_verify_code_roundtrip(weights, codes, 10)) {
        printf("  T21 FAIL: BeamCode roundtrip failed\n");
        failed++;
    } else {
        printf("  T21: batch BeamCode store + verify PASS\n");
    }

    /* T22: stats */
    BeamStats s = beam_compute_stats(weights, 10);
    printf("  T22: stats count=%u min=%d max=%d pos=%u neg=%u %s\n",
           s.count, s.min_value, s.max_value,
           s.positive_count, s.negative_count,
           (s.count == 10 && s.min_value == -100 && s.max_value == 90 &&
            s.positive_count == 5 && s.negative_count == 5) ? "PASS" : "FAIL");
    if (s.count != 10 || s.min_value != -100 || s.max_value != 90 ||
        s.positive_count != 5 || s.negative_count != 5) failed++;

    printf("  Batch tests: %d/3 failed\n\n", failed > 0 ? 3 : 0);
    return failed;
}

static int test_histogram(void)
{
    printf("=== Test 7: Histogram ===\n");
    int failed = 0;

    int32_t weights[512];
    for (int i = 0; i < 512; i++) {
        weights[i] = (int32_t)((i % 256) - 128);
    }
    BeamHistogram h = beam_compute_histogram(weights, 512);

    printf("  total=%llu, non_zero=%u, zero_count=%u\n",
           (unsigned long long)h.total, h.non_zero, h.zero_count);
    printf("  max_count=%u at code=%u (zone=%u, pos=%u)\n",
           h.max_count, h.max_code,
           beam_code_zone(h.max_code), beam_code_pos(h.max_code));

    int ok = (h.total == 512 && h.non_zero == 256 && h.zero_count == 2);
    printf("  T23: histogram correct %s\n", ok ? "PASS" : "FAIL");
    if (!ok) failed++;

    printf("  Histogram tests: %d/1 failed\n\n", failed);
    return failed;
}

static int test_fibonacci_tick_verify(void)
{
    printf("=== Test 8: FGLS Core Verify ===\n");
    int failed = 0;

    int result = fibo_tick_verify();
    printf("  fibo_tick_verify: %s\n", (result == 0) ? "PASS" : "FAIL");

    int result2 = geo_frame_seek_verify();
    printf("  geo_frame_seek_verify: %s\n", (result2 == 0) ? "PASS" : "FAIL");

    int result3 = beam_value_verify();
    printf("  beam_value_verify: %s\n", (result3 == 0) ? "PASS" : "FAIL");

    int f = (result != 0) + (result2 != 0) + (result3 != 0);
    printf("  Core verify: %d/3 passed\n\n", 3 - f);
    return f;
}

/* ══════════════════════════════════════════════════════════════
   BENCHMARK — throughput test
   ══════════════════════════════════════════════════════════════ */

static void benchmark_throughput(void)
{
    printf("=== Benchmark: Throughput ===\n");

    const uint32_t N = 1000000;
    const uint32_t ITERS = 10;
    int32_t *weights = (int32_t *)malloc(N * sizeof(int32_t));
    BeamCoord *coords = (BeamCoord *)malloc(N * sizeof(BeamCoord));
    BeamCode *codes = (BeamCode *)malloc(N * sizeof(BeamCode));

    if (!weights || !coords || !codes) {
        printf("  Malloc failed\n");
        free(weights);
        free(coords);
        free(codes);
        return;
    }

    srand(42);
    for (uint32_t i = 0; i < N; i++) {
        weights[i] = (int32_t)(rand() % 256) - 128;
    }

    double t0, t1, elapsed;
    volatile int32_t sink_i32 = 0;
    volatile uint32_t sink_u32 = 0;
    volatile uint16_t sink_u16 = 0;

    /* Benchmark: weight → BeamCode (new storage format) */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            codes[i] = beam_code_from_weight(weights[i]);
        }
    }
    t1 = now_sec();
    elapsed = t1 - t0;
    printf("  weight -> BeamCode: %.4f sec (%.0f ops/sec)\n",
           elapsed, (double)N * ITERS / elapsed);

    /* Benchmark: BeamCode → weight */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            int32_t r = beam_weight_from_code(codes[i]);
            sink_i32 = r;
        }
    }
    t1 = now_sec();
    elapsed = t1 - t0;
    printf("  BeamCode -> weight: %.4f sec (%.0f ops/sec)\n",
           elapsed, (double)N * ITERS / elapsed);

    /* Benchmark: weight → coord (runtime) */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            uint32_t capo_id = i / BEAM_PARAMS_PER_CAPO;
            coords[i] = beam_weight_to_coord(capo_id, i, weights[i]);
        }
    }
    t1 = now_sec();
    elapsed = t1 - t0;
    printf("  weight -> coord:   %.4f sec (%.0f ops/sec)\n",
           elapsed, (double)N * ITERS / elapsed);

    /* Benchmark: coord → weight */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            int32_t r = beam_coord_to_weight(coords[i]);
            sink_i32 = r;
        }
    }
    t1 = now_sec();
    printf("  coord -> weight:   %.4f sec (%.0f ops/sec)\n",
           elapsed, (double)N * ITERS / elapsed);

    /* Benchmark: fibo_tick slot mapping */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            uint32_t slot = beam_to_fibo_slot(coords[i]);
            sink_u32 = slot;
        }
    }
    t1 = now_sec();
    elapsed = t1 - t0;
    printf("  -> fibo_slot:      %.4f sec (%.0f ops/sec)\n",
           elapsed, (double)N * ITERS / elapsed);

    /* Benchmark: frame mapping */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            DualFrame f = beam_to_frame(coords[i]);
            sink_u32 = f.face;
        }
    }
    t1 = now_sec();
    elapsed = t1 - t0;
    printf("  -> frame:          %.4f sec (%.0f ops/sec)\n",
           elapsed, (double)N * ITERS / elapsed);

    /* Benchmark: spherical mapping */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            uint16_t az, el;
            beam_to_spherical(coords[i], &az, &el);
            sink_u16 = az;
        }
    }
    t1 = now_sec();
    elapsed = t1 - t0;
    printf("  -> spherical:      %.4f sec (%.0f ops/sec)\n",
           elapsed, (double)N * ITERS / elapsed);

    /* Benchmark: batch BeamCode store */
    t0 = now_sec();
    uint32_t stored = beam_store_codes(weights, N, codes, N);
    t1 = now_sec();
    elapsed = t1 - t0;
    printf("  batch_codes:       %.4f sec (%.0f ops/sec) [%u stored]\n",
           elapsed, (double)N / elapsed, stored);

    /* Benchmark: verify code roundtrip */
    t0 = now_sec();
    int ok = beam_verify_code_roundtrip(weights, codes, N);
    t1 = now_sec();
    elapsed = t1 - t0;
    printf("  verify_codes:      %.4f sec (%.0f ops/sec) [%s]\n",
           elapsed, (double)N / elapsed, ok ? "PASS" : "FAIL");

    free(weights);
    free(coords);
    free(codes);
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  BEAM VALUE v2 — 8-bit BeamCode Storage                ║\n");
    printf("║  2-level: zone(4b) × position(4b) = 256 = Q8          ║\n");
    printf("║  Runtime: BeamCoord (transient) | Storage: BeamCode    ║\n");
    printf("║  Navigation computed at runtime — NEVER stored         ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    int total_failed = 0;

    total_failed += test_beam_code_storage();
    total_failed += test_beam_value_core();
    total_failed += test_fibo_tick_integration();
    total_failed += test_frame_seek_integration();
    total_failed += test_spherical_integration();
    total_failed += test_batch_operations();
    total_failed += test_histogram();
    total_failed += test_fibonacci_tick_verify();

    printf("══════════════════════════════════════════════════════════\n");
    printf("TOTAL: %s (%d test groups failed)\n",
           (total_failed == 0) ? "ALL PASS" : "SOME FAILED",
           total_failed);
    printf("══════════════════════════════════════════════════════════\n\n");

    benchmark_throughput();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("DONE\n");
    printf("══════════════════════════════════════════════════════════\n");

    return total_failed;
}
