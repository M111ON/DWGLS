/* kv_quant_bitexact — teacher-forced probe: does K8V4 KV give the same
 * next-token argmax as F16? Run with KV_TYPE=f16 and KV_TYPE=k8v4, compare.
 * Prints: argmax token, its logit, top-5 ids+logits, and a logit checksum.
 * Read-only w.r.t. the repo; writes nothing.
 *
 * BUILD (MSYS gcc, same as kv-cold-chat):
 *   gcc -O2 -std=c11 -I core -I I:/llama/include -o build/kv_quant_bitexact \
 *       tools/kv_quant_bitexact.c \
 *       I:/llama/llama.cpp/build_zc2/bin/Release/{llama,ggml,ggml-base,ggml-cpu-x64}.dll -lm
 */
#include "llama.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static struct llama_context *mk_ctx(struct llama_model *m, enum ggml_type tk, enum ggml_type tv) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096; cp.n_batch = 512; cp.no_perf = true;
    cp.type_k = tk; cp.type_v = tv;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return llama_init_from_model(m, cp);
}

static int32_t tokenize(const struct llama_vocab *v, const char *s, int add, llama_token *out, int cap) {
    return llama_tokenize(v, s, (int32_t)strlen(s), out, cap, add, false);
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *kt = getenv("KV_TYPE"); if (!kt) kt = "f16";
    enum ggml_type tk = GGML_TYPE_F16, tv = GGML_TYPE_F16;
    if (!strcmp(kt, "q8v8")) { tk = GGML_TYPE_Q8_0; tv = GGML_TYPE_Q8_0; }
    else if (!strcmp(kt, "q4v4")) { tk = GGML_TYPE_Q4_0; tv = GGML_TYPE_Q4_0; }
    else if (!strcmp(kt, "k8v4")) { tk = GGML_TYPE_Q8_0; tv = GGML_TYPE_Q4_0; }

    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL model\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);

    struct llama_context *ctx = mk_ctx(model, tk, tv);
    if (!ctx) { fprintf(stderr, "FAIL ctx\n"); return 1; }

    /* teacher-forced prompt: user turn + assistant opener */
    const char *prompt = "<|im_start|>user\nสวัสดีครับ ช่วยแนะนำตัวสั้นๆ<|im_end|>\n<|im_start|>assistant\n";
    static llama_token toks[2048];
    int32_t n = tokenize(vocab, prompt, true, toks, 2048);
    if (n <= 0) { fprintf(stderr, "FAIL tokenize %d\n", n); return 1; }

    llama_batch b = llama_batch_init(n, 0, 1);
    for (int32_t i = 0; i < n; i++) {
        b.token[i] = toks[i]; b.pos[i] = i; b.n_seq_id[i] = 1; b.seq_id[i][0] = 0;
        b.logits[i] = 1;
    }
    b.n_tokens = n;
    if (llama_decode(ctx, b)) { fprintf(stderr, "FAIL decode\n"); return 1; }
    llama_batch_free(b);

    const float *lg = llama_get_logits_ith(ctx, n - 1);
    if (!lg) { fprintf(stderr, "FAIL logits\n"); return 1; }

    /* argmax + logit checksum + top-5 */
    int bi = 0; double sum = 0.0; for (int32_t i = 0; i < n_vocab; i++) { if (lg[i] > lg[bi]) bi = i; sum += (double)lg[i]; }
    int top[5] = {-1,-1,-1,-1,-1}; float tv5[5];
    for (int32_t i = 0; i < n_vocab; i++) {
        for (int r = 0; r < 5; r++) {
            if (top[r] < 0 || lg[i] > tv5[r]) {
                for (int s = 4; s > r; s--) { top[s] = top[s-1]; tv5[s] = tv5[s-1]; }
                top[r] = i; tv5[r] = lg[i]; break;
            }
        }
    }
    printf("KV_TYPE=%s type_k=%d type_v=%d n_prompt=%d\n", kt, (int)tk, (int)tv, n);
    printf("ARGMAX id=%d logit=%.6f\n", bi, lg[bi]);
    printf("LOGIT_SUM=%.6f\n", sum);
    printf("TOP5:");
    for (int r = 0; r < 5; r++) printf(" %d:%.6f", top[r], tv5[r]);
    printf("\n");

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
