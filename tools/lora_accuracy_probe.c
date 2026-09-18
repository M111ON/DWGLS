// lora_accuracy_probe.c — accuracy base vs +lora on TSV (question<TAB>gold), ChatML prompt, greedy
// usage: lora_accuracy_probe <base.gguf> <lora.gguf|-> <eval.tsv> <max_new> <dll_dir> [n_items=0(all)]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llama.h"

static const char *SYS = "You are a careful math assistant. Solve the problem, then ALWAYS verify your answer by recomputing with a different method before giving the final answer.";

static int argmax(const float *lg, int n) {
    int bi = 0;
    for (int i = 1; i < n; i++) if (lg[i] > lg[bi]) bi = i;
    return bi;
}

static int rollout(struct llama_model *m, struct llama_adapter_lora *ad,
                   const llama_token *prompt, int nprompt, int nnew,
                   const struct llama_vocab *vocab, int n_vocab, char *out, int cap) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)(nprompt + nnew + 16);
    cp.n_batch = (uint32_t)(nprompt + 16);
    struct llama_context *ctx = llama_init_from_model(m, cp);
    if (!ctx) return -1;
    if (ad) {
        float sc = 1.0f;
        if (llama_set_adapters_lora(ctx, &ad, 1, &sc) != 0) { llama_free(ctx); return -1; }
    }
    struct llama_batch b = llama_batch_init(nprompt, 0, 1);
    for (int i = 0; i < nprompt; i++) {
        b.token[i] = prompt[i]; b.pos[i] = i;
        b.n_seq_id[i] = 1; b.seq_id[i][0] = 0;
        b.logits[i] = (i == nprompt - 1) ? 1 : 0;
    }
    b.n_tokens = nprompt;
    if (llama_decode(ctx, b) != 0) { llama_batch_free(b); llama_free(ctx); return -1; }
    llama_batch_free(b);
    llama_token *gen = (llama_token *)malloc((size_t)nnew * sizeof(llama_token));
    int got = 0;
    for (int s = 0; s < nnew; s++) {
        float *lg = llama_get_logits(ctx);
        if (!lg) break;
        llama_token cur = (llama_token)argmax(lg, n_vocab);
        gen[got++] = cur;
        if (llama_vocab_is_eog(vocab, cur)) break;
        if (llama_decode(ctx, llama_batch_get_one(&cur, 1)) != 0) break;
    }
    int nch = llama_detokenize(vocab, gen, got, out, cap - 1, false, false);
    free(gen);
    llama_free(ctx);
    if (nch < 0) { out[0] = 0; return got; }
    out[nch < cap ? nch : cap - 1] = 0;
    return got;
}

// last integer after "final answer" (any case, ':' or 'is'); has_verify = contains "verif"
static int extract(const char *txt, int *has_verify) {
    *has_verify = (strstr(txt, "erif") != NULL || strstr(txt, "erify") != NULL);
    char low[4096];
    size_t L = strlen(txt);
    if (L > sizeof(low) - 1) L = sizeof(low) - 1;
    for (size_t i = 0; i < L; i++) low[i] = (char)tolower((unsigned char)txt[i]);
    low[L] = 0;
    const char *p = NULL, *q = low;
    while ((q = strstr(q, "final answer")) != NULL) { p = q; q += 12; }
    if (!p) return 0x7fffffff;
    q = p + 12;
    while (*q && (*q < '0' || *q > '9') && *q != '-') q++;
    if (!*q) return 0x7fffffff;
    return atoi(q);
}

int main(int argc, char **argv) {
    if (argc < 6) { printf("usage: %s <base.gguf> <lora|-> <eval.tsv> <max_new> <dll_dir> [n]\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    int use_lora = strcmp(argv[2], "-") != 0;
    int nnew = atoi(argv[4]);
    int nlimit = argc > 6 ? atoi(argv[6]) : 0;
    if (nnew < 1) nnew = 60;

    llama_backend_init();
    ggml_backend_load_all_from_path(argv[5]);
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { printf("model load fail\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    struct llama_adapter_lora *ad = NULL;
    if (use_lora) {
        ad = llama_adapter_lora_init(model, argv[2]);
        if (!ad) { printf("lora load fail\n"); return 1; }
        printf("lora loaded\n");
    }

    FILE *f = fopen(argv[3], "r");
    if (!f) { printf("tsv open fail\n"); return 1; }
    char line[2048], chat[3072], out[4096];
    int n = 0, correct = 0, nverify = 0;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        int gold = atoi(tab + 1);
        snprintf(chat, sizeof chat,
            "<|im_start|>system\n%s<|im_end|>\n<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", SYS, line);
        llama_token *prompt = (llama_token *)malloc(1024 * sizeof(llama_token));
        int np = llama_tokenize(vocab, chat, (int32_t)strlen(chat), prompt, 1024, false, false);
        if (np <= 0 || np >= 1024) { free(prompt); continue; }
        if (rollout(model, ad, prompt, np, nnew, vocab, n_vocab, out, sizeof out) < 0) {
            printf("item %d: ROLLOUT FAIL\n", n);
            free(prompt); continue;
        }
        int hv = 0, ans = extract(out, &hv);
        int ok = (ans == gold);
        correct += ok; nverify += hv; n++;
        printf("item %d: gold=%d got=%d %s verify=%d\n", n, gold, ans == 0x7fffffff ? -99999 : ans,
               ok ? "OK" : "WRONG", hv);
        if (n <= 3) printf("  out: %.220s\n", out);
        free(prompt);
        if (nlimit > 0 && n >= nlimit) break;
    }
    fclose(f);
    printf("ACC=%d/%d (%.1f%%) VERIFY_RATE=%d/%d (%.1f%%)\n", correct, n, n ? 100.0 * correct / n : 0,
           nverify, n, n ? 100.0 * nverify / n : 0);
    if (ad) llama_adapter_lora_free(ad);
    return 0;
}
