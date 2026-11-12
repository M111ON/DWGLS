/*
 * test_b_fs_base.c — Phase 1 foundation verification
 * T1: BFS write/read lossless (incl. partial last block)
 * T2: Twin hot/cold roundtrip lossless
 * T3: Pair transfer bijection over all 20736 addresses
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -o build/test_b_fs_base tests/test_b_fs_base.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_fs_base.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; printf("  OK   %s\n", msg); } \
    else { fail++; printf("  FAIL %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Phase 1: b_fs base verification ===\n");

    /* T1: BFS write/read lossless */
    {
        BreathingFS fs; bfs_init(&fs);
        uint8_t data[300];
        for (int i = 0; i < 300; i++) data[i] = (uint8_t)((i * 37 + 11) & 0xFF);
        CHECK(bfs_write(&fs, "t1.bin", (const int8_t*)data, 300) == 0, "T1 write 300B (partial last block)");
        uint8_t out[300]; uint32_t act = 0;
        CHECK(bfs_read(&fs, "t1.bin", (int8_t*)out, 300, &act) == 0, "T1 read ok");
        CHECK(act == 300, "T1 actual size == 300");
        CHECK(memcmp(data, out, 300) == 0, "T1 roundtrip memcmp");
        uint8_t orig[300]; memcpy(orig, data, 300);
        CHECK(bfs_verify_file(&fs, "t1.bin", (const int8_t*)orig, 300) == 0, "T1 verify_file");
    }

    /* T2: Twin hot/cold roundtrip */
    {
        uint8_t orig[256];
        for (int i = 0; i < 256; i++) orig[i] = (uint8_t)(i ^ 0xA5);
        uint8_t hot[256], cold[256];
        memcpy(hot, orig, 256);
        BfsTwin t; bfs_twin_init(&t, hot, cold, 256);
        CHECK(t.separate == 1, "T2 twin is separate (hot != cold)");
        bfs_twin_sync(&t);
        CHECK(bfs_twin_roundtrip_ok(orig, &t), "T2 cold == orig after sync");
        /* aliased twin */
        BfsTwin a; bfs_twin_init(&a, hot, hot, 256);
        CHECK(a.separate == 0, "T2 aliased twin separate==0");
    }

    /* T3: Pair transfer bijection over all addresses */
    {
        int ok = 1;
        for (uint32_t x = 0; x < BFS_PAIR_N; x++) {
            if (!bfs_pair_roundtrip_ok(BFS_DIM_FLAT, BFS_DIM_TESS, x)) { ok = 0; break; }
            if (!bfs_pair_roundtrip_ok(BFS_DIM_TESS, BFS_DIM_KIS, x)) { ok = 0; break; }
            if (!bfs_pair_roundtrip_ok(BFS_DIM_FLAT, BFS_DIM_KIS, x)) { ok = 0; break; }
        }
        CHECK(ok, "T3 pair roundtrip identity for all 20736 addresses");
        /* same-dim identity */
        CHECK(bfs_pair_transfer(BFS_DIM_FLAT, BFS_DIM_FLAT, 12345) == 12345, "T3 same-dim identity");
        /* transfer is bijective: distinct inputs -> distinct outputs */
        /* spot check a small range */
        uint8_t seen[BFS_PAIR_N]; memset(seen, 0, sizeof(seen));
        int bijective = 1;
        for (uint32_t x = 0; x < 1000; x++) {
            uint32_t y = bfs_pair_transfer(BFS_DIM_FLAT, BFS_DIM_TESS, x);
            if (y >= BFS_PAIR_N || seen[y]) { bijective = 0; break; }
            seen[y] = 1;
        }
        CHECK(bijective, "T3 transfer injective on 1000 samples");
    }

    /* T4: modinv correctness */
    {
        uint32_t inv = bfs_pair_modinv(BFS_PAIR_A, BFS_PAIR_N);
        CHECK(( (uint64_t)BFS_PAIR_A * inv ) % BFS_PAIR_N == 1, "T4 modular inverse correct");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}
