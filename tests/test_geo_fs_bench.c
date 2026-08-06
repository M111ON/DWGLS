/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_fs_bench.c — GeoFS Real File Benchmark
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tests with REAL files: text, binary, mixed data.
 * Measures: throughput, compression ratio, cache hit rate.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "geofs_core.h"

/* ── Helpers ─────────────────────────────────────────────────── */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Generate pseudo-random but reproducible data */
static void fill_pattern(uint8_t *buf, uint32_t size, uint32_t seed) {
    uint32_t s = seed;
    for (uint32_t i = 0; i < size; i++) {
        s = s * 1103515245 + 12345;
        buf[i] = (uint8_t)(s >> 16);
    }
}

/* Generate low-entropy data (repeated patterns) */
static void fill_low_entropy(uint8_t *buf, uint32_t size) {
    for (uint32_t i = 0; i < size; i++)
        buf[i] = (uint8_t)(i % 4);  /* only 4 distinct values */
}

/* Count distinct byte values */
static uint32_t count_distinct(const uint8_t *buf, uint32_t size) {
    uint8_t seen[256] = {0};
    for (uint32_t i = 0; i < size; i++) seen[buf[i]] = 1;
    uint32_t n = 0;
    for (int i = 0; i < 256; i++) n += seen[i];
    return n;
}

/* ═══════════════════════════════════════════════════════════════
   BENCHMARK 1: Create files of various sizes
   ═══════════════════════════════════════════════════════════════ */

static void bench_create_files(void) {
    printf("\n── Benchmark 1: Create Files ──────────────────────\n");

    GeosVolume vol;
    geos_volume_init(&vol);
    strncpy(vol.vol_name, "bench_create", 31);

    struct { const char *name; uint32_t size; } files[] = {
        {"tiny.bin",     64},
        {"small.bin",    256},
        {"medium.bin",   4096},
        {"large.bin",    65536},
        {"huge.bin",     524288},
    };
    int n_files = sizeof(files) / sizeof(files[0]);

    uint8_t *data = (uint8_t *)malloc(524288);
    fill_pattern(data, 524288, 42);

    double t0 = now_ms();
    for (int i = 0; i < n_files; i++) {
        uint8_t entropy = (uint8_t)((count_distinct(data, files[i].size) * 255) / 256);
        GeosInode *inode = geos_create(&vol, files[i].name, files[i].size, data);
        if (inode) {
            inode->entropy = entropy;
            inode->tier = adaptive_tier(entropy);
        }
    }
    double elapsed = now_ms() - t0;

    printf("  Created %d files in %.2f ms\n", n_files, elapsed);
    printf("  Total blocks used: %u / %u\n", vol.total_blocks_used, GEOS_ADDR_SPACE);
    printf("  Free: %u blocks (%.1f KB)\n", vol.total_blocks_free,
           vol.total_blocks_free * GEOS_BLOCK_SZ / 1024.0);

    /* List all files */
    printf("\n  %-20s %8s %6s %5s %6s\n", "Name", "Size", "Blocks", "Tier", "Cell");
    printf("  %-20s %8s %6s %5s %6s\n", "────────────────────", "────────", "──────", "─────", "──────");
    for (uint16_t i = 0; i < vol.inode_count; i++) {
        GeosInode *inode = &vol.inodes[i];
        printf("  %-20s %8u %6u %5u [%s]\n",
               inode->name, inode->size_bytes, inode->block_count,
               inode->tier, cell_type_name(inode->addr.cell_type));
    }

    free(data);
}

/* ═══════════════════════════════════════════════════════════════
   BENCHMARK 2: Voronoi cache hit rates
   ═══════════════════════════════════════════════════════════════ */

static void bench_voronoi_cache(void) {
    printf("\n── Benchmark 2: Voronoi Cache ─────────────────────\n");

    GeosVolume vol;
    geos_volume_init(&vol);
    strncpy(vol.vol_name, "bench_voronoi", 31);

    VoronoiCache vc;
    voronoi_init(&vc);

    /* Create 20 files */
    uint8_t data[4096];
    fill_pattern(data, 4096, 77);
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file_%02d.bin", i);
        geos_create(&vol, name, 4096, data);
    }

    /* Simulate access pattern: hot files accessed repeatedly */
    printf("  Simulating access pattern (1000 accesses)...\n");

    double t0 = now_ms();
    for (int round = 0; round < 100; round++) {
        for (int i = 0; i < 10; i++) {  /* access first 10 files (hot) */
            char name[32];
            snprintf(name, sizeof(name), "file_%02d.bin", i);
            geos_voronoi_access(&vol, &vc, name, NULL, NULL);
        }
        /* Access cold files occasionally */
        if (round % 10 == 0) {
            char name[32];
            snprintf(name, sizeof(name), "file_%02d.bin", 15);
            geos_voronoi_access(&vol, &vc, name, NULL, NULL);
        }
        voronoi_tick(&vc);
    }
    double elapsed = now_ms() - t0;

    voronoi_stats(&vc);
    printf("  Elapsed: %.2f ms (1000 accesses)\n", elapsed);
    printf("  Throughput: %.0f ops/ms\n", 1000.0 / elapsed);

    int vrc = voronoi_verify(&vc);
    printf("  Verify: %s\n", vrc == 0 ? "PASS" : "FAIL");
}

/* ═══════════════════════════════════════════════════════════════
   BENCHMARK 3: Self-compression (idle_compress)
   ═══════════════════════════════════════════════════════════════ */

static void bench_self_compress(void) {
    printf("\n── Benchmark 3: Self-Compression ──────────────────\n");

    GeosVolume vol;
    geos_volume_init(&vol);
    strncpy(vol.vol_name, "bench_compress", 31);

    /* Create files with different entropy profiles */
    struct { const char *name; uint32_t size; int low_entropy; } files[] = {
        {"config.bin",    1024,  1},  /* low entropy — compressible */
        {"weights.bin",   8192,  0},  /* high entropy — random */
        {"logo.bin",      4096,  1},  /* low entropy — repeated pattern */
        {"weights2.bin", 16384,  0},  /* high entropy */
        {"data.bin",      2048,  1},  /* low entropy */
    };
    int n_files = sizeof(files) / sizeof(files[0]);

    uint8_t *buf = (uint8_t *)malloc(16384);

    for (int i = 0; i < n_files; i++) {
        if (files[i].low_entropy)
            fill_low_entropy(buf, files[i].size);
        else
            fill_pattern(buf, files[i].size, i * 31);

        GeosInode *inode = geos_create(&vol, files[i].name, files[i].size, buf);
        if (inode) {
            uint8_t ent = (uint8_t)((count_distinct(buf, files[i].size) * 255) / 256);
            inode->entropy = ent;
            inode->tier = adaptive_tier(ent);
        }
    }

    uint32_t blocks_before = vol.total_blocks_used;

    double t0 = now_ms();
    uint32_t saved = geos_idle_compress(&vol);
    double elapsed = now_ms() - t0;

    printf("  Files created: %d\n", n_files);
    printf("  Blocks before: %u\n", blocks_before);
    printf("  Blocks after:  %u\n", vol.total_blocks_used);
    printf("  Saved:         %u bytes (%.1f%%)\n", saved,
           blocks_before > 0 ? (double)saved / (blocks_before * GEOS_BLOCK_SZ) * 100.0 : 0);
    printf("  Compress time: %.3f ms\n", elapsed);

    /* List compressed files */
    printf("\n  %-20s %8s %6s %5s %8s\n", "Name", "Size", "Blocks", "Tier", "Compressed");
    printf("  %-20s %8s %6s %5s %8s\n", "────────────────────", "────────", "──────", "─────", "──────────");
    for (uint16_t i = 0; i < vol.inode_count; i++) {
        GeosInode *inode = &vol.inodes[i];
        printf("  %-20s %8u %6u %5u %s\n",
               inode->name, inode->size_bytes, inode->block_count,
               inode->tier,
               (inode->flags & GEOS_FLAG_COMPRESSED) ? "YES" : "no");
    }

    free(buf);
}

/* ═══════════════════════════════════════════════════════════════
   BENCHMARK 4: Serialize/Deserialize with real data
   ═══════════════════════════════════════════════════════════════ */

static void bench_serialize(void) {
    printf("\n── Benchmark 4: Serialize/Deserialize ─────────────\n");

    GeosVolume vol;
    geos_volume_init(&vol);
    strncpy(vol.vol_name, "bench_serialize", 31);

    /* Create 10 files with real data */
    uint8_t *data = (uint8_t *)malloc(32768);
    for (int i = 0; i < 10; i++) {
        fill_pattern(data, 32768, i * 100);
        char name[32];
        snprintf(name, sizeof(name), "dataset_%02d.bin", i);
        geos_create(&vol, name, 32768, data);
    }

    printf("  Created 10 files, %u blocks used\n", vol.total_blocks_used);

    /* Serialize */
    double t0 = now_ms();
    geos_serialize(&vol, "build/bench_geofs.geofs");
    double t_ser = now_ms() - t0;
    printf("  Serialize:   %.3f ms\n", t_ser);

    /* Deserialize */
    GeosVolume vol2;
    t0 = now_ms();
    geos_deserialize(&vol2, "build/bench_geofs.geofs");
    double t_des = now_ms() - t0;
    printf("  Deserialize: %.3f ms\n", t_des);

    /* Verify */
    assert(vol2.inode_count == vol.inode_count);
    assert(vol2.total_blocks_used == vol.total_blocks_used);
    printf("  Verify: PASS (inode_count=%u, blocks=%u)\n",
           vol2.inode_count, vol2.total_blocks_used);

    /* File size */
    FILE *f = fopen("build/bench_geofs.geofs", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        printf("  .geofs file size: %.1f KB (%.1f%% of block data)\n",
               sz / 1024.0,
               vol.total_blocks_used * GEOS_BLOCK_SZ > 0 ?
               (double)sz / (vol.total_blocks_used * GEOS_BLOCK_SZ) * 100.0 : 0);
    }

    free(data);
}

/* ═══════════════════════════════════════════════════════════════
   BENCHMARK 5: Directory operations
   ═══════════════════════════════════════════════════════════════ */

static void bench_directories(void) {
    printf("\n── Benchmark 5: Directory Operations ──────────────\n");

    GeosVolume vol;
    geos_volume_init(&vol);

    GeosDirTable dt;
    geos_dir_table_init(&dt);

    /* Create directory tree */
    double t0 = now_ms();
    geos_mkdir(&vol, &dt, "models", 0);
    geos_mkdir(&vol, &dt, "qwen", 1);
    geos_mkdir(&vol, &dt, "llama", 1);
    geos_mkdir(&vol, &dt, "docs", 0);
    geos_mkdir(&vol, &dt, "archive", 3);
    double t_dir = now_ms() - t0;

    /* Create files in different dirs */
    uint8_t data[128] = {0};
    GeosInode *f1 = geos_create(&vol, "readme.txt", 128, data);
    if (f1) f1->parent_addr = 0;  /* root */
    GeosInode *f2 = geos_create(&vol, "model.bin", 128, data);
    if (f2) f2->parent_addr = 1;  /* models/ */
    GeosInode *f3 = geos_create(&vol, "notes.md", 128, data);
    if (f3) f3->parent_addr = 3;  /* docs/ */

    printf("  Created %d dirs, %d files in %.3f ms\n",
           dt.dir_count, vol.inode_count, t_dir);

    /* List root */
    const char *names[16];
    int is_dirs[16];
    int n = geos_ls(&vol, &dt, 0, names, is_dirs, 16);
    printf("  Root entries: %d\n", n);
    for (int i = 0; i < n; i++) {
        printf("    %s %s\n", is_dirs[i] ? "[DIR] " : "[FILE]", names[i]);
    }

    /* Path resolve */
    uint32_t idx;
    int rc = geos_path_resolve(&vol, &dt, "readme.txt", &idx);
    printf("  Resolve 'readme.txt': %s (idx=%u)\n", rc == 0 ? "FOUND" : "MISS", idx);
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("===============================================================\n");
    printf("  GeoFS Benchmark Suite\n");
    printf("===============================================================\n");

    bench_create_files();
    bench_voronoi_cache();
    bench_self_compress();
    bench_serialize();
    bench_directories();

    printf("\n===============================================================\n");
    printf("  Benchmark complete.\n");
    printf("===============================================================\n");

    return 0;
}
