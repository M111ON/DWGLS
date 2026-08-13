/* ═══════════════════════════════════════════════════════════════════════════
 * bench_geo_mdim.c — Compare old MDIM vs new geometric MDIM
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geofs_mdim.h"
#include "geo_mdim.h"

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

#define BENCH_ITERS 10000

/* Benchmark old MDIM */
typedef struct {
    double create_us;
    double read_us;
    double write_us;
    double delete_us;
} OldMdimBench;

static OldMdimBench bench_old_mdim(uint32_t size, int n_files) {
    OldMdimBench r = {0};
    MdimVolume v;
    mdim_volume_init(&v, NULL);
    
    char name[24];
    uint8_t *data = (uint8_t *)malloc(size);
    uint8_t *readbuf = (uint8_t *)malloc(size);
    for (uint32_t i = 0; i < size; i++) data[i] = (uint8_t)(i * 7 + 3);
    
    /* Create */
    double best = 1e18, sum = 0;
    for (int i = 0; i < BENCH_ITERS && i < n_files; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        int err = MDIM_OK;
        double t0 = now_us();
        mdim_summon(&v, name, data, size, &err);
        double dt = now_us() - t0;
        if (err != MDIM_OK) break;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.create_us = (best < 1e18) ? sum / (BENCH_ITERS < n_files ? BENCH_ITERS : n_files) : 0;
    
    /* Read */
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
        if (dt < best) best = dt;
        sum += dt;
    }
    r.read_us = (best < 1e18) ? sum / read_count : 0;
    
    /* Write */
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
    
    /* Delete */
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
    
    free(data);
    free(readbuf);
    mdim_volume_free(&v);
    return r;
}

/* Benchmark new geometric MDIM */
typedef struct {
    double create_us;
    double read_us;
    double write_us;
    double delete_us;
} GeoMdimBench;

static GeoMdimBench bench_geo_mdim(uint32_t size, int n_files) {
    GeoMdimBench r = {0};
    GeoMdimVolume *v = (GeoMdimVolume *)malloc(sizeof(GeoMdimVolume));
    uint8_t *buf = (uint8_t *)malloc(GEO_MDIM_VOL_BYTES);
    memset(buf, 0, GEO_MDIM_VOL_BYTES);
    geo_mdim_init(v, buf);
    
    char name[24];
    uint8_t *data = (uint8_t *)malloc(size);
    uint8_t *readbuf = (uint8_t *)malloc(size);
    for (uint32_t i = 0; i < size; i++) data[i] = (uint8_t)(i * 7 + 3);
    
    /* Create */
    double best = 1e18, sum = 0;
    for (int i = 0; i < BENCH_ITERS && i < n_files; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        double t0 = now_us();
        int err = geo_mdim_create(v, name, data, size);
        double dt = now_us() - t0;
        if (err != GEO_MDIM_OK) break;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.create_us = (best < 1e18) ? sum / (BENCH_ITERS < n_files ? BENCH_ITERS : n_files) : 0;
    
    /* Read */
    best = 1e18; sum = 0;
    uint32_t actual = 0;
    int read_count = (BENCH_ITERS < n_files) ? BENCH_ITERS : n_files;
    for (int i = 0; i < read_count; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        double t0 = now_us();
        geo_mdim_read(v, name, readbuf, size, &actual);
        double dt = now_us() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.read_us = (best < 1e18) ? sum / read_count : 0;
    
    /* Write */
    best = 1e18; sum = 0;
    int write_count = (BENCH_ITERS < n_files) ? BENCH_ITERS : n_files;
    for (int i = 0; i < write_count; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        double t0 = now_us();
        geo_mdim_write(v, name, data, size);
        double dt = now_us() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.write_us = (best < 1e18) ? sum / write_count : 0;
    
    /* Delete */
    best = 1e18; sum = 0;
    int del_count = (BENCH_ITERS < n_files) ? BENCH_ITERS : n_files;
    for (int i = 0; i < del_count; i++) {
        snprintf(name, sizeof(name), "f%04d.bin", i);
        double t0 = now_us();
        geo_mdim_delete(v, name);
        double dt = now_us() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    r.delete_us = (best < 1e18) ? sum / del_count : 0;
    
    free(data);
    free(readbuf);
    geo_mdim_free(v);
    free(v);
    return r;
}

int main(void) {
    printf("GeoFS MDIM — Old vs New Geometric Benchmark\n");
    printf("Old: Traditional MDIM with probe chains, bitmap, journal, CRC\n");
    printf("New: Geometric MDIM with pre-computed locked positions\n\n");
    fflush(stdout);
    
    struct { uint32_t size; int count; const char *label; } tests[] = {
        {     1,  100, "1 B × 100 files" },
        {    63,   50, "63 B × 50 files" },
        {   128,   30, "128 B × 30 files" },
        {  1024,   20, "1 KB × 20 files" },
        {  4096,   10, "4 KB × 10 files" },
    };
    int n_tests = sizeof(tests) / sizeof(tests[0]);
    
    printf("┌─────────────────────────┬──────────────────────────────────────────┐\n");
    printf("│                         │ Old MDIM (µs)    New Geo (µs)   Speedup  │\n");
    printf("│ Test                    │ Crt  Rd   Wr  Del │ Crt  Rd   Wr  Del │      │\n");
    printf("├─────────────────────────┼──────────────────────────────────────────┤\n");
    fflush(stdout);
    
    for (int i = 0; i < n_tests; i++) {
        OldMdimBench old = bench_old_mdim(tests[i].size, tests[i].count);
        GeoMdimBench geo = bench_geo_mdim(tests[i].size, tests[i].count);
        
        double speedup_create = (geo.create_us > 0) ? old.create_us / geo.create_us : 0;
        double speedup_read = (geo.read_us > 0) ? old.read_us / geo.read_us : 0;
        double speedup_write = (geo.write_us > 0) ? old.write_us / geo.write_us : 0;
        double speedup_delete = (geo.delete_us > 0) ? old.delete_us / geo.delete_us : 0;
        double avg_speedup = (speedup_create + speedup_read + speedup_write + speedup_delete) / 4.0;
        
        printf("│ %-23s │ %4.0f %4.0f %4.0f %4.0f │ %4.0f %4.0f %4.0f %4.0f │ %4.1fx │\n",
               tests[i].label,
               old.create_us, old.read_us, old.write_us, old.delete_us,
               geo.create_us, geo.read_us, geo.write_us, geo.delete_us,
               avg_speedup);
        fflush(stdout);
    }
    
    printf("└─────────────────────────┴──────────────────────────────────────────┘\n");
    
    /* Verify correctness */
    printf("\nCorrectness verification:\n");
    {
        GeoMdimVolume *v = (GeoMdimVolume *)malloc(sizeof(GeoMdimVolume));
        uint8_t *buf = (uint8_t *)malloc(GEO_MDIM_VOL_BYTES);
        memset(buf, 0, GEO_MDIM_VOL_BYTES);
        geo_mdim_init(v, buf);
        
        uint8_t data[64] = {0};
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)i;
        
        /* Create files */
        for (int i = 0; i < 10; i++) {
            char name[24];
            snprintf(name, sizeof(name), "test%d.bin", i);
            if (geo_mdim_create(v, name, data, 64) != GEO_MDIM_OK) {
                printf("  ❌ Create failed for %s\n", name);
                continue;
            }
        }
        
        /* Read and verify */
        uint8_t readbuf[64];
        uint32_t actual;
        int errors = 0;
        for (int i = 0; i < 10; i++) {
            char name[24];
            snprintf(name, sizeof(name), "test%d.bin", i);
            if (geo_mdim_read(v, name, readbuf, 64, &actual) != GEO_MDIM_OK) {
                printf("  ❌ Read failed for %s\n", name);
                errors++;
                continue;
            }
            if (actual != 64 || memcmp(data, readbuf, 64) != 0) {
                printf("  ❌ Data mismatch for %s\n", name);
                errors++;
            }
        }
        
        /* Verify geometric positions */
        if (geo_mdim_verify(v)) {
            printf("  ✅ All positions verified correctly\n");
        } else {
            printf("  ❌ Position verification failed\n");
            errors++;
        }
        
        printf("  Files: %u, Slots used: %u\n", v->n_files, v->n_slots_used);
        
        geo_mdim_free(v);
        free(v);
        free(buf);
        
        if (errors == 0) {
            printf("  ✅ All tests passed\n");
        } else {
            printf("  ❌ %d tests failed\n", errors);
        }
    }
    
    return 0;
}
