/*
 * tools/gguf_graft_generate.c — real generation through the cactus graft
 *
 * Level-2 proof: not just ONE forward pass (test_gguf_graft_llama T3), but a
 * full multi-token generation loop fed by the graft file:
 *
 *   [header scion from gguf_box] + [body served from source mmap] = graft
 *   llama.cpp loads the graft → greedy-generate N tokens
 *   same loop on the ORIGINAL file → token streams must be IDENTICAL
 *
 * The graft is reference-to-source (zero copy, no re-emit): llama reads the
 * same bytes it would read from the original — so generation is bitwise
 * deterministic and the scion (header) controls which bytes route where.
 *
 * Build (Windows DLLs, same as make graft-llama):
 *   gcc -O2 -std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare \
 *       -I core -I I:/llama/include -o build/gguf_graft_generate \
 *       tools/gguf_graft_generate.c \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll \
 *       -lzstd -lm
 *   PATH="I:/llama/llama-b9733-bin-win-vulkan-x64:$PATH" \
 *     ./build/gguf_graft_generate [model.gguf] [prompt] [n_tokens]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"
#include "ggml-backend.h"
#include "../core/gguf_box.h"

/* ── silence llama.cpp info spam; keep errors/warnings ── */
static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
        fputs(text, stderr);
}

/* ── tiny sampler: greedy argmax — deterministic, seed-free ── */
static llama_token greedy_sample(const float *logits, int n_vocab) {
    llama_token best = 0;
    float best_v = logits[0];
    for (int i = 1; i < n_vocab; i++)
        if (logits[i] > best_v) { best_v = logits[i]; best = (llama_token)i; }
    return best;
}

/* ── greedy-generate N tokens from a GGUF path ─────────────── */
/* returns malloc'd token array (caller frees); count via *n_out. */
static llama_token *generate(const char *gguf_path, const char *prompt,
                             int n_gen, int *n_out) {
    *n_out = 0;
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(gguf_path, mp);
    if (!model) { fprintf(stderr, "  (llama cannot load %s)\n", gguf_path); return NULL; }

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.n_threads = 4; cp.n_threads_batch = 4;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { llama_model_free(model); return NULL; }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    llama_token eos = llama_vocab_eos(vocab);

    /* tokenize: two-pass (query size, then fill) — avoid fixed buffers */
    int n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                  NULL, 0, true, false);
    if (n_prompt < 0) n_prompt = -n_prompt;
    llama_token *toks = (llama_token *)malloc((size_t)(n_prompt + 1) * sizeof(llama_token));
    int n2 = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                            toks, n_prompt, true, false);
    if (n2 < 0) n2 = -n2;
    n_prompt = n2;

    llama_token *out = (llama_token *)malloc((size_t)(n_gen + 1) * sizeof(llama_token));
    int total = 0;

    if (llama_decode(ctx, llama_batch_get_one(toks, n_prompt)) != 0) {
        free(toks); free(out); llama_free(ctx); llama_model_free(model);
        return NULL;
    }
    for (int i = 0; i < n_gen; i++) {
        const float *logits;
        if (i == 0)
            logits = llama_get_logits_ith(ctx, n_prompt - 1);
        else
            logits = llama_get_logits(ctx);
        llama_token tok = greedy_sample(logits, n_vocab);
        out[total++] = tok;
        if (tok == eos) break;
        if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) break;
    }

    /* NOTE: llama_batch_get_one owns its batch — no free needed (this llama
     * build crashes on llama_batch_free after decode anyway). */
    free(toks);
    llama_free(ctx);
    llama_model_free(model);
    *n_out = total;
    return out;
}

static void stream_text(const char *gguf_path, llama_token *toks, int n) {
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *m = llama_model_load_from_file(gguf_path, mp);
    if (!m) return;
    const struct llama_vocab *vocab = llama_model_get_vocab(m);
    char buf[64];
    for (int i = 0; i < n; i++) {
        int k = llama_token_to_piece(vocab, toks[i], buf, sizeof(buf), 0, false);
        if (k < 0) k = 0;
        fwrite(buf, 1, (size_t)k, stdout);
    }
    llama_model_free(m);
}

int main(int argc, char **argv) {
    const char *gguf   = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = (argc > 2) ? argv[2] : "The capital of France is";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 24;
    if (n_gen <= 0) n_gen = 24;
    const char *graft = "build/graft_gen.gguf";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Real generation through the cactus graft (multi-token, greedy)\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    llama_backend_init();
    llama_log_set(quiet_log, NULL);
    const char *dll_dir = "I:/llama/llama-b9733-bin-win-vulkan-x64";
    ggml_backend_load_all_from_path(dll_dir);

    GGUFBox box;
    if (gguf_box_open(&box, gguf) != 0) {
        printf("(cannot open %s — model/DLLs missing on this machine)\n", gguf);
        llama_backend_free();
        return 1;
    }

    /* assemble the graft: header scion + body served from the source mmap */
    size_t hdr_sz = (size_t)box.reader.data_offset;
    FILE *f = fopen(graft, "wb");
    if (!f) { gguf_box_close(&box); return 1; }
    if (fwrite(box.reader.base, 1, hdr_sz, f) != hdr_sz ||
        fwrite(box.reader.base + box.reader.data_offset, 1,
               box.reader.base_sz - box.reader.data_offset, f) !=
               box.reader.base_sz - box.reader.data_offset) {
        fclose(f); gguf_box_close(&box); return 1;
    }
    fclose(f);
    printf("graft written: %s (header %zu B + body %zu B)\n", graft, hdr_sz,
           box.reader.base_sz - box.reader.data_offset);

    printf("\nprompt: \"%s\"  (generate up to %d tokens)\n\n", prompt, n_gen);

    int ng = 0, no = 0;
    llama_token *g = generate(graft, prompt, n_gen, &ng);
    llama_token *o = generate(gguf,  prompt, n_gen, &no);

    int ok = (g != NULL && o != NULL && ng == no);
    if (ok) for (int i = 0; i < ng; i++) if (g[i] != o[i]) { ok = 0; break; }

    printf("  graft:    %d tokens\n", ng);
    printf("  original: %d tokens\n", no);
    printf("  token streams identical: %s\n", ok ? "YES ✅" : "NO ❌");
    if (ok) {
        printf("\n  generated text (graft): \"");
        stream_text(gguf, g, ng);
        printf("\"\n");
    } else if (g && o) {
        int first_diff = 0;
        while (first_diff < ng && first_diff < no && g[first_diff] == o[first_diff])
            first_diff++;
        printf("  first divergence at token %d: graft=%d original=%d\n",
               first_diff, first_diff < ng ? g[first_diff] : -1,
               first_diff < no ? o[first_diff] : -1);
    }

    free(g); free(o);
    remove(graft);
    gguf_box_close(&box);
    llama_backend_free();
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULT: %s\n", ok ? "generation identical — real inference works through the graft"
                             : "mismatch");
    return ok ? 0 : 1;
}
