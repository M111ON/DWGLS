/*
 * bench_hex_quad_upgrades.c — Performance Benchmark for Hex-Quad-Dual Upgrades
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Measures:
 *   1. CRT Bridge: address decomposition (linear scan vs CRT seek)
 *   2. Gosper Path: locality ratio across all levels
 *   3. FCC DRamTile: neighbor access latency
 *   4. Robinson Tiles: tile assignment throughput
 *   5. Cross-system: integration overhead
 *
 * Build: gcc -O2 -Wall -Icore -Icore/infra -I. -no-pie bench_hex_quad_upgrades.c -lm -o build/bench_hex_quad_upgrades
 */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "core/geo_crt_bridge.h"
#include "core/geo_param_grid.h"
#include "core/infra/geo_twin_rebalance.h"
#include "core/geo_gosper_path.h"
#include "core/geo_fcc_dramtile.h"
#include "core/geo_robinson.h"

/* ═══════════════════════════════════════════════════════════════════════════
   TIMING UTILITIES
   ═══════════════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <windows.h>
static double now_sec(void) {
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}
#else
#include <sys/time.h>
static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
   BENCHMARK: CRT Bridge
   ═══════════════════════════════════════════════════════════════════════════ */

static void bench_crt(void)
{
    printf("══ CRT Bridge Address Decomposition ══\n");
    const uint32_t N = 20736;
    const int ITERS = 100;
    volatile uint32_t sink = 0;

    /* Linear scan: O(N) per lookup */
    double t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t flat = 0; flat < N; flat++) {
            /* Simulate linear scan: find cube and subcell by brute force */
            uint32_t cube = 0, subcell = 0;
            uint32_t acc = 0;
            for (uint32_t c = 0; c < 256; c++) {
                for (uint32_t s = 0; s < 81; s++) {
                    if (acc == flat) { cube = c; subcell = s; goto found_linear; }
                    acc++;
                }
            }
            found_linear:
            sink += cube + subcell;
        }
    }
    double t_linear = (now_sec() - t0) / ITERS;

    /* CRT seek: O(337) per lookup */
    t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t flat = 0; flat < N; flat++) {
            CRTSeekResult r = crt_seek(flat);
            sink += r.cube + r.subcell;
        }
    }
    double t_crt = (now_sec() - t0) / ITERS;

    /* CRT encode: O(1) per lookup */
    t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t flat = 0; flat < N; flat++) {
            CRTSeekResult r = crt_seek(flat);
            uint32_t back = crt_encode(r.cube, r.subcell);
            sink += back;
        }
    }
    double t_encode = (now_sec() - t0) / ITERS;

    printf("  Linear scan:  %.3f ms/call  (O(20736))\n", t_linear * 1000);
    printf("  CRT seek:     %.3f ms/call  (O(337))\n", t_crt * 1000);
    printf("  CRT encode:   %.3f ms/call  (O(1))\n", t_encode * 1000);
    printf("  Speedup:      %.1fx\n", t_linear / t_crt);
    printf("  Roundtrip:    %s\n", crt_verify() == 0 ? "LOSSLESS" : "FAIL");
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   BENCHMARK: Gosper Locality
   ═══════════════════════════════════════════════════════════════════════════ */

static void bench_gosper(void)
{
    printf("══ Gosper Curve Locality ══\n");

    for (uint32_t n = 1; n <= 3; n++) {
        uint64_t cells = gosper_cell_count(n);
        GosperStats gs = gosper_stats(n);

        /* Roundtrip check */
        uint64_t rt_ok = 0;
        for (uint64_t i = 0; i < cells; i++) {
            HexCoord pos = gosper_at(n, i);
            int64_t idx = gosper_index(n, pos);
            if (idx >= 0 && (uint64_t)idx == i) rt_ok++;
        }

        printf("  Level %u: %llu cells, radius %d, locality %.3f, roundtrip %llu/%llu\n",
               n, (unsigned long long)cells, gs.radius, gs.avg_locality,
               (unsigned long long)rt_ok, (unsigned long long)cells);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   BENCHMARK: FCC DRamTile
   ═══════════════════════════════════════════════════════════════════════════ */

static void bench_fcc(void)
{
    printf("══ FCC DRamTile Neighbor Access ══\n");
    const uint32_t N = 20736;
    const int ITERS = 100;
    volatile uint32_t sink = 0;

    /* Neighbor access: 12 directions × 20736 cells */
    double t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t f = 0; f < N; f++) {
            for (uint32_t d = 0; d < 12; d++) {
                uint32_t nbr = fcc_neighbor_flat(f, d);
                sink += nbr;
            }
        }
    }
    double t_neighbor = (now_sec() - t0) / ITERS;

    /* Flat→FCC conversion */
    t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t f = 0; f < N; f++) {
            FCCCoord c = flat_to_fcc(f);
            sink += c.x + c.y + c.z;
        }
    }
    double t_convert = (now_sec() - t0) / ITERS;

    FCCStats fs = fcc_verify();
    printf("  Flat→FCC:     %.3f ms/call  (20736 conversions)\n", t_convert * 1000);
    printf("  Neighbor:     %.3f ms/call  (20736 × 12 neighbors)\n", t_neighbor * 1000);
    printf("  Per-neighbor: %.1f ns\n", t_neighbor * 1e9 / (N * 12));
    printf("  FCC sites:    %u / %u = %u%%\n", fs.valid_count, fs.total_positions, fs.density_pct);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   BENCHMARK: Robinson Tiles
   ═══════════════════════════════════════════════════════════════════════════ */

static void bench_robinson(void)
{
    printf("══ Robinson Tile Assignment ══\n");
    const uint32_t N = 20736;
    const int ITERS = 100;
    volatile uint32_t sink = 0;

    /* Tile type assignment */
    double t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t f = 0; f < N; f++) {
            uint32_t x = f % 144;
            uint32_t y = f / 144;
            uint32_t t = rob_tile_type(x, y);
            sink += t;
        }
    }
    double t_type = (now_sec() - t0) / ITERS;

    /* Edge color computation */
    t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t f = 0; f < N; f++) {
            uint32_t x = f % 144;
            uint32_t y = f / 144;
            for (uint32_t e = 0; e < 4; e++) {
                uint8_t c = rob_edge_color(x, y, e);
                sink += c;
            }
        }
    }
    double t_edge = (now_sec() - t0) / ITERS;

    /* Type distribution */
    uint32_t types[4] = {0, 0, 0, 0};
    for (uint32_t f = 0; f < N; f++) {
        types[rob_tile_type(f % 144, f / 144)]++;
    }

    printf("  Tile type:    %.3f ms/call  (20736 assignments)\n", t_type * 1000);
    printf("  Edge color:   %.3f ms/call  (20736 × 4 edges)\n", t_edge * 1000);
    printf("  Per-tile:     %.1f ns\n", t_type * 1e9 / N);
    printf("  Distribution: A=%u B=%u C=%u D=%u\n", types[0], types[1], types[2], types[3]);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   BENCHMARK: Cross-System Integration
   ═══════════════════════════════════════════════════════════════════════════ */

static void bench_cross_system(void)
{
    printf("══ Cross-System Integration ══\n");
    const uint32_t N = 20736;
    const int ITERS = 100;
    volatile uint32_t sink = 0;

    /* Full pipeline: flat → CRT → FCC → Robinson → flat */
    double t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t f = 0; f < N; f++) {
            CRTSeekResult crt = crt_seek(f);
            FCCCoord fcc = flat_to_fcc(f);
            uint32_t rob = rob_tile_type(f % 144, f / 144);
            uint32_t back = crt_encode(crt.cube, crt.subcell);
            sink += back + fcc.x + rob;
        }
    }
    double t_pipeline = (now_sec() - t0) / ITERS;

    /* Twin rebalance: flat → hard → nat → flat */
    t0 = now_sec();
    for (int iter = 0; iter < ITERS; iter++) {
        for (uint32_t f = 0; f < N; f++) {
            TW_HardAddr h = tw_flat_to_hard(f);
            TW_NatAddr n = tw_hard_to_nat(h);
            uint32_t back = tw_nat_to_flat(n);
            sink += back;
        }
    }
    double t_twin = (now_sec() - t0) / ITERS;

    printf("  Full pipeline:  %.3f ms/call  (CRT+FCC+Robinson)\n", t_pipeline * 1000);
    printf("  Twin rebalance: %.3f ms/call  (flat→hard→nat→flat)\n", t_twin * 1000);
    printf("  Per-address:    %.1f ns (pipeline), %.1f ns (twin)\n",
           t_pipeline * 1e9 / N, t_twin * 1e9 / N);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Hex-Quad-Dual Upgrade Benchmark\n");
    printf("  System: 20736 address space, all upgrades #1-6\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");

    bench_crt();
    bench_gosper();
    bench_fcc();
    bench_robinson();
    bench_cross_system();

    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Benchmark complete.\n");
    printf("═══════════════════════════════════════════════════════════════════\n");

    return 0;
}
