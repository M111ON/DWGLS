/* kv_frame_seek_test.c — proves the timeline index can actually seek:
 *   1. run N turns through a headless loop (same engine as kv_cold_chat),
 *   2. commit each turn into kv_timeline.idx,
 *   3. for a target turn T, use ftl_seek() to obtain the resume plan,
 *      resume the base .kvcb, teacher-decode the .kvd token suffix,
 *   4. assert next-token argmax == the argmax recorded live at turn T.
 *
 * This is the end-to-end proof that "chat turn as a frame you can seek to"
 * reconstructs the SAME generation without keeping all turns resident.
 *
 * BUILD: same flags as kv-cold-chat.
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include "kv_frame_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_TURNS 3

static uint64_t prefix_hash(const llama_token *t, int32_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (int32_t i = 0; i < n; i++) { h ^= (uint64_t)(uint32_t)t[i]; h *= 1099511628211ULL; }
    return h;
}
static int32_t tokenize(const struct llama_vocab *v, const char *s, int add, llama_token *o, int cap) {
    return llama_tokenize(v, s, (int32_t)strlen(s), o, cap, add, false);
}
static struct llama_context *mk_ctx(struct llama_model *m) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096; cp.n_batch = 512; cp.no_perf = true;
    cp.type_k = GGML_TYPE_Q8_0; cp.type_v = GGML_TYPE_Q4_0;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return llama_init_from_model(m, cp);
}
static int decode_at_err(struct llama_context *ctx, const llama_token *t, int32_t n, int32_t pos0) {
    int32_t outs = 0;
    for (int32_t off = 0; off < n; off += 512) {
        llama_batch b = llama_batch_init(512, 0, 1);
        int32_t k = n - off > 512 ? 512 : n - off;
        for (int32_t i = 0; i < k; i++) {
            b.token[i] = t[off+i]; b.pos[i] = pos0+off+i; b.n_seq_id[i] = 1; b.seq_id[i][0] = 0;
            b.logits[i] = 1;
        }
        b.n_tokens = k;
        int rc = llama_decode(ctx, b);
        if (rc) { fprintf(stderr, "[decode rc=%d off=%d k=%d pos0=%d]\n", rc, off, k, pos0); llama_batch_free(b); return -1; }
        outs = k;
        llama_batch_free(b);
    }
    return outs;
}
static void decode_at(struct llama_context *ctx, const llama_token *t, int32_t n, int32_t pos0) {
    decode_at_err(ctx, t, n, pos0);
}
static int g_nvocab = 151936;
static int argmax_last(struct llama_context *ctx, int32_t out_idx) {
    const float *lg = llama_get_logits_ith(ctx, out_idx);
    if (!lg) return -1;
    int bi = 0;
    for (int i = 1; i < g_nvocab; i++) if (lg[i] > lg[bi]) bi = i;
    return bi;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir = argc > 2 ? argv[2] : ".";
    const char *backend = argc > 3 ? argv[3] : "build\\cpuonly";
    const char *turns_in[N_TURNS] = {"สวัสดีครับ", "เล่าเรื่องสั้นๆ", "ขอบคุณ"};

    ggml_backend_load_all_from_path(backend);
    struct llama_model_params mp = llama_model_default_params(); mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL model\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);
    g_nvocab = n_vocab;
    fprintf(stderr, "[vocab] n_vocab=%d\n", n_vocab);

    struct llama_context *ctx = mk_ctx(model);
    static llama_token hist[4096]; int32_t n_hist = 0;
    int live_argmax[N_TURNS + 1]; memset(live_argmax, 0, sizeof(live_argmax));

    FTLIndex tl;
    if (ftl_open(&tl, outdir, 4096, model_path) != 0) { fprintf(stderr, "FAIL ftl\n"); return 1; }

    /* ── pass 1: run the turns, record live argmax, commit frames ── */
    for (int t = 1; t <= N_TURNS; t++) {
        char wrapped[512];
        snprintf(wrapped, sizeof(wrapped), "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", turns_in[t-1]);
        int32_t prev = n_hist;
        char whole[640];
        snprintf(whole, sizeof(whole), "%s<|im_end|>\n", wrapped);
        n_hist += tokenize(vocab, whole, n_hist == 0, hist + n_hist, 4096 - n_hist);
        int32_t nu = n_hist - prev;
        /* decode the ENTIRE turn in one contiguous run — no overlap, no gap */
        if (decode_at_err(ctx, hist + prev, nu, prev) < 0) {
            fprintf(stderr, "[live t%d] turn decode failed (nu=%d prev=%d)\n", t, nu, prev); return 1;
        }
        live_argmax[t] = argmax_last(ctx, nu - 1);   /* last output of this batch */
        printf("[live t%d] frame-end argmax=%d (n_hist=%d, nu=%d)\n", t, live_argmax[t], n_hist, nu);

        KVColdBase base; kvcb_init(&base);
        kvcb_hold(ctx, 0, &base, n_hist);
        char pfx2[512], pd[512];
        snprintf(pfx2, sizeof(pfx2), "%s\\chat_%016llx.kvcb", outdir, (unsigned long long)prefix_hash(hist, n_hist));
        snprintf(pd, sizeof(pd), "%s\\chat_t%d.kvd", outdir, t);
        kvcb_save(&base, pfx2);
        kvcb_spill(hist + prev, (uint32_t)(n_hist - prev), prev, pd);
        kvcb_clear(&base);

        FTLFrame fr; memset(&fr, 0, sizeof(fr));
        fr.turn = t; fr.base_n_tokens = n_hist;
        fr.n_delta = (uint32_t)(n_hist - prev);
        fr.prefix_hash = prefix_hash(hist, n_hist); fr.t_unix = (uint64_t)time(NULL);
        const char *bn = strrchr(pfx2,'\\'); bn = bn?bn+1:pfx2;
        const char *dn = strrchr(pd,'\\');  dn = dn?dn+1:pd;
        snprintf(fr.base_file, sizeof(fr.base_file), "%s", bn);
        snprintf(fr.delta_file, sizeof(fr.delta_file), "%s", dn);
        ftl_commit(&tl, &fr);
    }
    printf("--- pass 1 done: %d frames ---\n", tl.hdr.n_frames);

    /* ── pass 2: seek back to each turn in a FRESH ctx and compare ── */
    int ok = 0, fail = 0;
    for (int T = 1; T <= N_TURNS; T++) {
        struct llama_context *c2 = mk_ctx(model);
        FTLSeek sk;
        if (ftl_seek(&tl, T, &sk) != 0) { printf("[seek t%d] FAIL plan\n", T); fail++; llama_free(c2); continue; }
        if (sk.have_base) {
            char bp[512];
            snprintf(bp, sizeof(bp), "%s\\%s", outdir, tl.frames[T-2].base_file);
            KVColdBase b; kvcb_init(&b);
            if (kvcb_load(&b, bp) != 0) { printf("[seek t%d] FAIL load base\n", T); fail++; kvcb_clear(&b); llama_free(c2); continue; }
            kvcb_resume(c2, 0, &b); kvcb_clear(&b);
        }
        /* teacher-decode the delta suffix tokens for turn T */
        char dp[512];
        snprintf(dp, sizeof(dp), "%s\\%s", outdir, tl.frames[T-1].delta_file);
        int32_t *dt = NULL; uint32_t nd = 0; int32_t bn = 0;
        if (kvcb_unspill(&dt, &nd, &bn, dp) != 0) { printf("[seek t%d] FAIL delta\n", T); fail++; llama_free(c2); continue; }
        if (nd == 0) { printf("[seek t%d] FAIL empty delta\n", T); fail++; free(dt); llama_free(c2); continue; }
        int32_t outs = decode_at_err(c2, dt, (int32_t)nd, bn);
        if (outs < 0) { printf("[seek t%d] FAIL decode\n", T); fail++; free(dt); llama_free(c2); continue; }
        free(dt);
        /* decode_at sets logits=1 on every token, so the LAST output is nd-1 */
        int am = argmax_last(c2, (int32_t)nd - 1);
        int match = (am == live_argmax[T]);
        printf("[seek t%d] argmax=%d live=%d %s\n", T, am, live_argmax[T], match ? "MATCH ✅" : "DIFF ❌");
        if (match) ok++; else fail++;
        llama_free(c2);
    }
    printf("=== frame-seek: %d/%d MATCH ===\n", ok, ok + fail);

    ftl_close(&tl);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    return fail ? 1 : 0;
}
