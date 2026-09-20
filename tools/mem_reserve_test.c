/* mem_reserve_test.c — VirtualAlloc(MEM_RESERVE) + per-tensor MEM_COMMIT
 *
 * Proves: reserve full model address space (zero physical RAM),
 * commit only the tensors that inference needs, measure RSS delta.
 *
 * Usage: mem_reserve_test <model.gguf> [prompt] [n_gen]
 *
 * Build: gcc -O2 -w -I llama.cpp/include -I llama.cpp/ggml/include \
 *        tools/mem_reserve_test.c -o build/mem_reserve_test.exe \
 *        llama.cpp/build_zc2/bin/Release/llama.dll -lpsapi -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <psapi.h>
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "gguf.h"
#include "../core/gguf_reader.h"

/* RSS measurement */
static size_t get_rss_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / (1024 * 1024);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   PHASE 1: mmap baseline — standard llama load via file
   ═══════════════════════════════════════════════════════════════════════════ */
static void phase_mmap(const char *gguf_path, const char *prompt, int n_gen) {
    fprintf(stderr, "\n=== Phase 1: mmap baseline ===\n");
    size_t rss0 = get_rss_mb();
    fprintf(stderr, "RSS before load: %zu MB\n", rss0);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(gguf_path, mp);
    if (!model) { fprintf(stderr, "load FAIL\n"); return; }
    size_t rss1 = get_rss_mb();
    fprintf(stderr, "RSS after load: %zu MB (delta +%zu MB)\n", rss1, rss1 - rss0);

    /* Generate */
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512; cp.n_batch = 256; cp.n_threads = 4;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { llama_model_free(model); return; }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int np = llama_tokenize(vocab, prompt, (int)strlen(prompt), NULL, 0, true, false);
    if (np < 0) np = -np;
    llama_token *toks = malloc(np * sizeof(llama_token));
    llama_tokenize(vocab, prompt, (int)strlen(prompt), toks, np, true, false);

    llama_decode(ctx, llama_batch_get_one(toks, np));
    for (int i = 0; i < n_gen; i++) {
        const float *logits = (i == 0) ? llama_get_logits_ith(ctx, np - 1) : llama_get_logits(ctx);
        llama_token best = 0; float bv = logits[0];
        for (int t = 1; t < llama_vocab_n_tokens(vocab); t++)
            if (logits[t] > bv) { bv = logits[t]; best = (llama_token)t; }
        if (llama_decode(ctx, llama_batch_get_one(&best, 1)) != 0) break;
    }
    size_t rss2 = get_rss_mb();
    fprintf(stderr, "RSS after inference: %zu MB (delta +%zu MB)\n", rss2, rss2 - rss1);

    free(toks);
    llama_free(ctx);
    llama_model_free(model);
    fprintf(stderr, "mmap RSS: baseline=%zu  loaded=%zu  infer=%zu\n", rss0, rss1, rss2);
}

/* ═══════════════════════════════════════════════════════════════════════════
   PHASE 2: MEM_RESERVE + per-tensor COMMIT — custom loader
   ═══════════════════════════════════════════════════════════════════════════ */

/* Per-tensor callback context */
typedef struct {
    GgufReader    *reader;         /* source GGUF reader (mmap'd) */
    uint32_t       n_commits;      /* tensors committed so far */
    uint64_t       bytes_committed; /* bytes committed so far */
} ReserveCtx;

static void dequant_q8_0(const uint8_t *src, float *dst, size_t n) {
    for (size_t k = 0; k < n / 32; k++) {
        const uint8_t *blk = src + k * 34;
        uint16_t h; memcpy(&h, blk, 2);
        float d = ggml_fp16_to_fp32(h);
        const int8_t *q = (const int8_t *)(blk + 2);
        for (int i = 0; i < 32; i++) dst[k * 32 + i] = (float)q[i] * d;
    }
}

static void reserve_cb(struct ggml_tensor *t, void *ud) {
    ReserveCtx *c = (ReserveCtx *)ud;
    const char *name = ggml_get_name(t);

    for (uint32_t i = 0; i < c->reader->n_tensors; i++) {
        if (strcmp(c->reader->names[i], name) != 0) continue;

        size_t nb = ggml_nbytes(t);
        if (nb != c->reader->sizes[i]) {
            fprintf(stderr, "  [skip] %s: size mismatch (%zu vs %zu)\n", name, nb, c->reader->sizes[i]);
            return;
        }

        const uint8_t *src = c->reader->base + c->reader->data_offset + c->reader->offsets[i];
        memcpy(t->data, src, nb);

        c->n_commits++;
        c->bytes_committed += nb;
        return;
    }
    /* Missing tensor — mirror iso_user_path.c behavior */
    if (strcmp(name, "output.weight") == 0) {
        for (uint32_t i = 0; i < c->reader->n_tensors; i++) {
            if (strcmp(c->reader->names[i], "token_embd.weight") == 0) {
                size_t n = ggml_nelements(t);
                if (n * 4 == ggml_nbytes(t) && c->reader->sizes[i] == n / 32 * 34) {
                    dequant_q8_0(c->reader->base + c->reader->data_offset + c->reader->offsets[i],
                                 (float *)t->data, n);
                    return;
                }
            }
        }
    }
    if (strstr(name, ".bias")) {
        memset(t->data, 0, ggml_nbytes(t));
    } else {
        float *fd = (float *)t->data;
        size_t nf = ggml_nbytes(t) / sizeof(float);
        for (size_t i = 0; i < nf; i++) fd[i] = 1.0f;
    }
}

static void phase_reserve(const char *gguf_path, const char *prompt, int n_gen) {
    fprintf(stderr, "\n=== Phase 2: MEM_RESERVE + per-tensor COMMIT ===\n");
    fflush(stderr);

    /* Open source GGUF */
    GgufReader reader;
    if (gguf_open(gguf_path, &reader) != 0) {
        fprintf(stderr, "gguf_open fail\n");
        return;
    }
    fprintf(stderr, "GGUF: %u tensors, data section %llu bytes\n",
            reader.n_tensors, (unsigned long long)reader.data_offset);

    uint64_t data_size = 0;
    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        uint64_t end = reader.offsets[i] + reader.sizes[i];
        if (end > data_size) data_size = end;
    }
    fprintf(stderr, "Data region: %llu bytes (%.1f MB)\n",
            (unsigned long long)data_size, data_size / 1e6);

    size_t rss0 = get_rss_mb();
    fprintf(stderr, "RSS before load: %zu MB\n", rss0);
    fflush(stderr);

    /* Init llama with callback — needs gguf_context from gguf_init_from_file */
    struct ggml_context *meta_ctx = NULL;
    struct gguf_init_params gip = { .no_alloc = false, .ctx = &meta_ctx };
    struct gguf_context *guf = gguf_init_from_file(gguf_path, gip);
    if (!guf) {
        fprintf(stderr, "gguf_init_from_file FAIL\n");
        gguf_close(&reader);
        return;
    }

    ReserveCtx ctx = { .reader = &reader };

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_init_from_user(guf, reserve_cb, &ctx, mp);
    if (!model) {
        fprintf(stderr, "model init FAIL\n"); fflush(stderr);
        gguf_close(&reader);
        return;
    }

    size_t rss1 = get_rss_mb();
    fprintf(stderr, "RSS after load: %zu MB (delta +%zu MB)\n", rss1, rss1 - rss0);
    fprintf(stderr, "Committed: %u tensors, %llu bytes (%.1f MB)\n",
            ctx.n_commits, (unsigned long long)ctx.bytes_committed, ctx.bytes_committed / 1e6);
    fflush(stderr);

    /* Generate */
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512; cp.n_batch = 256; cp.n_threads = 4;
    struct llama_context *lctx = llama_init_from_model(model, cp);
    if (!lctx) { llama_model_free(model); gguf_close(&reader); return; }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int np = llama_tokenize(vocab, prompt, (int)strlen(prompt), NULL, 0, true, false);
    if (np < 0) np = -np;
    llama_token *toks = malloc(np * sizeof(llama_token));
    llama_tokenize(vocab, prompt, (int)strlen(prompt), toks, np, true, false);

    llama_decode(lctx, llama_batch_get_one(toks, np));

    /* Print top-1 token text for verification */
    const float *logits = llama_get_logits_ith(lctx, np - 1);
    llama_token best = 0; float bv = logits[0];
    int n_vocab = llama_vocab_n_tokens(vocab);
    for (int t = 1; t < n_vocab; t++)
        if (logits[t] > bv) { bv = logits[t]; best = (llama_token)t; }
    fprintf(stderr, "top1: token=%d logit=%.4f text=\"%s\"\n",
            best, bv, llama_vocab_get_text(vocab, best));

    for (int i = 0; i < n_gen; i++) {
        const float *lg = (i == 0) ? llama_get_logits_ith(lctx, np - 1) : llama_get_logits(lctx);
        best = 0; bv = lg[0];
        for (int t = 1; t < n_vocab; t++)
            if (lg[t] > bv) { bv = lg[t]; best = (llama_token)t; }
        fprintf(stdout, "%s", llama_vocab_get_text(vocab, best));
        if (llama_decode(lctx, llama_batch_get_one(&best, 1)) != 0) break;
    }
    fprintf(stdout, "\n");

    size_t rss2 = get_rss_mb();
    fprintf(stderr, "RSS after inference: %zu MB\n", rss2);

    fprintf(stderr, "\n=== SUMMARY ===\n");
    fprintf(stderr, "Callback RSS load: %zu MB\n", rss1);
    fprintf(stderr, "Callback RSS infer: %zu MB\n", rss2);
    fflush(stderr);

    free(toks);
    llama_free(lctx);
    llama_model_free(model);
    gguf_free(guf);
    gguf_close(&reader);
}

/* ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = (argc > 2) ? argv[2] : "The capital of France is";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 5;

    llama_backend_init();
    ggml_backend_load_all_from_path("I:/llama/llama.cpp/build_zc2/bin/Release");
    ggml_backend_load_all();

    fprintf(stderr, "Model: %s\n", gguf);
    fprintf(stderr, "Prompt: \"%s\" n_gen=%d\n", prompt, n_gen);

    /* Phase 2 only — MEM_RESERVE path */
    phase_reserve(gguf, prompt, n_gen);

    llama_backend_free();
    return 0;
}
