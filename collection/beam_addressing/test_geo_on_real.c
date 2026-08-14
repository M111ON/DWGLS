/*
 * test_geo_on_real.c — Test Geometric Beam Codec on Real Q8_0 Weights
 * ═══════════════════════════════════════════════════════════════════
 *
 * Reads actual Q8_0 model weights, encodes with geometric beam codec,
 * decodes back, and compares integrity vs Q8_0.
 *
 * Compile:
 *   gcc -O2 -I. test_geo_on_real.c -o test_geo_on_real.exe
 *
 * Run:
 *   ./test_geo_on_real.exe
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
   METRICS
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t total_weights;
    uint64_t exact_match;      /* weight decoded perfectly */
    uint64_t off_by_one;       /* off by 1 Q12 unit */
    uint64_t off_by_more;      /* off by >1 */
    int32_t  max_error;        /* max |original - decoded| in Q12 */
    double   mse;              /* mean squared error in Q12 units */
    uint64_t sum_sq_error;
} GeoQuality;

static void geo_quality_init(GeoQuality *q) {
    memset(q, 0, sizeof(GeoQuality));
}

/* ══════════════════════════════════════════════════════════════
   TEST PARAMETERS
   ══════════════════════════════════════════════════════════════ */

/* Diameters to test (in Q12) */
static int32_t test_diameters[] = {
    FP_SCALE * 1,     /* D=1.0  */
    FP_SCALE * 2,     /* D=2.0  */
    FP_SCALE * 4,     /* D=4.0  */
    FP_SCALE * 8,     /* D=8.0  */
    FP_SCALE * 16,    /* D=16.0 */
    FP_SCALE * 32,    /* D=32.0 */
    FP_SCALE * 64,    /* D=64.0 */
    FP_SCALE * 128,   /* D=128.0 */
};
#define N_DIAMETERS (sizeof(test_diameters) / sizeof(test_diameters[0]))

/* ══════════════════════════════════════════════════════════════
   READ Q8_0 WEIGHTS FROM GGUF
   ══════════════════════════════════════════════════════════════ */

static int8_t *read_q8_weights(GGUF_File *gf, int tensor_idx, uint64_t *out_count)
{
    GGUF_Tensor *t = &gf->tensors[tensor_idx];

    uint64_t n_blocks = (t->n_weights + 31) / 32;
    uint64_t data_start = gf->tensor_data_start + t->offset;
    /* Align to 32 */
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
   COMPARE: Geometric Beam Codec vs Q8_0
   ══════════════════════════════════════════════════════════════ */

static void test_one_diameter(GeoQuality *q,
                               const int8_t *weights, uint64_t count,
                               int32_t diameter_q12, const char *label)
{
    GeoChain ch = geo_chain_init(diameter_q12, 65536);
    geo_quality_init(q);
    q->total_weights = count;

    for (uint64_t i = 0; i < count; i++) {
        int32_t w_q12 = (int32_t)weights[i] * FP_SCALE;  /* -128..127 → Q12 */
        int32_t packed = geo_chain_encode(&ch, w_q12);
        int32_t decoded = geo_chain_decode(&ch, packed);

        int32_t err = decoded - w_q12;
        if (err < 0) err = -err;

        if (err == 0) q->exact_match++;
        else if (err <= FP_SCALE) q->off_by_one++;
        else q->off_by_more++;

        if (err > q->max_error) q->max_error = err;

        q->sum_sq_error += (uint64_t)err * err;
    }

    q->mse = (double)q->sum_sq_error / (double)count;
}

static void print_quality(const char *name, GeoQuality *q, int32_t diameter_q12)
{
    double D = (double)diameter_q12 / FP_SCALE;
    double exact_pct = 100.0 * q->exact_match / q->total_weights;
    double off1_pct  = 100.0 * q->off_by_one / q->total_weights;
    double offN_pct  = 100.0 * q->off_by_more / q->total_weights;
    double rmse = sqrt(q->mse) / FP_SCALE;  /* in weight units (-128..127) */

    printf("  D=%-7.2f | exact=%-9llu (%-5.1f%%) off-by-1=%-9llu (%-5.1f%%) >1=%-9llu (%-5.1f%%) max_err=%-4lld rmse=%.6f  %s\n",
           D,
           (unsigned long long)q->exact_match, exact_pct,
           (unsigned long long)q->off_by_one, off1_pct,
           (unsigned long long)q->off_by_more, offN_pct,
           (unsigned long long)(q->max_error / FP_SCALE),
           rmse,
           name);
}

/* ══════════════════════════════════════════════════════════════
   Q8_0 BASELINE INFO
   ══════════════════════════════════════════════════════════════ */

static void print_q8_info(GGUF_File *gf, int tensor_idx)
{
    GGUF_Tensor *t = &gf->tensors[tensor_idx];
    uint64_t n_blocks = (t->n_weights + 31) / 32;
    uint64_t data_bytes = n_blocks * 34;  /* 2 scale + 32 weights per block */

    printf("  Tensor: %s\n", t->name);
    printf("  Dims: ");
    for (uint32_t d = 0; d < t->n_dims; d++)
        printf("%llu ", (unsigned long long)t->dims[d]);
    printf("\n");
    printf("  Type: Q8_0\n");
    printf("  Weights: %llu\n", (unsigned long long)t->n_weights);
    printf("  Blocks:  %llu\n", (unsigned long long)n_blocks);
    printf("  Q8_0 size: %llu bytes (%.2f MB)\n",
           (unsigned long long)data_bytes, data_bytes / (1024.0 * 1024.0));
    printf("\n");

    /* Read first block to show range */
    uint64_t data_start = gf->tensor_data_start + t->offset;
    data_start = (data_start + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)data_start, SEEK_SET);

    int min_w = 127, max_w = -128;
    int64_t sum = 0;
    int8_t buf[32];
    for (uint64_t b = 0; b < n_blocks && b < 10000; b++) {
        uint16_t sc;
        if (fread(&sc, 2, 1, gf->fp) != 1) break;
        if (fread(buf, 1, 32, gf->fp) != 32) break;
        for (int i = 0; i < 32; i++) {
            if (buf[i] < min_w) min_w = buf[i];
            if (buf[i] > max_w) max_w = buf[i];
            sum += buf[i];
        }
    }

    double mean = (double)sum / (n_blocks > 10000 ? 10000 * 32 : n_blocks * 32);
    printf("  Weight range: %d .. %d (mean=%.2f)\n", min_w, max_w, mean);
    printf("  Q8_0 encoding: scale(i16) + 32×int8 per block\n");
    printf("  Geometric codec: cell_idx(16) + normalized_within(16) per weight\n");
    printf("  Delta format: ~2 bytes/weight vs Q8_0 ~1.06 bytes/weight\n");
}

/* ══════════════════════════════════════════════════════════════
   COMPUTE DELTA SIZE (entropy estimate)
   ══════════════════════════════════════════════════════════════ */

static void compute_delta_entropy(const int8_t *weights, uint64_t count,
                                   int32_t diameter_q12)
{
    GeoChain ch = geo_chain_init(diameter_q12, 65536);

    uint64_t hist[65536] = {0};
    uint64_t total = 0;
    int32_t min_d = 999999, max_d = -999999;

    for (uint64_t i = 0; i < count && i < 500000; i++) {
        int32_t w_q12 = (int32_t)weights[i] * FP_SCALE;
        int32_t packed = geo_chain_encode(&ch, w_q12);
        uint32_t abs_packed = (uint32_t)((packed < 0) ? -packed : packed);
        uint32_t delta_part = abs_packed & 0xFFFF;

        hist[delta_part]++;
        total++;

        if ((int32_t)delta_part < min_d) min_d = (int32_t)delta_part;
        if ((int32_t)delta_part > max_d) max_d = (int32_t)delta_part;
    }

    /* Count distinct deltas and compute entropy */
    uint64_t distinct = 0;
    double entropy = 0;
    for (int i = 0; i < 65536; i++) {
        if (hist[i] > 0) {
            distinct++;
            double p = (double)hist[i] / (double)total;
            entropy -= p * log2(p);
        }
    }

    double D = (double)diameter_q12 / FP_SCALE;
    printf("  Delta range: %d .. %d (distinct=%llu/65536)\n",
           min_d, max_d, (unsigned long long)distinct);
    printf("  Delta entropy: %.4f bits (%.4f bytes per delta)\n",
           entropy, entropy / 8.0);
    printf("  Cell index entropy includes integer part of w/R\n");

    /* Compare with Q8_0 */
    double q8_entropy = 0;
    uint64_t q8_hist[256] = {0};
    for (uint64_t i = 0; i < count && i < 500000; i++) {
        uint8_t idx = (uint8_t)(weights[i]);
        q8_hist[idx]++;
    }
    for (int i = 0; i < 256; i++) {
        if (q8_hist[i] > 0) {
            double p = (double)q8_hist[i] / (double)(total > 0 ? total : 1);
            q8_entropy -= p * log2(p);
        }
    }
    printf("  Q8_0 entropy:  %.4f bits (from sample)\n", q8_entropy);
    printf("  Delta savings: %.1f%% vs Q8_0 raw (entropy basis)\n",
           100.0 * (1.0 - entropy / q8_entropy));
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    const char *model_path = argc > 1 ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Geometric Beam Codec vs Q8_0 — Real Model Test       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* Open model */
    printf("Opening: %s\n", model_path);
    GGUF_File *gf = gguf_open(model_path);
    if (!gf) {
        fprintf(stderr, "FAIL: cannot open GGUF file\n");
        return 1;
    }

    /* Find first Q8_0 tensor */
    int tensor_idx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) {
            tensor_idx = (int)i;
            break;
        }
    }
    if (tensor_idx < 0) {
        fprintf(stderr, "FAIL: no Q8_0 tensor found\n");
        gguf_close(gf);
        return 1;
    }

    /* Print tensor info */
    printf("\n─── Tensor ───\n");
    print_q8_info(gf, tensor_idx);

    /* Read weights */
    printf("─── Reading weights ───\n");
    uint64_t n_weights = 0;
    int8_t *weights = read_q8_weights(gf, tensor_idx, &n_weights);
    if (!weights || n_weights == 0) {
        fprintf(stderr, "FAIL: cannot read weights\n");
        gguf_close(gf);
        return 1;
    }
    printf("  Read %llu Q8_0 weights\n\n", (unsigned long long)n_weights);

    /* Test each diameter */
    printf("─── Geometric Codec Integrity ───\n");
    printf("  (exact = perfect reconstruction, off-by-1 = 1/4096 error,\n");
    printf("   >1 = larger error, max_err in Q8 units [-128..127])\n\n");

    GeoQuality best_q;
    double best_rmse = 1e9;
    int32_t best_D = 0;
    int best_idx = -1;

    for (int d = 0; d < (int)N_DIAMETERS; d++) {
        GeoQuality q;
        test_one_diameter(&q, weights, n_weights, test_diameters[d], "");
        print_quality("", &q, test_diameters[d]);

        double rmse = sqrt(q.mse) / FP_SCALE;
        if (rmse < best_rmse) {
            best_rmse = rmse;
            best_q = q;
            best_D = test_diameters[d];
            best_idx = d;
        }
    }

    printf("\n─── Best Diameter ───\n");
    printf("  D = %.2f (R = %.4f)\n",
           (double)best_D / FP_SCALE,
           (double)(best_D >> 1) / FP_SCALE);
    print_quality("BEST", &best_q, best_D);

    /* Delta entropy analysis */
    printf("\n─── Delta Entropy Analysis (D=best) ───\n");
    compute_delta_entropy(weights, n_weights, best_D);

    /* Size comparison */
    printf("\n─── Size Comparison ───\n");
    uint64_t n_blocks = (n_weights + 31) / 32;
    uint64_t q8_size = n_blocks * 34;
    uint64_t geo_raw_size = n_weights * 4;  /* 4 bytes per packed delta */
    uint64_t geo_if_2byte = n_weights * 2;  /* 2 bytes per delta (theoretical) */

    printf("  Q8_0:        %llu bytes (%.2f MB)  [2 scale + 32 w per block]\n",
           (unsigned long long)q8_size, q8_size / (1024.0 * 1024.0));
    printf("  Geo (4B):    %llu bytes (%.2f MB)  [4 bytes/delta]\n",
           (unsigned long long)geo_raw_size, geo_raw_size / (1024.0 * 1024.0));
    printf("  Geo (2B):    %llu bytes (%.2f MB)  [2 bytes/delta, theoretical]\n",
           (unsigned long long)geo_if_2byte, geo_if_2byte / (1024.0 * 1024.0));
    printf("  Q8_0 ratio:  %.4f bytes/weight\n",
           (double)q8_size / n_weights);
    printf("  Geo ratio:   %.4f bytes/weight (raw 4B)\n",
           (double)geo_raw_size / n_weights);

    /* Pipeline throughput estimate */
    printf("\n─── Throughput Estimate (from bench) ───\n");
    printf("  Encode: ~250M weights/sec\n");
    printf("  Decode: ~240M weights/sec\n");
    printf("  Q8_0 model %.0fM weights → ~%.0f ms encode\n",
           n_weights / 1e6,
           (double)n_weights / 250e6 * 1000);

    /* Cleanup */
    free(weights);
    gguf_close(gf);

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║   TEST COMPLETE                                         ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    return 0;
}
