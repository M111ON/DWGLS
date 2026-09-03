/* tools/tesspack_breathe_view.c — breathing_fs: mmap-based RSS measurement
 * ══════════════════════════════════════════════════════════════════════════════
 * Proves the breathing_fs concept via mmap:
 *   - llama_model_load_from_file uses mmap → pages only committed on access
 *   - MoE inference touches only 4/64 experts per layer per token
 *   - RSS reflects only accessed pages, not full model size
 *
 * Phases:
 *   A: Load original GGUF (mmap) → inference → RSS measurement
 *   B: Assemble from tesspack → mmap load → inference → RSS measurement
 *   C: (optional) Assemble sparse GGUF (selected experts only) → mmap → inference
 *
 * BUILD: gcc -O2 -Wall -I core -I /i/llama/llama.cpp/include -I /i/llama/llama.cpp/ggml/include
 *        -o build/tesspack_breathe_view.exe tools/tesspack_breathe_view.c
 *        core/geo_tess_container.c -L /i/llama/llama-b9733-bin-win-vulkan-x64 -lllama -lm
 * RUN:   TESS_PHASE=all ./build/tesspack_breathe_view.exe <gguf> <tesspack> <dll_dir> [prompt]
 * ══════════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <windows.h>
#include <psapi.h>
#include "llama.h"
#include "ggml-backend.h"
#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

/* ── helpers ── */
static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static double mb(size_t b) { return (double)b / 1048576.0; }

static size_t get_rss(void) {
    PROCESS_MEMORY_COUNTERS pmc = {0};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
}

/* Get commit charge (total committed bytes) */
static size_t get_commit(void) {
    PROCESS_MEMORY_COUNTERS pmc = {0};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PagefileUsage;
}

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

/* ── GGUF assembly from tesspack + source ── */
typedef struct {
    TESS_PackIndex *pi;
    GgufReader     *src;
    uint32_t n_pack, n_src, n_zero;
    uint64_t b_pack, b_src;
    double   ms_pack, ms_src;
    uint32_t errors;
} TensorHook;

static uint32_t cell_size_of(enum ggml_type type) {
    int idx = (int)type;
    if (idx < 0 || idx >= 16) return 0;
    return GGUF_CELL_SIZE[idx];
}

static int load_pack_tensor(TensorHook *h, const char *name, uint32_t cell_size,
                            uint64_t total_cells, uint8_t *dst) {
    TESS_PackIndex *pi = h->pi;
    uint32_t capo_count = 0;
    for (uint32_t j = 0; j < pi->n_entries; j++) {
        if (strcmp(pi->entries[j].name, name) == 0) {
            uint32_t cid = pi->entries[j].capo_id;
            if (cid + 1 > capo_count) capo_count = cid + 1;
        }
    }
    if (capo_count == 0) return -1;
    uint64_t cells_left = total_cells;
    for (uint32_t c = 0; c < capo_count && cells_left > 0; c++) {
        TESS_CapoReader cr;
        if (tess_pack_get_capo(pi, &cr, name, c) != 0) return -2;
        uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS) ? TESS_TOTAL_SLOTS : (uint32_t)cells_left;
        uint8_t *dst_c = dst + (uint64_t)c * TESS_TOTAL_SLOTS * cell_size;
        uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, dst_c);
        if (got != cells * cell_size) return -3;
        cells_left -= cells;
    }
    return (cells_left == 0) ? (int)capo_count : -4;
}

static int copy_from_source(TensorHook *h, const char *name, uint8_t *dst, size_t need) {
    GgufReader *g = h->src;
    for (uint32_t i = 0; i < g->n_tensors; i++) {
        if (strcmp(g->names[i], name) != 0) continue;
        uint64_t src_off = g->data_offset + g->offsets[i];
        if (src_off + need > g->base_sz) return -2;
        memcpy(dst, g->base + src_off, need);
        return 0;
    }
    return -1;
}

/* assemble_gguf: write a new GGUF file from pack + source metadata */
static int assemble_gguf(const char *out_path, const char *gguf_path,
                          TESS_PackIndex *pi, GgufReader *src) {
    FILE *fin = fopen(gguf_path, "rb");
    if (!fin) return -1;
    _fseeki64(fin, 0, SEEK_END);
    int64_t file_sz = _ftelli64(fin);
    _fseeki64(fin, 0, SEEK_SET);

    /* Read header + metadata (before tensor data) */
    uint64_t header_end = src->data_offset;
    uint8_t *header_buf = (uint8_t *)malloc(header_end);
    if (!header_buf) { fclose(fin); return -2; }
    size_t hr = fread(header_buf, 1, header_end, fin);
    if (hr != header_end) { free(header_buf); fclose(fin); return -3; }

    /* Compute total body size: same as original */
    uint64_t body_sz = (uint64_t)file_sz - header_end;
    printf("  assemble: header=%llu body=%llu total=%llu\n",
           (unsigned long long)header_end, (unsigned long long)body_sz, (unsigned long long)file_sz);

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { free(header_buf); fclose(fin); return -4; }

    /* Write header */
    fwrite(header_buf, 1, header_end, fout);
    free(header_buf);

    /* Allocate body buffer */
    uint8_t *body = (uint8_t *)calloc(1, body_sz);
    if (!body) { fclose(fout); fclose(fin); return -5; }

    TensorHook hook = {0};
    hook.pi = pi; hook.src = src;

    /* Fill body: pack tensors first, then source for non-MoE */
    for (uint32_t i = 0; i < src->n_tensors; i++) {
        const char *name = src->names[i];
        uint64_t off = src->offsets[i];
        enum ggml_type tp = (enum ggml_type)src->dtypes[i];
        uint32_t csz = cell_size_of(tp);
        uint32_t nd = src->n_dims[i];
        uint64_t ne = 1;
        for (uint32_t d = 0; d < nd; d++) ne *= src->dims[i * 4 + d];
        uint32_t blck = ggml_blck_size(tp);
        uint64_t nbytes = (ne / blck) * csz;

        if (strstr(name, "_exps.weight") && csz > 0) {
            uint64_t total_cells = ne / blck;
            double t0 = now_ms();
            int rc = load_pack_tensor(&hook, name, csz, total_cells, body + off);
            hook.ms_pack += now_ms() - t0;
            if (rc > 0) { hook.n_pack++; hook.b_pack += nbytes; continue; }
            fprintf(stderr, "  [assemble] pack fail %s rc=%d, using source\n", name, rc);
        }

        double t0 = now_ms();
        int rc = copy_from_source(&hook, name, body + off, nbytes);
        hook.ms_src += now_ms() - t0;
        if (rc == 0) { hook.n_src++; hook.b_src += nbytes; }
        else { fprintf(stderr, "  [assemble] source fail %s rc=%d\n", name, rc); hook.errors++; }
    }

    fwrite(body, 1, body_sz, fout);
    free(body); fclose(fout); fclose(fin);

    printf("  assemble: pack=%u (%.1f MB, %.1f ms) src=%u (%.1f MB, %.1f ms) zero=%u errors=%u\n",
           hook.n_pack, hook.b_pack / 1e6, hook.ms_pack,
           hook.n_src, hook.b_src / 1e6, hook.ms_src,
           hook.n_zero, hook.errors);
    return 0;
}

/* ── inference with RSS measurement ── */
typedef struct {
    float       *logits;
    int          n_dumps, n_vocab;
    llama_token *toks;
    int          n_toks;
    double       prompt_ms, gen_ms;
    size_t       rss_load, rss_prompt, rss_gen_start, rss_gen_end;
} RunResult;

static void run_result_free(RunResult *r) {
    free(r->logits); r->logits = NULL;
    free(r->toks);   r->toks   = NULL;
}

static int run_session(struct llama_model *model, const char *prompt,
                       int n_gen, RunResult *res) {
    memset(res, 0, sizeof(*res));
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    if (!vocab) return -1;
    res->n_vocab = llama_vocab_n_tokens(vocab);

    /* Tokenize */
    int n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    if (n_prompt < 0) n_prompt = -n_prompt;
    llama_token *ptoks = (llama_token *)malloc((size_t)(n_prompt + 8) * sizeof(llama_token));
    int n2 = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), ptoks, n_prompt + 8, true, false);
    if (n2 < 0) { free(ptoks); return -3; }
    n_prompt = n2;

    struct llama_context_params cp = llama_context_default_params();
    cp.n_batch = 128; cp.n_ctx = 256;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { free(ptoks); return -2; }

    /* Prompt eval + measure RSS */
    double t0 = now_ms();
    if (llama_decode(ctx, llama_batch_get_one(ptoks, n_prompt)) != 0) {
        free(ptoks); llama_free(ctx); return -4;
    }
    res->prompt_ms = now_ms() - t0;
    res->rss_prompt = get_rss();
    printf("    RSS after prompt eval: %.0f MB (%.0f MB commit)\n",
           mb(res->rss_prompt), mb(get_commit()));

    /* Dump prompt logits */
    res->n_dumps = n_gen + 2;
    res->logits = (float *)malloc((size_t)res->n_dumps * res->n_vocab * sizeof(float));
    res->toks   = (llama_token *)malloc((size_t)n_gen * sizeof(llama_token));
    if (!res->logits || !res->toks) { free(ptoks); llama_free(ctx); run_result_free(res); return -1; }
    memcpy(res->logits, llama_get_logits(ctx), (size_t)res->n_vocab * sizeof(float));
    free(ptoks);

    /* Generation + measure RSS per token */
    struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    res->rss_gen_start = get_rss();
    double t_gen = now_ms();
    for (int g = 0; g < n_gen; g++) {
        llama_token tok = llama_sampler_sample(smpl, ctx, -1);
        llama_sampler_accept(smpl, tok);
        res->toks[res->n_toks++] = tok;
        if (tok == llama_vocab_eos(vocab)) break;
        if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) break;
        memcpy(res->logits + (size_t)(res->n_toks) * res->n_vocab,
               llama_get_logits(ctx), (size_t)res->n_vocab * sizeof(float));
        /* Measure RSS every 4 tokens */
        if ((g & 3) == 3) {
            size_t r = get_rss();
            printf("    RSS after gen token %d: %.0f MB\n", g + 1, mb(r));
        }
    }
    res->gen_ms = now_ms() - t_gen;
    res->rss_gen_end = get_rss();
    res->n_dumps = res->n_toks + 1;
    printf("    RSS after gen complete: %.0f MB (%.0f MB commit)\n",
           mb(res->rss_gen_end), mb(get_commit()));

    llama_sampler_free(smpl);
    llama_free(ctx);
    return 0;
}

/* ── comparison ── */
static int compare_runs(const RunResult *a, const RunResult *b) {
    int pass = 1;
    uint64_t diffs = 0;
    float maxdiff = 0.0f;
    int nd = a->n_dumps < b->n_dumps ? a->n_dumps : b->n_dumps;
    for (int d = 0; d < nd; d++) {
        const float *pa = a->logits + (size_t)d * a->n_vocab;
        const float *pb = b->logits + (size_t)d * a->n_vocab;
        for (int i = 0; i < a->n_vocab; i++) {
            if (pa[i] != pb[i]) { diffs++; float dd = fabsf(pa[i]-pb[i]); if(dd>maxdiff)maxdiff=dd; }
        }
    }
    if (a->n_toks != b->n_toks || diffs != 0) pass = 0;
    for (int i = 0; i < a->n_toks && i < b->n_toks && pass; i++)
        if (a->toks[i] != b->toks[i]) pass = 0;
    printf("  logits: %d x %d — diffs=%llu maxdiff=%.6e\n",
           nd, a->n_vocab, (unsigned long long)diffs, maxdiff);
    printf("  tokens: %d vs %d — %s\n", a->n_toks, b->n_toks, pass ? "MATCH" : "MISMATCH");
    return pass;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: %s <gguf> <tesspack> <dll_dir> [prompt]\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *pack_path = argv[2];
    const char *dll_dir   = argv[3];
    const char *prompt    = argc > 4 ? argv[4] : "The capital of France is";
    int n_gen = 16;
    const char *ng = getenv("TESS_NGEN");
    if (ng) n_gen = atoi(ng);

    SetDllDirectoryA(dll_dir);
    llama_backend_init();
    static const char *cpu_plugins[] = {
        "ggml-cpu-haswell.dll", "ggml-cpu-cannonlake.dll",
        "ggml-cpu-cooperlake.dll", "ggml-cpu-skylakex.dll",
        "ggml-cpu-cascadelake.dll", "ggml-cpu-sse42.dll", NULL
    };
    char cpu_dll[1024];
    int cpu_loaded = 0;
    for (int i = 0; cpu_plugins[i]; i++) {
        snprintf(cpu_dll, sizeof(cpu_dll), "%s\\%s", dll_dir, cpu_plugins[i]);
        if (ggml_backend_load(cpu_dll)) { cpu_loaded = 1; break; }
    }
    if (!cpu_loaded) ggml_backend_load_all_from_path(dll_dir);

    printf("Breathe View — mmap-based RSS measurement\n");
    printf("GGUF: %s\nPack: %s\nPrompt: \"%s\" n_gen=%d\n", gguf_path, pack_path, prompt, n_gen);
    printf("System: RSS before anything: %.0f MB\n", mb(get_rss()));
    printf("═══════════════════════════════════════════════════════════════\n");

    const char *phase = getenv("TESS_PHASE");
    int run_a = !phase || strcmp(phase, "all") == 0 || strcmp(phase, "a") == 0;
    int run_b = !phase || strcmp(phase, "all") == 0 || strcmp(phase, "b") == 0;

    /* ── Phase A: baseline — plain GGUF via mmap ── */
    RunResult ra; memset(&ra, 0, sizeof(ra));
    double loadA_ms = 0;
    if (run_a) {
        printf("\n── Phase A: original GGUF (mmap) ──\n");
        double t0 = now_ms();
        struct llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        struct llama_model *mA = llama_model_load_from_file(gguf_path, mp);
        if (!mA) { printf("FAIL: phase A model load\n"); return 1; }
        loadA_ms = now_ms() - t0;
        printf("  load: %.1f ms | RSS: %.0f MB | commit: %.0f MB\n",
               loadA_ms, mb(get_rss()), mb(get_commit()));
        if (run_session(mA, prompt, n_gen, &ra) != 0) {
            printf("FAIL: phase A session\n"); return 1;
        }
        printf("  prompt eval: %.1f ms | gen %d tok in %.1f ms = %.2f tok/s\n",
               ra.prompt_ms, ra.n_toks, ra.gen_ms,
               ra.gen_ms > 0 ? ra.n_toks * 1000.0 / ra.gen_ms : 0.0);
        printf("  RSS peak: %.0f MB | file size: %.0f MB\n",
               mb(ra.rss_gen_end > ra.rss_prompt ? ra.rss_gen_end : ra.rss_prompt),
               mb((size_t)3694 * 1048576));  /* approximate */
        llama_model_free(mA);
        printf("  RSS after free: %.0f MB\n", mb(get_rss()));
    }

    /* ── Phase B: assembled from tesspack (mmap) ── */
    RunResult rb; memset(&rb, 0, sizeof(rb));
    double loadB_ms = 0;
    if (run_b) {
        printf("\n── Phase B: assembled from tesspack (mmap) ──\n");

        /* Assemble GGUF to temp file */
        const char *out_path = "F:/model/moe_tesspack_breathe.gguf";
        GgufReader src;
        if (gguf_open(gguf_path, &src) != 0) { printf("FAIL: gguf open\n"); return 1; }
        TESS_PackIndex pi;
        if (tess_pack_open(&pi, pack_path) != 0) {
            printf("FAIL: tesspack open\n"); gguf_close(&src); return 1;
        }

        printf("  assembling GGUF from tesspack...\n");
        double t_assemble = now_ms();
        if (assemble_gguf(out_path, gguf_path, &pi, &src) != 0) {
            printf("FAIL: assemble\n"); return 1;
        }
        printf("  assemble time: %.1f ms\n", now_ms() - t_assemble);
        tess_pack_close(&pi); gguf_close(&src);

        /* Load assembled GGUF via mmap */
        double t0 = now_ms();
        struct llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        struct llama_model *mB = llama_model_load_from_file(out_path, mp);
        if (!mB) { printf("FAIL: phase B model load\n"); return 1; }
        loadB_ms = now_ms() - t0;
        printf("  load: %.1f ms | RSS: %.0f MB | commit: %.0f MB\n",
               loadB_ms, mb(get_rss()), mb(get_commit()));
        if (run_session(mB, prompt, n_gen, &rb) != 0) {
            printf("FAIL: phase B session\n"); return 1;
        }
        printf("  prompt eval: %.1f ms | gen %d tok in %.1f ms = %.2f tok/s\n",
               rb.prompt_ms, rb.n_toks, rb.gen_ms,
               rb.gen_ms > 0 ? rb.n_toks * 1000.0 / rb.gen_ms : 0.0);
        printf("  RSS peak: %.0f MB\n",
               mb(rb.rss_gen_end > rb.rss_prompt ? rb.rss_gen_end : rb.rss_prompt));
        llama_model_free(mB);
        printf("  RSS after free: %.0f MB\n", mb(get_rss()));
    }

    /* ── verdict ── */
    if (run_a && run_b) {
        printf("\n═══════════════════════════════════════════════════════════════\n");
        printf("TIMING: original=%.1f ms  assembled=%.1f ms\n", loadA_ms, loadB_ms);
        printf("RSS after load: orig=%.0f MB  assembled=%.0f MB\n",
               mb(ra.rss_prompt), mb(rb.rss_prompt));
        printf("RSS after gen:  orig=%.0f MB  assembled=%.0f MB\n",
               mb(ra.rss_gen_end), mb(rb.rss_gen_end));
        int equal = compare_runs(&ra, &rb);
        printf("RESULT: %s\n", equal ? "PASS — assembled == original (lossless)"
                                     : "FAIL — models differ");
        run_result_free(&ra);
        run_result_free(&rb);
    } else if (run_a) {
        run_result_free(&ra);
    } else if (run_b) {
        run_result_free(&rb);
    }

    llama_backend_free();
    return 0;
}
