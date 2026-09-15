/* Minimal control: plain GGUF mmap served through llama_model_init_from_user. */
#define _CRT_SECURE_NO_WARNINGS
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "gguf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <windows.h>

typedef struct {
    const uint8_t *base;
    size_t size;
    struct gguf_context *meta;
    unsigned served;
    unsigned synthetic;
    int copy;
} Hook;

static void quiet_log(enum ggml_log_level level, const char * text, void * user_data) {
    (void) user_data;
    if (level >= GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

static void provide_tensor(struct ggml_tensor * t, void * ud) {
    Hook * h = (Hook *) ud;
    const char * name = ggml_get_name(t);
    int64_t idx = name ? gguf_find_tensor(h->meta, name) : -1;
    if (idx >= 0) {
        size_t off = gguf_get_data_offset(h->meta) + gguf_get_tensor_offset(h->meta, idx);
        size_t nb = ggml_nbytes(t);
        if (off <= h->size && nb <= h->size - off) {
            if (h->copy) {
                t->data = _aligned_malloc(nb, 32);
                if (!t->data) return;
                memcpy(t->data, h->base + off, nb);
            } else {
                t->data = (void *) (h->base + off);
            }
            h->served++;
            return;
        }
        fprintf(stderr, "OUT OF RANGE: %s off=%zu bytes=%zu file=%zu\n", name, off, nb, h->size);
    }

    /* llama's user schema may contain optional tensors absent from this GGUF. */
    t->data = _aligned_malloc(ggml_nbytes(t), 32);
    if (!t->data) return;
    h->synthetic++;
    if (name && strstr(name, "scale")) {
        float * p = (float *) t->data;
        for (size_t i = 0; i < ggml_nbytes(t) / sizeof(float); i++) p[i] = 1.0f;
    }
}

static int map_file(const char * path, HANDLE * hf, HANDLE * hm,
                    const uint8_t ** base, size_t * size) {
    LARGE_INTEGER n;
    *hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (*hf == INVALID_HANDLE_VALUE || !GetFileSizeEx(*hf, &n) || n.QuadPart <= 0) return 0;
    *hm = CreateFileMappingA(*hf, NULL, PAGE_READONLY, 0, 0, NULL);
    *base = *hm ? (const uint8_t *) MapViewOfFile(*hm, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!*base) return 0;
    *size = (size_t) n.QuadPart;
    return 1;
}

int main(int argc, char ** argv) {
    const char * path = argc > 1 ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char * prompt = argc > 2 ? argv[2] : "The capital of France is";
    HANDLE hf = INVALID_HANDLE_VALUE, hm = NULL;
    const uint8_t * base = NULL;
    size_t size = 0;
    if (!map_file(path, &hf, &hm, &base, &size)) {
        fprintf(stderr, "mmap failed: %s\n", path);
        return 1;
    }

    struct ggml_context * meta_ctx = NULL;
    struct gguf_init_params gip = { .no_alloc = true, .ctx = &meta_ctx };
    struct gguf_context * meta = gguf_init_from_file(path, gip);
    if (!meta) { fprintf(stderr, "metadata failed\n"); return 1; }

    Hook hook = { base, size, meta, 0, 0, argc > 4 && strcmp(argv[4], "copy") == 0 };
    llama_backend_init();
    llama_log_set(quiet_log, NULL);
    const char * backend_path = argc > 3 ? argv[3] : "I:/llama/llama-b10830-win-vulkan-x64";
    SetDllDirectoryA(backend_path);
    ggml_backend_load_all_from_path(NULL); /* discover backends beside the executable */
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model * model = llama_model_init_from_user(meta, provide_tensor, &hook, mp);
    printf("model=%s mode=%s tensors=%lld served=%u synthetic=%u\n", model ? "ok" : "FAIL",
           hook.copy ? "copy" : "mmap", (long long) gguf_get_n_tensors(meta), hook.served, hook.synthetic);
    if (!model) return 1;

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 64; cp.n_batch = 64; cp.n_threads = 8;
    struct llama_context * ctx = llama_init_from_model(model, cp);
    const struct llama_vocab * vocab = llama_model_get_vocab(model);
    int n = llama_tokenize(vocab, prompt, (int32_t) strlen(prompt), NULL, 0, true, false);
    if (n < 0) n = -n;
    llama_token * toks = malloc((size_t) n * sizeof(*toks));
    n = llama_tokenize(vocab, prompt, (int32_t) strlen(prompt), toks, n, true, false);
    int rc = ctx && n > 0 ? llama_decode(ctx, llama_batch_get_one(toks, n)) : -1;
    printf("first_decode_rc=%d\n", rc);

    free(toks);
    if (ctx) llama_free(ctx);
    llama_model_free(model);
    gguf_free(meta);
    llama_backend_free();
    UnmapViewOfFile(base);
    if (hm) CloseHandle(hm);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    return rc == 0 ? 0 : 2;
}
