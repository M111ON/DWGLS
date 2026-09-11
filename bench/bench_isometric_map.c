/*
 * bench_isometric_map.c — Performance benchmark for isometric_map.h
 * ═══════════════════════════════════════════════════════════════════
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/bench_isometric_map.exe bench/bench_isometric_map.c -lm
 * RUN:   ./build/bench_isometric_map.exe [iters]   (default 10M)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "isometric_map.h"

#define DEF_ITERS 10000000u

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void sink_u8(uint8_t v)   { __asm__ volatile("" :: "r"(v)); }
static void sink_u16(uint16_t v) { __asm__ volatile("" :: "r"(v)); }
static void sink_u32(uint32_t v) { __asm__ volatile("" :: "r"(v)); }
static void sink_skel(im_skel_t s) { sink_u8(s.zone); sink_u16(s.enc); }

static void report(const char *label, double ns_per) {
    printf("  %-30s %8.1f ns/op  (%7.1f M ops/s)\n",
           label, ns_per, 1e3 / ns_per);
}

int main(int argc, char **argv)
{
    uint32_t iters = argc > 1 ? (uint32_t)atoi(argv[1]) : DEF_ITERS;
    printf("=== isometric_map.h benchmark (%u iters) ===\n\n", iters);

    /* prepare test data */
    uint8_t zeros[64] = {0};
    uint8_t dense[64] = {0};
    memset(dense, 0xFF, 8);
    uint8_t prev_chunk[64];
    memset(prev_chunk, 0xAA, 64);

    im_cache_t cache;
    im_cache_init(&cache);
    im_cache_l1_insert(&cache, 100, 0x1111);
    im_cache_l2_insert(&cache, 200, 0x2222);

    im_perm_t perm;
    im_perm_init(&perm, 0xDEADBEEF);

    im_rewind_t rw;
    im_rewind_init(&rw);

    im_countdown_t cd;
    im_countdown_init(&cd);

    im_shadow_t sh;
    im_shadow_init(&sh, 0, 10000);

    double t0, t1;
    uint32_t i;

    /* ── Core Mapping ── */
    printf("Core Mapping:\n");

    t0 = now_ns();
    for (i = 0; i < iters; i++) sink_skel(im_skel((uint16_t)(i % 720)));
    t1 = now_ns();
    report("B1:  im_skel (O(1) lookup)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        im_addr_t a = im_decompose((uint16_t)(i % 720));
        sink_u8(a.skel.zone);
    }
    t1 = now_ns();
    report("B2:  im_decompose (full)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        im_screen_t s = im_project((int16_t)(i%12),(int16_t)(i%12),(int16_t)(i%12));
        sink_u16((uint16_t)s.sx);
    }
    t1 = now_ns();
    report("B3:  im_project (cube->screen)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        uint32_t x, y;
        im_peano_xy(i & 0xFF, 4, &x, &y);
        sink_u32(im_peano_d(x, y, 4));
    }
    t1 = now_ns();
    report("B4:  Peano xy+d roundtrip", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        sink_u32(im_peano_traverse(i & 0xF, 2));
    }
    t1 = now_ns();
    report("B5:  Peano traverse (next)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        sink_u8(im_snap_gear((uint16_t)(100 + (i % 4000))));
    }
    t1 = now_ns();
    report("B6:  im_snap_gear", (t1-t0)/iters);

    /* ── Signatures ── */
    printf("\nSignatures:\n");

    t0 = now_ns();
    for (i = 0; i < iters; i++) sink_u32(im_crc32c_block(dense, 64));
    t1 = now_ns();
    report("B7:  CRC32C block (64B)", (t1-t0)/iters);

    uint64_t state6[6] = {1,2,3,4,5,6};
    t0 = now_ns();
    for (i = 0; i < iters; i++) sink_u32(im_sig_fast(state6));
    t1 = now_ns();
    report("B8:  CRC32C sig_fast (48B)", (t1-t0)/iters);

    /* ── Decision Tree ── */
    printf("\nDecision Tree:\n");

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        sink_u8((uint8_t)im_decide(prev_chunk, prev_chunk, 1));
    }
    t1 = now_ns();
    report("B9:  im_decide (IDENTITY)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        sink_u8((uint8_t)im_decide(dense, NULL, 0));
    }
    t1 = now_ns();
    report("B10: im_decide (RAW path)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        sink_u8((uint8_t)im_decide(zeros, NULL, 0));
    }
    t1 = now_ns();
    report("B10b:im_decide (FLAT path)", (t1-t0)/iters);

    /* ── Tuning Components ── */
    printf("\nTuning Components:\n");

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        im_cache_lookup(&cache, (uint16_t)(300 + (i % 100)));
    }
    t1 = now_ns();
    report("B11: cache_lookup (miss)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        im_cache_lookup(&cache, 100);
    }
    t1 = now_ns();
    report("B12: cache_lookup (L1 hit)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        im_cache_lookup(&cache, 200);
    }
    t1 = now_ns();
    report("B13: cache_lookup (L2 hit)", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) sink_u32(im_permute(i, &perm));
    t1 = now_ns();
    report("B14: im_permute", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        im_rewind_push(&rw, (uint16_t)(i % 720), i);
    }
    t1 = now_ns();
    report("B15: im_rewind_push", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        sink_u8((uint8_t)im_countdown_tick(&cd));
    }
    t1 = now_ns();
    report("B16: im_countdown_tick", (t1-t0)/iters);

    t0 = now_ns();
    for (i = 0; i < iters; i++) im_shadow_advance(&sh);
    t1 = now_ns();
    report("B17: im_shadow_advance", (t1-t0)/iters);

    /* ── Full Pipeline ── */
    printf("\nFull Pipeline:\n");

    im_ctx_t ctx;
    im_ctx_init(&ctx);
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        im_strategy_t s = im_process_chunk(&ctx,
            (uint16_t)(i % 720),
            (i & 1) ? dense : zeros);
        sink_u8((uint8_t)s);
    }
    t1 = now_ns();
    report("B18: im_process_chunk (full)", (t1-t0)/iters);

    printf("\nDone.\n");
    return 0;
}
