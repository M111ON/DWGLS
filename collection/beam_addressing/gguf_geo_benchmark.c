/*
 * gguf_geo_benchmark.c — Real GGUF inference: Array vs Engine vs Hash Table
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * เทียบ 3 วิธีเข้าถึง weight:
 *   1. Array:   direct array[i] access (baseline)
 *   2. Engine:  geometric capo-based access (our approach)
 *   3. Hash:    hash table lookup (traditional)
 *
 * Build: gcc -O2 -std=c11 -D_GNU_SOURCE beam_addressing/gguf_geo_benchmark.c \
 *        -o beam_addressing/gguf_geo_benchmark.exe -lm
 * Usage: gguf_geo_benchmark.exe model.gguf [layer_idx]
 *
 * Example: gguf_geo_benchmark.exe I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf 0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <errno.h>

#ifdef _MSC_VER
typedef unsigned __int128 uint128_t;
#else
typedef unsigned __int128 uint128_t;
#endif

#include "gguf_reader.h"

/* ═══════════════════════════════════════════════════════════════════ */
/*  Helpers                                                           */
/* ═══════════════════════════════════════════════════════════════════ */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static float fp16_to_fp32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t h_exp  = (h >> 10) & 0x1f;
    uint32_t h_mant = h & 0x3ff;
    uint32_t f32_exp, f32_mant;

    if (h_exp == 0) {
        if (h_mant == 0) return 0.0f;
        int shift = 0;
        while (!(h_mant & 0x400)) { h_mant <<= 1; shift++; }
        f32_exp  = 113u - (uint32_t)shift;
        f32_mant = (h_mant & 0x3ff) << 13;
    } else if (h_exp == 31) {
        f32_exp  = 0xff;
        f32_mant = h_mant << 13;
    } else {
        f32_exp  = h_exp + 112u;
        f32_mant = h_mant << 13;
    }

    uint32_t bits = sign | (f32_exp << 23) | f32_mant;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

/* Read Q8_0 tensor into int8 weights + float scales */
static int gguf_read_q8_tensor(GGUF_File *gf, uint64_t idx,
                                int8_t **out_weights, float **out_scales,
                                uint64_t *out_count)
{
    if (idx >= gf->tensor_count) return -1;
    GGUF_Tensor *t = &gf->tensors[idx];
    if (t->type != GGML_TYPE_Q8_0) return -2;

    uint64_t n_weights = t->n_weights;
    uint64_t n_blocks = (n_weights + 31) / 32;
    uint64_t n_bytes = n_blocks * 34;

    uint8_t *raw = (uint8_t*)malloc((size_t)n_bytes);
    if (!raw) return -3;

    uint64_t file_offset = gf->tensor_data_start + t->offset;
    if (fseeko(gf->fp, (off_t)file_offset, SEEK_SET) != 0) {
        free(raw); return -4;
    }
    if (fread(raw, 1, (size_t)n_bytes, gf->fp) != (size_t)n_bytes) {
        free(raw); return -5;
    }

    int8_t *weights = (int8_t*)malloc((size_t)n_weights);
    float *scales = (float*)malloc((size_t)n_weights * sizeof(float));
    if (!weights || !scales) {
        free(raw); free(weights); free(scales); return -6;
    }

    for (uint64_t b = 0; b < n_blocks; b++) {
        uint64_t boff = b * 34;
        uint16_t scale_raw;
        memcpy(&scale_raw, raw + boff, 2);
        if ((scale_raw & 0x7c00) == 0x7c00) scale_raw = 0;
        float scale = fp16_to_fp32(scale_raw);

        uint64_t dst_off = b * 32;
        uint64_t copy = (n_weights - dst_off > 32) ? 32 : (n_weights - dst_off);
        for (uint64_t j = 0; j < copy; j++) {
            weights[dst_off + j] = (int8_t)raw[boff + 2 + j];
            scales[dst_off + j] = scale;
        }
    }

    free(raw);
    *out_weights = weights;
    *out_scales = scales;
    *out_count = n_weights;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  1. ARRAY ACCESS (baseline)                                        */
/* ═══════════════════════════════════════════════════════════════════ */

static inline float array_get_weight(int idx, const int8_t *w, const float *s) {
    return (float)w[idx] * s[idx];
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  2. ENGINE ACCESS (geometric capo)                                 */
/* ═══════════════════════════════════════════════════════════════════ */

#define CAPO_SIZE    65536u
#define CAPO_DIM     256u
#define STRIDE       37u

typedef struct {
    uint64_t n_weights;
    uint16_t capo_count;
    uint8_t  *y_pos;    /* y_pos[i] for each weight */
} EngineMap;

static EngineMap *engine_build(const int8_t *weights, uint64_t count) {
    EngineMap *m = (EngineMap*)calloc(1, sizeof(EngineMap));
    if (!m) return NULL;
    m->n_weights = count;
    m->capo_count = (uint16_t)((count + CAPO_SIZE - 1) / CAPO_SIZE);
    if (m->capo_count == 0) m->capo_count = 1;
    m->y_pos = (uint8_t*)calloc((size_t)count, 1);
    if (!m->y_pos) { free(m); return NULL; }

    /* Track used positions per capo */
    uint8_t **used = (uint8_t**)calloc(m->capo_count, sizeof(uint8_t*));
    for (uint16_t c = 0; c < m->capo_count; c++)
        used[c] = (uint8_t*)calloc(CAPO_SIZE, 1);

    for (uint64_t i = 0; i < count; i++) {
        uint16_t capo = (uint16_t)(i / CAPO_SIZE);
        if (capo >= m->capo_count) capo = (uint16_t)(m->capo_count - 1);
        uint8_t d = (uint8_t)((int32_t)weights[i] + 128);
        uint8_t x = (uint8_t)((i * STRIDE) % CAPO_DIM);
        uint8_t y = (uint8_t)(x ^ d);
        uint32_t attempts = 0;
        while (attempts < CAPO_DIM) {
            uint32_t idx = (uint32_t)y * CAPO_DIM + x;
            if (idx < CAPO_SIZE && !used[capo][idx]) {
                used[capo][idx] = 1;
                break;
            }
            y = (uint8_t)(y + 1);
            attempts++;
        }
        m->y_pos[i] = y;
    }

    for (uint16_t c = 0; c < m->capo_count; c++) free(used[c]);
    free(used);
    return m;
}

static inline float engine_get_weight(uint64_t i, const EngineMap *m,
                                       const int8_t *w, const float *s) {
    uint8_t x = (uint8_t)((i * STRIDE) % CAPO_DIM);
    uint8_t y = m->y_pos[i];
    int decoded = (int)((uint8_t)(x ^ y)) - 128;
    return (float)decoded * s[i];
}

static void engine_free(EngineMap *m) {
    if (!m) return;
    free(m->y_pos);
    free(m);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  3. HASH TABLE ACCESS                                              */
/* ═══════════════════════════════════════════════════════════════════ */

#define HASH_SIZE  1048576u  /* 1M slots — enough for large tensors */

typedef struct {
    uint32_t key;       /* weight index */
    float    value;     /* dequantized weight */
    uint8_t  occupied;
} HashEntry;

typedef struct {
    HashEntry *entries;
    uint32_t   size;
    uint64_t   collisions;
} HashMap;

static HashMap *hash_build(const int8_t *w, const float *s, uint64_t count) {
    HashMap *h = (HashMap*)calloc(1, sizeof(HashMap));
    if (!h) return NULL;
    h->size = HASH_SIZE;
    h->entries = (HashEntry*)calloc(HASH_SIZE, sizeof(HashEntry));
    if (!h->entries) { free(h); return NULL; }

    for (uint64_t i = 0; i < count; i++) {
        uint32_t key = (uint32_t)i;
        uint32_t idx = (key * 2654435761u) & (HASH_SIZE - 1);  /* multiply shift */

        while (h->entries[idx].occupied) {
            idx = (idx + 1) & (HASH_SIZE - 1);
            h->collisions++;
        }
        h->entries[idx].key = key;
        h->entries[idx].value = (float)w[i] * s[i];
        h->entries[idx].occupied = 1;
    }
    return h;
}

static inline float hash_get_weight(uint32_t key, const HashMap *h) {
    uint32_t idx = (key * 2654435761u) & (HASH_SIZE - 1);
    while (h->entries[idx].occupied) {
        if (h->entries[idx].key == key)
            return h->entries[idx].value;
        idx = (idx + 1) & (HASH_SIZE - 1);
    }
    return 0.0f;
}

static void hash_free(HashMap *h) {
    if (!h) return;
    free(h->entries);
    free(h);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MATMUL: Matrix × Vector (3 methods)                              */
/* ═══════════════════════════════════════════════════════════════════ */

static void matmul_array(float *out, const float *in,
                          const int8_t *w, const float *s, int M, int N)
{
    for (int i = 0; i < M; i++) {
        double sum = 0.0;
        for (int j = 0; j < N; j++) {
            int idx = i * N + j;
            sum += (double)array_get_weight(idx, w, s) * (double)in[j];
        }
        out[i] = (float)sum;
    }
}

static void matmul_engine(float *out, const float *in,
                           const EngineMap *m, const int8_t *w, const float *s,
                           int M, int N)
{
    for (int i = 0; i < M; i++) {
        float sum = 0.0f;
        for (int j = 0; j < N; j++) {
            uint64_t idx = (uint64_t)i * N + j;
            sum += engine_get_weight(idx, m, w, s) * in[j];
        }
        out[i] = sum;
    }
}

static void matmul_hash(float *out, const float *in,
                          const HashMap *h, int M, int N)
{
    for (int i = 0; i < M; i++) {
        float sum = 0.0f;
        for (int j = 0; j < N; j++) {
            uint32_t key = (uint32_t)(i * N + j);
            sum += hash_get_weight(key, h) * in[j];
        }
        out[i] = sum;
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  FFN FORWARD PASS                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

static float silu(float x) {
    return x / (1.0f + expf(-x));
}

/* Reference FFN: gate+up via silu, then down */
static void ffn_array(float *out, const float *x, int D, int ID,
                       const int8_t *wg, const float *sg,
                       const int8_t *wu, const float *su,
                       const int8_t *wd, const float *sd)
{
    float *hidden = (float*)malloc(ID * sizeof(float));
    matmul_array(hidden, x, wg, sg, ID, D);
    float *up = (float*)malloc(ID * sizeof(float));
    matmul_array(up, x, wu, su, ID, D);
    for (int i = 0; i < ID; i++) hidden[i] = silu(hidden[i]) * up[i];
    matmul_array(out, hidden, wd, sd, D, ID);
    free(hidden); free(up);
}

static void ffn_engine(float *out, const float *x, int D, int ID,
                        const EngineMap *mg, const int8_t *wg, const float *sg,
                        const EngineMap *mu, const int8_t *wu, const float *su,
                        const EngineMap *md, const int8_t *wd, const float *sd)
{
    float *hidden = (float*)malloc(ID * sizeof(float));
    matmul_engine(hidden, x, mg, wg, sg, ID, D);
    float *up = (float*)malloc(ID * sizeof(float));
    matmul_engine(up, x, mu, wu, su, ID, D);
    for (int i = 0; i < ID; i++) hidden[i] = silu(hidden[i]) * up[i];
    matmul_engine(out, hidden, md, wd, sd, D, ID);
    free(hidden); free(up);
}

static void ffn_hash(float *out, const float *x, int D, int ID,
                      const HashMap *hg, const HashMap *hu, const HashMap *hd)
{
    float *hidden = (float*)malloc(ID * sizeof(float));
    matmul_hash(hidden, x, hg, ID, D);
    float *up = (float*)malloc(ID * sizeof(float));
    matmul_hash(up, x, hu, ID, D);
    for (int i = 0; i < ID; i++) hidden[i] = silu(hidden[i]) * up[i];
    matmul_hash(out, hidden, hd, D, ID);
    free(hidden); free(up);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                              */
/* ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s model.gguf [layer_idx]\n", argv[0]);
        return 1;
    }

    const char *model_path = argv[1];
    int layer_idx = (argc > 2) ? atoi(argv[2]) : 0;

    printf("═══ Geo vs Hash Inference Benchmark ═══\n\n");
    printf("  Model: %s\n", model_path);
    printf("  Layer: blk.%d\n\n", layer_idx);

    /* Open GGUF */
    GGUF_File *gf = gguf_open(model_path);
    if (!gf) { printf("  FAILED to open GGUF\n"); return 1; }
    printf("1. GGUF: %llu tensors\n", (unsigned long long)gf->tensor_count);

    /* Find FFN tensors */
    char gate_name[64], up_name[64], down_name[64];
    snprintf(gate_name, sizeof(gate_name), "blk.%d.ffn_gate.weight", layer_idx);
    snprintf(up_name,   sizeof(up_name),   "blk.%d.ffn_up.weight",   layer_idx);
    snprintf(down_name, sizeof(down_name), "blk.%d.ffn_down.weight", layer_idx);

    int gate_idx = gguf_find_tensor(gf, gate_name);
    int up_idx   = gguf_find_tensor(gf, up_name);
    int down_idx = gguf_find_tensor(gf, down_name);

    if (gate_idx < 0 || up_idx < 0 || down_idx < 0) {
        printf("  FAILED to find FFN tensors for layer %d\n", layer_idx);
        gguf_close(gf);
        return 1;
    }

    int D  = (int)gf->tensors[gate_idx].dims[0];
    int ID = (int)gf->tensors[gate_idx].dims[1];
    printf("  FFN: D=%d, ID=%d\n", D, ID);

    /* Read tensor data */
    printf("\n2. Reading tensor data...\n");
    int8_t *w_gate, *w_up, *w_down;
    float *s_gate, *s_up, *s_down;
    uint64_t n_gate, n_up, n_down;

    double t0 = now_sec();
    if (gguf_read_q8_tensor(gf, gate_idx, &w_gate, &s_gate, &n_gate) != 0) { printf("  FAILED gate\n"); return 1; }
    if (gguf_read_q8_tensor(gf, up_idx,   &w_up,   &s_up,   &n_up)   != 0) { printf("  FAILED up\n");   return 1; }
    if (gguf_read_q8_tensor(gf, down_idx, &w_down, &s_down, &n_down) != 0) { printf("  FAILED down\n"); return 1; }
    double t_read = now_sec() - t0;
    printf("  Read: %.3f sec\n", t_read);
    printf("  Gate: %llu weights, Up: %llu, Down: %llu\n",
           (unsigned long long)n_gate, (unsigned long long)n_up, (unsigned long long)n_down);

    /* Build data structures */
    printf("\n3. Building access structures...\n");

    t0 = now_sec();
    EngineMap *m_gate = engine_build(w_gate, n_gate);
    EngineMap *m_up   = engine_build(w_up,   n_up);
    EngineMap *m_down = engine_build(w_down, n_down);
    double t_engine = now_sec() - t0;
    printf("  Engine: %.3f sec\n", t_engine);

    t0 = now_sec();
    HashMap *h_gate = hash_build(w_gate, s_gate, n_gate);
    HashMap *h_up   = hash_build(w_up,   s_up,   n_up);
    HashMap *h_down = hash_build(w_down, s_down, n_down);
    double t_hash = now_sec() - t0;
    printf("  Hash:   %.3f sec (collisions: %llu)\n", t_hash,
           (unsigned long long)(h_gate->collisions + h_up->collisions + h_down->collisions));

    /* Generate random input */
    float *x = (float*)malloc(D * sizeof(float));
    srand(42);
    for (int i = 0; i < D; i++)
        x[i] = (float)(rand() % 256 - 128) / 128.0f;

    /* Benchmark: 10 iterations */
    int ITERS = 10;
    printf("\n4. Benchmark: %d iterations of FFN forward pass...\n", ITERS);

    float *out_ref = (float*)calloc(D, sizeof(float));
    float *out_eng = (float*)calloc(D, sizeof(float));
    float *out_hsh = (float*)calloc(D, sizeof(float));

    /* Array */
    double ta0 = now_sec();
    for (int it = 0; it < ITERS; it++)
        ffn_array(out_ref, x, D, ID, w_gate, s_gate, w_up, s_up, w_down, s_down);
    double t_array = (now_sec() - ta0) / ITERS;

    /* Engine */
    double te0 = now_sec();
    for (int it = 0; it < ITERS; it++)
        ffn_engine(out_eng, x, D, ID, m_gate, w_gate, s_gate, m_up, w_up, s_up, m_down, w_down, s_down);
    double t_engine_it = (now_sec() - te0) / ITERS;

    /* Hash */
    double th0 = now_sec();
    for (int it = 0; it < ITERS; it++)
        ffn_hash(out_hsh, x, D, ID, h_gate, h_up, h_down);
    double t_hash_it = (now_sec() - th0) / ITERS;

    /* Results */
    printf("\n═══ RESULTS ═══\n\n");
    printf("  %-20s %10.4f ms\n", "Array (baseline)", t_array * 1000.0);
    printf("  %-20s %10.4f ms  (%.2fx vs array)\n", "Engine (geometric)", t_engine_it * 1000.0, t_array / t_engine_it);
    printf("  %-20s %10.4f ms  (%.2fx vs array)\n", "Hash table",        t_hash_it * 1000.0, t_array / t_hash_it);
    printf("\n");
    printf("  Engine vs Hash:    %.2fx\n", t_hash_it / t_engine_it);

    /* Accuracy check */
    float max_err_eng = 0, max_err_hsh = 0;
    for (int i = 0; i < D; i++) {
        float e1 = fabsf(out_eng[i] - out_ref[i]);
        float e2 = fabsf(out_hsh[i] - out_ref[i]);
        if (e1 > max_err_eng) max_err_eng = e1;
        if (e2 > max_err_hsh) max_err_hsh = e2;
    }
    printf("\n  Max error (Engine vs Array): %f\n", max_err_eng);
    printf("  Max error (Hash vs Array):   %f\n", max_err_hsh);

    /* Cleanup */
    free(out_ref); free(out_eng); free(out_hsh); free(x);
    engine_free(m_gate); engine_free(m_up); engine_free(m_down);
    hash_free(h_gate); hash_free(h_up); hash_free(h_down);
    free(w_gate); free(w_up); free(w_down);
    free(s_gate); free(s_up); free(s_down);
    gguf_close(gf);

    return 0;
}
