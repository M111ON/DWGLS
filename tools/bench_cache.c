/* ═══════════════════════════════════════════════════════════════════════════
 * bench_cache.c — Cache Miss Analysis
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

/* Sequential access (cache-friendly) */
static double bench_sequential(GeoUnifiedVolume *v) {
    double best = 1e18, sum = 0;
    
    for (int iter = 0; iter < 100; iter++) {
        double t0 = now_ns();
        
        volatile uint8_t val = 0;
        for (uint32_t i = 0; i < GEO_UNIFIED_SLOTS; i++) {
            val += ((uint8_t *)v->slot_ptrs[i])[0];
        }
        
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    
    return sum / 100;
}

/* Random access (cache-hostile) */
static double bench_random(GeoUnifiedVolume *v) {
    double best = 1e18, sum = 0;
    
    /* pre-generate random indices */
    uint32_t indices[BENCH_ITERS];
    for (int i = 0; i < BENCH_ITERS; i++) {
        indices[i] = (i * 37) % GEO_UNIFIED_SLOTS;  /* pseudo-random */
    }
    
    for (int iter = 0; iter < 100; iter++) {
        double t0 = now_ns();
        
        volatile uint8_t val = 0;
        for (int i = 0; i < BENCH_ITERS; i++) {
            val += ((uint8_t *)v->slot_ptrs[indices[i]])[0];
        }
        
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    
    return sum / 100;
}

/* Stride access (cache test) */
static double bench_stride(GeoUnifiedVolume *v, uint32_t stride) {
    double best = 1e18, sum = 0;
    
    for (int iter = 0; iter < 100; iter++) {
        double t0 = now_ns();
        
        volatile uint8_t val = 0;
        for (uint32_t i = 0; i < GEO_UNIFIED_SLOTS; i += stride) {
            val += ((uint8_t *)v->slot_ptrs[i])[0];
        }
        
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    
    return sum / 100;
}

/* Cold cache access (first access after init) */
static double bench_cold(void) {
    double best = 1e18, sum = 0;
    
    for (int iter = 0; iter < 1000; iter++) {
        /* create fresh volume (cold cache) */
        GeoUnifiedVolume v;
        geo_unified_init(&v);
        
        uint8_t data[64] = {0};
        for (int i = 0; i < 10; i++) {
            char name[24];
            snprintf(name, sizeof(name), "f%04d.bin", i);
            geo_unified_create(&v, name, data, 64);
        }
        
        double t0 = now_ns();
        
        /* read all 10 files */
        volatile uint8_t val = 0;
        for (int i = 0; i < 10; i++) {
            char name[24];
            snprintf(name, sizeof(name), "f%04d.bin", i);
            void *ptr = geo_unified_read(&v, name);
            if (ptr) val += ((uint8_t *)ptr)[0];
        }
        
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
        
        geo_unified_free(&v);
    }
    
    return sum / 1000;
}

int main(void) {
    printf("Cache Miss Analysis\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");
    
    /* init volume */
    GeoUnifiedVolume v;
    geo_unified_init(&v);
    
    /* fill with data */
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 7 + 3);
    for (int i = 0; i < 100; i++) {
        char name[24];
        snprintf(name, sizeof(name), "f%04d.bin", i);
        geo_unified_create(&v, name, data, 64);
    }
    
    /* warm up cache */
    printf("Warming up cache...\n");
    for (int i = 0; i < 1000000; i++) {
        volatile uint8_t x = ((uint8_t *)v.slot_ptrs[i % GEO_UNIFIED_SLOTS])[0];
        (void)x;
    }
    
    printf("Running benchmarks...\n\n");
    
    /* sequential access */
    double seq_time = bench_sequential(&v);
    printf("Sequential access:    %8.1f ns/slot\n", seq_time / GEO_UNIFIED_SLOTS);
    
    /* random access */
    double rand_time = bench_random(&v);
    printf("Random access:        %8.1f ns/slot\n", rand_time / BENCH_ITERS);
    
    /* stride access */
    printf("\nStride access:\n");
    uint32_t strides[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    int n_strides = sizeof(strides) / sizeof(strides[0]);
    
    for (int i = 0; i < n_strides; i++) {
        double stride_time = bench_stride(&v, strides[i]);
        uint32_t n_accesses = GEO_UNIFIED_SLOTS / strides[i];
        printf("  Stride %4u: %8.1f ns/access\n", strides[i], stride_time / n_accesses);
    }
    
    /* cold cache access */
    double cold_time = bench_cold();
    printf("\nCold cache (first access): %8.1f ns/file\n", cold_time / 10);
    
    /* cache size analysis */
    printf("\nCache Size Analysis:\n");
    printf("  Volume size: %u bytes (%.2f KB)\n", 
           GEO_UNIFIED_SLOTS * 64, 
           (GEO_UNIFIED_SLOTS * 64) / 1024.0);
    printf("  L1 cache: 32-64 KB (fits? %s)\n",
           (GEO_UNIFIED_SLOTS * 64) <= 64 * 1024 ? "YES" : "NO");
    printf("  L2 cache: 256 KB-1 MB (fits? %s)\n",
           (GEO_UNIFIED_SLOTS * 64) <= 1024 * 1024 ? "YES" : "NO");
    printf("  L3 cache: 8-32 MB (fits? %s)\n",
           (GEO_UNIFIED_SLOTS * 64) <= 32 * 1024 * 1024 ? "YES" : "NO");
    
    /* cache miss estimation */
    printf("\nCache Miss Estimation:\n");
    double l1_latency = 1.0;   /* ns */
    double l2_latency = 4.0;   /* ns */
    double l3_latency = 10.0;  /* ns */
    double ram_latency = 100.0; /* ns */
    
    printf("  If all L1 hits:   %8.1f ns total\n", GEO_UNIFIED_SLOTS * l1_latency);
    printf("  If all L2 hits:   %8.1f ns total\n", GEO_UNIFIED_SLOTS * l2_latency);
    printf("  If all L3 hits:   %8.1f ns total\n", GEO_UNIFIED_SLOTS * l3_latency);
    printf("  If all RAM:       %8.1f ns total\n", GEO_UNIFIED_SLOTS * ram_latency);
    
    printf("\nMeasured:          %8.1f ns total (sequential)\n", seq_time);
    
    /* determine cache level */
    if (seq_time < GEO_UNIFIED_SLOTS * l2_latency) {
        printf("  → Mostly L1/L2 hits\n");
    } else if (seq_time < GEO_UNIFIED_SLOTS * l3_latency) {
        printf("  → Mostly L2/L3 hits\n");
    } else if (seq_time < GEO_UNIFIED_SLOTS * ram_latency) {
        printf("  → Mostly L3 hits\n");
    } else {
        printf("  → Mostly RAM (cache misses!)\n");
    }
    
    geo_unified_free(&v);
    return 0;
}
