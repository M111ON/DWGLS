/*
 * tools/gguf_gpu_zerocopy.c — GPU zero-copy inference via DRamTile pinned memory
 *
 * Flow:
 *   1. mmap .tesspack (or .gguf) read-only
 *   2. cudaHostRegister → pin pages for GPU direct access
 *   3. cudaHostGetDevicePointer → device-accessible pointer
 *   4. llama_model_init_from_user callback: t->data = device pointer (ZERO COPY)
 *   5. GPU reads weights directly from pinned host memory via PCIe
 *   6. No H2D copy needed — weights arrive on-demand via page faults
 *
 * Compare with: gguf_lazy_serve.c (CPU zero-copy), iso_user_path.c (memcpy)
 *
 * BUILD: gcc -O2 -Wall -I../core -I~/llama.cpp/include -I~/llama.cpp/ggml/include \
 *        -o gguf_gpu_zerocopy gguf_gpu_zerocopy.c \
 *        -L~/llama.cpp/build_gpu/bin -lllama -lggml -lggml-base -lggml-cuda \
 *        -lcudart -lm -lpthread
 * RUN:   ./gguf_gpu_zerocopy model.gguf [prompt]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <cuda_runtime.h>
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "../core/gguf_reader.h"

#define CUDA_CHECK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA ERROR: %s\n", cudaGetErrorString(_e)); \
        return 1; \
    } \
} while(0)

typedef struct {
    GgufReader *gguf;
    uint8_t *mmap_base;       /* mmap'd file base */
    size_t mmap_size;
    uint8_t *d_pinned;        /* device-accessible pointer (zero-copy) */
    uint32_t matched, missing;
    uint64_t bytes_served;
} GPUZeroCopyCtx;

static void provide_tensor_gpu(struct ggml_tensor *t, void *ud) {
    GPUZeroCopyCtx *ctx = (GPUZeroCopyCtx *)ud;
    const char *name = ggml_get_name(t);
    if (!name || name[0] == '\0') return;

    for (uint32_t i = 0; i < ctx->gguf->n_tensors; i++) {
        if (strcmp(ctx->gguf->names[i], name) == 0) {
            size_t nb = ggml_nbytes(t);
            if (nb == ctx->gguf->sizes[i]) {
                /* ZERO-COPY: point t->data into pinned device memory */
                uint64_t file_off = ctx->gguf->data_offset + ctx->gguf->offsets[i];
                t->data = (void *)(ctx->d_pinned + file_off);
                ctx->bytes_served += nb;
                ctx->matched++;
                return;
            }
        }
    }

    /* Missing tensors: zero-fill (same as file-load defaults) */
    memset(t->data, 0, ggml_nbytes(t));
    ctx->missing++;
}

static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
        fputs(text, stderr);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [prompt] [n_gen]\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *prompt = (argc > 2) ? argv[2] : "Paris. It is the";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 32;

    fprintf(stderr, "=== GPU Zero-Copy Inference ===\n");
    fprintf(stderr, "Model: %s\n", gguf_path);

    /* ── Step 1: Open GGUF via gguf_reader (mmap) ── */
    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        fprintf(stderr, "FATAL: cannot open %s\n", gguf_path);
        return 1;
    }
    fprintf(stderr, "GGUF: %u tensors, data_offset=%llu\n",
            gguf.n_tensors, (unsigned long long)gguf.data_offset);

    /* ── Step 2: Pin mmap'd memory for GPU access ── */
    fprintf(stderr, "\n=== Pinning host memory for GPU zero-copy ===\n");
    CUDA_CHECK(cudaSetDevice(0));

    int dev;
    cudaGetDevice(&dev);
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, dev);
    fprintf(stderr, "GPU: %s (sm_%d%d), VRAM: %.1f GB\n",
            prop.name, prop.major, prop.minor, prop.totalGlobalMem / 1e9);

    CUDA_CHECK(cudaHostRegister(gguf.base, gguf.base_sz, cudaHostRegisterReadOnly));

    uint8_t *d_pinned = NULL;
    CUDA_CHECK(cudaHostGetDevicePointer((void **)&d_pinned, (void *)gguf.base, 0));
    fprintf(stderr, "Device pointer: %p (offset from host: %td)\n",
            d_pinned, (ptrdiff_t)(d_pinned - gguf.base));

    /* ── Step 3: Load model via user-path callback (zero-copy) ── */
    fprintf(stderr, "\n=== Loading model (zero-copy callback) ===\n");
    GPUZeroCopyCtx ctx = {
        .gguf = &gguf,
        .mmap_base = gguf.base,
        .mmap_size = gguf.base_sz,
        .d_pinned = d_pinned,
        .matched = 0, .missing = 0, .bytes_served = 0
    };

    struct llama_model_params mp = llama_model_default_params();
    mp.no_alloc = true;   /* callback will set t->data into pinned memory */
    mp.use_mmap = false;  /* we provide our own mmap */
    mp.n_gpu_layers = 99; /* offload all to GPU */

    /* We need to pass the GGUF header to llama for metadata parsing.
     * llama_model_init_from_user expects (header_buf, header_size, callback, ud).
     * The header is the first data_offset bytes of the GGUF. */
    struct llama_model *model = llama_model_init_from_user(
        gguf.base, gguf.data_offset, provide_tensor_gpu, &ctx, &mp);

    if (!model) {
        fprintf(stderr, "FATAL: llama_model_init_from_user failed\n");
        cudaHostUnregister(gguf.base);
        gguf_close(&gguf);
        return 1;
    }

    fprintf(stderr, "Model loaded: %u matched, %u missing, %.1f MB served\n",
            ctx.matched, ctx.missing, ctx.bytes_served / 1e6);

    /* ── Step 4: Generate tokens ── */
    fprintf(stderr, "\n=== Generating (prompt: \"%s\", %d tokens) ===\n", prompt, n_gen);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048;
    cp.n_batch = 512;
    cp.n_threads = 4;
    cp.n_threads_batch = 4;

    struct llama_context *lctx = llama_init_from_model(model, cp);
    if (!lctx) {
        fprintf(stderr, "FATAL: llama_init_from_model failed\n");
        llama_model_free(model);
        cudaHostUnregister(gguf.base);
        gguf_close(&gguf);
        return 1;
    }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    llama_token toks[64];
    int32_t np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                toks, 64, true, false);
    if (np < 0) np = -np;

    if (llama_decode(lctx, llama_batch_get_one(toks, np)) != 0) {
        fprintf(stderr, "FATAL: decode failed\n");
        llama_free(lctx); llama_model_free(model);
        cudaHostUnregister(gguf.base); gguf_close(&gguf);
        return 1;
    }

    fprintf(stdout, "%s", prompt);
    for (int i = 0; i < n_gen; i++) {
        float * logits = llama_get_logits(lctx);
        int n_vocab = llama_vocab_n_tokens(vocab);
        llama_token best = 0;
        float best_val = logits[0];
        for (int v = 1; v < n_vocab; v++)
            if (logits[v] > best_val) { best_val = logits[v]; best = v; }
        if (best == llama_vocab_eos(vocab)) break;
        char buf[256];
        int n = llama_token_to_piece(vocab, best, buf, sizeof(buf), 0, true);
        if (n > 0) { buf[n] = '\0'; fprintf(stdout, "%s", buf); }
        llama_batch batch = llama_batch_get_one(&best, 1);
        if (llama_decode(lctx, batch) != 0) break;
    }
    fprintf(stdout, "\n");

    /* ── Cleanup ── */
    llama_free(lctx);
    llama_model_free(model);
    CUDA_CHECK(cudaHostUnregister(gguf.base));
    gguf_close(&gguf);

    fprintf(stderr, "\n=== Done ===\n");
    fprintf(stderr, "Zero-copy: %u matched, %u missing, %.1f MB served via pinned memory\n",
            ctx.matched, ctx.missing, ctx.bytes_served / 1e6);
    return 0;
}
