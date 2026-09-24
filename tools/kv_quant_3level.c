/*
 * tools/kv_quant_3level.c — cold-KV quantization: 3-level confirmation.
 * L1 teacher-forced KL(ref||q) · L2 greedy continuation match · L3 seeded free-gen divergence.
 * All configs run flash_attn DISABLED (fair kernel) + one Q8/Q4/Q4_1 AUTO row to show kernel effect.
 * Prompts: EN / TH / reasoning. Ref = F16 KV.
 *
 * BUILD: gcc -O2 -I I:/llama/include -I core -o build/kv_quant_3level tools/kv_quant_3level.c \
 *   I:/llama/llama-v040-bin-win-vulkan-x64/llama.dll .../ggml.dll .../ggml-base.dll .../ggml-cpu-x64.dll -lm
 */
#include "llama.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

static const char *PROMPTS[] = {
    "<|im_start|>user\nExplain why the sky is blue in two sentences.<|im_end|>\n"
    "<|im_start|>assistant\nThe sky appears blue because air molecules scatter short-wavelength "
    "blue light more strongly than other colors, a phenomenon called Rayleigh scattering. "
    "At sunset the light travels through more atmosphere, scattering away the blue and "
    "leaving the reds and oranges we see near the horizon.<|im_end|>\n",
    "<|im_start|>user\nอธิบายว่าทำไมข้าวผัดถึงอร่อยในสองประโยค<|im_end|>\n"
    "<|im_start|>assistant\nข้าวผัดอร่อยเพราะความร้อนสูงจากกระทะทำให้เกิดกลิ่นหอมของข้าวคั่วที่เรียกว่ากลิ่นกระทะ "
    "และความสมดุลของรสเค็มหวานจากซีอิ๊วและน้ำตาลทำให้กินได้ไม่เบื่อ<|im_end|>\n",
    "<|im_start|>user\nIf a train leaves at 3pm going 60km/h, when does it arrive 150km away? Reply briefly.<|im_end|>\n"
    "<|im_start|>assistant\nIt arrives at 5:30pm, since 150 divided by 60 is two and a half hours after 3pm.<|im_end|>\n",
};
#define N_PRM (sizeof(PROMPTS)/sizeof(PROMPTS[0]))
#define N_PROBE 24
#define N_GREEDY 20
#define N_FREE 50

typedef struct { const char *name; enum ggml_type tk, tv; int flash; } KVCfg;
static const KVCfg CFGS[] = {
    { "F16",  GGML_TYPE_F16,  GGML_TYPE_F16,  0 },
    { "Q8_A", GGML_TYPE_Q8_0, GGML_TYPE_Q8_0, -1 },  /* kernel-effect rows FIRST: global-state test */
    { "Q4_A", GGML_TYPE_Q4_0, GGML_TYPE_Q4_0, -1 },
    { "Q8_0", GGML_TYPE_Q8_0, GGML_TYPE_Q8_0, 0 },
    { "Q4_0", GGML_TYPE_Q4_0, GGML_TYPE_Q4_0, 0 },
    { "Q4_1", GGML_TYPE_Q4_1, GGML_TYPE_Q4_1, 0 },
    { "K4V8", GGML_TYPE_Q4_0, GGML_TYPE_Q8_0, 0 },  /* mixed: which side tolerates Q4? */
    { "K8V4", GGML_TYPE_Q8_0, GGML_TYPE_Q4_0, 0 },
    { "K4KM", GGML_TYPE_Q4_K, GGML_TYPE_Q4_K, 0 },  /* k-means as KV type: supported? */
};
#define N_CFG (sizeof(CFGS)/sizeof(CFGS[0]))

static int g_force_auto = 0;
static struct llama_context *mk_ctx(struct llama_model *m, const KVCfg *c) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.no_perf = true;
    cp.type_k = c->tk; cp.type_v = c->tv;
    cp.flash_attn_type = g_force_auto ? LLAMA_FLASH_ATTN_TYPE_AUTO : (enum llama_flash_attn_type)c->flash;
    return llama_init_from_model(m, cp);
}

static void decode_all(struct llama_context *ctx, const llama_token *t, int32_t n) {
    for (int32_t off = 0; off < n; off += 512) {
        llama_batch b = llama_batch_init(512, 0, 1);
        int32_t k = n - off > 512 ? 512 : n - off;
        for (int32_t i = 0; i < k; i++) {
            b.token[i] = t[off+i]; b.pos[i] = off+i;
            b.n_seq_id[i] = 1; b.seq_id[i][0] = 0; b.logits[i] = 1;
        }
        b.n_tokens = k;
        if (llama_decode(ctx, b)) { fprintf(stderr, "decode fail\n"); exit(1); }
        llama_batch_free(b);
    }
}

static llama_token step_one(struct llama_context *ctx, llama_token tok, int32_t pos, float *lg_out, int32_t n_vocab) {
    llama_batch b = llama_batch_init(1, 0, 1);
    b.token[0] = tok; b.pos[0] = pos; b.n_seq_id[0] = 1; b.seq_id[0][0] = 0; b.logits[0] = 1;
    b.n_tokens = 1;
    if (llama_decode(ctx, b)) { fprintf(stderr, "step fail\n"); exit(1); }
    llama_batch_free(b);
    const float *lg = llama_get_logits(ctx);
    int bi = 0;
    for (int32_t i = 1; i < n_vocab; i++) if (lg[i] > lg[bi]) bi = i;
    if (lg_out) memcpy(lg_out, lg, (size_t)n_vocab * sizeof(float));
    return (llama_token)bi;
}

static double kl_full(const float *lr, const float *lq, int32_t n, double *maxd) {
    float mr = -FLT_MAX, mq = -FLT_MAX;
    for (int32_t i = 0; i < n; i++) { if (lr[i]>mr) mr=lr[i]; if (lq[i]>mq) mq=lq[i]; }
    double s1 = 0, s2 = 0;
    for (int32_t i = 0; i < n; i++) { s1 += exp(lr[i]-mr); s2 += exp(lq[i]-mq); }
    double lsr = log(s1), lsq = log(s2);
    double kl = 0, md = 0;
    for (int32_t i = 0; i < n; i++) {
        double pr = exp(lr[i]-mr-lsr), qr = exp(lq[i]-mq-lsq);
        double dd = fabs((lr[i]-mr-lsr) - (lq[i]-mq-lsq));
        if (dd > md) md = dd;
        if (pr > 1e-12) kl += pr * (log(pr) - log(qr > 1e-12 ? qr : 1e-12));
    }
    *maxd = md;
    return kl;
}

static int top1(const float *lg, int32_t n) {
    int bi = 0;
    for (int32_t i = 1; i < n; i++) if (lg[i] > lg[bi]) bi = i;
    return bi;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    int force_auto = argc > 2 && !strcmp(argv[2], "auto"); /* all-AUTO process: contamination check */
    g_force_auto = force_auto;
    if (argc > 3) ggml_backend_load_all_from_path(argv[3]); /* e.g. build/cpuonly: CPU backend only */
    else ggml_backend_load_all();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);

    /* tokenize prompts */
    llama_token *PT[N_PRM]; int32_t PN[N_PRM];
    for (size_t p = 0; p < N_PRM; p++) {
        int32_t L = (int32_t)strlen(PROMPTS[p]);
        int32_t need = -llama_tokenize(vocab, PROMPTS[p], L, NULL, 0, true, true);
        PT[p] = malloc((need+16) * sizeof(llama_token));
        PN[p] = llama_tokenize(vocab, PROMPTS[p], L, PT[p], need+16, true, true);
    }

    float *ref_lg = malloc((size_t)N_PRM * N_PROBE * n_vocab * sizeof(float));
    float *cur_lg = malloc((size_t)N_PROBE * n_vocab * sizeof(float));
    llama_token *ref_seq = malloc(N_PRM * N_GREEDY * sizeof(llama_token));
    llama_token *cur_seq = malloc(N_GREEDY * sizeof(llama_token));
    llama_token *ref_free = malloc(N_PRM * N_FREE * sizeof(llama_token));
    llama_token *cur_free = malloc(N_FREE * sizeof(llama_token));

    printf("=== L1 teacher KL (mean over %d probes x %d prompts) · L2 greedy match/%d · L3 free-gen first-diverge/%d ===\n",
           N_PROBE, (int)N_PRM, N_GREEDY, N_FREE);
    printf("%-5s %10s %10s %12s %14s %s\n", "KV", "meanKL", "maxdlog", "greedy1st", "free-div@", "tok0-2(p0)");
    for (size_t c = 0; c < N_CFG; c++) {
        double kl_sum = 0; int kl_n = 0, div_sum = 0, gdiv_sum = 0; double md_max = 0;
        for (size_t p = 0; p < N_PRM; p++) {
            /* ---- L1 + L2 share one context ---- */
            struct llama_context *ctx = mk_ctx(model, &CFGS[c]);
            if (!ctx) { printf("%-5s %10s   (KV type unsupported by this model)\n", CFGS[c].name, "N/A"); goto next_cfg; }
            decode_all(ctx, PT[p], PN[p]);
            for (int q = 0; q < N_PROBE; q++) {
                int32_t pos = PN[p] - N_PROBE + q;
                float *lg = llama_get_logits_ith(ctx, pos);
                if (!lg) return 1;
                if (c == 0) memcpy(ref_lg + ((size_t)p*N_PROBE+q)*n_vocab, lg, (size_t)n_vocab*4);
                else memcpy(cur_lg + (size_t)q*n_vocab, lg, (size_t)n_vocab*4);
            }
            if (c > 0) for (int q = 0; q < N_PROBE; q++) {
                double md = 0;
                kl_sum += kl_full(ref_lg + ((size_t)p*N_PROBE+q)*n_vocab, cur_lg + (size_t)q*n_vocab, n_vocab, &md);
                if (md > md_max) md_max = md;
                kl_n++;
            }
            /* L2 greedy from prompt end */
            llama_token t = PT[p][PN[p]-1];
            /* re-decode last token state already in ctx; continue stepwise */
            for (int s = 0; s < N_GREEDY; s++) {
                t = step_one(ctx, t, PN[p]+s, NULL, n_vocab);
                (c == 0 ? ref_seq + (size_t)p*N_GREEDY : cur_seq)[s] = t;
            }
            if (c > 0) {
                int d = N_GREEDY;
                for (int s = 0; s < N_GREEDY; s++) if (ref_seq[(size_t)p*N_GREEDY+s] != cur_seq[s]) { d = s; break; }
                gdiv_sum += d;
            }
            llama_free(ctx);
            /* ---- L3 fresh context, seeded sampler ---- */
            if (argc > 4 && !strcmp(argv[4], "nol3")) {
                /* skip: no sampler use anywhere in process */
            } else {
            ctx = mk_ctx(model, &CFGS[c]);
            if (!ctx) return 1;
            decode_all(ctx, PT[p], PN[p]);
            struct llama_sampler *sm = llama_sampler_chain_init(llama_sampler_chain_default_params());
            llama_sampler_chain_add(sm, llama_sampler_init_temp(0.8f));
            llama_sampler_chain_add(sm, llama_sampler_init_dist(42));
            t = PT[p][PN[p]-1];
            for (int s = 0; s < N_FREE; s++) {
                llama_batch b = llama_batch_init(1, 0, 1);
                b.token[0] = t; b.pos[0] = PN[p]+s; b.n_seq_id[0] = 1; b.seq_id[0][0] = 0; b.logits[0] = 1;
                b.n_tokens = 1;
                if (llama_decode(ctx, b)) return 1;
                llama_batch_free(b);
                t = llama_sampler_sample(sm, ctx, -1);
                llama_sampler_accept(sm, t);
                (c == 0 ? ref_free + (size_t)p*N_FREE : cur_free)[s] = t;
            }
            llama_sampler_free(sm);
            if (c > 0) {
                int d = N_FREE;
                for (int s = 0; s < N_FREE; s++) if (ref_free[(size_t)p*N_FREE+s] != cur_free[s]) { d = s; break; }
                div_sum += d;
            }
            llama_free(ctx);
            } /* nol3 else-end */
        }
        // fallthrough to print
        if (kl_n == 0) { printf("%-5s %10s %10s %12s %14s   (unsupported)\n", CFGS[c].name, "-", "-", "-", "-"); }
        else if (c == 0) printf("%-5s %10s %10s %12s %14s   (ref)\n", CFGS[c].name, "-", "-", "-", "-");
        else printf("%-5s %10.4f %10.4f %9d/%-3d %9d/%-3d   %d %d %d\n", CFGS[c].name, kl_sum/kl_n, md_max,
                   gdiv_sum/(int)N_PRM, N_GREEDY, div_sum/(int)N_PRM, N_FREE,
                   cur_seq[0], cur_seq[1], cur_seq[2]);
next_cfg: ;
    }
    llama_model_free(model);
    return 0;
}
