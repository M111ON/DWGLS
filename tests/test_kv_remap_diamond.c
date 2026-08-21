/*
 * test_kv_remap_diamond.c — KV Remap Diamond Shell Integration Test
 * ═══════════════════════════════════════════════════════════════════
 *
 * Test the Diamond Shell compress/decompress integration for KV Remap.
 * Verifies lossless roundtrip and compares with RLE.
 *
 * Build: gcc -O2 -std=c11 -I../collection/geopixel -I../collection/Hfolder
 *        -I../collection/geo_jump_module/include -I../collection/dgls/diamond/include
 *        -o test_kv_remap_diamond.exe test_kv_remap_diamond.c -L. -lzstd -lm
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "kv_remap.h"
#include "kv_remap_diamond.h"

/* ── Timer ───────────────────────────────────────────── */
typedef struct { long tv_sec; long tv_nsec; } T;
static inline void timer_now(T *t) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    t->tv_sec  = (long)(c.QuadPart / f.QuadPart);
    t->tv_nsec = (long)(c.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart);
#else
    clock_gettime(CLOCK_MONOTONIC, (struct timespec *)t);
#endif
}
static inline double timer_ms(T *a, T *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000.0;
}

/* ── KV parameters ───────────────────────────────────── */
#define N_LAYERS  6
#define N_EMBD    512
#define N_CTX     1024
#define KV_TOTAL  (N_LAYERS * 2 * N_EMBD * N_CTX * (int)sizeof(uint16_t))

/* ── Mock data ───────────────────────────────────────── */
static void kv_gen(uint16_t *buf, int fill, uint32_t seed) {
    for (int l = 0; l < N_LAYERS * 2; l++)
        for (int p = 0; p < N_CTX; p++)
            for (int d = 0; d < N_EMBD; d++) {
                int active = (p * 100 / N_CTX) < fill;
                if (!active) { buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = 0; continue; }
                uint32_t h = (uint32_t)(l*1000000 + p*7919 + d*104729 + seed);
                buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = (uint16_t)((h^(h>>16))%2048);
            }
}
static void apply_chg(uint16_t *buf, int pct, uint32_t s) {
    for (int l = 0; l < N_LAYERS * 2; l++)
        for (int p = 0; p < N_CTX; p++)
            if ((p * 100 / N_CTX) < pct)
                for (int d = 0; d < N_EMBD; d++) {
                    uint32_t h = (uint32_t)(l*3000000 + p*1337 + d*99991 + s);
                    buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = (uint16_t)((h^(h>>16))%2048);
                }
}

/* ── Pretty print ────────────────────────────────────── */
static void sep(const char *title) {
    fprintf(stderr, "\n═══════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "  %s\n", title);
    fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");
}

int main(void) {
    sep("KV Remap Diamond Shell Integration Test");
    fprintf(stderr, "  KV: %dL × %dE × %dC × 2B = %.1f MB\n",
        N_LAYERS, N_EMBD, N_CTX, (double)KV_TOTAL / 1048576.0);

    size_t kv_u16 = (size_t)N_LAYERS * 2 * N_EMBD * N_CTX;
    uint16_t *skeleton = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    uint16_t *current  = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    if (!skeleton || !current) { fprintf(stderr, "OOM\n"); return 1; }

    kv_gen(skeleton, 100, 42);

    struct {
        const char *label; int pct; uint32_t seed; const char *meaning;
    } tests[] = {
        {"0%",   0,   0,    "no change"},
        {"15%", 15,  777,   "same topic, new sentence"},
        {"40%", 40,  123,   "related topic shift"},
        {"60%", 60,  456,   "new topic emerging"},
        {"85%", 85,  999,   "topic changed"},
    };
    int n_tests = 5;
    int pass = 0, fail = 0;

    for (int t = 0; t < n_tests; t++) {
        sep(tests[t].label);

        memcpy(current, skeleton, kv_u16 * sizeof(uint16_t));
        if (tests[t].pct > 0) apply_chg(current, tests[t].pct, tests[t].seed);

        uint8_t *diff = (uint8_t *)malloc(KV_TOTAL);
        const uint8_t *sk = (const uint8_t *)skeleton;
        const uint8_t *cu = (const uint8_t *)current;
        for (uint64_t i = 0; i < KV_TOTAL; i++) diff[i] = sk[i] ^ cu[i];

        uint64_t nz = 0;
        for (uint64_t i = 0; i < KV_TOTAL; i++) if (diff[i] != 0) nz++;
        fprintf(stderr, "  XOR diff: %.1f%% changed\n", (double)nz / KV_TOTAL * 100.0);

        /* Test RLE */
        void *rle_comp = NULL;
        size_t rle_comp_sz = 0;
        T t0, t1;
        timer_now(&t0);
        int rle_raw = kv_remap_compress(diff, KV_TOTAL, &rle_comp, &rle_comp_sz);
        timer_now(&t1);
        double rle_enc_ms = timer_ms(&t0, &t1);

        size_t rle_dec_sz = 0;
        timer_now(&t0);
        void *rle_dec = kv_remap_decompress(rle_comp, rle_comp_sz, &rle_dec_sz);
        timer_now(&t1);
        double rle_dec_ms = timer_ms(&t0, &t1);

        int rle_ok = (rle_dec && rle_dec_sz == KV_TOTAL && memcmp(rle_dec, diff, KV_TOTAL) == 0);

        /* Test Diamond Shell */
        void *dia_comp = NULL;
        size_t dia_comp_sz = 0;
        timer_now(&t0);
        int dia_raw = kv_remap_compress_diamond(diff, KV_TOTAL, &dia_comp, &dia_comp_sz);
        timer_now(&t1);
        double dia_enc_ms = timer_ms(&t0, &t1);

        size_t dia_dec_sz = 0;
        timer_now(&t0);
        void *dia_dec = kv_remap_decompress_diamond(dia_comp, dia_comp_sz, &dia_dec_sz);
        timer_now(&t1);
        double dia_dec_ms = timer_ms(&t0, &t1);

        int dia_ok = (dia_dec && dia_dec_sz == KV_TOTAL && memcmp(dia_dec, diff, KV_TOTAL) == 0);

        /* Diamond Shell stats */
        DiamondStats dstats = kv_remap_diamond_stats(dia_comp, dia_comp_sz);

        /* Print results */
        fprintf(stderr, "\n  [RLE]:\n");
        fprintf(stderr, "    ratio: %.2fx  enc: %.3f ms  dec: %.3f ms  %s\n",
            (double)KV_TOTAL / (double)(rle_comp_sz - sizeof(RLEHeader)),
            rle_enc_ms, rle_dec_ms, rle_raw ? "RAW" : "COMP");
        fprintf(stderr, "    verify: %s\n", rle_ok ? "PASS" : "FAIL");

        fprintf(stderr, "\n  [Diamond Shell]:\n");
        fprintf(stderr, "    ratio: %.2fx  enc: %.3f ms  dec: %.3f ms  %s\n",
            (double)KV_TOTAL / (double)(dia_comp_sz - sizeof(DiamondRemapHeader)),
            dia_enc_ms, dia_dec_ms, dia_raw ? "RAW" : "COMP");
        fprintf(stderr, "    FLAT: %llu  SPARSE: %llu  DENSE: %llu\n",
            (unsigned long long)dstats.flat,
            (unsigned long long)dstats.sparse,
            (unsigned long long)dstats.dense);
        fprintf(stderr, "    verify: %s\n", dia_ok ? "PASS" : "FAIL");

        /* Comparison */
        double savings = (1.0 - (double)dia_comp_sz / (double)rle_comp_sz) * 100.0;
        fprintf(stderr, "\n  Diamond vs RLE: %.1f%% %s\n",
            savings > 0 ? savings : -savings,
            savings > 0 ? "smaller" : "larger");
        fprintf(stderr, "  Speed: enc %.1fx  dec %.1fx\n",
            rle_enc_ms / dia_enc_ms, rle_dec_ms / dia_dec_ms);

        if (rle_ok && dia_ok) { pass++; fprintf(stderr, "\n  ✅ PASS\n"); }
        else { fail++; fprintf(stderr, "\n  ❌ FAIL\n"); }

        free(diff);
        free(rle_comp);
        free(rle_dec);
        free(dia_comp);
        free(dia_dec);
    }

    /* Summary */
    sep("Summary");
    fprintf(stderr, "  Tests: %d pass, %d fail\n", pass, fail);

    free(skeleton);
    free(current);
    fprintf(stderr, "\nDone.\n");
    return fail > 0 ? 1 : 0;
}
