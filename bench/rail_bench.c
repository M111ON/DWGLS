/*
 * rail_bench.c — Phase 2: BFSHub synthetic tensor pull benchmark
 * ═══════════════════════════════════════════════════════════════════
 * Measures the full geometric pull chain:
 *   BIMG image → block flat id → cell_addr (pipe,tick) → spine ceremony
 *   → jet_bridge_hop → gear_cpu_tick → zero-copy pointer
 *
 * R1: pull latency (ns/pull) — single-block ceremony
 * R2: pull throughput (pulls/s) — all blocks, all files
 * R3: zero-copy verify — pointer always lands inside mapping
 * R4: full-file decode lossless (100% of payloads)
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -o build/rail_bench bench/rail_bench.c -lm
 * RUN:   ./build/rail_bench [files] [repeats]
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "geo_bfs_hub.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void fill_file(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31 + i * 7 + (i >> 3)) & 0xFF);
}

int main(int argc, char **argv)
{
    uint32_t n_files = argc > 1 ? (uint32_t)atoi(argv[1]) : 24;
    uint32_t repeats = argc > 2 ? (uint32_t)atoi(argv[2]) : 100;
    if (n_files > 56) n_files = 56;

    /* ── build image ── */
    BreathingFS fs; bfs_init(&fs);
    static int8_t payloads[BFS_MAX_FILES][BFS_SLOTS_BLOCK * 4];
    uint32_t sizes[BFS_MAX_FILES];
    const uint32_t size_tab[] = {1, 2, 3, 4};
    for (uint32_t i = 0; i < n_files; i++) {
        uint32_t nb = size_tab[i % 4];
        sizes[i] = BFS_SLOTS_BLOCK * nb;
        fill_file(payloads[i], sizes[i], i * 17 + 5);
        fs.seeker.current_pos = (i * 200) % BFS_TOTAL_SLOTS;
        fs.seeker.home_pos = fs.seeker.current_pos;
        if (bfs_write(&fs, (const char[]){'t', '0' + (char)(i / 10), '0' + (char)(i % 10), '\0'},
                      payloads[i], sizes[i]) != 0) {
            printf("write fail at %u\n", i); return 1;
        }
    }
    if (bfs_save_img("build/rail_bench.img", &fs) != 0) { printf("save fail\n"); return 1; }
    uint32_t total_blocks = fs.n_blocks_used;
    uint64_t total_b = fs.total_bytes;

    /* ── R1+R2: pull ceremony ── */
    BFSHub hub;
    if (bfs_hub_open(&hub, "build/rail_bench.img") != 0) { printf("open fail\n"); return 1; }

    double t0 = now_ms();
    uint64_t pulls = 0;
    int in_map = 1;
    for (uint32_t r = 0; r < repeats; r++) {
        for (uint32_t f = 0; f < n_files; f++) {
            uint32_t nb = hub.map.fs.files[f].n_blocks;
            for (uint32_t k = 0; k < nb; k++) {
                const uint8_t *enc; uint32_t esz;
                int rc = bfs_hub_pull(&hub, f, k, &enc, &esz);
                if (rc != 0) { printf("pull fail f=%u k=%u rc=%d\n", f, k, rc); return 1; }
                if (enc < hub.map.map_ptr || enc >= hub.map.map_ptr + hub.map.map_size)
                    in_map = 0;
                pulls++;
            }
        }
    }
    double t1 = now_ms() - t0;

    /* ── R4: full-file decode lossless ── */
    t0 = now_ms();
    int lossless = 1;
    for (uint32_t f = 0; f < n_files; f++) {
        static int8_t out[BFS_SLOTS_BLOCK * 4];
        if (bfs_hub_pull_file(&hub, f, out, sizes[f]) != 0) { lossless = 0; break; }
        if (memcmp(out, payloads[f], sizes[f]) != 0) { lossless = 0; break; }
    }
    double t2 = now_ms() - t0;

    /* ── R3 zero-copy verify count ── */
    double ns_per_pull = (pulls > 0) ? (t1 * 1e6) / (double)pulls : 0.0;
    double pulls_per_s = (t1 > 0) ? (double)pulls / (t1 / 1000.0) : 0.0;

    printf("═══════════════════════════════════════════════════════════\n");
    printf("BFSHub Pull Benchmark — %u files, %u blocks, %.3f MB\n",
           n_files, total_blocks, (double)total_b / 1e6);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  R1  pull latency       %8.2f ns/pull\n", ns_per_pull);
    printf("  R2  pull throughput    %8.2f K pulls/s\n", pulls_per_s / 1e3);
    printf("  R3  zero-copy in-map   %s (%lu pulls)\n",
           in_map ? "YES" : "NO", (unsigned long)pulls);
    printf("  R4  full-decode lossless %s  (%.2f ms)\n",
           lossless ? "YES" : "NO", t2);
    printf("  bridges fired          %u\n", hub.bridges);
    printf("  gear cpu ops           %u  (worlds=%u)\n",
           hub.gear.cpu_ops, hub.gear.cpu_worlds);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("(synthetic tensor payloads; decode excluded from R1/R2 —\n");
    printf(" R1/R2 measure the addressing + ceremony + pointer return)\n");

    bfs_hub_close(&hub);
    return (in_map && lossless) ? 0 : 1;
}