/*
 * test_bcr.c — Balanced Crystal Routing verification & benchmark
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <x86intrin.h>
#include "b_fs_base.h"
#include "bcr.h"

#define K 10

static uint32_t lcg(uint32_t*s){ *s=*s*1664525u+1013904223u; return *s;}

int main(void) {
    static volatile uint32_t sink_bcr = 0;
    printf("=== Balanced Crystal Routing (b_fs) ===\n");

    /* Build from 0..20735 */
    BcrTree tree;
    static uint16_t addrs[BCR_N];
    for (uint32_t i = 0; i < BCR_N; i++) addrs[i] = (uint16_t)i;

    unsigned int aux = 0;
    uint64_t t0 = __rdtscp(&aux);
    bcr_build(&tree, addrs, BCR_N);
    uint64_t t1 = __rdtscp(&aux);
    double build_ms = (double)(t1 - t0) / 3e6; /* ~3GHz */

    printf("Build: count=%u height=%d (limit ~20) time=%.2f ms\n",
           tree.count, bcr_height(&tree), build_ms);

    /* Correctness: all addresses present */
    int ok = (tree.count == BCR_N);
    for (uint32_t i = 0; i < BCR_N && ok; i++) {
        if (bcr_search(&tree, (uint16_t)i) == BCR_SENTINEL) { ok = 0; break; }
    }
    /* cycle check */
    int cyc = bcr_has_cycle(&tree);
    if (cyc) ok = 0;
    printf("Correctness gate: %s  Cycle: %s\n", ok ? "PASS" : "FAIL", cyc ? "YES (FAIL)" : "NO");

    /* Balance gate */
    int h = bcr_height(&tree);
    int bal_ok = (h <= 20);
    printf("Balance gate: height=%d %s\n", h, bal_ok ? "PASS" : "FAIL");

    /* Range search test */
    uint16_t out[256];
    uint32_t rc = bcr_range_search(&tree, 100, 200, out, 256);
    printf("Range [100,200]: %u results (expect 101)\n", rc);

    /* Performance: search latency */
    uint32_t queries[1000];
    uint32_t seed = 0x999u;
    for (int i = 0; i < 1000; i++) queries[i] = lcg(&seed) % BCR_N;

    /* Flat scan top-k (min dist to query? here search exact addr; use bcr_search) */
    /* Benchmark bcr_search exact */
    t0 = __rdtscp(&aux);
    for (int it = 0; it < 5; it++)
        for (int i = 0; i < 1000; i++) {
            uint16_t r = bcr_search(&tree, (uint16_t)queries[i]);
            sink_bcr += r;
        }
    t1 = __rdtscp(&aux);
    double cyc_bcr = (double)(t1 - t0) / 5000.0;

    /* Flat: linear scan find addr (real work) */
    t0 = __rdtscp(&aux);
    for (int it = 0; it < 5; it++)
        for (int i = 0; i < 1000; i++) {
            uint32_t q = queries[i];
            uint32_t found = BCR_N;
            for (uint32_t a = 0; a < BCR_N; a++) {
                if (a == q) { found = a; break; }
            }
            sink_bcr += found;
        }
    t1 = __rdtscp(&aux);
    double cyc_flat = (double)(t1 - t0) / 5000.0;

    printf("Latency (cycles/query, exact search):\n");
    printf("  Flat scan : %.1f\n", cyc_flat);
    printf("  BCR search: %.1f\n", cyc_bcr);
    printf("  Speedup   : %.2fx %s\n", cyc_flat / cyc_bcr,
           (cyc_flat / cyc_bcr >= 10.0) ? "PASS" : "FAIL");

    /* Integration: b_fs TESS dimension lossless check */
    /* Insert all addresses into tree already; verify search returns correct data_addr */
    int lossless = 1;
    for (uint32_t i = 0; i < BCR_N; i++) {
        uint16_t node = bcr_search(&tree, (uint16_t)i);
        if (node == BCR_SENTINEL || tree.nodes[node].data_addr != (uint16_t)i) { lossless = 0; break; }
    }
    printf("Integration (lossless addr roundtrip): %s\n", lossless ? "PASS" : "FAIL");

    return 0;
}