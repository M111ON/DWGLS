/*
 * test_kv_rail_geofs.c — Rail checkpoint ↔ GeoFS wire test
 * ══════════════════════════════════════════════════════════
 *
 * Proves the fresh-process restore contract for the background rail:
 *   scan mid-flight → freeze → save state to GeoFS → destroy →
 *   load → resume → same decision as an uninterrupted reference scan.
 *
 * Independent oracle: the uninterrupted run on identical bytes —
 * deterministic memcmp walk must produce the identical change_pct.
 *
 * T1  interrupted scan resumes to reference pct (ENTROPY band)
 * T2  interrupted scan resumes to reference pct (GEO band)
 * T3  rail save/load is field-exact (serialized bytes equal)
 * T4  auto-snapshot: rail cycle detects change → snapshot → wipe →
 *     bridge resume reproduces live state byte-for-byte
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "kv_geofs_bridge.h"

#define NLAYERS   6
#define LAYER_SZ  (48u * 1024u)
#define TOTAL     (NLAYERS * 2 * LAYER_SZ)

static GeosVolume vol;
static uint8_t *kbuf[NLAYERS], *vbuf[NLAYERS];
static int g_fail = 0;

static void *xalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "OOM\n"); exit(1); }
    return p;
}

static void fill_pattern(uint8_t *buf, size_t n, uint32_t seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] = (uint8_t)(s & 0xFF);
    }
}

/* perturb a contiguous region inside one layer (bounded by layer size) */
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
        if (memcmp(g + off, kbuf[l], LAYER_SZ) != 0) return -1;
        off += LAYER_SZ;
        if (memcmp(g + off, vbuf[l], LAYER_SZ) != 0) return -2;
        off += LAYER_SZ;
    }
    return 0;
}

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

/* run scan to completion on current buffers, return change_pct */
static int scan_to_pct(KVRemapCtx *ctx) {
    KVRemapRail rail;
    kv_remap_rail_init(&rail, ctx);
    kv_remap_rail_start_scan(&rail);
    int guard = 100000;
    while (kv_remap_rail_step(&rail) == 0 && guard-- > 0) {}
    int pct = rail.change_pct;
    kv_remap_rail_destroy(&rail);
    return pct;
}

/* interrupted variant: step `cut_after` steps → freeze → save →
   fresh rail → load → resume → finish. Returns final pct. */
static int scan_interrupted_persist(KVRemapCtx *ctx, int cut_after,
                                    const char *agent) {
    KVRemapRail rail;
    kv_remap_rail_init(&rail, ctx);
    kv_remap_rail_start_scan(&rail);
    for (int i = 0; i < cut_after; i++)
        if (kv_remap_rail_step(&rail) == 1) break;

    kv_remap_rail_freeze(&rail);
    if (kv_geofs_rail_save(&vol, agent, &rail) != 0) return -100;

    /* simulate fresh process: brand-new rail bound to same ctx */
    KVRemapRail rail2;
    kv_remap_rail_init(&rail2, ctx);
    if (kv_geofs_rail_load(&vol, agent, &rail2) != 0) return -101;
    kv_remap_rail_resume(&rail2);

    int guard = 100000;
    while (kv_remap_rail_step(&rail2) == 0 && guard-- > 0) {}
    int pct = rail2.change_pct;
    kv_remap_rail_destroy(&rail);
    kv_remap_rail_destroy(&rail2);
    return pct;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    geos_volume_init(&vol);
    layers_alloc();
    uint8_t *golden = (uint8_t *)xalloc(TOTAL);

    for (int l = 0; l < NLAYERS; l++) {
        fill_pattern(kbuf[l], LAYER_SZ, (uint32_t)(100 + l));
        fill_pattern(vbuf[l], LAYER_SZ, (uint32_t)(200 + l));
    }

    KVRemapCtx ctx;
    kv_remap_init(&ctx, 4096);
    layers_register(&ctx);
    kv_remap_set_skeleton(&ctx);

    /* ── T1: interrupted scan (ENTROPY band) ── */
    printf("== T1: rail persist mid-scan (small change) ==\n");
    perturb(kbuf[2], TOTAL * 5 / 100, 7001);      /* ~5% of total */
    int ref = scan_to_pct(&ctx);
    check(ref > 0 && ref <= 15, "reference pct in ENTROPY band");
    int got = scan_interrupted_persist(&ctx, 3, "r1");
    check(got == ref, "resumed scan pct == reference pct");

    /* ── T2: interrupted scan (GEO band) ── */
    printf("== T2: rail persist mid-scan (GEO band) ==\n");
    perturb(kbuf[4], LAYER_SZ, 8002);             /* one full layer = 1/12 ≈ 8%… */
    perturb(vbuf[4], LAYER_SZ, 8003);             /* …two layers ≈ 17% → GEO */
    ref = scan_to_pct(&ctx);
    check(ref > 15 && ref < 85, "reference pct in GEO band");
    got = scan_interrupted_persist(&ctx, 7, "r2");
    check(got == ref, "resumed scan pct == reference pct");

    /* ── T3: save/load field-exact ── */
    printf("== T3: rail state field-exact roundtrip ==\n");
    KVRemapRail ra, rb;
    kv_remap_rail_init(&ra, &ctx);
    kv_remap_rail_start_scan(&ra);
    for (int i = 0; i < 5; i++) kv_remap_rail_step(&ra);
    kv_remap_rail_freeze(&ra);
    check(kv_geofs_rail_save(&vol, "r3", &ra) == 0, "save ok");
    kv_remap_rail_init(&rb, &ctx);
    check(kv_geofs_rail_load(&vol, "r3", &rb) == 0, "load ok");
    check(ra.state == rb.state && ra.lane == rb.lane &&
          ra.freeze_state == rb.freeze_state &&
          ra.total_diff == rb.total_diff &&
          ra.total_checked == rb.total_checked &&
          ra.change_pct == rb.change_pct,
          "rail scalars equal");
    int lanes_eq = 1;
    for (int i = 0; i < 3; i++) {
        const RailLane *a = &ra.lanes[i], *b = &rb.lanes[i];
        if (a->layer_start != b->layer_start || a->layer_end != b->layer_end ||
            a->cur_layer != b->cur_layer || a->cur_phase != b->cur_phase ||
            a->cur_off != b->cur_off || a->diff_count != b->diff_count ||
            a->checked != b->checked || a->total != b->total ||
            a->complete != b->complete)
            lanes_eq = 0;
    }
    check(lanes_eq, "all lane cursors equal");
    kv_remap_rail_destroy(&ra);
    kv_remap_rail_destroy(&rb);

    /* ── T4: auto-snapshot via rail cycle → lossless resume ── */
    printf("== T4: rail cycle auto-snapshot ==\n");
    perturb(kbuf[0], LAYER_SZ, 9004);
    perturb(vbuf[0], LAYER_SZ, 9005);             /* ~17% → GEO band */

    KVRemapRail rc_;
    kv_remap_rail_init(&rc_, &ctx);
    kv_remap_rail_start_scan(&rc_);
    int guard = 100000, saw_snapshot = 0;
    while (guard-- > 0) {
        int r = kv_geofs_rail_cycle(&vol, "agentR", &rc_);
        if (r == 1 && rc_.state == RAIL_PATCH) {
            /* delta encodes live state AT decision time — capture now,
               before the patch phase writes skeleton over live KV */
            if (!saw_snapshot) golden_take(golden);
            saw_snapshot = 1;
        }
        if (r == 1 && rc_.state == RAIL_PARK) break;   /* patch done */
        if (r < 0) break;
    }
    check(saw_snapshot, "auto-snapshot fired on PATCH decision");
    kv_remap_rail_destroy(&rc_);

    layers_wipe();

    KVRemapCtx ctx2;
    kv_remap_init(&ctx2, 4096);
    layers_register(&ctx2);
    check(kv_geofs_resume(&vol, &ctx2, "agentR") == 0, "resume ok");
    check(golden_cmp(golden) == 0, "live state reproduced byte-for-byte");
    kv_remap_destroy(&ctx2);

    kv_remap_destroy(&ctx);
    free(golden);
    layers_free();
    geos_volume_free(&vol);

    printf("═══════════════════════════════════════\n");
    printf("%s — %s\n", "test_kv_rail_geofs",
           g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
