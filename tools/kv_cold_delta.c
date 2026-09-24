/*
 * tools/kv_cold_delta.c — COLD-KV step 2 proof: token-suffix spill → reconstruct.
 * ═══════════════════════════════════════════════════════════════════════
 * Arch: docs/COLD-KV-ARCH-2026-09-23.md (build order step 2).
 * Design measured 2026-09-23: opaque state blobs churn 98.6%/token
 * (build/kvslots/deltaA vs deltaB), so the spill unit is a TOKEN suffix:
 *   live(t_n) = resume(base@t0) + teacher-decode(tokens[t0..t_n]).
 * Proof: reconstruct-from-spill vs direct full decode, memcmp both links
 * of a 2-delta chain. Precision-orthogonal (deltas are token ids).
 * Kernel: F16 + flash DISABLED on all ctx (threshold fair-kernel rule).
 *
 * BUILD: gcc -O2 -I I:/llama/include -I core -o build/kv_cold_delta
 *   tools/kv_cold_delta.c I:/llama/llama-v040-bin-win-vulkan-x64/llama.dll
 *   .../ggml.dll .../ggml-base.dll .../ggml-cpu-x64.dll -lm
 * RUN: PATH="...;$PATH" ./build/kv_cold_delta [model.gguf] [outdir] [backend_dir]
 *   default backend_dir = build/cpuonly (CPU-only, matches threshold verdicts)
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same turn texts as tools/kv_dump_turns.c (comparable with card #7). */
static const char *T_BASE =
    "<|im_start|>user\nWhat is the capital of France? Answer in one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe capital of France is Paris.<|im_end|>\n";
static const char *T_S1 =
    "<|im_start|>user\nNow tell me one famous landmark there, one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe Eiffel Tower is Paris's most famous landmark.<|im_end|>\n";
static const char *T_S2 =
    "<|im_start|>user\nWhat year was it built? One sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe Eiffel Tower was completed in 1889.<|im_end|>\n";

static struct llama_context *mk_ctx(struct llama_model *m) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096; cp.n_batch = 512; cp.no_perf = true;
    cp.type_k = GGML_TYPE_F16; cp.type_v = GGML_TYPE_F16;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return llama_init_from_model(m, cp);
}

/* Teacher-decode toks[0..n) at positions pos0+i. */
static void decode_at(struct llama_context *ctx, const llama_token *toks,
                      int32_t n, int32_t pos0) {
    for (int32_t off = 0; off < n; off += 512) {
        llama_batch b = llama_batch_init(512, 0, 1);
        int32_t k = n - off > 512 ? 512 : n - off;
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

/* Snapshot current seq-0 state into malloc'd buf (caller frees). */
static uint8_t *get_state(struct llama_context *ctx, size_t *sz_out) {
    size_t sz = llama_state_seq_get_size(ctx, 0);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf || llama_state_seq_get_data(ctx, buf, sz, 0) != sz) {
        fprintf(stderr, "state get fail\n"); exit(1);
    }
    *sz_out = sz;
    return buf;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 2 ? argv[2] : "build\\kvslots";
    const char *backend    = argc > 3 ? argv[3] : "build\\cpuonly";
    ggml_backend_load_all_from_path(backend);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    char p_base[512], p_d1[512], p_d2[512];
    snprintf(p_base, sizeof(p_base), "%s\\cold_base.kvcb", outdir);
    snprintf(p_d1, sizeof(p_d1), "%s\\cold_d1.kvd", outdir);
    snprintf(p_d2, sizeof(p_d2), "%s\\cold_d2.kvd", outdir);

    llama_token toks[4096];
    int32_t n0 = tokenize(vocab, T_BASE, 1, toks, 4096);
    int32_t n1 = tokenize(vocab, T_S1, 0, toks + n0, 4096 - n0);
    int32_t n2 = tokenize(vocab, T_S2, 0, toks + n0 + n1, 4096 - n0 - n1);
    printf("tokens: base=%d d1=%d d2=%d total=%d\n", n0, n1, n2, n0+n1+n2);

    /* ── live path: base hold + spill + advance ── */
    struct llama_context *live = mk_ctx(model);
    if (!live) { fprintf(stderr, "FAIL: ctx\n"); return 1; }
    decode_at(live, toks, n0, 0);
    KVColdBase base; kvcb_init(&base);
    if (kvcb_hold(live, 0, &base, n0) != 0) { fprintf(stderr, "FAIL: hold\n"); return 1; }
    if (kvcb_save(&base, p_base) != 0) { fprintf(stderr, "FAIL: save base\n"); return 1; }
    printf("HOLD: base %d tok %llu B -> %s\n", n0, (unsigned long long)base.size, p_base);

    if (kvcb_spill(toks + n0, (uint32_t)n1, n0, p_d1) != 0) { fprintf(stderr, "FAIL: spill d1\n"); return 1; }
    decode_at(live, toks + n0, n1, n0);
    size_t l1_sz; uint8_t *L1 = get_state(live, &l1_sz);

    /* ── ref1: fresh ctx, direct full decode ── */
    struct llama_context *ref = mk_ctx(model);
    if (!ref) { fprintf(stderr, "FAIL: ref ctx\n"); return 1; }
    decode_at(ref, toks, n0 + n1, 0);
    size_t r1_sz; uint8_t *R1 = get_state(ref, &r1_sz);
    int chain1 = (l1_sz == r1_sz && memcmp(L1, R1, l1_sz) == 0);
    printf("CHAIN1: %s (reconstruct base+d1 vs direct, %llu B)\n",
           chain1 ? "PASS" : "FAIL", (unsigned long long)l1_sz);
    free(L1); free(R1); llama_free(ref);

    /* ── link 2: spill d2, advance, compare ── */
    if (kvcb_spill(toks + n0 + n1, (uint32_t)n2, n0 + n1, p_d2) != 0) { fprintf(stderr, "FAIL: spill d2\n"); return 1; }
    decode_at(live, toks + n0 + n1, n2, n0 + n1);
    size_t l2_sz; uint8_t *L2 = get_state(live, &l2_sz);

    /* reconstruct-from-files path: load base + unspill d1/d2, resume + decode */
    struct llama_context *rc = mk_ctx(model);
    if (!rc) { fprintf(stderr, "FAIL: rc ctx\n"); return 1; }
    KVColdBase rb; kvcb_init(&rb);
    int32_t *u1 = NULL, *u2 = NULL; uint32_t un1 = 0, un2 = 0; int32_t sb1 = -1, sb2 = -1;
    int rc_ok = (kvcb_load(&rb, p_base) == 0 &&
                 kvcb_unspill(&u1, &un1, &sb1, p_d1) == 0 &&
                 kvcb_unspill(&u2, &un2, &sb2, p_d2) == 0 &&
                 sb1 == n0 && sb2 == n0 + n1 &&
                 kvcb_resume(rc, 0, &rb) == rb.size);
    if (rc_ok) {
        decode_at(rc, u1, (int32_t)un1, sb1);
        decode_at(rc, u2, (int32_t)un2, sb2);
    }
    size_t c2_sz = 0; uint8_t *C2 = NULL;
    int chain2 = 0;
    if (rc_ok) {
        C2 = get_state(rc, &c2_sz);
        chain2 = (c2_sz == l2_sz && memcmp(C2, L2, l2_sz) == 0);
    }
    printf("CHAIN2: %s (file path base+d1+d2 vs live, %llu B)\n",
           chain2 ? "PASS" : "FAIL", (unsigned long long)l2_sz);
    free(u1); free(u2); free(C2); free(L2);
    kvcb_clear(&base); kvcb_clear(&rb);
    llama_free(live); llama_free(rc);

    /* ── receipt: the actual ratio ── */
    FILE *f = fopen(p_base, "rb"); fseek(f, 0, SEEK_END);
    long sb = ftell(f); fclose(f);
    f = fopen(p_d1, "rb"); fseek(f, 0, SEEK_END);
    long s1 = ftell(f); fclose(f);
    f = fopen(p_d2, "rb"); fseek(f, 0, SEEK_END);
    long s2 = ftell(f); fclose(f);
    printf("SIZES: base=%ld B d1=%ld B d2=%ld B | cold-stored=%ld B vs live-full=%llu B (%.1fx)\n",
           sb, s1, s2, sb + s1 + s2, (unsigned long long)l2_sz,
           (double)l2_sz / (double)(sb + s1 + s2));

    int ok = chain1 && chain2;
    printf("%s: spill→reconstruct %s\n", ok ? "PASS" : "FAIL",
           ok ? "bit-exact, chained" : "MISMATCH — see above");
    llama_model_free(model);
    return ok ? 0 : 1;
}
