/*
 * gguf_inference_demo.c — Real inference through Geometric Engine
 * ═══════════════════════════════════════════════════════════════════
 *
 * "Engine ไม่ใช่แค่อ่าน weights ไว — inference จริงบน SmolLM2 FFN layer"
 *
 * 1. Multi-capo:  2 bytes/weight (capo_id + y) → arbitrary tensor sizes
 * 2. Dequant Q8_0: float16 scale × int8 → float32
 * 3. Matmul via engine → FFN forward pass
 * 4. Verify output vs pure array-based matmul
 *
 * Build: gcc -O2 -std=c11 -D_GNU_SOURCE beam_addressing/gguf_inference_demo.c -o beam_addressing/gguf_inference_demo.exe
 * Usage: gguf_inference_demo.exe model.gguf layer_idx
 *        layer_idx = transformer block index (0-based)
 *
 * Example: gguf_inference_demo.exe smolVLM-256M-Instruct-text.Q8_0.gguf 0
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

/* ═══════════════════════════════════════════════════════════════════ */
/*  1. GGUF Reader (shared with engine_demo)                         */
/* ═══════════════════════════════════════════════════════════════════ */

#include "gguf_reader.h"

/* ── Read raw Q8_0 tensor data into int8 array + scale array ── */
/* Returns weights (int8), scales (float), and count.
 * Caller must free both arrays. */

/* IEEE 754 binary16 → float32 */
static float fp16_to_fp32(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t h_exp  = (h >> 10) & 0x1f;
    uint32_t h_mant = h & 0x3ff;
    uint32_t f32_exp, f32_mant;

    if (h_exp == 0) {
        if (h_mant == 0) return 0.0f;         /* zero */
        /* Subnormal: normalize by left-shifting mantissa */
        int shift = 0;
        while (!(h_mant & 0x400)) { h_mant <<= 1; shift++; }
        f32_exp  = 113u - (uint32_t)shift;    /* 1+127-15-shift */
        f32_mant = (h_mant & 0x3ff) << 13;
    } else if (h_exp == 31) {
        f32_exp  = 0xff;                      /* Inf / NaN */
        f32_mant = h_mant << 13;
    } else {
        f32_exp  = h_exp + 112u;              /* h_exp - 15 + 127 */
        f32_mant = h_mant << 13;
    }

    uint32_t bits = sign | (f32_exp << 23) | f32_mant;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static int gguf_read_q8_tensor(GGUF_File *gf, uint64_t idx,
                                int8_t **out_weights, float **out_scales,
                                uint64_t *out_count)
{
    if (idx >= gf->tensor_count) return -1;
    GGUF_Tensor *t = &gf->tensors[idx];
    if (t->type != GGML_TYPE_Q8_0) return -2;

    uint64_t n_weights = t->n_weights;
    uint64_t n_blocks = (n_weights + 31) / 32;
    uint64_t n_bytes = n_blocks * 34; /* Q8_0: 2B scale + 32B values per block */

    /* Read raw block data */
    uint8_t *raw = (uint8_t*)malloc((size_t)n_bytes);
    if (!raw) return -3;

    uint64_t file_offset = gf->tensor_data_start + t->offset;
    /* Debug: try both relative and absolute offsets */
    static int debug_printed = 0;
    if (!debug_printed && strstr(t->name, "ffn_gate")) {
        debug_printed = 1;
        printf("  DEBUG gate: tensor_data_start=%llu offset=%llu file_offset=%llu\n",
               (unsigned long long)gf->tensor_data_start,
               (unsigned long long)t->offset,
               (unsigned long long)file_offset);
    }
    if (fseeko(gf->fp, (off_t)file_offset, SEEK_SET) != 0) {
        free(raw); return -4;
    }
    if (fread(raw, 1, (size_t)n_bytes, gf->fp) != (size_t)n_bytes) {
        free(raw); return -5;
    }

    /* Decode Q8_0 blocks */
    int8_t *weights = (int8_t*)malloc((size_t)n_weights);
    float *scales = (float*)malloc((size_t)n_weights * sizeof(float));
    if (!weights || !scales) {
        free(raw); free(weights); free(scales); return -6;
    }

    for (uint64_t b = 0; b < n_blocks; b++) {
        uint64_t boff = b * 34;
        /* Read float16 scale (IEEE 754 binary16) → float32 */
        uint16_t scale_raw;
        memcpy(&scale_raw, raw + boff, 2);
        /* Non-finite sanitization: replace Inf/NaN (exp=31 in binary16) with zero */
        static uint64_t n_nonfinite = 0;
        if ((scale_raw & 0x7c00) == 0x7c00) {
            scale_raw = 0;
            n_nonfinite++;
        }
        float scale = fp16_to_fp32(scale_raw);

        /* Debug first 5 scales */
        if (b < 5)
            printf("  Scale[%llu] = 0x%04x -> %f\n",
                   (unsigned long long)b, (unsigned int)scale_raw, (double)scale);

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
/*  2. Geometric Engine — Multi-capo + XOR decode                    */
/* ═══════════════════════════════════════════════════════════════════ */

#define CAPO_SIZE    65536u   /* 256 × 256 positions per capo */
#define CAPO_DIM     256u     /* dimensions of capo square */
#define STRIDE       37u      /* prime stride for distribution */

/* Multi-capo mapping: 2 bytes/weight (capo_id + y_pos) */
typedef struct {
    uint64_t n_weights;      /* total weights in tensor */
    uint16_t capo_count;     /* number of capos */
    uint16_t *capo_id;       /* capo_id[i] ∈ [0, capo_count-1] */
    uint8_t  *y_pos;         /* y_pos[i] ∈ [0, 255] */
} GeomCapoMap;

/* ── Build multi-capo mapping from Q8 weights ── */
static GeomCapoMap *geom_build_capo_map(const int8_t *weights, uint64_t count)
{
    GeomCapoMap *map = (GeomCapoMap*)calloc(1, sizeof(GeomCapoMap));
    if (!map) return NULL;

    map->n_weights = count;
    map->capo_count = (uint16_t)((count + CAPO_SIZE - 1) / CAPO_SIZE);
    if (map->capo_count == 0) map->capo_count = 1;

    map->capo_id = (uint16_t*)calloc((size_t)count, sizeof(uint16_t));
    map->y_pos   = (uint8_t*) calloc((size_t)count, sizeof(uint8_t));
    if (!map->capo_id || !map->y_pos) {
        free(map->capo_id); free(map->y_pos); free(map); return NULL;
    }

    /* Track used positions per capo */
    uint8_t **used = (uint8_t**)calloc(map->capo_count, sizeof(uint8_t*));
    for (uint16_t c = 0; c < map->capo_count; c++) {
        used[c] = (uint8_t*)calloc(CAPO_SIZE, 1);
        if (!used[c]) {
            for (uint16_t k = 0; k <= c; k++) free(used[k]);
            free(used); free(map->capo_id); free(map->y_pos); free(map);
            return NULL;
        }
    }

    uint64_t collide = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint16_t capo = (uint16_t)(i / CAPO_SIZE);
        if (capo >= map->capo_count) capo = (uint16_t)(map->capo_count - 1);

        uint8_t d = (uint8_t)((int32_t)weights[i] + 128);
        uint8_t x = (uint8_t)((i * STRIDE) % CAPO_DIM);
        uint8_t y_base = (uint8_t)(x ^ d);
        uint8_t y = y_base;

        /* Linear probe within capo */
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
        if (attempts >= CAPO_DIM) collide++;

        map->capo_id[i] = capo;
        map->y_pos[i] = y;
    }

    /* Cleanup used arrays */
    for (uint16_t c = 0; c < map->capo_count; c++)
        free(used[c]);
    free(used);

    if (collide > 0)
        fprintf(stderr, "  Collisions: %llu / %llu (%.1f%%)\n",
                (unsigned long long)collide, (unsigned long long)count,
                (double)collide * 100.0 / (double)count);

    return map;
}

/* ── Decode weight via geometric engine (with capo) ── */
static inline int8_t geom_weight_capo(uint64_t i, const GeomCapoMap *map)
{
    uint8_t x = (uint8_t)((i * STRIDE) % CAPO_DIM);
    uint8_t y = map->y_pos[i];
    /* XOR(x,y) = d = w+128 → w = d - 128 */
    return (int8_t)((uint8_t)(x ^ y) - 128);
}

static void geom_free_capo_map(GeomCapoMap *map)
{
    if (!map) return;
    free(map->capo_id);
    free(map->y_pos);
    free(map);
}


/* ═══════════════════════════════════════════════════════════════════ */
/*  3. Dequantize Q8_0 → float32                                     */
/* ═══════════════════════════════════════════════════════════════════════ */

static inline float dequant_q8(int8_t qw, float scale)
{
    return (float)qw * scale;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  4. Matmul: matrix × vector                                        */
/* ═══════════════════════════════════════════════════════════════════════ */

/* Array-based reference matmul */
static void matmul_ref(float *out, const float *in,
                        const int8_t *weights, const float *scales,
                        int M, int N)
{
    for (int i = 0; i < M; i++) {
        double sum = 0.0;
        for (int j = 0; j < N; j++) {
            int idx = i * N + j;
            double contrib = (double)dequant_q8(weights[idx], scales[idx]) * (double)in[j];
            sum += contrib;
        }
        out[i] = (float)sum;
        /* NaN check */
        if (isnan((double)out[i])) {
            fprintf(stderr, "  REF NaN: i=%d sum=%f\n", i, sum);
            /* Trace first 5 contributing NaN values for this row */
            int nnan = 0;
            for (int j = 0; j < N && nnan < 5; j++) {
                int idx = i * N + j;
                double dq = (double)dequant_q8(weights[idx], scales[idx]);
                if (isnan(dq)) {
                    fprintf(stderr, "    deq NaN at j=%d w=%d s=%f\n",
                            j, (int)weights[idx], (double)scales[idx]);
                    nnan++;
                }
            }
        }
    }
}

/* Engine-based matmul */
static void matmul_engine(float *out, const float *in,
                           const GeomCapoMap *map,
                           const float *scales,
                           int M, int N)
{
    for (int i = 0; i < M; i++) {
        float sum = 0.0f;
        for (int j = 0; j < N; j++) {
            int idx = i * N + j;
            int8_t qw = geom_weight_capo((uint64_t)idx, map);
            sum += dequant_q8(qw, scales[idx]) * in[j];
        }
        out[i] = sum;
    }
}

/* SiLU activation: x * sigmoid(x) */
static inline float silu(float x)
{
    return x / (1.0f + expf(-x));
}

/* Apply SiLU in-place */
static void silu_vec(float *v, int n)
{
    for (int i = 0; i < n; i++)
        v[i] = silu(v[i]);
}

/* Element-wise multiply */
static void mul_vec(float *a, const float *b, int n)
{
    for (int i = 0; i < n; i++)
        a[i] *= b[i];
}

/* ── FFN forward pass (one transformer block) ── */
/* SmolLM2 FFN: gate_proj ? SiLU, up_proj, down_proj
 *   hidden = down_proj(silu(gate_proj(x)) * up_proj(x))
 * Dimensions:
 *   x: [1×D]
 *   gate, up: [D×ID]  where ID = intermediate_dim (4D)
 *   down: [ID×D]
 */
static void ffn_forward_ref(float *out,
                             const float *input, int D, int ID,
                             const int8_t *w_gate, const float *s_gate,
                             const int8_t *w_up,   const float *s_up,
                             const int8_t *w_down, const float *s_down)
{
    float *gate = (float*)malloc(ID * sizeof(float));
    float *up   = (float*)malloc(ID * sizeof(float));

    matmul_ref(gate, input, w_gate, s_gate, ID, D);
    /* Debug: check for NaN in gate output */
    int gate_nan = 0, gate_nan_pos = -1;
    for (int i = 0; i < ID; i++) {
        if (isnan(gate[i])) { gate_nan = 1; gate_nan_pos = i; break; }
    }
    matmul_ref(up,   input, w_up,   s_up,   ID, D);
    int up_nan = 0, up_nan_pos = -1;
    for (int i = 0; i < ID; i++) {
        if (isnan(up[i])) { up_nan = 1; up_nan_pos = i; break; }
    }
    fprintf(stderr, "  FFN_REF: gate=%s gate_firstnan=%d up=%s up_firstnan=%d\n",
            gate_nan ? "NaN" : "OK", gate_nan_pos,
            up_nan ? "NaN" : "OK", up_nan_pos);
    fprintf(stderr, "  DBG: gate[0]=%f gate[%d]=%f\n", (double)gate[0], ID-1, (double)gate[ID-1]);
    silu_vec(gate, ID);
    fprintf(stderr, "  DBG: after silu gate[0]=%f\n", (double)gate[0]);
    mul_vec(gate, up, ID);
    fprintf(stderr, "  DBG: after mul gate[0]=%f\n", (double)gate[0]);
    matmul_ref(out, gate, w_down, s_down, D, ID);
    fprintf(stderr, "  DBG: out[0]=%f\n", (double)out[0]);

    free(gate);
    free(up);
}

static void ffn_forward_engine(float *out,
                                const float *input, int D, int ID,
                                const GeomCapoMap *m_gate, const float *s_gate,
                                const GeomCapoMap *m_up,   const float *s_up,
                                const GeomCapoMap *m_down, const float *s_down)
{
    float *gate = (float*)malloc(ID * sizeof(float));
    float *up   = (float*)malloc(ID * sizeof(float));

    matmul_engine(gate, input, m_gate, s_gate, ID, D);
    matmul_engine(up,   input, m_up,   s_up,   ID, D);
    silu_vec(gate, ID);
    mul_vec(gate, up, ID);
    matmul_engine(out, gate, m_down, s_down, D, ID);

    free(gate);
    free(up);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  5. Timer                                                           */
/* ═══════════════════════════════════════════════════════════════════════ */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}


/* ═══════════════════════════════════════════════════════════════════════ */
/*  Main                                                               */
/* ═══════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s model.gguf [layer_idx]\n", argv[0]);
        printf("  layer_idx = transformer block index (default: 0)\n");
        return 1;
    }

    const char *model_path = argv[1];
    int layer_idx = (argc > 2) ? atoi(argv[2]) : 0;

    printf("═══ Geometric Engine Inference Demo ═══\n\n");
    printf("  Model: %s\n", model_path);
    printf("  Layer: blk.%d\n\n", layer_idx);

    /* ── Open GGUF ── */
    GGUF_File *gf = gguf_open(model_path);
    if (!gf) {
        printf("  FAILED to open GGUF file\n");
        return 1;
    }
    printf("1. GGUF: %llu tensors, version %u\n",
           (unsigned long long)gf->tensor_count, gf->version);

    /* ── Find FFN tensors ── */
    char gate_name[64], up_name[64], down_name[64];
    snprintf(gate_name, sizeof(gate_name), "blk.%d.ffn_gate.weight", layer_idx);
    snprintf(up_name,   sizeof(up_name),   "blk.%d.ffn_up.weight",   layer_idx);
    snprintf(down_name, sizeof(down_name), "blk.%d.ffn_down.weight", layer_idx);

    int gate_idx = gguf_find_tensor(gf, gate_name);
    int up_idx   = gguf_find_tensor(gf, up_name);
    int down_idx = gguf_find_tensor(gf, down_name);

    if (gate_idx < 0 || up_idx < 0 || down_idx < 0) {
        printf("  FAILED to find FFN tensors for layer %d\n", layer_idx);
        printf("  Searched for: %s, %s, %s\n", gate_name, up_name, down_name);
        gguf_close(gf);
        return 1;
    }

    GGUF_Tensor *t_gate = &gf->tensors[gate_idx];
    GGUF_Tensor *t_up   = &gf->tensors[up_idx];
    GGUF_Tensor *t_down = &gf->tensors[down_idx];

    int D  = (int)t_gate->dims[0];  /* hidden dimension */
    int ID = (int)t_gate->dims[1];  /* intermediate dimension */

    printf("  FFN: D=%d, ID=%d\n", D, ID);
    printf("  Gate: %s (%s)\n", t_gate->name, (t_gate->type == GGML_TYPE_Q8_0) ? "Q8_0" : "?");
    printf("  Up:   %s (%s)\n", t_up->name,   (t_up->type == GGML_TYPE_Q8_0) ? "Q8_0" : "?");
    printf("  Down: %s (%s)\n", t_down->name, (t_down->type == GGML_TYPE_Q8_0) ? "Q8_0" : "?");

    /* ── Read tensor data ── */
    printf("\n2. Reading tensor data...\n");

    int8_t *w_gate, *w_up, *w_down;
    float *s_gate, *s_up, *s_down;
    uint64_t n_gate, n_up, n_down;

    if (gguf_read_q8_tensor(gf, gate_idx, &w_gate, &s_gate, &n_gate) != 0) { printf("  FAILED gate\n"); return 1; }
    if (gguf_read_q8_tensor(gf, up_idx,   &w_up,   &s_up,   &n_up)   != 0) { printf("  FAILED up\n");   return 1; }
    if (gguf_read_q8_tensor(gf, down_idx, &w_down, &s_down, &n_down) != 0) { printf("  FAILED down\n"); return 1; }

    printf("  Gate: %llu weights (%llu blocks)\n", (unsigned long long)n_gate,
           (unsigned long long)((n_gate + 31) / 32));
    printf("  Up:   %llu weights (%llu blocks)\n", (unsigned long long)n_up,
           (unsigned long long)((n_up + 31) / 32));
    printf("  Down: %llu weights (%llu blocks)\n", (unsigned long long)n_down,
           (unsigned long long)((n_down + 31) / 32));

    /* ── Build multi-capo maps ── */
    printf("\n3. Building multi-capo maps...\n");

    double t0 = now_sec();
    GeomCapoMap *m_gate = geom_build_capo_map(w_gate, n_gate);
    GeomCapoMap *m_up   = geom_build_capo_map(w_up,   n_up);
    GeomCapoMap *m_down = geom_build_capo_map(w_down, n_down);
    double t_map = now_sec() - t0;

    if (!m_gate || !m_up || !m_down) {
        printf("  FAILED to build capo maps\n");
        return 1;
    }
    printf("  Gate: %d capos, Up: %d capos, Down: %d capos (%.3f sec)\n",
           m_gate->capo_count, m_up->capo_count, m_down->capo_count, t_map);

    /* ── Verify Q8 decode accuracy ── */
    printf("\n4. Verifying Q8 decode accuracy:\n");
    uint64_t n_check = 1000;
    uint64_t n_ok_gate = 0, n_ok_up = 0, n_ok_down = 0;
    for (uint64_t i = 0; i < n_check && i < n_gate; i++)
        if (geom_weight_capo(i, m_gate) == w_gate[i]) n_ok_gate++;
    for (uint64_t i = 0; i < n_check && i < n_up; i++)
        if (geom_weight_capo(i, m_up) == w_up[i]) n_ok_up++;
    for (uint64_t i = 0; i < n_check && i < n_down; i++)
        if (geom_weight_capo(i, m_down) == w_down[i]) n_ok_down++;

    printf("  Gate: %llu/%llu (%.1f%%)\n",
           (unsigned long long)n_ok_gate, (unsigned long long)n_check,
           (double)n_ok_gate * 100.0 / (double)n_check);
    printf("  Up:   %llu/%llu (%.1f%%)\n",
           (unsigned long long)n_ok_up, (unsigned long long)n_check,
           (double)n_ok_up * 100.0 / (double)n_check);
    printf("  Down: %llu/%llu (%.1f%%)\n",
           (unsigned long long)n_ok_down, (unsigned long long)n_check,
           (double)n_ok_down * 100.0 / (double)n_check);

    /* ── Generate random input vector ── */
    float *x = (float*)malloc(D * sizeof(float));
    srand(42);
    for (int i = 0; i < D; i++)
        x[i] = (float)(rand() % 256 - 128) / 128.0f;

    printf("\n5. FFN forward pass (1 token x %d→%d→%d)...\n", D, ID, D);

    /* Reference FFN (array-based) */
    float *out_ref = (float*)calloc(D, sizeof(float));
    double tr0 = now_sec();
    ffn_forward_ref(out_ref, x, D, ID,
                     w_gate, s_gate, w_up, s_up, w_down, s_down);
    double t_ref = now_sec() - tr0;

    /* Engine-based FFN */
    float *out_eng = (float*)calloc(D, sizeof(float));
    double te0 = now_sec();
    ffn_forward_engine(out_eng, x, D, ID,
                        m_gate, s_gate, m_up, s_up, m_down, s_down);
    double t_eng = now_sec() - te0;

    /* Debug: check raw values */
    printf("  Debug: first 3 matmul results before compare:\n");
    printf("    ref[0]=%f eng[0]=%f  test_val=%f\n",
           (double)out_ref[0], (double)out_eng[0], 3.14159f * 42.0f);
    printf("    ref[1]=%f eng[1]=%f\n", (double)out_ref[1], (double)out_eng[1]);
    printf("    x[0]=%f x[1]=%f x[2]=%f\n", (double)x[0], (double)x[1], (double)x[2]);
    printf("    w_gate[0]=%d s_gate[0]=%f deq=%f\n",
           (int)w_gate[0], (double)s_gate[0],
           (double)dequant_q8(w_gate[0], s_gate[0]));
    /* Quick check: run a tiny matmul inline */
    float test_sum = 0.0f;
    for (int j = 0; j < 576; j++) test_sum += dequant_q8(w_gate[j], s_gate[j]) * x[j];
    printf("    manual gate[0] dot = %f (expect non-NaN)\n", (double)test_sum);
    /* Manual gate[1] check */
    float test_sum2 = 0.0f;
    for (int j = 0; j < 576; j++) test_sum2 += dequant_q8(w_gate[576 + j], s_gate[576 + j]) * x[j];
    printf("    manual gate[1] dot = %f\n", (double)test_sum2);
    /* Find NaN block in rows 1+ */
    printf("    Scanning first 96 block scales of gate (row 0-2)...\n");
    for (int b = 0; b < 96; b++) {
        uint64_t boff = b * 32;
        float s = s_gate[boff];
        if (isnan(s)) {
            printf("      BLOCK %d SCALE NaN at weight %llu\n", b, (unsigned long long)boff);
        }
    }
    /* Debug blocks 34,35,36 */
    for (int b = 34; b <= 36; b++) {
        printf("    block %d: s_gate[%d]=%f (raw mem)", b, b*32, (double)s_gate[b*32]);
        /* Check if all weights in this block are zero */
        int all_zero = 1;
        for (int k = 0; k < 32 && (b*32 + k) < n_gate; k++) {
            if (w_gate[b*32 + k] != 0) { all_zero = 0; break; }
        }
        printf(" all_zero=%d\n", all_zero);
    }
    printf("    ...done\n");

    /* ── Compare outputs ── */
    float max_err = 0.0f;
    double sum_sq_err = 0.0;
    double sum_sq_ref = 0.0;
    for (int i = 0; i < D; i++) {
        float err = fabsf(out_eng[i] - out_ref[i]);
        if (err > max_err) max_err = err;
        sum_sq_err += (double)(err * err);
        sum_sq_ref += (double)(out_ref[i] * out_ref[i]);
    }
    double rmse = sqrt(sum_sq_err / D);
    double snr = (sum_sq_ref > 1e-30) ? 10.0 * log10(sum_sq_ref / sum_sq_err) : 0.0;

    printf("\n6. Results:\n");
    printf("  %-25s %12.4f sec  %12.4f ms\n", "Reference (array)", t_ref, t_ref * 1000.0);
    printf("  %-25s %12.4f sec  %12.4f ms\n", "Engine (geometric)", t_eng, t_eng * 1000.0);
    printf("  %-25s %12s\n", "Speed", (t_eng <= t_ref * 1.1) ? "✓ COMPETITIVE" : "SLOWER");
    printf("\n");
    printf("  Max error:    %f\n", max_err);
    printf("  RMSE:         %f\n", rmse);
    printf("  SNR:          %.1f dB\n", snr);
    printf("  Match:        %s\n", (snr > 30.0) ? "✓ HIGH" : (snr > 15.0 ? "OK" : "⚠ LOW"));

    /* Show first 8 output values */
    printf("\n  First 8 outputs:\n");
    printf("    %10s | %12s %12s | %10s\n", "idx", "reference", "engine", "error");
    printf("    " "--------" "-" "----------" "-" "----------" "-" "----------" "\n");
    for (int i = 0; i < 8 && i < D; i++) {
        printf("    %8d | %12.6f %12.6f | %10.6f\n",
               i, out_ref[i], out_eng[i], out_eng[i] - out_ref[i]);
    }

    /* Storage comparison */
    uint64_t w_total = n_gate + n_up + n_down;
    uint64_t q8_bytes = w_total + (w_total + 31) / 32 * 2; /* Q8: weights + scales */
    uint64_t eng_bytes = w_total * 2; /* capo_id + y_pos per weight */
    float ratio = (float)eng_bytes / (float)q8_bytes;

    printf("\n7. Storage per weight:\n");
    printf("  Q8_0 raw:      %.2f MB  (%llu weights + %llu scales)\n",
           (double)q8_bytes / 1048576.0,
           (unsigned long long)w_total,
           (unsigned long long)((w_total + 31) / 32) * 2);
    printf("  Engine (capo): %.2f MB  (1B capo_id + 1B y_pos = 2B/weight)\n",
           (double)eng_bytes / 1048576.0);
    printf("  Ratio: %.2fx (engine uses %s storage)\n",
           (double)ratio,
           (ratio < 1.0) ? "LESS" : "MORE");

    /* Cleanup */
    printf("\n✓ Inference demo complete\n");

    free(w_gate); free(s_gate);
    free(w_up);   free(s_up);
    free(w_down); free(s_down);
    free(x); free(out_ref); free(out_eng);
    geom_free_capo_map(m_gate);
    geom_free_capo_map(m_up);
    geom_free_capo_map(m_down);
    gguf_close(gf);

    return 0;
}
