// lora_delta_probe.c — per-token logit[target] base vs +lora + freq buckets
// usage: lora_delta_probe <base.gguf> <lora.gguf|-> <corpus.txt> <ntok> <dll_dir> <out.csv>
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

int main(int argc, char **argv) {
    if (argc < 7) { printf("usage: %s <base.gguf> <lora.gguf|-> <corpus> <ntok> <dll_dir> <out.csv>\n", argv[0]); return 2; }
    const char *gguf = argv[1], *lora_path = argv[2], *corpus_path = argv[3];
    int NTOK = atoi(argv[4]);
    if (NTOK <= 16) NTOK = 500;
    const char *dll_dir = argv[5], *csv_path = argv[6];
    int use_lora = strcmp(lora_path, "-") != 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    llama_backend_init();
    ggml_backend_load_all_from_path(dll_dir);
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(gguf, mp);
    if (!model) { printf("model load fail\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    printf("vocab=%d\n", n_vocab);

    long clen = 0;
    char *text = read_file(corpus_path, &clen);
    if (!text) { printf("corpus read fail\n"); return 1; }
    llama_token *toks = (llama_token *)malloc(65536u * sizeof(llama_token));
    int n = llama_tokenize(vocab, text, (int32_t)clen, toks, 65536, true, false);
    free(text);
    if (n <= 16) { printf("tokenize fail n=%d\n", n); return 1; }
    if (n > NTOK) n = NTOK;
    printf("tokens=%d\n", n);

    int *freq = (int *)calloc((size_t)n_vocab, sizeof(int));
    for (int i = 0; i < n; i++) if (toks[i] >= 0 && toks[i] < n_vocab) freq[toks[i]]++;

    float *lb = (float *)malloc((size_t)n * sizeof(float));
    float *ll = (float *)malloc((size_t)n * sizeof(float));
    struct llama_adapter_lora *ad = NULL;
    if (use_lora) {
        ad = llama_adapter_lora_init(model, lora_path);
        if (!ad) { printf("lora load fail\n"); return 1; }
        printf("lora loaded\n");
    }

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && !use_lora) { memcpy(ll, lb, (size_t)n * sizeof(float)); break; }
        struct llama_context_params cp = llama_context_default_params();
        cp.n_ctx = (uint32_t)(n + 16);
        cp.n_batch = 8;
        struct llama_context *ctx = llama_init_from_model(model, cp);
        if (!ctx) { printf("ctx fail\n"); return 1; }
        if (pass == 1 && use_lora) {
            float sc = 1.0f;
            if (llama_set_adapters_lora(ctx, &ad, 1, &sc) != 0) { printf("set lora fail\n"); return 1; }
        }
        for (int i = 0; i < n - 1; i++) {
            if (llama_decode(ctx, llama_batch_get_one(&toks[i], 1)) != 0) {
                printf("decode fail at %d\n", i);
                return 1;
            }
            float *lg = llama_get_logits(ctx);
            int tgt = toks[i + 1];
            float v = (tgt >= 0 && tgt < n_vocab) ? lg[tgt] : 0.0f;
            if (pass == 0) lb[i] = v; else ll[i] = v;
        }
        llama_free(ctx);
        printf("pass %d done\n", pass);
    }

    FILE *f = fopen(csv_path, "w");
    fprintf(f, "pos,tok,freq,logit_base,logit_lora\n");
    double s_rare = 0, s_mid = 0, s_com = 0;
    long c_rare = 0, c_mid = 0, c_com = 0;
    for (int i = 0; i < n - 1; i++) {
        double d = (double)ll[i] - (double)lb[i];
        double ad_ = d < 0 ? -d : d;
        int fr = (toks[i + 1] >= 0 && toks[i + 1] < n_vocab) ? freq[toks[i + 1]] : 0;
        if (fr <= 1) { s_rare += ad_; c_rare++; }
        else if (fr <= 5) { s_mid += ad_; c_mid++; }
        else { s_com += ad_; c_com++; }
        fprintf(f, "%d,%d,%d,%.4f,%.4f\n", i, toks[i + 1], fr, lb[i], ll[i]);
    }
    fclose(f);
    printf("MEAN|delta| rare(f<=1,n=%ld)=%.4f mid(2-5,n=%ld)=%.4f common(>5,n=%ld)=%.4f\n",
        c_rare, c_rare ? s_rare / c_rare : 0, c_mid, c_mid ? s_mid / c_mid : 0,
        c_com, c_com ? s_com / c_com : 0);
    if (ad) llama_adapter_lora_free(ad);
    return 0;
}
