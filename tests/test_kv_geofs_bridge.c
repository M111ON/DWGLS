/*
 * test_kv_geofs_bridge.c — KV Remap ↔ GeoFS wire test
 * ═════════════════════════════════════════════════════
 *
 * Proves the lossless park/resume contract through GeoFS:
 *   snapshot(live) → wipe → resume → byte-for-byte == live
 *
 * Independent oracle: golden copies kept in heap buffers,
 * compared with memcmp — not with kv_remap internals.
 *
 * Paths:
 *   T1  ENTROPY delta (8% contiguous change) roundtrip
 *   T2  GEO delta (50% contiguous change) roundtrip
 *   T3  multi-turn: two snapshots, resume each turn's exact state
 *   T4  drop frees blocks; snapshot overwrite does not leak
 *   T5  NONE delta (no change) roundtrip
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "kv_geofs_bridge.h"

#define NLAYERS   2
#define LAYER_SZ  (64u * 1024u)
#define TOTAL     (NLAYERS * 2 * LAYER_SZ)   /* K+V per layer */

static GeosVolume vol;
static uint8_t *kbuf[NLAYERS], *vbuf[NLAYERS];
static int g_fail = 0;

static void *xalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "OOM\n"); exit(1); }
    return p;
}

/* deterministic fill (xorshift) — oracle data, not from codec */
static void fill_pattern(uint8_t *buf, size_t n, uint32_t seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] = (uint8_t)(s & 0xFF);
    }
}

/* perturb first `nbytes` bytes deterministically */
static void perturb(uint8_t *buf, size_t nbytes, uint32_t seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < nbytes; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] ^= (uint8_t)(s | 1);
    }
}

static void layers_alloc(void) {
    for (int l = 0; l < NLAYERS; l++) {
        kbuf[l] = (uint8_t *)xalloc(LAYER_SZ);
        vbuf[l] = (uint8_t *)xalloc(LAYER_SZ);
    }
}

static void layers_free(void) {
    for (int l = 0; l < NLAYERS; l++) { free(kbuf[l]); free(vbuf[l]); }
}

static void layers_register(KVRemapCtx *ctx) {
    size_t k_nb1[NLAYERS], v_nb1[NLAYERS], k_sz[NLAYERS], v_sz[NLAYERS];
    void *kd[NLAYERS], *vd[NLAYERS];
    int ne[NLAYERS], lid[NLAYERS];
    for (int l = 0; l < NLAYERS; l++) {
        kd[l] = kbuf[l]; vd[l] = vbuf[l];
        k_nb1[l] = v_nb1[l] = LAYER_SZ / 16;
        k_sz[l] = v_sz[l] = LAYER_SZ;
        ne[l] = 16; lid[l] = l;
    }
    kv_remap_register(ctx, kd, vd, k_nb1, v_nb1, k_sz, v_sz, ne, lid, NLAYERS);
}

static void layers_wipe(void) {
    for (int l = 0; l < NLAYERS; l++) {
        memset(kbuf[l], 0, LAYER_SZ);
        memset(vbuf[l], 0, LAYER_SZ);
    }
}

static void golden_take(uint8_t *dst) {
    size_t off = 0;
    for (int l = 0; l < NLAYERS; l++) {
        memcpy(dst + off, kbuf[l], LAYER_SZ); off += LAYER_SZ;
        memcpy(dst + off, vbuf[l], LAYER_SZ); off += LAYER_SZ;
    }
}

static int golden_cmp(const uint8_t *g) {
    size_t off = 0;
    for (int l = 0; l < NLAYERS; l++) {
        if (memcmp(g + off, kbuf[l], LAYER_SZ) != 0) return -(int)(off + 1);
        off += LAYER_SZ;
        if (memcmp(g + off, vbuf[l], LAYER_SZ) != 0) return -(int)(off + 1);
        off += LAYER_SZ;
    }
    return 0;
}

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int stop_at = getenv("KVG_STOP") ? atoi(getenv("KVG_STOP")) : 5;
    geos_volume_init(&vol);
    layers_alloc();
    uint8_t *golden = (uint8_t *)xalloc(TOTAL);

    /* ── T1: ENTROPY path (8% contiguous change) ── */
    printf("== T1: ENTROPY delta roundtrip ==\n");
    fill_pattern(kbuf[0], LAYER_SZ, 111);
    fill_pattern(vbuf[0], LAYER_SZ, 222);
    fill_pattern(kbuf[1], LAYER_SZ, 333);
    fill_pattern(vbuf[1], LAYER_SZ, 444);

    KVRemapCtx ctx;
    kv_remap_init(&ctx, 4096);
    layers_register(&ctx);
    kv_remap_set_skeleton(&ctx);
    size_t hot = TOTAL * 8 / 100;
    perturb(kbuf[0], hot, 777);          /* contiguous 8% of total */
    int pct = kv_remap_classify(&ctx);
    check(pct > 0 && pct <= 15, "classify reports <=15%%");
    check(kv_remap_store_delta(&ctx, pct) == 0 && ctx.delta.type == DELTA_ENTROPY,
        "delta type = ENTROPY");
    kv_geofs_snapshot(&vol, &ctx, "agentA");
    golden_take(golden);
    layers_wipe();

    KVRemapCtx ctx2;
    kv_remap_init(&ctx2, 4096);
    layers_register(&ctx2);
    check(kv_geofs_resume(&vol, &ctx2, "agentA") == 0, "resume ok");
    check(golden_cmp(golden) == 0, "live state reproduced byte-for-byte");
    kv_remap_destroy(&ctx);
    kv_remap_destroy(&ctx2);

    /* ── T2: GEO path (50% contiguous change) ── */
    if (stop_at < 2) { printf("stopped\n"); goto done; }
    printf("== T2: GEO delta roundtrip ==\n");
    fill_pattern(kbuf[0], LAYER_SZ, 555);
    KVRemapCtx c3;
    kv_remap_init(&c3, 4096);
    layers_register(&c3);
    kv_remap_set_skeleton(&c3);
    /* one full layer = 25% of total → lands in the GEO band */
    perturb(kbuf[0], LAYER_SZ, 888);
    pct = kv_remap_classify(&c3);
    check(pct > 15 && pct < 85, "classify in GEO band");
    check(kv_remap_store_delta(&c3, pct) == 0 && c3.delta.type == DELTA_GEO,
        "delta type = GEO");
    check(c3.delta.n_ranges >= 1, "geo ranges captured");
    kv_geofs_snapshot(&vol, &c3, "agentB");
    golden_take(golden);
    layers_wipe();

    KVRemapCtx c4;
    kv_remap_init(&c4, 4096);
    layers_register(&c4);
    check(kv_geofs_resume(&vol, &c4, "agentB") == 0, "resume ok");
    check(golden_cmp(golden) == 0, "GEO state reproduced byte-for-byte");
    kv_remap_destroy(&c3);
    kv_remap_destroy(&c4);

    /* ── T3: multi-turn — two snapshots, each resumes its own state ── */
    if (stop_at < 3) { printf("stopped\n"); goto done; }
    printf("== T3: multi-turn snapshots ==\n");
    fill_pattern(kbuf[0], LAYER_SZ, 1000);
    fill_pattern(vbuf[0], LAYER_SZ, 2000);
    fill_pattern(kbuf[1], LAYER_SZ, 3000);
    fill_pattern(vbuf[1], LAYER_SZ, 4000);

    KVRemapCtx m;
    kv_remap_init(&m, 4096);
    layers_register(&m);
    kv_remap_set_skeleton(&m);

    /* turn 1: small change */
    perturb(kbuf[0], TOTAL * 5 / 100, 5001);
    pct = kv_remap_classify(&m);
    kv_remap_store_delta(&m, pct);
    kv_geofs_snapshot(&vol, &m, "turn1");
    uint8_t *gold1 = (uint8_t *)xalloc(TOTAL);
    golden_take(gold1);

    /* turn 2: more change on top */
    perturb(vbuf[1], TOTAL * 6 / 100, 6002);
    pct = kv_remap_classify(&m);
    kv_remap_store_delta(&m, pct);
    kv_geofs_snapshot(&vol, &m, "turn2");
    uint8_t *gold2 = (uint8_t *)xalloc(TOTAL);
    golden_take(gold2);

    /* resume turn1 */
    layers_wipe();
    KVRemapCtx r1;
    kv_remap_init(&r1, 4096);
    layers_register(&r1);
    check(kv_geofs_resume(&vol, &r1, "turn1") == 0, "resume turn1 ok");
    memcpy(golden, gold1, TOTAL);
    check(golden_cmp(golden) == 0, "turn1 state exact");
    kv_remap_destroy(&r1);

    /* resume turn2 */
    layers_wipe();
    KVRemapCtx r2;
    kv_remap_init(&r2, 4096);
    layers_register(&r2);
    check(kv_geofs_resume(&vol, &r2, "turn2") == 0, "resume turn2 ok");
    memcpy(golden, gold2, TOTAL);
    check(golden_cmp(golden) == 0, "turn2 state exact");
    kv_remap_destroy(&m);
    kv_remap_destroy(&r2);
    free(gold1);
    free(gold2);

    /* ── T4: drop + overwrite hygiene ── */
    if (stop_at < 4) { printf("stopped\n"); goto done; }
    printf("== T4: drop + overwrite hygiene ==\n");
    uint32_t used_before = vol.total_blocks_used;
    check(kv_geofs_drop(&vol, "turn1") == 0, "drop removes files");
    check(geos_find(&vol, "turn1.skel") == NULL &&
          geos_find(&vol, "turn1.delta") == NULL, "inodes gone");
    check(vol.total_blocks_used < used_before, "blocks freed");

    uint32_t used_a = 0;
    uint8_t *pristine = (uint8_t *)xalloc(LAYER_SZ);
    memcpy(pristine, kbuf[0], LAYER_SZ);
    KVRemapCtx w;
    kv_remap_init(&w, 4096);
    layers_register(&w);
    kv_remap_set_skeleton(&w);
    /* identical live state each pass → identical delta size →
       delete+create must reuse the same block count (no leak) */
    for (int i = 0; i < 3; i++) {
        memcpy(kbuf[0], pristine, LAYER_SZ);
        perturb(kbuf[0], TOTAL * 10 / 100, 9000);
        kv_remap_store_delta(&w, kv_remap_classify(&w));
        check(kv_geofs_snapshot(&vol, &w, "hot") == 0, "overwrite snapshot");
        if (i == 0) used_a = vol.total_blocks_used;
    }
    check(vol.total_blocks_used == used_a, "no block leak across overwrites");
    check(kv_geofs_drop(&vol, "hot") == 0 &&
          vol.total_blocks_used < used_a, "drop returns blocks");
    free(pristine);
    kv_remap_destroy(&w);

    /* ── T5: NONE delta roundtrip ── */
    if (stop_at < 5) { printf("stopped\n"); goto done; }
    printf("== T5: NONE delta roundtrip ==\n");
    KVRemapCtx n;
    kv_remap_init(&n, 4096);
    layers_register(&n);
    kv_remap_set_skeleton(&n);
    check(kv_remap_classify(&n) == 0, "no change detected");
    check(kv_geofs_snapshot(&vol, &n, "frozen") == 0, "snapshot ok");
    golden_take(golden);
    layers_wipe();
    KVRemapCtx n2;
    kv_remap_init(&n2, 4096);
    layers_register(&n2);
    check(kv_geofs_resume(&vol, &n2, "frozen") == 0, "resume ok");
    check(golden_cmp(golden) == 0, "skeleton-only state exact");
    kv_remap_destroy(&n);
    kv_remap_destroy(&n2);

    done:
    free(golden);
    layers_free();
    geos_volume_free(&vol);

    printf("═══════════════════════════════════════\n");
    printf("%s — %s\n", "test_kv_geofs_bridge",
           g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
