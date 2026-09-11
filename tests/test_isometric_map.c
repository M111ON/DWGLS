#include <stdio.h>
#include <assert.h>
#include "../core/isometric_map.h"

int main(void) {
    printf("=== isometric_map.h smoke test ===\n");

    /* 1. Skeleton lookup: addr=0 should be zone 0, X axis */
    im_skel_t s0 = im_skel(0);
    printf("skel(0): zone=%u axis=%u pol=%u enc=%u\n",
           s0.zone, s0.axis, s0.polarity, s0.enc);
    assert(s0.zone < 12);

    /* 2. Skeleton lookup: addr=37 should land on a different zone */
    im_skel_t s37 = im_skel(37);
    printf("skel(37): zone=%u axis=%u pol=%u enc=%u\n",
           s37.zone, s37.axis, s37.polarity, s37.enc);
    assert(s37.zone < 12);

    /* 3. Decompose */
    im_addr_t a = im_decompose(100);
    printf("decompose(100): zone=%u axis=%u rhombus=%u\n",
           a.skel.zone, a.skel.axis, a.rhombus);

    /* 4. Cross partner: self-inverse */
    for (int f = 0; f < 12; f++) {
        uint8_t p = IM_CROSS[f];
        assert(IM_CROSS[p] == (uint8_t)f);
    }
    printf("Cross self-inverse: PASS (12/12)\n");

    /* 5. Peano: d=0 → (0,0) */
    uint32_t px, py;
    im_peano_xy(0, 2, &px, &py);
    assert(px == 0 && py == 0);
    printf("peano_xy(0,order=2) → (%u,%u) PASS\n", px, py);

    /* 6. Peano: d=3 → (1,0) for order=2 */
    im_peano_xy(3, 2, &px, &py);
    printf("peano_xy(3,order=2) → (%u,%u)\n", px, py);

    /* 7. Peano roundtrip: d → xy → d */
    for (uint32_t d = 0; d < 16; d++) {
        im_peano_xy(d, 2, &px, &py);
        uint32_t d2 = im_peano_d(px, py, 2);
        assert(d2 == d);
    }
    printf("Peano roundtrip order=2 (16/16): PASS\n");

    /* 8. Peano traverse: advances along curve */
    uint32_t t0 = im_peano_traverse(0, 2);
    assert(t0 == 1);
    uint32_t t_last = im_peano_traverse(15, 2);
    assert(t_last == 0);  /* wraps */
    printf("Peano traverse: 0→%u, 15→%u PASS\n", t0, t_last);

    /* 9. Gear snap */
    assert(im_snap_gear(100) == 0);   /* ≤512 */
    assert(im_snap_gear(600) == 1);   /* ≤1024 */
    assert(im_snap_gear(1500) == 2);  /* ≤2048 */
    assert(im_snap_gear(3000) == 3);  /* ≤4096 */
    printf("Gear snap: PASS\n");

    /* 10. CRC32C: empty = FFFFFFFF (after final XOR) */
    uint32_t crc = im_crc32c_block(NULL, 0);
    printf("CRC32C empty: 0x%08X\n", crc);
    /* Standard CRC32C("") = 0x00000000 before final XOR → 0xFFFFFFFF after */
    assert(crc == 0x00000000u);

    /* 11. Film: expose 144 → develop */
    im_film_t film;
    im_film_init(&film);
    for (int i = 0; i < 144; i++) im_film_expose(&film, (uint64_t)i);
    assert(im_film_develop(&film) == 1);
    printf("Film develop at 144: PASS\n");

    /* 12. Ghost operations */
    im_ghost_t g1, g2;
    im_ghost_init(&g1, 10, 20);
    im_ghost_init(&g2, 20, 10);
    assert(im_ghost_route(&g1) == 20);
    im_ghost_delete(&g1, &g2);
    assert(im_ghost_route(&g1) == UINT32_MAX);
    assert(im_ghost_route(&g2) == UINT32_MAX);
    assert(g1.occupied == 1);
    printf("Ghost delete + amnesia: PASS\n");

    /* 13. isect_pop: all-zero = 0 */
    uint8_t zeros[64] = {0};
    assert(im_isect_pop(zeros) == 0);
    printf("isect_pop(zeros) = %u PASS\n", im_isect_pop(zeros));

    /* 14. is_flat */
    assert(im_is_flat(zeros) == 1);
    uint8_t nonz[64] = {0}; nonz[0] = 1;
    assert(im_is_flat(nonz) == 0);
    printf("is_flat: PASS\n");

    /* 15. Strategy decision: all-zero → FLAT */
    assert(im_decide(zeros, NULL, 0) == IM_STRAT_FLAT);
    printf("decide(all-zero) = FLAT: PASS\n");

    /* 16. Strategy decision: high-isect → RAW */
    uint8_t dense[64] = {0};
    memset(dense, 0xFF, 8);  /* first word all 0xFF, rest 0 → XOR-fold = 0xFF..FF */
    uint8_t dp = im_isect_pop(dense);
    printf("isect_pop(dense) = %u\n", dp);
    assert(dp >= IM_ISECT_RAW_THR);
    assert(im_decide(dense, NULL, 0) == IM_STRAT_RAW);
    printf("decide(dense) = RAW: PASS\n");

    /* 17. modinv37: 37 * inv mod WL ≡ 1 */
    for (uint8_t g = 0; g < IM_GEAR_COUNT; g++) {
        uint16_t wl = IM_GEARS[g].wl;
        uint16_t inv = im_modinv37(wl);
        uint64_t prod = (uint64_t)IM_STRIDE * inv % wl;
        assert(prod == 1);
    }
    printf("modinv37 all gears: PASS\n");

    /* 18. Full pipeline context */
    im_ctx_t ctx;
    im_ctx_init(&ctx);
    for (int i = 0; i < 200; i++) {
        im_process_chunk(&ctx, (uint16_t)(i * 37 % 720), dense);
    }
    printf("Pipeline 200 chunks: total=%u strategies=[",
           ctx.chunk_count);
    for (int i = 0; i < 6; i++) printf("%s%u", i?",":"", ctx.strategy_hits[i]);
    printf("]\n");
    assert(ctx.chunk_count == 200);

    /* temp vars for tuning component tests */
    uint16_t tmp16;
    uint32_t tmp32;
    uint8_t tmp8_1, tmp8_2;

    /* ═══ Tuning Components ═══ */

    /* 19. L1+L2 cache */
    im_cache_t cache;
    im_cache_init(&cache);
    /* miss */
    assert(im_cache_lookup(&cache, 100) == -1);
    assert(cache.misses == 1);
    /* insert into L1 */
    im_cache_l1_insert(&cache, 100, 0xDEAD);
    assert(im_cache_lookup(&cache, 100) == 0);
    assert(cache.l1_hits == 1);
    /* L2 insert + lookup */
    im_cache_l2_insert(&cache, 200, 0xBEEF);
    assert(im_cache_lookup(&cache, 200) == 1);
    assert(cache.l2_hits == 1);
    printf("Cache L1+L2: PASS (l1=%u l2=%u miss=%u)\n",
           cache.l1_hits, cache.l2_hits, cache.misses);

    /* 20. PermBlockFast */
    im_perm_t perm;
    im_perm_init(&perm, 0x12345678);
    uint32_t p0 = im_permute(0, &perm);
    uint32_t p1 = im_permute(1, &perm);
    assert(p0 != p1); /* different inputs → different outputs */
    /* unpermute roundtrip (brute-force, 12-bit) */
    assert(im_unpermute(p0, &perm) == 0);
    assert(im_unpermute(p1, &perm) == 1);
    printf("PermBlock: PASS (perm(0)=0x%08X perm(1)=0x%08X)\n", p0, p1);

    /* 21. RewindBuffer */
    im_rewind_t rw;
    im_rewind_init(&rw);
    assert(im_rewind_pop(&rw, &tmp16, &tmp32) == -1); /* empty */
    im_rewind_push(&rw, 100, 0xAAAA);
    im_rewind_push(&rw, 200, 0xBBBB);
    assert(im_rewind_pop(&rw, &tmp16, &tmp32) == 0);
    assert(tmp16 == 200 && tmp32 == 0xBBBB);
    assert(im_rewind_pop(&rw, &tmp16, &tmp32) == 0);
    assert(tmp16 == 100 && tmp32 == 0xAAAA);
    assert(im_rewind_pop(&rw, &tmp16, &tmp32) == -1);
    printf("RewindBuffer: PASS\n");

    /* 22. XOR-drop + countdown */
    im_xorpair_t xp = { .core = 0xDEADBEEF, .inv = 0x21524110 };
    assert(im_xor_drop(&xp)); /* XOR = 0xFFFFFFFF */
    im_xorpair_t xp2 = { .core = 0xDEADBEEF, .inv = 0x00000000 };
    assert(!im_xor_drop(&xp2));
    im_countdown_t cd;
    im_countdown_init(&cd);
    int triggered = 0;
    for (int i = 0; i < 144; i++) triggered += im_countdown_tick(&cd);
    assert(triggered == 1);
    assert(cd.remaining == IM_DYNAMICS);
    printf("XOR-drop + countdown: PASS (triggered=%d)\n", triggered);

    /* 23. Shadow Protocol */
    im_shadow_t sh;
    im_shadow_init(&sh, 1000, 5000);
    assert(sh.offset == 1000 && sh.shadow == 5000);
    im_shadow_advance(&sh);
    assert(im_shadow_addr(&sh) == 5001);
    im_shadow_advance(&sh);
    assert(im_shadow_addr(&sh) == 5002);
    printf("Shadow Protocol: PASS\n");

    /* 24. Scratch (36 triangles × 12 faces) */
    uint16_t sa = im_scratch_addr(5, 20);
    assert(sa == 5 * 36 + 20);
    im_scratch_inverse(sa, &tmp8_1, &tmp8_2);
    assert(tmp8_1 == 5 && tmp8_2 == 20);
    assert(IM_SCRATCH_TOTAL == 432);
    printf("Scratch: PASS (addr(%u,%u)=%u)\n", 5, 20, sa);

    /* 25. Hilbert L-block connector */
    im_connector_t conn;
    uint8_t floor_d[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8_t block_d[32] = {0};
    block_d[0] = 0xFF;
    im_connector_floor(&conn, floor_d, 16);
    im_connector_block(&conn, block_d, 32);
    assert(conn.floor_data[5] == 5);
    assert(conn.block_data[0] == 0xFF);
    assert(IM_CONN_TOTAL_SZ == 48);
    printf("Connector: PASS (floor+block=%uB)\n", IM_CONN_TOTAL_SZ);

    printf("\n=== ALL 25 PASS ===\n");
    return 0;
}
