/* ═══════════════════════════════════════════════════════════════════════════
 * bench_geo_fast.c — Ultra-Fast Geometric FS: <10ns Benchmark
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geo_fast.h"

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

/* Benchmark functions */
typedef struct {
    double lookup_ns;
    double read_ns;
    double write_ns;
    double batch_ns;
    double batch_per_weight_ns;
} BenchResult;

static BenchResult bench_geo_fast(void) {
    BenchResult r = {0};
    GeoFastVolume v;
    geo_fast_init_malloc(&v);
    
    /* fill with test data */
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 7 + 3);
    
    /* create some files for testing */
    for (int i = 0; i < 100; i++) {
        char name[24];
        snprintf(name, sizeof(name), "f%04d.bin", i);
        geo_fast_create(&v, name, data, 64);
    }
    
    /* benchmark lookup */
    double best = 1e18, sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        char name[24];
        snprintf(name, sizeof(name), "f%04d.bin", i % 100);
        double t0 = now_ns();
        volatile void *ptr = geo_fast_read(&v, name);
        double dt = now_ns() - t0;
        (void)ptr;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.lookup_ns = sum / BENCH_ITERS;
    
    /* benchmark read (pointer dereference) */
    best = 1e18; sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        double t0 = now_ns();
        volatile void *ptr = geo_fast_read_idx(&v, i % GEO_FAST_SLOTS);
        double dt = now_ns() - t0;
        (void)ptr;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.read_ns = sum / BENCH_ITERS;
    
    /* benchmark write */
    best = 1e18; sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        double t0 = now_ns();
        geo_fast_write(&v, "f0000.bin", data, 64);
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.write_ns = sum / BENCH_ITERS;
    
    /* benchmark batch read */
    uint32_t indices[20736];
    void *ptrs[20736];
    for (uint32_t i = 0; i < GEO_FAST_SLOTS; i++) indices[i] = i;
    
    best = 1e18; sum = 0;
    for (int i = 0; i < 1000; i++) {
        double t0 = now_ns();
        geo_fast_read_batch(&v, indices, GEO_FAST_SLOTS, ptrs);
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.batch_ns = sum / 1000;
    r.batch_per_weight_ns = r.batch_ns / GEO_FAST_SLOTS;
    
    geo_fast_free(&v);
    return r;
}

/* Baseline: direct array access */
static double bench_direct_array(void) {
    uint8_t arr[GEO_FAST_SLOTS * 64];
    memset(arr, 0, sizeof(arr));
    
    double best = 1e18, sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        double t0 = now_ns();
        volatile uint8_t *p = &arr[(i % GEO_FAST_SLOTS) * 64];
        double dt = now_ns() - t0;
        (void)p;
        if (dt < best) best = dt;
        sum += dt;
    }
    return sum / BENCH_ITERS;
}

int main(void) {
    printf("Ultra-Fast Geometric FS: <10ns Benchmark\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");
    
    /* warm up */
    printf("Warming up...\n");
    for (int i = 0; i < 1000000; i++) {
        volatile uint8_t x = (uint8_t)i;
        (void)x;
    }
    
    /* run benchmarks */
    printf("Running benchmarks...\n\n");
    
    BenchResult geo = bench_geo_fast();
    double direct = bench_direct_array();
    
    printf("Results:\n");
    printf("┌─────────────────────────┬────────────────┬────────────────┐\n");
    printf("│ Operation               │ Time (ns)      │ Target (ns)    │\n");
    printf("├─────────────────────────┼────────────────┼────────────────┤\n");
    printf("│ Direct array access     │ %12.1f   │         N/A    │\n", direct);
    printf("│ GeoFast lookup          │ %12.1f   │         < 10   │\n", geo.lookup_ns);
    printf("│ GeoFast read            │ %12.1f   │         < 10   │\n", geo.read_ns);
    printf("│ GeoFast write           │ %12.1f   │         < 10   │\n", geo.write_ns);
    printf("│ GeoFast batch (total)   │ %12.1f   │         N/A    │\n", geo.batch_ns);
    printf("│ GeoFast batch (per wt)  │ %12.1f   │          < 1   │\n", geo.batch_per_weight_ns);
    printf("└─────────────────────────┴────────────────┴────────────────┘\n");
    
    /* verification */
    printf("\nVerification:\n");
    {
        GeoFastVolume v;
        geo_fast_init_malloc(&v);
        
        uint8_t data[64] = {0};
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)i;
        
        /* create and read back */
        for (int i = 0; i < 10; i++) {
            char name[24];
            snprintf(name, sizeof(name), "test%d.bin", i);
            geo_fast_create(&v, name, data, 64);
        }
        
        /* verify reads */
        int errors = 0;
        for (int i = 0; i < 10; i++) {
            char name[24];
            snprintf(name, sizeof(name), "test%d.bin", i);
            void *ptr = geo_fast_read(&v, name);
            if (!ptr || memcmp(ptr, data, 64) != 0) {
                printf("  ❌ Read mismatch for %s\n", name);
                errors++;
            }
        }
        
        /* verify pointer table */
        if (geo_fast_verify(&v)) {
            printf("  ✅ Pointer table verified correctly\n");
        } else {
            printf("  ❌ Pointer table verification failed\n");
            errors++;
        }
        
        printf("  Files: %u\n", v.n_files);
        
        geo_fast_free(&v);
        
        if (errors == 0) {
            printf("  ✅ All tests passed\n");
        } else {
            printf("  ❌ %d tests failed\n", errors);
        }
    }
    
    /* comparison with old systems */
    printf("\nComparison with old systems:\n");
    printf("┌─────────────────────────┬────────────────┬────────────────┐\n");
    printf("│ System                  │ Time (ns)      │ Speedup        │\n");
    printf("├─────────────────────────┼────────────────┼────────────────┤\n");
    printf("│ Old MDIM (probe chain)  │    234,000     │     1x (base)  │\n");
    printf("│ New Geo (pre-computed)  │          1,000 │   234x         │\n");
    printf("│ GeoFast (pointer table) │         %5.0f   │ %6.0fx        │\n", 
           geo.lookup_ns, 234000.0 / geo.lookup_ns);
    printf("│ Direct array (baseline) │         %5.0f   │ %6.0fx        │\n",
           direct, 234000.0 / direct);
    printf("└─────────────────────────┴────────────────┴────────────────┘\n");
    
    printf("\nConclusion:\n");
    if (geo.lookup_ns < 10) {
        printf("✅ TARGET ACHIEVED: Lookup < 10ns\n");
    } else {
        printf("❌ TARGET NOT MET: Lookup = %.1f ns (need < 10)\n", geo.lookup_ns);
    }
    
    if (geo.batch_per_weight_ns < 1) {
        printf("✅ TARGET ACHIEVED: Batch < 1ns per weight\n");
    } else {
        printf("❌ TARGET NOT MET: Batch = %.1f ns/weight (need < 1)\n", geo.batch_per_weight_ns);
    }
    
    return 0;
}
