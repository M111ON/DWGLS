/*
 * test_bfs_fold.c — Explicit fold operator (usable now, reason later)
 * ═══════════════════════════════════════════════════════════════════
 * Scattered homes -> one fold -> compact span + enclosing residual.
 * Asserts: compaction exact, reads lossless, planets untouched,
 * fg_log unpolluted (no fake scale events), counter counts.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_bfs_fold tests/test_bfs_fold.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bfs_fold.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint32_t home_spread(BFSBreath *b, BreathingFS *fs,
                            uint32_t *mn_out, uint32_t *mx_out) {
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        if (fs->block_owner[i] == 0xFFFFFFFF) continue;
        uint32_t h = b->live[i].home_pos;
        if (h < mn) mn = h;
        if (h > mx) mx = h;
    }
    if (mn_out) *mn_out = mn;
    if (mx_out) *mx_out = mx;
    return mx - mn;
}

int main(void) {
    printf("═ FOLD — explicit compact, usable now ═\n");
    BreathingFS fs;
    int8_t d0[144], d1[432];
    for (int i = 0; i < 144; i++) d0[i] = (int8_t)(i * 7 + 1);
    for (int i = 0; i < 432; i++) d1[i] = (int8_t)(i * 3 + 2);
    bfs_init(&fs);
    fs.seeker.current_pos = 9000; fs.seeker.home_pos = 9000;
    bfs_write(&fs, "a.bin", d0, 144);
    fs.seeker.current_pos = 19000; fs.seeker.home_pos = 19000;
    bfs_write(&fs, "b.bin", d1, 432);
    BFSBreath b;
    bfs_breath_init(&b, &fs, 0.05);

    uint32_t mn, mx;
    uint32_t before = home_spread(&b, &fs, &mn, &mx);
    uint32_t fg_before = bfs_gear_count(&fs.fg_log);

    /* ── F1: one fold compacts scattered homes into [0, n) ── */
    uint32_t n = bfs_fold_compact(&fs, &b);
    uint32_t after = home_spread(&b, &fs, &mn, &mx);
    CHECK("F1: 4 blocks compacted to span [0,4), residual surrounds",
          n == 4u && mn == 0u && mx == 3u && after == 3u && before > 1000u);

    /* ── F2: meta + live agree, deltas zero ── */
    {
        int agree = 1;
        for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
            if (fs.block_owner[i] == 0xFFFFFFFF) continue;
            if (fs.block_meta[i].home_pos != b.live[i].home_pos ||
                b.live[i].delta != 0 || fs.block_meta[i].delta != 0) {
                agree = 0; break;
            }
        }
        CHECK("F2: meta/live agree, all deltas 0", agree);
    }

    /* ── F3: reads lossless + planets untouched + fg_log clean ── */
    {
        static int8_t out[576];
        uint32_t act = 0;
        int reads = (bfs_read(&fs, "a.bin", out, 144, &act) == 0 &&
                     memcmp(out, d0, 144) == 0 &&
                     bfs_read(&fs, "b.bin", out, 432, &act) == 0 &&
                     memcmp(out, d1, 432) == 0);
        int planets = (fs.planet_mismatch == 0u &&
                       fs.planets[0].tail_n == 0u &&
                       fs.planets[1].tail_n == 0u);
        CHECK("F3: reads lossless, no planet touched, fg_log unpolluted",
              reads && planets &&
              bfs_gear_count(&fs.fg_log) == fg_before && fs.fold_count == 1u);
    }

    /* ── F4: drift after fold stays healthy (fold is a valid baseline) ── */
    for (int i = 0; i < 300; i++) bfs_breath_tick(&b);
    CHECK("F4: 300 post-fold ticks healthy",
          fs.planet_mismatch == 0u && bfs_breath_all_bounded(&b));

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
