/*
 * twin_seeker_test.c — Twin Seeker: KIS + Hyper พร้อมกัน
 *
 * เปรียบเทียบ:
 *   1. Frame Seek (KIS only) — เร็ว
 *   2. Hyperbolic Seek (Hyper only) — ช้า
 *   3. Twin Seeker (ทั้งคู่พร้อมกัน) — เร็วเท่า frame + rich เท่า hyper
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/twin_seeker_test tests/twin_seeker_test.c -lm
 * RUN:   build/twin_seeker_test
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../core/hyperbolic_seek.h"
#include "../core/geo_kis_projection.h"

#define GEO_SLOTS   20736u
#define STRIDE_37   37u
#define STRIDE_INV  16813u
#define BOUND_START 5184u
#define BOUND_END   15552u
#define BOUND_SIZE  (BOUND_END - BOUND_START)  /* 10368 */

/* ═══════════════════════════════════════════════════════════════════════════
   Geometry Address Functions
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t geo_slot(uint32_t idx) {
    return (idx * STRIDE_37) % GEO_SLOTS;
}

static inline uint32_t geo_inverse(uint32_t slot) {
    return (slot * STRIDE_INV) % GEO_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Bound Range — ไข่ดาว structure
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t size;
    double   ratio;  /* scale ratio = size / GEO_SLOTS */
} BoundRange;

static inline BoundRange bound_create(uint32_t start, uint32_t end) {
    BoundRange b;
    b.start = start;
    b.end = end;
    b.size = end - start;
    b.ratio = (double)b.size / (double)GEO_SLOTS;
    return b;
}

/* Map slot within bound range */
static inline uint32_t bound_slot(BoundRange b, uint32_t local_idx) {
    return b.start + (local_idx % b.size);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Twin Seeker — เลข 8
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* KIS side (fast, integer) */
    uint32_t kis_slot;
    uint32_t kis_value;

    /* Hyper side (rich, float) */
    HypComplex hyper_pos;
    double     hyper_scale;
    uint32_t   hyper_slot;

    /* Bound range */
    BoundRange bound;
    double     ratio;
} TwinResult;

/*
 * twin_seek — อ่านทั้ง 2 โลกพร้อมกันจากจุดเดียว
 *
 * 1. คำนวณ KIS slot (integer, O(1))
 * 2. คำนวณ Hyper position (float, O(1))
 * 3. ทั้งคู่ bind ด้วย ratio เดียวกัน
 */
static TwinResult twin_seek(uint32_t global_slot, BoundRange bound) {
    TwinResult r;
    memset(&r, 0, sizeof(r));

    /* KIS side — เร็ว, integer */
    r.kis_slot = global_slot;
    r.kis_value = geo_inverse(global_slot);

    /* Hyper side — rich, float */
    uint8_t axis = hyperbolic_axis_of(global_slot);   /* owner band, NOT %% 3 */
    r.hyper_pos = kis_to_hyperbolic_axis(global_slot, axis);
    r.hyper_slot = hyperbolic_to_kis_axis(r.hyper_pos, axis);

    /* Bind — ratio เดียวกัน */
    r.bound = bound;
    r.ratio = bound.ratio;

    return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Benchmark — เปรียบเทียบ 3 วิธี
   ═══════════════════════════════════════════════════════════════════════════ */

static void benchmark_frame_seek(uint32_t n) {
    volatile uint32_t sink = 0;
    clock_t t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = bound_slot(bound_create(BOUND_START, BOUND_END), i);
        uint32_t val = geo_inverse(slot);
        sink += val;
    }
    clock_t t1 = clock();
    double ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("  Frame Seek:     %8.3f ms (%u ops, %.1f ns/op)\n",
           ms, n, ms * 1e6 / n);
}

static void benchmark_hyper_seek(uint32_t n) {
    volatile uint32_t sink = 0;
    clock_t t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = bound_slot(bound_create(BOUND_START, BOUND_END), i);
        uint8_t axis = hyperbolic_axis_of(slot);
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);
        uint32_t back = hyperbolic_to_kis_axis(h, axis);
        sink += back;
    }
    clock_t t1 = clock();
    double ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("  Hyper Seek:     %8.3f ms (%u ops, %.1f ns/op)\n",
           ms, n, ms * 1e6 / n);
}

static void benchmark_twin_seek(uint32_t n) {
    volatile uint32_t sink = 0;
    BoundRange bound = bound_create(BOUND_START, BOUND_END);
    clock_t t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = bound_slot(bound, i);
        TwinResult r = twin_seek(slot, bound);
        sink += r.kis_value + r.hyper_slot;
    }
    clock_t t1 = clock();
    double ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("  Twin Seeker:    %8.3f ms (%u ops, %.1f ns/op)\n",
           ms, n, ms * 1e6 / n);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Twin Seeker Test — KIS + Hyper พร้อมกัน                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    BoundRange bound = bound_create(BOUND_START, BOUND_END);

    /* ── Step 1: Bound Range ────────────────────────────────────────────── */
    printf("═══ Step 1: Bound Range (ไข่ดาว) ═══\n");
    printf("  Start:    %u\n", bound.start);
    printf("  End:      %u\n", bound.end);
    printf("  Size:     %u slots\n", bound.size);
    printf("  Ratio:    %.4f (KIS/4)\n\n", bound.ratio);

    /* ── Step 2: Twin Seek Demo ────────────────────────────────────────── */
    printf("═══ Step 2: Twin Seek Demo ═══\n\n");

    printf("  %-8s %-10s %-10s %-10s %-10s\n",
           "Slot", "KIS Value", "Hyper Re", "Hyper Im", "Hyper Back");
    printf("  %-8s %-10s %-10s %-10s %-10s\n",
           "────", "────────", "────────", "────────", "──────────");

    for (uint32_t i = 0; i < 10; i++) {
        uint32_t slot = bound_slot(bound, i);
        TwinResult r = twin_seek(slot, bound);

        printf("  [%5u]  [%5u]    [%.3f]   [%.3f]   [%u]\n",
               r.kis_slot, r.kis_value,
               r.hyper_pos.re, r.hyper_pos.im,
               r.hyper_slot);
    }

    /* ── Step 3: Roundtrip Proof ────────────────────────────────────────── */
    printf("\n═══ Step 3: Roundtrip Proof ═══\n\n");

    int kis_pass = 0, kis_fail = 0, hyp_pass = 0, hyp_fail = 0;
    for (uint32_t i = 0; i < BOUND_SIZE; i++) {
        uint32_t slot = bound_slot(bound, i);
        TwinResult r = twin_seek(slot, bound);

        /* KIS roundtrip */
        uint32_t kis_back = geo_slot(r.kis_value);
        if (kis_back == slot) kis_pass++; else kis_fail++;

        /* Hyper roundtrip */
        if (r.hyper_slot == slot) hyp_pass++; else hyp_fail++;
    }

    printf("  KIS roundtrip:    %d PASS, %d FAIL\n", kis_pass, kis_fail);
    printf("  Hyper roundtrip:  %d PASS, %d FAIL\n", hyp_pass, hyp_fail);
    printf("  Twin (both):      %d PASS, %d FAIL\n",
           (kis_pass == (int)BOUND_SIZE && hyp_pass == (int)BOUND_SIZE)
               ? (int)BOUND_SIZE : 0,
           (kis_pass == (int)BOUND_SIZE && hyp_pass == (int)BOUND_SIZE)
               ? 0 : (int)BOUND_SIZE);

    /* ── Step 4: Benchmark ──────────────────────────────────────────────── */
    printf("\n═══ Step 4: Benchmark ═══\n\n");

    uint32_t n = 100000;

    benchmark_frame_seek(n);
    benchmark_hyper_seek(n);
    benchmark_twin_seek(n);

    /* ── Step 5: Ratio Analysis ────────────────────────────────────────── */
    printf("\n═══ Step 5: Ratio Analysis ═══\n\n");

    /* Measure actual speedup */
    clock_t t0, t1;
    double frame_ms, hyper_ms, twin_ms;

    /* Frame */
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = bound_slot(bound, i);
        volatile uint32_t v = geo_inverse(slot);
        (void)v;
    }
    t1 = clock();
    frame_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    /* Hyper */
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = bound_slot(bound, i);
        uint8_t axis = hyperbolic_axis_of(slot);
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);
        volatile uint32_t v = hyperbolic_to_kis_axis(h, axis);
        (void)v;
    }
    t1 = clock();
    hyper_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    /* Twin */
    BoundRange b = bound_create(BOUND_START, BOUND_END);
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = bound_slot(b, i);
        TwinResult r = twin_seek(slot, b);
        volatile uint32_t v = r.kis_value + r.hyper_slot;
        (void)v;
    }
    t1 = clock();
    twin_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    printf("  Frame:  %.3f ms (baseline)\n", frame_ms);
    printf("  Hyper:  %.3f ms (%.1fx slower)\n", hyper_ms, hyper_ms / frame_ms);
    printf("  Twin:   %.3f ms (%.1fx frame)\n", twin_ms, twin_ms / frame_ms);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Twin Seeker:\n");
    printf("    - KIS side:  frame_seek speed (integer, O(1))\n");
    printf("    - Hyper side: hyperbolic context (float, O(1))\n");
    printf("    - Both bound by ratio (same slot)\n");
    printf("    - Reads 2 worlds simultaneously from 1 point\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
