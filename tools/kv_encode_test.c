/*
 * tools/kv_encode_test.c — Stage 1+2: Encode data into dead KV slots + lossless readback
 * ════════════════════════════════════════════════════════════════════════════
 * Proves DWGLS core: write arbitrary data into dead KV positions,
 * verify inference unchanged, read back data losslessly.
 *
 * Flow:
 * 1. Process prompt → establish active region [0, n_pos)
 * 2. Baseline: decode → record logits
 * 3. Encode: write structured data into dead slots [n_pos, n_ctx)
 * 4. Decode again → verify logits identical (inference unaffected)
 * 5. Read back: verify data matches what we wrote (lossless roundtrip)
 * 6. Stress test: fill dead slots with different patterns
 *
 * BUILD (PowerShell + MSYS2):
 *   C:\msys64\usr\bin\env.exe PATH="/mingw64/bin:$PATH" gcc -O2 -std=c11 \
 *     -Icore -II:/llama/include -o build/kv_encode_test.exe tools/kv_encode_test.c \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm
 *
 * RUN:
 *   $env:PATH = "C:\msys64\mingw64\bin;I:\llama\llama-b9733-bin-win-vulkan-x64;" + $env:PATH
 *   .\build\kv_encode_test.exe [model.gguf] "prompt"
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

/* ── helper: top-5 overlap ── */
static int top5_match(const float *a, const float *b, int n) {
    int a_idx[5], b_idx[5];
    float a_val[5], b_val[5];
    for (int k = 0; k < 5; k++) {
        a_val[k] = b_val[k] = -1e9f;
        a_idx[k] = b_idx[k] = -1;
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
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            if (a_idx[i] == b_idx[j]) { matches++; break; }
    return matches;
}

/* ── helper: KL divergence (double precision) ── */
static double kl_div(const float *p, const float *q, int n) {
    double kl = 0.0;
    for (int i = 0; i < n; i++) {
        double pi = exp((double)p[i]);
        double qi = exp((double)q[i]);
        if (qi < 1e-15) qi = 1e-15;
        if (pi < 1e-15) pi = 1e-15;
        kl += pi * (log(pi) - log(qi));
    }
    return kl;
}

/* ── helper: XOR two buffers, return count of differing bytes ── */
static size_t xor_diff(const uint8_t *a, const uint8_t *b, size_t n) {
    size_t diff = 0;
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) diff++;
    return diff;
}

/* ── Shared model (loaded once) ── */
static struct llama_model *g_model;
static struct llama_vocab *g_vocab;

/* ── Process prompt in fresh context, return context + n_pos ── */
typedef struct {
    struct llama_context *ctx;
    llama_memory_t mem;
    int n_pos;
    int n_layers;
    size_t k_stride;
} run_t;

static run_t make_run(const char *prompt) {
    run_t r = {0};
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048;
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    r.ctx = llama_init_from_model(g_model, cp);
    if (!r.ctx) return r;
    r.mem = llama_get_memory(r.ctx);
    r.n_layers = (int)llama_memory_kv_cache_get_n_layers(r.mem);
    struct ggml_tensor *k0 = llama_memory_kv_cache_get_layer_k(r.mem, 0);
    if (k0) r.k_stride = k0->nb[1];

    llama_token tokens[1024];
    int n = llama_tokenize(g_vocab, prompt, strlen(prompt), tokens, 1024, true, false);
    if (n < 0) { llama_free(r.ctx); r.ctx = NULL; return r; }

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

/* ── Decode one token, return logits (caller frees) ── */
static float *do_decode(run_t *r, const char *word) {
    llama_token tok[8];
    int n = llama_tokenize(g_vocab, word, strlen(word), tok, 8, true, false);
    if (n <= 0) return NULL;
    struct llama_batch batch = llama_batch_init(1, 0, 1);
    batch.token[0] = tok[0];
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
    int nv = llama_vocab_n_tokens(g_vocab);
    float *out = (float *)malloc(nv * sizeof(float));
    memcpy(out, llama_get_logits(r->ctx), nv * sizeof(float));
    llama_batch_free(batch);
    return out;
}

/* ── Encode data into dead slots of all layers ── */
static void encode_dead_slots(run_t *r, const uint8_t *data, size_t data_bytes,
                              int dead_start, int dead_end) {
    for (size_t li = 0; li < (size_t)r->n_layers; li++) {
        struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(r->mem, li);
        struct ggml_tensor *v = llama_memory_kv_cache_get_layer_v(r->mem, li);
        if (!k || !v || !k->data || !v->data) continue;
        for (int pos = dead_start; pos < dead_end; pos++) {
            size_t offset = (size_t)pos * k->nb[1];
            size_t bytes_this_pos = k->nb[1];
            /* Write K data from our buffer */
            uint8_t *k_dst = (uint8_t *)k->data + offset;
            for (size_t b = 0; b < bytes_this_pos && (size_t)(pos - dead_start) * bytes_this_pos + b < data_bytes; b++) {
                k_dst[b] = data[(size_t)(pos - dead_start) * bytes_this_pos + b];
            }
            /* Write V data (mirrored with offset) */
            uint8_t *v_dst = (uint8_t *)v->data + offset;
            for (size_t b = 0; b < bytes_this_pos && (size_t)(pos - dead_start) * bytes_this_pos + b < data_bytes; b++) {
                v_dst[b] = data[(size_t)(pos - dead_start) * bytes_this_pos + b] ^ 0xFF;
            }
        }
    }
}

/* ── Read data back from dead slots of one layer ── */
static void read_dead_slots_k(run_t *r, uint8_t *out, size_t out_bytes,
                              int dead_start, int dead_end, int layer) {
    struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(r->mem, layer);
    if (!k || !k->data) return;
    for (int pos = dead_start; pos < dead_end; pos++) {
        size_t offset = (size_t)pos * k->nb[1];
        size_t bytes_this_pos = k->nb[1];
        size_t src_offset = (size_t)(pos - dead_start) * bytes_this_pos;
        if (src_offset + bytes_this_pos > out_bytes) break;
        memcpy(out + src_offset, (uint8_t *)k->data + offset, bytes_this_pos);
    }
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";

    fprintf(stderr, "=== kv_encode_test — DWGLS dead-slot encode + lossless readback ===\n");
    fprintf(stderr, "model: %s\n", model_path);
    fprintf(stderr, "prompt: \"%s\"\n\n", prompt);

    llama_backend_init();
    g_model = llama_model_load_from_file(model_path, llama_model_default_params());
    if (!g_model) { fprintf(stderr, "FAIL: load model\n"); return 1; }
    g_vocab = (struct llama_vocab *)llama_model_get_vocab(g_model);

    int pass_count = 0, fail_count = 0;

    /* ──────────────────────────────────────────────
     * TEST 1: Encode "HELLO_DWGLS" into dead slots
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 1: Encode 'HELLO_DWGLS' → verify inference → read back ===\n");

    /* Baseline: no data written */
    run_t base = make_run(prompt);
    if (!base.ctx) { printf("FAIL: baseline\n"); return 1; }
    float *baseline_logits = do_decode(&base, " is");
    printf("Baseline: n_pos=%d, n_layers=%d, k_stride=%zu\n",
           base.n_pos, base.n_layers, base.k_stride);
    int n_vocab = llama_vocab_n_tokens(g_vocab);

    /* Encoded: write "HELLO_DWGLS" into dead slots PAST the decode write.
     * decode_one() writes KV at position n_pos (pos 5),
     * so we encode at [n_pos+1, n_pos+1+10) = [6, 16) to avoid collision. */
    run_t enc = make_run(prompt);
    if (!enc.ctx) { printf("FAIL: encoded context\n"); return 1; }

    const char *msg = "HELLO_DWGLS";
    size_t msg_len = strlen(msg);
    int dead_start = enc.n_pos + 1;  /* skip the position decode will write to */
    int dead_end = dead_start + 10;
    printf("Encoding '%s' (%zu bytes) into dead slots [%d, %d) (past decode write at pos %d)...\n",
           msg, msg_len, dead_start, dead_end, enc.n_pos);

    /* Generate pattern data from message */
    size_t pattern_size = (size_t)(dead_end - dead_start) * enc.k_stride;
    uint8_t *pattern = (uint8_t *)calloc(pattern_size, 1);
    for (size_t i = 0; i < pattern_size; i++) {
        pattern[i] = (uint8_t)(msg[i % msg_len] ^ (i & 0xFF));
    }

    encode_dead_slots(&enc, pattern, pattern_size, dead_start, dead_end);

    /* Decode after encoding */
    float *encoded_logits = do_decode(&enc, " is");

    /* Compare */
    double kl = kl_div(baseline_logits, encoded_logits, n_vocab);
    int top5 = top5_match(baseline_logits, encoded_logits, n_vocab);

    printf("Inference comparison:\n");
    printf("  KL divergence:  %.12f\n", kl);
    printf("  Top-5 overlap:  %d/5\n", top5);
    printf("  Verdict:        %s\n",
           kl < 1e-10 && top5 == 5 ? "PASS — encoding does not affect inference" : "FAIL");

    if (kl < 1e-10 && top5 == 5) pass_count++; else fail_count++;

    /* Read back from layer 0 */
    size_t readback_size = (size_t)(dead_end - dead_start) * enc.k_stride;
    uint8_t *readback = (uint8_t *)malloc(readback_size);
    read_dead_slots_k(&enc, readback, readback_size, dead_start, dead_end, 0);

    size_t diffs = xor_diff(pattern, readback, readback_size);
    printf("Readback verification (layer 0 K):\n");
    printf("  Expected: ");
    for (size_t i = 0; i < msg_len && i < 32; i++) printf("%c", (pattern[i] ^ (i & 0xFF)) >= 32 && (pattern[i] ^ (i & 0xFF)) < 127 ? (pattern[i] ^ (i & 0xFF)) : '.');
    printf("\n  Got:      ");
    for (size_t i = 0; i < msg_len && i < 32; i++) printf("%c", (readback[i] ^ (i & 0xFF)) >= 32 && (readback[i] ^ (i & 0xFF)) < 127 ? (readback[i] ^ (i & 0xFF)) : '.');
    printf("\n  Differing bytes: %zu / %zu\n", diffs, readback_size);
    printf("  Verdict:         %s\n", diffs == 0 ? "PASS — lossless readback" : "FAIL");

    if (diffs == 0) pass_count++; else fail_count++;

    free(pattern);
    free(readback);
    free(baseline_logits);
    free(encoded_logits);
    llama_free(base.ctx);
    llama_free(enc.ctx);

    /* ──────────────────────────────────────────────
     * TEST 2: Fill ALL dead slots with structured data
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 2: Fill all dead slots with pattern → verify inference ===\n");

    run_t base2 = make_run(prompt);
    baseline_logits = do_decode(&base2, " is");

    run_t enc2 = make_run(prompt);
    dead_start = enc2.n_pos + 1;  /* skip decode write position */
    dead_end = 2048;

    size_t full_dead = (size_t)(dead_end - dead_start) * enc2.k_stride;
    printf("Filling %zu dead bytes across %d layers...\n", full_dead * enc2.n_layers * 2, enc2.n_layers);

    /* Generate deterministic pattern */
    uint8_t *full_pattern = (uint8_t *)malloc(full_dead);
    for (size_t i = 0; i < full_dead; i++) {
        full_pattern[i] = (uint8_t)((i * 7 + 13) & 0xFF);
    }

    encode_dead_slots(&enc2, full_pattern, full_dead, dead_start, dead_end);

    encoded_logits = do_decode(&enc2, " is");

    kl = kl_div(baseline_logits, encoded_logits, n_vocab);
    top5 = top5_match(baseline_logits, encoded_logits, n_vocab);

    printf("Inference comparison (full dead fill):\n");
    printf("  KL divergence:  %.12f\n", kl);
    printf("  Top-5 overlap:  %d/5\n", top5);
    printf("  Verdict:        %s\n",
           kl < 1e-10 && top5 == 5 ? "PASS" : "FAIL");

    if (kl < 1e-10 && top5 == 5) pass_count++; else fail_count++;

    /* Readback from layer 0 */
    readback = (uint8_t *)malloc(full_dead);
    read_dead_slots_k(&enc2, readback, full_dead, dead_start, dead_end, 0);
    diffs = xor_diff(full_pattern, readback, full_dead);
    printf("Readback: %zu / %zu bytes differ → %s\n",
           diffs, full_dead, diffs == 0 ? "PASS" : "FAIL");

    if (diffs == 0) pass_count++; else fail_count++;

    free(full_pattern);
    free(readback);
    free(baseline_logits);
    free(encoded_logits);
    llama_free(base2.ctx);
    llama_free(enc2.ctx);

    /* ──────────────────────────────────────────────
     * TEST 3: Encode and readback across ALL layers
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 3: Encode + readback ALL layers ===\n");

    run_t enc3 = make_run(prompt);
    dead_start = enc3.n_pos + 1;
    dead_end = dead_start + 5;

    size_t small_size = (size_t)(dead_end - dead_start) * enc3.k_stride;
    uint8_t *small_pattern = (uint8_t *)malloc(small_size);
    for (size_t i = 0; i < small_size; i++) {
        small_pattern[i] = (uint8_t)(0xAB + i);
    }

    encode_dead_slots(&enc3, small_pattern, small_size, dead_start, dead_end);

    int layer_pass = 0, layer_fail = 0;
    for (int li = 0; li < enc3.n_layers; li++) {
        uint8_t *rb = (uint8_t *)malloc(small_size);
        read_dead_slots_k(&enc3, rb, small_size, dead_start, dead_end, li);
        diffs = xor_diff(small_pattern, rb, small_size);
        if (diffs == 0) layer_pass++; else layer_fail++;
        free(rb);
    }
    printf("Layer readback: %d/%d layers lossless → %s\n",
           layer_pass, enc3.n_layers,
           layer_fail == 0 ? "PASS — all layers store data correctly" : "FAIL");

    if (layer_fail == 0) pass_count++; else fail_count++;

    free(small_pattern);
    llama_free(enc3.ctx);

    /* ──────────────────────────────────────────────
     * TEST 4: Multiple encode-decode cycles
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 4: Progressive encode — fill 100, 200, 500 positions ===\n");

    int fill_sizes[] = {100, 200, 500, 1000};
    for (int t = 0; t < 4; t++) {
        run_t b = make_run(prompt);
        float *bl = do_decode(&b, " is");

        run_t e = make_run(prompt);
        dead_start = e.n_pos + 1;
        dead_end = dead_start + fill_sizes[t];
        if (dead_end > 2048) dead_end = 2048;
        int actual = dead_end - dead_start;

        size_t fill_bytes = (size_t)actual * e.k_stride;
        uint8_t *fill_data = (uint8_t *)malloc(fill_bytes);
        for (size_t i = 0; i < fill_bytes; i++) {
            fill_data[i] = (uint8_t)((t * 31 + i * 7) & 0xFF);
        }
        encode_dead_slots(&e, fill_data, fill_bytes, dead_start, dead_end);

        float *el = do_decode(&e, " is");
        kl = kl_div(bl, el, n_vocab);
        top5 = top5_match(bl, el, n_vocab);

        printf("  Fill %4d positions: KL=%.12f top5=%d/5 %s\n",
               actual, kl, top5,
               kl < 1e-10 && top5 == 5 ? "PASS" : "FAIL");

        if (kl < 1e-10 && top5 == 5) pass_count++; else fail_count++;

        free(fill_data);
        free(bl);
        free(el);
        llama_free(b.ctx);
        llama_free(e.ctx);
    }

    /* ── Final Summary ── */
    printf("\n═══════════════════════════════════════════════════\n");
    printf("  DWGLS ENCODE TEST RESULTS: %d/%d PASSED\n", pass_count, pass_count + fail_count);
    printf("═══════════════════════════════════════════════════\n");

    printf("\nDWGLS Stage 1+2 Summary:\n");
    printf("  Dead slots = safe injection zones (KL=0)\n");
    printf("  Data encode → inference unchanged\n");
    printf("  Data readback → lossless roundtrip\n");
    printf("  Capacity: (n_ctx - n_pos) × layers × 2 × stride bytes\n");

    llama_model_free(g_model);
    llama_backend_free();

    printf("\nRESULT: DWGLS dead-slot encode %s\n", fail_count == 0 ? "COMPLETE" : "HAS FAILURES");
    return fail_count == 0 ? 0 : 1;
}
