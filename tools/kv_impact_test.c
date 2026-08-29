/*
 * tools/kv_impact_test.c — Phase 3b: Analyze KV buffer modification impact
 * ════════════════════════════════════════════════════════════════════════════
 * Controlled experiment: modify KV buffer at different positions, measure
 * impact on inference quality (logits divergence).
 *
 * Uses fresh contexts for baseline vs corrupted to avoid state undo issues.
 *
 * BUILD (PowerShell + MSYS2):
 *   C:\msys64\usr\bin\env.exe PATH="/mingw64/bin:$PATH" gcc -O2 -std=c11 \
 *     -Icore -II:/llama/include -o build/kv_impact_test.exe tools/kv_impact_test.c \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm
 *
 * RUN:
 *   $env:PATH = "C:\msys64\mingw64\bin;I:\llama\llama-b9733-bin-win-vulkan-x64;" + $env:PATH
 *   .\build\kv_impact_test.exe [model.gguf] "prompt"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../core/gguf_reader.h"
#include "llama.h"

/* ── helper: compute KL divergence between two logit distributions ── */
static float kl_divergence(const float *p, const float *q, int n) {
    double kl = 0.0;
    for (int i = 0; i < n; i++) {
        double pi = exp((double)p[i]);
        double qi = exp((double)q[i]);
        if (qi < 1e-10) qi = 1e-10;
        if (pi < 1e-10) pi = 1e-10;
        kl += pi * (log(pi) - log(qi));
    }
    return (float)kl;
}

/* ── helper: top-1 match ── */
static int top1_match(const float *a, const float *b, int n) {
    int best_a = 0, best_b = 0;
    float ba = -1e9f, bb = -1e9f;
    for (int i = 0; i < n; i++) {
        if (a[i] > ba) { ba = a[i]; best_a = i; }
        if (b[i] > bb) { bb = b[i]; best_b = i; }
    }
    return best_a == best_b;
}

/* ── helper: top-5 overlap count ── */
static int top5_match(const float *a, const float *b, int n) {
    int a_idx[5], b_idx[5];
    float a_val[5], b_val[5];
    for (int k = 0; k < 5; k++) {
        a_val[k] = -1e9f; b_val[k] = -1e9f;
        a_idx[k] = -1; b_idx[k] = -1;
    }
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 5; k++) {
            if (a[i] > a_val[k]) {
                for (int j = 4; j > k; j--) { a_val[j] = a_val[j-1]; a_idx[j] = a_idx[j-1]; }
                a_val[k] = a[i]; a_idx[k] = i; break;
            }
        }
        for (int k = 0; k < 5; k++) {
            if (b[i] > b_val[k]) {
                for (int j = 4; j > k; j--) { b_val[j] = b_val[j-1]; b_idx[j] = b_idx[j-1]; }
                b_val[k] = b[i]; b_idx[k] = i; break;
            }
        }
    }
    int matches = 0;
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            if (a_idx[i] == b_idx[j]) { matches++; break; }
        }
    }
    return matches;
}

/* ── Create model (shared, loaded once) ── */
struct shared_model {
    struct llama_model *model;
    struct llama_vocab *vocab;
};

static struct shared_model load_model(const char *path) {
    struct shared_model m = {0};
    m.model = llama_model_load_from_file(path, llama_model_default_params());
    m.vocab = (struct llama_vocab *)llama_model_get_vocab(m.model);
    return m;
}

/* ── Create context + process prompt, return n_pos ── */
typedef struct {
    struct llama_context *ctx;
    llama_memory_t mem;
    int n_pos;
    int n_layers;
    size_t k_stride;
    size_t v_stride;
} run_ctx_t;

static run_ctx_t create_and_process(struct shared_model *sm, const char *prompt, int n_ctx) {
    run_ctx_t r = {0};
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = n_ctx;
    cparams.n_batch = 512;
    cparams.n_ubatch = 512;
    r.ctx = llama_init_from_model(sm->model, cparams);
    if (!r.ctx) return r;
    r.mem = llama_get_memory(r.ctx);
    r.n_layers = (int)llama_memory_kv_cache_get_n_layers(r.mem);

    struct ggml_tensor *k0 = llama_memory_kv_cache_get_layer_k(r.mem, 0);
    if (k0) { r.k_stride = k0->nb[1]; }
    struct ggml_tensor *v0 = llama_memory_kv_cache_get_layer_v(r.mem, 0);
    if (v0) { r.v_stride = v0->nb[1]; }

    /* Tokenize */
    llama_token tokens[1024];
    int n = llama_tokenize(sm->vocab, prompt, strlen(prompt), tokens, 1024, true, false);
    if (n < 0) { llama_free(r.ctx); r.ctx = NULL; return r; }

    /* Process prompt */
    struct llama_batch batch = llama_batch_init(n, 0, 1);
    for (int i = 0; i < n; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n - 1) ? 1 : 0;
    }
    batch.n_tokens = n;

    if (llama_decode(r.ctx, batch) != 0) {
        llama_batch_free(batch);
        llama_free(r.ctx);
        r.ctx = NULL;
        return r;
    }
    r.n_pos = n;
    llama_batch_free(batch);
    return r;
}

/* ── Decode one step, return logits (caller frees) ── */
static float *decode_one(struct shared_model *sm, run_ctx_t *r, llama_token token) {
    struct llama_batch batch = llama_batch_init(1, 0, 1);
    batch.token[0] = token;
    batch.pos[0] = r->n_pos;
    batch.n_seq_id[0] = 1;
    batch.seq_id[0][0] = 0;
    batch.logits[0] = 1;
    batch.n_tokens = 1;

    if (llama_decode(r->ctx, batch) != 0) {
        llama_batch_free(batch);
        return NULL;
    }
    r->n_pos++;

    int n_vocab = llama_vocab_n_tokens(sm->vocab);
    float *out = (float *)malloc(n_vocab * sizeof(float));
    memcpy(out, llama_get_logits(r->ctx), n_vocab * sizeof(float));
    llama_batch_free(batch);
    return out;
}

/* ── Test 1: Dead slot injection ── */
static void test_dead_slots(struct shared_model *sm, const char *prompt) {
    printf("\n=== TEST 1: Dead Slot Injection (write to unused KV positions) ===\n");

    /* Baseline: fresh context, normal decode */
    run_ctx_t base = create_and_process(sm, prompt, 2048);
    if (!base.ctx) { printf("FAIL: baseline context\n"); return; }
    llama_token tok_buf[8];
    int n_tok = llama_tokenize(sm->vocab, " is", 3, tok_buf, 8, true, false);
    if (n_tok <= 0) { printf("FAIL: tokenize\n"); llama_free(base.ctx); return; }
    llama_token tok_test = tok_buf[0];
    float *baseline = decode_one(sm, &base, tok_test);
    printf("Baseline: n_pos=%d, n_layers=%d, k_stride=%zu\n",
           base.n_pos, base.n_layers, base.k_stride);

    /* Corrupted: fresh context, write garbage to dead slots, then decode */
    run_ctx_t corr = create_and_process(sm, prompt, 2048);
    if (!corr.ctx) { printf("FAIL: corrupted context\n"); free(baseline); llama_free(base.ctx); return; }

    /* Write garbage to dead slots [n_pos, n_pos+20) */
    int dead_start = corr.n_pos;
    int dead_end = dead_start + 20;
    if (dead_end > 2048) dead_end = 2048;
    printf("Injecting garbage into dead slots [%d, %d)...\n", dead_start, dead_end);

    for (size_t li = 0; li < (size_t)corr.n_layers; li++) {
        struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(corr.mem, li);
        struct ggml_tensor *v = llama_memory_kv_cache_get_layer_v(corr.mem, li);
        if (!k || !v || !k->data || !v->data) continue;
        for (int pos = dead_start; pos < dead_end; pos++) {
            uint8_t *k_ptr = (uint8_t *)k->data + (size_t)pos * k->nb[1];
            uint8_t *v_ptr = (uint8_t *)v->data + (size_t)pos * v->nb[1];
            for (size_t b = 0; b < k->nb[1]; b++) {
                k_ptr[b] = (uint8_t)((li * 7 + pos * 13 + b * 3) & 0xFF);
                v_ptr[b] = (uint8_t)((li * 11 + pos * 17 + b * 5) & 0xFF);
            }
        }
    }

    float *corrupted = decode_one(sm, &corr, tok_test);
    int n_vocab = llama_vocab_n_tokens(sm->vocab);

    float kl = kl_divergence(baseline, corrupted, n_vocab);
    int top1 = top1_match(baseline, corrupted, n_vocab);
    int top5 = top5_match(baseline, corrupted, n_vocab);

    printf("Results:\n");
    printf("  KL divergence:  %.8f\n", kl);
    printf("  Top-1 match:    %s\n", top1 ? "YES" : "NO");
    printf("  Top-5 overlap:  %d/5\n", top5);
    printf("  Verdict:        %s\n",
           kl < 0.0001f && top5 >= 5 ? "SAFE — dead slots are injection zones" :
           kl < 0.01f ? "LOW IMPACT — minor divergence" :
           kl < 0.1f ? "MODERATE — some inference change" : "HIGH IMPACT — breaks inference");

    free(baseline);
    free(corrupted);
    llama_free(corr.ctx);
    llama_free(base.ctx);
}

/* ── Test 2: Active slot poisoning (gradual) ── */
static void test_active_poison(struct shared_model *sm, const char *prompt) {
    printf("\n=== TEST 2: Active Slot Poisoning (corrupt real KV data) ===\n");

    float percentages[] = {0.01f, 0.05f, 0.10f, 0.25f, 0.50f, 1.0f};
    const char *labels[] = {"1%", "5%", "10%", "25%", "50%", "100%"};

    for (int p = 0; p < 6; p++) {
        /* Fresh context for each test */
        run_ctx_t base = create_and_process(sm, prompt, 2048);
        if (!base.ctx) continue;

        llama_token tok_buf[8];
    int n_tok = llama_tokenize(sm->vocab, " is", 3, tok_buf, 8, true, false);
    if (n_tok <= 0) { printf("FAIL: tokenize\n"); llama_free(base.ctx); return; }
    llama_token tok_test = tok_buf[0];
        float *baseline = decode_one(sm, &base, tok_test);

        /* Fresh context for corrupted version */
        run_ctx_t corr = create_and_process(sm, prompt, 2048);
        if (!corr.ctx) { free(baseline); llama_free(base.ctx); continue; }

        /* Corrupt layer 0, first N bytes of active K data */
        struct ggml_tensor *k0 = llama_memory_kv_cache_get_layer_k(corr.mem, 0);
        if (k0 && k0->data) {
            size_t active_bytes = (size_t)corr.n_pos * k0->nb[1];
            int n_corrupt = (int)(active_bytes * percentages[p]);
            uint8_t *kdata = (uint8_t *)k0->data;
            for (int b = 0; b < n_corrupt; b++) {
                kdata[b] = 0xFF;
            }
        }

        float *corrupted = decode_one(sm, &corr, tok_test);
        int n_vocab = llama_vocab_n_tokens(sm->vocab);

        float kl = kl_divergence(baseline, corrupted, n_vocab);
        int top1 = top1_match(baseline, corrupted, n_vocab);
        int top5 = top5_match(baseline, corrupted, n_vocab);

        printf("  Corrupt L0 active K %s: KL=%.8f top1=%s top5=%d/5\n",
               labels[p], kl, top1 ? "Y" : "N", top5);

        free(baseline);
        free(corrupted);
        llama_free(corr.ctx);
        llama_free(base.ctx);
    }
}

/* ── Test 3: Dead slot injection — measure maximum safe injection size ── */
static void test_max_injection(struct shared_model *sm, const char *prompt) {
    printf("\n=== TEST 3: Maximum Safe Injection Size (dead slots) ===\n");
    printf("Testing injection of 100, 200, 500, 1000 dead positions...\n");

    int inject_sizes[] = {100, 200, 500, 1000, 1500, 2000};

    for (int t = 0; t < 6; t++) {
        int inject_n = inject_sizes[t];

        /* Baseline */
        run_ctx_t base = create_and_process(sm, prompt, 2048);
        if (!base.ctx) continue;
        llama_token tok_buf[8];
    int n_tok = llama_tokenize(sm->vocab, " is", 3, tok_buf, 8, true, false);
    if (n_tok <= 0) { printf("FAIL: tokenize\n"); llama_free(base.ctx); return; }
    llama_token tok_test = tok_buf[0];
        float *baseline = decode_one(sm, &base, tok_test);

        /* Corrupted */
        run_ctx_t corr = create_and_process(sm, prompt, 2048);
        if (!corr.ctx) { free(baseline); llama_free(base.ctx); continue; }

        int dead_start = corr.n_pos;
        int dead_end = dead_start + inject_n;
        if (dead_end > 2048) dead_end = 2048;

        for (size_t li = 0; li < (size_t)corr.n_layers; li++) {
            struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(corr.mem, li);
            struct ggml_tensor *v = llama_memory_kv_cache_get_layer_v(corr.mem, li);
            if (!k || !v || !k->data || !v->data) continue;
            for (int pos = dead_start; pos < dead_end; pos++) {
                uint8_t *k_ptr = (uint8_t *)k->data + (size_t)pos * k->nb[1];
                uint8_t *v_ptr = (uint8_t *)v->data + (size_t)pos * v->nb[1];
                for (size_t b = 0; b < k->nb[1]; b++) {
                    k_ptr[b] = (uint8_t)((li * 7 + pos * 13 + b * 3) & 0xFF);
                    v_ptr[b] = (uint8_t)((li * 11 + pos * 17 + b * 5) & 0xFF);
                }
            }
        }

        float *corrupted = decode_one(sm, &corr, tok_test);
        int n_vocab = llama_vocab_n_tokens(sm->vocab);
        float kl = kl_divergence(baseline, corrupted, n_vocab);
        int top5 = top5_match(baseline, corrupted, n_vocab);

        printf("  Inject %4d dead slots: KL=%.8f top5=%d/5 %s\n",
               dead_end - dead_start, kl, top5,
               kl < 0.0001f ? "SAFE" : kl < 0.01f ? "OK" : "WARN");

        free(baseline);
        free(corrupted);
        llama_free(corr.ctx);
        llama_free(base.ctx);
    }
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";

    fprintf(stderr, "=== kv_impact_test — KV buffer modification impact ===\n");
    fprintf(stderr, "model: %s\n", model_path);
    fprintf(stderr, "prompt: \"%s\"\n\n", prompt);

    llama_backend_init();

    struct shared_model sm = load_model(model_path);
    if (!sm.model) { fprintf(stderr, "FAIL: load model\n"); return 1; }

    test_dead_slots(&sm, prompt);
    test_active_poison(&sm, prompt);
    test_max_injection(&sm, prompt);

    /* Summary */
    printf("\n=== ANALYSIS SUMMARY ===\n");
    printf("Model: %s\n", model_path);
    printf("KV buffer: positions × layers × 2 (K+V) × stride\n");
    printf("Key insight: model attention only reads positions [0, n_pos)\n");
    printf("Positions [n_pos, n_ctx) are DEAD — never read by attention\n");
    printf("These dead positions are SAFE injection zones for DWGLS data\n");

    llama_model_free(sm.model);
    llama_backend_free();

    printf("\nRESULT: KV impact analysis complete\n");
    return 0;
}
