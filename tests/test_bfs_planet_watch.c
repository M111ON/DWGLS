/*
 * test_bfs_planet_watch.c — BreathingFS per-block planet wiring (v1)
 * ═══════════════════════════════════════════════════════════════════
 * Birth at bfs_write, verify-collect in bfs_breath_tick (idle-zero),
 * cumulative counter fs->planet_mismatch. Read path untouched (const/pure).
 * v1 limits: in-memory only (restart reborn), birth W=0, no delete path
 * exists so no retire hook.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_bfs_planet_watch tests/test_bfs_planet_watch.c -lm
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
    printf("═ PLANET WATCH — BreathingFS integration v1 ═\n");
    BreathingFS fs;
    int8_t d0[144], d1[432];
    bfs_init(&fs);
    fill_buf(d0, 144, 1);
    fill_buf(d1, 432, 2);
    fs.seeker.current_pos = 500; fs.seeker.home_pos = 500;
    bfs_write(&fs, "a.bin", d0, 144);
    fs.seeker.current_pos = 1200; fs.seeker.home_pos = 1200;
    bfs_write(&fs, "b.bin", d1, 432);

    BFSBreath b;
    bfs_breath_init(&b, &fs, 0.05);

    /* ── W1: healthy ticks — zero mismatches, all planets born ── */
    for (int i = 0; i < 200; i++) bfs_breath_tick(&b);
    {
        int born = 1;
        for (uint32_t bi = 0; bi < 4u; bi++) {
            if (fs.planets[bi].magic != PLANET_MAGIC) { born = 0; break; }
            if (fs.planets[bi].tail_n != 0u) { born = 0; break; }
            if (planet_digest((const int8_t *)fs.block_encoded[bi],
                              fs.block_encoded_size[bi]) !=
                fs.planets[bi].digest) { born = 0; break; }
        }
        CHECK("W1: 200 healthy ticks, mismatch=0, 4 planets born exact",
              fs.planet_mismatch == 0u && born);
    }

    /* ── W2: transient corruption — collected, gate opens, restore clean ── */
    {
        uint8_t save = fs.block_encoded[1][77 % fs.block_encoded_size[1]];
        uint32_t hit = 77 % fs.block_encoded_size[1];
        fs.block_encoded[1][hit] ^= 0x20;
        bfs_breath_tick(&b);
        int collected = (fs.planet_mismatch == 1u &&
                         fs.planets[1].tail_n == 1u &&
                         fs.planets[1].link_open == 1u);
        fs.block_encoded[1][hit] = save;
        bfs_breath_tick(&b);
        CHECK("W2: 1 corrupt tick collected+gate open; restore reads clean",
              collected && fs.planet_mismatch == 1u &&
              fs.planets[1].tail_n == 1u && fs.planets[1].link_open == 1u);
    }

    /* ── W3: permanent corruption — tail fills, auto-reanchor adopts ── */
    {
        fs.block_encoded[2][5 % fs.block_encoded_size[2]] ^= 0x01; /* kept */
        for (int i = 0; i < 9; i++) bfs_breath_tick(&b);
        CHECK("W3: 9 corrupt ticks -> reanchor epoch (scar kept, tail clear)",
              fs.planet_mismatch == 10u && fs.planets[2].reanchors == 1u &&
              fs.planets[2].tail_overflow == 1u && fs.planets[2].tail_n == 0u &&
              fs.planets[2].digest ==
                  planet_digest((const int8_t *)fs.block_encoded[2],
                                fs.block_encoded_size[2]));
    }

    /* ── W4: isolation + self-close — block 1's 9 clean ticks during W3
     * shut its gate and dropped its tail (training wheels off, live) ── */
    CHECK("W4: tails (0,0,0); block 1 gate self-closed, no scar",
          fs.planets[0].tail_n == 0u && fs.planets[1].tail_n == 0u &&
          fs.planets[3].tail_n == 0u && fs.planets[1].link_open == 0u &&
          fs.planets[1].tail_overflow == 0u);

    /* ── W5: two layers agree.
     * a.bin pristine. b.bin's payload carries 1 adopted byte; the codec
     * refuses it (bfs_read -5 = decode failure) while the planet recorded
     * the cause (W3). Refusal + record, nothing silent, nothing hidden. */
    {
        static int8_t out[576];
        uint32_t act = 0;
        int ra = bfs_read(&fs, "a.bin", out, 144, &act);
        int a_ok = (ra == 0 && memcmp(out, d0, 144) == 0);
        int rb = bfs_read(&fs, "b.bin", out, 432, &act);
        CHECK("W5: a.bin pristine; b.bin refused (-5) with planet record",
              a_ok && rb == -5);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
