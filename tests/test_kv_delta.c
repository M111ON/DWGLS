/* tests/test_kv_delta.c — logical-delta mechanism proof on real llama KV.
 *
 * BASE dump at N tokens + decode K continuation tokens, then on a FRESH
 * context: set_data(BASE) + decode the same K tokens → RESULT.
 * Oracle: byte-identical RESULT == FULL reference dump (decode determinism
 * makes logical deltas exact; no byte-slicing of the repacked state needed).
 *
 * BUILD: gcc -O2 -I I:/llama/include -o build/test_kv_delta.exe tests/test_kv_delta.c <llama.dll> <ggml dlls>
 * RUN:   ./build/test_kv_delta [model.gguf]   (SKIP exit 2 if model absent)
 */
#include "llama.h"
#include "ggml-backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int decode_toks(struct llama_context *ctx, const llama_token *toks, int n, int pos0) {
    for (int off = 0; off < n; ) {
        int chunk = n - off > 512 ? 512 : n - off;
        struct llama_batch b = llama_batch_init(chunk, 0, 1);
        for (int j = 0; j < chunk; j++) {
            b.token[j] = toks[off + j];
            b.pos[j] = pos0 + off + j;
            b.n_seq_id[j] = 1;
            b.seq_id[j][0] = 0;
            b.logits[j] = 0;
        }
        b.n_tokens = chunk;
        int rc = llama_decode(ctx, b);
        llama_batch_free(b);
        if (rc != 0) return -1;
        off += chunk;
    }
    return 0;
}

static uint8_t *snap(struct llama_context *ctx, size_t *sz) {
    *sz = llama_state_seq_get_size(ctx, 0);
    uint8_t *buf = (uint8_t *)malloc(*sz);
    if (!buf || llama_state_seq_get_data(ctx, buf, *sz, 0) != *sz) { free(buf); return NULL; }
    return buf;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    FILE *probe = fopen(model_path, "rb");
    if (!probe) { printf("SKIP: model absent\n"); return 2; }
    fclose(probe);
    ggml_backend_load_all();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { printf("FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.no_perf = true;

    const char *p1 = "<|im_start|>user\nName three planets.<|im_end|>\n<|im_start|>assistant\n";
    const char *p2 = "Mercury Venus Earth.";
    llama_token t1[512], t2[512];
    int32_t n1 = llama_tokenize(vocab, p1, (int32_t)strlen(p1), t1, 512, true, true);
    int32_t n2 = llama_tokenize(vocab, p2, (int32_t)strlen(p2), t2, 512, false, true);
    if (n1 <= 0 || n2 <= 0) { printf("FAIL: tokenize\n"); return 1; }

    int pass = 0, fail = 0;
#define CHECK(ok, name) do { if (ok) { pass++; printf("  ok %s\n", name); } else { fail++; printf("  FAIL %s\n", name); } } while (0)

    /* A: full run → BASE @n1, FULL @(n1+n2) */
    struct llama_context *ctxA = llama_init_from_model(model, cp);
    int okA = ctxA && decode_toks(ctxA, t1, n1, 0) == 0;
    size_t szB = 0, szF = 0;
    uint8_t *BASE = okA ? snap(ctxA, &szB) : NULL;
    okA = okA && BASE && decode_toks(ctxA, t2, n2, n1) == 0;
    uint8_t *FULL = okA ? snap(ctxA, &szF) : NULL;
    CHECK(okA && BASE && FULL, "full run snapshots");

    /* B: fresh ctx, set BASE, decode delta t2 → RESULT must equal FULL */
    struct llama_context *ctxB = llama_init_from_model(model, cp);
    int okB = ctxB && BASE &&
              llama_state_seq_set_data(ctxB, BASE, szB, 0) == szB &&
              decode_toks(ctxB, t2, n2, n1) == 0;
    size_t szR = 0;
    uint8_t *RESULT = okB ? snap(ctxB, &szR) : NULL;
    CHECK(okB && RESULT, "base restore + delta decode");
    CHECK(RESULT && FULL && szR == szF && memcmp(RESULT, FULL, szF) == 0,
          "RESULT byte-identical to FULL reference");

    /* C: wrong-base guard — delta applied on an UNRELATED base must differ
     * (proves deltas are base-bound, never silently interchangeable) */
    struct llama_context *ctxC = llama_init_from_model(model, cp);
    const char *p3 = "<|im_start|>user\nName three fruits.<|im_end|>\n<|im_start|>assistant\n";
    llama_token t3[512];
    int32_t n3 = llama_tokenize(vocab, p3, (int32_t)strlen(p3), t3, 512, true, true);
    int okC = ctxC && n3 == n1 && decode_toks(ctxC, t3, n3, 0) == 0;
    size_t szW = 0;
    uint8_t *WRONG = okC ? snap(ctxC, &szW) : NULL;
    struct llama_context *ctxD = llama_init_from_model(model, cp);
    int okD = ctxD && WRONG && szW == szB &&
              llama_state_seq_set_data(ctxD, WRONG, szW, 0) == szW &&
              decode_toks(ctxD, t2, n2, n1) == 0;
    size_t szX = 0;
    uint8_t *XD = okD ? snap(ctxD, &szX) : NULL;
    CHECK(okD && XD && szX == szF && memcmp(XD, FULL, szF) != 0,
          "delta on wrong base diverges (base-bound, needs checksum gate)");

    free(BASE); free(FULL); free(RESULT); free(WRONG); free(XD);
    llama_free(ctxA); llama_free(ctxB); llama_free(ctxC); llama_free(ctxD);
    llama_model_free(model);
    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
