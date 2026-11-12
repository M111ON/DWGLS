/*
 * test_b_fs_kv.c — Phase 3 KV cache on b_fs verification
 * T1: put/get roundtrip for multiple entries
 * T2: has() reflects stored entries
 * T3: partial last block + larger payload
 * T4: persistence across save/load image
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -o build/test_b_fs_kv tests/test_b_fs_kv.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_fs_kv.h"
#include "bfs_persist.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; printf("  OK   %s\n", msg); } \
    else { fail++; printf("  FAIL %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Phase 3: KV cache on b_fs ===\n");

    BreathingFS fs; bfs_init(&fs);
    BfsKvCache kv; bfs_kv_init(&kv, &fs);

    /* T1: put/get roundtrip */
    {
        uint8_t v1[100], v2[256];
        for (int i = 0; i < 100; i++) v1[i] = (uint8_t)(i + 5);
        for (int i = 0; i < 256; i++) v2[i] = (uint8_t)(i ^ 0x33);
        CHECK(bfs_kv_put(&kv, "k1", v1, 100) == 0, "T1 put k1");
        CHECK(bfs_kv_put(&kv, "k2", v2, 256) == 0, "T1 put k2");
        uint8_t r1[100], r2[256]; uint32_t a1 = 0, a2 = 0;
        CHECK(bfs_kv_get(&kv, "k1", r1, 100, &a1) == 0, "T1 get k1");
        CHECK(a1 == 100 && memcmp(v1, r1, 100) == 0, "T1 k1 roundtrip");
        CHECK(bfs_kv_get(&kv, "k2", r2, 256, &a2) == 0, "T1 get k2");
        CHECK(a2 == 256 && memcmp(v2, r2, 256) == 0, "T1 k2 roundtrip");
    }

    /* T2: has() */
    {
        CHECK(bfs_kv_has(&kv, "k1") == 1, "T2 has k1");
        CHECK(bfs_kv_has(&kv, "k2") == 1, "T2 has k2");
        CHECK(bfs_kv_has(&kv, "nope") == 0, "T2 !has nope");
    }

    /* T3: partial last block (144*2+12) */
    {
        uint32_t sz = 144 * 2 + 12;
        uint8_t *d = (uint8_t*)malloc(sz);
        for (uint32_t i = 0; i < sz; i++) d[i] = (uint8_t)(i & 0xFF);
        CHECK(bfs_kv_put(&kv, "partial", d, sz) == 0, "T3 put partial");
        uint8_t *r = (uint8_t*)malloc(sz);
        uint32_t a = 0;
        CHECK(bfs_kv_get(&kv, "partial", r, sz, &a) == 0, "T3 get partial");
        CHECK(a == sz && memcmp(d, r, sz) == 0, "T3 partial roundtrip");
        free(d); free(r);
    }

    /* T4: persistence via image save/load */
    {
        CHECK(bfs_save_img("build/t_kv.img", &fs) == 0, "T4 save img");
        BreathingFS fs2; bfs_init(&fs2);
        CHECK(bfs_load_img("build/t_kv.img", &fs2) == 0, "T4 load img");
        BfsKvCache kv2; bfs_kv_init(&kv2, &fs2);
        uint8_t r[100]; uint32_t a = 0;
        CHECK(bfs_kv_get(&kv2, "k1", r, 100, &a) == 0, "T4 get k1 after reload");
        CHECK(a == 100, "T4 size preserved");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}
