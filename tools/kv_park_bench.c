/*
 * kv_park_bench.c — Park/Resume vs Re-prefill benchmark
 * ═══════════════════════════════════════════════════════
 *
 * North-star question: does parking an agent (skeleton+delta through
 * GeoFS) beat re-prefilling its KV from scratch?
 *
 * Measured per change-level (2%..60% of KV bytes touched per turn):
 *   park   : classify + store_delta + geofs snapshot  (write path)
 *   resume : geofs read + decompress + apply          (read path)
 *   re-prefill lower bound: full KV rewrite (memcpy) — the real thing
 *            also pays a forward pass over every token, so this is a
 *            FLOOR for prefill cost, not the cost itself.
 *   delta size vs full size = storage cost ratio.
 *
 * Lossless is asserted on every resume (memcmp vs golden).
 *
 * Build: make kv-park-bench → ./build/kv_park_bench
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "kv_geofs_bridge.h"

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

#define NLAYERS    4
#define LAYER_SZ   (64u * 1024u)           /* total = 4*2*64KB = 512KB
                                              (skeleton + 60% delta must fit
                                               one GeoFS volume, 1.3MB store) */
#define TOTAL      (NLAYERS * 2 * LAYER_SZ)

static GeosVolume vol;
static uint8_t *kbuf[NLAYERS], *vbuf[NLAYERS];
static uint8_t *golden;

static void fill_pattern(uint8_t *buf, size_t n, uint32_t seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] = (uint8_t)(s & 0xFF);
    }
}

static void perturb(uint8_t *buf, size_t nbytes, uint32_t seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < nbytes; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] ^= (uint8_t)(s | 1);
    }
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

static int verify_golden(void) {
    size_t off = 0;
    for (int l = 0; l < NLAYERS; l++) {
        if (memcmp(golden + off, kbuf[l], LAYER_SZ) != 0) return -1;
        off += LAYER_SZ;
        if (memcmp(golden + off, vbuf[l], LAYER_SZ) != 0) return -1;
        off += LAYER_SZ;
    }
    return 0;
}

static void golden_take(void) {
    size_t off = 0;
    for (int l = 0; l < NLAYERS; l++) {
        memcpy(golden + off, kbuf[l], LAYER_SZ); off += LAYER_SZ;
        memcpy(golden + off, vbuf[l], LAYER_SZ); off += LAYER_SZ;
    }
}

/* perturb `nbytes` spread layer-major across K then V buffers */
static void perturb_spread(size_t nbytes, uint32_t seed) {
    uint32_t s = seed;
    size_t left = nbytes;
    for (int l = 0; l < NLAYERS && left > 0; l++) {
        uint8_t *bufs[2] = { kbuf[l], vbuf[l] };
        for (int b = 0; b < 2 && left > 0; b++) {
            size_t n = left < LAYER_SZ ? left : LAYER_SZ;
            uint8_t *p = bufs[b];
            for (size_t i = 0; i < n; i++) {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                p[i] ^= (uint8_t)(s | 1);
            }
            left -= n;
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    geos_volume_init(&vol);
    golden = (uint8_t *)malloc(TOTAL);
    for (int l = 0; l < NLAYERS; l++) {
        kbuf[l] = (uint8_t *)malloc(LAYER_SZ);
        vbuf[l] = (uint8_t *)malloc(LAYER_SZ);
        fill_pattern(kbuf[l], LAYER_SZ, (uint32_t)(10 + l));
        fill_pattern(vbuf[l], LAYER_SZ, (uint32_t)(20 + l));
    }

    printf("KV Remap park/resume vs re-prefill\n");
    printf("KV: %d layers x (K+V) x %dKB = %.2f MB total\n\n",
           NLAYERS, LAYER_SZ / 1024, TOTAL / 1048576.0);
    printf("%8s %12s %9s %9s %10s %10s %8s\n",
           "change%", "delta_size", "park_ms", "resume_ms",
           "delta/KV", "skel/KV", "lossless");

    const int pcts[] = {2, 8, 20, 40, 60};
    int all_ok = 1;
    size_t skel_comp_sz = 0;

    for (unsigned pi = 0; pi < sizeof(pcts) / sizeof(pcts[0]); pi++) {
        int pct_target = pcts[pi];

        /* fresh skeleton each round */
        KVRemapCtx ctx;
        kv_remap_init(&ctx, 4096);
        layers_register(&ctx);

        double t0 = now_ms();
        kv_remap_set_skeleton(&ctx);
        /* skeleton-set time excluded from park: it's amortized across turns */

        /* perturb contiguous pct_target% of total, spread across layers */
        size_t hot = TOTAL * (size_t)pct_target / 100;
        perturb_spread(hot, (uint32_t)(1000 + pct_target));

        t0 = now_ms();
        int pct = kv_remap_classify(&ctx);
        kv_remap_store_delta(&ctx, pct);
        if (kv_geofs_snapshot(&vol, &ctx, "bench") != 0) {
            fprintf(stderr, "snapshot failed (volume full?) at %d%%\n",
                pct_target);
            return 1;
        }
        double park_ms = now_ms() - t0;

        golden_take();

        /* wipe everything — parked agent leaves RAM */
        for (int l = 0; l < NLAYERS; l++) {
            memset(kbuf[l], 0, LAYER_SZ);
            memset(vbuf[l], 0, LAYER_SZ);
        }

        /* ── resume path ── */
        KVRemapCtx ctx2;
        kv_remap_init(&ctx2, 4096);
        layers_register(&ctx2);
        t0 = now_ms();
        kv_geofs_resume(&vol, &ctx2, "bench");
        double resume_ms = now_ms() - t0;
        int ok = verify_golden() == 0;
        if (!ok) all_ok = 0;

        /* ── re-prefill floor: rewrite full KV (no compute included) ── */
        t0 = now_ms();
        for (int l = 0; l < NLAYERS; l++) {
            memcpy(kbuf[l], golden + (size_t)l * 2 * LAYER_SZ, LAYER_SZ);
            memcpy(vbuf[l], golden + ((size_t)l * 2 + 1) * LAYER_SZ, LAYER_SZ);
        }
        double prefill_ms = now_ms() - t0;

        size_t dsize = ctx.delta.delta_size;
        skel_comp_sz = ctx.skeleton_comp;
        printf("%7d%% %10zu B %9.3f %9.3f %8.1f%% %8.1f%% %8s\n",
               pct, dsize, park_ms, resume_ms,
               (double)dsize * 100.0 / TOTAL,
               (double)skel_comp_sz * 100.0 / TOTAL,
               ok ? "OK" : "FAIL");

        kv_remap_destroy(&ctx);
        kv_remap_destroy(&ctx2);
    }

    printf("\nnotes:\n");
    printf("  delta/KV = marginal bytes an agent #2..N reads on resume\n");
    printf("  (skeleton is shared — paid once by agent #1).\n");
    printf("  skel/KV = 100%% here because synthetic KV is random data\n");
    printf("  (worst case); real KV caches have structure and compress.\n");
    printf("  prefill floor = pure memcpy rewrite; real re-prefill adds a\n");
    printf("  forward pass over every token (compute-bound, 10-100x more).\n");
    printf("  lossless: %s\n", all_ok ? "ALL OK" : "FAILURE");

    free(golden);
    for (int l = 0; l < NLAYERS; l++) { free(kbuf[l]); free(vbuf[l]); }
    geos_volume_free(&vol);
    return all_ok ? 0 : 1;
}
