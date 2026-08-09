/*
 * test_geo_bfs_hub.c — Phase 2: Breathing FS ↔ GeoPipeline Bridge
 * ═══════════════════════════════════════════════════════════════════
 * T1: hub open/close over a real BIMG image
 * T2: block → (pipe,tick) addressing matches cell_addr arithmetic
 * T3: bfs_hub_pull — zero-copy pointer into mapping, ceremony fires
 * T4: bfs_hub_pull_file — full-file decode, lossless vs original
 * T5: multi-file batch pull
 * T6: scale → go_home → hub still lossless (breathing through pipeline)
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -o build/test-geo_bfs_hub
 *        tests/test_geo_bfs_hub.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_bfs_hub.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static void fill_file(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31 + i * 7 + (i >> 3)) & 0xFF);
}

int main(void)
{
    printf("Phase 2: Breathing FS ↔ GeoPipeline Bridge\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* ── build a real BIMG image with 4 files ── */
    BreathingFS fs; bfs_init(&fs);
    static int8_t d0[BFS_SLOTS_BLOCK];            fill_file(d0, 144, 1);
    static int8_t d1[BFS_SLOTS_BLOCK * 3];        fill_file(d1, 432, 2);
    static int8_t d2[BFS_SLOTS_BLOCK * 2];        fill_file(d2, 288, 3);
    static int8_t d3[BFS_SLOTS_BLOCK * 4];        fill_file(d3, 576, 4);

    fs.seeker.current_pos = 0;    fs.seeker.home_pos = 0;
    bfs_write(&fs, "h0.bin", d0, 144);
    fs.seeker.current_pos = 300;  fs.seeker.home_pos = 300;
    bfs_write(&fs, "h1.bin", d1, 432);
    fs.seeker.current_pos = 700;  fs.seeker.home_pos = 700;
    bfs_write(&fs, "h2.bin", d2, 288);
    fs.seeker.current_pos = 1200; fs.seeker.home_pos = 1200;
    bfs_write(&fs, "h3.bin", d3, 576);
    CHECK(1, "image save ok", bfs_save_img("build/t_p2.img", &fs) == 0);
    CHECK(1, "4 files, 10 blocks", fs.n_files == 4 && fs.n_blocks_used == 10);

    /* ── T2: addressing ── */
    printf("\nTEST 2: Block → (pipe, tick) addressing\n");
    for (uint32_t f = 0; f < 4; f++) {
        uint32_t nb = fs.files[f].n_blocks;
        for (uint32_t k = 0; k < nb; k++) {
            uint32_t flat = (fs.files[f].home_block + k) % BFS_TOTAL_SLOTS;
            uint16_t p1; uint8_t t1;
            geo_cell_addr_offset_to_pipe(flat, &p1, &t1);
            CHECK(2, "pipe < 1728", p1 < FS_PIPES);
            CHECK(2, "tick < 12", t1 < FS_TICKS_PER_CYCLE);
            CHECK(2, "flat id covers full space", flat < 20736);
        }
    }
    /* flat id bijection sanity: f1 block0 = home_block 1 → pipe/tick */
    {
        uint16_t p; uint8_t t;
        geo_cell_addr_offset_to_pipe(1, &p, &t);
        CHECK(2, "flat=1 → pipe=1 tick=0", p == 1 && t == 0);
        geo_cell_addr_offset_to_pipe(1728, &p, &t);
        CHECK(2, "flat=1728 → pipe=0 tick=1 (1728/1728=1)", p == 0 && t == 1);
    }

    /* ── T3: hub open + zero-copy pull ── */
    printf("\nTEST 3: Hub open + zero-copy pull ceremony\n");
    BFSHub hub;
    CHECK(3, "hub open ok", bfs_hub_open(&hub, "build/t_p2.img") == 0);
    CHECK(3, "TOC parsed (4 files)", hub.map.fs.n_files == 4);
    CHECK(3, "spine initialized (1728 pipes)", FS_PIPES == 1728u && hub.spine.mode == FS_MODE_ACTIVE);
    CHECK(3, "gear initialized (20736 = 128×162)",
          GEAR_GEO_FULL == 20736u && FS_SLOTS == 20736u);

    const uint8_t *enc; uint32_t esz;
    int rc = bfs_hub_pull(&hub, 0, 0, &enc, &esz);
    CHECK(3, "pull file0 block0 ok", rc == 0);
    CHECK(3, "encoded size > 0", esz > 0);
    CHECK(3, "pointer is INTO mapping (zero-copy)",
          enc >= hub.map.map_ptr && enc < hub.map.map_ptr + hub.map.map_size);
    CHECK(3, "pulls count incremented", hub.pulls == 1);
    CHECK(3, "bridge fired", hub.bridges >= 1);
    CHECK(3, "gear cpu ticked", hub.gear.cpu_ops >= 1);
    CHECK(3, "pull missing file idx → -2",
          bfs_hub_pull(&hub, 99, 0, &enc, &esz) == -2);

    /* ── T4: full-file decode through hub (lossless) ── */
    printf("\nTEST 4: Full-file decode via hub (lossless)\n");
    {
        static int8_t out[BFS_SLOTS_BLOCK * 4];
        memset(out, 0, sizeof(out));
        rc = bfs_hub_pull_file(&hub, 1, out, 432);
        CHECK(4, "h1 decode ok", rc == 0);
        CHECK(4, "h1 lossless vs original", memcmp(out, d1, 432) == 0);
        rc = bfs_hub_pull_file(&hub, 3, out, 576);
        CHECK(4, "h3 decode ok", rc == 0);
        CHECK(4, "h3 lossless vs original", memcmp(out, d3, 576) == 0);
    }

    /* ── T5: batch ── */
    printf("\nTEST 5: Batch pull\n");
    {
        static int8_t o0[BFS_SLOTS_BLOCK];
        static int8_t o2[BFS_SLOTS_BLOCK * 2];
        const int8_t *outs[2] = { o0, o2 };
        uint32_t ns[2] = { 0, 0 };
        uint32_t fidx[2] = { 0, 2 };
        uint32_t ok = bfs_hub_pull_batch(&hub, fidx, 2, outs, ns);
        CHECK(5, "2 files batch-pulled", ok == 2);
        CHECK(5, "h0 lossless (batch)", memcmp(o0, d0, 144) == 0);
        CHECK(5, "h2 lossless (batch)", memcmp(o2, d2, 288) == 0);
    }

    /* ── T6: breathing through pipeline ── */
    printf("\nTEST 6: Scale cycle through hub, go_home, still lossless\n");
    {
        /* mutate the mapped TOC seeker, write-through, reopen */
        BFSHub *h2 = &hub;
        h2->map.fs.seeker.scale = 0.5;
        bfs_move_seeker(&h2->map.fs, 0.5);
        bfs_mmap_sync(&h2->map);

        static int8_t out[BFS_SLOTS_BLOCK * 4];
        rc = bfs_hub_pull_file(h2, 2, out, 288);
        CHECK(6, "pull at scale 0.5 still decodes (raw OK)", rc == 0);

        bfs_go_home(&h2->map.fs);
        bfs_mmap_sync(&h2->map);
        rc = bfs_hub_pull_file(h2, 2, out, 288);
        CHECK(6, "after go_home lossless", rc == 0 && memcmp(out, d2, 288) == 0);
        CHECK(6, "seeker home", seeker_is_home(&h2->map.fs.seeker));
    }

    /* ── stats + close ── */
    printf("\nStats:\n");
    bfs_hub_stats(&hub);
    bfs_hub_close(&hub);
    CHECK(6, "close resets hub", hub.is_open == 0);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("RESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail == 0 ? 0 : 1;
}