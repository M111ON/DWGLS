/*
 * no_trig_benchmark.c — วัดจริง: LUT+Cayley (ไม่ trig) vs Original (มี trig)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/no_trig_bench tests/no_trig_benchmark.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "../core/hyperbolic_seek.h"

#define GEO_SLOTS  20736u
#define AXIS_SLOTS 6912u
#define PI         3.141592653589793

static inline uint32_t geo_slot(uint32_t idx) {
    return (idx * 37u) % GEO_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   LUT — precompute cos/sin (init once)
   ═══════════════════════════════════════════════════════════════════════════ */
static double LUT_c[AXIS_SLOTS];
static double LUT_s[AXIS_SLOTS];

static void build_lut(void) {
    for (uint32_t i = 0; i < AXIS_SLOTS; i++) {
        double a = 2.0 * PI * (double)i / (double)AXIS_SLOTS;
        LUT_c[i] = cos(a);
        LUT_s[i] = sin(a);
    }
}

/* Forward: slot → Cayley → {re,im} — ไม่ trig (ใช้ LUT) */
static void slot_to_cayley(uint32_t slot, double *out_re, double *out_im) {
    uint32_t as = slot % AXIS_SLOTS;
    double c = LUT_c[as];
    double s = LUT_s[as];

    /* Cayley: w = i(1-z)/(1+z) */
    double nr = 1.0 - c, ni = -s;
    double dr = 1.0 + c, di = s;
    double d = dr * dr + di * di;
    *out_re = -(ni * dr - nr * di) / d;
    *out_im = (nr * dr + ni * di) / d;
}

/* Forward + Distance: วัดระยะจาก center (ไม่ trig) */
static double slot_distance_no_trig(uint32_t slot_a, uint32_t slot_b) {
    double re_a, im_a, re_b, im_b;
    slot_to_cayley(slot_a, &re_a, &im_a);
    slot_to_cayley(slot_b, &re_b, &im_b);
    double dot = re_a * re_b + im_a * im_b;
    return sqrt(2.0 - 2.0 * dot);
}

/* Forward + Distance: วัดระยะแบบเดิม (trig) */
static double slot_distance_trig(uint32_t slot_a, uint32_t slot_b) {
    double a1 = 2.0 * PI * (slot_a % AXIS_SLOTS) / AXIS_SLOTS;
    double a2 = 2.0 * PI * (slot_b % AXIS_SLOTS) / AXIS_SLOTS;
    double d = fabs(a1 - a2);
    if (d > PI) d = 2.0 * PI - d;
    return 2.0 * sin(d / 2.0);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Benchmark
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  No-Trig Benchmark — วัดจริง ใช้ได้ไหม?                 ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    build_lut();

    uint32_t n = 100000;
    volatile double sink = 0;

    /* ── Accuracy: no-trig vs trig distance ────────────────────────── */
    printf("═══ Accuracy ═══\n\n");

    double max_diff = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t a = geo_slot(i);
        uint32_t b = geo_slot((i + 500) % GEO_SLOTS);
        double d_trig = slot_distance_trig(a, b);
        double d_no   = slot_distance_no_trig(a, b);
        double diff = fabs(d_trig - d_no);
        if (diff > max_diff) max_diff = diff;
    }
    printf("  Max distance diff (no-trig vs trig): %.15f\n", max_diff);
    printf("  Status: %s\n\n", max_diff < 1e-10 ? "EXACT ✓" : "BAD ✗");

    /* ── Speed: Forward (slot → Cayley → distance) ─────────────────── */
    printf("═══ Speed: Forward (slot → distance) ═══\n\n");

    clock_t t0, t1;

    /* Trig path */
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = geo_slot(i);
        uint32_t b = geo_slot((i + 100) % GEO_SLOTS);
        sink += slot_distance_trig(a, b);
    }
    t1 = clock();
    double trig_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    /* No-trig path (LUT) */
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = geo_slot(i);
        uint32_t b = geo_slot((i + 100) % GEO_SLOTS);
        sink += slot_distance_no_trig(a, b);
    }
    t1 = clock();
    double lut_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    printf("  Trig:     %8.3f ms (%.1f ns/op)\n", trig_ms, trig_ms*1e6/n);
    printf("  LUT:      %8.3f ms (%.1f ns/op)\n", lut_ms, lut_ms*1e6/n);
    printf("  Speedup:  %.1fx\n\n", trig_ms / lut_ms);

    /* ── Cayley roundtrip: forward only (ไม่ atan2) ────────────────── */
    printf("═══ Cayley Roundtrip (no atan2) ═══\n\n");

    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i);
        double re, im;
        slot_to_cayley(slot, &re, &im);
        sink += re + im;
    }
    t1 = clock();
    double fwd_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    /* Original (with trig) */
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i);
        uint8_t axis = slot % 3;
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);
        sink += h.re + h.im;
    }
    t1 = clock();
    double orig_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    printf("  LUT+Cayley (no trig): %8.3f ms (%.1f ns/op)\n", fwd_ms, fwd_ms*1e6/n);
    printf("  Original (trig):      %8.3f ms (%.1f ns/op)\n", orig_ms, orig_ms*1e6/n);
    printf("  Speedup:              %.1fx\n\n", orig_ms / fwd_ms);

    /* ── Pure integer baseline ──────────────────────────────────────── */
    printf("═══ Baseline ═══\n\n");

    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i);
        sink += slot;
    }
    t1 = clock();
    double int_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    printf("  Integer: %8.3f ms (%.1f ns/op)\n", int_ms, int_ms*1e6/n);
    printf("  LUT+Cayley / Integer = %.1fx\n\n", fwd_ms / int_ms);

    /* ── Summary ────────────────────────────────────────────────────── */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  สรุป\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Original:   %.1f ns/op (มี cos/sin)\n", orig_ms*1e6/n);
    printf("  LUT+Cayley: %.1f ns/op (ไม่ trig)\n", fwd_ms*1e6/n);
    printf("  Integer:    %.1f ns/op (baseline)\n", int_ms*1e6/n);
    printf("  \n");
    printf("  LUT+Cayley เร็วกว่า Original: %.1fx\n", orig_ms/fwd_ms);
    printf("  LUT+Cayley ช้ากว่า Integer:   %.1fx\n", fwd_ms/int_ms);
    printf("  \n");
    printf("  ใช้ได้ไหม? %s\n",
           (fwd_ms/orig_ms < 0.5) ? "YES — เร็วขึ้นอย่างมีนัยยะ" :
           (fwd_ms/orig_ms < 0.8) ? "MAYBE — เร็วขึ้นเล็กน้อย" :
           "NO — ไม่เร็วขึ้น");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
