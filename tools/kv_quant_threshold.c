/*
 * tools/kv_quant_threshold.c — empirical cold-KV quantization threshold.
 * ═══════════════════════════════════════════════════════════════════════
 * Question: how much deviation does KV-cache quantization introduce?
 * Method: teacher-forced decode of a fixed prompt+continuation on
 * Qwen2.5-0.5B with type_k/type_v = F16 (baseline) vs Q8_0 / Q4_0 / Q4_1.
 * All configs walk identical token rails → logits comparable per position.
 * Metrics per config: mean KL(ref||q), top-1 match, top-5 overlap, max |dlogit|.
 *
 * BUILD (MSYS2): gcc -O2 -I I:/llama/include -I core -o build/kv_quant_threshold \
 *   tools/kv_quant_threshold.c I:/llama/llama-v040-bin-win-vulkan-x64/llama.dll \
 *   I:/llama/llama-v040-bin-win-vulkan-x64/ggml.dll \
 *   I:/llama/llama-v040-bin-win-vulkan-x64/ggml-base.dll \
 *   I:/llama/llama-v040-bin-win-vulkan-x64/ggml-cpu-x64.dll -lm
 * RUN: PATH="I:/llama/llama-v040-bin-win-vulkan-x64:$PATH" ./build/kv_quant_threshold [model.gguf]
 */
#include "llama.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

static const char *PROMPT =
    "<|im_start|>user\nExplain why the sky is blue in two sentences.<|im_end|>\n"
    "<|im_start|>assistant\nThe sky appears blue because air molecules scatter short-wavelength "
    "blue light more strongly than other colors, a phenomenon called Rayleigh scattering. "
    "At sunset the light travels through more atmosphere, scattering away the blue and "
    "leaving the reds and oranges we see near the horizon.<|im_end|>\n";

#define N_PROBE 32   /* last N token positions scored as probe points */
#define TOPK    5

typedef struct { const char *name; enum ggml_type tk, tv; } KVCfg;
static const KVCfg CFGS[] = {
    { "F16",  GGML_TYPE_F16,  GGML_TYPE_F16  },
    { "Q8_0", GGML_TYPE_Q8_0, GGML_TYPE_Q8_0 },
    { "Q4_0", GGML_TYPE_Q4_0, GGML_TYPE_Q4_0 },
    { "Q4_1", GGML_TYPE_Q4_1, GGML_TYPE_Q4_1 },
};
#define N_CFG (sizeof(CFGS)/sizeof(CFGS[0]))

/* top-1 + top-5 set from a logits vector */
static void topk(const float *lg, int32_t n, int *t1, int top5[TOPK]) {
    int idx[TOPK]; float val[TOPK];
    for (int k = 0; k < TOPK; k++) { idx[k] = -1; val[k] = -FLT_MAX; }
    for (int32_t i = 0; i < n; i++) {
        float v = lg[i];
        for (int k = 0; k < TOPK; k++) if (v > val[k]) {
            for (int j = TOPK-1; j > k; j--) { val[j]=val[j-1]; idx[j]=idx[j-1]; }
            val[k]=v; idx[k]=i; break;
        }
    }
    *t1 = idx[0];
    for (int k = 0; k < TOPK; k++) top5[k] = idx[k];
}

/* KL(ref||q) over full vocab from raw logits (log-softmax both sides) */
static double kl_full(const float *lr, const float *lq, int32_t n, double *maxd) {
    float mr = -FLT_MAX, mq = -FLT_MAX;
    for (int32_t i = 0; i < n; i++) { if (lr[i]>mr) mr=lr[i]; if (lq[i]>mq) mq=lq[i]; }
    double sr = 0, sq = 0;
    for (int32_t i = 0; i < n; i++) { sr += exp(lr[i]-mr); sq += exp(lq[i]-mq); }
    double lsr = log(sr), lsq = log(sq), kl = 0, md = 0;
    for (int32_t i = 0; i < n; i++) {
        double pr = exp(lr[i]-mr-lsr), qr = exp(lq[i]-mq-lsq);
        double d = fabs(lr[i]-mr-lsr - (lq[i]-mq-lsq));
        if (d > md) md = d;
        if (pr > 1e-12) kl += pr * (log(pr) - log(qr > 1e-12 ? qr : 1e-12));
    }
    *maxd = md;
    return kl;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    ggml_backend_load_all();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);

    /* tokenize once */
    int32_t n_txt = (int32_t)strlen(PROMPT);
    int32_t need = -llama_tokenize(vocab, PROMPT, n_txt, NULL, 0, true, true);
    llama_token *toks = malloc((need+16) * sizeof(llama_token));
    int32_t n_tok = llama_tokenize(vocab, PROMPT, n_txt, toks, need+16, true, true);
    if (n_tok <= N_PROBE + 4) { fprintf(stderr, "FAIL: prompt too short (%d)\n", n_tok); return 1; }
    int32_t n_probe_pos = N_PROBE;

    /* logits[ci][p][v]: N_CFG × N_PROBE × vocab — F16 row filled first as ref */
    float **LG = malloc(N_CFG * sizeof(float *));
    for (size_t c = 0; c < N_CFG; c++)
        LG[c] = malloc((size_t)n_probe_pos * n_vocab * sizeof(float));

    for (size_t c = 0; c < N_CFG; c++) {
        struct llama_context_params cp = llama_context_default_params();
        cp.n_ctx = 2048; cp.n_batch = 512; cp.no_perf = true;
        cp.type_k = CFGS[c].tk; cp.type_v = CFGS[c].tv;
        struct llama_context *ctx = llama_init_from_model(model, cp);
        if (!ctx) { fprintf(stderr, "FAIL: ctx %s\n", CFGS[c].name); return 1; }
        /* manual batch: request logits at every position */
        llama_batch b = llama_batch_init(n_tok, 0, 1);
        for (int32_t i = 0; i < n_tok; i++) {
            b.token[i] = toks[i]; b.pos[i] = i;
            b.n_seq_id[i] = 1; b.seq_id[i][0] = 0; b.logits[i] = 1;
        }
        b.n_tokens = n_tok;
        /* decode in 512-chunks (logits flag kept per token) */
        for (int32_t off = 0; off < n_tok; off += 512) {
            llama_batch b2 = llama_batch_init(512, 0, 1);
            int32_t n = n_tok - off > 512 ? 512 : n_tok - off;
            for (int32_t i = 0; i < n; i++) {
                b2.token[i] = toks[off+i]; b2.pos[i] = off+i;
                b2.n_seq_id[i] = 1; b2.seq_id[i][0] = 0; b2.logits[i] = 1;
            }
            b2.n_tokens = n;
            if (llama_decode(ctx, b2)) {
                fprintf(stderr, "FAIL: decode %s @%d\n", CFGS[c].name, off); return 1;
            }
            llama_batch_free(b2);
        }
        llama_batch_free(b);
        /* probe = last N_PROBE positions */
        for (int p = 0; p < n_probe_pos; p++) {
            int32_t pos = n_tok - n_probe_pos + p;
            float *lg = llama_get_logits_ith(ctx, pos);
            if (!lg) { fprintf(stderr, "FAIL: logits %s pos %d\n", CFGS[c].name, pos); return 1; }
            memcpy(LG[c] + (size_t)p * n_vocab, lg, (size_t)n_vocab * sizeof(float));
        }
        llama_free(ctx);
        printf("decoded %-4s  (%d tokens, %d probes)\n", CFGS[c].name, n_tok, n_probe_pos);
    }

    printf("\n%-5s %12s %10s %10s %12s\n", "KV", "meanKL", "top1match", "top5ovlp", "max|dlogit|");
    for (size_t c = 1; c < N_CFG; c++) {
        double kl_sum = 0, md_max = 0; int t1hit = 0, t5hit = 0;
        for (int p = 0; p < n_probe_pos; p++) {
            const float *lr = LG[0] + (size_t)p * n_vocab;
            const float *lq = LG[c] + (size_t)p * n_vocab;
            double md = 0;
            kl_sum += kl_full(lr, lq, n_vocab, &md);
            if (md > md_max) md_max = md;
            int r1, q1, r5[TOPK], q5[TOPK];
            topk(lr, n_vocab, &r1, r5); topk(lq, n_vocab, &q1, q5);
            if (r1 == q1) t1hit++;
            for (int k = 0; k < TOPK; k++)
                for (int j = 0; j < TOPK; j++) if (q5[k] == r5[j]) { t5hit++; goto nxt; }
nxt:        ;
        }
        printf("%-5s %12.6f %7d/%-2d %7d/%-2d %12.4f\n", CFGS[c].name,
               kl_sum / n_probe_pos, t1hit, n_probe_pos, t5hit, n_probe_pos, md_max);
    }
    printf("\nref=F16  probes=%d  vocab=%d  model=%s\n", n_probe_pos, n_vocab, model_path);
    llama_model_free(model);
    return 0;
}
