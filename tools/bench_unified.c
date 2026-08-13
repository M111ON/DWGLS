/* ═══════════════════════════════════════════════════════════════════════════
 * bench_unified.c — Unified Geometric FS Benchmark
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geo_unified.h"

#if defined(_WIN32)
  #include <windows.h>
  static double now_ns(void) {
      LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&c);
      return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
  }
#else
  #include <time.h>
  static double now_ns(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
  }
#endif

#define BENCH_ITERS 100000

int main(void) {
    printf("Unified Geometric FS Benchmark (geo_fast + DRamTile + RDH + GearLock)\n");
    printf("═══════════════════════════════════════════════════════════════════════\n\n");
    
    /* warm up */
    printf("Warming up...\n");
    for (int i = 0; i < 1000000; i++) {
        volatile uint8_t x = (uint8_t)i;
        (void)x;
    }
    
    printf("Running benchmarks...\n\n");
    
    /* init volume */
    GeoUnifiedVolume v;
    geo_unified_init(&v);
    
    /* fill with test data */
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 7 + 3);
    
    /* create files */
    for (int i = 0; i < 100; i++) {
        char name[24];
        snprintf(name, sizeof(name), "f%04d.bin", i);
        geo_unified_create(&v, name, data, 64);
    }
    
    /* benchmark: name lookup */
    double best = 1e18, sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        char name[24];
        snprintf(name, sizeof(name), "f%04d.bin", i % 100);
        double t0 = now_ns();
        volatile void *ptr = geo_unified_read(&v, name);
        double dt = now_ns() - t0;
        (void)ptr;
        if (dt < best) best = dt;
        sum += dt;
    }
    printf("Name lookup:     %8.1f ns (best: %.1f ns)\n", sum / BENCH_ITERS, best);
    
    /* benchmark: flat index */
    best = 1e18; sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        double t0 = now_ns();
        volatile void *ptr = geo_unified_read_flat(&v, i % GEO_UNIFIED_SLOTS);
        double dt = now_ns() - t0;
        (void)ptr;
        if (dt < best) best = dt;
        sum += dt;
    }
    printf("Flat index:      %8.1f ns (best: %.1f ns)\n", sum / BENCH_ITERS, best);
    
    /* benchmark: DRamTile coordinates */
    best = 1e18; sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        uint32_t anchor = i % DRAM_ANCHORS;
        uint32_t x = i % DRAM_GRID_X;
        uint32_t y = (i / DRAM_GRID_X) % DRAM_GRID_Y;
        uint32_t layer = i % DRAM_LAYERS;
        double t0 = now_ns();
        volatile void *ptr = geo_unified_read_dram(&v, anchor, x, y, layer);
        double dt = now_ns() - t0;
        (void)ptr;
        if (dt < best) best = dt;
        sum += dt;
    }
    printf("DRAM coordinates: %7.1f ns (best: %.1f ns)\n", sum / BENCH_ITERS, best);
    
    /* benchmark: RDH coordinates */
    best = 1e18; sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        int64_t ring = i % 128;
        int64_t wedge = i % 162;
        double t0 = now_ns();
        volatile void *ptr = geo_unified_read_rdh(&v, &v.rdh_cfg, ring, wedge, 0, 0);
        double dt = now_ns() - t0;
        (void)ptr;
        if (dt < best) best = dt;
        sum += dt;
    }
    printf("RDH coordinates:  %7.1f ns (best: %.1f ns)\n", sum / BENCH_ITERS, best);
    
    /* benchmark: GearLock */
    best = 1e18; sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        double t0 = now_ns();
        uint32_t flat = geo_unified_from_gear(&v.gear);
        gear_cpu_tick(&v.gear);
        double dt = now_ns() - t0;
        (void)flat;
        if (dt < best) best = dt;
        sum += dt;
    }
    printf("GearLock tick:   %7.1f ns (best: %.1f ns)\n", sum / BENCH_ITERS, best);
    
    /* benchmark: batch read (all 20736 slots) */
    uint32_t flats[GEO_UNIFIED_SLOTS];
    void *ptrs[GEO_UNIFIED_SLOTS];
    for (uint32_t i = 0; i < GEO_UNIFIED_SLOTS; i++) flats[i] = i;
    
    best = 1e18; sum = 0;
    for (int i = 0; i < 1000; i++) {
        double t0 = now_ns();
        geo_unified_read_batch(&v, flats, GEO_UNIFIED_SLOTS, ptrs);
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    printf("Batch (20736):    %7.1f ns total, %.2f ns/weight\n", 
           sum / 1000, (sum / 1000) / GEO_UNIFIED_SLOTS);
    
    /* verification */
    printf("\nVerification:\n");
    if (geo_unified_verify(&v)) {
        printf("  ✅ All systems verified\n");
    } else {
        printf("  ❌ Verification failed\n");
    }
    printf("  Files: %u\n", v.n_files);
    
    /* comparison */
    printf("\nComparison:\n");
    printf("┌─────────────────────────┬────────────────┬────────────────┐\n");
    printf("│ System                  │ Time (ns)      │ Speedup        │\n");
    printf("├─────────────────────────┼────────────────┼────────────────┤\n");
    printf("│ Old MDIM (probe chain)  │    234,000     │     1x         │\n");
    printf("│ GeoFast (pointer table) │         76     │ 3,087x         │\n");
    printf("│ Unified (DRamTile+RDH)  │         %3.0f    │ %6.0fx        │\n",
           sum / (1000 * GEO_UNIFIED_SLOTS), 234000.0 / (sum / (1000 * GEO_UNIFIED_SLOTS)));
    printf("└─────────────────────────┴────────────────┴────────────────┘\n");
    
    geo_unified_free(&v);
    return 0;
}
