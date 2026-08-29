/*
 * tools/kv_geo_addr_test.c — Geometric Addressing on KV Buffer
 *
 * Maps DWGLS geometric coordinates (144 slots) to KV buffer positions
 * using stride-37 walk (coprime to 144, uniform coverage).
 *
 * KEY INSIGHT: geometry IS the address space.
 *   geo_slot g ∈ [0,144) → KV position = scatter(g) ∈ [n_pos+1, n_ctx)
 *
 * Build (MSYS2 MinGW — copy from kv_container_test pattern):
 *   gcc -O2 -std=c11 -Wno-unused-parameter -Wno-sign-compare -Wno-format \
 *     -Icore -II:/llama/include -o build/kv_geo_addr_test.exe \
 *     tools/kv_geo_addr_test.c \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm
 * Run:
 *   set PATH=C:\msys64\mingw64\bin;I:\llama\llama-b9733-bin-win-vulkan-x64;%PATH%
 *   .\build\kv_geo_addr_test.exe I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf "Hello"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "llama.h"
#include "ggml.h"

/* ============================================================
 * Geometric Addressing Constants
 * ============================================================ */

#define GEO_SLOTS      144   /* 6ico compound: 144 vertices */
#define STRIDE_37      37    /* universal scatter stride */
#define MAX_LAYERS     32

/* ============================================================
 * Geometric scatter: geo slot → KV position
 * gcd(37, 144) = 1 → uniform coverage, no collisions
 * ============================================================ */

static int geo_to_kv(int geo_slot, int dead_start, int dead_count) {
    return dead_start + (geo_slot * STRIDE_37) % dead_count;
}

static int kv_to_geo(int kv_pos, int dead_start, int dead_count) {
    int rel = kv_pos - dead_start;
    /* inverse of 37 mod dead_count */
    int inv = 1;
    for (int i = 1; i < dead_count; i++) {
        if ((37 * i) % dead_count == 1) { inv = i; break; }
    }
    return (rel * inv) % dead_count;
}

/* ============================================================
 * Model context (matching kv_container_test.c API)
 * ============================================================ */

static struct llama_model *g_model;
static struct llama_vocab *g_vocab;

typedef struct {
    struct llama_context *ctx;
    llama_memory_t mem;
    int n_pos, n_layers;
    size_t k_stride;
} run_t;

static run_t make_run(const char *prompt) {
    run_t r = {0};
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.n_ubatch = 512;
    r.ctx = llama_init_from_model(g_model, cp);
    if (!r.ctx) return r;
    r.mem = llama_get_memory(r.ctx);
    r.n_layers = (int)llama_memory_kv_cache_get_n_layers(r.mem);
    struct ggml_tensor *k0 = llama_memory_kv_cache_get_layer_k(r.mem, 0);
    if (k0) r.k_stride = k0->nb[1];

    llama_token tok[1024];
    int n = llama_tokenize(g_vocab, prompt, strlen(prompt), tok, 1024, true, false);
    if (n < 0) { llama_free(r.ctx); r.ctx = NULL; return r; }
    struct llama_batch batch = llama_batch_init(n, 0, 1);
    for (int i = 0; i < n; i++) {
        batch.token[i] = tok[i]; batch.pos[i] = i;
        batch.n_seq_id[i] = 1; batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n - 1) ? 1 : 0;
    }
    batch.n_tokens = n;
    if (llama_decode(r.ctx, batch) != 0) {
        llama_batch_free(batch); llama_free(r.ctx); r.ctx = NULL; return r;
    }
    r.n_pos = n;
    llama_batch_free(batch);
    return r;
}

static float *do_decode(run_t *r) {
    struct llama_batch batch = llama_batch_init(1, 0, 1);
    batch.token[0] = llama_vocab_eos(g_vocab);
    batch.pos[0] = r->n_pos;
    batch.n_seq_id[0] = 1; batch.seq_id[0][0] = 0;
    batch.logits[0] = 1; batch.n_tokens = 1;
    if (llama_decode(r->ctx, batch) != 0) { llama_batch_free(batch); return NULL; }
    r->n_pos++;
    int nv = llama_vocab_n_tokens(g_vocab);
    float *out = (float *)malloc(nv * sizeof(float));
    memcpy(out, llama_get_logits(r->ctx), nv * sizeof(float));
    llama_batch_free(batch);
    return out;
}

/* ============================================================
 * KV buffer read/write (direct pointer access)
 * ============================================================ */

static void write_at_pos(run_t *r, int kv_pos, const uint8_t *data, size_t len) {
    for (int li = 0; li < r->n_layers; li++) {
        struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(r->mem, li);
        struct ggml_tensor *v = llama_memory_kv_cache_get_layer_v(r->mem, li);
        if (!k || !v || !k->data || !v->data) continue;
        size_t off = (size_t)kv_pos * k->nb[1];
        size_t sl = k->nb[1]; /* stride per position */
        for (size_t b = 0; b < sl && b < len; b++) {
            ((uint8_t *)k->data)[off + b] = data[b];
            ((uint8_t *)v->data)[off + b] = data[b] ^ 0xFF;
        }
    }
}

static void read_at_pos(run_t *r, int kv_pos, uint8_t *out, size_t len, int layer) {
    struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(r->mem, layer);
    if (!k || !k->data) return;
    size_t off = (size_t)kv_pos * k->nb[1];
    size_t sl = k->nb[1];
    for (size_t b = 0; b < sl && b < len; b++) {
        out[b] = ((uint8_t *)k->data)[off + b];
    }
}

/* ============================================================
 * TEST 1: Inverse mapping — kv_to_geo(geo_to_kv(x)) == x
 * ============================================================ */

static int test_inverse_mapping(int dead_start, int dead_count) {
    printf("\n=== TEST 1: Inverse mapping (144 slots) ===\n");
    int ok = 0;
    for (int g = 0; g < GEO_SLOTS; g++) {
        int kv = geo_to_kv(g, dead_start, dead_count);
        int g2 = kv_to_geo(kv, dead_start, dead_count);
        if (g2 == g) ok++;
        else printf("  FAIL: geo=%d -> kv=%d -> geo=%d\n", g, kv, g2);
    }
    printf("  Result: %d/144 OK\n", ok);
    return ok < GEO_SLOTS;
}

/* ============================================================
 * TEST 2: Collision check — all 144 geo slots map to unique positions
 * ============================================================ */

static int test_no_collisions(int dead_start, int dead_count) {
    printf("\n=== TEST 2: No collisions (144 unique positions) ===\n");
    int used[2048];
    memset(used, 0, sizeof(used));
    int collisions = 0;
    for (int g = 0; g < GEO_SLOTS; g++) {
        int kv = geo_to_kv(g, dead_start, dead_count);
        if (kv < dead_start || kv >= dead_start + dead_count) {
            printf("  OOB: geo %d -> kv %d\n", g, kv);
            collisions++;
        } else if (used[kv]) {
            printf("  COLLISION: geo %d -> kv %d (already used)\n", g, kv);
            collisions++;
        }
        if (kv >= dead_start && kv < dead_start + dead_count)
            used[kv] = 1;
    }
    printf("  Result: %d collisions\n", collisions);
    return collisions > 0;
}

/* ============================================================
 * TEST 3: Encode 144 geo slots → lossless readback
 * ============================================================ */

static int test_geo_roundtrip(run_t *r) {
    printf("\n=== TEST 3: Geo roundtrip (encode 144 slots, read back) ===\n");
    int dead_start = r->n_pos + 1;
    int dead_count = (int)2048 - dead_start;

    /* Encode unique pattern per geo slot */
    for (int g = 0; g < GEO_SLOTS; g++) {
        uint8_t pat[64];
        for (int j = 0; j < 64; j++)
            pat[j] = (uint8_t)((g * 7 + j * 13) & 0xFF);
        int kv = geo_to_kv(g, dead_start, dead_count);
        write_at_pos(r, kv, pat, 64);
    }

    /* Read back from layer 0 */
    int ok = 0;
    for (int g = 0; g < GEO_SLOTS; g++) {
        uint8_t expected[64], actual[64];
        for (int j = 0; j < 64; j++)
            expected[j] = (uint8_t)((g * 7 + j * 13) & 0xFF);
        int kv = geo_to_kv(g, dead_start, dead_count);
        memset(actual, 0, 64);
        read_at_pos(r, kv, actual, 64, 0);
        if (memcmp(expected, actual, 64) == 0) ok++;
    }
    printf("  Result: %d/144 lossless\n", ok);
    return ok < GEO_SLOTS;
}

/* ============================================================
 * TEST 4: Inference unchanged after geometric encoding
 * ============================================================ */

static int test_inference_unchanged(const char *prompt) {
    printf("\n=== TEST 4: Inference unchanged after geo encoding ===\n");

    /* Context A: baseline — no encoding, decode once */
    run_t ra = make_run(prompt);
    float *before = do_decode(&ra);
    llama_free(ra.ctx);

    /* Context B: same prompt, encode 144 geo slots, decode once */
    run_t rb = make_run(prompt);
    int dead_start = rb.n_pos + 1;
    int dead_count = (int)2048 - dead_start;
    for (int g = 0; g < GEO_SLOTS; g++) {
        uint8_t pat[64];
        for (int j = 0; j < 64; j++)
            pat[j] = (uint8_t)((g * 11 + j * 3) & 0xFF);
        int kv = geo_to_kv(g, dead_start, dead_count);
        write_at_pos(&rb, kv, pat, 64);
    }
    float *after = do_decode(&rb);
    llama_free(rb.ctx);

    int nv = llama_vocab_n_tokens(g_vocab);
    double maxdiff = 0;
    for (int i = 0; i < nv; i++) {
        double d = fabs((double)before[i] - (double)after[i]);
        if (d > maxdiff) maxdiff = d;
    }
    printf("  Max logit diff: %.10f\n", maxdiff);
    printf("  Result: %s\n", maxdiff < 1e-6 ? "PASS" : "FAIL");

    free(before);
    free(after);
    return maxdiff >= 1e-6;
}

/* ============================================================
 * TEST 5: Multi-layer encoding — write + read ALL layers
 * ============================================================ */

static int test_multi_layer(run_t *r) {
    printf("\n=== TEST 5: Multi-layer encoding (all %d layers) ===\n", r->n_layers);
    int dead_start = r->n_pos + 1;
    int dead_count = (int)2048 - dead_start;

    /* Write different pattern per layer per geo slot */
    for (int li = 0; li < r->n_layers; li++) {
        for (int g = 0; g < GEO_SLOTS && g < 10; g++) {
            uint8_t pat[64];
            for (int j = 0; j < 64; j++)
                pat[j] = (uint8_t)((li * 37 + g * 7 + j * 13) & 0xFF);
            int kv = geo_to_kv(g, dead_start, dead_count);
            struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(r->mem, li);
            if (!k || !k->data) continue;
            size_t off = (size_t)kv * k->nb[1];
            memcpy((uint8_t *)k->data + off, pat, 64 < k->nb[1] ? 64 : k->nb[1]);
        }
    }

    /* Read back ALL layers */
    int ok = 0, total = 0;
    for (int li = 0; li < r->n_layers; li++) {
        for (int g = 0; g < GEO_SLOTS && g < 10; g++) {
            uint8_t expected[64], actual[64];
            for (int j = 0; j < 64; j++)
                expected[j] = (uint8_t)((li * 37 + g * 7 + j * 13) & 0xFF);
            int kv = geo_to_kv(g, dead_start, dead_count);
            read_at_pos(r, kv, actual, 64, li);
            total++;
            if (memcmp(expected, actual, 64) == 0) ok++;
        }
    }
    printf("  Result: %d/%d per-layer lossless\n", ok, total);
    return ok < total;
}

/* ============================================================ */

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <model.gguf> <prompt>\n", argv[0]);
        return 1;
    }

    printf("=== kv_geo_addr_test — Geometric Addressing on KV Buffer ===\n");
    printf("model: %s\n", argv[1]);
    printf("GEO_SLOTS=%d STRIDE=%d\n", GEO_SLOTS, STRIDE_37);

    llama_backend_init();
    g_model = llama_model_load_from_file(argv[1], llama_model_default_params());
    if (!g_model) { fprintf(stderr, "FAIL: load model\n"); return 1; }
    g_vocab = (struct llama_vocab *)llama_model_get_vocab(g_model);

    run_t r = make_run(argv[2]);
    if (!r.ctx) { fprintf(stderr, "FAIL: make_run\n"); return 1; }

    int dead_start = r.n_pos + 1;
    int dead_count = (int)2048 - dead_start;
    printf("Model: n_ctx=2048 n_layers=%d k_stride=%zu n_pos=%d dead=[%d,%d)=%d slots\n",
           r.n_layers, r.k_stride, r.n_pos, dead_start, 2048, dead_count);

    int fails = 0;
    fails += test_inverse_mapping(dead_start, dead_count);
    fails += test_no_collisions(dead_start, dead_count);
    fails += test_geo_roundtrip(&r);
    fails += test_inference_unchanged(argv[2]);
    fails += test_multi_layer(&r);

    printf("\n========================================\n");
    printf("  kv_geo_addr_test: %d/5 FAILED\n", fails);
    printf("========================================\n");

    llama_free(r.ctx);
    llama_model_free(g_model);
    llama_backend_free();
    return fails > 0;
}
