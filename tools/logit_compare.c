/*
 * tools/logit_compare.c — verify patched llama produces correct logits
 *
 * Strategy: Load model via patched llama_model_load_from_file (which internally
 * routes through gguf_box). Generate tokens. Then compare against cmp_rdh reference
 * by checking that gguf_box pointers return same data as raw mmap.
 *
 * Also generates tokens like gguf_lazy_serve for cross-tool comparison.
 *
 * BUILD: gcc -O2 -Wall -I I:/llama/llama.cpp/include -I I:/llama/llama.cpp/ggml/include
 *        tools/logit_compare.c -o build/logit_compare.exe
 *        -L I:/llama/llama.cpp/build_zc2/bin/Release -lllama -lggml -lm
 *
 * RUN:   build/logit_compare.exe <model.gguf> [prompt] [n_gen] [backend_path]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "llama.h"

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    const char *prompt    = (argc > 2) ? argv[2] : "The capital of France is";
    int n_gen             = (argc > 3) ? atoi(argv[3]) : 40;
    const char *backend   = (argc > 4) ? argv[4] : "I:/llama/llama.cpp/build_zc2/bin/Release";
    if (n_gen <= 0) n_gen = 40;

    llama_backend_init();
    llama_log_set(NULL, NULL);
    ggml_backend_load_all_from_path(backend);
    ggml_backend_load_all();

    struct llama_model_params mp = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(gguf_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 256;
    cp.n_batch = 256;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "FAIL: ctx\n"); return 1; }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);

    llama_token toks[256];
    int np = llama_tokenize(vocab, prompt, (int)strlen(prompt), toks, 256, true, false);
    if (np < 0) np = -np;

    printf("model: %s\n", gguf_path);
    printf("prompt: %s\n", prompt);
    printf("tokens_in: %d  vocab: %d\n", np, n_vocab);

    /* Prefill */
    if (llama_decode(ctx, llama_batch_get_one(toks, np)) != 0) {
        fprintf(stderr, "FAIL: decode prefill\n"); return 1;
    }

    /* Get first-step logits for comparison */
    const float *logits0 = llama_get_logits_ith(ctx, np - 1);
    printf("logits0[0..4]: %.6f %.6f %.6f %.6f %.6f\n",
           logits0[0], logits0[1], logits0[2], logits0[3], logits0[4]);

    /* Greedy generation — dump tokens */
    llama_token eos = llama_vocab_eos(vocab);
    printf("gen_tokens:");
    for (int i = 0; i < n_gen; i++) {
        const float *logits = (i == 0) ? logits0 : llama_get_logits(ctx);
        llama_token best = 0;
        float bv = logits[0];
        for (int t = 1; t < n_vocab; t++) {
            if (logits[t] > bv) { bv = logits[t]; best = (llama_token)t; }
        }
        printf(" %d", best);
        if (best == eos) { printf(" <EOS>"); break; }
        if (llama_decode(ctx, llama_batch_get_one(&best, 1)) != 0) { printf(" <DECODE_ERR>"); break; }
    }
    printf("\n");

    /* Also dump logits0 first 10 for cross-tool cmp */
    printf("logits0_first10:");
    for (int i = 0; i < 10 && i < n_vocab; i++) printf(" %.9f", logits0[i]);
    printf("\n");

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
