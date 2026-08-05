/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_prune.c — Test geometric pruning engine
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1: Uniform weights — all types get count > 0
 *   T2: Uniform weights — no pruning at 0.5% threshold
 *   T3: Heavy skew — type with most mass is kept
 *   T4: Mask correctness — kept + pruned = total
 *   T5: Mask pruned count matches report
 *   T6: Threshold sweep — prune ratio monotonically non-decreasing
 *   T7: Zero weights — mass=0, counts still valid
 *   T8: Per-type analysis — mean_abs computed correctly
 *
 * Compile:
 *   gcc -O2 -Wall -Icore -o tests/test_geo_prune.exe tests/test_geo_prune.c -lm
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "geo_cell_prune.h"

#define MAX_GEN  CUBE_ADDR_GEN_MAX   /* 31 — use full range */
#define SNAP     20736u

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(n, desc, cond) do { \
    if (cond) { pass_count++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail_count++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

int main(void) {
    printf("Geometric Pruning Engine Test\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* ── T1: Uniform weights — all 8 types populated ────────── */
    printf("T1: Uniform Weights — All Types Populated\n");
    {
        float w[SNAP];
        for (uint32_t i = 0; i < SNAP; i++) w[i] = 1.0f;

        GeoPruneReport rpt;
        geo_cell_prune_analyze(w, SNAP, MAX_GEN, &rpt);

        int nonzero = 0;
        for (int ct = 0; ct < 8; ct++)
            if (rpt.count[ct] > 0) nonzero++;

        CHECK(1, "all 8 types have counts", nonzero == 8);
        CHECK(2, "total_count = SNAP", rpt.total_count == SNAP);
        CHECK(3, "total_mass = SNAP", fabs(rpt.total_mass - SNAP) < 0.1);

        printf("    Distribution: ");
        for (int ct = 0; ct < 8; ct++)
            printf("%s:%u ", geo_cell_classify_name(ct), rpt.count[ct]);
        printf("\n\n");
    }

    /* ── T2: Uniform weights — no pruning at low threshold ──── */
    printf("T2: Uniform — No Pruning at 0.5%% Threshold\n");
    {
        float w[SNAP];
        for (uint32_t i = 0; i < SNAP; i++) w[i] = 1.0f;

        GeoPruneReport rpt;
        geo_cell_prune_analyze(w, SNAP, MAX_GEN, &rpt);

        /* With uniform weights, each type has > 0.5% of total count */
        CHECK(4, "no types prunable at 0.5%", rpt.prunable_types == 0);
        printf("\n");
    }

    /* ── T3: Heavy skew — mass集中在 type 0 ────────────────── */
    printf("T3: Heavy Skew (type 0 = 100.0, others = 0.01)\n");
    {
        float w[SNAP];
        memset(w, 0, sizeof(w));

        for (uint32_t i = 0; i < SNAP; i++) {
            GeoCubeAddr addr = geo_flat_to_addr(i);
            uint8_t ct = geo_cell_classify(addr);
            w[i] = (ct == 0) ? 100.0f : 0.01f;
        }

        GeoPruneReport rpt;
        geo_cell_prune_analyze(w, SNAP, MAX_GEN, &rpt);

        CHECK(5, "type 0 has highest mass", rpt.mass[0] > rpt.mass[1]);
        CHECK(6, "type 0 mean_abs >> others",
              rpt.mean_abs[0] > rpt.mean_abs[1] * 100.0);

        printf("    Type 0 mass: %.2f, mean: %.2f\n", rpt.mass[0], rpt.mean_abs[0]);
        printf("    Type 1 mass: %.4f, mean: %.4f\n", rpt.mass[1], rpt.mean_abs[1]);
        printf("\n");
    }

    /* ── T4: Mask correctness — counts match ────────────────── */
    printf("T4: Mask Correctness\n");
    {
        float w[SNAP];
        for (uint32_t i = 0; i < SNAP; i++) w[i] = (float)(i % 10);

        uint8_t *mask = (uint8_t *)malloc(SNAP);
        GeoPruneReport rpt;
        geo_cell_prune_mask(w, SNAP, MAX_GEN, 10.0, mask, &rpt);

        int mask_kept = 0, mask_pruned = 0;
        for (uint32_t i = 0; i < SNAP; i++) {
            if (mask[i]) mask_kept++;
            else mask_pruned++;
        }

        CHECK(7, "kept + pruned = SNAP",
              (uint32_t)(mask_kept + mask_pruned) == SNAP);
        CHECK(8, "mask pruned == report pruned",
              (uint32_t)mask_pruned == rpt.pruned_count);

        printf("    Kept: %d, Pruned: %d (report: %u)\n",
               mask_kept, mask_pruned, rpt.pruned_count);
        free(mask);
        printf("\n");
    }

    /* ── T5: Threshold sweep — monotonically non-decreasing ─── */
    printf("T5: Threshold Sweep\n");
    {
        float w[SNAP];
        for (uint32_t i = 0; i < SNAP; i++) w[i] = 1.0f;

        float thresholds[] = {0.5f, 1.0f, 5.0f, 10.0f, 15.0f, 20.0f};
        float prev_ratio = -1.0f;
        int monotonic = 1;
        uint8_t *mask = (uint8_t *)malloc(SNAP);

        for (int t = 0; t < 6; t++) {
            GeoPruneReport rpt;
            geo_cell_prune_mask(w, SNAP, MAX_GEN, thresholds[t], mask, &rpt);
            printf("    threshold=%5.1f%% → prune %5.1f%%, mass_loss %5.1f%%\n",
                   thresholds[t], rpt.prune_ratio * 100.0, rpt.mass_loss_ratio * 100.0);

            if (prev_ratio >= 0.0f && rpt.prune_ratio + 1e-9 < prev_ratio) {
                monotonic = 0;
                printf("    ← VIOLATION: %.6f < %.6f\n", rpt.prune_ratio, prev_ratio);
            }
            prev_ratio = rpt.prune_ratio;
        }
        CHECK(9, "prune ratio monotonically non-decreasing", monotonic);
        free(mask);
        printf("\n");
    }

    /* ── T6: Zero weights — mass=0, counts valid ────────────── */
    printf("T6: Zero Weights\n");
    {
        float w[SNAP];
        memset(w, 0, sizeof(w));

        GeoPruneReport rpt;
        geo_cell_prune_analyze(w, SNAP, MAX_GEN, &rpt);

        CHECK(10, "total_mass = 0", rpt.total_mass == 0.0);
        CHECK(11, "total_count = SNAP", rpt.total_count == SNAP);

        int all_zero_mean = 1;
        for (int ct = 0; ct < 8; ct++)
            if (rpt.mean_abs[ct] != 0.0) all_zero_mean = 0;
        CHECK(12, "all mean_abs = 0", all_zero_mean);

        printf("    Total count: %u, mass: %.2f\n", rpt.total_count, rpt.total_mass);
        printf("\n");
    }

    /* ── T7: Per-type mean_abs ───────────────────────────────── */
    printf("T7: Per-Type Mean Absolute Value\n");
    {
        float w[SNAP];
        for (uint32_t i = 0; i < SNAP; i++) {
            GeoCubeAddr addr = geo_flat_to_addr(i);
            uint8_t ct = geo_cell_classify(addr);
            /* Type 0: |w|=2, others: |w|=1 */
            w[i] = (ct == 0) ? 2.0f : 1.0f;
        }

        GeoPruneReport rpt;
        geo_cell_prune_analyze(w, SNAP, MAX_GEN, &rpt);

        CHECK(13, "type 0 mean = 2.0", fabs(rpt.mean_abs[0] - 2.0) < 0.01);
        CHECK(14, "type 1 mean = 1.0", fabs(rpt.mean_abs[1] - 1.0) < 0.01);

        printf("    Type 0 mean: %.4f (expect 2.0)\n", rpt.mean_abs[0]);
        printf("    Type 1 mean: %.4f (expect 1.0)\n", rpt.mean_abs[1]);
        printf("\n");
    }

    /* ── T8: Prune then verify mask zeroes correct indices ──── */
    printf("T8: Prune + Verify Zeroed Indices\n");
    {
        float w[SNAP];
        for (uint32_t i = 0; i < SNAP; i++) w[i] = 1.0f;

        uint8_t *mask = (uint8_t *)malloc(SNAP);
        GeoPruneReport rpt;
        geo_cell_prune_mask(w, SNAP, MAX_GEN, 10.0, mask, &rpt);

        /* Apply mask and verify */
        int pruned_verify = 0;
        for (uint32_t i = 0; i < SNAP; i++) {
            if (!mask[i]) {
                w[i] = 0.0f;
                pruned_verify++;
            }
        }

        CHECK(15, "pruned count matches", (uint32_t)pruned_verify == rpt.pruned_count);

        /* After pruning, remaining weights should all be 1.0 */
        int non_one = 0;
        for (uint32_t i = 0; i < SNAP; i++)
            if (w[i] != 0.0f && w[i] != 1.0f) non_one++;
        CHECK(16, "all kept weights are 1.0", non_one == 0);

        printf("    Pruned %d indices, all remaining = 1.0\n", pruned_verify);
        free(mask);
        printf("\n");
    }

    /* ═══════════════════════════════════════════════════════════════
       SUMMARY
       ═══════════════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("FINAL: %d PASS / %d FAIL\n", pass_count, fail_count);
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("KEY INSIGHTS:\n");
    printf("  1. Cell classify maps flat index → 8 types via 3-bit parity\n");
    printf("  2. Pruning mask: types below threshold% → zeroed\n");
    printf("  3. Heavy weights in kept types → mass_loss << prune_ratio\n");
    printf("  4. Threshold sweep: ratio monotonically non-decreasing\n");
    printf("  5. Zero weights: mass=0 but counts/classification still valid\n");

    return fail_count;
}
