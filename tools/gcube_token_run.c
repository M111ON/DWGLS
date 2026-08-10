/*
 * tools/gcube_token_run.c — Level-1 proof: llama.cpp reads tensor data
 * from a .gcube via rail hub (set_tensor_data callback), no re-emit.
 *
 * The model header (KV metadata + tensor index) comes from the .gguf we
 * baked from; the actual weight BYTES come from the .gcube through
 * geo_rail_pull — zero re-emit. If llama.cpp generates identical tokens
 * to a run fed by the original .gguf, the .gcube->llama.cpp path is
 * proven end-to-end.
 *
 * Build (WSL):  gcc -O2 -o gcube_token_run gcube_token_run.c \
 *               -I~/llama.cpp/include -I~/llama.cpp/ggml/include \
 *               -L~/llama.cpp/build_cpu/bin -lllama -lggml-base \
 *               -Wl,-rpath,$HOME/llama.cpp/build_cpu/bin
 * Run:  ./gcube_token_run model.gguf model.gcube [prompt]
 */
#define _CRT_SECURE_NO_WARNINGS
#include "ggml.h"
#include "llama.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "geo_rail_hub.h"

typedef struct {
    GeoRailHub railway;
    int matched;
    int missing;
} HookCtx;

/* set_tensor_data callback: llama.cpp calls this for every tensor it
 * needs initialized. Look the name up in the .gcube and memcpy the
 * bytes straight out of the mmap'd block region. */
static uint64_t g_xor_provided = 0; /* XOR of bytes handed to llama */
static uint64_t g_xor_source   = 0; /* XOR of bytes read from .gcube */
#define N_FREEZE 16
static struct { const uint8_t *data; size_t nb; char name[64]; uint64_t xorv; } g_fz[N_FREEZE];
static int g_fz_n = 0;
static void check_frozen(void) {
    for (int i = 0; i < g_fz_n; i++) {
        uint64_t x = 0;
        for (size_t j = 0; j < g_fz[i].nb; j++) x ^= g_fz[i].data[j];
        printf("  [freeze %2d] %-40s xor_now=%llx %s\n", i, g_fz[i].name,
               (unsigned long long)x, x == g_fz[i].xorv ? "STABLE" : "CORRUPTED");
    }
}
static void provide_tensor(struct ggml_tensor * t, void * ud) {
    HookCtx * h = (HookCtx *)ud;
    const char * name = ggml_get_name(t);
    if (name == NULL || name[0] == '\0') { fprintf(stderr, "  [tensor] unnamed\n"); return; }

    const uint8_t * ptr = NULL; uint32_t n_elems; uint32_t dtype;
    int rc = geo_rail_pull(&h->railway, name, &ptr, &n_elems, &dtype);
    if (rc != 0) {
        /* Tensor exists in llama's schema but not in this GGUF (e.g. output.bias,
         * *.scale imatrix). Plain loader leaves them absent; here we must zero-fill
         * — zero bias/scale is a no-op in the graph, garbage is not. */
        memset(t->data, 0, ggml_nbytes(t));
        fprintf(stderr, "  [tensor] ZERO %s (%lld elems, %s)\n", name,
                (long long)ggml_nelements(t), ggml_type_name(t->type));
        h->missing++;
        return;
    }

    /* .gcube stores the same bytes GGUF does (byte-identical bake):
     * element count must match; byte layout comes from llama's type. */
    if ((uint64_t)n_elems != ggml_nelements(t)) {
        fprintf(stderr, "  [tensor] ELEM MISMATCH %s: gcube=%u llama=%lld\n",
                name, n_elems, (long long)ggml_nelements(t));
        h->missing++; return;
    }
    size_t nb = ggml_nbytes(t);
    memcpy(t->data, ptr, nb);
    /* remember first N tensors we fill for a post-init stability check */
    if (g_fz_n < N_FREEZE) {
        uint64_t x = 0; const uint8_t *p = (const uint8_t *)ptr;
        size_t keep = nb < 8192 ? nb : 8192;
        for (size_t i = 0; i < keep; i++) x ^= p[i + nb - keep]; /* TAIL bytes */
        snprintf(g_fz[g_fz_n].name, sizeof g_fz[g_fz_n].name, "%s", name);
        g_fz[g_fz_n].data = (const uint8_t *)t->data + (nb - keep);
        g_fz[g_fz_n].nb   = keep;
        g_fz[g_fz_n].xorv = x;
        g_fz_n++;
    }
    /* trace: log every 25th tensor name so we see the full set llama asks for */
    if (h->matched % 25 == 0 || nb > (256u << 20))
        fprintf(stderr, "  [trace] #%d %s (%lld elems, %llu B)\n", h->matched, name,
                (long long)ggml_nelements(t), (unsigned long long)nb);
    if (nb <= 256 * 1024 || h->matched < 5) {
        uint64_t x = 0; const uint8_t *p = (const uint8_t *)ptr;
        for (size_t i = 0; i < nb; i++) x ^= p[i];
        uint64_t y = 0; const uint8_t *d2 = (const uint8_t *)t->data;
        for (size_t i = 0; i < nb; i++) y ^= d2[i];
        g_xor_source ^= x; g_xor_provided ^= y;
    }
    h->matched++;
}

/* tiny sampler: greedy argmax of logits — deterministic, seed-free */
static llama_token greedy_sample(struct llama_context * ctx, const struct llama_vocab * vocab) {
    const float * logits = llama_get_logits(ctx);
    int n_vocab = llama_vocab_n_tokens(vocab);
    llama_token best = 0; float best_v = logits[0];
    for (int i = 1; i < n_vocab; i++) if (logits[i] > best_v) { best_v = logits[i]; best = i; }
    /* dump a fixed window incl. the argmax — lets us compare logits
     * numerically between plain-GGUF and .gcube runs */
    fprintf(stderr, "  [logits] best=%d v=%f win[874:878]=%.3f %.3f %.3f %.3f %.3f\n", best, best_v,
            logits[874], logits[875], logits[876], logits[877], logits[878]);
    return best;
}

int main(int argc, char ** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s model.gguf model.gcube [prompt]\n", argv[0]); return 2; }
    const char * ggf_path = argv[1];
    const char * gcb_path = argv[2];
    const char * prompt = (argc > 3) ? argv[3] : "What is 2+2?";

    /* open .gcube once (mmap) */
    HookCtx h; memset(&h, 0, sizeof(h));
    if (geo_rail_open(&h.railway, gcb_path) != 0) { fprintf(stderr, "rail open failed\n"); return 1; }

    /* load GGUF header + tensor index (no_alloc=false so gguf_find_tensor
 * can resolve names → real quant types; without it llama falls back to F32) */
    struct ggml_context * meta_ctx = NULL;
    struct gguf_init_params ip = { .no_alloc = false, .ctx = &meta_ctx };
    struct gguf_context * meta = gguf_init_from_file(ggf_path, ip);
    if (!meta) { fprintf(stderr, "gguf open failed\n"); return 1; }
    printf("metadata: %lld tensors\n", (long long)gguf_get_n_tensors(meta));

    llama_backend_init();

    /* model params: explicit no mmap — our callback provides bytes.
     * If GCUBE_SKIP=1: plain GGUF load (no rail) — isolates client vs data. */
    struct llama_model_params mpar = llama_model_default_params();
    struct llama_model * model = NULL;
    if (getenv("GCUBE_SKIP")) {
        model = llama_model_load_from_file(ggf_path, mpar);
        if (!model) { fprintf(stderr, "plain GGUF load failed\n"); return 1; }
        printf("model loaded (plain GGUF)\n");
    } else {
        mpar.load_mode  = LLAMA_LOAD_MODE_NONE;
        /* use_extra_bufts default (true) — matches plain-load params exactly */
        model = llama_model_init_from_user(meta, provide_tensor, &h, mpar);
        if (!model) { fprintf(stderr, "model init failed (matched=%d missing=%d)\n", h.matched, h.missing); return 1; }
        printf("model loaded: %d tensors via .gcube (%d missing)\n", h.matched, h.missing);
        printf("xor(callback-time): source=%llx provided=%llx %s\n",
               (unsigned long long)g_xor_source, (unsigned long long)g_xor_provided,
               g_xor_source == g_xor_provided ? "OK" : "DIFF");
        check_frozen();   /* re-read the frozen tensor from t->data now */
    }

    /* context */
    struct llama_context_params cpar = llama_context_default_params();
    cpar.n_ctx = 64; cpar.n_batch = 64; cpar.n_threads = 8;
    struct llama_context * ctx = llama_init_from_model(model, cpar);
    if (!ctx) { fprintf(stderr, "ctx init failed\n"); return 1; }
    const struct llama_vocab * vocab = llama_model_get_vocab(model);
    if (!vocab) { fprintf(stderr, "no vocab\n"); return 1; }
    printf("vocab: %d tokens\n", llama_vocab_n_tokens(vocab));

    /* tokenize prompt — two-pass: query size (negative = needed), then fill */
    int n_prompt = llama_tokenize(vocab, prompt, strlen(prompt), NULL, 0, true, false);
    if (n_prompt < 0) n_prompt = -n_prompt;
    if (n_prompt == 0) { fprintf(stderr, "tokenize failed\n"); return 1; }
    llama_token * toks = (llama_token *)malloc((size_t)(n_prompt + 32) * sizeof(llama_token));
    int n2 = llama_tokenize(vocab, prompt, strlen(prompt), toks, n_prompt + 32, true, false);
    if (n2 < 0 || n2 > n_prompt + 32) { fprintf(stderr, "tokenize fill failed (%d)\n", n2); return 1; }
    n_prompt = n2;
    printf("tokenized: %d tokens\n", n_prompt);

    /* loop: feed prompt, then greedy-generate a few tokens */
    int n_total = 0;
    if (llama_decode(ctx, llama_batch_get_one(toks, n_prompt))) { fprintf(stderr, "decode fail\n"); return 1; }
    n_total += n_prompt;
    printf("prompt: %s\n", prompt);

    for (int i = 0; i < 24; i++) {
        llama_token tok = greedy_sample(ctx, vocab);
        char buf[64]; int n = llama_token_to_piece(vocab, tok, buf, sizeof(buf), 0, false);
        buf[n < 0 ? 0 : n] = '\0';
        printf("%s", buf); fflush(stdout);
        if (tok == llama_vocab_eos(vocab)) break;
        llama_token one = tok;
        if (llama_decode(ctx, llama_batch_get_one(&one, 1))) break;
        n_total++;
    }
    printf("\n");

    llama_free(ctx);
    llama_free_model(model);
    gguf_free(meta);
    geo_rail_close(&h.railway);
    printf("DONE (tokens=%d)\n", n_total);
    return 0;
}