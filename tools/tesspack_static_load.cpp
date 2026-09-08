// tesspack_static_load.cpp — Static-link loader for tesspack validation
// Links against HEAD llama.cpp static libs (build_novec) with full tensor access.
// Strategy: load model via llama_model_load_from_file (ALL tensors correct),
// compare output with tesspack bridge to prove baseline.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static void *mmap_file(const char *path, size_t *out_size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return NULL; }
    *out_size = (size_t)sz.QuadPart;
    HANDLE fm = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(h);
    if (!fm) return NULL;
    void *p = MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(fm);
    return p;
}
static int rss_mb() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (int)(pmc.WorkingSetSize / (1024*1024));
    return 0;
}
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static void *mmap_file(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    *out_size = st.st_size;
    void *p = mmap(NULL, *out_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return p;
}
static int rss_mb() {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0;
    fscanf(f, "%ld", &pages);
    fclose(f);
    return (int)(pages * 4096 / (1024*1024));
}
#endif

extern "C" {
#include "gguf.h"
#include "ggml.h"
#include "llama.h"
}
#include "llama-model.h"
#include "geo_tess_container.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gguf> <prompt>\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *prompt = argv[2];

    printf("[static] RSS at start: %d MB\n", rss_mb());

    // Load model via llama_model_load_from_file (handles ALL tensors correctly)
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    mp.no_alloc = false;

    printf("[static] Loading model: %s\n", gguf_path); fflush(stdout);
    struct llama_model *model = llama_model_load_from_file(gguf_path, mp);
    if (!model) { fprintf(stderr, "ERROR: model load failed\n"); return 1; }
    printf("[static] Model loaded. RSS: %d MB\n", rss_mb()); fflush(stdout);

    // --- Tesspack swap: overwrite model tensor data with tesspack decoded data ---
    const char *tesspack_path = (argc >= 4) ? argv[3] : NULL;
    if (tesspack_path) {
        printf("[static] Opening tesspack: %s\n", tesspack_path); fflush(stdout);
        TESS_PackIndex pi;
        if (tess_pack_open_mmap(tesspack_path, &pi) == 0) {
            printf("[static] Tesspack: %d capos, %d onion entries\n", pi.n_capos, pi.n_onion_count); fflush(stdout);
            int swapped = 0, skipped = 0, notfound = 0;
            for (auto &p : model->tensors_by_name) {
                const char *name = p.first.c_str();
                struct ggml_tensor *t = p.second;
                if (!t || !t->data) { skipped++; continue; }
                // Try ONION first (f32/f16 tensors)
                const TESS_OnionEntry *onion = tess_pack_find_onion(&pi, name);
                if (onion) {
                    size_t need = (size_t)t->ne[0];
                    for (int d = 1; d < GGML_MAX_DIMS && t->ne[d] > 0; d++) need *= (size_t)t->ne[d];
                    size_t elem_sz = ggml_type_size(t->type);
                    size_t byte_count = need * elem_sz;
                    // ONION stores raw F32, cast to match target type
                    size_t src_sz = onion->size;
                    if (src_sz == byte_count) {
                        memcpy(t->data, pi.data + onion->offset, src_sz);
                        swapped++;
                    } else if (src_sz == need * sizeof(float) && t->type == GGML_TYPE_F16) {
                        // F32→F16: convert in-place
                        const float *src = (const float *)(pi.data + onion->offset);
                        ggml_fp16_t *dst = (ggml_fp16_t *)t->data;
                        for (size_t i = 0; i < need; i++) dst[i] = ggml_fp32_to_fp16(src[i]);
                        swapped++;
                    } else {
                        printf("[static] ONION size mismatch: %s need=%zu got=%zu\n", name, byte_count, src_sz);
                        skipped++;
                    }
                    continue;
                }
                // Try SCATTER capo
                int cid = tess_pack_get_capo(&pi, name);
                if (cid >= 0) {
                    uint8_t *dst = (uint8_t *)t->data;
                    size_t total = ggml_nbytes(t);
                    int rc = tess_capo_load_range(&pi, cid, dst, total);
                    if (rc == 0) { swapped++; }
                    else { printf("[static] SCATTER fail: %s rc=%d\n", name, rc); skipped++; }
                    continue;
                }
                // Try RESIDUAL
                int rid = tess_pack_find_residual(&pi, name);
                if (rid >= 0) {
                    const TESS_ResidualEntry *re = &pi.residuals[rid];
                    // Apply residual transform
                    if (re->transform == TESS_TRANSFORM_TYPE_CAST) {
                        int src_cid = tess_pack_get_capo(&pi, re->src_tensor);
                        if (src_cid >= 0) {
                            size_t total = ggml_nbytes(t);
                            int rc = tess_capo_load_range(&pi, src_cid, (uint8_t *)t->data, total);
                            if (rc == 0) { swapped++; } else { skipped++; }
                        } else { skipped++; }
                    } else {
                        skipped++;
                    }
                    continue;
                }
                notfound++;
            }
            printf("[static] Tesspack swap: %d swapped, %d skipped, %d not found\n", swapped, skipped, notfound);
            tess_pack_close(&pi);
        } else {
            fprintf(stderr, "ERROR: tesspack open failed\n");
        }
    }

    // Get vocab
    printf("[static] Getting vocab..."); fflush(stdout);
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    if (!vocab) { fprintf(stderr, "ERROR: no vocab\n"); llama_model_free(model); return 1; }
    printf(" OK\n"); fflush(stdout);

    // Create context
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 256;
    cp.n_batch = 128;
    cp.n_threads = 4;
    printf("[static] Creating context..."); fflush(stdout);
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "ERROR: context init failed\n"); llama_model_free(model); return 1; }
    printf(" OK RSS: %d MB\n", rss_mb()); fflush(stdout);

    // Tokenize
    llama_token tokens[256];
    printf("[static] Tokenizing..."); fflush(stdout);
    int n_tokens = llama_tokenize(vocab, prompt, strlen(prompt), tokens, 256, true, false);
    if (n_tokens < 0) { fprintf(stderr, "ERROR: tokenize failed (%d)\n", n_tokens); llama_free(ctx); llama_model_free(model); return 1; }
    printf(" OK (%d tokens)\n", n_tokens); fflush(stdout);

    // Evaluate prompt
    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    if (llama_decode(ctx, batch)) {
        fprintf(stderr, "ERROR: decode failed\n"); llama_free(ctx); llama_model_free(model); return 1;
    }

    // Generate
    const int n_gen = 16;
    int n_vocab = llama_vocab_n_tokens(vocab);
    printf("[static] Generating %d tokens (vocab=%d):\n", n_gen, n_vocab);
    printf("[static] ");

    // Create greedy sampler
    struct llama_sampler *greedy = llama_sampler_init_greedy();
    if (!greedy) { fprintf(stderr, "ERROR: greedy sampler init failed\n"); llama_free(ctx); llama_model_free(model); return 1; }

    for (int i = 0; i < n_gen; i++) {
        float *logits = llama_get_logits(ctx);
        llama_token tok = llama_sampler_sample(greedy, ctx, -1);
        char buf[256];
        int n = llama_token_to_piece(vocab, tok, buf, sizeof(buf) - 1, 0, true);
        if (n > 0) { buf[n] = '\0'; printf("%s", buf); }
        llama_token arr[1] = {tok};
        struct llama_batch next = llama_batch_get_one(arr, 1);
        if (llama_decode(ctx, next)) { printf("\n[static] decode error at %d\n", i); break; }
    }
    printf("\n[static] Done. RSS: %d MB\n", rss_mb());

    llama_sampler_free(greedy);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}
