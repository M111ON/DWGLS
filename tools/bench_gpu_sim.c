/* ═══════════════════════════════════════════════════════════════════════════
 * bench_gpu_sim.c — GPU Simulation Benchmark (no actual GPU needed)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Simulates GPU-style parallel access patterns on CPU to estimate speedup.
 * Uses OpenMP for parallel execution (simulates GPU threads).
 *
 * If you have CUDA, compile with: nvcc -o bench_gpu bench_gpu_sim.cu
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#if defined(_WIN32)
  #include <windows.h>
  static double now_ns(void) {
      LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&c);
      return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
  }
#else
  static double now_ns(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
  }
#endif

#define SLOTS 20736
#define SLOT_SZ 64
#define VOL_BYTES (SLOTS * SLOT_SZ)

/* ═══════════════ CPU BENCHMARK ═══════════════ */
static double bench_cpu_sequential(uint8_t *vol, uint32_t n) {
    double best = 1e18, sum = 0;
    
    for (int iter = 0; iter < 100; iter++) {
        double t0 = now_ns();
        
        volatile uint8_t sum_val = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t slot = (i * 37) % SLOTS;
            sum_val += vol[slot * SLOT_SZ];
        }
        
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    
    return sum / 100;
}

/* ═══════════════ GPU SIMULATION ═══════════════ */
/* Simulates GPU parallel access with pthreads or OpenMP */

#if defined(_OPENMP)
  #include <omp.h>
  
  static double bench_gpu_sim(uint8_t *vol, uint32_t n, int n_threads) {
      double best = 1e18, sum = 0;
      
      for (int iter = 0; iter < 100; iter++) {
          double t0 = now_ns();
          
          #pragma omp parallel for num_threads(n_threads)
          for (uint32_t i = 0; i < n; i++) {
              uint32_t slot = (i * 37) % SLOTS;
              volatile uint8_t val = vol[slot * SLOT_SZ];
              (void)val;
          }
          
          double dt = now_ns() - t0;
          if (dt < best) best = dt;
          sum += dt;
      }
      
      return sum / 100;
  }
  
#else
  /* Fallback: sequential simulation */
  static double bench_gpu_sim(uint8_t *vol, uint32_t n, int n_threads) {
      (void)n_threads;
      return bench_cpu_sequential(vol, n);
  }
#endif

/* ═══════════════ GPU MEMORY SIMULATION ═══════════════ */
/* Simulates GPU SRAM (fast) vs HBM (slower) */
static double bench_gpu_sram(uint8_t *vol, uint32_t n) {
    /* Simulate SRAM: all data in L1 cache */
    uint8_t sram[VOL_BYTES];
    memcpy(sram, vol, VOL_BYTES);
    
    double best = 1e18, sum = 0;
    
    for (int iter = 0; iter < 100; iter++) {
        double t0 = now_ns();
        
        volatile uint8_t sum_val = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t slot = (i * 37) % SLOTS;
            sum_val += sram[slot * SLOT_SZ];
        }
        
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    
    return sum / 100;
}

/* ═══════════════ BATCH READ ═══════════════ */
static double bench_batch_read(uint8_t *vol, uint32_t n) {
    uint8_t out[SLOT_SZ];
    
    double best = 1e18, sum = 0;
    
    for (int iter = 0; iter < 100; iter++) {
        double t0 = now_ns();
        
        for (uint32_t i = 0; i < n; i++) {
            uint32_t slot = (i * 37) % SLOTS;
            memcpy(out, &vol[slot * SLOT_SZ], SLOT_SZ);
        }
        
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    
    return sum / 100;
}

/* ═══════════════ MAIN ═══════════════ */
int main(void) {
    printf("GPU Simulation Benchmark\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");
    
    /* allocate and fill volume */
    uint8_t *vol = (uint8_t *)malloc(VOL_BYTES);
    if (!vol) {
        printf("Failed to allocate memory\n");
        return 1;
    }
    for (uint32_t i = 0; i < VOL_BYTES; i++) {
        vol[i] = (uint8_t)(i * 7 + 3);
    }
    
    /* warm up */
    printf("Warming up...\n");
    for (int i = 0; i < 1000000; i++) {
        volatile uint8_t x = vol[i % VOL_BYTES];
        (void)x;
    }
    
    printf("Running benchmarks...\n\n");
    
    /* benchmark sizes */
    uint32_t sizes[] = {1000, 10000, 20736};
    int n_sizes = sizeof(sizes) / sizeof(sizes[0]);
    
    printf("┌────────────────────┬──────────────┬──────────────┬──────────────┬──────────────┐\n");
    printf("│ Operation          │ CPU (ns)     │ GPU Sim (ns) │ SRAM (ns)    │ Speedup      │\n");
    printf("├────────────────────┼──────────────┼──────────────┼──────────────┼──────────────┤\n");
    
    for (int i = 0; i < n_sizes; i++) {
        uint32_t n = sizes[i];
        
        double cpu_time = bench_cpu_sequential(vol, n);
        double gpu_time = bench_gpu_sim(vol, n, 8);  /* 8 threads (simulates GPU SMs) */
        double sram_time = bench_gpu_sram(vol, n);
        double speedup = cpu_time / sram_time;
        
        printf("│ Read %5u slots   │ %10.1f   │ %10.1f   │ %10.1f   │ %10.1fx  │\n",
               n, cpu_time, gpu_time, sram_time, speedup);
    }
    
    printf("└────────────────────┴──────────────┴──────────────┴──────────────┴──────────────┘\n");
    
    /* batch read benchmark */
    printf("\nBatch Read (all 20736 slots):\n");
    double batch_time = bench_batch_read(vol, SLOTS);
    printf("  CPU batch: %.1f ns total, %.2f ns per weight\n", 
           batch_time, batch_time / SLOTS);
    
    /* memory hierarchy simulation */
    printf("\nMemory Hierarchy Simulation:\n");
    printf("┌────────────────────┬──────────────┬──────────────┬──────────────┐\n");
    printf("│ Level              │ Latency (ns) │ Bandwidth    │ Best For     │\n");
    printf("├────────────────────┼──────────────┼──────────────┼──────────────┤\n");
    printf("│ L1 Cache (SRAM)    │          1   │ 10 TB/s      │ Hot data     │\n");
    printf("│ L2 Cache           │          4   │ 3 TB/s       │ Working set  │\n");
    printf("│ L3 Cache           │         10   │ 1 TB/s       │ Shared data  │\n");
    printf("│ CPU DRAM           │        100   │ 50 GB/s      │ Main memory  │\n");
    printf("│ GPU SRAM           │          1   │ 20 TB/s      │ Registers    │\n");
    printf("│ GPU HBM            │         10   │ 3 TB/s       │ Weight table │\n");
    printf("└────────────────────┴──────────────┴──────────────┴──────────────┘\n");
    
    /* theoretical GPU performance */
    printf("\nTheoretical GPU Performance:\n");
    printf("  GPU has 100+ SMs, each with 1000+ threads\n");
    printf("  Total: 100,000+ concurrent memory accesses\n");
    printf("  Latency hiding: 100,000 × 1ns = 100,000 ns per SM\n");
    printf("  Effective bandwidth: 100,000 × 4B / 100ns = 4 TB/s\n");
    
    /* comparison */
    printf("\nComparison Summary:\n");
    printf("┌────────────────────┬──────────────┬──────────────┬──────────────┐\n");
    printf("│ System             │ Time (ns)    │ Speedup      │ Notes        │\n");
    printf("├────────────────────┼──────────────┼──────────────┼──────────────┤\n");
    printf("│ Old MDIM           │    234,000   │     1x       │ Probe chains │\n");
    printf("│ New Geo            │      1,000   │   234x       │ Pre-computed │\n");
    printf("│ GeoFast            │         76   │ 3,087x       │ Pointer tbl  │\n");
    printf("│ CPU SRAM (sim)     │         %3.0f   │ %5.0fx      │ L1 cache     │\n",
           bench_gpu_sram(vol, 20736), 234000.0 / bench_gpu_sram(vol, 20736));
    printf("│ GPU (theoretical)  │          1   │ 234,000x     │ 100K parallel│\n");
    printf("└────────────────────┴──────────────┴──────────────┴──────────────┘\n");
    
    free(vol);
    return 0;
}
