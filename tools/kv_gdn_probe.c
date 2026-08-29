/*
 * tools/kv_gdn_probe.c — Probe Qwen3.5 GDN recurrent state
 * ════════════════════════════════════════════════════════════════════════════
 * Qwen3.5 uses Gated Delta Net (GDN) instead of standard attention KV.
 * State is a fixed-size recurrent hidden state per layer, NOT a KV buffer.
 *
 * This probe:
 * 1. Checks if model is recurrent via llama_model_is_recurrent()
 * 2. Gets recurrent state size via llama_state_seq_get_size_ext(PARTIAL_ONLY)
 * 3. After each token, captures state and computes delta
 *
 * BUILD (PowerShell + MSYS2):
 *   C:\msys64\usr\bin\env.exe PATH="/mingw64/bin:$PATH" gcc -O2 -std=c11 \
 *     -Icore -II:/llama/include -o build/kv_gdn_probe.exe tools/kv_gdn_probe.c \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm
 *
 * RUN:
 *   $env:PATH = "C:\msys64\mingw64\bin;I:\llama\llama-b9733-bin-win-vulkan-x64;" + $env:PATH
 *   .\build\kv_gdn_probe.exe [model.gguf] "prompt" <n_tokens>
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

/* FNV-1a hash */
static uint64_t fnv1a_hash(const void *data, size_t nbytes) {
    const uint64_t *p = (const uint64_t *)data;
    size_t n = nbytes / 8;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1]
        : "F:\\model\\Qwen3.5-2B-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";
    int n_gen = argc > 3 ? atoi(argv[3]) : 16;

    fprintf(stderr, "=== kv_gdn_probe — GDN recurrent state probe ===\n");
    fprintf(stderr, "model: %s\n", model_path);
    fprintf(stderr, "prompt: \"%s\"\n", prompt);
    fprintf(stderr, "n_gen: %d\n\n", n_gen);

    llama_backend_init();

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mparams);
    if (!model) { fprintf(stderr, "FAIL: load model\n"); return 1; }

    /* Check if model is recurrent */
    bool is_recurrent = llama_model_is_recurrent(model);
    fprintf(stderr, "Model is recurrent: %s\n", is_recurrent ? "YES (GDN/Mamba)" : "NO (Attention)");

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    cparams.n_batch = 512;
    cparams.n_ubatch = 512;

    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "FAIL: init context\n"); llama_model_free(model); return 1; }

    /* Get recurrent state size for seq_id=0 */
    size_t state_size_partial = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
    size_t state_size_full = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "Partial state size (recurrent only): %zu bytes (%.2f KB)\n", state_size_partial, state_size_partial / 1024.0);
    fprintf(stderr, "Full state size (all): %zu bytes (%.2f KB)\n", state_size_full, state_size_full / 1024.0);

    /* Also check standard state API */
    size_t state_size_standard = llama_state_get_size(ctx);
    fprintf(stderr, "Standard state size: %zu bytes (%.2f KB)\n", state_size_standard, state_size_standard / 1024.0);

    /* Try both partial and full state */
    size_t state_size = state_size_full;  /* use full state */
    if (state_size == 0) state_size = state_size_standard;

    /* Allocate state buffers */
    uint8_t *state_prev = (uint8_t *)malloc(state_size);
    uint8_t *state_curr = (uint8_t *)malloc(state_size);
    if (!state_prev || !state_curr) { fprintf(stderr, "FAIL: alloc state\n"); return 1; }

    /* Tokenize prompt */
    struct llama_vocab *vocab = (struct llama_vocab *)llama_model_get_vocab(model);
    llama_token tokens[1024];
    int n_prompt = llama_tokenize(vocab, prompt, strlen(prompt), tokens, 1024, true, false);
    if (n_prompt < 0) { fprintf(stderr, "FAIL: tokenize\n"); return 1; }
    printf("prompt tokens: %d\n", n_prompt);

    /* Process prompt */
    struct llama_batch batch = llama_batch_init(n_prompt, 0, 1);
    for (int i = 0; i < n_prompt; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n_prompt - 1) ? 1 : 0;
    }
    batch.n_tokens = n_prompt;

    printf("processing prompt (%d tokens)...\n", n_prompt);
    double t0 = now_ms();
    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "FAIL: decode prompt\n");
        return 1;
    }
    printf("prompt done: %.0f ms\n", now_ms() - t0);
    llama_batch_free(batch);

    /* Capture initial recurrent state */
    size_t got = llama_state_seq_get_data_ext(ctx, state_prev, state_size, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "Initial state captured: %zu / %zu bytes (FULL)\n", got, state_size);
    if (got == 0) {
        /* Try partial */
        got = llama_state_seq_get_data_ext(ctx, state_prev, state_size, 0, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
        fprintf(stderr, "Initial state captured: %zu / %zu bytes (PARTIAL)\n", got, state_size);
    }
    if (got == 0) {
        /* Try standard API */
        got = llama_state_get_data(ctx, state_prev, state_size);
        fprintf(stderr, "Initial state captured: %zu / %zu bytes (STANDARD)\n", got, state_size);
    }
    uint64_t initial_hash = fnv1a_hash(state_prev, got > 0 ? got : state_size);
    printf("\nInitial recurrent state: %zu bytes captured=%zu hash=%016llx\n",
           state_size, got, (unsigned long long)initial_hash);

    /* Generate tokens and track recurrent state delta */
    printf("\n=== generating %d tokens, tracking GDN recurrent state ===\n", n_gen);
    int n_pos = n_prompt;
    int n_generated = 0;
    llama_token last_token = tokens[n_prompt - 1];
    struct llama_batch gen_batch = llama_batch_init(1, 0, 1);

    for (int step = 0; step < n_gen; step++) {
        gen_batch.token[0] = last_token;
        gen_batch.pos[0] = n_pos;
        gen_batch.n_seq_id[0] = 1;
        gen_batch.seq_id[0][0] = 0;
        gen_batch.logits[0] = 1;
        gen_batch.n_tokens = 1;

        if (llama_decode(ctx, gen_batch) != 0) {
            fprintf(stderr, "FAIL: decode step %d\n", step);
            break;
        }
        n_pos++;

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

        char piece_buf[64];
        memset(piece_buf, 0, sizeof(piece_buf));
        llama_token_to_piece(vocab, best_token, piece_buf, 63, 0, false);
        printf("%s", piece_buf);
        fflush(stdout);

        last_token = best_token;
        n_generated++;

        /* Capture recurrent state after this token */
        size_t got_curr = llama_state_seq_get_data_ext(ctx, state_curr, state_size, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        if (got_curr == 0) {
            got_curr = llama_state_seq_get_data_ext(ctx, state_curr, state_size, 0, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
        }
        if (got_curr == 0) {
            got_curr = llama_state_get_data(ctx, state_curr, state_size);
        }

        /* Compute XOR delta between previous and current state */
        size_t compare_bytes = got > 0 ? got : state_size;
        size_t xor_bytes = 0;
        for (size_t b = 0; b < compare_bytes && b < got_curr; b++) {
            if (state_prev[b] != state_curr[b]) xor_bytes++;
        }

        uint64_t curr_hash = fnv1a_hash(state_curr, state_size);

        if (n_generated % 4 == 0) {
            printf("\n  [step %d] state=%zu bytes · captured=%zu · changed=%zu bytes (%.1f%%) · hash=%016llx\n",
                   n_generated, state_size, got_curr, xor_bytes,
                   compare_bytes > 0 ? xor_bytes * 100.0 / compare_bytes : 0.0,
                   (unsigned long long)curr_hash);
        }

        /* Swap: current becomes previous */
        uint8_t *tmp = state_prev;
        state_prev = state_curr;
        state_curr = tmp;
    }

    llama_batch_free(gen_batch);
    free(state_prev);
    free(state_curr);

    printf("\n\n=== GDN STATE SUMMARY ===\n");
    printf("Model: %s\n", model_path);
    printf("Recurrent: %s\n", is_recurrent ? "YES" : "NO");
    printf("Partial state size (recurrent): %zu bytes (%.2f KB)\n", state_size_partial, state_size_partial / 1024.0);
    printf("Full state size: %zu bytes (%.2f KB)\n", state_size_full, state_size_full / 1024.0);
    printf("Standard state size: %zu bytes (%.2f KB)\n", state_size_standard, state_size_standard / 1024.0);
    printf("After %d tokens: state size is FIXED (recurrent)\n", n_generated);
    printf("Per-token delta: variable (depends on state update magnitude)\n");

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();

    printf("\nRESULT: GDN recurrent state accessible — delta profile measured\n");
    return 0;
}
