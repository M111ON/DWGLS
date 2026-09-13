/*
 * test_bfs_persist_planets.c — v4 image: tombs + counters survive restart
 * ═══════════════════════════════════════════════════════════════════════════
 * Planets rebirth with EXACT digests (payloads persisted); tomb archive +
 * 3 counters persist; tails don't (documented). v3 files still load
 * (forged in-test: version->3, truncated after v3 CRC).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_bfs_persist_planets tests/test_bfs_persist_planets.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "bfs_persist.h"
#include "bfs_breath.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

#define IMG_PATH "build/t_pplanets.img"

static void fill_buf(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31 + i * 7 + (i >> 3)) & 0xFF);
}

int main(void) {
    printf("═ PERSIST PLANETS — v4 roundtrip ═\n");
    BreathingFS fs;
    int8_t d0[144], d1[432];
    bfs_init(&fs);
    fill_buf(d0, 144, 1);
    fill_buf(d1, 432, 2);
    fs.seeker.current_pos = 500; fs.seeker.home_pos = 500;
    bfs_write(&fs, "a.bin", d0, 144);       /* block 0 */
    fs.seeker.current_pos = 1200; fs.seeker.home_pos = 1200;
    bfs_write(&fs, "b.bin", d1, 432);       /* blocks 1..3 */

    BFSBreath b;
    bfs_breath_init(&b, &fs, 0.05);
    for (int i = 0; i < 50; i++) bfs_breath_tick(&b);

    /* one transient trouble: counter=1, tail=1 (tail must NOT persist) */
    uint32_t hit = 9u % fs.block_encoded_size[0];
    uint8_t save = fs.block_encoded[0][hit];
    fs.block_encoded[0][hit] ^= 0x40;
    bfs_breath_tick(&b);
    fs.block_encoded[0][hit] = save;
    bfs_breath_tick(&b);

    uint64_t dig0 = fs.planets[0].digest;
    bfs_delete(&fs, "b.bin");               /* 3 tombs archived */

    /* ── V1: save + load — tombs, counters, digests survive ── */
    CHECK("V1: save ok", bfs_save_img(IMG_PATH, &fs) == 0);
    {
        BreathingFS fs2;
        int rc = bfs_load_img(IMG_PATH, &fs2);
        int8_t out[144];
        uint32_t act = 0;
        int reads = (rc == 0 && bfs_read(&fs2, "a.bin", out, 144, &act) == 0 &&
                     memcmp(out, d0, 144) == 0);
        int tomb = (fs2.tombs[1].magic == PLANET_TOMB_MAGIC &&
                    fs2.tombs[1].id == 1u && fs2.tombs[1].final_home == 1u &&
                    fs2.tombs[1].origin == fs2.tombs[1].digest &&
                    fs2.tombs[2].magic == PLANET_TOMB_MAGIC &&
                    fs2.tombs[3].magic == PLANET_TOMB_MAGIC &&
                    fs2.tomb_count == 3u);
        int rebirth = (fs2.planets[0].magic == PLANET_MAGIC &&
                       fs2.planets[0].digest == dig0 &&
                       fs2.planets[0].tail_n == 0u &&
                       fs2.planets[0].link_open == 0u);
        int counters = (fs2.planet_mismatch == 1u && fs2.fold_count == 0u);
        CHECK("V1: reads lossless, 3 tombs exact, digest rebirthed, counter kept",
              reads && tomb && rebirth && counters);

        /* post-load ticks stay healthy (rebirth is a valid baseline) */
        BFSBreath b2;
        bfs_breath_init(&b2, &fs2, 0.05);
        for (int i = 0; i < 100; i++) bfs_breath_tick(&b2);
        CHECK("V1b: 100 post-load ticks healthy",
              fs2.planet_mismatch == 1u && bfs_breath_all_bounded(&b2));
    }

    /* ── V2: v3 file tolerance (forged: version 3, cut after v3 CRC) ── */
    {
        static uint8_t img[BFS_IMG_MAX_SIZE];
        uint32_t n = bfs_img_serialize(&fs, img);
        (void)n;
        uint32_t data_size = bfs_img_u32(img, BFS_HDR_DATA_SIZE);
        uint32_t v3end = BFS_IMG_DATA_OFF + data_size + 4u;
        /* forge a genuine v3 file: version back to 3, v3 CRC recomputed
         * over the forged bytes, then cut after the v3 CRC */
        bfs_img_wu32(img, 4, 3u);
        bfs_img_wu32(img, v3end - 4u, v6b_dc_crc32(img, v3end - 4u));
        BreathingFS fs3;
        int rc = bfs_img_parse(img, v3end, &fs3, NULL);
        int8_t out[144];
        uint32_t act = 0;
        CHECK("V2: v3 image loads (tombs zeroed, planets rebirthed, reads ok)",
              rc == 0 && fs3.tombs[1].magic == 0u && fs3.tomb_count == 0u &&
              fs3.planets[0].magic == PLANET_MAGIC &&
              fs3.planets[0].digest == dig0 &&
              bfs_read(&fs3, "a.bin", out, 144, &act) == 0 &&
              memcmp(out, d0, 144) == 0);
    }

    /* ── V3: tomb tamper trips the tail CRC (strict, like v3 CRC) ── */
    {
        static uint8_t img[BFS_IMG_MAX_SIZE];
        uint32_t n = bfs_img_serialize(&fs, img);
        uint32_t data_size = bfs_img_u32(img, BFS_HDR_DATA_SIZE);
        uint32_t t = BFS_IMG_DATA_OFF + data_size + 4u;
        img[t + 40 + 8] ^= 0x01;   /* tomb[1].birth_w */
        BreathingFS fs4;
        CHECK("V3: 1 flipped tomb byte refuses (-4)",
              bfs_img_parse(img, n, &fs4, NULL) == -4);
    }

    remove(IMG_PATH);
    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
