/*
 * tools/sid_kv_compact.c — eviction policy for SID cold-KV lineage.
 * ═══════════════════════════════════════════════════════════════════════
 * Policy: once base.N is anchored, every base older than N is superseded
 * and evictable; the delta that bridged them is consumed. The live set is
 * {latest base + suffix delta after it}. Compaction copies exactly the
 * live set (identity + bytes preserved) into a fresh store and proves a
 * fresh ctx resumes bit-exact from the COMPACTED file.
 * Store has no delete API by design (append-only arena), so eviction =
 * copy-live-set-forward. Run after sid_kv_reanchor (reads its .tmem).
 *
 * RUN: ./build/sid_kv_compact_zc2 <model.gguf> [outdir] [backend_dir]
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
static uint8_t g_src[ARENA_BYTES];
static uint8_t g_dst[ARENA_BYTES];
static uint8_t g_back[ARENA_BYTES];

/* Copy record idx from src store into dst store, identity + bytes intact. */
static int carry(const TensorMemStore *src, TensorMemStore *dst, uint32_t idx) {
    int sz = tmem_read_record(src, idx, NULL, 0, NULL, NULL);
    if (sz <= 0) return -1;
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    const ZoneCardSID *z = tmem_record_zcsid(src, idx);
    const char *nm = tmem_record_name(src, idx);
    char name[140];
    if (!buf || !z) { free(buf); return -1; }
    snprintf(name, sizeof(name), "%s", nm ? nm : "kv.unnamed");
    if (tmem_read_record(src, idx, buf, (size_t)sz, NULL, NULL) != sz) {
        free(buf); return -1;
    }
    int rc = tmem_append_raw(dst, z, name, buf, (size_t)sz);
    free(buf);
    return rc;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 2 ? argv[2] : "build\\kvslots-sid";
    const char *backend    = argc > 3 ? argv[3] : "I:/llama/llama.cpp/build_zc2/bin/Release";
    ggml_backend_load_all_from_path(backend);

    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s\\sid_kv_reanchor.tmem", outdir);
    snprintf(dst_path, sizeof(dst_path), "%s\\sid_kv_compact.tmem", outdir);

    TensorMemStore src;
    if (tmem_load(&src, g_src, sizeof(g_src), src_path) != 0) {
        fprintf(stderr, "FAIL: load source .tmem (run sid-kv-reanchor-zc2 first)\n");
        return 1;
    }
    printf("source records: %u (expect 3: base.0, base.10, delta.10)\n", src.n_records);
    if (src.n_records != 3) { fprintf(stderr, "FAIL: unexpected lineage shape\n"); return 1; }

    /* ── evict: carry only the live set (latest base + its suffix) ── */
    TensorMemStore dst;
    tmem_init(&dst, g_dst, sizeof(g_dst), 0);
    if (carry(&src, &dst, 1) != 0 || carry(&src, &dst, 2) != 0) {
        fprintf(stderr, "FAIL: carry live set\n"); return 1;
    }
    if (tmem_save(&dst, dst_path) != 0) { fprintf(stderr, "FAIL: save compacted\n"); return 1; }

    FILE *f = fopen(src_path, "rb"); fseek(f, 0, SEEK_END);
    long before = ftell(f); fclose(f);
    f = fopen(dst_path, "rb"); fseek(f, 0, SEEK_END);
    long after = ftell(f); fclose(f);
    printf("COMPACT: %ld B -> %ld B (evicted superseded base.0)\n", before, after);

    /* ── prove the compacted file still resumes: rebuild live reference ── */
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

    struct llama_context *live = mk_ctx(model);
    if (!live) { fprintf(stderr, "FAIL: live ctx\n"); return 1; }
    { /* prefill in 128-chunks, logits on last token only */
        for (int32_t off = 0; off < np; off += 128) {
            llama_batch b = llama_batch_init(128, 0, 1);
            int32_t k = np - off > 128 ? 128 : np - off;
            for (int32_t i = 0; i < k; i++) {
                b.token[i] = prompt[off+i]; b.pos[i] = off+i;
                b.n_seq_id[i] = 1; b.seq_id[i][0] = 0;
                b.logits[i] = (off + i == np - 1) ? 1 : 0;
            }
            b.n_tokens = k;
            if (llama_decode(live, b)) { fprintf(stderr, "prefill fail\n"); return 1; }
            llama_batch_free(b);
        }
    }
    struct llama_sampler *sm = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sm, llama_sampler_init_greedy());
    llama_token gen[N_GEN];
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
    }
    llama_token live_next = argmax_last(live, n_vocab);
    size_t live_sz; uint8_t *L = get_state(live, &live_sz);

    /* resume from the COMPACTED file (record 0 = base.10, record 1 = delta) */
    TensorMemStore back;
    if (tmem_load(&back, g_back, sizeof(g_back), dst_path) != 0) {
        fprintf(stderr, "FAIL: reload compacted\n"); return 1;
    }
    int bsz = tmem_read_record(&back, 0, NULL, 0, NULL, NULL);
    int dsz = tmem_read_record(&back, 1, NULL, 0, NULL, NULL);
    uint8_t *sb = (uint8_t *)malloc(bsz > 0 ? (size_t)bsz : 1);
    uint8_t *sd = (uint8_t *)malloc(dsz > 0 ? (size_t)dsz : 1);
    int resume_ok = (bsz > 0 && dsz == (N_GEN - RE_ANCHOR_AT) * (int)sizeof(llama_token) &&
        tmem_read_record(&back, 0, sb, (size_t)bsz, NULL, NULL) == bsz &&
        tmem_read_record(&back, 1, sd, (size_t)dsz, NULL, NULL) == dsz);
    struct llama_context *rc = mk_ctx(model);
    if (!rc) { fprintf(stderr, "FAIL: rc ctx\n"); return 1; }
    if (resume_ok)
        resume_ok = (llama_state_seq_set_data(rc, sb, (size_t)bsz, 0) == (size_t)bsz);
    const int NSUF = N_GEN - RE_ANCHOR_AT;
    if (resume_ok) {
        const llama_token *suf = (const llama_token *)sd;
        for (int i = 0; i < NSUF; i++) {
            llama_batch b = llama_batch_init(1, 0, 1);
            b.token[0] = suf[i]; b.pos[0] = np + RE_ANCHOR_AT + i;
            b.n_seq_id[0] = 1; b.seq_id[0][0] = 0; b.logits[0] = 1;
            b.n_tokens = 1;
            if (llama_decode(rc, b)) { fprintf(stderr, "resume step fail\n"); return 1; }
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
    printf("COMPACT_RESUME: %s (compacted base.10 + suffix vs live final, %llu B)\n",
           resume ? "PASS" : "FAIL", (unsigned long long)live_sz);
    printf("DIAG next-token: %s (live=%d compact-resumed=%d)\n",
           live_next == rc_next ? "MATCH" : "DIFF", (int)live_next, (int)rc_next);
    printf("SIZES: compacted=%ld B vs live-full=%llu B (%.1fx)\n",
           after, (unsigned long long)live_sz, (double)live_sz / (double)after);

    int ok = resume && live_next == rc_next;
    printf("%s: eviction keeps resume bit-exact %s\n",
           ok ? "PASS" : "FAIL", ok ? "+ continuation identical" : "MISMATCH — see above");
    free(sb); free(sd); free(C); free(L);
    llama_sampler_free(sm);
    llama_free(live); llama_free(rc);
    llama_model_free(model);
    return ok ? 0 : 1;
}
