/* test_kis_mirror_symmetry.c
 * Q8_0 Mirror Symmetry Analysis — 8-Octant KIS Address Space
 *
 * Reads real GGUF weights, maps them to KIS 3-axis address space (20736 slots),
 * then checks mirror symmetry: does flipping (+X→-X, +Y→-Y, +Z→-Z) on each
 * axis produce identical weight values?
 *
 * 8 Octants (3 binary dimensions):
 *   0: (+X, +Y, +Z) = original
 *   1: (-X, +Y, +Z) = X-flip
 *   2: (+X, -Y, +Z) = Y-flip
 *   3: (+X, +Y, -Z) = Z-flip
 *   4: (-X, -Y, +Z) = XY-flip
 *   5: (-X, +Y, -Z) = XZ-flip
 *   6: (+X, -Y, -Z) = YZ-flip
 *   7: (-X, -Y, -Z) = XYZ-flip
 *
 * BUILD: gcc -O2 -Wall -Icore -I.hermes/desktop-attachments \
 *        -o build/test_kis_mirror_symmetry tests/test_kis_mirror_symmetry.c -lm
 *
 * RUN:   build/test_kis_mirror_symmetry I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "gguf_reader.h"

/* ── Constants ──────────────────────────────────────────────────────────── */
#define KIS_TOTAL    20736u      /* 3 × 6912 */
#define KIS_AXIS     6912u       /* slots per axis */
#define KIS_INFINITY 3456u       /* midpoint = KIS_AXIS / 2 */
#define Q8_BLOCK     34u         /* 2B FP16 scale + 32B int8 weights */

/* ── FP16 → FP32 decode ────────────────────────────────────────────────── */
static float fp16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    float f;
    if (exp == 0)
        f = (float)mant / 1024.0f * 5.960464478e-8f;
    else if (exp == 31)
        f = mant ? NAN : INFINITY;
    else {
        f = (float)mant / 1024.0f + 1.0f;
        f = ldexpf(f, (int)exp - 15);
    }
    return sign ? -f : f;
}

/* ── Mirror table: 8 octants ───────────────────────────────────────────── */
/* Mirror flip: slot → (KIS_AXIS - slot) % KIS_AXIS  (within one axis)     */
static const char *octant_names[] = {
    "(+X,+Y,+Z)", "(-X,+Y,+Z)", "(+X,-Y,+Z)", "(+X,+Y,-Z)",
    "(-X,-Y,+Z)", "(-X,+Y,-Z)", "(+X,-Y,-Z)", "(-X,-Y,-Z)"
};

/* ── Weight buffer: KIS slots × 3 axes ─────────────────────────────────── */
typedef struct {
    float values[KIS_TOTAL];    /* float-decoded weights per slot */
    int    valid[KIS_TOTAL];    /* 1 = slot has data */
    uint32_t count;             /* total slots populated */
} WeightGrid;

/* ── Read Q8_0 weights from GGUF, decode to float, fill grid ───────────── */
static int read_q8_grid(const char *gguf_path, WeightGrid *grid, int verbose) {
    GgufReader reader;
    if (gguf_open(gguf_path, &reader) != 0) {
        printf("  ERROR: Cannot open %s\n", gguf_path);
        return -1;
    }

    memset(grid, 0, sizeof(*grid));

    /* Find all Q8_0 tensors and fill the grid slot-by-slot */
    uint32_t slot = 0;
    int tensors_used = 0;

    for (uint32_t ti = 0; ti < reader.n_tensors && slot < KIS_TOTAL; ti++) {
        if (reader.sizes[ti] == 0 || reader.sizes[ti] % Q8_BLOCK != 0)
            continue;  /* skip non-Q8_0 tensors */

        uint32_t tensor_sz = reader.sizes[ti];
        uint8_t *buf = (uint8_t *)malloc(tensor_sz);
        if (!buf) continue;

        if (gguf_read_tensor(gguf_path, &reader, ti, buf, tensor_sz) != 0) {
            free(buf);
            continue;
        }

        uint32_t n_blocks = tensor_sz / Q8_BLOCK;
        if (verbose && tensors_used < 5) {
            printf("  Tensor[%2u] %-36s  Q8_0 blocks=%u  -> slots %u..%u\n",
                   ti, reader.names[ti] ? reader.names[ti] : "?",
                   n_blocks, slot, slot + (n_blocks * 32) - 1);
        }

        /* Decode Q8_0 blocks: 2B scale + 32 × int8 per block */
        for (uint32_t b = 0; b < n_blocks && slot < KIS_TOTAL; b++) {
            uint16_t raw_scale = *(uint16_t *)(buf + b * Q8_BLOCK);
            float scale = fp16_to_f32(raw_scale);

            for (int j = 0; j < 32 && slot < KIS_TOTAL; j++) {
                int8_t q = (int8_t)buf[b * Q8_BLOCK + 2 + j];
                grid->values[slot] = (float)q * scale;
                grid->valid[slot] = 1;
                slot++;
            }
        }

        free(buf);
        tensors_used++;
    }

    grid->count = slot;
    gguf_close(&reader);

    if (verbose) {
        printf("  Decoded %u Q8_0 tensors, filled %u / %u slots\n",
               tensors_used, grid->count, KIS_TOTAL);
    }
    return 0;
}

/* ── Mirror index: flip sign on one axis ────────────────────────────────── */
/* Axis 0 = slots [0, 6912), Axis 1 = [6912, 13824), Axis 2 = [13824, 20736) */
static uint32_t mirror_slot(uint32_t slot, int axis) {
    uint32_t base = (uint32_t)axis * KIS_AXIS;
    uint32_t local = slot - base;
    if (local >= KIS_AXIS) return slot;  /* out of axis range */
    uint32_t flipped = (KIS_AXIS - local) % KIS_AXIS;
    return base + flipped;
}

/* ── Check which octant a source slot belongs to ────────────────────────── */
static int slot_axis(uint32_t slot) {
    return (int)(slot / KIS_AXIS);
}

/* ── Main analysis ──────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <model.gguf>\n", argv[0]);
        return 1;
    }

    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  KIS Mirror Symmetry Analysis — 8-Octant Address Space\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  Model: %s\n\n", argv[1]);

    WeightGrid *grid = (WeightGrid *)calloc(1, sizeof(WeightGrid));
    if (!grid) { printf("ERROR: out of memory\n"); return 1; }

    if (read_q8_grid(argv[1], grid, 1) != 0) {
        free(grid);
        return 1;
    }

    printf("\n── Per-Axis Mirror Symmetry ──────────────────────────────────────\n");

    /* For each axis, check mirror symmetry */
    double axis_sym_pct[3] = {0};
    uint32_t axis_sym_count[3] = {0};
    uint32_t axis_total[3] = {0};

    for (int axis = 0; axis < 3; axis++) {
        uint32_t matched = 0;
        uint32_t total = 0;
        float sum_diff = 0, max_diff = 0;

        for (uint32_t slot = (uint32_t)axis * KIS_AXIS;
             slot < (uint32_t)(axis + 1) * KIS_AXIS; slot++) {
            if (!grid->valid[slot]) continue;

            uint32_t mirror = mirror_slot(slot, axis);
            if (mirror == slot) continue;  /* midpoint = always self-mirror */
            if (!grid->valid[mirror]) continue;

            float diff = fabsf(grid->values[slot] - grid->values[mirror]);
            sum_diff += diff;
            if (diff > max_diff) max_diff = diff;
            total++;

            if (diff < 1e-6f) matched++;
        }

        axis_sym_count[axis] = matched;
        axis_total[axis] = total;
        axis_sym_pct[axis] = total > 0 ? 100.0 * matched / total : 0;

        printf("  Axis %c: %u/%u slots mirror-matched (%.2f%% symmetric)\n",
               "XYZ"[axis], matched, total, axis_sym_pct[axis]);
        printf("         avg|Δ| = %.6f  max|Δ| = %.6f\n",
               total > 0 ? sum_diff / total : 0, max_diff);
    }

    printf("\n── Combined Mirror Symmetry (all 3 axes) ─────────────────────────\n");

    /* Count slots where ALL 3 axis mirrors match */
    uint32_t all3_match = 0, all3_total = 0;
    for (uint32_t slot = 0; slot < KIS_TOTAL; slot++) {
        if (!grid->valid[slot]) continue;
        int ax = slot_axis(slot);
        uint32_t mirror = mirror_slot(slot, ax);
        if (mirror == slot || !grid->valid[mirror]) continue;
        all3_total++;
        if (fabsf(grid->values[slot] - grid->values[mirror]) < 1e-6f)
            all3_match++;
    }
    double all3_pct = all3_total > 0 ? 100.0 * all3_match / all3_total : 0;
    printf("  All 3 axes mirror-matched: %u/%u (%.2f%%)\n",
           all3_match, all3_total, all3_pct);

    printf("\n── Octant Mirror Pairs ──────────────────────────────────────────\n");
    printf("  Octant pair     │ Axis │ Symmetry %%  │ Interpretation\n");
    printf("  ────────────────┼──────┼─────────────┼──────────────────────────────\n");

    /* 8 octants: test pair (i, i XOR (1<<axis)) for each axis */
    for (int axis = 0; axis < 3; axis++) {
        uint32_t matched = 0, total = 0;
        for (uint32_t slot = 0; slot < KIS_TOTAL; slot++) {
            if (!grid->valid[slot]) continue;
            uint32_t m = mirror_slot(slot, axis);
            if (m == slot || !grid->valid[m]) continue;
            total++;
            if (fabsf(grid->values[slot] - grid->values[m]) < 1e-6f)
                matched++;
        }
        double pct = total > 0 ? 100.0 * matched / total : 0;

        const char *interp;
        if (pct > 95.0)      interp = "STRONG — near-perfect mirror";
        else if (pct > 80.0) interp = "MODERATE — partial redundancy";
        else if (pct > 50.0) interp = "WEAK — some correlation";
        else                 interp = "NONE — no mirror redundancy";

        printf("  oct %d ↔ oct %d   │   %c   │ %6.2f%%    │ %s\n",
               0, 1 << axis, "XYZ"[axis], pct, interp);
    }

    printf("\n── Compression Ratio Estimates ──────────────────────────────────\n");

    double avg_sym = 0;
    int n_axes = 0;
    for (int a = 0; a < 3; a++) {
        if (axis_total[a] > 0) {
            avg_sym += axis_sym_pct[a];
            n_axes++;
        }
    }
    if (n_axes > 0) avg_sym /= n_axes;

    /* Theoretical: if all octants are identical, store 1 copy = 1/8 = 0.125
     * Real: depends on symmetry percentage.
     * Compression = original_size / (original_size × (1 - savings_pct))
     * savings_pct = symmetry × redundancy_factor
     *
     * Realistic model:
     *   - Each axis mirror gives ~sym% redundancy on that axis
     *   - But octant pairs share data only if axis mirrors match
     *   - Store canonical half (e.g., +X side), reconstruct -X via mirror formula */
    printf("  Average per-axis symmetry: %.2f%%\n", avg_sym);
    printf("\n  Realistic compression scenarios:\n");
    printf("    Perfect symmetry (100%%):  8.0x  (store 1 octant, formula = 8 views)\n");
    printf("    Strong (>95%%):           ~5-6x  (store canonical + small delta table)\n");
    printf("    Moderate (80%%):          ~3-4x  (store 2 octants + delta table)\n");
    printf("    Weak (50%%):              ~1.5x  (partial dedup + delta encoding)\n");
    printf("    None (0%%):               1.0x  (no mirror benefit)\n");
    printf("\n  → Measured average symmetry: %.2f%%\n", avg_sym);

    double est_ratio;
    if (avg_sym > 95.0)      est_ratio = 6.0;
    else if (avg_sym > 80.0) est_ratio = 3.5;
    else if (avg_sym > 50.0) est_ratio = 1.5;
    else                     est_ratio = 1.0;
    printf("  → Estimated realistic ratio: ~%.1fx\n", est_ratio);

    printf("\n═══════════════════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  (1) Symmetry:  X=%.2f%%  Y=%.2f%%  Z=%.2f%%  avg=%.2f%%\n",
           axis_sym_pct[0], axis_sym_pct[1], axis_sym_pct[2], avg_sym);
    printf("  (2) Realistic compression: ~%.1fx\n", est_ratio);
    printf("  (3) Octant mirror pairs:\n");
    for (int ax = 0; ax < 3; ax++) {
        int o0 = 0;
        int o1 = 1 << ax;
        double p = axis_total[ax] > 0 ? 100.0 * axis_sym_count[ax] / axis_total[ax] : 0;
        printf("       %s ↔ %s  (axis %c, %.1f%%)\n",
               octant_names[o0], octant_names[o1], "XYZ"[ax], p);
    }
    printf("═══════════════════════════════════════════════════════════════════\n");

    free(grid);
    return 0;
}
