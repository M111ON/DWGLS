/*
 * tools/sid_kv_resume.c — SID drives llama resume (cold-KV chain via SID).
 * ═══════════════════════════════════════════════════════════════════════
 * Claim: bytes that SID returns can resume live inference. The tool moves
 *   live base blob + token suffix → TensorMemStore (.tmem save/load) →
 *   llama_state_seq_set_data on a fresh ctx → teacher-decode suffix →
 *   bit-exact vs the live path that never touched SID.
 * Non-goals: SID core untouched, llama untouched, no KV-graph surgery.
 * Config mirrors tools/kv_cold_delta.c (F16, flash DISABLED, ngl=0) so the
 * only new variable is "bytes travelled through SID".
 *
 * RUN: ./build/sid_kv_resume_zc2 <model.gguf> [outdir] [backend_dir]
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include "adaptive_route_sid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *T_BASE =
    "<|im_start|>user\nWhat is the capital of France? Answer in one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe capital of France is Paris.<|im_end|>\n";
static const char *T_S1 =
    "<|im_start|>user\nNow tell me one famous landmark there, one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe Eiffel Tower is Paris's most famous landmark.<|im_end|>\n";

static struct llama_context *mk_ctx(struct llama_model *m) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 1024; cp.n_batch = 128; cp.no_perf = true;
    cp.type_k = GGML_TYPE_F16; cp.type_v = GGML_TYPE_F16;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return llama_init_from_model(m, cp);
}

static void decode_at(struct llama_context *ctx, const llama_token *toks,
                       int32_t n, int32_t pos0) {
    for (int32_t off = 0; off < n; off += 128) {
        llama_batch b = llama_batch_init(128, 0, 1);
        int32_t k = n - off > 128 ? 128 : n - off;
        for (int32_t i = 0; i < k; i++) {
            b.token[i] = toks[off+i]; b.pos[i] = pos0+off+i;
            b.n_seq_id[i] = 1; b.seq_id[i][0] = 0; b.logits[i] = 1;
        }
        b.n_tokens = k;
        if (llama_decode(ctx, b)) { fprintf(stderr, "decode fail\n"); exit(1); }
        llama_batch_free(b);
    }
}

static int32_t tokenize(const struct llama_vocab *vocab, const char *txt,
                        int add_bos, llama_token *out, int32_t cap) {
    int32_t n = llama_tokenize(vocab, txt, (int32_t)strlen(txt), out, cap, add_bos, true);
    if (n < 0) { fprintf(stderr, "tokenize fail\n"); exit(1); }
    return n;
}

static uint8_t *get_state(struct llama_context *ctx, size_t *sz_out) {
    size_t sz = llama_state_seq_get_size(ctx, 0);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf || llama_state_seq_get_data(ctx, buf, sz, 0) != sz) {
        fprintf(stderr, "state get fail\n"); exit(1);
    }
    *sz_out = sz;
    return buf;
}

static llama_token argmax_last(struct llama_context *ctx, int32_t n_vocab) {
    const float *lg = llama_get_logits(ctx);
    if (!lg) { fprintf(stderr, "logits fail\n"); exit(1); }
    int bi = 0;
    for (int32_t i = 1; i < n_vocab; i++) if (lg[i] > lg[bi]) bi = i;
    return (llama_token)bi;
}

#define ARENA_BYTES (2u * 1024u * 1024u)
static uint8_t g_arena[ARENA_BYTES];
static uint8_t g_loaded[ARENA_BYTES];

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 2 ? argv[2] : "build\\kvslots-sid";
    const char *backend    = argc > 3 ? argv[3] : "I:/llama/llama.cpp/build_zc2/bin/Release";
    ggml_backend_load_all_from_path(backend);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);

    llama_token toks[4096];
    int32_t n0 = tokenize(vocab, T_BASE, 1, toks, 4096);
    int32_t n1 = tokenize(vocab, T_S1, 0, toks + n0, 4096 - n0);
    printf("tokens: base=%d d1=%d total=%d\n", n0, n1, n0 + n1);

    /* ── live path (never touches SID): decode base, HOLD, decode suffix ── */
    struct llama_context *live = mk_ctx(model);
    if (!live) { fprintf(stderr, "FAIL: live ctx\n"); return 1; }
    decode_at(live, toks, n0, 0);
    KVColdBase base; kvcb_init(&base);
    if (kvcb_hold(live, 0, &base, n0) != 0) { fprintf(stderr, "FAIL: hold\n"); return 1; }
    decode_at(live, toks + n0, n1, n0);
    llama_token live_next = argmax_last(live, n_vocab);
    size_t live_sz; uint8_t *L = get_state(live, &live_sz);

    /* ── SID carries the chain: base blob record + token-suffix record ── */
    TensorMemStore store;
    ZoneCard card;
    memset(&card, 0, sizeof(card));
    card.entropy = 12;
    tmem_init(&store, g_arena, sizeof(g_arena), 0);
    AdaptiveRouteSIDEvent rb = { 2, 15, 1, 1728, 37, 1, 0 };
    AdaptiveRouteSIDEvent rd = { 2, 21, 1, 1729, 37, 1, 1 };
    int put_ok = (adaptive_route_sid_append(&store, &rb, &card, "kv.base",
                                            base.blob, base.size, 0, 0, 0, 0) == 0 &&
                  adaptive_route_sid_append(&store, &rd, &card, "kv.delta",
                                            (const uint8_t *)(toks + n0),
                                            (size_t)n1 * sizeof(llama_token), 0, 0, 1, 0) == 0);
    if (!put_ok) { fprintf(stderr, "FAIL: sid append\n"); return 1; }
    char tmem_path[512];
    snprintf(tmem_path, sizeof(tmem_path), "%s\\sid_kv_chain.tmem", outdir);
    if (tmem_save(&store, tmem_path) != 0) { fprintf(stderr, "FAIL: sid save\n"); return 1; }

    /* ── consumer reads the chain back out of SID (fresh arena, from file) ── */
    TensorMemStore loaded;
    if (tmem_load(&loaded, g_loaded, sizeof(g_loaded), tmem_path) != 0) {
        fprintf(stderr, "FAIL: sid load\n"); return 1;
    }
    uint8_t *sid_base = (uint8_t *)malloc(base.size);
    uint8_t *sid_dtoks = (uint8_t *)malloc((size_t)n1 * sizeof(llama_token));
    if (!sid_base || !sid_dtoks) { fprintf(stderr, "FAIL: alloc\n"); return 1; }
    int get_ok = (tmem_read_record(&loaded, 0, sid_base, base.size, NULL, NULL) == (int)base.size &&
                  tmem_read_record(&loaded, 1, sid_dtoks,
                                   (size_t)n1 * sizeof(llama_token), NULL, NULL) ==
                      (int)((size_t)n1 * sizeof(llama_token)));
    int roundtrip = get_ok && memcmp(sid_base, base.blob, base.size) == 0 &&
                    memcmp(sid_dtoks, toks + n0, (size_t)n1 * sizeof(llama_token)) == 0;
    printf("SID_ROUNDTRIP: %s (base %llu B + delta %llu B via .tmem save/load)\n",
           roundtrip ? "PASS" : "FAIL",
           (unsigned long long)base.size,
           (unsigned long long)((size_t)n1 * sizeof(llama_token)));
    if (!roundtrip) { fprintf(stderr, "FAIL: sid bytes differ\n"); return 1; }

    /* ── resume a FRESH ctx purely from SID bytes, teacher-decode suffix ── */
    struct llama_context *rc = mk_ctx(model);
    if (!rc) { fprintf(stderr, "FAIL: rc ctx\n"); return 1; }
    size_t applied = llama_state_seq_set_data(rc, sid_base, base.size, 0);
    int resume_ok = (applied == base.size);
    if (resume_ok)
        decode_at(rc, (const llama_token *)sid_dtoks, n1, n0);
    llama_token rc_next = resume_ok ? argmax_last(rc, n_vocab) : -1;
    size_t c_sz = 0; uint8_t *C = NULL;
    int resume = 0;
    if (resume_ok) {
        C = get_state(rc, &c_sz);
        resume = (c_sz == live_sz && memcmp(C, L, live_sz) == 0);
    }
    printf("RESUME: %s (SID bytes -> llama resume + teacher-decode vs live, %llu B)\n",
           resume ? "PASS" : "FAIL", (unsigned long long)live_sz);
    printf("DIAG next-token: %s (live=%d sid-resumed=%d)\n",
           live_next == rc_next ? "MATCH" : "DIFF", (int)live_next, (int)rc_next);

    FILE *f = fopen(tmem_path, "rb"); fseek(f, 0, SEEK_END);
    long stored = ftell(f); fclose(f);
    printf("SIZES: sid-stored=%ld B vs live-full=%llu B (%.1fx)\n",
           stored, (unsigned long long)live_sz,
           (double)live_sz / (double)stored);

    int ok = roundtrip && resume;
    printf("%s: SID carries cold-KV into live inference %s\n",
           ok ? "PASS" : "FAIL", ok ? "bit-exact" : "MISMATCH — see above");
    free(sid_base); free(sid_dtoks); free(C); free(L);
    kvcb_clear(&base);
    llama_free(live); llama_free(rc);
    llama_model_free(model);
    return ok ? 0 : 1;
}
