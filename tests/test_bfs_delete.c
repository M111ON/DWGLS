/*
 * test_bfs_delete.c — retire-then-free lifecycle (v1)
 * ═══════════════════════════════════════════════════════════════════
 * Delete retires every block planet (tomb archived, gate shut) before
 * freeing; file slots compact (swap-with-last); freed blocks are reborn
 * clean on rewrite. Args/not-found codes match read conventions.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_bfs_delete tests/test_bfs_delete.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bfs_breath.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static void fill_buf(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31 + i * 7 + (i >> 3)) & 0xFF);
}

int main(void) {
    printf("═ DELETE — retire-then-free ═\n");
    BreathingFS fs;
    int8_t d0[144], d1[432];
    bfs_init(&fs);
    fill_buf(d0, 144, 1);
    fill_buf(d1, 432, 2);
    fs.seeker.current_pos = 500; fs.seeker.home_pos = 500;
    bfs_write(&fs, "a.bin", d0, 144);     /* block 0 */
    fs.seeker.current_pos = 1200; fs.seeker.home_pos = 1200;
    bfs_write(&fs, "b.bin", d1, 432);     /* blocks 1..3 */

    /* ── D1: arg guards ── */
    CHECK("D1: NULL/-2 codes (args, missing)",
          bfs_delete(0, "a.bin") == -1 && bfs_delete(&fs, 0) == -1 &&
          bfs_delete(&fs, "nope.bin") == -2);

    uint64_t dig1 = fs.planets[1].digest;

    /* ── D2: delete retires first — tomb exact, gate shut, severed ── */
    int rc = bfs_delete(&fs, "b.bin");
    {
        int tombs = 1;
        for (uint32_t bi = 1; bi <= 3u; bi++) {
            PlanetTomb *t = &fs.tombs[bi];
            if (t->magic != PLANET_TOMB_MAGIC || t->id != bi ||
                t->birth_w != 0u || t->death_w != 0u ||
                t->final_home != bi || t->origin != t->digest) {
                tombs = 0; break;
            }
            if (fs.planets[bi].link_open != 0u) { tombs = 0; break; }
        }
        int8_t out[432];
        uint32_t act = 0;
        CHECK("D2: 3 tombs exact (origin==digest, gate shut), unreadable",
              rc == 0 && tombs && fs.tomb_count == 3u &&
              bfs_read(&fs, "b.bin", out, 432, &act) == -2 &&
              planet_verify(&fs.planets[1], out, 1) == -2);
    }

    /* ── D3: survivors intact, slots compacted ── */
    {
        int8_t out[144];
        uint32_t act = 0;
        CHECK("D3: a.bin alive+lossless, files 2->1, blocks 4->1",
              bfs_read(&fs, "a.bin", out, 144, &act) == 0 &&
              memcmp(out, d0, 144) == 0 && fs.n_files == 1u &&
              fs.n_blocks_used == 1u && fs.planets[0].tail_n == 0u);
    }

    /* ── D4: freed blocks reborn clean on rewrite ── */
    {
        int8_t d2[300];
        fill_buf(d2, 300, 7);
        fs.seeker.current_pos = 2000; fs.seeker.home_pos = 2000;
        bfs_write(&fs, "c.bin", d2, 300);   /* reuses blocks 1,2 */
        int reborn = (fs.planets[1].magic == PLANET_MAGIC &&
                      !fs.planets[1].retired && fs.planets[1].tail_n == 0u &&
                      fs.planets[1].tomb.magic == 0u &&
                      fs.planets[1].digest != dig1);
        int8_t out[300];
        uint32_t act = 0;
        BFSBreath b;
        bfs_breath_init(&b, &fs, 0.05);
        for (int i = 0; i < 100; i++) bfs_breath_tick(&b);
        CHECK("D4: rewrite rebirths clean (new soul), 100 ticks healthy",
              reborn && bfs_read(&fs, "c.bin", out, 300, &act) == 0 &&
              memcmp(out, d2, 300) == 0 && fs.planet_mismatch == 0u);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
