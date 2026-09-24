/* tools/merge_probe.c — load GGUF, greedy decode prompt, print token ids.
 * Mechanical graft verdict: loads + generates coherent ids.
 * BUILD: gcc -O2 -I I:/llama/include -o build/merge_probe.exe tools/merge_probe.c <dlls>
 */
#include "llama.h"
#include "ggml-backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";
    int n_gen = argc > 3 ? atoi(argv[3]) : 20;
    ggml_backend_load_all();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { printf("LOAD_FAIL\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.no_perf = true;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { printf("CTX_FAIL\n"); return 1; }
    int n_vocab = llama_vocab_n_tokens(vocab);
    llama_token toks[2048];
    int32_t np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, 2048, true, true);
    if (np <= 0 || llama_decode(ctx, llama_batch_get_one(toks, np))) { printf("PREFILL_FAIL\n"); return 1; }
    printf("ids:");
    int past = np;
    for (int i = 0; i < n_gen; i++) {
        const float *lg = llama_get_logits(ctx);
        llama_token best = 0;
        float bv = lg[0];
        int bad = 0;
        for (int t = 1; t < n_vocab; t++) {
            float v = lg[t];
            if (v != v) { bad = 1; break; }
            if (v > bv) { bv = v; best = t; }
        }
        if (bad) { printf(" NONFINITE"); break; }
        printf(" %d", (int)best);
        if (best == llama_vocab_eos(vocab)) break;
        if (llama_decode(ctx, llama_batch_get_one(&best, 1))) { printf(" DECODE_FAIL"); break; }
        past++;
    }
    printf("\n");
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}
