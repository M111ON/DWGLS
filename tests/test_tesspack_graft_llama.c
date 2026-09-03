/* test_tesspack_graft_llama.c — llama.cpp inference on tesspack graft GGUF
 *
 * Loads original GGUF and tesspack-grafted GGUF, runs same prompt,
 * compares logits bitwise + multi-token generation.
 *
 * BUILD (llama.cpp at I:/llama, DLLs at I:/llama/llama-b9733-bin-win-vulkan-x64):
 *   gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
 *       -I core -I I:/llama/include -o build/test_tesspack_graft_llama \
 *       tests/test_tesspack_graft_llama.c \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll \
 *       -lzstd -lm
 *   PATH="I:/llama/llama-b9733-bin-win-vulkan-x64:$PATH" ./build/test_tesspack_graft_llama
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "llama.h"
#include "ggml-backend.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

struct llama_model *load_model(const char *path) {
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    return llama_model_load_from_file(path, mp);
}

int main(int argc, char **argv) {
    const char *original = (argc > 1) ? argv[1] : "F:/model/qwen3-4b-moe-q4_k_m.gguf";
    const char *graft    = (argc > 2) ? argv[2] : "F:/model/moe_tesspack_graft.gguf";
    const char *dll_dir  = (argc > 3) ? argv[3] : "I:/llama/llama-b9733-bin-win-vulkan-x64";
    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
    SetDllDirectoryA(dll_dir);
#endif

    printf("Tesspack Graft — llama.cpp inference verification\n");
    printf("Original: %s\n", original);
    printf("Graft:    %s\n", graft);
    printf("═══════════════════════════════════════════════════════════════\n");

    llama_backend_init();
    ggml_backend_load_all_from_path(dll_dir);

    /* ── T1: both load ── */
    struct llama_model *mO = load_model(original);
    struct llama_model *mG = load_model(graft);
    CHECK("T1: original loads", mO != NULL);
    CHECK("T2: graft loads", mG != NULL);
    if (!mO || !mG) {
        llama_model_free(mO); llama_model_free(mG);
        printf("\nRESULTS: %u/%u PASS\n", pass_count, pass_count + fail_count);
        return 1;
    }

    /* ── T3: metadata matches ── */
    {
        int embd_match  = llama_model_n_embd(mO)  == llama_model_n_embd(mG);
        int layer_match = llama_model_n_layer(mO) == llama_model_n_layer(mG);
        int vocab_match = llama_vocab_n_tokens(llama_model_get_vocab(mO))
                       == llama_vocab_n_tokens(llama_model_get_vocab(mG));
        CHECK("T3: n_embd matches", embd_match);
        CHECK("T4: n_layer matches", layer_match);
        CHECK("T5: vocab size matches", vocab_match);
        printf("     original: n_embd=%d n_layer=%d vocab=%d\n",
               llama_model_n_embd(mO), llama_model_n_layer(mO),
               llama_vocab_n_tokens(llama_model_get_vocab(mO)));
        printf("     graft:    n_embd=%d n_layer=%d vocab=%d\n",
               llama_model_n_embd(mG), llama_model_n_layer(mG),
               llama_vocab_n_tokens(llama_model_get_vocab(mG)));
    }

    /* ── T6: logits bitwise identical ── */
    {
        struct llama_context_params cp = llama_context_default_params();
        cp.n_batch = 2048;
        struct llama_context *ctxO = llama_init_from_model(mO, cp);
        struct llama_context *ctxG = llama_init_from_model(mG, cp);
        CHECK("T6: contexts init", ctxO != NULL && ctxG != NULL);
        if (!ctxO || !ctxG) {
            llama_free(ctxO); llama_free(ctxG);
            llama_model_free(mO); llama_model_free(mG);
            return 1;
        }

        const char *prompt = "The capital of France is";
        llama_token toks[64];
        const struct llama_vocab *vocab = llama_model_get_vocab(mO);
        int32_t nt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                    toks, 64, true, false);
        CHECK("T7: tokenize prompt", nt > 0);
        if (nt <= 0) {
            llama_free(ctxO); llama_free(ctxG);
            llama_model_free(mO); llama_model_free(mG);
            return 1;
        }

        /* decode prompt on both */
        llama_batch bO = llama_batch_get_one(toks, nt);
        llama_batch bG = llama_batch_get_one(toks, nt);
        int dO = llama_decode(ctxO, bO);
        int dG = llama_decode(ctxG, bG);
        CHECK("T8: decode prompt (both)", dO == 0 && dG == 0);

        /* compare logits */
        const float *logO = llama_get_logits(ctxO);
        const float *logG = llama_get_logits(ctxG);
        int nv = llama_vocab_n_tokens(vocab);

        uint64_t diffs = 0;
        float maxdiff = 0.0f;
        for (int i = 0; i < nv; i++) {
            float d = logO[i] > logG[i] ? logO[i] - logG[i] : logG[i] - logO[i];
            if (d > maxdiff) maxdiff = d;
            if (logO[i] != logG[i]) diffs++;
        }
        CHECK("T9: logits BITWISE identical", diffs == 0);
        printf("     n_vocab=%d  diffs=%llu  maxdiff=%.6e\n",
               nv, (unsigned long long)diffs, maxdiff);

        /* greedy next token */
        int32_t bestO = 0, bestG = 0;
        for (int i = 1; i < nv; i++) {
            if (logO[i] > logO[bestO]) bestO = i;
            if (logG[i] > logG[bestG]) bestG = i;
        }
        CHECK("T10: greedy next token identical", bestO == bestG);
        printf("     next token: original=%d graft=%d\n", bestO, bestG);

        /* ── T11: multi-token generation (24 tokens) ── */
        struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

        int n_gen = 24;
        int token_match = 1;
        printf("     generate %d tokens:\n", n_gen);
        for (int g = 0; g < n_gen; g++) {
            llama_token tO = llama_sampler_sample(smpl, ctxO, -1);
            llama_token tG = llama_sampler_sample(smpl, ctxG, -1);
            llama_sampler_accept(smpl, tO);

            if (tO != tG) {
                printf("     step %d: orig=%d graft=%d MISMATCH\n", g, tO, tG);
                token_match = 0;
                break;
            }

            char buf[64];
            int k = llama_token_to_piece(vocab, tO, buf, sizeof(buf)-1, 0, false);
            if (k < 0) k = 0;
            buf[k] = '\0';
            printf("       %s", buf);

            llama_batch b = llama_batch_get_one(&tO, 1);
            if (llama_decode(ctxO, b) != 0 || llama_decode(ctxG, b) != 0) {
                fprintf(stderr, "\n  FAIL: decode step %d\n", g);
                token_match = 0;
                break;
            }
        }
        printf("\n");
        CHECK("T11: 24-token generation BITWISE identical", token_match);

        llama_sampler_free(smpl);
        llama_free(ctxO); llama_free(ctxG);
    }

    llama_model_free(mO); llama_model_free(mG);
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
