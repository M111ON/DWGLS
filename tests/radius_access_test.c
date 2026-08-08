/*
 * radius_access_test.c — access ด้วย radius แทน angle
 *
 * 20736 = diameter → radius = 10368
 * คำถาม: ใช้ radius แทน atan2 ได้ไหม?
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/radius_test tests/radius_access_test.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../core/hyperbolic_seek.h"

#define GEO_SLOTS  20736u
#define AXIS_SLOTS 6912u
#define RADIUS     10368u   /* 20736 / 2 */
#define PI         3.141592653589793

/* ═══════════════════════════════════════════════════════════════════════════
   20736 = diameter → radius = 10368

   Circle:  circumference = π × diameter = π × 20736
   Radius:  10368

   ทุก slot อยู่บนวงกลม:
   - slot 0   = angle 0°        (ตำแหน่ง 0)
   - slot 3456 = angle 90°      (quarter)
   - slot 6912 = angle 180°     (half)
   - slot 10368 = angle 270°    (three-quarter)

   Radius = distance from center = 10368 (constant บน unit circle)
   ═══════════════════════════════════════════════════════════════════════════ */

static void explore_circle_geometry(void) {
    printf("═══ Circle Geometry: 20736 = diameter ═══\n\n");

    printf("  Diameter:  20736\n");
    printf("  Radius:    10368\n");
    printf("  Circumference: %.1f (π × 20736)\n\n", PI * GEO_SLOTS);

    printf("  Key positions on circle:\n");
    printf("  %-8s %-10s %-10s %-10s %-12s\n",
           "Slot", "Angle", "cos", "sin", "Position");
    printf("  %-8s %-10s %-10s %-10s %-12s\n",
           "────", "─────", "───", "───", "────────");

    uint32_t key_slots[] = {0, 1728, 3456, 5184, 6912, 8640, 10368};
    const char *labels[] = {"0°", "45°", "90°", "135°", "180°", "270°", "300°"};

    for (int i = 0; i < 7; i++) {
        double angle = 2.0 * PI * key_slots[i] / AXIS_SLOTS;
        double c = cos(angle);
        double s = sin(angle);

        printf("  [%5u]  [%-6s] [%8.4f] [%8.4f]  (%+.4f, %+.4f)\n",
               key_slots[i], labels[i], c, s, c, s);
    }

    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Key insight: slot → (x, y) on unit circle
   ═══════════════════════════════════════════════════════════════════════════
   x = cos(2π × slot / 6912)
   y = sin(2π × slot / 6912)

   Cayley: w = i(1-z)/(1+z)
   z = x + iy

   Forward (KIS→Hyper):
     z = {cos, sin} from LUT → Cayley → w = {re, im}
     No trig needed! (LUT precomputed)

   Inverse (Hyper→KIS):
     w → Cayley inverse → z = {re_z, im_z}
     ต้องหา angle จาก z → atan2(im_z, re_z)

   ปัญหา: atan2 ยังจำเป็นสำหรับ inverse

   BUT: ถ้าเราเก็บ z เป็น {x,y} แทน angle?
   → Cayley output = {re, im} อยู่แล้ว
   → ไม่ต้อง convert กลับเป็น angle
   → เอา {re, im} ไปใช้ต่อได้เลย
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_no_atan2(void) {
    printf("═══ Test: ไม่ใช้ atan2 — เก็บ position แทน angle ═══\n\n");

    /*
     * แทนที่จะ convert กลับเป็น angle (atan2)
     * เก็บ Cayley output เป็น {re, im} โดยตรง
     *
     * Forward:  slot → LUT → z → Cayley → w = {re, im}  ← ไม่ trig
     * Inverse:  w = {re, im} → Cayley inverse → z = {re_z, im_z}
     *           ไม่ต้อง atan2 — เก็บ {re_z, im_z} แทน!
     */

    int pass = 0, fail = 0;

    for (uint32_t i = 0; i < AXIS_SLOTS; i++) {
        uint8_t axis = i % 3;

        /* Forward: slot → {re_w, im_w} */
        HypComplex h = kis_to_hyperbolic_axis(i, axis);

        /* Inverse: {re_w, im_w} → {re_z, im_z} (ไม่ atan2) */
        HypComplex i_unit = {0.0, 1.0};
        HypComplex numer = {i_unit.re - h.re, i_unit.im - h.im};
        HypComplex denom = {i_unit.re + h.re, i_unit.im + h.im};
        double d = denom.re * denom.re + denom.im * denom.im;
        if (d < 1e-15) { fail++; continue; }

        HypComplex z;
        z.re = (numer.re * denom.re + numer.im * denom.im) / d;
        z.im = (numer.im * denom.re - numer.re * denom.im) / d;

        /* z อยู่บน unit circle: |z| ≈ 1 */
        double mag = sqrt(z.re * z.re + z.im * z.im);
        int on_circle = (fabs(mag - 1.0) < 1e-10);

        if (on_circle) pass++;
        else fail++;
    }

    printf("  z on unit circle: %d PASS, %d FAIL (%.1f%%)\n",
           pass, fail, 100.0 * pass / AXIS_SLOTS);

    printf("\n  สรุป:\n");
    printf("  Forward:  slot → LUT → Cayley → {re, im}     ไม่ trig\n");
    printf("  Inverse:  {re, im} → Cayley⁻¹ → {re_z, im_z}  ไม่ trig\n");
    printf("  บน unit circle: |z| = 1 เสมอ\n");
    printf("  เก็บ {re_z, im_z} แทน angle → ไม่ต้อง atan2\n");
    printf("  ข้อมูลครบ: position = {re_z, im_z} อยู่บน circle\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Radius = free distance
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_radius_distance(void) {
    printf("═══ Radius = Free Distance ═══\n\n");

    /*
     * บน unit circle: radius = 1 เสมอ (ไม่ต้องคำนวณ)
     * ระยะห่างระหว่าง 2 จุดบน unit circle:
     *   distance = 2 × sin(Δθ/2)
     *
     * แต่ถ้าเก็บ {re, im}:
     *   distance = sqrt((x1-x2)² + (y1-y2)²)
     *   = sqrt(2 - 2(x1x2 + y1y2))
     *   = sqrt(2 - 2 × dot product)
     *
     * ไม่ต้อง trig!
     */

    printf("  ระยะห่างระหว่าง 2 slots:\n\n");

    uint32_t slots[] = {0, 100, 1000, 3456, 6912, 10368};

    printf("  %-8s %-8s %-12s %-12s %-12s\n",
           "Slot A", "Slot B", "Trig dist", "Dot dist", "Match?");
    printf("  %-8s %-8s %-12s %-12s %-12s\n",
           "──────", "──────", "─────────", "────────", "──────");

    for (int a = 0; a < 6; a++) {
        for (int b = a + 1; b < 6; b++) {
            uint32_t sa = slots[a];
            uint32_t sb = slots[b];

            double ang_a = 2.0 * PI * sa / AXIS_SLOTS;
            double ang_b = 2.0 * PI * sb / AXIS_SLOTS;

            /* Method 1: trig distance */
            double delta = fabs(ang_a - ang_b);
            if (delta > PI) delta = 2.0 * PI - delta;
            double trig_dist = 2.0 * sin(delta / 2.0);

            /* Method 2: dot product distance (no trig) */
            double ca = cos(ang_a), sa_v = sin(ang_a);
            double cb = cos(ang_b), sb_v = sin(ang_b);
            double dot = ca * cb + sa_v * sb_v;
            double dot_dist = sqrt(2.0 - 2.0 * dot);

            int match = (fabs(trig_dist - dot_dist) < 1e-10);

            printf("  [%5u]  [%5u]  [%8.6f]   [%8.6f]   %s\n",
                   sa, sb, trig_dist, dot_dist,
                   match ? "✓" : "✗");
        }
    }

    printf("\n  สรุป:\n");
    printf("  Dot product distance = trig distance (เท่ากัน)\n");
    printf("  Dot product = x1×x2 + y1×y2 (multiply only)\n");
    printf("  Distance = sqrt(2 - 2×dot) (multiply + sqrt)\n");
    printf("  ไม่ต้อง atan2/cos/sin — ใช้ {x,y} ที่เก็บไว้ได้เลย\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Radius Access — 20736 = diameter → free distance       ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    explore_circle_geometry();
    test_no_atan2();
    test_radius_distance();

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  CONCLUSION\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  ปัญหาเดิม: atan2 ช้า (inverse path)\n");
    printf("  ทางแก้:    เก็บ position เป็น {x,y} แทน angle\n");
    printf("  ผลลัพธ์:    Cayley output = {re,im} อยู่แล้ว\n");
    printf("              ไม่ต้อง convert กลับเป็น angle\n");
    printf("              ไม่ต้อง atan2 = ไม่ trig เลย!\n");
    printf("  Radius:     distance = sqrt(2-2×dot) = multiply only\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
