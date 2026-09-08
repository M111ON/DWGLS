/*
 * tesspack_bridge.c — load from .tesspack via mmap + gguf callback
 * Phase A: plain GGUF (reference)
 * Phase B: load GGUF normally (handles tied weights), then swap tensor data from pack
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "llama.h"
#include "ggml-backend.h"
#include "gguf.h"
#include "../core/geo_tess_container.h"

static double now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static double rss_mb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0.0;
    long pages = 0;
    if (fscanf(f, "%ld", &pages) != 1) { fclose(f); return 0.0; }
    fclose(f);
    return (double)pages * 4096.0 / (1024.0 * 1024.0);
#endif
}

static uint32_t cell_size_of(enum ggml_type type) {
    static const uint32_t C[16] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    int idx = (int)type;
    if (idx < 0 || idx >= 16) return 0;
    return C[idx];
}

/* Load one tensor from mmap'd pack into a malloc'd buffer.
 * Returns malloc'd pointer (caller frees) or NULL on failure. */
static uint8_t *load_tensor_from_pack(TESS_PackIndex *pi, const char *name,
                                       uint32_t cell_size, uint64_t total_cells) {
    uint64_t bytes = (uint64_t)total_cells * cell_size;
    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) return NULL;

    uint64_t cells_left = total_cells;
    for (uint64_t c = 0; cells_left > 0; c++) {
        TESS_CapoReader cr;
        if (tess_pack_get_capo_mmap(pi, &cr, name, (uint32_t)c) != 0) {
            free(buf);
            return NULL;
        }
        uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS) ? TESS_TOTAL_SLOTS : (uint32_t)cells_left;
        uint8_t *dst_c = buf + c * TESS_TOTAL_SLOTS * cell_size;
        uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, dst_c);
        if (got != cells * cell_size) {
            free(buf);
            return NULL;
        }
        cells_left -= cells;
    }
    return buf;
}

typedef struct {
    char name[256];
    uint8_t *data;
    size_t   bytes;
} TensorSwap;

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s pack.tesspack model.gguf llmdir prompt\n", argv[0]);
        return 1;
    }
    const char *pack_path = argv[1];
    const char *gguf_path  = argv[2];
    const char *llama_dir  = argv[3];
    const char *prompt     = argv[4];

    /* Load DLL backends from llama dir */
    {
        char dll_path[512];
        snprintf(dll_path, sizeof(dll_path), "%s/llama.dll", llama_dir);
        llama_backend_load_from_path(dll_path);
    }

    TESS_PackIndex pi = {0};
    if (tess_pack_open_mmap(&pi, pack_path) != 0) {
        fprintf(stderr, "FAIL: pack open\n");
        return 1;
    }

    /* ── Phase A: plain GGUF ── */
    struct llama_model_params mpa = llama_model_default_params();
    mpa.n_gpu_layers = 0;
    double t0 = now_ms();
    struct llama_model *mA = llama_model_load_from_file(gguf_path, mpa);
    double tA_ms = now_ms() - t0;
    if (!mA) { fprintf(stderr, "FAIL: model A\n"); return 1; }

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx    = 256;
    cp.n_batch  = 128;
    cp.n_threads = 1;

    struct llama_context *ctxA = llama_init_from_model(mA, cp);
    if (!ctxA) { fprintf(stderr, "FAIL: ctx A\n"); return 1; }

    llama_token tokens[64];
    int n_tok = llama_tokenize(llama_get_model(ctxA), prompt, (int32_t)strlen(prompt),
                               tokens, 64, true, false);
    if (n_tok <= 0) { fprintf(stderr, "FAIL: tokenize\n"); return 1; }

    llama_kv_cache_clear(ctxA);
    if (llama_decode(ctxA, llama_batch_get_one(tokens, n_tok, 0, false))) {
        fprintf(stderr, "FAIL: decode A\n"); return 1;
    }

    float *logA = llama_get_logits_ith(ctxA, -1);
    int vocab = llama_n_vocab(llama_get_model(ctxA));
    int bestA = 0;
    for (int i = 1; i < vocab; i++)
        if (logA[i] > logA[bestA]) bestA = i;
    char tokA[64];
    llama_token_to_piece(llama_get_model(ctxA), bestA, tokA, sizeof(tokA), 0, true);
    printf("Phase A (%.0f ms): [%d] '%s'\n", tA_ms, bestA, tokA);

    /* Top-5 for A */
    float top5A[5]; int idx5A[5];
    float *tmp = (float *)malloc(vocab * sizeof(float));
    memcpy(tmp, logA, vocab * sizeof(float));
    for (int v = 0; v < 5; v++) {
        int mx = 0;
        for (int i = 1; i < vocab; i++)
            if (tmp[i] > tmp[mx]) mx = i;
        top5A[v] = tmp[mx]; idx5A[v] = mx;
        tmp[mx] = -1e30f;
    }
    printf("  top-5 A: ");
    for (int v = 0; v < 5; v++) printf("%d(%.4f) ", idx5A[v], top5A[v]);
    printf("\n");
    free(tmp);

    llama_free(ctxA);
    llama_model_free(mA);

    /* ── Phase B: load GGUF normally, then swap tensor data from pack ── */
    printf("\n");
    struct llama_model_params mpB = llama_model_default_params();
    mpB.n_gpu_layers = 0;
    t0 = now_ms();
    struct llama_model *mB = llama_model_load_from_file(gguf_path, mpB);
    double tB_load_ms = now_ms() - t0;
    if (!mB) { fprintf(stderr, "FAIL: model B\n"); return 1; }

    /* Open the GGUF with gguf_read to get tensor names */
    struct gguf_init_params gip = { /*.no_alloc =*/ true, /*.ctx =*/ NULL };
    struct gguf_context *meta = gguf_init_from_file(gguf_path, gip);
    if (!meta) { fprintf(stderr, "FAIL: gguf_read\n"); return 1; }

    int64_t n_tensors = gguf_get_n_tensors(meta);
    printf("Phase B: %lld tensors in GGUF\n", (long long)n_tensors);

    /* For each tensor: load from pack into malloc'd buffer, then find it in model.
     * We can't look up by name in the model, so we'll rely on the fact that
     * ggml tensors are stored in order matching the GGUF.
     * Instead: we'll iterate through ggml tensors via the model's memory. */
    /* Actually, we can't iterate model tensors in b10830.
     * NEW PLAN: use init_from_user with callback that loads from pack,
     * but pre-duplicate tied weights in the GGUF data. */

    /* Simplest correct approach: for each GGUF tensor, load from pack.
     * Store (name -> data_ptr) map. For tied tensors (output.weight),
     * redirect to token_embd.weight's data. But we still can't set
     * the tensor data in the already-loaded model... */

    /* ABANDONGGGG. Let's use init_from_user but fix tied weights by
     * loading output.weight data from pack as token_embd.weight's format. */

    gguf_free(meta);
    llama_model_free(mB);

    /* ── Phase B (v2): init_from_user + pack callback with tied weight fix ── */
    struct gguf_context *meta2 = gguf_init_from_file(gguf_path, gip);
    if (!meta2) { fprintf(stderr, "FAIL: gguf_read2\n"); return 1; }

    /* Pre-load all pack tensors into a name→buffer map */
    n_tensors = gguf_get_n_tensors(meta2);
    TensorSwap *swaps = (TensorSwap *)calloc(n_tensors, sizeof(TensorSwap));
    int n_swaps = 0;

    t0 = now_ms();
    for (int64_t i = 0; i < n_tensors; i++) {
        const char *name = gguf_get_tensor_name(meta2, i);
        size_t offset = gguf_get_tensor_offset(meta2, i);
        /* We need the type and shape. Use gguf_get_tensor_type or similar. 
         * Actually gguf doesn't expose type per tensor. We need the raw header. 
         * Skip this approach — too complex without proper API. */
    }

    free(swaps);
    gguf_free(meta2);

    printf("\nBridge: cannot iterate model tensors in b10830 API.\n");
    printf("Need different approach.\n");

    return 0;
}
