/*
 * test_bfs_migrate.c — wrap-relocate between worlds (model surgery primitive)
 * ═══════════════════════════════════════════════════════════════════════════
 * Migrate = lossless read + write + verify + retire/free. Old world keeps
 * graves (tombs) and reusable blocks; new world gets living watched data.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_bfs_migrate tests/test_bfs_migrate.c -lm
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
    printf("═ MIGRATE — wrap-relocate, not delete-destroy ═\n");
    BreathingFS src, dst;
    int8_t d0[144], d1[432];
    bfs_init(&src);
    bfs_init(&dst);
    fill_buf(d0, 144, 1);
    fill_buf(d1, 432, 2);
    src.seeker.current_pos = 500; src.seeker.home_pos = 500;
    bfs_write(&src, "a.bin", d0, 144);
    src.seeker.current_pos = 1200; src.seeker.home_pos = 1200;
    bfs_write(&src, "b.bin", d1, 432);

    /* ── G1: guards (args, missing, self, name taken) ── */
    bfs_write(&dst, "a.bin", d0, 144);
    CHECK("G1: -1 args, -2 missing, -3 self, -4 name taken",
          bfs_migrate(0, &dst, "b.bin") == -1 &&
          bfs_migrate(&src, &dst, "nope.bin") == -2 &&
          bfs_migrate(&src, &src, "b.bin") == -3 &&
          bfs_migrate(&src, &dst, "a.bin") == -4);
    bfs_delete(&dst, "a.bin");   /* clear the blocker (tomb kept in dst) */

    /* ── G2: migrate b.bin — bytes identical, watched, src freed+tombs ── */
    int rc = bfs_migrate(&src, &dst, "b.bin");
    {
        int8_t out[432];
        uint32_t act = 0;
        int living = (bfs_read(&dst, "b.bin", out, 432, &act) == 0 &&
                      memcmp(out, d1, 432) == 0);
        int watched = (dst.planets[0].magic == PLANET_MAGIC &&
                       dst.planets[0].tail_n == 0u);
        int src_freed = (src.n_blocks_used == 1u && src.n_files == 1u &&
                         src.tomb_count == 3u &&
                         src.tombs[1].magic == PLANET_TOMB_MAGIC);
        CHECK("G2: dst living+watched, src freed with 3 graves",
              rc == 0 && living && watched && src_freed);
    }

    /* ── G3: both worlds healthy under ticks ── */
    {
        BFSBreath bs, bd;
        bfs_breath_init(&bs, &src, 0.05);
        bfs_breath_init(&bd, &dst, 0.05);
        for (int i = 0; i < 100; i++) {
            bfs_breath_tick(&bs);
            bfs_breath_tick(&bd);
        }
        CHECK("G3: 100 ticks, both worlds silent",
              src.planet_mismatch == 0u && dst.planet_mismatch == 0u);
    }

    /* ── G4: src's freed blocks reusable after migrate ── */
    {
        int8_t d2[200];
        fill_buf(d2, 200, 9);
        src.seeker.current_pos = 3000; src.seeker.home_pos = 3000;
        int w = bfs_write(&src, "c.bin", d2, 200);
        int8_t out[200];
        uint32_t act = 0;
        CHECK("G4: old world reusable (c.bin written+lossless)",
              w == 0 && bfs_read(&src, "c.bin", out, 200, &act) == 0 &&
              memcmp(out, d2, 200) == 0);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
