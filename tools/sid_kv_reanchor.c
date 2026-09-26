/*
 * tools/sid_kv_reanchor.c — re-anchor THROUGH SID (cold-KV step 3 via SID).
 * ═══════════════════════════════════════════════════════════════════════
 * Chain: live free-run greedy 20 steps from a prompt → HOLD@10 becomes the
 * new base written INTO the SID store (old base stays as history) → suffix
 * tokens 11..20 spilled as a SID delta record → .tmem save/load → fresh ctx
 * resumes purely from SID's base.10 + delta.10 → bit-exact vs live final.
 * Config mirrors sid_kv_resume (F16, flash DISABLED, ngl=0); the only new
 * variable is "re-anchor point lives in SID".
 *
 * RUN: ./build/sid_kv_reanchor_zc2 <model.gguf> [outdir] [backend_dir]
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include "adaptive_route_sid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_GEN 20
#define RE_ANCHOR_AT 10

static const char *T_PROMPT =
    "<|im_start|>user\nExplain why the sky is blue in two sentences.<|im_end|>\n"
    "<|im_start|>assistant\n";

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

#define ARENA_BYTES (4u * 1024u * 1024u)
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

    llama_token prompt[1024];
    int32_t np = llama_tokenize(vocab, T_PROMPT, (int32_t)strlen(T_PROMPT),
                                prompt, 1024, true, true);
    if (np < 0) { fprintf(stderr, "tokenize fail\n"); return 1; }
    printf("prompt=%d + %d gen steps, re-anchor @%d\n", np, N_GEN, RE_ANCHOR_AT);

    TensorMemStore store;
    ZoneCard card;
    memset(&card, 0, sizeof(card));
    card.entropy = 12;
    tmem_init(&store, g_arena, sizeof(g_arena), 0);

    /* ── live free-run: prefill, HOLD base.0 into SID, greedy 20 steps ── */
    struct llama_context *live = mk_ctx(model);
    if (!live) { fprintf(stderr, "FAIL: live ctx\n"); return 1; }
    decode_at(live, prompt, np, 0);
    KVColdBase b0; kvcb_init(&b0);
    if (kvcb_hold(live, 0, &b0, np) != 0) { fprintf(stderr, "FAIL: hold base.0\n"); return 1; }
    AdaptiveRouteSIDEvent r0 = { 2, 15, 1, 1728, 37, 1, 0 };
    if (adaptive_route_sid_append(&store, &r0, &card, "kv.base.0",
                                  b0.blob, b0.size, 0, 0, 0, 0) != 0) {
        fprintf(stderr, "FAIL: sid append base.0\n"); return 1;
    }

    struct llama_sampler *sm = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sm, llama_sampler_init_greedy());
    llama_token gen[N_GEN];
    KVColdBase b10; kvcb_init(&b10);
    size_t b10_size = 0;
    for (int s = 0; s < N_GEN; s++) {
        llama_token nx = llama_sampler_sample(sm, live, -1);
        llama_sampler_accept(sm, nx);
        gen[s] = nx;
        llama_batch b = llama_batch_init(1, 0, 1);
        b.token[0] = nx; b.pos[0] = np + s;
        b.n_seq_id[0] = 1; b.seq_id[0][0] = 0; b.logits[0] = 1;
        b.n_tokens = 1;
        if (llama_decode(live, b)) { fprintf(stderr, "step decode fail\n"); return 1; }
        llama_batch_free(b);
        if (s + 1 == RE_ANCHOR_AT) {
            /* RE-ANCHOR: fresh HOLD becomes the new base, written into SID. */
            if (kvcb_hold(live, 0, &b10, np + s + 1) != 0) {
                fprintf(stderr, "FAIL: hold base.10\n"); return 1;
            }
            b10_size = b10.size;
            AdaptiveRouteSIDEvent r10 = { 2, 21, 1, 1738, 37, 1, 10 };
            if (adaptive_route_sid_append(&store, &r10, &card, "kv.base.10",
                                          b10.blob, b10.size, 0, 0, 10, 0) != 0) {
                fprintf(stderr, "FAIL: sid append base.10\n"); return 1;
            }
            printf("RE-ANCHOR @%d: %llu B -> %llu B (into SID)\n",
                   s + 1, (unsigned long long)b0.size, (unsigned long long)b10.size);
        }
    }
    llama_token live_next = argmax_last(live, n_vocab);
    size_t live_sz; uint8_t *L = get_state(live, &live_sz);

    /* suffix after the anchor point travels as a SID delta record */
    const int NSUF = N_GEN - RE_ANCHOR_AT;
    AdaptiveRouteSIDEvent rd = { 2, 27, 1, 1739, 37, 1, 11 };
    if (adaptive_route_sid_append(&store, &rd, &card, "kv.delta.10",
                                  (const uint8_t *)(gen + RE_ANCHOR_AT),
                                  (size_t)NSUF * sizeof(llama_token), 0, 0, 11, 0) != 0) {
        fprintf(stderr, "FAIL: sid append delta\n"); return 1;
    }
    char tmem_path[512];
    snprintf(tmem_path, sizeof(tmem_path), "%s\\sid_kv_reanchor.tmem", outdir);
    if (tmem_save(&store, tmem_path) != 0) { fprintf(stderr, "FAIL: sid save\n"); return 1; }
    printf("SID records: %u (base.0, base.10, delta.10)\n", store.n_records);

    /* ── consumer: load SID from file, resume from base.10 + delta.10 ── */
    TensorMemStore loaded;
    if (tmem_load(&loaded, g_loaded, sizeof(g_loaded), tmem_path) != 0) {
        fprintf(stderr, "FAIL: sid load\n"); return 1;
    }
    uint8_t *sid_b10 = (uint8_t *)malloc(b10_size);
    uint8_t *sid_suf = (uint8_t *)malloc((size_t)NSUF * sizeof(llama_token));
    uint8_t *sid_b0 = (uint8_t *)malloc(b0.size);
    if (!sid_b10 || !sid_suf || !sid_b0) { fprintf(stderr, "FAIL: alloc\n"); return 1; }
    int roundtrip = (tmem_read_record(&loaded, 0, sid_b0, b0.size, NULL, NULL) == (int)b0.size &&
                     memcmp(sid_b0, b0.blob, b0.size) == 0 &&
                     tmem_read_record(&loaded, 1, sid_b10, b10_size, NULL, NULL) == (int)b10_size &&
                     memcmp(sid_b10, b10.blob, b10_size) == 0 &&
                     tmem_read_record(&loaded, 2, sid_suf,
                                      (size_t)NSUF * sizeof(llama_token), NULL, NULL) ==
                         (int)((size_t)NSUF * sizeof(llama_token)) &&
                     memcmp(sid_suf, gen + RE_ANCHOR_AT,
                            (size_t)NSUF * sizeof(llama_token)) == 0);
    printf("SID_ROUNDTRIP: %s (3 records exact via .tmem save/load)\n",
           roundtrip ? "PASS" : "FAIL");
    if (!roundtrip) { fprintf(stderr, "FAIL: sid bytes differ\n"); return 1; }

    struct llama_context *rc = mk_ctx(model);
    if (!rc) { fprintf(stderr, "FAIL: rc ctx\n"); return 1; }
    size_t applied = llama_state_seq_set_data(rc, sid_b10, b10_size, 0);
    int resume_ok = (applied == b10_size);
    if (resume_ok) {
        /* single-step batches mirroring the live greedy loop: same splits →
           same numerics, so continuation (next token) matches too. */
        const llama_token *suf = (const llama_token *)sid_suf;
        for (int i = 0; i < NSUF; i++) {
            llama_batch b = llama_batch_init(1, 0, 1);
            b.token[0] = suf[i]; b.pos[0] = np + RE_ANCHOR_AT + i;
            b.n_seq_id[0] = 1; b.seq_id[0][0] = 0; b.logits[0] = 1;
            b.n_tokens = 1;
            if (llama_decode(rc, b)) { fprintf(stderr, "resume step decode fail\n"); exit(1); }
            llama_batch_free(b);
        }
    }
    llama_token rc_next = resume_ok ? argmax_last(rc, n_vocab) : -1;
    size_t c_sz = 0; uint8_t *C = NULL;
    int resume = 0;
    if (resume_ok) {
        C = get_state(rc, &c_sz);
        resume = (c_sz == live_sz && memcmp(C, L, live_sz) == 0);
    }
    printf("REANCHOR_RESUME: %s (SID base.10 + 10 suffix vs live final, %llu B)\n",
           resume ? "PASS" : "FAIL", (unsigned long long)live_sz);
    printf("DIAG next-token: %s (live=%d sid-resumed=%d)\n",
           live_next == rc_next ? "MATCH" : "DIFF", (int)live_next, (int)rc_next);

    FILE *f = fopen(tmem_path, "rb"); fseek(f, 0, SEEK_END);
    long stored = ftell(f); fclose(f);
    printf("SIZES: sid-stored=%ld B vs live-full=%llu B (%.1fx)\n",
           stored, (unsigned long long)live_sz,
           (double)live_sz / (double)stored);

    int ok = roundtrip && resume;
    printf("%s: re-anchor through SID %s\n",
           ok ? "PASS" : "FAIL", ok ? "bit-exact" : "MISMATCH — see above");
    free(sid_b0); free(sid_b10); free(sid_suf); free(C); free(L);
    kvcb_clear(&b0); kvcb_clear(&b10);
    llama_sampler_free(sm);
    llama_free(live); llama_free(rc);
    llama_model_free(model);
    return ok ? 0 : 1;
}
