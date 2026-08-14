/*
 * bench_rdh.c — Benchmark RDH vs hash address computation
 *
 * Loads tensor names from a GGUF model, computes addresses
 * using both addr_from_tensor_name() (FNV-1a hash) and
 * addr_from_rdh_name() (RDH geometric formula), reports
 * latency and collision stats.
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -I../../runner -I../../collection -I../../collection/Hfolder
 *       -o bench_rdh.exe bench_rdh.c ../../runner/gguf_index.c -lm
 *
 * Usage:
 *   .\bench_rdh.exe I:\model\LFM2.5-1.2B-Instruct-Q4_K_M.gguf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include "rdh_addr.h"
#include "../../runner/addr_space.h"
#include "../../runner/gguf_index.h"

/* High-res timer */
static double perf_freq = 0.0;
static void perf_init(void) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); perf_freq = (double)f.QuadPart;
}
static double perf_us(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / perf_freq * 1e6;
}

int main(int argc, char **argv) {
    perf_init();
    if (argc < 2) {
        fprintf(stderr, "Usage: bench_rdh.exe model.gguf\n");
        return 1;
    }

    GGUFTensorIndex idx;
    memset(&idx, 0, sizeof(idx));
    if (gguf_idx_open(argv[1], &idx) != 0) {
        fprintf(stderr, "ERROR: can't open %s\n", argv[1]);
        return 1;
    }
    int N = (int)idx.n_tensors;
    printf("Model: %s\n", argv[1]);
    printf("Tensors: %d\n\n", N);

    /* ── Warmup + collision check ── */
    int hash_collisions = 0, rdh_collisions = 0;
    int hash_first[65536], rdh_first[65536];
    for (int i = 0; i < 65536; i++) hash_first[i] = rdh_first[i] = -1;

    for (int i = 0; i < N; i++) {
        uint32_t ha = addr_from_tensor_name(idx.names[i], 0);
        uint32_t ra = addr_from_rdh_name(idx.names[i], 0);
        if (ha < 65536) {
            if (hash_first[ha] >= 0) {
                hash_collisions++;
                if (hash_collisions <= 3)
                    printf("  HASH COLLISION: addr=%u \"%s\" vs \"%s\"\n",
                           ha, idx.names[hash_first[ha]], idx.names[i]);
            } else hash_first[ha] = i;
        }
        if (ra < 65536) {
            if (rdh_first[ra] >= 0) {
                rdh_collisions++;
                if (rdh_collisions <= 3)
                    printf("  RDH COLLISION:  addr=%u \"%s\" vs \"%s\"\n",
                           ra, idx.names[rdh_first[ra]], idx.names[i]);
            } else rdh_first[ra] = i;
        }
    }

    /* ── Latency benchmark (1000 iterations × N tensors) ── */
    const int ITER = 10000;
    volatile uint32_t sink = 0; /* prevent optimization away */

    double t0 = perf_us();
    for (int iter = 0; iter < ITER; iter++)
        for (int i = 0; i < N; i++)
            sink += addr_from_tensor_name(idx.names[i], 0);
    double t_hash = perf_us() - t0;

    t0 = perf_us();
    for (int iter = 0; iter < ITER; iter++)
        for (int i = 0; i < N; i++)
            sink += addr_from_rdh_name(idx.names[i], 0);
    double t_rdh = perf_us() - t0;

    double total_calls = (double)ITER * N;
    printf("═══════════════════════════════════════════\n");
    printf("  Method         Total (ms)  ns/addr  Addr/s\n");
    printf("───────────────────────────────────────────\n");
    printf("  FNV-1a Hash    %8.2f   %5.1f  %8.2e\n",
           t_hash / 1e3, t_hash / total_calls * 1e3, total_calls / (t_hash / 1e6));
    printf("  RDH Formula    %8.2f   %5.1f  %8.2e\n",
           t_rdh / 1e3, t_rdh / total_calls * 1e3, total_calls / (t_rdh / 1e6));
    printf("───────────────────────────────────────────\n");
    double ratio = t_hash / t_rdh;
    printf("  RDH speedup: %.2fx %s\n", ratio > 1.0 ? ratio : 1.0/ratio,
           ratio > 1.0 ? "faster" : "slower");

    printf("\n═══════════════════════════════════════════\n");
    printf("  Collisions (hash): %d / %d\n", hash_collisions, N);
    printf("  Collisions (rdh):  %d / %d\n", rdh_collisions, N);

    /* ── Stress test: 1M synthetic tensor names ── */
    const int STRESS_N = 1000000;
    char **names = (char **)malloc(STRESS_N * sizeof(char *));
    for (int i = 0; i < STRESS_N; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "blk.%d.attn_q.weight", i % 256);
        names[i] = _strdup(buf);
    }

    t0 = perf_us();
    for (int i = 0; i < STRESS_N; i++)
        sink += addr_from_tensor_name(names[i], 0);
    t_hash = perf_us() - t0;

    t0 = perf_us();
    for (int i = 0; i < STRESS_N; i++)
        sink += addr_from_rdh_name(names[i], 0);
    t_rdh = perf_us() - t0;

    printf("\n═══════════════════════════════════════════\n");
    printf("  Synthetic stress test (%d addresses):\n", STRESS_N);
    printf("───────────────────────────────────────────\n");
    printf("  FNV-1a Hash    %8.2f ms  %5.1f ns/addr\n",
           t_hash / 1e3, t_hash / STRESS_N * 1e3);
    printf("  RDH Formula    %8.2f ms  %5.1f ns/addr\n",
           t_rdh / 1e3, t_rdh / STRESS_N * 1e3);

    for (int i = 0; i < STRESS_N; i++) free(names[i]);
    free(names);
    gguf_idx_close(&idx);
    return 0;
}
