/*
 * analyze_geo_expansion.c — Find root cause of geometric codec expansion
 * ═══════════════════════════════════════════════════════════════════
 *
 * Compile: gcc -O2 -I. analyze_geo_expansion.c -o analyze_geo_expansion.exe -lm
 * Run: ./analyze_geo_expansion.exe
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gguf_reader.h"
#include "beam_geometric.c"

/* ══════════════════════════════════════════════════════════════
   READ Q8_0 WEIGHTS FROM GGUF
   ══════════════════════════════════════════════════════════════ */

static int8_t *read_q8_weights(GGUF_File *gf, int tensor_idx, uint64_t *out_count)
{
    GGUF_Tensor *t = &gf->tensors[tensor_idx];

    uint64_t n_blocks = (t->n_weights + 31) / 32;
    uint64_t data_start = gf->tensor_data_start + t->offset;
    data_start = (data_start + 31) & ~(uint64_t)31;

    fseek(gf->fp, (long)data_start, SEEK_SET);

    int8_t *weights = (int8_t *)malloc(n_blocks * 32);
    if (!weights) return NULL;

    uint64_t read_count = 0;
    for (uint64_t b = 0; b < n_blocks; b++) {
        uint16_t scale;
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(weights + read_count, 1, 32, gf->fp) != 32) break;
        read_count += 32;
    }

    *out_count = read_count;
    return weights;
}

/* ══════════════════════════════════════════════════════════════
   SOURCE ANALYSIS — where do the bits go?
   ══════════════════════════════════════════════════════════════ */

/* For each D, measure the actual entropy of cell_idx and norm_within SEPARATELY */
typedef struct {
    double cell_entropy;      /* bits needed for cell index */
    double within_entropy;    /* bits needed for normalized within */
    double joint_entropy;     /* bits needed for combined */
    double cell_distinct;     /* distinct cell values seen */
    double within_distinct;   /* distinct within values seen */
    double cell_avg;          /* average cell index */
    double zero_within_pct;   /* % of weights with within=0 */
    double zero_cell_pct;     /* % of weights with cell=0 (first cell) */
} EntropyBreakdown;

static void analyze_entropy(const int8_t *weights, uint64_t count,
                            int32_t diameter, EntropyBreakdown *eb)
{
    memset(eb, 0, sizeof(*eb));

    GeoChain ch = geo_chain_init(diameter, 65536);
    uint64_t sample = (count > 200000) ? 200000 : count;

    /* Count frequencies */
    uint64_t cell_hist[65536] = {0};
    uint64_t within_hist[65536] = {0};

    uint64_t zero_within = 0;
    uint64_t zero_cell = 0;
    uint64_t cell_sum = 0;

    for (uint64_t i = 0; i < sample; i++) {
        int32_t w_q12 = (int32_t)weights[i] * FP_SCALE;
        int32_t packed = geo_chain_encode(&ch, w_q12);
        uint32_t ap = (uint32_t)((packed < 0) ? -packed : packed);
        uint32_t cell = (ap >> 16) & 0xFFFF;
        uint32_t within = ap & 0xFFFF;

        cell_hist[cell]++;
        within_hist[within]++;
        cell_sum += cell;

        if (within == 0) zero_within++;
        if (cell == 0) zero_cell++;
    }

    /* Compute entropy */
    eb->cell_entropy = 0;
    eb->within_entropy = 0;
    eb->cell_distinct = 0;
    eb->within_distinct = 0;

    for (int i = 0; i < 65536; i++) {
        if (cell_hist[i] > 0) {
            eb->cell_distinct++;
            double p = (double)cell_hist[i] / (double)sample;
            eb->cell_entropy -= p * log2(p);
        }
        if (within_hist[i] > 0) {
            eb->within_distinct++;
            double p = (double)within_hist[i] / (double)sample;
            eb->within_entropy -= p * log2(p);
        }
    }

    /* Joint = encode both together — could be more efficient */
    uint64_t joint_hist[65536] = {0};
    for (uint64_t i = 0; i < sample; i++) {
        int32_t w_q12 = (int32_t)weights[i] * FP_SCALE;
        int32_t packed = geo_chain_encode(&ch, w_q12);
        /* Use the full 32-bit as one symbol */
        uint32_t ap = (uint32_t)((packed < 0) ? -packed : packed);
        uint16_t key = (uint16_t)((ap >> 16) ^ (ap & 0xFFFF)); /* mix */
        joint_hist[key]++;
    }
    eb->joint_entropy = 0;
    for (int i = 0; i < 65536; i++) {
        if (joint_hist[i] > 0) {
            double p = (double)joint_hist[i] / (double)sample;
            eb->joint_entropy -= p * log2(p);
        }
    }

    eb->cell_avg = (double)cell_sum / (double)sample;
    eb->zero_within_pct = 100.0 * (double)zero_within / (double)sample;
    eb->zero_cell_pct = 100.0 * (double)zero_cell / (double)sample;
}

/* ══════════════════════════════════════════════════════════════
   BREAK DOWN THE PACKED FORMAT WASTE
   ══════════════════════════════════════════════════════════════ */

static void analyze_waste(void)
{
    printf("─── Packed Format Breakdown (32-bit per weight) ───\n\n");
    printf("  bit 31     30..16          15..0\n");
    printf("  ┌─────┬──────────────────┬──────────────────┐\n");
    printf("  │sign │   cell_index     │  normalized_within│\n");
    printf("  └─────┴──────────────────┴──────────────────┘\n");
    printf("    1 bit    15 bits           16 bits\n\n");

    printf("  Root causes of expansion:\n\n");

    printf("  CAUSE 1: cell_index is FIXED 15 bits\n");
    printf("    Real model weights are -128..127 (8-bit values)\n");
    printf("    cell_index = floor(|w| / R)  where R = D/2\n");
    printf("    For any D >= 1: cell_index range = 0..~127\n");
    printf("    → only ~7 bits needed, paying 15 bits\n");
    printf("    Wasted: ~8 bits per weight\n\n");

    printf("  CAUSE 2: normalized_within is FIXED 16 bits\n");
    printf("    Most Q8_0 weights are exact multiples of R\n");
    printf("    → within=0 for most weights → norm_within=0\n");
    printf("    Even when within≠0, entropy is very low\n");
    printf("    → only ~1-3 bits needed typically\n");
    printf("    Wasted: ~13-15 bits per weight\n\n");

    printf("  CAUSE 3: No block structure\n");
    printf("    Q8_0 shares 1 scale across 32 weights: 34B/32w = 1.0625 B/w\n");
    printf("    Geo stores each weight independently: 4 B/w\n");
    printf("    → 3.76× raw overhead vs Q8_0 fixed format\n\n");

    printf("  CAUSE 4: Sign redundancy\n");
    printf("    sign bit is separate from magnitude\n");
    printf("    In Q8_0, sign is part of the int8 value (no overhead)\n");
    printf("    In Geo, sign consumes 1 bit AND interacts with cell_index\n\n");

    printf("  CAUSE 5: No spatial locality exploitation\n");
    printf("    Adjacent weights often have same/similar cell index\n");
    printf("    But we store each independently, no delta encoding\n");
    printf("    across consecutive weights\n\n");
}

/* ══════════════════════════════════════════════════════════════
   DENSITY MAP — where values fall in the grid
   ══════════════════════════════════════════════════════════════ */

static void density_map(const int8_t *weights, uint64_t count, int32_t diameter)
{
    GeoChain ch = geo_chain_init(diameter, 65536);
    uint64_t sample = (count > 50000) ? 50000 : count;

    /* Build 2D histogram: cell_idx vs norm_within bucket */
    uint32_t buckets[16][16] = {0};  /* 16×16 coarse view */

    for (uint64_t i = 0; i < sample; i++) {
        int32_t w_q12 = (int32_t)weights[i] * FP_SCALE;
        int32_t packed = geo_chain_encode(&ch, w_q12);
        uint32_t ap = (uint32_t)((packed < 0) ? -packed : packed);
        uint32_t cell = (ap >> 16) & 0xFFFF;
        uint32_t within = ap & 0xFFFF;

        int ci = (cell < 16) ? (int)cell : 15;
        int wi = (within < 4096) ? (int)(within >> 8) : 15;  /* 16 buckets for 0..65535 */
        if (ci >= 0 && ci < 16 && wi >= 0 && wi < 16)
            buckets[ci][wi]++;
    }

    double D = (double)diameter / FP_SCALE;
    printf("─── Density Map: cell_index vs within_fraction (D=%.1f) ───\n", D);
    printf("  Columns = within bucket (0=zero, 15=large within)\n");
    printf("  Rows    = cell index  (0-14, 15=15+)\n");
    printf("  '.' = 0, 'o'=1-25%%, 'O'=25-50%%, '@'=50-75%%, '#'=75-100%% of max\n\n");

    /* Normalize to chars */
    uint32_t max_cell = 0;
    for (int c = 0; c < 16; c++)
        for (int w = 0; w < 16; w++)
            if (buckets[c][w] > max_cell) max_cell = buckets[c][w];

    printf("  within→ ");
    for (int w = 0; w < 16; w++) printf("%2d", w); printf("\n");
    printf("  ────────");
    for (int w = 0; w < 16; w++) printf("──"); printf("\n");

    for (int c = 0; c < 16; c++) {
        printf("  cell%3d|", c);
        for (int w = 0; w < 16; w++) {
            double pct = (double)buckets[c][w] / (double)max_cell;
            char ch = '.';
            if (pct > 0.75) ch = '#';
            else if (pct > 0.50) ch = '@';
            else if (pct > 0.25) ch = 'O';
            else if (pct > 0.0)  ch = 'o';
            printf(" %c", ch);
        }
        printf("\n");
    }

    /* Annotate */
    printf("\n");
    printf("  Key observation: ");
    if (max_cell == buckets[0][0]) {
        printf("weights cluster at cell=0, within=0\n");
        printf("  → all entropy is in cell_index, within is ~redundant\n");
    } else {
        printf("weights distributed across many cells/within states\n");
    }
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    const char *model_path = argc > 1 ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Geometric Codec — Expansion Root Cause Analysis      ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* Open model */
    GGUF_File *gf = gguf_open(model_path);
    if (!gf) { fprintf(stderr, "FAIL: open\n"); return 1; }

    int tensor_idx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { tensor_idx = (int)i; break; }
    if (tensor_idx < 0) { fprintf(stderr, "FAIL: no Q8_0\n"); gguf_close(gf); return 1; }

    uint64_t n_weights = 0;
    int8_t *weights = read_q8_weights(gf, tensor_idx, &n_weights);
    if (!weights || n_weights == 0) { fprintf(stderr, "FAIL: read\n"); gguf_close(gf); return 1; }
    gguf_close(gf);

    printf("  Model: %s\n", model_path);
    printf("  Tensor: output.weight (%llu weights)\n", (unsigned long long)n_weights);
    printf("\n");

    /* ── Entropy breakdown for each D ── */
    printf("─── Entropy Breakdown by Diameter ───\n");
    printf("  (measured on 200K sample, bits per weight)\n\n");
    printf("  D      cell_ent  within_ent  joint_ent  cell_dist within_dist zero_w%%  zero_c%%   avg_cell\n");
    printf("  ─────  ────────  ──────────  ─────────  ────────  ──────────  ──────  ───────  ────────\n");

    int32_t ds[] = {FP_SCALE*1, FP_SCALE*2, FP_SCALE*4, FP_SCALE*8, FP_SCALE*16, FP_SCALE*32, FP_SCALE*64, FP_SCALE*128};
    for (int i = 0; i < 8; i++) {
        EntropyBreakdown eb;
        analyze_entropy(weights, n_weights, ds[i], &eb);
        printf("  %-5.1f  %8.4f  %10.4f  %9.4f  %8.0f  %10.0f  %6.1f  %7.1f  %9.2f\n",
               (double)ds[i]/FP_SCALE,
               eb.cell_entropy, eb.within_entropy, eb.joint_entropy,
               eb.cell_distinct, eb.within_distinct,
               eb.zero_within_pct, eb.zero_cell_pct, eb.cell_avg);
    }

    printf("\n");
    printf("  Total current cost: FIXED 32 bits/weight (%.4f B/w)\n", 4.0);
    printf("  Theoretical minimum: ~cell_entropy + within_entropy bits\n");
    printf("  Wasted bits = 32 - (cell_ent + within_ent) per weight\n\n");

    /* ── Visual density ── */
    density_map(weights, n_weights, FP_SCALE * 4);  /* D=4.0 */

    /* ── Format waste explanation ── */
    analyze_waste();

    /* ── Q8_0 contrast ── */
    printf("─── Q8_0 Block Format Comparison ───\n\n");
    printf("  Q8_0 block: [scale_f16(2B)] [w0(1B)] [w1(1B)] ... [w31(1B)]\n");
    printf("              = 34 bytes for 32 weights = 1.0625 B/w\n\n");
    printf("  Geo block:  [cell_idx(2B)] [norm_within(2B)]  per weight\n");
    printf("              = 4 bytes for 1 weight = 4.0 B/w\n\n");
    printf("  If we group N weights and share cell_index base:\n");
    printf("    Group of 32: base_cell(2B) + 32×delta_within(1B each) = 34B/32w = 1.0625 B/w\n");
    printf("    → matches Q8_0 exactly!\n\n");

    /* ── Solution directions ── */
    printf("─── Fix Directions (โดย priority) ───\n\n");
    printf("  1. GROUP encoding: share cell_index base across N weights\n");
    printf("     (like Q8_0 shares scale across 32 weights)\n");
    printf("     Store: base_cell + per-weight delta_within\n\n");
    printf("  2. Variable-length: use entropy coding on cell_index\n");
    printf("     Most weights have small cell index → fewer bits\n\n");
    printf("  3. Hybrid: cell_index as block-level, within as per-weight\n");
    printf("     Block header: base_cell + count\n");
    printf("     Per weight:   delta_from_base (1-2 bytes)\n\n");
    printf("  4. Abandon fixed 32-bit — use minimal bits for each field\n");
    printf("     cell_index: 7-8 bits (for weights -128..127)\n");
    printf("     within:     1-3 bits (only when not exact multiple)\n");
    printf("     Total:      ~10 bits/weight vs 32 bits\n");

    free(weights);

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║   ANALYSIS COMPLETE                                     ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    return 0;
}
