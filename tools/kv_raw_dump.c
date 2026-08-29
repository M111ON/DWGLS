/*
 * tools/kv_raw_dump.c — Phase 3: Raw K/V Hook
 * ════════════════════════════════════════════════════════════════════════════
 * Dump real KV at the CACHE BUFFER level (not serialized state) via the new
 * llama.cpp C API: llama_memory_kv_cache_get_layer_{k,v}
 *
 * This is the KEY to delta ∝ events (not ∝ full state):
 * - Serialized state = 100-107% of context (state format shifts on every token)
 * - Raw KV cache buffers = only NEW tokens appended (append-only by design)
 *
 * Usage:
 *   ./build/kv_raw_dump <model.gguf> "prompt" <n_tokens>
 *
 * Output:
 *   - Per-layer K/V tensor info (shape, type, nbytes)
 *   - Total KV cache size
 *   - Raw pointer addresses for zero-copy mapping
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

#include "../core/gguf_reader.h"
#include "llama.h"

int main(int argc, char **argv) {
    llama_backend_init();
    const char *model_path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";
    int n_gen = argc > 3 ? atoi(argv[3]) : 24;

    printf("=== kv_raw_dump — raw K/V at cache buffer level ===\n");
    printf("model: %s\n", model_path);
    printf("prompt: \"%s\"\n", prompt);
    printf("n_gen: %d\n\n", n_gen);

    // Load model
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;  // CPU only
    mparams.use_mmap = true;

    fprintf(stderr, "Loading model...\n");
    fflush(stderr);
    struct llama_model *model = llama_model_load_from_file(model_path, mparams);
    fprintf(stderr, "Model loaded: %p\n", model);
    fflush(stderr);
    if (!model) {
        printf("FAIL: load model\n");
        return 1;
    }

    // Create context
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    cparams.n_batch = 512;
    cparams.n_ubatch = 512;

    fprintf(stderr, "Initializing context...\n");
    fflush(stderr);
    struct llama_context *ctx = llama_init_from_model(model, cparams);
    fprintf(stderr, "Context: %p\n", ctx);
    fflush(stderr);

    // Get KV cache memory
    llama_memory_t mem = llama_get_memory(ctx);
    if (!mem) {
        printf("FAIL: no memory\n");
        llama_free(ctx);
        llama_model_free(model);
        return 1;
    }

    // Tokenize prompt
    struct llama_vocab *vocab = (struct llama_vocab *)llama_model_get_vocab(model);
    int n_tokens_max = 1024;
    llama_token *tokens = (llama_token *)malloc(n_tokens_max * sizeof(llama_token));
    int n_prompt = llama_tokenize(vocab, prompt, strlen(prompt), tokens, n_tokens_max, true, false);
    if (n_prompt < 0) {
        printf("FAIL: tokenize (need %d tokens)\n", -n_prompt);
        free(tokens);
        llama_free(ctx);
        llama_model_free(model);
        return 1;
    }
    printf("prompt tokens: %d\n", n_prompt);

    // Process prompt
    struct llama_batch batch = llama_batch_init(n_prompt, 0, 1);
    for (int i = 0; i < n_prompt; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i] = (llama_seq_id *)malloc(sizeof(llama_seq_id));
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n_prompt - 1) ? 1 : 0;
    }
    batch.n_tokens = n_prompt;

    printf("processing prompt...\n");
    double t0 = now_ms();
    if (llama_decode(ctx, batch) != 0) {
        printf("FAIL: decode prompt\n");
        free(tokens);
        llama_free(ctx);
        llama_model_free(model);
        return 1;
    }
    printf("prompt done: %.0f ms\n\n", now_ms() - t0);

    // Get KV cache layer info
    size_t n_layers = llama_memory_kv_cache_get_n_layers(mem);
    printf("KV cache layers: %zu\n", n_layers);

    size_t total_k_bytes = 0, total_v_bytes = 0;

    for (size_t i = 0; i < n_layers; i++) {
        int32_t il = llama_memory_kv_cache_get_layer_il(mem, i);
        struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(mem, i);
        struct ggml_tensor *v = llama_memory_kv_cache_get_layer_v(mem, i);

        if (!k || !v) {
            printf("  layer %zu (il=%d): NULL tensor\n", i, il);
            continue;
        }

        size_t k_bytes = ggml_nbytes(k);
        size_t v_bytes = ggml_nbytes(v);
        total_k_bytes += k_bytes;
        total_v_bytes += v_bytes;

        printf("  layer %zu (il=%d): K[%s] %zu B · V[%s] %zu B\n",
               i, il, ggml_type_name(k->type), k_bytes,
               ggml_type_name(v->type), v_bytes);
        printf("    K shape: [%lld, %lld, %lld, %lld] · V shape: [%lld, %lld, %lld, %lld]\n",
               k->ne[0], k->ne[1], k->ne[2], k->ne[3],
               v->ne[0], v->ne[1], v->ne[2], v->ne[3]);
        printf("    K ptr: %p · V ptr: %p\n", k->data, v->data);
    }

    printf("\nTotal KV cache: K=%zu MB · V=%zu MB · TOTAL=%.2f MB\n",
           total_k_bytes / (1024*1024),
           total_v_bytes / (1024*1024),
           (total_k_bytes + total_v_bytes) / (1024.0*1024.0));

    // Generate tokens and show KV growth
    printf("\n=== generating %d tokens ===\n", n_gen);
    int n_generated = 0;
    for (int step = 0; step < n_gen; step++) {
        struct llama_batch gen_batch = llama_batch_init(1, 0, 1);
        gen_batch.token[0] = (n_generated == 0) ? tokens[n_prompt - 1] : 0; // will be filled by sampler
        gen_batch.pos[0] = n_prompt + n_generated;
        gen_batch.n_seq_id[0] = 1;
        gen_batch.seq_id[0] = (llama_seq_id *)malloc(sizeof(llama_seq_id));
        gen_batch.seq_id[0][0] = 0;
        gen_batch.logits[0] = 1;
        gen_batch.n_tokens = 1;

        // Get logits and sample (greedy)
        if (llama_decode(ctx, gen_batch) != 0) {
            printf("FAIL: decode step %d\n", step);
            break;
        }

        float *logits = llama_get_logits(ctx);
        int n_vocab = llama_vocab_n_tokens(vocab);
        int best_token = 0;
        float best_logit = -1e9f;
        for (int i = 0; i < n_vocab; i++) {
            if (logits[i] > best_logit) {
                best_logit = logits[i];
                best_token = i;
            }
        }

        char *piece = (char *)malloc(32);
        llama_token_to_piece(vocab, best_token, piece, 32, 0, false);
        printf("%s", piece);
        fflush(stdout);
        free(piece);

        // Update for next iteration
        n_generated++;
    }
    printf("\n\n");

    // Show KV cache after generation
    printf("=== KV cache after %d tokens ===\n", n_generated);
    total_k_bytes = 0; total_v_bytes = 0;
    for (size_t i = 0; i < n_layers; i++) {
        struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(mem, i);
        struct ggml_tensor *v = llama_memory_kv_cache_get_layer_v(mem, i);
        if (k) total_k_bytes += ggml_nbytes(k);
        if (v) total_v_bytes += ggml_nbytes(v);
    }
    printf("Total KV cache: K=%zu MB · V=%zu MB · TOTAL=%.2f MB\n",
           total_k_bytes / (1024*1024),
           total_v_bytes / (1024*1024),
           (total_k_bytes + total_v_bytes) / (1024.0*1024.0));

    // Cleanup
    free(tokens);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();

    printf("\nRESULT: Raw K/V cache pointers accessible via C API\n");
    return 0;
}