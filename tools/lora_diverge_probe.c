// lora_diverge_probe.c — free-run base vs +lora (greedy), first-divergence + diff count
// usage: lora_diverge_probe <base.gguf> <lora.gguf> <prompt|@file> <ntokens> <dll_dir>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llama.h"

static char *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0;
    fclose(f);
    if (out_len) *out_len = n;
    return b;
}

static int argmax(const float *lg, int n) {
    int bi = 0;
    for (int i = 1; i < n; i++) if (lg[i] > lg[bi]) bi = i;
    return bi;
}

// greedy rollout; returns tokens generated (out must hold cap)
static int rollout(struct llama_model *m, struct llama_adapter_lora *ad,
                   const llama_token *prompt, int nprompt, int nnew, int *out) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)(nprompt + nnew + 16);
    cp.n_batch = 32;
    struct llama_context *ctx = llama_init_from_model(m, cp);
    if (!ctx) return -1;
    if (ad) {
        float sc = 1.0f;
        if (llama_set_adapters_lora(ctx, &ad, 1, &sc) != 0) { llama_free(ctx); return -1; }
    }
    const struct llama_vocab *vocab = llama_model_get_vocab(m);
    int n_vocab = llama_vocab_n_tokens(vocab);
    llama_token cur = -1;
    // feed prompt in one batch
    struct llama_batch b = llama_batch_init(nprompt, 0, 1);
    for (int i = 0; i < nprompt; i++) {
        b.token[i] = prompt[i];
        b.pos[i] = i;
        b.n_seq_id[i] = 1; b.seq_id[i][0] = 0;
        b.logits[i] = (i == nprompt - 1) ? 1 : 0;
    }
    b.n_tokens = nprompt;
    if (llama_decode(ctx, b) != 0) { llama_batch_free(b); llama_free(ctx); return -1; }
    llama_batch_free(b);
    int pos = nprompt, got = 0;
    for (int s = 0; s < nnew; s++) {
        float *lg = llama_get_logits(ctx);
        if (!lg) { llama_free(ctx); return -1; }
        cur = (llama_token)argmax(lg, n_vocab);
        out[got++] = cur;
        if (llama_vocab_is_eog(vocab, cur)) break;
        if (llama_decode(ctx, llama_batch_get_one(&cur, 1)) != 0) { llama_free(ctx); return -1; }
        pos++;
    }
    llama_free(ctx);
    return got;
}

int main(int argc, char **argv) {
    if (argc < 6) { printf("usage: %s <base.gguf> <lora.gguf> <prompt|@file> <nnew> <dll_dir>\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    int nnew = atoi(argv[4]);
    if (nnew < 1) nnew = 100;

    llama_backend_init();
    ggml_backend_load_all_from_path(argv[5]);
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { printf("model load fail\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    char *prompt_text = (argv[3][0] == '@') ? read_file(argv[3] + 1, NULL) : argv[3];
    int need_free = (argv[3][0] == '@');
    if (!prompt_text) { printf("prompt read fail\n"); return 1; }
    llama_token *prompt = (llama_token *)malloc(4096 * sizeof(llama_token));
    int np = llama_tokenize(vocab, prompt_text, (int32_t)strlen(prompt_text), prompt, 4096, true, false);
    if (need_free) free(prompt_text);
    if (np <= 0) { printf("prompt tokenize fail\n"); return 1; }
    if (np > 256) np = 256; // cap prompt
    printf("prompt_tokens=%d nnew=%d\n", np, nnew);

    struct llama_adapter_lora *ad = llama_adapter_lora_init(model, argv[2]);
    if (!ad) { printf("lora load fail\n"); return 1; }
    printf("lora loaded\n");

    int *tb = (int *)malloc((size_t)nnew * sizeof(int));
    int *tl = (int *)malloc((size_t)nnew * sizeof(int));
    int nb = rollout(model, NULL, prompt, np, nnew, tb);
    printf("base rollout=%d\n", nb);
    int nl = rollout(model, ad, prompt, np, nnew, tl);
    printf("lora rollout=%d\n", nl);
    if (nb < 0 || nl < 0) { printf("rollout fail\n"); return 1; }

    int ncmp = nb < nl ? nb : nl, first = -1, ndiff = 0;
    for (int i = 0; i < ncmp; i++) {
        if (tb[i] != tl[i]) { if (first < 0) first = i; ndiff++; }
    }
    printf("FIRST_DIVERGE=%d NDIFF=%d/%d LEN base=%d lora=%d\n", first, ndiff, ncmp, nb, nl);
    // print both streams as text (first 600 chars each)
    char *sb = (char *)malloc(4096), *sl = (char *)malloc(4096);
    int cb = llama_detokenize(vocab, (const llama_token *)tb, nb, sb, 4095, false, false);
    int cl = llama_detokenize(vocab, (const llama_token *)tl, nl, sl, 4095, false, false);
    printf("--- BASE ---\n%.*s\n--- LORA ---\n%.*s\n", cb < 0 ? 0 : (cb > 600 ? 600 : cb), sb, cl < 0 ? 0 : (cl > 600 ? 600 : cl), sl);
    llama_adapter_lora_free(ad);
    return 0;
}
