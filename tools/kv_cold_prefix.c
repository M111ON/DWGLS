/*
 * tools/kv_cold_prefix.c — COLD-KV step 4 proof: prefix-shared base cache.
 * ═══════════════════════════════════════════════════════════════════════
 * Arch: docs/COLD-KV-ARCH-2026-09-23.md (build order step 4, LAST).
 * Claim: "the real 10x" — sessions sharing a token prefix share ONE base
 * file keyed by FNV-1a hash of the prefix token ids. A new session with a
 * known prefix skips prefill decode entirely: load base + resume + decode
 * only its own suffix.
 * Proof (cross-PROCESS: separate invocations share only the .kvcb file):
 *   save: decode PREFIX → HOLD → save pfx_<hash>.kvcb
 *   load <A|B>: load base by hash → RESUME into fresh ctx → decode only
 *     the session suffix → memcmp vs fresh full-decode reference.
 *   Both sessions A and B must PASS → one base serves N sessions.
 * F16 + DISABLED + cpuonly (precision-orthogonal; tests cross-process
 * decode determinism — PASS requires bit-identical prefill across runs).
 *
 * RUN: ./build/kv_cold_prefix save|loadA|loadB [model.gguf] [outdir] [backend_dir]
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *T_PREFIX =
    "<|im_start|>user\nWhat is the capital of France? Answer in one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe capital of France is Paris.<|im_end|>\n";
static const char *T_SUF_A =
    "<|im_start|>user\nNow tell me one famous landmark there, one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe Eiffel Tower is Paris's most famous landmark.<|im_end|>\n";
static const char *T_SUF_B =
    "<|im_start|>user\nWhat language do they speak there?<|im_end|>\n"
    "<|im_start|>assistant\nThey speak French in Paris.<|im_end|>\n";

/* FNV-1a 64 over token ids → prefix key. */
static uint64_t prefix_hash(const llama_token *toks, int32_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (int32_t i = 0; i < n; i++) {
        h ^= (uint64_t)(uint32_t)toks[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static struct llama_context *mk_ctx(struct llama_model *m) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096; cp.n_batch = 512; cp.no_perf = true;
    cp.type_k = GGML_TYPE_F16; cp.type_v = GGML_TYPE_F16;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return llama_init_from_model(m, cp);
}

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
    const char *mode       = argc > 1 ? argv[1] : "save";
    const char *model_path = argc > 2 ? argv[2] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 3 ? argv[3] : "build\\kvslots";
    const char *backend    = argc > 4 ? argv[4] : "build\\cpuonly";
    int is_save = !strcmp(mode, "save");
    int is_b = !strcmp(mode, "loadB");
    if (!is_save && strcmp(mode, "loadA") && !is_b) { fprintf(stderr, "mode: save|loadA|loadB\n"); return 1; }
    ggml_backend_load_all_from_path(backend);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    llama_token toks[4096];
    const char *suf = is_b ? T_SUF_B : T_SUF_A;
    int32_t np = tokenize(vocab, T_PREFIX, 1, toks, 4096);
    int32_t ns = tokenize(vocab, suf, 0, toks + np, 4096 - np);
    char path[512];
    snprintf(path, sizeof(path), "%s\\pfx_%016llx.kvcb", outdir,
             (unsigned long long)prefix_hash(toks, np));
    printf("prefix=%d tok hash=%016llx session-suffix=%d tok\n",
           np, (unsigned long long)prefix_hash(toks, np), ns);

    if (is_save) {
        struct llama_context *ctx = mk_ctx(model);
        if (!ctx) { fprintf(stderr, "FAIL: ctx\n"); return 1; }
        decode_at(ctx, toks, np, 0);
        KVColdBase base; kvcb_init(&base);
        if (kvcb_hold(ctx, 0, &base, np) != 0) { fprintf(stderr, "FAIL: hold\n"); return 1; }
        if (kvcb_save(&base, path) != 0) { fprintf(stderr, "FAIL: save\n"); return 1; }
        printf("SAVE: %d tok %llu B -> %s\n", np, (unsigned long long)base.size, path);
        kvcb_clear(&base); llama_free(ctx);
        printf("PASS: prefix base cached\n");
        llama_model_free(model);
        return 0;
    }

    /* load path: fresh ctx, resume shared base, decode ONLY the suffix */
    struct llama_context *ctx = mk_ctx(model);
    if (!ctx) { fprintf(stderr, "FAIL: ctx\n"); return 1; }
    KVColdBase base; kvcb_init(&base);
    if (kvcb_load(&base, path) != 0) { fprintf(stderr, "FAIL: load %s (run save first)\n", path); return 1; }
    if (base.n_tokens != np) { fprintf(stderr, "FAIL: base covers %d tok, prefix is %d\n", base.n_tokens, np); return 1; }
    if (kvcb_resume(ctx, 0, &base) != base.size) { fprintf(stderr, "FAIL: resume\n"); return 1; }
    decode_at(ctx, toks + np, ns, np);
    size_t lsz; uint8_t *L = get_state(ctx, &lsz);

    /* reference: separate fresh ctx, full prefill decode (the skipped work) */
    struct llama_context *ref = mk_ctx(model);
    if (!ref) { fprintf(stderr, "FAIL: ref ctx\n"); return 1; }
    decode_at(ref, toks, np + ns, 0);
    size_t rsz; uint8_t *R = get_state(ref, &rsz);
    int ok = (lsz == rsz && memcmp(L, R, lsz) == 0);
    printf("LOAD-%s: %s (suffix-only %d tok vs full %d tok; prefill skipped %d tok, %llu B)\n",
           is_b ? "B" : "A", ok ? "PASS" : "FAIL", ns, np + ns, np, (unsigned long long)lsz);
    free(L); free(R); kvcb_clear(&base);
    llama_free(ctx); llama_free(ref); llama_model_free(model);
    return ok ? 0 : 1;
}
