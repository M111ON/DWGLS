/*
 * tools/kv_cold_base.c — COLD-KV step 1 proof: base HOLD → continue → RESUME.
 * ═══════════════════════════════════════════════════════════════════════
 * Arch: docs/COLD-KV-ARCH-2026-09-23.md (build order step 1).
 * Shape ported from FGLS_new runner/kv_sid_evict.h kv_snapshot_layer/
 * kv_restore_layer/kv_sid_free_snapshots, via public llama_state_seq_* API.
 *
 * Proof sequence (Qwen2.5-0.5B, CPU):
 *   1. decode prompt P (N tokens) → HOLD base (clipboard, F16 full)
 *   2. save base to file, load into second clipboard → memcmp (file RT)
 *   3. decode K more tokens (live state advances past base)
 *   4. RESUME base into seq → get state → memcmp vs held blob: must be
 *      byte-equal (PASS) — resume is bit-exact by construction.
 *   5. receipt: sizes, token counts, growth B/token (expect ~12300, card #7).
 *
 * BUILD: gcc -O2 -I I:/llama/include -I core -o build/kv_cold_base
 *   tools/kv_cold_base.c I:/llama/llama-v040-bin-win-vulkan-x64/llama.dll
 *   I:/llama/llama-v040-bin-win-vulkan-x64/ggml.dll
 *   I:/llama/llama-v040-bin-win-vulkan-x64/ggml-base.dll
 *   I:/llama/llama-v040-bin-win-vulkan-x64/ggml-cpu-x64.dll -lm
 * RUN: PATH="I:/llama/llama-v040-bin-win-vulkan-x64:$PATH"
 *   ./build/kv_cold_base [model.gguf] [out.kvcb]
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include <stdio.h>
#include <string.h>

static const char *P_BASE =
    "<|im_start|>user\nWhat is the capital of France? Answer in one sentence.<|im_end|>\n"
    "<|im_start|>assistant\nThe capital of France is Paris.<|im_end|>\n";
static const char *P_EXTRA =
    "<|im_start|>user\nNow tell me one famous landmark there, one sentence.<|im_end|>\n";

static int32_t decode_text(const struct llama_vocab *vocab, struct llama_context *ctx,
                           const char *txt, int add_bos, llama_token *toks, int32_t off) {
    int32_t n = llama_tokenize(vocab, txt, (int32_t)strlen(txt), toks + off, 4096 - off, add_bos, true);
    if (n < 0) return -1;
    int32_t done = 0;
    while (done < n) {
        int32_t chunk = n - done > 512 ? 512 : n - done;
        if (llama_decode(ctx, llama_batch_get_one(toks + off + done, chunk))) return -1;
        done += chunk;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *out_path   = argc > 2 ? argv[2] : "build\\kvslots\\cold_base.kvcb";
    ggml_backend_load_all();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096; cp.n_batch = 512; cp.no_perf = true;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "FAIL: ctx\n"); return 1; }

    llama_token toks[4096];
    int32_t n1 = decode_text(vocab, ctx, P_BASE, 1, toks, 0);
    if (n1 < 0) { fprintf(stderr, "FAIL: decode base\n"); return 1; }

    /* 1. HOLD */
    KVColdBase base; kvcb_init(&base);
    if (kvcb_hold(ctx, 0, &base, n1) != 0) { fprintf(stderr, "FAIL: hold\n"); return 1; }
    printf("HOLD: n_tokens=%d state=%llu B\n", n1, (unsigned long long)base.size);

    /* 2. file roundtrip */
    if (kvcb_save(&base, out_path) != 0) { fprintf(stderr, "FAIL: save\n"); return 1; }
    KVColdBase base2; kvcb_init(&base2);
    if (kvcb_load(&base2, out_path) != 0) { fprintf(stderr, "FAIL: load\n"); return 1; }
    int file_rt = (base2.size == base.size && base2.n_tokens == n1 &&
                   memcmp(base2.blob, base.blob, base.size) == 0);
    printf("FILE-RT: %s (%llu B -> %s)\n", file_rt ? "PASS" : "FAIL",
           (unsigned long long)base2.size, out_path);
    kvcb_clear(&base2);

    /* 3. advance live state */
    int32_t n2 = decode_text(vocab, ctx, P_EXTRA, 0, toks, n1);
    if (n2 < 0) { fprintf(stderr, "FAIL: decode extra\n"); return 1; }
    size_t live_sz = llama_state_seq_get_size(ctx, 0);
    printf("ADVANCE: +%d tokens (total %d) live=%llu B growth=%.0f B/tok\n",
           n2, n1 + n2, (unsigned long long)live_sz,
           (double)(live_sz - base.size) / (double)n2);

    /* 4. RESUME + verify byte-equal */
    size_t applied = kvcb_resume(ctx, 0, &base);
    size_t chk_sz = llama_state_seq_get_size(ctx, 0);
    uint8_t *chk = (uint8_t *)malloc(chk_sz);
    int resume_rt = 0;
    if (chk && llama_state_seq_get_data(ctx, chk, chk_sz, 0) == chk_sz)
        resume_rt = (chk_sz == base.size && memcmp(chk, base.blob, base.size) == 0);
    free(chk);
    printf("RESUME-RT: %s (applied=%llu expect=%llu)\n",
           resume_rt ? "PASS" : "FAIL",
           (unsigned long long)applied, (unsigned long long)base.size);

    /* 5. clipboard replace: second hold frees first (no leak path check).
       NOTE: resume rewound live state to the base, so the re-held blob is
       the base state again — label it with base.n_tokens, not n1+n2. */
    if (kvcb_hold(ctx, 0, &base, base.n_tokens) != 0) { fprintf(stderr, "FAIL: re-hold\n"); return 1; }
    printf("RE-HOLD: n_tokens=%d state=%llu B (clipboard replaced)\n",
           base.n_tokens, (unsigned long long)base.size);
    kvcb_clear(&base); /* EXPIRED delete shape */

    int ok = file_rt && resume_rt;
    printf("%s: hold/resume/file-rt %s\n", ok ? "PASS" : "FAIL", ok ? "all bit-exact" : "MISMATCH");
    llama_free(ctx);
    llama_model_free(model);
    return ok ? 0 : 1;
}
