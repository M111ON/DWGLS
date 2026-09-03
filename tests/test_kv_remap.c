/*
 * test_kv_remap.c — Full cycle test for adaptive skeleton+delta system
 * ════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   1. Set skeleton (baseline)
 *   2. Classify at 0%, 10%, 50%, 90% change
 *   3. Store delta (adaptive format)
 *   4. Restore (reconstruct current from skeleton+delta)
 *   5. Rail scan + patch cycle
 *   6. Rebuild at 85%+
 *
 * Mock layout: 6 layers × 2 × 512 × 16 × 2 = 192KB total
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "kv_remap.h"
#include "kv_remap_rail.h"

/* ── Mock KV layout ─────────────────────────────────────── */
#define N_LAYERS    6
#define N_EMBD      512
#define N_HEAD_KV   8
#define DIM_HEAD    (N_EMBD / N_HEAD_KV)  /* 64 */

/* Each layer: K + V, each = N_EMBD × n_ctx × sizeof(f16) = 512 × n_ctx × 2 */
static uint8_t *layer_k[N_LAYERS];
static uint8_t *layer_v[N_LAYERS];
static size_t   k_size[N_LAYERS];
static size_t   v_size[N_LAYERS];
static size_t   k_nb1[N_LAYERS];
static size_t   v_nb1[N_LAYERS];
static int      n_embd_k[N_LAYERS];
static int      layer_id[N_LAYERS];

static void alloc_mock_kv(int n_ctx) {
    size_t per_layer = (size_t)N_EMBD * n_ctx * 2; /* f16 = 2 bytes */
    for (int i = 0; i < N_LAYERS; i++) {
        layer_k[i] = (uint8_t *)calloc(1, per_layer);
        layer_v[i] = (uint8_t *)calloc(1, per_layer);
        k_size[i] = per_layer;
        v_size[i] = per_layer;
        k_nb1[i]  = 2;    /* f16 stride */
        v_nb1[i]  = 2;
        n_embd_k[i] = N_EMBD;
        layer_id[i] = i;

        /* Fill with pseudo-random data (seeded by layer) */
        srand((unsigned)(i * 1337 + 42));
        uint32_t *pk = (uint32_t *)layer_k[i];
        uint32_t *pv = (uint32_t *)layer_v[i];
        for (size_t w = 0; w < per_layer / 4; w++) {
            pk[w] = (uint32_t)rand();
            pv[w] = (uint32_t)rand();
        }
    }
}

static void free_mock_kv(void) {
    for (int i = 0; i < N_LAYERS; i++) {
        free(layer_k[i]);
        free(layer_v[i]);
    }
}

/* Simple xorshift32 PRNG — full 32-bit range */
static uint32_t rng_state = 12345;
static uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void seed_rng(unsigned s) { rng_state = s ? s : 1; }

/* ── Perturb: change pct% of bytes in-place ─────────────── */
static void perturb_kv(int pct) {
    if (pct == 0) return;

    size_t total = 0;
    for (int i = 0; i < N_LAYERS; i++) total += k_size[i] + v_size[i];

    size_t to_change = total * (size_t)pct / 100;

    seed_rng((unsigned)(pct * 9999 + 12345));

    for (size_t c = 0; c < to_change; c++) {
        size_t pos = (size_t)(xorshift32() % (uint32_t)total);

        size_t accum = 0;
        for (int i = 0; i < N_LAYERS; i++) {
            size_t kv_total = k_size[i] + v_size[i];
            if (pos < accum + kv_total) {
                size_t in_kv = pos - accum;
                uint8_t *data;
                size_t sz;
                if (in_kv < k_size[i]) {
                    data = layer_k[i];
                    sz = k_size[i];
                } else {
                    data = layer_v[i];
                    sz = v_size[i];
                }
                size_t byte_pos = (size_t)(xorshift32() % (uint32_t)sz);
                data[byte_pos] ^= (uint8_t)(xorshift32() & 0xFF);
                break;
            }
            accum += kv_total;
        }
    }
}


/* =============================================================
 * Test 1: Basic skeleton + classify
 * ============================================================= */
static void test_classify(void) {
    printf("\n══ Test 1: Classify at different change levels ══\n");

    int n_ctx = 128;
    alloc_mock_kv(n_ctx);

    KVRemapCtx ctx;
    kv_remap_init(&ctx, n_ctx);
    kv_remap_register(&ctx, (void **)layer_k, (void **)layer_v, k_nb1, v_nb1,
                       k_size, v_size, n_embd_k, layer_id, N_LAYERS);

    /* Set skeleton (captures current random state as baseline) */
    kv_remap_set_skeleton(&ctx);

    /* Classify at various levels */
    int tests[] = {0, 5, 10, 15, 20, 40, 50, 70, 85, 90, 100};
    int n_tests = sizeof(tests) / sizeof(tests[0]);

    printf("  Change%%  |  Detected%%  |  Method\n");
    printf("  -------- | ---------- | --------\n");

    for (int t = 0; t < n_tests; t++) {
        /* Restore from skeleton ref, then perturb in-place */
        size_t off = 0;
        for (int l = 0; l < N_LAYERS; l++) {
            memcpy(layer_k[l], ctx.ref_skeleton + off, k_size[l]);
            off += k_size[l];
            memcpy(layer_v[l], ctx.ref_skeleton + off, v_size[l]);
            off += v_size[l];
        }

        perturb_kv(tests[t]);

        int pct = kv_remap_classify(&ctx);
        const char *method;
        if (pct == 0) method = "NONE";
        else if (pct <= 15) method = "ENTROPY";
        else if (pct <= 85) method = "GEO";
        else method = "REBUILD";

        printf("  %3d%%     |  %3d%%      |  %s\n", tests[t], pct, method);
    }

    kv_remap_destroy(&ctx);
    free_mock_kv();
}


/* =============================================================
 * Test 2: Full cycle — store delta, restore, verify
 * ============================================================= */
static void test_full_cycle(void) {
    printf("\n══ Test 2: Full cycle (store delta → restore → verify) ══\n");

    int n_ctx = 128;
    alloc_mock_kv(n_ctx);

    KVRemapCtx ctx;
    kv_remap_init(&ctx, n_ctx);
    kv_remap_register(&ctx, (void **)layer_k, (void **)layer_v, k_nb1, v_nb1,
                       k_size, v_size, n_embd_k, layer_id, N_LAYERS);

    /* Set skeleton at 0% change */
    kv_remap_set_skeleton(&ctx);

    /* Store reference of current live KV */
    size_t total = ctx.total_kv_bytes;
    uint8_t *ref_live = (uint8_t *)malloc(total);
    size_t off = 0;
    for (int l = 0; l < N_LAYERS; l++) {
        memcpy(ref_live + off, layer_k[l], k_size[l]);
        off += k_size[l];
        memcpy(ref_live + off, layer_v[l], v_size[l]);
        off += v_size[l];
    }

    /* Perturb 10% */
    perturb_kv(10);

    /* Classify + store */
    int pct = kv_remap_classify(&ctx);
    kv_remap_store_delta(&ctx, pct);

    printf("  Change: %d%%, delta type=%d (%s)\n", pct, ctx.delta.type,
        ctx.delta.type == 1 ? "ENTROPY" : ctx.delta.type == 2 ? "GEO" : "?");

    /* Save post-perturbation live KV for verification */
    uint8_t *post_perturb = (uint8_t *)malloc(total);
    off = 0;
    for (int l = 0; l < N_LAYERS; l++) {
        memcpy(post_perturb + off, layer_k[l], k_size[l]);
        off += k_size[l];
        memcpy(post_perturb + off, layer_v[l], v_size[l]);
        off += v_size[l];
    }

    /* Restore: writes skeleton + applies delta → should match post-perturb */
    kv_remap_restore(&ctx);

    /* Verify restored matches post-perturb (skeleton + delta) */
    int match = 1;
    for (size_t i = 0; i < total; i++) {
        uint8_t val = 0;
        size_t toff = i;
        for (int l = 0; l < N_LAYERS; l++) {
            if (toff < k_size[l]) { val = ((uint8_t*)layer_k[l])[toff]; break; }
            toff -= k_size[l];
            if (toff < v_size[l]) { val = ((uint8_t*)layer_v[l])[toff]; break; }
            toff -= v_size[l];
        }
        if (val != post_perturb[i]) { match = 0; break; }
    }

    printf("  Delta type: %s (change=%d%%)\n",
        ctx.delta.type == 1 ? "ENTROPY" : ctx.delta.type == 2 ? "GEO" : "?",
        ctx.delta.change_pct);
    printf("  Delta: entropy=%zu geo=%zu | Skeleton: orig=%zu comp=%zu\n",
        ctx.delta.entropy_size, ctx.delta.geo_data_size,
        ctx.skeleton_orig, ctx.skeleton_comp);
    printf("  Restore %s (skeleton + delta == post-perturb)\n",
        match ? "VERIFIED ✓" : "MISMATCH ✗");

    free(ref_live);
    free(post_perturb);
    kv_remap_destroy(&ctx);
    free_mock_kv();
}


/* =============================================================
 * Test 3: Geo delta at high change
 * ============================================================= */
static void test_geo_delta(void) {
    printf("\n══ Test 3: Geo delta at 50%% change ══\n");

    int n_ctx = 256;
    alloc_mock_kv(n_ctx);

    KVRemapCtx ctx;
    kv_remap_init(&ctx, n_ctx);
    kv_remap_register(&ctx, (void **)layer_k, (void **)layer_v, k_nb1, v_nb1,
                       k_size, v_size, n_embd_k, layer_id, N_LAYERS);

    kv_remap_set_skeleton(&ctx);

    /* Perturb 50% */
    perturb_kv(50);

    int pct = kv_remap_classify(&ctx);
    kv_remap_store_delta(&ctx, pct);

    printf("  Change: %d%%, type=%d\n", pct, ctx.delta.type);
    printf("  Geo ranges: %u\n", ctx.delta.n_ranges);
    printf("  Delta storage: %zu bytes\n", ctx.delta.delta_size);

    if (ctx.delta.n_ranges > 0) {
        printf("  First 5 ranges:\n");
        for (uint32_t i = 0; i < 5 && i < ctx.delta.n_ranges; i++) {
            printf("    range[%u]: start=%u len=%u\n",
                i, ctx.delta.ranges[i].start, ctx.delta.ranges[i].length);
        }
    }

    kv_remap_destroy(&ctx);
    free_mock_kv();
}


/* =============================================================
 * Test 4: Rebuild at 90%
 * ============================================================= */
static void test_rebuild(void) {
    printf("\n══ Test 4: Rebuild at 90%% change ══\n");

    int n_ctx = 128;
    alloc_mock_kv(n_ctx);

    KVRemapCtx ctx;
    kv_remap_init(&ctx, n_ctx);
    kv_remap_register(&ctx, (void **)layer_k, (void **)layer_v, k_nb1, v_nb1,
                       k_size, v_size, n_embd_k, layer_id, N_LAYERS);

    kv_remap_set_skeleton(&ctx);
    printf("  Skeleton: %zu bytes (comp=%zu)\n", ctx.skeleton_orig, ctx.skeleton_comp);

    /* Perturb 90% */
    perturb_kv(90);

    int pct = kv_remap_classify(&ctx);
    printf("  Detected change: %d%%\n", pct);

    /* Full cycle should rebuild */
    kv_remap_rebuild(&ctx);
    printf("  After rebuild: valid=%d, comp=%zu\n", ctx.skeleton_valid, ctx.skeleton_comp);
    printf("  Rebuilds: %u\n", ctx.n_rebuilds);

    kv_remap_destroy(&ctx);
    free_mock_kv();
}


/* =============================================================
 * Test 5: Rail scan + step cycle
 * ============================================================= */
static void test_rail(void) {
    printf("\n══ Test 5: Rail background scan ══\n");

    int n_ctx = 128;
    alloc_mock_kv(n_ctx);

    KVRemapCtx ctx;
    kv_remap_init(&ctx, n_ctx);
    kv_remap_register(&ctx, (void **)layer_k, (void **)layer_v, k_nb1, v_nb1,
                       k_size, v_size, n_embd_k, layer_id, N_LAYERS);

    kv_remap_set_skeleton(&ctx);

    /* Perturb 20% */
    perturb_kv(20);

    /* Rail scan */
    KVRemapRail rail;
    kv_remap_rail_init(&rail, &ctx);
    kv_remap_rail_start_scan(&rail);

    int steps = 0;
    while (rail.state != RAIL_PARK) {
        int r = kv_remap_rail_step(&rail);
        steps++;
        if (steps > 100000) {
            fprintf(stderr, "  Rail step limit reached!\n");
            break;
        }
    }

    printf("  Rail completed in %d steps\n", steps);
    printf("  Detected change: %d%%\n", rail.change_pct);
    printf("  Scan elapsed: %.1f ms\n", rail.scan_elapsed_ms);

    kv_remap_rail_print_status(&rail);

    kv_remap_rail_destroy(&rail);
    kv_remap_destroy(&ctx);
    free_mock_kv();
}


/* =============================================================
 * Test 6: Rail freeze / resume
 * ============================================================= */
static void test_rail_freeze(void) {
    printf("\n══ Test 6: Rail freeze / resume ══\n");

    int n_ctx = 256;
    alloc_mock_kv(n_ctx);

    KVRemapCtx ctx;
    kv_remap_init(&ctx, n_ctx);
    kv_remap_register(&ctx, (void **)layer_k, (void **)layer_v, k_nb1, v_nb1,
                       k_size, v_size, n_embd_k, layer_id, N_LAYERS);

    kv_remap_set_skeleton(&ctx);
    perturb_kv(30);

    KVRemapRail rail;
    kv_remap_rail_init(&rail, &ctx);
    kv_remap_rail_start_scan(&rail);

    /* Run a few steps then freeze */
    for (int i = 0; i < 5; i++) kv_remap_rail_step(&rail);

    printf("  Before freeze:\n");
    kv_remap_rail_print_status(&rail);

    kv_remap_rail_freeze(&rail);
    printf("  After freeze:\n");
    kv_remap_rail_print_status(&rail);

    /* Simulate interrupt — rail is parked */
    kv_remap_rail_resume(&rail);
    printf("  After resume:\n");
    kv_remap_rail_print_status(&rail);

    /* Continue scan */
    while (rail.state != RAIL_PARK) {
        kv_remap_rail_step(&rail);
    }

    printf("  Final:\n");
    kv_remap_rail_print_status(&rail);

    kv_remap_rail_destroy(&rail);
    kv_remap_destroy(&ctx);
    free_mock_kv();
}


/* =============================================================
 * Test 7: Multi-turn streaming simulation
 * ============================================================= */
static void test_streaming(void) {
    printf("\n══ Test 7: Multi-turn streaming simulation ══\n");

    int n_ctx = 128;
    alloc_mock_kv(n_ctx);

    KVRemapCtx ctx;
    kv_remap_init(&ctx, n_ctx);
    kv_remap_register(&ctx, (void **)layer_k, (void **)layer_v, k_nb1, v_nb1,
                       k_size, v_size, n_embd_k, layer_id, N_LAYERS);

    /* Turn 0: set skeleton */
    kv_remap_set_skeleton(&ctx);
    printf("  Turn 0: skeleton set (%zu bytes)\n", ctx.skeleton_comp);

    /* Simulate 10 turns */
    for (int turn = 1; turn <= 10; turn++) {
        /* Each turn adds ~5% change */
        perturb_kv(turn * 5);

        int pct = kv_remap_classify(&ctx);
        kv_remap_store_delta(&ctx, pct);

        printf("  Turn %d: change=%d%%, delta_type=%d, delta_size=%zu\n",
            turn, pct, ctx.delta.type, ctx.delta.delta_size);

        /* At 85%+ the system would rebuild */
        if (pct >= 85) {
            printf("  Turn %d: REBUILD triggered\n", turn);
            kv_remap_rebuild(&ctx);
            printf("  Turn %d: new skeleton set\n", turn);
        }
    }

    kv_remap_print_status(&ctx);
    kv_remap_destroy(&ctx);
    free_mock_kv();
}


/* =============================================================
 * Main
 * ============================================================= */
int main(void) {
    printf("═══════════════════════════════════════════════\n");
    printf("  KV Remap Test Suite\n");
    printf("═══════════════════════════════════════════════\n");

    test_classify();
    test_full_cycle();
    test_geo_delta();
    test_rebuild();
    test_rail();
    test_rail_freeze();
    test_streaming();

    printf("\n═══════════════════════════════════════════════\n");
    printf("  All tests complete\n");
    printf("═══════════════════════════════════════════════\n");

    return 0;
}
