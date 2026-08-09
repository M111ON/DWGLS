/*
 * test_breathing_fs.c — Geometric File System via Breathing Seeker
 * ═══════════════════════════════════════════════════════════════════
 * Tests:
 *   T1: Init + write + read roundtrip (lossless)
 *   T2: Multiple files (directory)
 *   T3: Delta tracking (seeker movement)
 *   T4: Hyperbolic crossing (scale < 0.25)
 *   T5: Go-home restores lossless state
 *   T6: Compression ratio per file
 *   T7: Large file (multi-block)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/test_breathing_fs
 *        tests/test_breathing_fs.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "breathing_fs.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ══════════════════════════════════════════════════════════════════
   T1: Init + write + read roundtrip
   ══════════════════════════════════════════════════════════════════ */
static void test_init_write_read(void)
{
    printf("TEST 1: Init + Write + Read Roundtrip\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingFS fs;
    bfs_init(&fs);

    CHECK(1, "init succeeds", fs.magic == BFS_MAGIC);
    CHECK(2, "0 files initially", fs.n_files == 0);
    CHECK(3, "seeker at home", seeker_is_home(&fs.seeker));

    /* Write a file */
    int8_t data[144];
    for (int i = 0; i < 144; i++) data[i] = (int8_t)(i * 3);

    int rc = bfs_write(&fs, "test.bin", data, 144);
    CHECK(4, "write succeeds", rc == 0);
    CHECK(5, "1 file in directory", fs.n_files == 1);
    CHECK(6, "1 block used", fs.n_blocks_used == 1);

    /* Read back */
    int8_t recon[144];
    uint32_t actual = 0;
    rc = bfs_read(&fs, "test.bin", recon, 144, &actual);
    CHECK(7, "read succeeds", rc == 0);
    CHECK(8, "correct size", actual == 144);

    /* Verify lossless */
    rc = bfs_verify_file(&fs, "test.bin", data, 144);
    CHECK(9, "lossless roundtrip", rc == 0);

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T2: Multiple files
   ══════════════════════════════════════════════════════════════════ */
static void test_multiple_files(void)
{
    printf("TEST 2: Multiple Files\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingFS fs;
    bfs_init(&fs);

    /* File 1: sparse (zeros) */
    int8_t sparse[288];
    memset(sparse, 0, sizeof(sparse));
    sparse[10] = 42;
    sparse[200] = 7;

    /* File 2: repeated pattern */
    int8_t repeated[144];
    for (int i = 0; i < 144; i++) repeated[i] = (int8_t)(i % 5);

    /* File 3: random */
    int8_t random_data[432];
    srand(42);
    for (int i = 0; i < 432; i++) random_data[i] = (int8_t)(rand() % 256 - 128);

    int rc1 = bfs_write(&fs, "sparse.bin", sparse, 288);
    int rc2 = bfs_write(&fs, "repeated.bin", repeated, 144);
    int rc3 = bfs_write(&fs, "random.bin", random_data, 432);

    CHECK(1, "write sparse", rc1 == 0);
    CHECK(2, "write repeated", rc2 == 0);
    CHECK(3, "write random", rc3 == 0);
    CHECK(4, "3 files in directory", fs.n_files == 3);

    /* Verify all lossless */
    int v1 = bfs_verify_file(&fs, "sparse.bin", sparse, 288);
    int v2 = bfs_verify_file(&fs, "repeated.bin", repeated, 144);
    int v3 = bfs_verify_file(&fs, "random.bin", random_data, 432);

    CHECK(5, "sparse lossless", v1 == 0);
    CHECK(6, "repeated lossless", v2 == 0);
    CHECK(7, "random lossless", v3 == 0);

    bfs_print_dir(&fs);
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T3: Delta tracking
   ══════════════════════════════════════════════════════════════════ */
static void test_delta_tracking(void)
{
    printf("TEST 3: Delta Tracking\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingFS fs;
    bfs_init(&fs);

    /* Advance seeker to non-zero position before writing */
    fs.seeker.current_pos = 1000;

    int8_t data[144];
    memset(data, 42, sizeof(data));
    bfs_write(&fs, "file.bin", data, 144);

    /* Before move: delta = 0 */
    CHECK(1, "delta=0 at home", fs.block_meta[0].delta == 0);

    /* Scale down to 0.5 → space shrinks → deltas appear */
    bfs_move_seeker(&fs, 0.5);
    CHECK(2, "delta != 0 after scale change", fs.block_meta[0].delta != 0);
    printf("  After scale=0.5: delta=%d\n", fs.block_meta[0].delta);

    /* Scale down to 0.25 → more delta */
    bfs_move_seeker(&fs, 0.25);
    printf("  After scale=0.25: delta=%d\n", fs.block_meta[0].delta);

    /* Check if hyperbolic crossed */
    CHECK(3, "hyperbolic crossed at 0.25", fs.seeker.is_hyperbolic);

    /* Go home → delta = 0 */
    bfs_go_home(&fs);
    CHECK(4, "delta=0 after go_home", fs.block_meta[0].delta == 0);
    CHECK(5, "delta=0 for block at home", fs.block_meta[0].delta == 0);

    /* Verify lossless after movement cycle */
    int rc = bfs_verify_file(&fs, "file.bin", data, 144);
    CHECK(6, "lossless after movement", rc == 0);

    bfs_delta_stats(&fs);
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T4: Hyperbolic crossing
   ══════════════════════════════════════════════════════════════════ */
static void test_hyperbolic_crossing(void)
{
    printf("TEST 4: Hyperbolic Crossing\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingSeeker s;
    seeker_init(&s);

    printf("  scale=1.0:  space=%u window=%u %s\n",
           s.space_size, s.window, s.is_hyperbolic ? "[HYPER]" : "[KIS]");

    seeker_scale(&s, 0.5);
    printf("  scale=0.5:  space=%u window=%u %s\n",
           s.space_size, s.window, s.is_hyperbolic ? "[HYPER]" : "[KIS]");
    CHECK(1, "not hyperbolic at 0.5", !s.is_hyperbolic);

    seeker_scale(&s, 0.25);
    printf("  scale=0.25: space=%u window=%u %s\n",
           s.space_size, s.window, s.is_hyperbolic ? "[HYPER]" : "[KIS]");
    CHECK(2, "hyperbolic at 0.25", s.is_hyperbolic);

    seeker_scale(&s, 0.1);
    printf("  scale=0.1:  space=%u window=%u %s\n",
           s.space_size, s.window, s.is_hyperbolic ? "[HYPER]" : "[KIS]");
    CHECK(3, "hyperbolic at 0.1", s.is_hyperbolic);

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T5: Go-home restores lossless
   ══════════════════════════════════════════════════════════════════ */
static void test_go_home_lossless(void)
{
    printf("TEST 5: Go-Home Restores Lossless\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingFS fs;
    bfs_init(&fs);

    /* Advance seeker to non-zero position */
    fs.seeker.current_pos = 500;

    int8_t data[144];
    for (int i = 0; i < 144; i++) data[i] = (int8_t)(i & 0x7F);
    bfs_write(&fs, "data.bin", data, 144);

    /* Cycle through scales */
    double scales[] = {0.5, 0.25, 0.1, 0.5, 0.05, 0.5};
    int n_scales = sizeof(scales) / sizeof(scales[0]);

    for (int i = 0; i < n_scales; i++) {
        bfs_move_seeker(&fs, scales[i]);
    }

    /* Before go-home: deltas exist */
    int has_delta = 0;
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs.block_owner[b] != 0xFFFFFFFF && fs.block_meta[b].delta != 0)
            has_delta = 1;
    }
    CHECK(1, "deltas exist before go-home", has_delta);

    /* Go home */
    bfs_go_home(&fs);

    /* Verify lossless */
    int rc = bfs_verify_file(&fs, "data.bin", data, 144);
    CHECK(2, "lossless after go-home", rc == 0);

    /* All deltas should be 0 */
    int all_zero = 1;
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs.block_owner[b] != 0xFFFFFFFF && fs.block_meta[b].delta != 0)
            all_zero = 0;
    }
    CHECK(3, "all deltas zero at home", all_zero);

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T6: Compression ratio
   ══════════════════════════════════════════════════════════════════ */
static void test_compression_ratio(void)
{
    printf("TEST 6: Compression Ratio\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingFS fs;
    bfs_init(&fs);

    /* Sparse file */
    int8_t sparse[288];
    memset(sparse, 0, sizeof(sparse));
    sparse[10] = 42;
    bfs_write(&fs, "sparse.bin", sparse, 288);

    /* Dense file */
    int8_t dense[144];
    srand(99);
    for (int i = 0; i < 144; i++) dense[i] = (int8_t)(rand() % 256 - 128);
    bfs_write(&fs, "dense.bin", dense, 144);

    /* Uniform file */
    int8_t uniform[144];
    memset(uniform, 7, sizeof(uniform));
    bfs_write(&fs, "uniform.bin", uniform, 144);

    /* Calculate ratios */
    uint32_t total_encoded = 0;
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs.block_owner[b] != 0xFFFFFFFF)
            total_encoded += fs.block_encoded_size[b];
    }

    printf("  Total encoded: %u bytes / %u raw\n", total_encoded, fs.total_bytes);
    printf("  Ratio: %.4f\n", (float)total_encoded / (float)fs.total_bytes);

    CHECK(1, "files created", fs.n_files == 3);
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T7: Large file (multi-block)
   ══════════════════════════════════════════════════════════════════ */
static void test_large_file(void)
{
    printf("TEST 7: Large File (Multi-Block)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingFS fs;
    bfs_init(&fs);

    /* 10 blocks = 1440 bytes */
    uint32_t size = 10 * BFS_SLOTS_BLOCK;
    int8_t *data = (int8_t *)malloc(size);
    srand(777);
    for (uint32_t i = 0; i < size; i++)
        data[i] = (int8_t)(rand() % 256 - 128);

    int rc = bfs_write(&fs, "large.bin", data, size);
    CHECK(1, "write 10-block file", rc == 0);
    CHECK(2, "10 blocks used", fs.n_blocks_used == 10);

    /* Verify */
    rc = bfs_verify_file(&fs, "large.bin", data, size);
    CHECK(3, "lossless roundtrip", rc == 0);

    /* Movement cycle */
    bfs_move_seeker(&fs, 0.5);
    bfs_move_seeker(&fs, 0.25);
    bfs_move_seeker(&fs, 0.5);
    bfs_go_home(&fs);

    rc = bfs_verify_file(&fs, "large.bin", data, size);
    CHECK(4, "lossless after movement", rc == 0);

    bfs_print_dir(&fs);
    free(data);
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Breathing FS — Geometric File System                   ║\n");
    printf("║  Compression = Space Movement                           ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    test_init_write_read();
    test_multiple_files();
    test_delta_tracking();
    test_hyperbolic_crossing();
    test_go_home_lossless();
    test_compression_ratio();
    test_large_file();

    printf("═══════════════════════════════════════════════════════════\n");
    if (fail == 0) {
        printf("PASS: All %d tests passed ✓\n", pass);
    } else {
        printf("PASS: %d | FAIL: %d\n", pass, fail);
    }
    printf("═══════════════════════════════════════════════════════════\n");

    return fail;
}
