/* isolation: does llama_model_init_from_user (user path) match file load?
 * meta from the ORIGINAL file via gguf_init_from_file; callback from mmap. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "../core/gguf_reader.h"

typedef struct { const GgufReader *r; uint32_t matched, missing; } Ctx;

/* Q8_0 → F32: blocks of 32 elems, [f16 d][32×int8 q] — uses ggml's own fp16→fp32 */
static void dequant_q8_0(const uint8_t *src, float *dst, size_t n) {
    for (size_t k = 0; k < n / 32; k++) {
        const uint8_t *blk = src + k * 34;
        uint16_t h; memcpy(&h, blk, 2);
        float d = ggml_fp16_to_fp32(h);
        const int8_t *q = (const int8_t *)(blk + 2);
        for (int i = 0; i < 32; i++) dst[k * 32 + i] = (float)q[i] * d;
    }
}
static void cb(struct ggml_tensor *t, void *ud) {
    Ctx *c = (Ctx *)ud;
    const char *name = ggml_get_name(t);
    for (uint32_t i = 0; i < c->r->n_tensors; i++) {
        if (strcmp(c->r->names[i], name) == 0) {
            size_t nb = ggml_nbytes(t);
            if (nb == c->r->sizes[i]) {
                memcpy(t->data, c->r->base + c->r->data_offset + c->r->offsets[i], nb);
                c->matched++;
                return;
            }
        }
    }
    /* optional schema tensors absent from the GGUF — mirror file-load:
     * *.bias → 0, *.scale/*.input_scale → 1.0 (mul-by-one no-op),
     * output.weight → token_embd.weight (shared embedding head),
     * rope_freqs → 1.0 (ggml computes theta itself when absent). */
    if (getenv("ISO_DEBUG") && c->missing < 12)
        printf("  [missing] %s type=%d (%zu B)\n", name, (int)t->type, ggml_nbytes(t));
    if (getenv("ISO_TYPES") && (strcmp(name, "output.weight") == 0 || strcmp(name, "token_embd.weight") == 0))
        printf("  [type] %s type=%d nb=%zu\n", name, (int)t->type, ggml_nbytes(t));
    if (strcmp(name, "output.weight") == 0) {
        for (uint32_t i = 0; i < c->r->n_tensors; i++)
            if (strcmp(c->r->names[i], "token_embd.weight") == 0) {
                /* user path requests the head as F32; dequant from the stored Q8_0 */
                size_t n = ggml_nelements(t);
                if (n * 4 == ggml_nbytes(t) && c->r->sizes[i] == n / 32 * 34) {
                    dequant_q8_0(c->r->base + c->r->data_offset + c->r->offsets[i],
                                 (float *)t->data, n);
                    return;
                }
            }
    }
    float *fd = (float *)t->data;
    size_t nf = ggml_nbytes(t) / sizeof(float);
    if (strstr(name, ".bias")) {
        memset(t->data, 0, ggml_nbytes(t));
    } else {
        for (size_t i = 0; i < nf; i++) fd[i] = 1.0f;
    }
    c->missing++;
}
static llama_token *generate(struct llama_model *model, const char *prompt, int n_gen, int *n_out) {
    *n_out = 0;
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512; cp.n_batch = 64; cp.n_threads = 4; cp.n_threads_batch = 4;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) return NULL;
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    llama_token eos = llama_vocab_eos(vocab);
    int np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    if (np < 0) np = -np;
    llama_token *toks = (llama_token *)malloc((size_t)(np + 1) * sizeof(llama_token));
    np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, np, true, false);
    if (np < 0) np = -np;
    llama_token *out = (llama_token *)malloc((size_t)(n_gen + 1) * sizeof(llama_token));
    int total = 0;
    if (llama_decode(ctx, llama_batch_get_one(toks, np)) != 0) { free(toks); free(out); llama_free(ctx); return NULL; }
    for (int i = 0; i < n_gen; i++) {
        const float *logits = (i == 0) ? llama_get_logits_ith(ctx, np - 1) : llama_get_logits(ctx);
        llama_token best = 0; float bv = logits[0];
        for (int t = 1; t < n_vocab; t++) if (logits[t] > bv) { bv = logits[t]; best = (llama_token)t; }
        out[total++] = best;
        if (best == eos) break;
        if (llama_decode(ctx, llama_batch_get_one(&best, 1)) != 0) break;
    }
    free(toks); llama_free(ctx);
    *n_out = total;
    return out;
}
int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = (argc > 2) ? argv[2] : "The capital of France is";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 6;
    llama_backend_init();
    ggml_backend_load_all_from_path("I:/llama/llama-b9733-bin-win-vulkan-x64");
    GgufReader r;
    if (gguf_open(gguf, &r) != 0) { printf("open fail\n"); return 1; }
    struct ggml_context *meta_ctx = NULL;
    struct gguf_init_params ip = { .no_alloc = false, .ctx = &meta_ctx };
    struct gguf_context *meta = gguf_init_from_file(gguf, ip);
    if (!meta) { printf("gguf_init fail\n"); return 1; }
    Ctx c = { &r, 0, 0 };
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *m = llama_model_init_from_user(meta, cb, &c, mp);
    printf("user-path: %s (matched %u, missing %u)\n", m ? "loaded" : "FAIL", c.matched, c.missing);
    if (m) {
        const struct llama_vocab *vocab = llama_model_get_vocab(m);
        printf("vocab %d tokens; [0..3] = \"%s\" \"%s\" \"%s\" \"%s\"\n",
               llama_vocab_n_tokens(vocab), llama_vocab_get_text(vocab, 0),
               llama_vocab_get_text(vocab, 1), llama_vocab_get_text(vocab, 2),
               llama_vocab_get_text(vocab, 3));
        int n1 = 0, n2 = 0;
        llama_token *a = generate(m, prompt, n_gen, &n1);
        llama_model_free(m);
        struct llama_model_params mf = llama_model_default_params();
        mf.n_gpu_layers = 0;
        struct llama_model *f = llama_model_load_from_file(gguf, mf);
        llama_token *b = generate(f, prompt, n_gen, &n2);
        int ok = (n1 == n2);
        if (ok) for (int i = 0; i < n1; i++) if (a[i] != b[i]) { ok = 0; break; }
        printf("user-path vs file-load streams: %s\n", ok ? "IDENTICAL" : "DIFFER");
        if (!ok) {
            printf("  user: "); for (int i = 0; i < n1; i++) printf(" %d", a[i]); printf("\n");
            printf("  file: "); for (int i = 0; i < n2; i++) printf(" %d", b[i]); printf("\n");
        }
        free(a); free(b); llama_model_free(f);
    }
    gguf_free(meta);
    llama_backend_free();
    return 0;
}
