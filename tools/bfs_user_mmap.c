/* GGUF mmap -> BreathingFS -> Frustum -> llama user callback. */
#define _CRT_SECURE_NO_WARNINGS
#include "llama.h"
#include "ggml.h"
#include "gguf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <windows.h>
#include <time.h>
#include "../core/breathing_fs.h"
#include "../core/gguf_frustum_adapter.h"

typedef struct {
    const uint8_t *base;
    size_t size;
    struct gguf_context *meta;
    GGUFBox *box;
    BreathingFS fs;
    unsigned served, synthetic, failed;
} Hook;

static double now_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level >= GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

static void provide_tensor(struct ggml_tensor *t, void *ud) {
    Hook *h = (Hook *)ud;
    const char *name = ggml_get_name(t);
    int64_t idx = name ? gguf_find_tensor(h->meta, name) : -1;
    if (idx >= 0 && (uint64_t)idx < h->box->n_tensors) {
        const GGUFBoxEntry *entry = &h->box->entries[idx];
        FrustumSeeker seeker = {
            .position = ((uint32_t)idx * 37u) % 20736u,
            .view_id = 0,
            .voronoi_mask = 0xFFFFFFu,
            .frustum_depth = 1
        };
        FrRouteCache cache;
        FrustumRouteEvent route;
        GgufFrustumSpan span;
        fr_cache_init(&cache);
        if (fr_route_produce(&seeker, &cache, &route)) {
            /* Geometry chooses the route; GGUF catalog owns tensor identity. */
            route.tensor_id = (uint16_t)idx;
            route.span_offset = 0;
            route.span_size = entry->size;
            if (gguf_frustum_resolve(h->box, &route, &span) == 0 &&
                span.size == ggml_nbytes(t)) {
                t->data = (void *)span.data;
                h->served++;
                return;
            }
        }
        fprintf(stderr, "route resolve failed: %s idx=%lld\n", name ? name : "<unnamed>", (long long)idx);
        h->failed++;
    }

    t->data = _aligned_malloc(ggml_nbytes(t), 32);
    if (!t->data) return;
    memset(t->data, 0, ggml_nbytes(t));
    h->synthetic++;
}

static int map_file(const char *path, HANDLE *hf, HANDLE *hm,
                    const uint8_t **base, size_t *size) {
    LARGE_INTEGER n;
    *hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (*hf == INVALID_HANDLE_VALUE || !GetFileSizeEx(*hf, &n) || n.QuadPart <= 0) return 0;
    *hm = CreateFileMappingA(*hf, NULL, PAGE_READONLY, 0, 0, NULL);
    *base = *hm ? (const uint8_t *)MapViewOfFile(*hm, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!*base) return 0;
    *size = (size_t)n.QuadPart;
    return 1;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";
    const char *dll = argc > 3 ? argv[3] : "I:/llama/llama-v040-bin-win-vulkan-x64";
    HANDLE hf = INVALID_HANDLE_VALUE, hm = NULL;
    const uint8_t *base = NULL;
    size_t size = 0;
    if (!map_file(path, &hf, &hm, &base, &size)) return 1;

    struct ggml_context *meta_ctx = NULL;
    struct gguf_init_params gip = { .no_alloc = true, .ctx = &meta_ctx };
    struct gguf_context *meta = gguf_init_from_file(path, gip);
    GGUFBox box;
    if (!meta || gguf_box_open(&box, path) != 0) return 1;
    Hook h = { base, size, meta, &box, {0}, 0, 0, 0 };
    bfs_init(&h.fs);

    llama_backend_init();
    llama_log_set(quiet_log, NULL);
    SetDllDirectoryA(dll);
    /* Keep the callback on the CPU backend; Vulkan_Host cannot wrap mmap spans. */
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = argc > 5 ? atoi(argv[5]) : 35;
    mp.no_host = true;
    struct llama_model *model = llama_model_init_from_user(meta, provide_tensor, &h, mp);
    printf("bfs-user-mmap model=%s served=%u synthetic=%u failed=%u tensors=%lld\n",
           model ? "ok" : "FAIL", h.served, h.synthetic, h.failed,
           (long long)gguf_get_n_tensors(meta));
    if (!model) return 2;

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 16; cp.n_batch = 16; cp.n_threads = 8;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    if (n < 0) n = -n;
    llama_token *tokens = (llama_token *)malloc((size_t)n * sizeof(*tokens));
    n = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), tokens, n, true, false);
    double t0 = now_ms();
    int rc = ctx && n > 0 ? llama_decode(ctx, llama_batch_get_one(tokens, n)) : -1;
    double t1 = now_ms();
    int gen = argc > 4 ? atoi(argv[4]) : 16;
    int generated = 0;
    struct llama_sampler *sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    for (int i = 0; rc == 0 && i < gen; i++) {
        llama_token next = llama_sampler_sample(sampler, ctx, -1);
        llama_sampler_accept(sampler, next);
        rc = llama_decode(ctx, llama_batch_get_one(&next, 1));
        generated++;
    }
    double t2 = now_ms();
    llama_sampler_free(sampler);
    printf("bfs-user-mmap first_decode_rc=%d prompt_ms=%.2f gen=%d gen_ms=%.2f tok_s=%.2f\n",
           rc, t1 - t0, generated, t2 - t1,
           generated ? generated * 1000.0 / (t2 - t1) : 0.0);
    free(tokens);
    if (ctx) llama_free(ctx);
    llama_model_free(model);
    gguf_box_close(&box);
    gguf_free(meta);
    llama_backend_free();
    UnmapViewOfFile(base);
    CloseHandle(hm);
    CloseHandle(hf);
    return rc == 0 ? 0 : 3;
}
