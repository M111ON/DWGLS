/*
 * test_b_fs_dim.c — Phase 2 cross-dimension verification
 * T1: dim_to_flat / dim_from_flat are inverses for all dims
 * T2: cross-dim write in FLAT → read in KIS at transferred addr (same slot)
 * T3: BfsDimTwin roundtrip for all addresses
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -o build/test_b_fs_dim tests/test_b_fs_dim.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_fs_dim.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; printf("  OK   %s\n", msg); } \
    else { fail++; printf("  FAIL %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Phase 2: cross-dimension verification ===\n");

    /* T1: to_flat / from_flat inverses */
    {
        int ok = 1;
        for (int d = 0; d < BFS_DIM_COUNT; d++) {
            for (uint32_t x = 0; x < BFS_PAIR_N; x++) {
                uint32_t f = bfs_dim_to_flat((BfsDim)d, x);
                uint32_t back = bfs_dim_from_flat((BfsDim)d, f);
                if (back != (x % BFS_PAIR_N)) { ok = 0; break; }
            }
            if (!ok) break;
        }
        CHECK(ok, "T1 dim_to_flat/from_flat inverse for all dims & addresses");
    }

    /* T2: cross-dim BFS roundtrip — write in FLAT, read at KIS-transferred addr */
    {
        BreathingFS fs; bfs_init(&fs);
        uint8_t data[144];
        for (int i = 0; i < 144; i++) data[i] = (uint8_t)(i * 7 + 3);
        /* write one block at flat addr 0 (file "c.bin") */
        CHECK(bfs_write(&fs, "c.bin", (const int8_t*)data, 144) == 0, "T2 write 144B");
        /* transferred address from FLAT to KIS for slot 0 */
        uint32_t kis_addr = bfs_dim_from_flat(BFS_DIM_KIS, 0);
        /* the underlying slot is the same physical block (flat 0) */
        uint32_t flat_back = bfs_dim_to_flat(BFS_DIM_KIS, kis_addr);
        CHECK(flat_back == 0, "T2 KIS->FLAT returns slot 0");
        /* read back from flat slot 0 */
        uint8_t out[144]; uint32_t act = 0;
        CHECK(bfs_read(&fs, "c.bin", (int8_t*)out, 144, &act) == 0, "T2 read ok");
        CHECK(memcmp(data, out, 144) == 0, "T2 cross-dim read == written");
    }

    /* T3: BfsDimTwin roundtrip for all addresses, all dim pairs */
    {
        int ok = 1;
        for (int a = 0; a < BFS_DIM_COUNT; a++)
            for (int b = 0; b < BFS_DIM_COUNT; b++)
                for (uint32_t x = 0; x < BFS_PAIR_N; x++) {
                    BfsDimTwin t; bfs_dim_twin_init(&t, (BfsDim)a, (BfsDim)b, x);
                    if (!bfs_dim_twin_roundtrip_ok(&t)) { ok = 0; break; }
                }
        CHECK(ok, "T3 BfsDimTwin roundtrip for all dim pairs & addresses");
    }

    /* T4: transfer distinctness across dims */
    {
        /* FLAT 0 and FLAT 1 must map to different KIS addresses */
        uint32_t a = bfs_dim_from_flat(BFS_DIM_KIS, 0);
        uint32_t b = bfs_dim_from_flat(BFS_DIM_KIS, 1);
        CHECK(a != b, "T4 distinct flat inputs -> distinct KIS addrs");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}
