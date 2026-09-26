/* test_bfs_quadtree.c — live-block quadtree over 144 BFS blocks.
 * Oracles: fs->n_blocks_used (independent counter), file ranges from
 * home_block+n_blocks, brute linear scan. Rebuild per query.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/bfs_quadtree.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static BreathingFS fs;
static BQTree t;
static uint8_t d[144 * 4];

int main(void) {
    memset(d, 0xA5, sizeof(d));
    bfs_init(&fs);

    /* T1: empty FS — root 0, no live. */
    bqt_build(&fs, &t);
    CHECK(bqt_count(&t) == 0, "empty root");
    CHECK(bqt_first_live(&t) == -1, "empty none");

    /* T2: two files — root == n_blocks_used (independent oracle). */
    CHECK(bfs_write(&fs, "a", (int8_t *)d, 300) == 0, "write a");   /* 3 blocks */
    CHECK(bfs_write(&fs, "b", (int8_t *)d, 500) == 0, "write b");   /* 4 blocks */
    bqt_build(&fs, &t);
    CHECK(bqt_count(&t) == fs.n_blocks_used, "root == used");
    CHECK(fs.n_blocks_used == 7, "7 blocks");

    /* T3: iteration visits exactly the file ranges, in order. */
    {
        uint8_t expect[144] = {0};
        for (uint32_t i = 0; i < fs.n_files; i++)
            for (uint32_t b = 0; b < fs.files[i].n_blocks; b++)
                expect[fs.files[i].home_block + b] = 1;
        int n = 0, prev = -2, ok = 1;
        for (int b = bqt_first_live(&t); b >= 0; b = bqt_next_live(&t, b)) {
            if (!expect[b] || b <= prev) { ok = 0; break; }
            prev = b; n++;
            if (n > 144) { ok = 0; break; }
        }
        CHECK(ok && (uint32_t)n == fs.n_blocks_used, "iterate == ranges");
    }

    /* T4: delete middle — quadrant empties, iteration jumps it. */
    CHECK(bfs_delete(&fs, "a") == 0, "delete a");
    bqt_build(&fs, &t);
    CHECK(bqt_count(&t) == fs.n_blocks_used, "root after delete");
    CHECK(bqt_first_live(&t) == (int)fs.files[0].home_block, "first is b");
    {
        /* brute: every live block found, no dead block returned. */
        uint8_t expect[144] = {0};
        for (uint32_t i = 0; i < fs.n_files; i++)
            if (fs.files[i].valid)
                for (uint32_t b = 0; b < fs.files[i].n_blocks; b++)
                    expect[fs.files[i].home_block + b] = 1;
        int ok = 1, n = 0;
        for (int b = bqt_first_live(&t); b >= 0; b = bqt_next_live(&t, b)) {
            if (!expect[b]) { ok = 0; break; }
            n++;
        }
        uint32_t total = 0;
        for (int i = 0; i < 144; i++) total += expect[i];
        CHECK(ok && (uint32_t)n == total, "jump dead quadrant");
    }

    /* T5: quadrant counts agree with brute per L2 cell. */
    {
        int ok = 1;
        for (uint32_t cx = 0; cx < 4 && ok; cx++)
            for (uint32_t cy = 0; cy < 4 && ok; cy++) {
                /* count blocks whose L2 cell is (cx,cy). */
                uint32_t brute = 0;
                for (int b = 0; b < 144; b++)
                    if (t.live[b] && (b / 12) / 4 == (int)cx && (b % 12) / 4 == (int)cy)
                        brute++;
                if (bqt_quad(&t, 2, cx, cy) != brute) ok = 0;
            }
        CHECK(ok, "L2 quads == brute");
    }

    if (!fails) printf("bfs_quadtree: ALL PASS\n");
    return fails ? 1 : 0;
}
