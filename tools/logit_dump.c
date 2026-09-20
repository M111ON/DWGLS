/* logit_dump.c ? dump raw logit vector after first decode.
 * Usage: logit_dump.exe <model.gguf> <prompt> <output.bin>
 * Reads llama.dll at runtime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LLAMA_API_EXPORT
#include "llama.h"
#include "ggml-backend.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Usage: %s <model.gguf> <prompt> <output.bin>\n", argv[0]); return 1; }
    const char *model_path = argv[1];
    const char *prompt = argv[2];
    const char *out_path = argv[3];

    ggml_backend_load_all_from_path("I:/llama/llama.cpp/build_zc2/bin/Release");

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "model load failed\n"); return 1; }

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048;
    cp.n_batch = 2048;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "ctx init failed\n"); llama_model_free(model); return 1; }

    struct llama_vocab *voc = llama_model_get_vocab(model);

    /* tokenize */
    int32_t n_tokens = 0;
    llama_token tokens[2048];
    n_tokens = llama_tokenize(voc, prompt, strlen(prompt), tokens, 2048, true, true);
    if (n_tokens <= 0) { fprintf(stderr, "tokenize failed: %d\n", n_tokens); return 1; }

    /* batch + decode */
    struct llama_batch batch = llama_batch_init(n_tokens, 0, 1);
    for (int i = 0; i < n_tokens; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n_tokens - 1) ? 1 : 0;
    }
    batch.n_tokens = n_tokens;

    int rc = llama_decode(ctx, batch);
    if (rc != 0) { fprintf(stderr, "decode failed: %d\n", rc); return 1; }

    /* get logits at last position */
    float *logits = llama_get_logits(ctx);
    int32_t n_vocab = llama_n_vocab(voc);
    fprintf(stderr, "model=%s prompt=\"%s\" n_tokens=%d n_vocab=%d logits@%p\n",
            model_path, prompt, n_tokens, n_vocab, (void*)logits);

    /* dump to binary file */
    FILE *f = fopen(out_path, "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", out_path); return 1; }
    fwrite(logits, sizeof(float), n_vocab, f);
    fclose(f);
    fprintf(stderr, "dumped %d floats (%zu bytes) to %s\n", n_vocab, n_vocab * sizeof(float), out_path);

    /* top-1 greedy */
    int best = 0;
    for (int i = 1; i < n_vocab; i++) if (logits[i] > logits[best]) best = i;
    fprintf(stderr, "top1: token=%d logit=%.6f\n", best, logits[best]);

    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}