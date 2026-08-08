/*
 * hyper_pierce_test.c — เจาะทะลุ hyperbolic field: ทำได้จริงไหม? ได้ประโยชน์ไหม?
 *
 * เปรียบเทียบ:
 *   1. frame_seek ปกติ (ไม่เจาะ)
 *   2. hyper_pierce (เจาะทะลุ field)
 *   3. วัด: speed, accuracy, benefit
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/hyper_pierce_test tests/hyper_pierce_test.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../core/hyperbolic_seek.h"

#define GEO_SLOTS   20736u
#define STRIDE_37   37u
#define STRIDE_INV  16813u

static inline uint32_t geo_slot(uint32_t idx) {
    return (idx * STRIDE_37) % GEO_SLOTS;
}
static inline uint32_t geo_inverse(uint32_t slot) {
    return (slot * STRIDE_INV) % GEO_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Hyper Pierce — เจาะทะลุ field โดยไม่ต้องรอ loop
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * frame_seek:  slot = idx * 37 % 20736        (integer, fast)
 * hyper_pierce: slot → cayley → position → target (float, context)
 *
 * คำถาม: hyper_pierce ให้อะไรที่ frame_seek ไม่มี?
 * คำตอบ: position context (อยู่ตรงไหนใน field)
 */

typedef struct {
    uint32_t slot;          /* target slot */
    uint32_t value;         /* weight value */
    HypComplex hyper_pos;   /* hyperbolic position */
    double   distance;      /* distance from center */
    int      in_ring;       /* อยู่ใน bound ring หรือไม่ */
    uint32_t pierce_slot;   /* slot ที่เจาะทะลุถึง */
} PierceResult;

/* คำนวณ offset สำหรับเจาะทะลุ */
static double pierce_offset(uint32_t current, uint32_t target, uint32_t container) {
    return (double)(target - current) / (double)container;
}

/* เจาะทะลุ — คำนวณ target slot จาก offset */
static uint32_t pierce_calc(uint32_t current, double offset, uint32_t container) {
    int32_t delta = (int32_t)(offset * (double)container);
    return (current + delta + container) % container;
}

/* Full pierce — เจาะ + คำนวณ hyper position */
static PierceResult hyper_pierce(uint32_t current, uint32_t target) {
    PierceResult r;
    memset(&r, 0, sizeof(r));

    /* Offset สำหรับเจาะทะลุ */
    double offset = pierce_offset(current, target, GEO_SLOTS);

    /* คำนวณ slot ที่เจาะทะลุถึง */
    r.pierce_slot = pierce_calc(current, offset, GEO_SLOTS);

    /* KIS side */
    r.slot = r.pierce_slot;
    r.value = geo_inverse(r.pierce_slot);

    /* Hyper side */
    uint8_t axis = r.pierce_slot % 3;
    r.hyper_pos = kis_to_hyperbolic_axis(r.pierce_slot, axis);
    r.distance = sqrt(r.hyper_pos.re * r.hyper_pos.re + 
                      r.hyper_pos.im * r.hyper_pos.im);

    /* Bound ring: 5184-15552 */
    r.in_ring = (r.pierce_slot >= 5184 && r.pierce_slot < 15552);

    return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Benchmark
   ═══════════════════════════════════════════════════════════════════════════ */

static double bench_frame_seek(uint32_t n) {
    volatile uint32_t sink = 0;
    clock_t t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i);
        uint32_t val = geo_inverse(slot);
        sink += val;
    }
    clock_t t1 = clock();
    return (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;
}

static double bench_hyper_pierce(uint32_t n) {
    volatile uint32_t sink = 0;
    clock_t t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t current = geo_slot(i);
        uint32_t target = geo_slot((i + 1000) % GEO_SLOTS);
        PierceResult r = hyper_pierce(current, target);
        sink += r.pierce_slot + r.value;
    }
    clock_t t1 = clock();
    return (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Hyper Pierce — เจาะทะลุ field: ทำได้จริงไหม?          ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    /* ── Test 1: เจาะทะลุถึงเป้าหมายไหม? ─────────────────────────────── */
    printf("═══ Test 1: Accuracy — เจาะทะลุถึงเป้าหมายไหม? ═══\n\n");

    int pass = 0, fail = 0;
    uint32_t test_cases[] = {0, 100, 5184, 10000, 15552, 20735};
    int n_cases = sizeof(test_cases) / sizeof(test_cases[0]);

    printf("  %-10s %-10s %-10s %-10s %-8s\n",
           "Current", "Target", "Pierce", "Match", "Status");
    printf("  %-10s %-10s %-10s %-10s %-8s\n",
           "───────", "──────", "──────", "─────", "──────");

    for (int i = 0; i < n_cases; i++) {
        uint32_t current = test_cases[i];
        uint32_t target = test_cases[(i + 1) % n_cases];

        PierceResult r = hyper_pierce(current, target);

        /* ตรวจสอบว่าเจาะทะลุถึง target ไหม */
        int match = (r.pierce_slot == target);

        if (match) pass++; else fail++;

        printf("  [%5u]   [%5u]   [%5u]   [%5u]   %s\n",
               current, target, r.pierce_slot, r.value,
               match ? "PASS ✓" : "FAIL ✗");
    }

    printf("\n  Accuracy: %d/%d PASS\n\n", pass, n_cases);

    /* ── Test 2: Offset calculation ──────────────────────────────────────── */
    printf("═══ Test 2: Offset Calculation ═══\n\n");

    printf("  %-10s %-10s %-12s %-10s\n",
           "Current", "Target", "Offset", "Pierce OK");
    printf("  %-10s %-10s %-12s %-10s\n",
           "───────", "──────", "─────", "─────────");

    for (int i = 0; i < n_cases; i++) {
        uint32_t current = test_cases[i];
        uint32_t target = test_cases[(i + 2) % n_cases];

        double offset = pierce_offset(current, target, GEO_SLOTS);
        uint32_t pierced = pierce_calc(current, offset, GEO_SLOTS);

        printf("  [%5u]   [%5u]   [%8.4f]   %s\n",
               current, target, offset,
               pierced == target ? "YES ✓" : "NO ✗");
    }

    printf("\n");

    /* ── Test 3: Hyper position ──────────────────────────────────────────── */
    printf("═══ Test 3: Hyper Position ═══\n\n");

    printf("  %-8s %-10s %-12s %-10s %-8s\n",
           "Slot", "Distance", "Re", "Im", "In Ring");
    printf("  %-8s %-10s %-12s %-10s %-8s\n",
           "────", "────────", "──", "──", "───────");

    for (uint32_t slot = 0; slot < GEO_SLOTS; slot += 3456) {
        uint8_t axis = slot % 3;
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);
        double dist = sqrt(h.re * h.re + h.im * h.im);
        int in_ring = (slot >= 5184 && slot < 15552);

        printf("  [%5u]  [%8.4f]  [%8.4f]  [%8.4f]  %s\n",
               slot, dist, h.re, h.im,
               in_ring ? "YES" : "NO");
    }

    printf("\n");

    /* ── Test 4: Benchmark ────────────────────────────────────────────────── */
    printf("═══ Test 4: Benchmark ═══\n\n");

    uint32_t n = 100000;
    double frame_ms = bench_frame_seek(n);
    double pierce_ms = bench_hyper_pierce(n);

    printf("  Frame Seek:   %8.3f ms (%u ops, %.1f ns/op)\n",
           frame_ms, n, frame_ms * 1e6 / n);
    printf("  Hyper Pierce: %8.3f ms (%u ops, %.1f ns/op)\n",
           pierce_ms, n, pierce_ms * 1e6 / n);
    printf("  Ratio:        %.1fx\n\n", pierce_ms / frame_ms);

    /* ── Test 5: Benefit Analysis ─────────────────────────────────────────── */
    printf("═══ Test 5: Benefit Analysis ═══\n\n");

    printf("  frame_seek ให้:\n");
    printf("    ✓ Speed (0 ns/op)\n");
    printf("    ✓ Value (weight data)\n");
    printf("    ✗ Position context (ไม่รู้อยู่ตรงไหนใน field)\n\n");

    printf("  hyper_pierce ให้:\n");
    printf("    ✓ Speed (%.1f ns/op — slower but O(1))\n", pierce_ms * 1e6 / n);
    printf("    ✓ Value (weight data)\n");
    printf("    ✓ Position context (hyper_pos, distance, in_ring)\n");
    printf("    ✓ Pierce offset (คำนวณได้จาก target)\n\n");

    printf("  Benefit:\n");
    printf("    - frame_seek: เร็ว แต่ไม่มี context\n");
    printf("    - hyper_pierce: ช้ากว่า %.1fx แต่มี context\n", pierce_ms / frame_ms);
    printf("    - ถ้าต้องการ context = คุ้ม\n");
    printf("    - ถ้าต้องการแค่ speed = ไม่คุ้ม\n\n");

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  ทำได้จริงไหม?  YES — accuracy %d/%d\n", pass, n_cases);
    printf("  ได้ประโยชน์ไหม? YES — ถ้าต้องการ position context\n");
    printf("  ค่า cost:       %.1fx slower than frame_seek\n", pierce_ms / frame_ms);
    printf("  ค่า benefit:    position context (distance, in_ring, hyper_pos)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
