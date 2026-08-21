/*
 * tools/kv_dump_turns.c — dump REAL llama.cpp KV-cache state after each
 * conversation turn (multi-turn chat on Qwen2.5-0.5B).
 * ═══════════════════════════════════════════════════════════════════════
 * Links directly against the prebuilt b9733 llama.dll (vulkan package,
 * CPU path). Each turn appends user+assistant messages (Qwen chat
 * template, special tokens parsed), decodes only the NEW tokens, then
 * snapshots sequence-0 state via llama_state_seq_get_data → <out>/turnN.bin
 *
 * BUILD: gcc -O2 -I I:\llama\include -o build\kv_dump_turns tools\kv_dump_turns.c I:\llama\...\llama.dll
 * RUN:   PATH="...b9733 bin"; ./build/kv_dump_turns <model.gguf> <outdir>
 */
#include "llama.h"
#include "ggml-backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TURNS[] = {
    "<|im_start|>user\nWhat is the capital of France? Answer in one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe capital of France is Paris.<|im_end|>\n",
    "<|im_start|>user\nNow tell me one famous landmark there, one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe Eiffel Tower is Paris's most famous landmark.<|im_end|>\n",
    "<|im_start|>user\nWhat year was it built? One sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe Eiffel Tower was completed in 1889.<|im_end|>\n",
    "<|im_start|>user\nSummarize our whole conversation in one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nWe discussed Paris, its landmark the Eiffel Tower, and its 1889 completion.<|im_end|>\n",
};
#define N_TURNS 4

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 2 ? argv[2] : "build\\kvslots";

    const char *backend_dir = argc > 3 ? argv[3] : NULL;
    if (backend_dir) ggml_backend_load_all_from_path(backend_dir);
    else             ggml_backend_load_all();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx   = 4096;
    cp.n_batch = 512;
    cp.no_perf = true;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "FAIL: ctx\n"); return 1; }

    llama_token toks[4096];
    int32_t n_total = 0;

    for (int t = 0; t < N_TURNS; t++) {
        const char *txt = TURNS[t];
        int32_t len = (int32_t)strlen(txt);
        int need = -llama_tokenize(vocab, txt, len, NULL, 0, t == 0, true);
        if (need > 2048) { fprintf(stderr, "turn too long\n"); return 1; }
        int32_t n_new = llama_tokenize(vocab, txt, len,
                                       toks + n_total, need + 16, t == 0, true);
        if (n_new < 0) { fprintf(stderr, "tokenize fail\n"); return 1; }

        /* decode the new suffix in batches */
        int32_t done = 0;
        while (done < n_new) {
            int32_t chunk = n_new - done > 512 ? 512 : n_new - done;
            if (llama_decode(ctx, llama_batch_get_one(toks + n_total + done, chunk))) {
                fprintf(stderr, "decode fail @ turn %d\n", t + 1); return 1;
            }
            done += chunk;
        }
        n_total += n_new;

        /* snapshot seq-0 state → turnN.bin */
        char path[512];
        snprintf(path, sizeof(path), "%s\\turn%d.bin", outdir, t + 1);
        size_t sz = llama_state_seq_get_size(ctx, 0);
        void *buf = malloc(sz);
        if (!buf || llama_state_seq_get_data(ctx, (uint8_t *)buf, sz, 0) != sz) {
            fprintf(stderr, "state get fail\n"); return 1;
        }
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(buf, 1, sz, f) != sz) { fprintf(stderr, "write fail\n"); return 1; }
        fclose(f);
        free(buf);
        printf("turn %d: +%d tokens · total %d · state %.2f MB -> %s\n",
               t + 1, n_new, n_total, (double)sz / 1048576.0, path);
    }

    printf("done: %d tokens, %d snapshots in %s\n", n_total, N_TURNS, outdir);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}
