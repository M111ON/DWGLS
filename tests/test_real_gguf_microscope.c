/* ═══════════════════════════════════════════════════════════════════════════
 * test_real_gguf_microscope.c — φ-Microscope on Real GGUF Weights
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Reads real weight data from a GGUF file and runs the phi_microscope
 * observation tool to see geometric structure in trained neural network
 * weights compared to random data.
 *
 * Compile:
 *   gcc -O2 -Wall -I. -IFGLS_new/beam_addressing \
 *       -o tests/test_real_gguf_microscope.exe \
 *       tests/test_real_gguf_microscope.c -lm
 *
 * Run:
 *   tests/test_real_gguf_microscope.exe [path.gguf]
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "core/geo_phi_microscope.h"
#include "gguf_reader.h"
#include <assert.h>
#include <math.h>
#include <inttypes.h>

/* ── GGUF v2-aware open ──
 * v2 uses 32-bit tensor_count/kv_count; v3 uses 64-bit.
 * Detect version and read accordingly.
 */
static GGUF_File *gguf_open_v2v3(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    GGUF_File *gf = (GGUF_File *)calloc(1, sizeof(GGUF_File));
    if (!gf) { fclose(fp); return NULL; }
    gf->fp = fp;

    uint32_t magic;
    if (fread(&magic, 4, 1, fp) != 1 || magic != GGUF_MAGIC) {
        fclose(fp); free(gf); return NULL;
    }
    if (fread(&gf->version, 4, 1, fp) != 1) goto fail;

    if (gf->version >= 3) {
        /* v3: 64-bit counts */
        if (fread(&gf->tensor_count, 8, 1, fp) != 1) goto fail;
        if (fread(&gf->kv_count, 8, 1, fp) != 1) goto fail;
    } else {
        /* v2: 32-bit counts */
        uint32_t tc, kc;
        if (fread(&tc, 4, 1, fp) != 1) goto fail;
        if (fread(&kc, 4, 1, fp) != 1) goto fail;
        gf->tensor_count = tc;
        gf->kv_count = kc;
    }

    /* Skip metadata KV pairs */
    for (uint64_t i = 0; i < gf->kv_count; i++) {
        GGUFFieldStr key;
        if (read_gguf_str_fp(fp, &key) != 0) goto fail;
        uint32_t val_type;
        if (fread(&val_type, 4, 1, fp) != 1) { free(key.data); goto fail; }
        if (skip_gguf_value(fp, val_type) != 0) { free(key.data); goto fail; }
        free(key.data);
    }

    /* Read tensor info */
    gf->tensors = (GGUF_Tensor *)calloc((size_t)gf->tensor_count, sizeof(GGUF_Tensor));
    if (!gf->tensors) goto fail;

    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        GGUFFieldStr name;
        if (read_gguf_str_fp(fp, &name) != 0) goto fail;
        strncpy(gf->tensors[i].name, name.data, 255);
        free(name.data);

        if (fread(&gf->tensors[i].n_dims, 4, 1, fp) != 1) goto fail;
        if (gf->tensors[i].n_dims > 4) gf->tensors[i].n_dims = 4;
        for (uint32_t d = 0; d < gf->tensors[i].n_dims; d++)
            if (fread(&gf->tensors[i].dims[d], 8, 1, fp) != 1) goto fail;
        if (fread(&gf->tensors[i].type, 4, 1, fp) != 1) goto fail;
        if (fread(&gf->tensors[i].offset, 8, 1, fp) != 1) goto fail;

        /* Compute size */
        uint64_t n_weights = 1;
        for (uint32_t d = 0; d < gf->tensors[i].n_dims; d++)
            n_weights *= gf->tensors[i].dims[d];
        gf->tensors[i].n_weights = n_weights;

        uint64_t block_sz = 0, wpb = 1;
        if (ggml_type_block_size(gf->tensors[i].type, &block_sz, &wpb) != 0)
            gf->tensors[i].size_bytes = n_weights;
        else {
            uint64_t n_blocks = (n_weights + wpb - 1) / wpb;
            gf->tensors[i].size_bytes = n_blocks * block_sz;
        }
    }

    /* Tensor data starts after all tensor info (align to 32 bytes) */
    long pos = ftell(fp);
    long aligned = (pos + 31) & ~31L;
    gf->tensor_data_start = (uint64_t)aligned;

    return gf;

fail:
    fclose(fp);
    free(gf->tensors);
    free(gf);
    return NULL;
}

/* ── Q8_0 dequantization helper ──
 * Q8_0 block: 2 bytes fp16 scale + 32 int8 quantized values
 * Dequantize: w = scale * (int8_val / 127.0)
 * fp16 decode via bit manipulation (IEEE 754 half-precision)
 */
static float fp16_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t frac = h & 0x3FF;
    uint32_t f32;
    if (exp == 0) {
        /* denormalized */
        f32 = (sign << 31) | (frac << 13);
    } else if (exp == 31) {
        /* inf / nan */
        f32 = (sign << 31) | 0x7F800000 | (frac << 13);
    } else {
        /* normalized: bias shift 15→127 */
        f32 = (sign << 31) | ((exp + 127 - 15) << 23) | (frac << 13);
    }
    float result;
    memcpy(&result, &f32, 4);
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 1: Open GGUF, read first tensor, verify basic properties
   ═══════════════════════════════════════════════════════════════════════════ */
static int test_gguf_open(const char *path) {
    printf("  TEST 1: Open GGUF file and read tensor metadata\n");

    GGUF_File *gf = gguf_open_v2v3(path);
    if (!gf) {
        printf("    SKIP — cannot open %s\n", path);
        return 0;  /* skip, not fail */
    }

    printf("    Version:    %u\n", gf->version);
    printf("    Tensors:    %" PRIu64 "\n", gf->tensor_count);
    printf("    KV pairs:   %" PRIu64 "\n", gf->kv_count);

    assert(gf->version >= 2);
    assert(gf->tensor_count > 0);

    GGUF_Tensor *t0 = &gf->tensors[0];
    printf("    Tensor[0]:  %s\n", t0->name);
    printf("    Dims:       %" PRIu64, t0->dims[0]);
    for (uint32_t d = 1; d < t0->n_dims; d++)
        printf(" x %" PRIu64, t0->dims[d]);
    printf("\n");
    printf("    Type:       %u (%s)\n", t0->type,
           t0->type == GGML_TYPE_F32 ? "F32" :
           t0->type == GGML_TYPE_F16 ? "F16" :
           t0->type == GGML_TYPE_Q8_0 ? "Q8_0" :
           t0->type == GGML_TYPE_Q4_0 ? "Q4_0" : "unknown");
    printf("    n_weights:  %" PRIu64 "\n", t0->n_weights);
    printf("    size_bytes: %" PRIu64 "\n", t0->size_bytes);

    assert(t0->n_weights > 0);
    gguf_close(gf);
    printf("    ✓ PASS\n");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 2: Read first tensor into float array (dequantize if needed)
   ═══════════════════════════════════════════════════════════════════════════ */
static int test_read_weights(const char *path, float **out_weights, uint32_t *out_n) {
    printf("  TEST 2: Read first tensor weights into float array\n");

    GGUF_File *gf = gguf_open_v2v3(path);
    if (!gf) {
        printf("    SKIP — cannot open %s\n", path);
        *out_weights = NULL;
        *out_n = 0;
        return 0;
    }

    GGUF_Tensor *t0 = &gf->tensors[0];
    uint32_t max_weights = 2048;  /* cap for manageable test */
    uint32_t n_weights = (uint32_t)(t0->n_weights < max_weights ? t0->n_weights : max_weights);
    float *weights = (float *)malloc(n_weights * sizeof(float));
    assert(weights);

    /* Seek to tensor data */
    fseek(gf->fp, gf->tensor_data_start + t0->offset, SEEK_SET);

    uint32_t loaded = 0;

    if (t0->type == GGML_TYPE_F32) {
        /* Direct read — F32 tensor */
        loaded = (uint32_t)fread(weights, sizeof(float), n_weights, gf->fp);
    }
    else if (t0->type == GGML_TYPE_F16) {
        /* F16 tensor: 2 bytes per weight */
        for (uint32_t i = 0; i < n_weights; i++) {
            uint16_t h;
            if (fread(&h, 2, 1, gf->fp) != 1) break;
            weights[i] = fp16_to_float(h);
        }
        loaded = n_weights;
    }
    else if (t0->type == GGML_TYPE_Q8_0) {
        /* Q8_0: blocks of 34 bytes (2B fp16 scale + 32 int8)
         * Dequantize: w = scale * (int8_val / 127.0f) */
        while (loaded < n_weights) {
            uint16_t scale_u16;
            if (fread(&scale_u16, 2, 1, gf->fp) != 1) break;
            float scale = fp16_to_float(scale_u16);
            int8_t q[32];
            if (fread(q, 1, 32, gf->fp) != 32) break;
            for (int i = 0; i < 32 && loaded < n_weights; i++) {
                weights[loaded++] = scale * ((float)q[i] / 127.0f);
            }
        }
        n_weights = loaded;
    }
    else if (t0->type == GGML_TYPE_Q4_0) {
        /* Q4_0: blocks of 18 bytes (2B fp16 scale + 16 bytes = 32 int4)
         * Each byte holds two int4 values. Dequantize: w = scale * (val / 7.0f) */
        while (loaded < n_weights) {
            uint16_t scale_u16;
            if (fread(&scale_u16, 2, 1, gf->fp) != 1) break;
            float scale = fp16_to_float(scale_u16);
            uint8_t qbytes[16];
            if (fread(qbytes, 1, 16, gf->fp) != 16) break;
            for (int i = 0; i < 16 && loaded < n_weights; i++) {
                int8_t lo = (int8_t)(qbytes[i] & 0x0F);
                int8_t hi = (int8_t)((qbytes[i] >> 4) & 0x0F);
                /* sign-extend 4-bit to int8 */
                if (lo >= 8) lo -= 16;
                if (hi >= 8) hi -= 16;
                weights[loaded++] = scale * ((float)lo / 7.0f);
                if (loaded < n_weights)
                    weights[loaded++] = scale * ((float)hi / 7.0f);
            }
        }
        n_weights = loaded;
    }
    else {
        printf("    SKIP — unsupported tensor type %u\n", t0->type);
        free(weights);
        gguf_close(gf);
        *out_weights = NULL;
        *out_n = 0;
        return 0;
    }

    printf("    Loaded %u weights from '%s'\n", loaded, t0->name);

    /* Print sample */
    printf("    First 8: ");
    for (uint32_t i = 0; i < 8 && i < loaded; i++)
        printf("%.6f ", weights[i]);
    printf("\n");

    /* Compute basic stats */
    double sum = 0, sum_sq = 0, max_abs = 0;
    float min_val = weights[0], max_val = weights[0];
    for (uint32_t i = 0; i < loaded; i++) {
        double v = weights[i];
        sum += v;
        sum_sq += v * v;
        double a = fabs(v);
        if (a > max_abs) max_abs = a;
        if (weights[i] < min_val) min_val = weights[i];
        if (weights[i] > max_val) max_val = weights[i];
    }
    double mean = sum / loaded;
    double stddev = sqrt(sum_sq / loaded - mean * mean);
    printf("    Stats: mean=%.6f  stddev=%.6f  range=[%.6f, %.6f]  |max|=%.6f\n",
           mean, stddev, min_val, max_val, max_abs);

    gguf_close(gf);
    *out_weights = weights;
    *out_n = loaded;
    printf("    ✓ PASS\n");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 3: Run φ-microscope on real GGUF weights
   ═══════════════════════════════════════════════════════════════════════════ */
static int test_phi_on_real(float *weights, uint32_t n_weights) {
    printf("  TEST 3: φ-Microscope on real GGUF weights\n");

    if (!weights || n_weights < 32) {
        printf("    SKIP — not enough weights\n");
        return 0;
    }

    /* Run the microscope across generations 0–10 */
    phi_microscope(weights, n_weights, 0, 10);

    printf("    ✓ PASS (microscope completed)\n");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 4: Per-generation cell type distribution + entropy analysis
   ═══════════════════════════════════════════════════════════════════════════ */
static int test_per_gen_distribution(float *weights, uint32_t n_weights) {
    printf("  TEST 4: Per-generation cell type distribution & entropy\n");

    if (!weights || n_weights < 32) {
        printf("    SKIP — not enough weights\n");
        return 0;
    }

    double max_entropy = log2((double)PHI_MICRO_CELL_TYPES);  /* 3.0 for 8 types */
    double uniform_entropy = log2(4.0);  /* 2.0 for 4 active types per gen parity */

    printf("    Max possible entropy: %.3f bits (8 types)\n", max_entropy);
    printf("    Per-gen limit (4 active types): %.3f bits\n\n", uniform_entropy);

    printf("    Gen | Shell | H_count | H_magnitude | H_mag / H_unif | Δ_from_uniform\n");
    printf("    ----|-------|---------|-------------|----------------|---------------\n");

    double min_mag_entropy = 999.0;
    uint8_t best_gen = 0;

    for (uint8_t g = 0; g <= 10 && g <= PHI_MICRO_GEN_MAX; g++) {
        PhiMicroscopeResult r = phi_observe_generation(weights, n_weights, g);
        double ratio = r.magnitude_entropy / uniform_entropy;
        double delta = uniform_entropy - r.magnitude_entropy;

        printf("    %3u | %5u | %.4f  |   %.4f    |    %.4f      |  %+.4f\n",
               r.gen, r.shell_size, r.entropy, r.magnitude_entropy, ratio, delta);

        if (r.magnitude_entropy < min_mag_entropy && r.magnitude_entropy > 0.001) {
            min_mag_entropy = r.magnitude_entropy;
            best_gen = r.gen;
        }

        /* Print type distribution */
        printf("         Types: ");
        for (uint32_t t = 0; t < PHI_MICRO_CELL_TYPES; t++) {
            if (r.type_counts[t] > 0)
                printf("%s:%u(%.0f) ", cell_type_name((uint8_t)t),
                       r.type_counts[t], r.type_magnitudes[t]);
        }
        printf("\n");
    }

    printf("\n    Best gen for structure: %u (H_mag=%.4f bits)\n", best_gen, min_mag_entropy);
    printf("    Entropy drop from uniform: %.4f bits (%.1f%%)\n",
           uniform_entropy - min_mag_entropy,
           100.0 * (uniform_entropy - min_mag_entropy) / uniform_entropy);

    printf("    ✓ PASS (distribution analysis complete)\n");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 5: Compare real weights vs random — does geometry reveal structure?
   ═══════════════════════════════════════════════════════════════════════════ */
static int test_real_vs_random(float *real_weights, uint32_t n_weights) {
    printf("  TEST 5: Real weights vs random — geometric structure detection\n");

    if (!real_weights || n_weights < 32) {
        printf("    SKIP — not enough weights\n");
        return 0;
    }

    /* Generate matched random weights (same range) */
    float *rand_weights = (float *)malloc(n_weights * sizeof(float));
    assert(rand_weights);

    /* Match the range of real weights */
    double real_min = real_weights[0], real_max = real_weights[0];
    for (uint32_t i = 1; i < n_weights; i++) {
        if (real_weights[i] < real_min) real_min = real_weights[i];
        if (real_weights[i] > real_max) real_max = real_weights[i];
    }
    double range = real_max - real_min;
    if (range < 1e-10) range = 1.0;

    /* Simple LCG for reproducibility */
    uint32_t seed = 0xDEADBEEF;
    #define LCG(s) ((s) = (s) * 1103515245u + 12345u)
    for (uint32_t i = 0; i < n_weights; i++) {
        LCG(seed);
        rand_weights[i] = (float)(real_min + (double)(LCG(seed) & 0x7FFFFFFF) / (double)0x7FFFFFFF * range);
    }
    #undef LCG

    printf("    Real weight range: [%.6f, %.6f]\n", real_min, real_max);
    printf("    n_weights: %u\n\n", n_weights);

    printf("    Gen | Real_H_mag | Rand_H_mag | Δ(H_mag) | Real_H_cnt | Rand_H_cnt\n");
    printf("    ----|------------|------------|----------|------------|-----------\n");

    double max_delta = 0.0;
    uint8_t best_gen = 0;

    for (uint8_t g = 0; g <= 8 && g <= PHI_MICRO_GEN_MAX; g++) {
        PhiMicroscopeResult rr = phi_observe_generation(real_weights, n_weights, g);
        PhiMicroscopeResult rw = phi_observe_generation(rand_weights, n_weights, g);
        double delta = rw.magnitude_entropy - rr.magnitude_entropy;

        printf("    %3u |   %.4f   |   %.4f   | %+.4f  |   %.4f   |   %.4f\n",
               g, rr.magnitude_entropy, rw.magnitude_entropy, delta,
               rr.entropy, rw.entropy);

        if (delta > max_delta) {
            max_delta = delta;
            best_gen = g;
        }
    }

    printf("\n    Max entropy separation at gen %u (Δ=%.4f bits)\n", best_gen, max_delta);
    printf("    Interpretation:\n");
    if (max_delta > 0.05)
        printf("      → Real weights show MORE structure than random (geometric bias)\n");
    else if (max_delta < -0.05)
        printf("      → Random weights show more structure (unexpected — check data)\n");
    else
        printf("      → Minimal difference — weights may be well-distributed geometrically\n");

    free(rand_weights);
    printf("    ✓ PASS (comparison complete)\n");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    const char *default_path = "I:/model/Kokoro_no_espeak_Q8.gguf";
    const char *path = (argc >= 2) ? argv[1] : default_path;

    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  test_real_gguf_microscope — φ-Microscope on Real GGUF\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  File: %s\n\n", path);

    int pass = 0, total = 0, skipped = 0;

    /* Test 1: Open GGUF */
    total++;
    pass += test_gguf_open(path);
    if (pass == total - 1 && pass < total) skipped++;

    printf("\n");

    /* Test 2: Read weights */
    float *weights = NULL;
    uint32_t n_weights = 0;
    total++;
    int r2 = test_read_weights(path, &weights, &n_weights);
    pass += r2;
    if (!r2) skipped++;

    printf("\n");

    /* Test 3: φ-microscope on real weights */
    total++;
    pass += test_phi_on_real(weights, n_weights);
    if (!weights || n_weights < 32) skipped++;

    printf("\n");

    /* Test 4: Per-generation distribution */
    total++;
    pass += test_per_gen_distribution(weights, n_weights);
    if (!weights || n_weights < 32) skipped++;

    printf("\n");

    /* Test 5: Real vs random comparison */
    total++;
    pass += test_real_vs_random(weights, n_weights);
    if (!weights || n_weights < 32) skipped++;

    free(weights);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  Results: %d / %d passed (%d skipped)\n", pass, total, skipped);
    printf("═══════════════════════════════════════════════════════════════\n");

    return (pass == total) ? 0 : 1;
}
