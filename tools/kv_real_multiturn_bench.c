/*
 * tools/kv_real_multiturn_bench.c — KV Remap park/resume on REAL llama.cpp KV
 * ════════════════════════════════════════════════════════════════════════════
 * Input: build/kvslots/turn{1..4}.bin — REAL Qwen2.5-0.5B KV snapshots dumped
 * by kv_dump_turns (nested: turn_t = prefix(turn_{t-1}) + new tokens).
 *
 * Fixed-size working buffer (= turn4 size). Snapshots are zero-padded to it,
 * so one skeleton (turn1) serves every later turn — an agent parked after its
 * first exchange and chatting on:
 *
 *   park    : classify + store_delta + twin-store write     (write path)
 *   resume  : twin-store read + decompress + apply          (read path)
 *   floor   : full-buffer memcpy rewrite (re-prefill lower bound)
 *   lossless: memcmp of the FULL buffer vs padded snapshot  (oracle = source)
 *
 * BUILD: make kv-real-multiturn
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "kv_dramtile_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

#define N_TURNS 4

static uint8_t *snap[N_TURNS + 1];      /* raw snapshots */
static size_t   snap_sz[N_TURNS + 1];
static uint8_t *live;                   /* fixed working KV buffer */
static uint8_t *golden;

static int read_snap(const char *dir, int t) {
    char path[512];
    snprintf(path, sizeof(path), "%s\\turn%d.bin", dir, t);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    snap[t] = (uint8_t *)malloc((size_t)n);
    snap_sz[t] = fread(snap[t], 1, (size_t)n, f);
    fclose(f);
    return (size_t)n == snap_sz[t] ? 0 : -1;
}

/* register the fixed buffer as one remap entry: [0,half)="K", [half,SZ)="V" */
static void register_live(KVRemapCtx *ctx, size_t total) {
    size_t half = total / 2;
    size_t k_nb1[1], v_nb1[1], k_sz[1], v_sz[1];
    void *kd[1], *vd[1];
    int ne[1], lid[1];
    kd[0] = live; vd[0] = live + half;
    k_nb1[0] = v_nb1[0] = half / 16;
    k_sz[0] = v_sz[0] = half;
    ne[0] = 16; lid[0] = 0;
    kv_remap_register(ctx, kd, vd, k_nb1, v_nb1, k_sz, v_sz, ne, lid, 1);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build\\kvslots";

    for (int t = 1; t <= N_TURNS; t++)
        if (read_snap(dir, t) != 0) return 1;

    const size_t SZ = snap_sz[N_TURNS];     /* largest snapshot = buffer size */

    live   = (uint8_t *)calloc(1, SZ);
    golden = (uint8_t *)calloc(1, SZ);

    DRamTileStore store;
    if (dt_store_init_twin(&store, "build\\kv_multichat.twin", 64ull * 1024 * 1024) != 0) {
        fprintf(stderr, "twin store init failed\n"); return 1;
    }

    printf("=== KV Remap on REAL llama.cpp KV (Qwen2.5-0.5B, 24 layers, GQA) ===\n");
    printf("snapshots: ");
    for (int t = 1; t <= N_TURNS; t++) printf("turn%d=%.2fMB ", t, snap_sz[t] / 1048576.0);
    printf("\nbuffer=%zu bytes · skeleton=turn1 (zero-padded) · parked via DRamTile twin\n\n", SZ);
    printf("%5s %12s %12s %9s %9s %10s %10s %8s\n",
           "turn", "full_KV_B", "delta_B", "park_ms", "resume_ms",
           "delta/KV%", "skel/KV%", "lossless");

    /* ── skeleton = turn1, registered once at fixed size ── */
    KVRemapCtx ctx;
    kv_remap_init(&ctx, 4096);
    register_live(&ctx, SZ);                 /* live currently all-zero */
    memcpy(live, snap[1], snap_sz[1]);
    kv_remap_set_skeleton(&ctx);
    size_t skel_comp = ctx.skeleton_comp;

    /* park the initial state */
    int pct = kv_remap_classify(&ctx);
    kv_remap_store_delta(&ctx, pct);
    if (kv_dt_park(&store, &ctx, "agentA") != 0) { fprintf(stderr, "park fail\n"); return 1; }

    printf("%5d %12zu %12zu %9s %9s %9.1f%% %9.1f%% %8s\n",
           1, snap_sz[1], ctx.delta.delta_size, "-", "-",
           (double)ctx.delta.delta_size * 100.0 / snap_sz[1],
           (double)skel_comp * 100.0 / snap_sz[1], "-");

    int all_ok = 1;
    for (int t = 2; t <= N_TURNS; t++) {
        char name[64];
        snprintf(name, sizeof(name), "agentA_t%d", t);

        /* live := real snapshot t (prefix grows, old cells unchanged) */
        memset(live, 0, SZ);
        memcpy(live, snap[t], snap_sz[t]);

        double tp = now_ms();
        pct = kv_remap_classify(&ctx);
        kv_remap_store_delta(&ctx, pct);
        if (kv_dt_park(&store, &ctx, name) != 0) { fprintf(stderr, "park fail t%d\n", t); return 1; }
        double park_ms = now_ms() - tp;

        size_t dsize = ctx.delta.delta_size;

        /* oracle: golden copy BEFORE wiping */
        memcpy(golden, snap[t], snap_sz[t]);
        memset(live, 0, SZ);

        /* resume from twin store */
        KVRemapCtx cr;
        kv_remap_init(&cr, 4096);
        register_live(&cr, SZ);
        tp = now_ms();
        int rc = kv_dt_resume(&store, &cr, name);
        double resume_ms = now_ms() - tp;
        int ok = rc == 0 && memcmp(live, golden, SZ) == 0;   /* FULL buffer compare */
        if (!ok) all_ok = 0;

        /* floor: full-buffer rewrite */
        tp = now_ms();
        memcpy(live, golden, SZ);
        double floor_ms = now_ms() - tp;

        printf("%5d %12zu %12zu %9.2f %9.2f %9.1f%% %9.1f%% %8s\n",
               t, snap_sz[t], dsize, park_ms, resume_ms,
               (double)dsize * 100.0 / snap_sz[t],
               (double)skel_comp * 100.0 / snap_sz[t],
               ok ? "OK" : "FAIL");
        printf("      resume/floor = %.1fx  (floor %.3f ms = full %.2f MB rewrite)\n",
               resume_ms / (floor_ms + 1e-9), floor_ms, SZ / 1048576.0);

        kv_remap_destroy(&cr);
    }

    printf("\nlossless: %s\n", all_ok ? "ALL OK (memcmp full buffer vs real snapshots)" : "FAILURE");
    printf("twin store total parked bytes: %zu\n", dt_store_total_bytes(&store));

    kv_remap_destroy(&ctx);
    free(live); free(golden);
    for (int t = 1; t <= N_TURNS; t++) free(snap[t]);
    dt_store_destroy_twin(&store);
    return all_ok ? 0 : 1;
}
