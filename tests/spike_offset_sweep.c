/*
 * spike_offset_sweep.c — หา offset ที่เหมาะสมสำหรับ invert scale
 *
 * ลอง offset หลายๆ ค่า แล้ววัด:
 *   1. อยู่ใน ring (ไม่ทะลุ)
 *   2. ทะลุ ring (เข้า hyper)
 *   3. หา threshold ที่เปลี่ยนสถานะ
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/spike_offset_sweep tests/spike_offset_sweep.c -lm
 * RUN:   build/spike_offset_sweep
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define GEO_SLOTS   20736u
#define STRIDE_37   37u
#define PHI         1.618033988749895
#define PHI_INV     0.618033988749895

/* ═══════════════════════════════════════════════════════════════════════════
   Geometry
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t geo_slot(uint32_t idx) {
    return (idx * STRIDE_37) % GEO_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Spike Height — ความสูงของ spike icosa ↔ dodeca
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Normal spike: height = h
 * Inverted spike: height = h + offset
 *
 * ถ้า spike สูงกว่า ring threshold → ทะลุเข้า hyper
 */

/* Ring threshold — ขนาด ring ที่ data ยังอยู่ข้างใน */
static double ring_threshold(double ring_size, double data_length) {
    return data_length / ring_size;
}

/* Spike height — คำนวณจาก offset */
static double spike_height(double base_height, double offset) {
    return base_height + offset;
}

/* ทะลุหรือไม่ — เปรียบเทียบ spike height กับ ring expansion rate */
static int breaks_through(double spike_h, double ring_rate) {
    return spike_h > ring_rate;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Sweep — ลอง offset หลายๆ ค่า
   ═══════════════════════════════════════════════════════════════════════════ */

static void sweep_offsets(void) {
    printf("═══ Sweep: หา offset ที่เหมาะสม ═══\n\n");

    /* Parameters */
    double base_height = 1.0;      /* spike height ปกติ */
    double ring_rate = 1.0;        /* ring expansion rate */
    double ring_size = 20736.0;    /* address space */

    printf("  Base spike height: %.3f\n", base_height);
    printf("  Ring expansion rate: %.3f\n", ring_rate);
    printf("  Ring size: %.0f\n\n", ring_size);

    /* Sweep offsets */
    double offsets[] = {
        0.0001, 0.0002, 0.0005,
        0.001, 0.002, 0.005,
        0.01, 0.02, 0.05,
        0.1, 0.2, 0.5,
        1.0, 2.0, 5.0
    };
    int n_offsets = sizeof(offsets) / sizeof(offsets[0]);

    printf("  %-10s %-12s %-10s %-10s %-10s\n",
           "Offset", "Spike H", "Ring Rate", "Ratio", "Status");
    printf("  %-10s %-12s %-10s %-10s %-10s\n",
           "──────", "───────", "────────", "─────", "──────");

    double threshold_offset = -1.0;

    for (int i = 0; i < n_offsets; i++) {
        double offset = offsets[i];
        double h = spike_height(base_height, offset);
        int through = breaks_through(h, ring_rate);
        double ratio = h / ring_rate;

        /* หา threshold */
        if (through && threshold_offset < 0) {
            threshold_offset = offset;
        }

        printf("  [%7.4f] [%8.4f] [%8.4f] [%6.3f] %s\n",
               offset, h, ring_rate, ratio,
               through ? "THROUGH → hyper" : "IN RING");
    }

    printf("\n  Threshold offset: %.4f\n", threshold_offset);
    printf("  (offset >= %.4f → ทะลุเข้า hyper)\n\n", threshold_offset);
}

/* ═══════════════════════════════════════════════════════════════════════════
   φ-based offset — ใช้ phi ratio
   ═══════════════════════════════════════════════════════════════════════════ */

static void phi_based_offsets(void) {
    printf("═══ φ-based Offsets ═══\n\n");

    printf("  φ = %.15f\n", PHI);
    printf("  1/φ = %.15f\n", PHI_INV);
    printf("  1/φ² = %.15f\n\n", PHI_INV * PHI_INV);

    /* φ-based offsets */
    double phi_offsets[] = {
        PHI_INV * PHI_INV,          /* 1/φ² = 0.382 */
        PHI_INV * PHI_INV * 0.1,    /* 1/φ² × 0.1 = 0.038 */
        PHI_INV * PHI_INV * 0.01,   /* 1/φ² × 0.01 = 0.0038 */
        PHI_INV * 0.1,              /* 1/φ × 0.1 = 0.062 */
        PHI_INV * 0.01,             /* 1/φ × 0.01 = 0.0062 */
        1.0 / 20736.0,              /* 1/GEO_SLOTS = 0.000048 */
        1.0 / 144.0,                /* 1/144 = 0.00694 */
        1.0 / 12.0,                 /* 1/12 = 0.0833 */
    };

    const char *descriptions[] = {
        "1/φ²",
        "1/φ² × 0.1",
        "1/φ² × 0.01",
        "1/φ × 0.1",
        "1/φ × 0.01",
        "1/GEO_SLOTS",
        "1/144",
        "1/12",
    };

    double base_height = 1.0;
    double ring_rate = 1.0;

    printf("  %-20s %-12s %-10s %-10s\n",
           "Description", "Offset", "Spike H", "Status");
    printf("  %-20s %-12s %-10s %-10s\n",
           "───────────", "──────", "───────", "──────");

    for (int i = 0; i < 8; i++) {
        double offset = phi_offsets[i];
        double h = spike_height(base_height, offset);
        int through = breaks_through(h, ring_rate);

        printf("  %-20s [%8.5f] [%8.5f] %s\n",
               descriptions[i], offset, h,
               through ? "THROUGH" : "IN RING");
    }

    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Fine Sweep — หา threshold แม่นยำ
   ═══════════════════════════════════════════════════════════════════════════ */

static void fine_sweep(void) {
    printf("═══ Fine Sweep — หา threshold แม่นยำ ═══\n\n");

    double base_height = 1.0;
    double ring_rate = 1.0;

    /* Binary search for threshold */
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 50; i++) {
        double mid = (lo + hi) / 2.0;
        double h = spike_height(base_height, mid);
        if (breaks_through(h, ring_rate)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }

    double threshold = (lo + hi) / 2.0;

    printf("  Threshold offset: %.15f\n", threshold);
    printf("  Spike height at threshold: %.15f\n", spike_height(base_height, threshold));
    printf("  Ring rate: %.15f\n\n", ring_rate);

    /* Verify */
    printf("  Verify:\n");
    printf("    offset = %.15f → spike = %.15f → %s\n",
           threshold - 0.000001,
           spike_height(base_height, threshold - 0.000001),
           breaks_through(spike_height(base_height, threshold - 0.000001), ring_rate) ? "THROUGH" : "IN RING");
    printf("    offset = %.15f → spike = %.15f → %s\n",
           threshold,
           spike_height(base_height, threshold),
           breaks_through(spike_height(base_height, threshold), ring_rate) ? "THROUGH" : "IN RING");
    printf("    offset = %.15f → spike = %.15f → %s\n",
           threshold + 0.000001,
           spike_height(base_height, threshold + 0.000001),
           breaks_through(spike_height(base_height, threshold + 0.000001), ring_rate) ? "THROUGH" : "IN RING");

    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Spike Offset Sweep — หา offset ที่เหมาะสม              ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    sweep_offsets();
    phi_based_offsets();
    fine_sweep();

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Spike height = base + offset\n");
    printf("  Offset < threshold → อยู่ใน ring (ปกติ)\n");
    printf("  Offset >= threshold → ทะลุ ring (เข้า hyper)\n");
    printf("  Threshold ต้องจูนหาเอง (experiment)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
