/*
 * test_beam_geometric.c — Test suite for Geometric Beam Codec
 * ═══════════════════════════════════════════════════════════════════
 *
 * The geometric beam codec uses an adaptive equilateral triangle grid
 * parameterized by a single value: diameter D.
 *
 *   R = D/2 = cell radius (centroid spacing in the grid)
 *   weight w = beam radius
 *   beam drifts through chain of cells:
 *     w = n*R + delta   (n=cell_index, delta=within-cell pos)
 *
 * Compile:
 *   gcc -O2 test_beam_geometric.c -o test_beam_geometric.exe
 *
 * Run:
 *   ./test_beam_geometric.exe
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#include "beam_geometric.c"

/* ══════════════════════════════════════════════════════════════
   HELPERS
   ══════════════════════════════════════════════════════════════ */

static void print_cell_info(const GeoCell *c)
{
    double D  = (double)c->diameter / FP_SCALE;
    double R  = (double)c->radius / FP_SCALE;
    double ri = (double)c->incircle_r / FP_SCALE;
    double s  = (double)c->side / FP_SCALE;
    printf("  Cell: D=%.4f, R=%.4f, incircle=%.4f, side=%.4f\n", D, R, ri, s);
}

static void print_chain_info(const GeoChain *ch)
{
    double D = (double)ch->diameter / FP_SCALE;
    double R = (double)(ch->diameter >> 1) / FP_SCALE;
    printf("  Chain: D=%.4f, R=%.4f, cells=%u, max_weight=%.4f\n",
           D, R, ch->count, ch->count * R);
}

/* ══════════════════════════════════════════════════════════════
   TESTS
   ══════════════════════════════════════════════════════════════ */

static int test_cell_geometry(void)
{
    printf("=== Test 1: Cell Geometry ===\n");
    int fail = 0;

    /* T1: D=2.0 → R=1.0, incircle=0.5 */
    {
        GeoCell c = geo_cell_init(FP_SCALE * 2);
        double R = (double)c.radius / FP_SCALE;
        double ri = (double)c.incircle_r / FP_SCALE;
        int ok = (R == 1.0 && ri == 0.5);
        printf("  T1: D=2.0 → R=%.4f, incircle=%.4f %s\n", R, ri, ok ? "PASS" : "FAIL");
        if (!ok) fail++;
    }

    /* T2: D=8.0 → side ~6.928 */
    {
        GeoCell c = geo_cell_init(FP_SCALE * 8);
        double s = (double)c.side / FP_SCALE;
        int ok = (s > 6.92 && s < 6.93);
        printf("  T2: D=8.0 → side=%.4f (expected ~6.928) %s\n", s, ok ? "PASS" : "FAIL");
        if (!ok) fail++;
    }

    /* T3: Different D scale proportionally */
    {
        GeoCell c1 = geo_cell_init(FP_SCALE * 2);  /* D=2 */
        GeoCell c2 = geo_cell_init(FP_SCALE * 16); /* D=16 */
        /* R should scale 8× */
        int ok = (c2.radius == c1.radius * 8);
        printf("  T3: D scale: R(D=16)/R(D=2)=%d (expected 8) %s\n",
               c2.radius / c1.radius, ok ? "PASS" : "FAIL");
        if (!ok) fail++;
    }

    printf("  Result: %d/3 failed\n\n", fail);
    return fail;
}

static int test_single_cell_encode_decode(void)
{
    printf("=== Test 2: Single Cell Encode/Decode (trivial) ===\n");
    int fail = 0;

    GeoCell c = geo_cell_init(FP_SCALE * 4);
    print_cell_info(&c);

    /* Single cell: delta = weight (trivial pass-through) */
    const int32_t test_vals[] = {
        0,
        FP_SCALE / 4,       /* 0.25 */
        FP_SCALE / 2,       /* 0.5 */
        FP_SCALE,           /* 1.0 */
        FP_SCALE * 15 / 10, /* 1.5 */
        FP_SCALE * 2,       /* 2.0 = R */
        -FP_SCALE,          /* -1.0 */
        -FP_SCALE * 15 / 10,/* -1.5 */
    };

    int ok_count = 0;
    for (int i = 0; i < 8; i++) {
        int32_t w = test_vals[i];
        int32_t d = geo_beam_encode(&c, w);
        int32_t r = geo_beam_decode(&c, d);
        int ok = (d == w && r == w);
        printf("  T4[%d]: w=%-7.4f → delta=%-7.4f → decode=%-7.4f %s\n",
               i, (double)w/FP_SCALE, (double)d/FP_SCALE,
               (double)r/FP_SCALE, ok ? "PASS" : "FAIL");
        if (ok) ok_count++;
    }
    printf("  T4: %d/8 PASS\n", ok_count);
    if (ok_count != 8) fail++;

    printf("  Result: %d/1 failed\n\n", fail);
    return fail;
}

static int test_chain_exact_roundtrip(void)
{
    printf("=== Test 3: Chain Exact Roundtrip ===\n");
    int fail = 0;

    GeoChain ch = geo_chain_init(FP_SCALE * 4, 16);  /* D=4.0, R=2.0, 16 cells */
    print_chain_info(&ch);

    /* T5: Test all within range w ∈ [-31*R, 31*R] step R/4 */
    int32_t R = ch.diameter >> 1;
    int32_t min_w = -(int32_t)((ch.count - 1) * R);
    int32_t max_w = (int32_t)((ch.count - 1) * R + R - 1);

    int bad = 0;
    int total = 0;
    for (int32_t w = min_w; w <= max_w; w += FP_SCALE / 4) {
        int32_t packed = geo_chain_encode(&ch, w);
        int32_t r = geo_chain_decode(&ch, packed);
        if (r != w) { bad++; if (bad <= 3) printf("  FAIL at w=%d (0x%08x)\n", w, packed); }
        total++;
    }
    printf("  T5: chain exact roundtrip: %d/%d PASS\n", total - bad, total);
    if (bad > 0) fail++;

    /* T6: Cell boundary tests — check reconstructed values not packed format */
    {
        int b_ok = 1;
        int32_t R = ch.diameter >> 1;

        /* At w = 0: should decode to 0 */
        int32_t p = geo_chain_encode(&ch, 0);
        if (geo_chain_decode(&ch, p) != 0) {
            printf("  T6a FAIL: w=0 -> decode=%lld\n",
                   (long long)geo_chain_decode(&ch, p)); b_ok = 0;
        }

        /* At w = R: cell 1, within 0 — must reconstruct R exactly */
        p = geo_chain_encode(&ch, R);
        int32_t dec = geo_chain_decode(&ch, p);
        if (dec != R) {
            printf("  T6b FAIL: w=R -> decode=%lld (expected %d)\n",
                   (long long)dec, R); b_ok = 0;
        }

        /* At w = R/2: exact midpoint */
        p = geo_chain_encode(&ch, R/2);
        dec = geo_chain_decode(&ch, p);
        int32_t err = dec - R/2; if (err < 0) err = -err;
        if (err > 1) {
            printf("  T6c FAIL: w=R/2 -> decode=%lld (expected %d, err=%d)\n",
                   (long long)dec, R/2, err); b_ok = 0;
        }

        printf("  T6: cell boundaries %s\n", b_ok ? "PASS" : "FAIL");
        if (!b_ok) fail++;
    }

    printf("  Result: %d/2 failed\n\n", fail);
    return fail;
}

static int test_adaptive_scale(void)
{
    printf("=== Test 4: Adaptive Scale ===\n");
    int fail = 0;

    /* Different diameters should all pass exact roundtrip */
    int32_t diameters[] = {
        FP_SCALE * 1,     /* D=1.0 */
        FP_SCALE * 2,     /* D=2.0 */
        FP_SCALE * 4,     /* D=4.0 */
        FP_SCALE * 8,     /* D=8.0 */
        FP_SCALE * 16,    /* D=16.0 */
        FP_SCALE * 100,   /* D=100.0 */
    };
    int n_D = sizeof(diameters) / sizeof(diameters[0]);
    int all_ok = 1;

    for (int d = 0; d < n_D; d++) {
        int32_t D = diameters[d];
        int32_t R = D >> 1;
        GeoChain ch = geo_chain_init(D, 8);  /* 8 cells */

        int bad = 0;
        int total = 0;
        for (int32_t w = -R * 7; w <= R * 7 + R - 1; w += (R > FP_SCALE) ? (R / 8) : 1) {
            int32_t p = geo_chain_encode(&ch, w);
            int32_t r = geo_chain_decode(&ch, p);
            int32_t err = r - w; if (err < 0) err = -err;
            if (err > 1) bad++;
            total++;
        }
        double Df = (double)D / FP_SCALE;
        int ok = (bad == 0);
        printf("  D=%.1f: %d/%d PASS %s\n", Df, total - bad, total,
               ok ? "✓" : "FAIL");
        if (!ok) all_ok = 0;
    }

    printf("  T7: all diameters exact %s\n", all_ok ? "PASS" : "FAIL");
    if (!all_ok) fail++;

    /* Demonstrate adaptive scale: same delta means different weight */
    printf("\n  Scale shift demo:\n");
    {
        double D1 = 2.0, D2 = 8.0;
        GeoChain c1 = geo_chain_init((int32_t)(FP_SCALE * D1), 16);
        GeoChain c2 = geo_chain_init((int32_t)(FP_SCALE * D2), 16);
        int32_t delta = (int32_t)(0.3 * FP_SCALE);  /* delta = 0.3 */

        /* Same delta at different D → different weights */
        int32_t p1 = geo_chain_encode(&c1, delta);
        int32_t p2 = geo_chain_encode(&c2, delta);

        printf("    delta=0.3: D=%.0f -> w=%.4f, D=%.0f -> w=%.4f\n",
               D1, (double)geo_chain_decode(&c1, p1)/FP_SCALE,
               D2, (double)geo_chain_decode(&c2, p2)/FP_SCALE);
        printf("    Changing D re-scales the entire codec.\n");
    }

    printf("  Result: %d/1 failed\n\n", fail);
    return fail;
}

static int test_sign_handling(void)
{
    printf("=== Test 5: Sign Handling ===\n");
    int fail = 0;

    GeoChain ch = geo_chain_init(FP_SCALE * 4, 16);
    int32_t R = ch.diameter >> 1;

    int ok_all = 1;
    for (int32_t w = -R * 5; w <= R * 5; w += R / 2) {
        int32_t p = geo_chain_encode(&ch, w);
        int32_t r = geo_chain_decode(&ch, p);
        if (r != w) {
            printf("  FAIL at w=%lld: packed=0x%08x -> r=%lld\n",
                   (long long)w, (unsigned)p, (long long)r);
            ok_all = 0;
            break;
        }
        /* Sign of packed should match sign of weight */
        if ((w < 0 && p >= 0) || (w > 0 && p < 0)) {
            printf("  FAIL: sign mismatch w=%lld packed=0x%08x\n",
                   (long long)w, (unsigned)p);
            ok_all = 0;
            break;
        }
    }
    printf("  T8: sign handling %s\n", ok_all ? "PASS" : "FAIL");
    if (!ok_all) fail++;

    printf("  Result: %d/1 failed\n\n", fail);
    return fail;
}

static int test_edge_cases(void)
{
    printf("=== Test 6: Edge Cases ===\n");
    int fail = 0;

    GeoChain ch = geo_chain_init(FP_SCALE * 4, 8);  /* D=4, R=2, 8 cells */
    int32_t R = ch.diameter >> 1;

    /* T9: weight at cell centroids — reconstruction should be exact */
    {
        int ok_all = 1;
        for (uint32_t cell = 0; cell < ch.count; cell++) {
            int32_t w = (int32_t)(cell * R);  /* exactly at centroid */
            int32_t p = geo_chain_encode(&ch, w);
            int32_t dec = geo_chain_decode(&ch, p);
            if (dec != w) {
                printf("  T9 FAIL at cell=%u w=%d: decode=%lld\n",
                       cell, w, (long long)dec);
                ok_all = 0;
            }
        }
        printf("  T9: cell centroids %s\n", ok_all ? "PASS" : "FAIL");
        if (!ok_all) fail++;
    }

    /* T10: zero weight */
    {
        int32_t p = geo_chain_encode(&ch, 0);
        int32_t r = geo_chain_decode(&ch, p);
        int ok = (p == 0 && r == 0);
        printf("  T10: zero -> packed=0x%08x -> decode=%lld %s\n",
               (unsigned)p, (long long)r, ok ? "PASS" : "FAIL");
        if (!ok) fail++;
    }

    /* T11: max weight */
    {
        int32_t max_w = (int32_t)((ch.count - 1) * R + R - 1);
        int32_t p = geo_chain_encode(&ch, max_w);
        int32_t r = geo_chain_decode(&ch, p);
        int ok = (r == max_w);
        printf("  T11: max w=%.4f -> packed=0x%08x -> decode=%.4f %s\n",
               (double)max_w/FP_SCALE, (unsigned)p, (double)r/FP_SCALE,
               ok ? "PASS" : "FAIL");
        if (!ok) fail++;
    }

    /* T12: weight beyond chain saturates to last cell */
    {
        int32_t too_big = (ch.count * R) + FP_SCALE;  /* beyond last cell */
        int32_t p = geo_chain_encode(&ch, too_big);
        int32_t dec = geo_chain_decode(&ch, p);
        /* Should saturate near max */
        int32_t max_possible = (int32_t)((ch.count - 1) * R + R - 1);
        int32_t err = dec - max_possible; if (err < 0) err = -err;
        int ok = (err <= 1);
        printf("  T12: w beyond chain -> decode=%.4f (max=%.4f, err=%d) %s\n",
               (double)dec/FP_SCALE, (double)max_possible/FP_SCALE,
               err, ok ? "PASS" : "FAIL");
        if (!ok) fail++;
    }

    printf("  Result: %d/4 failed\n\n", fail);
    return fail;
}

static int test_verify(void)
{
    printf("=== Test 7: Self-Verify ===\n");
    int result = beam_geometric_verify();
    printf("  beam_geometric_verify: %s\n", result == 0 ? "PASS" : "FAIL");
    if (result != 0) printf("  (returned %d)\n", result);
    return (result != 0);
}

/* ══════════════════════════════════════════════════════════════
   BENCHMARK
   ══════════════════════════════════════════════════════════════ */

static void benchmark_throughput(void)
{
    printf("=== Benchmark: Throughput ===\n");

    const uint32_t N = 1000000;
    const uint32_t ITERS = 10;

    GeoChain ch = geo_chain_init(FP_SCALE * 4, 256);
    int32_t *weights = (int32_t *)malloc(N * sizeof(int32_t));
    int32_t *packed  = (int32_t *)malloc(N * sizeof(int32_t));
    int32_t *results = (int32_t *)malloc(N * sizeof(int32_t));

    if (!weights || !packed || !results) {
        printf("  Malloc failed\n");
        free(weights); free(packed); free(results);
        return;
    }

    srand(42);
    for (uint32_t i = 0; i < N; i++) {
        int32_t R = ch.diameter >> 1;
        weights[i] = (int32_t)((rand() % (R * 16)) - R * 8);
    }

    double t0, t1;
    volatile int32_t sink = 0;

    /* Encode benchmark */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            packed[i] = geo_chain_encode(&ch, weights[i]);
        }
    }
    t1 = now_sec();
    double enc_time = t1 - t0;
    printf("  Encode: %.4f sec (%.0f ops/sec)\n",
           enc_time, (double)N * ITERS / enc_time);

    /* Decode benchmark */
    t0 = now_sec();
    for (uint32_t iter = 0; iter < ITERS; iter++) {
        for (uint32_t i = 0; i < N; i++) {
            results[i] = geo_chain_decode(&ch, packed[i]);
            sink = results[i];
        }
    }
    t1 = now_sec();
    double dec_time = t1 - t0;
    printf("  Decode: %.4f sec (%.0f ops/sec)\n",
           dec_time, (double)N * ITERS / dec_time);

    /* Verify correctness */
    int bad = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (results[i] != weights[i]) { bad++; break; }
    }
    printf("  Correct: %s\n", bad == 0 ? "ALL PASS" : "FAIL");
    printf("  Bandwidth: %.2f GB/s (combined)\n",
           (double)N * ITERS * sizeof(int32_t) * 2 / (enc_time + dec_time) / 1e9);

    (void)sink;
    free(weights); free(packed); free(results);
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   DEMO — show the geometric encoding in action
   ══════════════════════════════════════════════════════════════ */

static void demo_encoding(void)
{
    printf("=== Demo: Geometric Beam Codec ===\n\n");

    /* Create a chain with D=4.0 */
    GeoChain ch = geo_chain_init(FP_SCALE * 4, 8);
    int32_t R = ch.diameter >> 1;
    double Rf = (double)R / FP_SCALE;

    printf("  Cell radius R = D/2 = %.4f\n", Rf);
    printf("  Centroids at: 0, %.4f, %.4f, ..., %.4f\n",
           Rf, 2*Rf, (ch.count-1)*Rf);
    printf("  Single parameter D = %.4f controls the entire scale\n",
           (double)ch.diameter / FP_SCALE);
    printf("  Changing D → all centroids shift → adaptive grid.\n");
    printf("\n");

    printf("  weight (w)  cell_idx  within   decode   error\n");
    printf("  ──────────  ────────  ──────   ──────   ─────\n");

    int32_t examples[] = {
        0,
        FP_SCALE / 2,       /* 0.5 */
        FP_SCALE,           /* 1.0 */
        FP_SCALE * 2 - 1,   /* 1.999 (just below R) */
        FP_SCALE * 2,       /* 2.0 = R (drifts to cell 1) */
        FP_SCALE * 3,       /* 3.0 */
        FP_SCALE * 5,       /* 5.0 */
        FP_SCALE * 10,      /* 10.0 */
        -FP_SCALE,          /* -1.0 */
        -FP_SCALE * 3,      /* -3.0 */
    };

    for (int i = 0; i < 10; i++) {
        int32_t w = examples[i];
        int32_t packed = geo_chain_encode(&ch, w);
        int32_t recovered = geo_chain_decode(&ch, packed);

        int32_t err = recovered - w; if (err < 0) err = -err;

        uint32_t abs_packed = (uint32_t)((packed < 0) ? -packed : packed);
        uint32_t cell = (abs_packed >> 16) & 0xFFFF;
        double wf = (double)w / FP_SCALE;
        double rf = (double)recovered / FP_SCALE;

        /* Compute decoded within-cell position */
        int32_t decoded_w = (packed < 0) ? -recovered : recovered;
        int32_t within = (uint32_t)(cell * R);
        within = (decoded_w >= within) ? decoded_w - within : 0;

        printf("  %10.4f  %8u  %7.4f  %7.4f  %s\n",
               wf, cell, (double)within / FP_SCALE, rf,
               (err == 0) ? "✓" : "≈");
    }

    printf("\n");
    printf("  The beam (radius = weight) drifts through the grid.\n");
    printf("  cell_idx = how many radii the beam crosses.\n");
    printf("  within   = remaining position inside the final cell.\n");
    printf("  The triangle geometry provides R = D/2 as the scale.\n");
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   Geometric Beam Codec Prototype                     ║\n");
    printf("║   \"weight = beam radius, R = D/2 from geometry\"      ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    int total_fail = 0;
    total_fail += test_cell_geometry();
    total_fail += test_single_cell_encode_decode();
    total_fail += test_chain_exact_roundtrip();
    total_fail += test_adaptive_scale();
    total_fail += test_sign_handling();
    total_fail += test_edge_cases();
    total_fail += test_verify();

    printf("╔══════════════════════════════════════════════════════╗\n");
    if (total_fail == 0) {
        printf("║   FINAL: ALL %d TESTS PASS                          ║\n", 7);
    } else {
        printf("║   FINAL: %d TESTS FAILED                              ║\n", total_fail);
    }
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* Only run benchmark and demo if all pass */
    if (total_fail == 0) {
        demo_encoding();
        benchmark_throughput();
    }

    return total_fail;
}
