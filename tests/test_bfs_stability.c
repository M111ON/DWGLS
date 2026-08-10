/*
 * test_bfs_stability.c — Stability regression for the Breathing FS stack.
 *
 * T1: PARTIAL-LAST-BLOCK overrun (Aug 10, 2026 stability audit):
 *     dyn_decode wrote a full 144-slot block even when a file's last block
 *     was partial (total_bytes % 144 != 0) — buffer overrun up to 143 B
 *     past the caller's exact-size buffer. Contained by a byte canary.
 *
 * T2: edge wrap — home at slot 20735, scale < 1 → delta stays in range.
 * T3: CRC corruption detection (flip byte → parse rejected).
 */
#include <stdio.h>
#include <string.h>
#include "breathing_fs.h"
#include "bfs_persist.h"
static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { checks++; \
    if (!(cond)) { failures++; printf("  FAIL: %s\n", msg); } \
    else printf("  PASS: %s\n", msg); } while (0)

static void fill_file(int8_t *d, uint32_t n, int seed)
{
    for (uint32_t i = 0; i < n; i++) d[i] = (int8_t)((i * seed + i / 7) % 101);
}

int main(void)
{
    printf("Stability Regression Suite\n");
    printf("══════════════════════════\n");

    /* ── T1: partial last block must not write past out_size ── */
    printf("TEST 1: Partial-Last-Block Buffer Overrun (canary)\n");
    {
        static uint8_t guard[2048];
        memset(guard, 0xA5, sizeof(guard));
        int8_t *out = (int8_t *)(guard + 1024);   /* exact 300 B, canaries both sides */

        BreathingFS fs; bfs_init(&fs);
        int8_t d[300]; fill_file(d, 300, 11);
        bfs_write(&fs, "partial.bin", d, 300);      /* 3 blocks, last is 12/144 */

        static uint8_t img[BFS_IMG_MAX_SIZE];
        uint32_t n = bfs_img_serialize(&fs, img);
        BreathingFS fs2; bfs_init(&fs2);
        CHECK(bfs_img_parse(img, n, &fs2, 0) == 0, "parse roundtrip");

        uint32_t actual = 0;
        int rc = bfs_read(&fs2, "partial.bin", out, 300, &actual);
        int overrun = 0;
        for (int i = 0; i < 1024; i++) if (guard[i] != 0xA5) overrun++;
        for (int i = 1024 + 300; i < 2048; i++) if (guard[i] != 0xA5) overrun++;
        CHECK(rc == 0 && actual == 300, "read ok, actual == total_bytes");
        CHECK(memcmp(out, d, 300) == 0, "plain read lossless");
        CHECK(overrun == 0, "no overrun past exact-size buffer");

        /* mmap path — same guard */
        BFSMmapFS mfs;
        CHECK(bfs_save_img("build/t_stab.img", &fs) == 0, "save img");
        CHECK(bfs_mmap_open("build/t_stab.img", &mfs) == 0, "mmap open");
        memset(guard, 0xA5, sizeof(guard));
        rc = bfs_mmap_read(&mfs, "partial.bin", out, 300, &actual);
        overrun = 0;
        for (int i = 0; i < 1024; i++) if (guard[i] != 0xA5) overrun++;
        for (int i = 1024 + 300; i < 2048; i++) if (guard[i] != 0xA5) overrun++;
        CHECK(rc == 0 && memcmp(out, d, 300) == 0, "mmap read lossless");
        CHECK(overrun == 0, "mmap: no overrun past exact-size buffer");
        bfs_mmap_close(&mfs);
    }

    /* ── T2: edge wrap — home at 20735 (max slot) ── */
    printf("TEST 2: Edge Wrap — home at max slot\n");
    {
        BreathingFS fs; bfs_init(&fs);
        fs.seeker.current_pos = 20735; fs.seeker.home_pos = 20735;
        int8_t d[144]; fill_file(d, 144, 5);
        bfs_write(&fs, "edge.bin", d, 144);
        bfs_move_seeker(&fs, 0.5);              /* scale down — delta negative */
        CHECK(fs.block_meta[0].delta <= 0, "scale<1 → delta ≤ 0");
        CHECK(fs.block_meta[0].current_pos < BFS_TOTAL_SLOTS, "current_pos wraps");
        bfs_go_home(&fs);
        int8_t out[144]; uint32_t a;
        CHECK(bfs_read(&fs, "edge.bin", out, 144, &a) == 0 &&
              memcmp(out, d, 144) == 0, "lossless after edge-wrap + go_home");
    }

    /* ── T3: CRC corruption detection ── */
    printf("TEST 3: CRC Corruption Detection\n");
    {
        BreathingFS fs; bfs_init(&fs);
        int8_t d[144]; fill_file(d, 144, 3);
        bfs_write(&fs, "x.bin", d, 144);
        bfs_save_img("build/t_stab.img", &fs);
        FILE *f = fopen("build/t_stab.img", "r+b");
        CHECK(f != NULL, "open r+b");
        if (f) {
            fseek(f, 200, SEEK_SET);              /* inside TOC region */
            uint8_t b; fread(&b, 1, 1, f); b ^= 0xFF;
            fseek(f, 200, SEEK_SET); fwrite(&b, 1, 1, f);
            fclose(f);
            BFSMmapFS mfs;
            CHECK(bfs_mmap_open("build/t_stab.img", &mfs) == -4,
                  "any corruption in [0,data_end) → CRC fails (rc=-4)");
        }
    }

    /* ── T4: extreme scale must not overflow window (stability fix) ── */
    printf("TEST 4: Extreme Scale Guard\n");
    {
        BreathingSeeker s; seeker_init(&s);
        seeker_scale(&s, 1e-9);
        CHECK(s.space_size >= 1, "space_size clamped ≥ 1 (no div-by-zero)");
        CHECK(s.window > 0, "window = K/scale stays positive (no uint32 wrap)");
        CHECK(s.is_hyperbolic == 1, "extreme small scale → hyperbolic");
        seeker_scale(&s, 0.0);
        CHECK(s.scale == 1e-6, "non-positive scale rejected (state unchanged)");
    }

    printf("\n══════════════════════════\nRESULT: %d PASS / %d FAIL\n",
           checks - failures, failures);
    return failures == 0 ? 0 : 1;
}