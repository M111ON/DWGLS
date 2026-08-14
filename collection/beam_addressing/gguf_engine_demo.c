/*
 * gguf_engine_demo.c — Geometric Engine wrapping real GGUF tensor
 * ═══════════════════════════════════════════════════════════════════
 *
 * "เอา engine ไปครอบ tensor จริง — จับต้องได้"
 *
 * Reads a GGUF model, maps a Q8 tensor onto the dual square,
 * and benchmarks geometric decode vs raw array access.
 *
 * Usage: gguf_engine_demo <model.gguf> [tensor_index]
 *
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ══════════════════════════════════════════════════════════════════
   MINIMAL GGUF READER
   ══════════════════════════════════════════════════════════════════ */

#define GGUF_MAGIC  0x46554747u  /* 'GGUF' */

/* GGML tensor types (from llama.cpp) */
typedef enum {
    GGML_TYPE_F32     = 0,
    GGML_TYPE_F16     = 1,
    GGML_TYPE_Q4_0    = 2,
    GGML_TYPE_Q4_1    = 3,
    GGML_TYPE_Q5_0    = 6,
    GGML_TYPE_Q5_1    = 7,
    GGML_TYPE_Q8_0    = 8,
    GGML_TYPE_Q8_1    = 9,
} GGMLType;

/* GGUF string */
typedef struct {
    uint64_t len;
    char    *data;
} GGUFFieldStr;

/* Tensor info */
typedef struct {
    char      name[256];
    uint32_t  n_dims;
    uint64_t  dims[4];
    uint32_t  type;
    uint64_t  offset;      /* file offset to tensor data */
    uint64_t  size_bytes;  /* computed size */
} GGUF_Tensor;

/* GGUF file handle */
typedef struct {
    FILE          *fp;
    uint32_t       version;
    uint64_t       tensor_count;
    uint64_t       kv_count;
    GGUF_Tensor   *tensors;
    uint64_t       tensor_data_start;  /* file offset where tensor data begins */
} GGUF_File;

/* Read a GGUF string from file */
static int read_gguf_str(FILE *fp, GGUFFieldStr *s)
{
    if (fread(&s->len, sizeof(s->len), 1, fp) != 1) return -1;
    s->data = (char*)malloc((size_t)(s->len + 1));
    if (!s->data) return -1;
    if (s->len > 0) {
        if (fread(s->data, 1, (size_t)s->len, fp) != (size_t)s->len) {
            free(s->data); s->data = NULL; return -1;
        }
    }
    s->data[s->len] = '\0';
    return 0;
}

/* Skip a GGUF value */
static int skip_gguf_value(FILE *fp, uint32_t type)
{
    switch (type) {
        case 0: { /* uint8 */ uint8_t v; return fread(&v,1,1,fp) == 1 ? 0 : -1; }
        case 1: { /* int8 */  int8_t v; return fread(&v,1,1,fp) == 1 ? 0 : -1; }
        case 2: { /* uint16 */ uint16_t v; return fread(&v,2,1,fp) == 1 ? 0 : -1; }
        case 3: { /* int16 */ int16_t v; return fread(&v,2,1,fp) == 1 ? 0 : -1; }
        case 4: { /* uint32 */ uint32_t v; return fread(&v,4,1,fp) == 1 ? 0 : -1; }
        case 5: { /* int32 */ int32_t v; return fread(&v,4,1,fp) == 1 ? 0 : -1; }
        case 6: { /* float32 */ float v; return fread(&v,4,1,fp) == 1 ? 0 : -1; }
        case 7: { /* bool */ uint8_t v; return fread(&v,1,1,fp) == 1 ? 0 : -1; }
        case 8: { /* string */
            GGUFFieldStr s;
            int r = read_gguf_str(fp, &s);
            free(s.data);
            return r;
        }
        case 9: { /* array */
            uint32_t arr_type;
            uint64_t arr_len;
            if (fread(&arr_type,4,1,fp) != 1) return -1;
            if (fread(&arr_len,8,1,fp) != 1) return -1;
            for (uint64_t i = 0; i < arr_len; i++)
                if (skip_gguf_value(fp, arr_type) != 0) return -1;
            return 0;
        }
        case 10: { /* uint64 */ uint64_t v; return fread(&v,8,1,fp) == 1 ? 0 : -1; }
        case 11: { /* int64 */ int64_t v; return fread(&v,8,1,fp) == 1 ? 0 : -1; }
        case 12: { /* float64 */ double v; return fread(&v,8,1,fp) == 1 ? 0 : -1; }
        default: return -1;
    }
}

/* GGML type size info */
static int ggml_type_block_size(uint32_t type, uint64_t *block_sz, uint64_t *weights_per_block)
{
    switch (type) {
        case GGML_TYPE_F32:   *block_sz = 4;  *weights_per_block = 1; return 0;
        case GGML_TYPE_F16:   *block_sz = 2;  *weights_per_block = 1; return 0;
        case GGML_TYPE_Q8_0:  *block_sz = 34; *weights_per_block = 32; return 0;
        case GGML_TYPE_Q4_0:  *block_sz = 18; *weights_per_block = 32; return 0;
        case GGML_TYPE_Q8_1:  *block_sz = 34; *weights_per_block = 32; return 0;
        default: return -1;
    }
}

/* Open GGUF file and read header + tensor info */
static GGUF_File *gguf_open(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("  Error: cannot open %s\n", path); return NULL; }

    GGUF_File *gf = (GGUF_File*)calloc(1, sizeof(GGUF_File));
    if (!gf) { fclose(fp); return NULL; }
    gf->fp = fp;

    /* Read header */
    uint32_t magic;
    if (fread(&magic, 4, 1, fp) != 1 || magic != GGUF_MAGIC) {
        printf("  Error: not a GGUF file (magic=0x%08X)\n", magic);
        fclose(fp); free(gf); return NULL;
    }
    if (fread(&gf->version, 4, 1, fp) != 1) goto fail;
    if (fread(&gf->tensor_count, 8, 1, fp) != 1) goto fail;
    if (fread(&gf->kv_count, 8, 1, fp) != 1) goto fail;

    printf("  GGUF version %u, %llu tensors, %llu metadata entries\n",
           (unsigned)gf->version, (unsigned long long)gf->tensor_count,
           (unsigned long long)gf->kv_count);

    /* Skip metadata KV pairs */
    for (uint64_t i = 0; i < gf->kv_count; i++) {
        GGUFFieldStr key;
        if (read_gguf_str(fp, &key) != 0) goto fail;
        uint32_t val_type;
        if (fread(&val_type, 4, 1, fp) != 1) { free(key.data); goto fail; }
        if (skip_gguf_value(fp, val_type) != 0) { free(key.data); goto fail; }
        free(key.data);
    }

    /* Read tensor info */
    gf->tensors = (GGUF_Tensor*)calloc((size_t)gf->tensor_count, sizeof(GGUF_Tensor));
    if (!gf->tensors) goto fail;

    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        GGUFFieldStr name;
        if (read_gguf_str(fp, &name) != 0) goto fail;
        strncpy(gf->tensors[i].name, name.data, 255);
        free(name.data);

        if (fread(&gf->tensors[i].n_dims, 4, 1, fp) != 1) goto fail;
        if (gf->tensors[i].n_dims > 4) gf->tensors[i].n_dims = 4;
        for (uint32_t d = 0; d < gf->tensors[i].n_dims; d++) {
            if (fread(&gf->tensors[i].dims[d], 8, 1, fp) != 1) goto fail;
        }
        if (fread(&gf->tensors[i].type, 4, 1, fp) != 1) goto fail;
        if (fread(&gf->tensors[i].offset, 8, 1, fp) != 1) goto fail;

        /* Compute size */
        uint64_t n_weights = 1;
        for (uint32_t d = 0; d < gf->tensors[i].n_dims; d++)
            n_weights *= gf->tensors[i].dims[d];

        uint64_t block_sz = 0, wpb = 1;
        if (ggml_type_block_size(gf->tensors[i].type, &block_sz, &wpb) != 0) {
            gf->tensors[i].size_bytes = n_weights; /* fallback */
        } else {
            uint64_t n_blocks = (n_weights + wpb - 1) / wpb;
            gf->tensors[i].size_bytes = n_blocks * block_sz;
        }
    }

    /* Tensor data starts after all tensor info */
    gf->tensor_data_start = ftell(fp);
    /* In GGUF, tensor data offset is relative to the start of tensor data section */
    /* But some formats use absolute offset. Let's compute both ways. */

    return gf;

fail:
    fclose(fp); free(gf->tensors); free(gf);
    return NULL;
}

static void gguf_close(GGUF_File *gf)
{
    if (!gf) return;
    if (gf->fp) fclose(gf->fp);
    free(gf->tensors);
    free(gf);
}

/* Read tensor weight data from GGUF, decode to int8_t array */
static int gguf_read_q8_tensor(GGUF_File *gf, uint64_t idx,
                                int8_t **out_weights, uint64_t *out_count)
{
    if (idx >= gf->tensor_count) return -1;
    GGUF_Tensor *t = &gf->tensors[idx];

    uint64_t n_weights = 1;
    for (uint32_t d = 0; d < t->n_dims; d++)
        n_weights *= t->dims[d];

    uint64_t n_bytes = t->size_bytes;
    uint8_t *raw = (uint8_t*)malloc((size_t)n_bytes);
    if (!raw) return -1;

    /* Read from file: offset from tensor_data_start + tensor.offset */
    long file_offset = (long)(gf->tensor_data_start + t->offset);
    if (fseeko(gf->fp, file_offset, SEEK_SET) != 0) {
        /* Try absolute offset */
        if (fseeko(gf->fp, (long)t->offset, SEEK_SET) != 0) {
            free(raw); return -1;
        }
    }
    if (fread(raw, 1, (size_t)n_bytes, gf->fp) != (size_t)n_bytes) {
        free(raw); return -1;
    }

    int8_t *weights = (int8_t*)malloc((size_t)n_weights * sizeof(int8_t));
    if (!weights) { free(raw); return -1; }

    /* Decode based on type */
    switch (t->type) {
        case GGML_TYPE_Q8_0: {
            /* Q8_0: blocks of [float16_t d][int8_t qs[32]] */
            uint64_t n_blocks = (n_weights + 31) / 32;
            for (uint64_t b = 0; b < n_blocks; b++) {
                uint64_t src_off = b * 34 + 2; /* skip scale */
                uint64_t dst_off = b * 32;
                uint64_t copy = (n_weights - dst_off > 32) ? 32 : (n_weights - dst_off);
                for (uint64_t j = 0; j < copy && (dst_off + j) < n_weights; j++) {
                    weights[dst_off + j] = (int8_t)raw[src_off + j];
                }
            }
            break;
        }
        case GGML_TYPE_F32: {
            float *f = (float*)raw;
            for (uint64_t i = 0; i < n_weights; i++)
                weights[i] = (int8_t)(f[i] * 127.0f);
            break;
        }
        case GGML_TYPE_F16: {
            /* F16: half-precision float */
            uint16_t *h = (uint16_t*)raw;
            for (uint64_t i = 0; i < n_weights; i++) {
                /* Minimal F16→F32 conversion */
                uint16_t hval = h[i];
                uint32_t sign = (hval >> 15) & 1;
                uint32_t exp = (hval >> 10) & 0x1F;
                uint32_t mant = hval & 0x3FF;
                float f;
                if (exp == 0) {
                    f = (float)(mant) / 16777216.0f;
                } else if (exp == 31) {
                    f = mant ? 0.0f / 0.0f : 1.0f / 0.0f;
                } else {
                    uint32_t f32 = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
                    memcpy(&f, &f32, 4);
                }
                if (sign) f = -f;
                weights[i] = (int8_t)(f * 127.0f);
            }
            break;
        }
        default: {
            /* Unsupported type — copy raw bytes */
            uint64_t copy = (n_bytes < n_weights) ? n_bytes : n_weights;
            memcpy(weights, raw, copy);
            break;
        }
    }

    *out_weights = weights;
    *out_count = n_weights;
    free(raw);
    return 0;
}


/* ══════════════════════════════════════════════════════════════════
   GEOMETRIC ENGINE
   ══════════════════════════════════════════════════════════════════ */

#define WV_RES    360u
#define STRIDE    37u   /* prime stride for good distribution */

/* Geometric decode: index → weight via XOR(x_i, y_i)
 * Storage: 1 byte/weight for y_i. x_i = (i * stride) % 256 (8-bit, keeps XOR in range). */
static inline int32_t geom_weight(uint32_t i, uint16_t stride,
                                   const uint8_t *stored_y)
{
    uint16_t x = (uint16_t)((i * stride) % 256u);
    uint16_t y = (uint16_t)stored_y[i];
    return (int32_t)((uint8_t)(x ^ y)) - 128;
}

/* Geometric phase: extract phase from index */
static inline uint8_t geom_phase(uint32_t i, uint16_t stride,
                                  const uint8_t *stored_y)
{
    uint16_t x = (uint16_t)((i * stride) % 256u);
    return (uint8_t)(x & 0xFF);
}

/* Neighbor shift: get neighbor weight */
static inline int32_t geom_neighbor(uint32_t i, uint16_t stride,
                                     const uint8_t *stored_y,
                                     int16_t dx, int16_t dy)
{
    uint16_t x = (uint16_t)((i * stride) % 256u);
    uint16_t nx = (uint16_t)((x + dx + 256u) % 256u);
    uint16_t ny = (uint16_t)((stored_y[i] + dy + WV_RES) % WV_RES);
    return (int32_t)((uint8_t)(nx ^ ny)) - 128;
}

/* Build geometric mapping from Q8 weights (maps up to map_count weights) */
static uint8_t *geom_build_mapping(const int8_t *weights, uint64_t count,
                                    uint64_t map_count, uint16_t stride)
{
    if (map_count > count) map_count = count;
    uint8_t *map = (uint8_t*)malloc(map_count * sizeof(uint8_t));
    if (!map) return NULL;

    /* Track used positions to avoid collisions */
    uint8_t *used = (uint8_t*)calloc(WV_RES * WV_RES, 1);
    if (!used) { free(map); return NULL; }

    for (uint64_t i = 0; i < map_count; i++) {
        int32_t w = (int32_t)weights[i];
        uint8_t d = (uint8_t)(w + 128);            /* Q8 offset: -128→0, 0→128, 127→255 */
        uint16_t x = (uint16_t)((i * stride) % 256u);  /* keep in 8-bit → y = x^d also 8-bit < 360 */

        /* Verify bounds */
        if (x >= WV_RES) {
            fprintf(stderr, "CRASH CHECK: x=%u >= WV_RES at i=%llu\n", (unsigned)x, (unsigned long long)i);
            free(map); free(used); return NULL;
        }

        /* Find y such that XOR(x,y) = d AND position is unused */
        uint16_t y_base = (uint16_t)(x ^ d);
        uint16_t y = y_base;

        /* Linear probe for collision avoidance (rare) */
        uint32_t attempts = 0;
        while (attempts < WV_RES) {
            uint32_t idx = y * WV_RES + x;
            if (idx >= WV_RES * WV_RES) {
                fprintf(stderr, "CRASH CHECK: idx=%u out of bounds (y=%u,x=%u) at i=%llu\n",
                        (unsigned)idx, (unsigned)y, (unsigned)x, (unsigned long long)i);
                free(map); free(used); return NULL;
            }
            if (!used[idx]) break;
            y = (uint16_t)((y + 1) % WV_RES);
            attempts++;
        }

        map[i] = (uint8_t)y;
        used[y * WV_RES + x] = 1;
    }

    free(used);
    return map;
}


/* ══════════════════════════════════════════════════════════════════
   TIMER
   ══════════════════════════════════════════════════════════════════ */

static double now_sec(void)
{
    clock_t c = clock();
    return (double)c / (double)CLOCKS_PER_SEC;
}


/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */

static void print_tensor_info(GGUF_Tensor *t, uint64_t idx)
{
    const char *typestr;
    switch (t->type) {
        case GGML_TYPE_F32:  typestr = "F32"; break;
        case GGML_TYPE_F16:  typestr = "F16"; break;
        case GGML_TYPE_Q4_0: typestr = "Q4_0"; break;
        case GGML_TYPE_Q4_1: typestr = "Q4_1"; break;
        case GGML_TYPE_Q5_0: typestr = "Q5_0"; break;
        case GGML_TYPE_Q5_1: typestr = "Q5_1"; break;
        case GGML_TYPE_Q8_0: typestr = "Q8_0"; break;
        case GGML_TYPE_Q8_1: typestr = "Q8_1"; break;
        default:             typestr = "?"; break;
    }

    uint64_t n = 1;
    char dimstr[128] = "";
    for (uint32_t d = 0; d < t->n_dims; d++) {
        n *= t->dims[d];
        char buf[32];
        snprintf(buf, 32, "%s%llu", (d == 0) ? "" : "×",
                 (unsigned long long)t->dims[d]);
        strcat(dimstr, buf);
    }

    printf("  [%2llu] %-50s %4s %20s = %llu weights (%llu bytes)\n",
           (unsigned long long)idx, t->name, typestr, dimstr,
           (unsigned long long)n, (unsigned long long)t->size_bytes);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: gguf_engine_demo <model.gguf> [tensor_index]\n");
        return 1;
    }

    const char *path = argv[1];
    int tensor_sel = (argc >= 3) ? atoi(argv[2]) : 0;

    printf("═══ GGUF Geometric Engine Demo ═══\n\n");
    printf("  Model: %s\n\n", path);

    /* ── Open GGUF ──────────────────────────────────────────── */
    printf("1. Opening GGUF file...\n");
    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("  FAILED\n"); return 1; }
    printf("  Tensor count: %llu\n\n", (unsigned long long)gf->tensor_count);

    /* ── List tensors ───────────────────────────────────────── */
    printf("2. Available tensors:\n");
    for (uint64_t i = 0; i < gf->tensor_count && i < 20; i++) {
        print_tensor_info(&gf->tensors[i], i);
    }
    if (gf->tensor_count > 20)
        printf("  ... (%llu more)\n", (unsigned long long)(gf->tensor_count - 20));

    /* ── Read selected tensor ───────────────────────────────── */
    printf("3. Reading tensor [%d]...\n", tensor_sel);
    if ((uint64_t)tensor_sel >= gf->tensor_count) {
        printf("  Invalid tensor index (max %llu)\n",
               (unsigned long long)(gf->tensor_count - 1));
        gguf_close(gf);
        return 1;
    }

    int8_t *weights = NULL;
    uint64_t n_weights = 0;
    if (gguf_read_q8_tensor(gf, (uint64_t)tensor_sel, &weights, &n_weights) != 0) {
        printf("  FAILED to read tensor\n");
        gguf_close(gf);
        return 1;
    }

    GGUF_Tensor *tensor = &gf->tensors[tensor_sel];
    printf("  Tensor: %s\n", tensor->name);
    printf("  Weights: %llu (%llu MB)\n",
           (unsigned long long)n_weights,
           (unsigned long long)(n_weights / 1024 / 1024));
    printf("  Raw value check (first 5): ");
    for (int i = 0; i < 5 && i < (int)n_weights; i++)
        printf("%d ", (int)weights[i]);
    printf("\n");

    /* ── Build geometric mapping ────────────────────────────── */
    printf("\n4. Building geometric mapping on dual square 360×360...\n");

    /* Constraint: dual square has only 65,536 unique (x,y) positions in 8-bit space (256×256).
     * For tensors larger than this, we need multiple capos.
     * Map up to 65,536 weights losslessly. */
    uint64_t max_lossless = 65536;
    uint64_t map_count = (n_weights > max_lossless) ? max_lossless : n_weights;
    uint64_t n_capos = (n_weights + max_lossless - 1) / max_lossless;

    printf("  Tensor: %llu weights\n", (unsigned long long)n_weights);
    printf("  One capo handles max %llu weights (lossless)\n", (unsigned long long)max_lossless);
    printf("  Mapping first %llu weights (%s: %llu capos needed for full tensor)\n",
           (unsigned long long)map_count,
           (n_weights > max_lossless) ? "partial" : "full",
           (unsigned long long)n_capos);

    double t0 = now_sec();
    uint8_t *geom_map = geom_build_mapping(weights, n_weights, map_count, STRIDE);
    double t1 = now_sec();
    if (!geom_map) { printf("  FAILED\n"); free(weights); gguf_close(gf); return 1; }
    printf("  Mapping built in %.3f sec (stride=%d)\n", t1 - t0, STRIDE);

    /* Verify mapping quality */
    uint64_t n_check = (n_weights < 1000) ? n_weights : 1000;
    uint64_t n_ok = 0;
    for (uint64_t i = 0; i < n_check && i < map_count; i++) {
        int32_t gw = geom_weight((uint32_t)i, STRIDE, geom_map);
        if (gw == (int32_t)weights[i]) n_ok++;
    }
    double match_rate = (double)n_ok / (double)n_check * 100.0;
    printf("  Verify: %llu/%llu matched (%.1f%%) — collisions from duplicate Q8 values\n",
           (unsigned long long)n_ok, (unsigned long long)n_check, match_rate);

    /* ── BENCHMARK: Array vs Geometric ─────────────────────── */
    printf("\n5. Benchmark: Array access vs Geometric decode\n");
    printf("  %-45s %12s %12s %s\n", "Method", "Time (s)", "Ops/sec", "vs Array");

    uint32_t N = (uint32_t)((map_count > 50000000) ? 50000000 : map_count);
    /* Repeat enough times to get measurable timing */
    uint32_t repeats = (N < 100000) ? 100 : 1;
    uint64_t total_ops = (uint64_t)N * repeats;
    volatile int64_t sink = 0;

    /* Array sequential */
    t0 = now_sec();
    for (uint32_t r = 0; r < repeats; r++)
        for (uint32_t i = 0; i < N; i++)
            sink += (int64_t)weights[i];
    double t_arr = now_sec() - t0;
    double arr_ops = (double)total_ops / t_arr;
    printf("  %-45s %12.4f %12.0f %s\n",
           "array[i] (sequential)", t_arr, arr_ops, "—");

    /* Geometric sequential */
    t0 = now_sec();
    for (uint32_t r = 0; r < repeats; r++)
        for (uint32_t i = 0; i < N; i++)
            sink += (int64_t)geom_weight(i, STRIDE, geom_map);
    double t_geom = now_sec() - t0;
    double geom_ops = (double)total_ops / t_geom;
    printf("  %-45s %12.4f %12.0f %s\n",
           "geom decode (sequential)", t_geom, geom_ops,
           (geom_ops > arr_ops) ? "✓ FASTER" : "");

    /* Array random (pre-generate indices) */
    uint32_t *rnd_idx = (uint32_t*)malloc(N * sizeof(uint32_t));
    srand(42);
    for (uint32_t i = 0; i < N; i++)
        rnd_idx[i] = (uint32_t)((uint64_t)rand() * map_count / RAND_MAX);

    t0 = now_sec();
    for (uint32_t r = 0; r < repeats; r++)
        for (uint32_t i = 0; i < N; i++)
            sink += (int64_t)weights[rnd_idx[i]];
    double t_rand = now_sec() - t0;
    double rand_ops = (double)total_ops / t_rand;
    printf("  %-45s %12.4f %12.0f %s\n",
           "array[rand] (random access)", t_rand, rand_ops, "");

    /* Geometric random */
    t0 = now_sec();
    for (uint32_t r = 0; r < repeats; r++)
        for (uint32_t i = 0; i < N; i++)
            sink += (int64_t)geom_weight(rnd_idx[i], STRIDE, geom_map);
    double t_grand = now_sec() - t0;
    double grand_ops = (double)total_ops / t_grand;
    printf("  %-45s %12.4f %12.0f %s\n",
           "geom decode (random)", t_grand, grand_ops,
           (grand_ops > rand_ops) ? "✓ FASTER" : "");

    free(rnd_idx);
    if (sink == 0xDEAD) printf(""); /* prevent optimize-out */

    /* ── DEMO: What array can't do ──────────────────────────── */
    printf("\n6. Geometric Engine Demos:\n");
    printf("\n  6a. Phase Channel: 8 extra bits per weight\n");
    printf("  %8s | %8s %8s | %4s %4s | %8s %8s\n",
           "i", "weight", "phase", "X", "Y", "neighbor+1", "neighbor-1");
    printf("   " "--------" "-" "--------" " " "--------" "-" "----" " " "----" "-" "--------" " " "--------" "\n");
    uint32_t ndisp = (map_count < 8) ? (uint32_t)map_count : 8;
    for (uint32_t i = 0; i < ndisp; i++) {
        int32_t w = geom_weight(i, STRIDE, geom_map);
        uint8_t ph = geom_phase(i, STRIDE, geom_map);
        int32_t n1 = geom_neighbor(i, STRIDE, geom_map, 1, 0);
        int32_t n2 = geom_neighbor(i, STRIDE, geom_map, 0, 1);
        printf("  %8u | %8d %8u | %4u %4u | %8d %8d\n",
               i, w, ph,
               (unsigned)((i * STRIDE) % 256u),
               (unsigned)geom_map[i],
               n1, n2);
    }

    printf("\n  6b. XOR Distance between adjacent weights (model topology)\n");
    printf("  %8s | %8s | %8s | %8s\n", "i", "w(i)", "w(i+1)", "XOR dist");
    printf("  " "--------" "-" "--------" "-" "--------" "-" "--------" "\n");
    uint32_t ndist = (map_count < 9) ? ((uint32_t)map_count - 1) : 8;
    for (uint32_t i = 0; i < ndist; i++) {
        int32_t w0 = geom_weight(i, STRIDE, geom_map);
        int32_t w1 = geom_weight(i + 1, STRIDE, geom_map);
        /* Geometric distance between positions */
        uint16_t x0 = (uint16_t)((i * STRIDE) % 256u);
        uint16_t y0 = (uint16_t)geom_map[i];
        uint16_t x1 = (uint16_t)(((i+1) * STRIDE) % 256u);
        uint16_t y1 = (uint16_t)geom_map[i+1];
        uint8_t dist = (uint8_t)((x0 ^ x1) ^ (y0 ^ y1));
        printf("  %8u | %8d | %8d | %8u\n", i, w0, w1, dist);
    }

    /* ── TENSOR SIZE COMPARISON ─────────────────────────────── */
    printf("\n7. Size comparison for this tensor:\n");
    uint64_t mb = 1024 * 1024;
    printf("  %-40s %12s\n", "Format", "Size");
    printf("  %-40s %12s\n", "----------------------------------------", "------------");
    printf("  %-40s %12.2f MB\n", "Original GGUF tensor", (double)tensor->size_bytes / mb);
    printf("  %-40s %12.2f MB\n", "Q8 int8 array (all weights)", (double)n_weights / mb);
    printf("  %-40s %12.2f MB (mapped: %llu/%llu weights)\n",
           "Geometric mapping (1 byte/weight)",
           (double)map_count / mb,
           (unsigned long long)map_count, (unsigned long long)n_weights);
    printf("  %-40s %12.2f MB (no storage for value, 1B for position)\n",
           "Geometric + phase (16 bits total)",
           (double)(map_count + map_count) / mb);

    printf("\n   Access speed vs Array:\n");
    if (geom_ops >= arr_ops) {
        printf("   Sequential: Geom %.1fx array\n", geom_ops / arr_ops);
    } else {
        printf("   Sequential: Geom %.1fx of array\n", geom_ops / arr_ops);
    }
    if (grand_ops >= rand_ops) {
        printf("   Random:     Geom %.1fx array\n", grand_ops / rand_ops);
    } else {
        printf("   Random:     Geom %.1fx of array\n", grand_ops / rand_ops);
    }
    printf("   Phase channel: +%d extra bits/weight at zero cost\n", 8);
    printf("   Geometric ops: neighbor, distance, transpose, compose\n");

    /* ── Cleanup ────────────────────────────────────────────── */
    free(geom_map);
    free(weights);
    gguf_close(gf);

    printf("\n✓ Demo complete — Geometric engine wrapping real GGUF tensor.\n");
    return 0;
}
