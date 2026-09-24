/* tools/kv_delta_map.c — one-token footprint: dump at N then N+1 tokens,
 * report changed byte ranges. Decides whether KV delta is slicable.
 *
 * BUILD: gcc -O2 -I I:/llama/include -o build/kv_delta_map.exe tools/kv_delta_map.c <llama.dll> <ggml dlls>
 * RUN:   ./build/kv_delta_map <model.gguf> ; reads build/kvslots/turn1.bin (29 tok) as base
 */
#include "llama.h"
#include "ggml-backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
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

    /* same prefix as kv_dump_turns turn1 (29 tokens) + 1 continuation token */
    const char *t1 =
        "<|im_start|>user\nWhat is the capital of France? Answer in one sentence.<|im_end|>\n"
        "<|im_start|>assistant\nThe capital of France is Paris.<|im_end|>\n";
    const char *t2 = "<|im_start|>user\nNow tell me one famous landmark there, one sentence.<|im_end|>\n";
    llama_token toks[4096];
    int32_t n1 = llama_tokenize(vocab, t1, (int32_t)strlen(t1), toks, 4096, true, true);
    int32_t n2 = llama_tokenize(vocab, t2, (int32_t)strlen(t2), toks + n1, 4096 - n1, false, true);
    printf("n1=%d n2=%d\n", n1, n2);
    if (llama_decode(ctx, llama_batch_get_one(toks, n1))) { fprintf(stderr, "decode1 fail\n"); return 1; }

    size_t sa = llama_state_seq_get_size(ctx, 0);
    uint8_t *A = (uint8_t *)malloc(sa);
    if (!A || llama_state_seq_get_data(ctx, A, sa, 0) != sa) { fprintf(stderr, "state A fail\n"); return 1; }

    if (llama_decode(ctx, llama_batch_get_one(toks + n1, 1))) { fprintf(stderr, "decode2 fail\n"); return 1; }
    size_t sb = llama_state_seq_get_size(ctx, 0);
    uint8_t *B = (uint8_t *)malloc(sb);
    if (!B || llama_state_seq_get_data(ctx, B, sb, 0) != sb) { fprintf(stderr, "state B fail\n"); return 1; }
    printf("size %llu -> %llu (growth %llu)\n",
           (unsigned long long)sa, (unsigned long long)sb, (unsigned long long)(sb - sa));

    /* changed ranges in shared part, merged within 256B gaps */
    size_t L = sa < sb ? sa : sb;
    size_t i = 0, nchg = 0, nreg = 0, rstart = 0;
    int inreg = 0;
    size_t gap = 0;
    while (i < L) {
        if (A[i] != B[i]) {
            if (!inreg) { rstart = i; inreg = 1; nreg++; }
            nchg++; gap = 0;
        } else if (inreg) {
            if (++gap > 256) {
                printf("  region [%llu,%llu) len %llu\n",
                       (unsigned long long)rstart, (unsigned long long)(i - gap + 1),
                       (unsigned long long)(i - gap + 1 - rstart));
                inreg = 0;
            }
        }
        i++;
    }
    if (inreg) printf("  region [%llu,%llu) len %llu\n",
                      (unsigned long long)rstart, (unsigned long long)i, (unsigned long long)(i - rstart));
    printf("changed bytes in shared %llu: %llu (%.2f%%), regions: %llu\n",
           (unsigned long long)L, (unsigned long long)nchg, 100.0 * nchg / L,
           (unsigned long long)nreg);
    /* is A's tail preserved anywhere in B? (shifted layout check) */
    size_t tail = 65536;
    if (sa > tail) {
        int found = 0;
        for (size_t o = 0; o + tail <= sb && !found; o += 16)
            if (memcmp(A + sa - tail, B + o, tail) == 0) { printf("A-tail found in B at %llu\n", (unsigned long long)o); found = 1; }
        if (!found) printf("A-tail NOT found contiguously in B\n");
    }
    /* save both for offline layout analysis */
    FILE *fa = fopen("build/kvslots/deltaA.bin", "wb");
    if (fa) { fwrite(A, 1, sa, fa); fclose(fa); }
    FILE *fb = fopen("build/kvslots/deltaB.bin", "wb");
    if (fb) { fwrite(B, 1, sb, fb); fclose(fb); }
    free(A); free(B);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}
