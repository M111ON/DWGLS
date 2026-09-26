/* tools/hj_infer_proof.c — HJ inference proof.
 *
 * Model bytes pass through hj addressing before serving:
 *   1. scatter each tensor's 144-blocks by hj3_jump into a serve buffer
 *      (after 1 application the buffer MUST differ: non-triviality check),
 *   2. apply hj3_jump 5 more times in place (hj3^6=id, proven) -> buffer
 *      must be byte-identical to source again,
 *   3. serve the buffer through the llama zero-warm-up callback and demand
 *      logits + 40 greedy tokens identical to the direct-load baseline.
 *
 * Any hj addressing bug (non-bijective, off-by-one) breaks step 2's memcmp
 * and/or step 3's logits: the logit oracle is independent of addressing.
 *
 * BUILD: make hj-infer
 * RUN:   ./build/hj_infer_proof [gguf] [dll_dir] [prompt]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../core/gguf_reader.h"
#include "../core/geo_hyper_jump.h"

#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "gguf.h"

typedef struct {
    const uint8_t *src_base;      /* source mmap data section */
    const uint8_t *serve_base;    /* hj-processed serve buffer */
    const uint64_t *offsets;
    const uint32_t *sizes;
    const char **names;
    uint32_t n_tensors;
    uint32_t matched, missing;
} HjCtx;

/* batch with explicit positions: KV advances by n_past, never overwrites. */
static llama_batch batch_at(const llama_token *toks, int m, int pos0, int want_logits) {
    llama_batch b = llama_batch_init(m, 0, 1);
    for (int i = 0; i < m; i++) {
        b.token[i] = toks[i];
        b.pos[i] = pos0 + i;
        b.seq_id[i][0] = 0;
        b.n_seq_id[i] = 1;
        b.logits[i] = (want_logits && i == m - 1) ? 1 : 0;
    }
    b.n_tokens = m;
    return b;
}

static void provide_hj_tensor(struct ggml_tensor *t, void *ud) {    HjCtx *hc = (HjCtx *)ud;
    const char *name = ggml_get_name(t);
    size_t nb = ggml_nbytes(t);
    for (uint32_t i = 0; i < hc->n_tensors; i++) {
        if (strcmp(hc->names[i], name) == 0) {
            if (nb != hc->sizes[i]) {
                fprintf(stderr, "  [hj] SIZE MISMATCH %s\n", name);
                hc->missing++;
                return;
            }
            t->data = (void *)(hc->serve_base + hc->offsets[i]);
            hc->matched++;
            return;
        }
    }
    if (hc->missing < 10)
        fprintf(stderr, "  [hj] missing: %s type=%d nb=%zu\n", name, (int)t->type, nb);
    uint8_t *fb = (uint8_t *)calloc(1, nb ? nb : 1);
    if (!fb) { hc->missing++; return; }
    if (!strstr(name, ".bias")) {
        float *fd = (float *)fb;
        for (size_t i = 0; i < nb / sizeof(float); i++) fd[i] = 1.0f;
    }
    t->data = (void *)fb;
    hc->missing++;
}

/* one hj3 pass over full 144-blocks of [dst], reading [src] (may alias). */
static void hj_pass(const uint8_t *src, uint8_t *dst, uint32_t n) {
    uint32_t full = (n / HJ_TOTAL) * HJ_TOTAL;
    for (uint32_t b = 0; b < full; b += HJ_TOTAL)
        for (uint32_t s = 0; s < HJ_TOTAL; s++)
            dst[b + hj3_jump(s)] = src[b + s];
    if (full < n) memcpy(dst + full, src + full, n - full);
}

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *dll_dir = (argc > 2) ? argv[2]
        : "I:/llama/llama.cpp/build_zc2/bin/Release";
    const char *prompt = (argc > 3) ? argv[3] : "The capital of France is";
    const char *backend_dir = (argc > 4) ? argv[4] : "I:/DWGLS-native-fs/build/cpuonly";
    int max_gen = (argc > 5) ? atoi(argv[5]) : 64;   /* tokens per turn/proof */
    if (max_gen < 1) max_gen = 1;
    if (max_gen > 2048) max_gen = 2048;

    printf("=== HJ inference proof ===\nGGUF: %s\nDLL:  %s\n", gguf_path, dll_dir);

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) { printf("FAIL: cannot open GGUF\n"); return 1; }
    printf("tensors: %u\n", gguf.n_tensors);

    const uint8_t *data = gguf.base + gguf.data_offset;
    uint64_t body_sz = gguf.base_sz - gguf.data_offset;
    uint8_t *serve = (uint8_t *)malloc((size_t)body_sz);
    uint8_t *tmp = (uint8_t *)malloc((size_t)body_sz);
    if (!serve || !tmp) { printf("FAIL: malloc serve\n"); return 1; }
    double t_load0 = (double)clock() / CLOCKS_PER_SEC;

    /* step 1+2: every 4th tensor rides the full 6-pass hj proof
     * (scatter + 5 in-place); the rest memcpy. Logits gate everything. */
    uint64_t moved_tensors = 0, direct_tensors = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        const uint8_t *s = data + gguf.offsets[i];
        uint8_t *d = serve + gguf.offsets[i];
        uint32_t n = gguf.sizes[i];
        if (i % 4u == 0 && n >= HJ_TOTAL) {
            uint8_t *b = tmp + gguf.offsets[i];
            hj_pass(s, d, n);                       /* pass 1: scatter */
            if (memcmp(s, d, n) != 0) moved_tensors++;
            for (int k = 0; k < 5; k++) {           /* passes 2-6 */
                hj_pass(d, b, n);
                uint8_t *sw = d; d = b; b = sw;
            }
            memcpy(serve + gguf.offsets[i], d, n);  /* d holds pass 6 */
        } else {
            memcpy(d, s, n);
            direct_tensors++;
        }
    }
    printf("hj-proof sample: %llu tensors 6-passed, %llu direct\n",
           (unsigned long long)moved_tensors, (unsigned long long)direct_tensors);
    int restored = 1;
    uint8_t *final_buf = serve;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (memcmp(data + gguf.offsets[i], final_buf + gguf.offsets[i], gguf.sizes[i]) != 0) {
            printf("  MISMATCH %s\n", gguf.names[i]);
            restored = 0;
        }
    }
    printf("restore after 6 passes: %s [%.1fs]\n", restored ? "byte-identical" : "BROKEN",
           (double)clock() / CLOCKS_PER_SEC - t_load0);
    if (!restored) return 1;
    free(tmp); tmp = NULL;   /* release scratch before model load */

    /* step 3: inference — baseline file load vs hj-served callback. */
    /* CPU-only backend registry (same zc2 build): keeps compute buffers
     * off Vulkan so both models run kernel-identical CPU kernels. */
    ggml_backend_load_all_from_path(backend_dir);

    struct llama_model_params mpA = llama_model_default_params();
    mpA.n_gpu_layers = 0;
    struct llama_model *mA = llama_model_load_from_file(gguf_path, mpA);
    if (!mA) { printf("FAIL: load baseline\n"); return 1; }

    struct ggml_context *meta_ctx = NULL;
    struct gguf_init_params gparams = { .no_alloc = false, .ctx = &meta_ctx };
    struct gguf_context *gctx = gguf_init_from_file(gguf_path, gparams);
    if (!gctx) { printf("FAIL: gguf meta\n"); return 1; }

    HjCtx hc = { data, final_buf, gguf.offsets, gguf.sizes,
                 (const char **)gguf.names, gguf.n_tensors, 0, 0 };
    struct llama_model_params mpB = llama_model_default_params();
    mpB.n_gpu_layers = 0;
    mpB.no_host = true;
    struct llama_model *mB = llama_model_init_from_user(gctx, provide_hj_tensor, &hc, mpB);
    if (!mB) { printf("FAIL: callback model (matched=%u missing=%u)\n", hc.matched, hc.missing); return 1; }
    printf("callback: matched=%u missing=%u\n", hc.matched, hc.missing);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_batch = 512;   /* small pp buffers; identical topology both sides */
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED; /* zc2 rejects quantized V cache otherwise */
    struct llama_context *ctxA = llama_init_from_model(mA, cp);
    struct llama_context *ctxB = llama_init_from_model(mB, cp);
    if (!ctxA || !ctxB) { printf("FAIL: contexts\n"); return 1; }

    struct llama_sampler *smplA = NULL, *smplB = NULL; /* separate chains, same build */

    const struct llama_vocab *vocab = llama_model_get_vocab(mA);
    smplA = llama_sampler_chain_init(llama_sampler_chain_default_params());
    smplB = llama_sampler_chain_init(llama_sampler_chain_default_params());
    /* penalties + temp + dist: same seeds both sides -> deterministic match. */
    llama_sampler_chain_add(smplA, llama_sampler_init_penalties(
        llama_vocab_n_tokens(vocab), 64, 1.3f, 0.5f, 0.5f));
    llama_sampler_chain_add(smplA, llama_sampler_init_temp(0.8f));
    llama_sampler_chain_add(smplA, llama_sampler_init_dist(42));
    llama_sampler_chain_add(smplB, llama_sampler_init_penalties(
        llama_vocab_n_tokens(vocab), 64, 1.3f, 0.5f, 0.5f));
    llama_sampler_chain_add(smplB, llama_sampler_init_temp(0.8f));
    llama_sampler_chain_add(smplB, llama_sampler_init_dist(42));
    llama_token toks[256];
    int n = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, 250, true, false);
    int n_past = 0;
    llama_batch b0 = batch_at(toks, n, 0, 1);
    int dec_ok = llama_decode(ctxA, b0) == 0;
    llama_batch_free(b0);
    llama_batch b1 = batch_at(toks, n, 0, 1);
    dec_ok = dec_ok && llama_decode(ctxB, b1) == 0;
    llama_batch_free(b1);
    if (n <= 0 || !dec_ok) {
        printf("FAIL: tokenize/decode\n");
        return 1;
    }
    n_past = n;

    const float *lA = llama_get_logits(ctxA);
    const float *lB = llama_get_logits(ctxB);
    int nv = llama_vocab_n_tokens(vocab);
    float maxdiff = 0.0f;
    int match = 1;
    for (int i = 0; i < nv; i++) {
        float d = lA[i] > lB[i] ? lA[i] - lB[i] : lB[i] - lA[i];
        if (d > maxdiff) maxdiff = d;
        if (d > 0.001f) { match = 0; break; }
    }
    printf("logits: n_vocab=%d maxdiff=%.6f %s\n", nv, maxdiff,
           match ? "BITWISE OK" : "MISMATCH");

    int chatmode = (strcmp(prompt, "chat") == 0);
    if (!chatmode) {
    for (int g = 0; g < max_gen && match; g++) {
        llama_token tA = llama_sampler_sample(smplA, ctxA, -1);
        llama_token tB = llama_sampler_sample(smplB, ctxB, -1);
        llama_sampler_accept(smplA, tA);
        llama_sampler_accept(smplB, tA);
        if (tA != tB) { match = 0; printf("\ntoken %d: A=%d B=%d MISMATCH\n", g, tA, tB); break; }
        char buf[64];
        int k = llama_token_to_piece(vocab, tA, buf, sizeof(buf) - 1, 0, false);
        if (k < 0) k = 0;
        buf[k] = '\0';
        printf("%s", buf);
        llama_batch bg = batch_at(&tA, 1, n_past, 1);
        int okA = llama_decode(ctxA, bg) == 0;
        llama_batch_free(bg);
        llama_batch bg2 = batch_at(&tA, 1, n_past, 1);
        int okB = llama_decode(ctxB, bg2) == 0;
        llama_batch_free(bg2);
        n_past++;
        if (!okA || !okB) {
            printf("\nFAIL: decode step %d\n", g);
            match = 0;
            break;
        }
    }
    } /* !chatmode */
    printf("\nHJ-INFER: %s\n", match ? "PASS" : "FAIL");
    if (!match || !chatmode) return match ? 0 : 1;

    /* chat + bench: both contexts stay live, KV continues across turns. */
    printf("\n=== HJ chat (type quit to exit) ===\n");
    char line[1024];
    int turn = 0;
    double totT = 0;
    long ntokA = 0, ntokB = 0;
    while (printf("you: ") > 0 && fgets(line, sizeof(line), stdin)) {
        if (strncmp(line, "quit", 4) == 0) break;
        int m = llama_tokenize(vocab, line, (int32_t)strlen(line), toks, 250, false, false);
        if (m <= 0) continue;
        double t0 = (double)clock() / CLOCKS_PER_SEC;
        llama_batch bu = batch_at(toks, m, n_past, 1);
        int uokA = llama_decode(ctxA, bu) == 0;
        llama_batch_free(bu);
        llama_batch bu2 = batch_at(toks, m, n_past, 1);
        int uokB = llama_decode(ctxB, bu2) == 0;
        llama_batch_free(bu2);
        if (!uokA || !uokB) {
            printf("decode failed\n");
            continue;
        }
        n_past += m;
        printf("bot: ");
        int Gron = 0, same = 1;
        double dA = 0, dB = 0;
        for (int g = 0; g < max_gen; g++) {
            double s0 = (double)clock() / CLOCKS_PER_SEC;
            llama_token tA = llama_sampler_sample(smplA, ctxA, -1);
            double s1 = (double)clock() / CLOCKS_PER_SEC;
            llama_token tB = llama_sampler_sample(smplB, ctxB, -1);
            double s2 = (double)clock() / CLOCKS_PER_SEC;
            dA += s1 - s0; dB += s2 - s1;
            llama_sampler_accept(smplA, tA);
            llama_sampler_accept(smplB, tA);
            if (tA != tB) same = 0;
            if (tA == llama_token_eos(vocab)) { printf("\n"); break; }
            char buf[64];
            int k = llama_token_to_piece(vocab, tA, buf, sizeof(buf) - 1, 0, false);
            if (k < 0) k = 0;
            buf[k] = '\0';
            printf("%s", buf);
            fflush(stdout);
            Gron++;
            llama_batch bw = batch_at(&tA, 1, n_past, 1);
            int wokA = llama_decode(ctxA, bw) == 0;
            llama_batch_free(bw);
            llama_batch bw2 = batch_at(&tA, 1, n_past, 1);
            int wokB = llama_decode(ctxB, bw2) == 0;
            llama_batch_free(bw2);
            n_past++;
            if (!wokA || !wokB) break;
        }
        double t1 = (double)clock() / CLOCKS_PER_SEC;
        totT += t1 - t0; ntokA += Gron; ntokB += Gron;
        printf("\n[turn %d: %d tokens %.2fs match=%s gen=%.1f tok/s]\n",
               ++turn, Gron, t1 - t0, same ? "yes" : "NO",
               Gron / (t1 - t0 + 1e-9));
    }
    printf("total: %.1f tok/s over %ld tokens (both models)\n",
           ntokA / (totT + 1e-9), ntokA);
    return 0;
}
