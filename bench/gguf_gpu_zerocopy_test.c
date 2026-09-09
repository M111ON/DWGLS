/*
 * gguf_gpu_zerocopy_test.c — Zero-copy GPU inference: mmap + cudaHostRegister
 *
 * Proves: GPU reads weights directly from pinned host memory (no H2D copy).
 * Callback: t->data = d_pinned + offset (device pointer into pinned mmap).
 *
 * Build (Colab T4):
 *   gcc -O2 -Wall \
 *     -I/content/llama.cpp/include -I/content/llama.cpp/ggml/include \
 *     -I/content/DWGLS-native-fs/core \
 *     -o zerocopy_test gguf_gpu_zerocopy_test.c \
 *     -L/content/llama.cpp/build/bin -llama -lggml -lggml-base \
 *     -L/usr/local/cuda/lib64 -lcudart -lm -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cuda_runtime.h>
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"

#define CUDA_CHECK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s\n", __FILE__, __LINE__, \
                cudaGetErrorString(_e)); return 1; \
    } \
} while(0)

/* ═══ GGUF reader (minimal, from core/gguf_reader.h) ═══ */
#include "gguf_reader.h"

/* ═══ Zero-copy context ═══ */
typedef struct {
    GgufReader *gguf;
    uint8_t *d_pinned;     /* device pointer into pinned mmap */
    uint32_t matched, missing;
    uint64_t bytes_served;
} ZCContext;

static void provide_tensor(struct ggml_tensor *t, void *ud) {
    ZCContext *z = (ZCContext *)ud;
    const char *name = ggml_get_name(t);
    if (!name || !name[0]) return;

    for (uint32_t i = 0; i < z->gguf->n_tensors; i++) {
        if (strcmp(z->gguf->names[i], name) == 0) {
            size_t nb = ggml_nbytes(t);
            if (nb == z->gguf->sizes[i]) {
                /* ZERO-COPY: t->data points into pinned device memory.
                 * GPU reads weights directly via PCIe — no H2D copy. */
                uint64_t file_off = z->gguf->data_offset + z->gguf->offsets[i];
                t->data = (void *)(z->d_pinned + file_off);
                z->bytes_served += nb;
                z->matched++;
                return;
            }
        }
    }

    /* Missing: zero-fill (safe default) */
    memset(t->data, 0, ggml_nbytes(t));
    z->missing++;
}

static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud; (void)level; fputs(text, stderr);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s model.gguf [prompt] [n_gen]\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *prompt = (argc > 2) ? argv[2] : "Paris. It is the";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 32;

    fprintf(stderr, "=== GPU Zero-Copy Inference Test ===\n");
    fprintf(stderr, "Model: %s\nPrompt: \"%s\"\n", gguf_path, prompt);

    /* ── CUDA setup ── */
    CUDA_CHECK(cudaSetDevice(0));
    int dev; cudaGetDevice(&dev);
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, dev);
    fprintf(stderr, "GPU: %s (sm_%d%d), VRAM: %.1f GB\n\n",
            prop.name, prop.major, prop.minor, prop.totalGlobalMem / 1e9);

    /* ── Open GGUF (mmap) ── */
    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        fprintf(stderr, "FATAL: cannot open %s\n", gguf_path);
        return 1;
    }
    fprintf(stderr, "GGUF: %u tensors, data_offset=%llu, file=%llu bytes\n",
            gguf.n_tensors, (unsigned long long)gguf.data_offset,
            (unsigned long long)gguf.base_sz);

    /* ── Pin mmap for GPU zero-copy ── */
    fprintf(stderr, "\nPinning %llu bytes for GPU access... ",
            (unsigned long long)gguf.base_sz);
    CUDA_CHECK(cudaHostRegister(gguf.base, gguf.base_sz, cudaHostRegisterReadOnly));

    uint8_t *d_pinned = NULL;
    CUDA_CHECK(cudaHostGetDevicePointer((void **)&d_pinned, (void *)gguf.base, 0));
    fprintf(stderr, "OK (device ptr offset: %td)\n", (ptrdiff_t)(d_pinned - gguf.base));

    /* ── Load model via user-path callback (zero-copy) ── */
    fprintf(stderr, "\nLoading model (zero-copy callback)...\n");
    ZCContext ctx = {
        .gguf = &gguf,
        .d_pinned = d_pinned,
        .matched = 0, .missing = 0, .bytes_served = 0
    };

    struct llama_model_params mp = llama_model_default_params();
    mp.no_alloc = true;   /* callback sets t->data — no internal alloc */

    llama_log_set(quiet_log, NULL);
    struct llama_model *model = llama_model_init_from_user(
        gguf.base, gguf.data_offset, provide_tensor, &ctx, &mp);

    if (!model) {
        fprintf(stderr, "FATAL: model load failed\n");
        cudaHostUnregister(gguf.base);
        gguf_close(&gguf);
        return 1;
    }
    fprintf(stderr, "Loaded: %u matched, %u missing, %.1f MB zero-copy\n",
            ctx.matched, ctx.missing, ctx.bytes_served / 1e6);

    /* ── Generate ── */
    fprintf(stderr, "\nGenerating %d tokens...\n", n_gen);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.n_threads = 4;
    struct llama_context *lctx = llama_init_from_model(model, cp);
    if (!lctx) { fprintf(stderr, "FATAL: context init failed\n"); return 1; }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    llama_token toks[64];
    int32_t np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                toks, 64, true, false);
    if (np < 0) np = -np;

    if (llama_decode(lctx, llama_batch_get_one(toks, np)) != 0) {
        fprintf(stderr, "FATAL: decode failed\n"); return 1;
    }

    fprintf(stdout, "%s", prompt);
    for (int i = 0; i < n_gen; i++) {
        float *logits = llama_get_logits(lctx);
        int nv = llama_vocab_n_tokens(vocab);
        llama_token best = 0;
        float bv = logits[0];
        for (int v = 1; v < nv; v++)
            if (logits[v] > bv) { bv = logits[v]; best = v; }
        if (best == llama_vocab_eos(vocab)) break;
        char buf[128];
        int n = llama_token_to_piece(vocab, best, buf, sizeof(buf), 0, true);
        if (n > 0) { buf[n] = '\0'; fprintf(stdout, "%s", buf); }
        llama_decode(lctx, llama_batch_get_one(&best, 1));
    }
    fprintf(stdout, "\n");

    /* ── Cleanup ── */
    llama_free(lctx);
    llama_model_free(model);
    CUDA_CHECK(cudaHostUnregister(gguf.base));
    gguf_close(&gguf);

    fprintf(stderr, "\n=== Done ===\n");
    fprintf(stderr, "Zero-copy: %u/%u tensors, %.1f MB via pinned PCIe\n",
            ctx.matched, ctx.matched + ctx.missing, ctx.bytes_served / 1e6);
    return 0;
}
