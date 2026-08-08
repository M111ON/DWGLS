/*
 * trig_alternatives_test.c — หาทางเลี่ยง trig ใน Cayley transform
 *
 * ทดสอบ:
 * 1. LUT (lookup table) — precompute cos/sin ทุก slot
 * 2. Rational parametrization — cos/sin จาก t = tan(θ/2)
 * 3. Direct rational — slot → Cayley โดยไม่ผ่าน angle
 * 4. Symmetry shortcut — exploit unit circle structure
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/trig_alt tests/trig_alternatives_test.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../core/hyperbolic_seek.h"

#define GEO_SLOTS   20736u
#define AXIS_SLOTS  6912u
#define PI          3.14159265358979323846

static inline uint32_t geo_slot(uint32_t idx) {
    return (idx * 37u) % GEO_SLOTS;
}
static inline uint32_t geo_inverse(uint32_t slot) {
    return (slot * 16813u) % GEO_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Method 1: LUT — precompute cos/sin ทุก slot
   ═══════════════════════════════════════════════════════════════════════════ */

static double LUT_cos[AXIS_SLOTS];
static double LUT_sin[AXIS_SLOTS];
static int LUT_built = 0;

static void build_lut(void) {
    for (uint32_t i = 0; i < AXIS_SLOTS; i++) {
        double angle = 2.0 * PI * (double)i / (double)AXIS_SLOTS;
        LUT_cos[i] = cos(angle);
        LUT_sin[i] = sin(angle);
    }
    LUT_built = 1;
}

/* KIS → Hyper ด้วย LUT (ไม่ trig) */
static HypComplex kis_to_hyper_lut(uint32_t slot, uint8_t axis) {
    uint32_t as = slot % AXIS_SLOTS;
    double c = LUT_cos[as];
    double s = LUT_sin[as];

    /* Phase rotation 120° — precomputed */
    double cos120 = -0.5;
    double sin120[] = {0.866025403784439, -0.866025403784439, 0.0};

    double c_rot = c * cos120 - s * sin120[axis];
    double s_rot = c * sin120[axis] + s * cos120;

    HypComplex z = {c_rot, s_rot};
    HypComplex one = {1.0, 0.0};
    HypComplex i_unit = {0.0, 1.0};

    /* w = i(1-z)/(1+z) — rational only, no trig */
    HypComplex numer = {one.re - z.re, one.im - z.im};
    HypComplex denom = {one.re + z.re, one.im + z.im};
    double d = denom.re * denom.re + denom.im * denom.im;
    if (d < 1e-15) return (HypComplex){1e15, 0};

    HypComplex result;
    result.re = -(numer.im * denom.re - numer.re * denom.im) / d;
    result.im = (numer.re * denom.re + numer.im * denom.im) / d;
    return result;
}

/* Hyper → KIS ด้วย LUT + atan2 (ยังมี atan2) */
static uint32_t hyper_to_kis_lut(HypComplex w, uint8_t axis) {
    HypComplex i_unit = {0.0, 1.0};
    HypComplex numer = {i_unit.re - w.re, i_unit.im - w.im};
    HypComplex denom = {i_unit.re + w.re, i_unit.im + w.im};
    double d = denom.re * denom.re + denom.im * denom.im;
    if (d < 1e-15) return 0;

    HypComplex z;
    z.re = (numer.re * denom.re + numer.im * denom.im) / d;
    z.im = (numer.im * denom.re - numer.re * denom.im) / d;

    double angle = atan2(z.im, z.re);
    if (angle < 0) angle += 2.0 * PI;

    angle -= (double)axis * 2.0 * PI / 3.0;
    if (angle < 0) angle += 2.0 * PI;

    uint32_t slot = (uint32_t)(angle * AXIS_SLOTS / (2.0 * PI));
    return slot;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Method 2: Rational Parametrization — tan(θ/2) substitution
   ═══════════════════════════════════════════════════════════════════════════
   t = tan(θ/2)
   cos(θ) = (1-t²)/(1+t²)
   sin(θ) = 2t/(1+t²)

   Problem: ยังต้องคำนวณ t = tan(θ/2) ซึ่งก็คือ trig อีกที
   ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
   Method 3: Direct Rational — slot → z โดยไม่ผ่าน angle
   ═══════════════════════════════════════════════════════════════════════════
   z = e^(iθ) บน unit circle
   θ = 2π × slot / AXIS_SLOTS

   ปัญหา: e^(iθ) = cos(θ) + i·sin(θ) = trig

   แต่ถ้าเรารู้ cos, sin อยู่แล้ว (LUT):
   z = {LUT_cos[slot], LUT_sin[slot]}
   → Cayley: w = i(1-z)/(1+z) = rational
   → ไม่ต้อง trig เพิ่ม!
   ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
   Method 4: Symmetry —  exploiting unit circle structure
   ═══════════════════════════════════════════════════════════════════════════
   cos(θ+2π/N) = cos(θ)cos(2π/N) - sin(θ)sin(2π/N)
   sin(θ+2π/N) = sin(θ)cos(2π/N) + cos(θ)sin(2π/N)

   Incremental update: cos_{n+1} = cos_n × a - sin_n × b
                      sin_{n+1} = sin_n × a + cos_n × b
   where a = cos(2π/N), b = sin(2π/N) — constants!

   → No trig at all after initialization!
   → Pure multiply + add
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    double re, im;
} Complex;

static Complex incremental_cayley(uint32_t slot, uint8_t axis) {
    static int initialized = 0;
    static double step_cos, step_sin;
    static double cos120, sin120;

    if (!initialized) {
        step_cos = cos(2.0 * PI / AXIS_SLOTS);
        step_sin = sin(2.0 * PI / AXIS_SLOTS);
        cos120 = cos(2.0 * PI / 3.0);
        sin120 = sin(2.0 * PI / 3.0);
        initialized = 1;
    }

    uint32_t as = slot % AXIS_SLOTS;

    /* Incremental: start from (1,0) and step as times */
    double c = 1.0, s = 0.0;
    for (uint32_t i = 0; i < as; i++) {
        double c_new = c * step_cos - s * step_sin;
        double s_new = s * step_cos + c * step_sin;
        c = c_new;
        s = s_new;
    }

    /* Phase rotation 120° */
    double c_rot = c * cos120 - s * sin120;
    double s_rot = s * cos120 + c * sin120;

    /* Cayley: w = i(1-z)/(1+z) */
    double numer_re = 1.0 - c_rot;
    double numer_im = -s_rot;
    double denom_re = 1.0 + c_rot;
    double denom_im = s_rot;
    double d = denom_re * denom_re + denom_im * denom_im;
    if (d < 1e-15) return (Complex){1e15, 0};

    Complex w;
    w.re = -(numer_im * denom_re - numer_re * denom_im) / d;
    w.im = (numer_re * denom_re + numer_im * denom_im) / d;
    return w;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Benchmark ทุก method
   ═══════════════════════════════════════════════════════════════════════════ */

static void bench_all(uint32_t n) {
    volatile uint32_t sink = 0;

    printf("  %-25s %-10s %-12s\n", "Method", "Time(ms)", "ns/op");
    printf("  %-25s %-10s %-12s\n", "──────", "────────", "─────");

    /* Original trig */
    clock_t t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i) % AXIS_SLOTS;
        uint8_t axis = slot % 3;
        HypComplex h = kis_to_hyperbolic_axis(slot, axis);
        sink += (uint32_t)(h.re * 1000);
    }
    double orig = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("  %-25s %-10.3f %-12.1f\n", "Original (trig)", orig, orig*1e6/n);

    /* LUT forward (no trig) */
    build_lut();
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i) % AXIS_SLOTS;
        uint8_t axis = slot % 3;
        HypComplex h = kis_to_hyper_lut(slot, axis);
        sink += (uint32_t)(h.re * 1000);
    }
    double lut = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("  %-25s %-10.3f %-12.1f\n", "LUT (no trig)", lut, lut*1e6/n);

    /* LUT + atan2 (still has atan2) */
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i) % AXIS_SLOTS;
        uint8_t axis = slot % 3;
        HypComplex h = kis_to_hyper_lut(slot, axis);
        uint32_t back = hyper_to_kis_lut(h, axis);
        sink += back;
    }
    double lut_back = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("  %-25s %-10.3f %-12.1f\n", "LUT + atan2 (roundtrip)", lut_back, lut_back*1e6/n);

    /* Pure integer multiply */
    t0 = clock();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = geo_slot(i);
        uint32_t val = geo_inverse(slot);
        sink += val;
    }
    double integer = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("  %-25s %-10.3f %-12.1f\n", "Pure integer (baseline)", integer, integer*1e6/n);

    printf("\n  Ratio vs integer: LUT forward = %.1fx\n", lut/integer);
    printf("  Ratio vs integer: LUT roundtrip = %.1fx\n\n", lut_back/integer);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Accuracy check: LUT vs original
   ═══════════════════════════════════════════════════════════════════════════ */

static void check_accuracy(void) {
    printf("═══ Accuracy: LUT vs Original ═══\n\n");

    build_lut();

    double max_diff = 0;
    uint32_t max_diff_slot = 0;

    for (uint32_t i = 0; i < AXIS_SLOTS; i++) {
        uint8_t axis = i % 3;

        HypComplex orig = kis_to_hyperbolic_axis(i, axis);
        HypComplex lut = kis_to_hyper_lut(i, axis);

        double diff = sqrt((orig.re - lut.re) * (orig.re - lut.re) +
                           (orig.im - lut.im) * (orig.im - lut.im));

        if (diff > max_diff) {
            max_diff = diff;
            max_diff_slot = i;
        }
    }

    printf("  Max diff: %.15f (slot %u)\n", max_diff, max_diff_slot);
    printf("  Accuracy: %s\n\n",
           max_diff < 1e-10 ? "EXACT ✓" :
           max_diff < 1e-6 ? "NEAR EXACT ✓" :
           max_diff < 1e-3 ? "APPROXIMATE ⚠" : "BAD ✗");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Roundtrip check: LUT forward + LUT back
   ═══════════════════════════════════════════════════════════════════════════ */

static void check_roundtrip(void) {
    printf("═══ Roundtrip: LUT forward + LUT back ═══\n\n");

    build_lut();

    int pass = 0, fail = 0;

    for (uint32_t i = 0; i < AXIS_SLOTS; i++) {
        uint8_t axis = i % 3;

        HypComplex h = kis_to_hyper_lut(i, axis);
        uint32_t back = hyper_to_kis_lut(h, axis);

        if (back == i) pass++;
        else fail++;
    }

    printf("  LUT roundtrip: %d PASS, %d FAIL (%.1f%%)\n\n",
           pass, fail, 100.0 * pass / AXIS_SLOTS);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Trig Alternatives — หาทางเลี่ยง trig ใน Cayley        ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    check_accuracy();
    check_roundtrip();
    bench_all(100000);

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  สรุป\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  LUT forward = ไม่ต้อง trig (cos/sin) แต่ยังมี memory access\n");
    printf("  LUT roundtrip = ยังต้อง atan2 (inverse angle)\n");
    printf("  ปัญหาหลัก = atan2 ใน inverse path\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
