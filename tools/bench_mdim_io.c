/* ═══════════════════════════════════════════════════════════════════════════
 * bench_mdim_io.c — GeoFS MDIM file I/O benchmark
 * ═══════════════════════════════════════════════════════════════════════════
 * Measures: create, read, write, delete, storage efficiency
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geofs_mdim.h"

#if defined(_WIN32)
  #include <windows.h>
  static double now_us(void) {
      LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&c);
      return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
  }
#else
  #include <time.h>
  static double now_us(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
  }
#endif

#define BENCH_ITERS 1000

typedef struct {
    double create_us;
    double read_us;
    double write_us;
    double delete_us;
    uint32_t file_size;
    uint32_t slots_used;
    double efficiency;  /* data bytes / (slots_used * 64) */
} BenchResult;

static BenchResult bench_file_ops(uint32_t size, int n_files) {
    BenchResult r = {0};
    r.file_size = size;

    MdimVolume v;
    mdim_volume_init(&v, NULL);

    char name[24];
    uint8_t *data = (uint8_t *)malloc(size);
    uint8_t *readbuf = (uint8_t *)malloc(size);
    for (uint32_t i = 0; i < size; i++) data[i] = (uint8_t)(i * 7 + 3);

    /* Benchmark: CREATE */
    double best = 1e18, sum = 0;
    for (int i = 0; i < BENCH_ITERS && i < n_files; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        int err = MDIM_OK;
        double t0 = now_us();
        mdim_summon(&v, name, data, size, &err);
        double dt = now_us() - t0;
        if (err != MDIM_OK) { printf("create err %d\n", err); break; }
        if (dt < best) best = dt;
        sum += dt;
    }
    r.create_us = (best < 1e18) ? sum / (BENCH_ITERS < n_files ? BENCH_ITERS : n_files) : 0;

    /* Benchmark: READ */
    best = 1e18; sum = 0;
    uint32_t actual = 0;
    int read_count = (BENCH_ITERS < n_files) ? BENCH_ITERS : n_files;
    for (int i = 0; i < read_count; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        MdimFile f;
        if (mdim_open(&v, name, &f) != MDIM_OK) continue;
        double t0 = now_us();
        mdim_read(&v, &f, readbuf, size, &actual);
        double dt = now_us() - t0;
        if (actual != size) { printf("read size err\n"); break; }
        if (dt < best) best = dt;
        sum += dt;
    }
    r.read_us = (best < 1e18) ? sum / read_count : 0;

    /* Benchmark: WRITE (update existing) */
    best = 1e18; sum = 0;
    int write_count = (BENCH_ITERS < n_files) ? BENCH_ITERS : n_files;
    for (int i = 0; i < write_count; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        double t0 = now_us();
        mdim_write(&v, name, data, size);
        double dt = now_us() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.write_us = (best < 1e18) ? sum / write_count : 0;

    /* Benchmark: DELETE */
    best = 1e18; sum = 0;
    int del_count = (BENCH_ITERS < n_files) ? BENCH_ITERS : n_files;
    for (int i = 0; i < del_count; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        double t0 = now_us();
        mdim_unsummon(&v, name);
        double dt = now_us() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.delete_us = (best < 1e18) ? sum / del_count : 0;

    /* Storage efficiency */
    r.slots_used = v.n_blocks_used;
    uint32_t overhead = r.slots_used * MDIM_SLOT_SZ;
    r.efficiency = (overhead > 0) ? (double)(n_files * size) / overhead * 100.0 : 0;

    free(data);
    free(readbuf);
    mdim_volume_free(&v);
    return r;
}

int main(void) {
    printf("GeoFS MDIM — file I/O benchmark\n");
    printf("Volume: %.1f MB (%u slots × %u B)\n\n", MDIM_VOL_BYTES / 1048576.0, MDIM_SLOTS, MDIM_SLOT_SZ);

    struct { uint32_t size; int count; const char *label; } tests[] = {
        {     1,  100, "1 B × 100 files" },
        {    63,   50, "63 B × 50 files" },
        {   128,   30, "128 B × 30 files" },
        {  1024,   20, "1 KB × 20 files" },
        {  4096,   10, "4 KB × 10 files" },
        { 16384,    5, "16 KB × 5 files" },
        { 65536,    2, "64 KB × 2 files" },
    };
    int n_tests = sizeof(tests) / sizeof(tests[0]);

    printf("┌─────────────────────────┬──────────┬──────────┬──────────┬──────────┬──────────┐\n");
    printf("│ Test                    │ Create   │ Read     │ Write    │ Delete   │ Efficiency│\n");
    printf("├─────────────────────────┼──────────┼──────────┼──────────┼──────────┼──────────┤\n");

    for (int i = 0; i < n_tests; i++) {
        BenchResult r = bench_file_ops(tests[i].size, tests[i].count);
        printf("│ %-23s │ %6.1f µs│ %6.1f µs│ %6.1f µs│ %6.1f µs│ %6.1f%%   │\n",
               tests[i].label, r.create_us, r.read_us, r.write_us, r.delete_us, r.efficiency);
    }

    printf("└─────────────────────────┴──────────┴──────────┴──────────┴──────────┴──────────┘\n");

    /* Throughput test */
    printf("\nThroughput test (sequential create + read):\n");
    {
        MdimVolume v;
        mdim_volume_init(&v, NULL);
        uint32_t block_size = 4096;
        uint8_t *data = (uint8_t *)malloc(block_size);
        uint8_t *readbuf = (uint8_t *)malloc(block_size);
        for (uint32_t i = 0; i < block_size; i++) data[i] = (uint8_t)i;

        double t0 = now_us();
        int n = 0;
        char name[24];
        while (1) {
            snprintf(name, sizeof(name), "blk%04d.bin", n);
            int err = MDIM_OK;
            mdim_summon(&v, name, data, block_size, &err);
            if (err != MDIM_OK) break;
            n++;
        }
        double create_total = now_us() - t0;
        printf("  Created %d files (%d KB total) in %.2f ms\n", n, n * block_size / 1024, create_total / 1000.0);
        printf("  Create throughput: %.2f MB/s\n", (double)(n * block_size) / create_total * 1e6 / 1048576.0);

        uint32_t actual = 0;
        t0 = now_us();
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof(name), "blk%04d.bin", i);
            MdimFile f;
            if (mdim_open(&v, name, &f) == MDIM_OK)
                mdim_read(&v, &f, readbuf, block_size, &actual);
        }
        double read_total = now_us() - t0;
        printf("  Read %d files in %.2f ms\n", n, read_total / 1000.0);
        printf("  Read throughput: %.2f MB/s\n", (double)(n * block_size) / read_total * 1e6 / 1048576.0);

        free(data);
        free(readbuf);
        mdim_volume_free(&v);
    }

    return 0;
}
