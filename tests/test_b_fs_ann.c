/*
 * test_b_fs_ann.c — Phase 4 ANN index on b_fs verification
 * T1: put entries, count
 * T2: addr_of returns registered flat address
 * T3: get payload by name (search -> address -> bytes) lossless
 * T4: value-blindness — search returns addresses, not payload
 * T5: persistence across save/load
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -o build/test_b_fs_ann tests/test_b_fs_ann.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_fs_ann.h"
#include "bfs_persist.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; printf("  OK   %s\n", msg); } \
    else { fail++; printf("  FAIL %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Phase 4: ANN index on b_fs ===\n");

    BreathingFS fs; bfs_init(&fs);
    BfsAnnIndex idx; bfs_ann_init(&idx, &fs);

    uint8_t p1[64], p2[128];
    for (int i = 0; i < 64; i++) p1[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 128; i++) p2[i] = (uint8_t)(i ^ 0x5A);

    /* T1 */
    CHECK(bfs_ann_put(&idx, "e1", p1, 64, 1000) == 0, "T1 put e1");
    CHECK(bfs_ann_put(&idx, "e2", p2, 128, 2000) == 0, "T1 put e2");
    CHECK(bfs_ann_count(&idx) == 2, "T1 count == 2");

    /* T2 */
    CHECK(bfs_ann_addr_of(&idx, "e1") == 1000, "T2 e1 addr == 1000");
    CHECK(bfs_ann_addr_of(&idx, "e2") == 2000, "T2 e2 addr == 2000");
    CHECK(bfs_ann_addr_of(&idx, "nope") == 0xFFFFFFFFu, "T2 missing -> 0xFFFFFFFF");

    /* T3 */
    {
        uint8_t r1[64], r2[128]; uint32_t a1 = 0, a2 = 0;
        CHECK(bfs_ann_get(&idx, "e1", r1, 64, &a1) == 0, "T3 get e1");
        CHECK(a1 == 64 && memcmp(p1, r1, 64) == 0, "T3 e1 payload lossless");
        CHECK(bfs_ann_get(&idx, "e2", r2, 128, &a2) == 0, "T3 get e2");
        CHECK(a2 == 128 && memcmp(p2, r2, 128) == 0, "T3 e2 payload lossless");
    }

    /* T4: value-blindness — addr_of returns address, not data */
    {
        uint32_t a = bfs_ann_addr_of(&idx, "e1");
        CHECK(a == 1000 && a < BFS_PAIR_N, "T4 address in field range, not payload");
    }

    /* T5: persistence */
    {
        CHECK(bfs_save_img("build/t_ann.img", &fs) == 0, "T5 save img");
        BreathingFS fs2; bfs_init(&fs2);
        CHECK(bfs_load_img("build/t_ann.img", &fs2) == 0, "T5 load img");
        BfsAnnIndex idx2; bfs_ann_init(&idx2, &fs2);
        /* re-register metadata (index struct is in-memory; BFS holds bytes) */
        /* verify bytes survived via BFS directly */
        uint8_t r[64]; uint32_t a = 0;
        CHECK(bfs_read(&fs2, "e1", (int8_t*)r, 64, &a) == 0, "T5 e1 bytes survive");
        CHECK(a == 64 && memcmp(p1, r, 64) == 0, "T5 e1 payload intact after reload");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}
