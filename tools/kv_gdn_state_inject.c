/*
 * tools/kv_gdn_state_inject.c — GDN state capture + restore roundtrip
 * ════════════════════════════════════════════════════════════════════════════
 * Test: process prompt → generate N tokens → save state → load into fresh
 * context → generate N more tokens → compare with original's next N tokens.
 *
 * KEY FINDING: llama_state_seq_get_size_ext returns WRONG size for GDN models
 * when state is empty (returns 460 bytes). After processing tokens, it returns
 * the true size (~20MB). File-based API works regardless.
 *
 * BUILD:
 *   C:\msys64\usr\bin\env.exe PATH="/mingw64/bin:$PATH" gcc -O2 -std=c11 \
 *     -II:/llama/include -o build/kv_gdn_state_inject.exe tools/kv_gdn_state_inject.c \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "llama.h"

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static uint64_t fnv1a(const void *d, size_t n) {
    const uint8_t *p = (const uint8_t *)d;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

struct gen_result {
    char text[256];
    llama_token tokens_out[32];
    uint64_t logits_hash;
    int n_tokens;
    llama_token last_token;
    int final_pos;
};

static struct gen_result generate_n(
    struct llama_context *ctx, struct llama_vocab *vocab,
    llama_token start_token, int start_pos, int n_gen)
{
    struct gen_result r = {0};
    llama_token cur = start_token;
    int pos = start_pos;
    float logits_snap[256] = {0};

    struct llama_batch batch = llama_batch_init(1, 0, 1);
    for (int i = 0; i < n_gen; i++) {
        batch.token[0] = cur;
        batch.pos[0] = pos;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0] = 1;
        batch.n_tokens = 1;
        if (llama_decode(ctx, batch) != 0) break;
        pos++;
        float *logits = llama_get_logits(ctx);
        if (i < 256) logits_snap[i] = logits[0];

        int best = 0;
        float bv = -1e9f;
        for (int t = 0; t < llama_vocab_n_tokens(vocab) && t < 32000; t++) {
            if (logits[t] > bv) { bv = logits[t]; best = t; }
        }
        if (i < 32) r.tokens_out[i] = best;
        char piece[64] = {0};
        llama_token_to_piece(vocab, best, piece, 63, 0, false);
        strncat(r.text, piece, sizeof(r.text) - strlen(r.text) - 1);
        r.n_tokens++;
        cur = best;
    }
    r.logits_hash = fnv1a(logits_snap, sizeof(logits_snap));
    r.last_token = cur;
    r.final_pos = pos;
    llama_batch_free(batch);
    return r;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "F:\\model\\Qwen3.5-2B-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";
    const char *state_file = "gdn_state_rt.bin";

    fprintf(stderr, "=== GDN State Roundtrip Test ===\n");
    fprintf(stderr, "model: %s\n", model_path);

    llama_backend_init();
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mparams);
    if (!model) { fprintf(stderr, "FAIL: load model\n"); return 1; }

    struct llama_vocab *vocab = (struct llama_vocab *)llama_model_get_vocab(model);
    llama_token tokens[1024];
    int n_prompt = llama_tokenize(vocab, prompt, strlen(prompt), tokens, 1024, true, false);

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    cparams.n_batch = 512;
    cparams.n_ubatch = 512;

    /* ═══ PHASE 1: Process prompt on ctx1 ═══ */
    fprintf(stderr, "\n--- PHASE 1: Process prompt ---\n");
    struct llama_context *ctx1 = llama_init_from_model(model, cparams);
    struct llama_batch pb = llama_batch_init(n_prompt, 0, 1);
    for (int i = 0; i < n_prompt; i++) {
        pb.token[i] = tokens[i];
        pb.pos[i] = i;
        pb.n_seq_id[i] = 1;
        pb.seq_id[i][0] = 0;
        pb.logits[i] = (i == n_prompt - 1) ? 1 : 0;
    }
    pb.n_tokens = n_prompt;
    llama_decode(ctx1, pb);
    llama_batch_free(pb);
    fprintf(stderr, "  prompt decoded (%d tokens)\n", n_prompt);

    /* ═══ PHASE 2: Generate 16 tokens (baseline) ═══ */
    fprintf(stderr, "\n--- PHASE 2: Generate baseline 16 tokens ---\n");
    struct gen_result baseline = generate_n(ctx1, vocab, tokens[n_prompt - 1], n_prompt, 16);
    fprintf(stderr, "  baseline: \"%s\"\n", baseline.text);
    fprintf(stderr, "  ended at pos %d, last_token=%d\n", baseline.final_pos, baseline.last_token);

    /* ═══ PHASE 3: Save state at this checkpoint ═══ */
    fprintf(stderr, "\n--- PHASE 3: Save state ---\n");
    size_t reported_sz = llama_state_seq_get_size_ext(ctx1, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "  API reported size: %zu bytes (%.2f MB)\n", reported_sz, reported_sz / 1048576.0);

    bool saved = llama_state_save_file(ctx1, state_file, tokens, n_prompt);
    fprintf(stderr, "  save: %s\n", saved ? "OK" : "FAIL");

    /* Get actual file size */
    LARGE_INTEGER fsz = {0};
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(state_file, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        fsz.LowPart = fd.nFileSizeLow;
        fsz.HighPart = fd.nFileSizeHigh;
        FindClose(fh);
    }
    fprintf(stderr, "  file: %lld bytes (%.2f MB)\n", fsz.QuadPart, fsz.QuadPart / 1048576.0);

    /* ═══ PHASE 4: Generate 16 MORE tokens on ctx1 (reference continuation) ═══ */
    fprintf(stderr, "\n--- PHASE 4: Reference continuation (16 more tokens) ---\n");
    struct gen_result ref_cont = generate_n(ctx1, vocab, baseline.last_token, baseline.final_pos, 16);
    fprintf(stderr, "  ref_cont: \"%s\"\n", ref_cont.text);
    fprintf(stderr, "  logits_hash: %016llx\n", (unsigned long long)ref_cont.logits_hash);
    llama_free(ctx1);

    /* ═══ PHASE 5: Create fresh context, restore state ═══ */
    fprintf(stderr, "\n--- PHASE 5: Fresh context + restore ---\n");
    struct llama_context *ctx2 = llama_init_from_model(model, cparams);

    /* First verify empty state size */
    size_t empty_sz = llama_state_seq_get_size_ext(ctx2, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "  empty state size: %zu bytes\n", empty_sz);

    llama_token load_tok[1024];
    size_t n_tok = 0;
    double t0 = now_ms();
    bool loaded = llama_state_load_file(ctx2, state_file, load_tok, 1024, &n_tok);
    double dt = now_ms() - t0;
    fprintf(stderr, "  load: %s (%.0f ms, %zu tokens)\n", loaded ? "OK" : "FAIL", dt, n_tok);

    /* Check state size after loading */
    size_t loaded_sz = llama_state_seq_get_size_ext(ctx2, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "  loaded state size: %zu bytes (%.2f MB)\n", loaded_sz, loaded_sz / 1048576.0);

    /* ═══ PHASE 6: Generate 16 tokens from restored state ═══ */
    fprintf(stderr, "\n--- PHASE 6: Generate from restored state ---\n");
    struct gen_result restored = generate_n(ctx2, vocab, baseline.last_token, baseline.final_pos, 16);
    fprintf(stderr, "  restored: \"%s\"\n", restored.text);
    fprintf(stderr, "  logits_hash: %016llx\n", (unsigned long long)restored.logits_hash);

    /* ═══ COMPARE ═══ */
    fprintf(stderr, "\n--- RESULTS ---\n");
    int text_eq = (strcmp(ref_cont.text, restored.text) == 0);
    int logit_eq = (ref_cont.logits_hash == restored.logits_hash);
    int token_eq = 1;
    for (int i = 0; i < 16; i++) {
        if (ref_cont.tokens_out[i] != restored.tokens_out[i]) {
            token_eq = 0;
            fprintf(stderr, "  token[%d]: ref=%d restored=%d\n", i, ref_cont.tokens_out[i], restored.tokens_out[i]);
        }
    }

    fprintf(stderr, "  text match:  %s  (ref: \"%s\")\n", text_eq ? "PASS" : "FAIL", ref_cont.text);
    fprintf(stderr, "  token match: %s\n", token_eq ? "PASS" : "FAIL");
    fprintf(stderr, "  logit match: %s\n", logit_eq ? "PASS" : "FAIL");
    fprintf(stderr, "\n=== %s ===\n",
            (text_eq && token_eq && logit_eq) ? "ROUNDTRIP PASS" : "ROUNDTRIP FAIL");

    llama_free(ctx2);
    llama_model_free(model);
    llama_backend_free();
    remove(state_file);

    return (text_eq && token_eq && logit_eq) ? 0 : 1;
}
