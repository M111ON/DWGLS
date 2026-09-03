/* tools/tesspack_stream_view.c — serve .tesspack straight into llama.cpp (no graft)
 * ═══════════════════════════════════════════════════════════════════════════
 * Goal (recorded next step): wire the streaming .tesspack reader into the
 * llama inference loop and measure tok/s vs full GGUF.
 *
 * Phase A (baseline):  llama_model_load_from_file(gguf) — llama reads the
 *                      whole GGUF the normal way.
 * Phase B (streamed):  llama_model_init_from_user(metadata, set_tensor_data)
 *                      — llama allocates the model tensors and calls our
 *                      callback per tensor at load:
 *                        · tensor found in .tesspack (expert or not) →
 *                          scatter-decoded straight from the pack mmap,
 *                          per capo, into llama's own buffer (no graft file)
 *                        · otherwise                           → memcpy slice
 *                          from the source GGUF mmap (header index only)
 *                        · optional tensors the GGUF does not carry (e.g.
 *                          *.scale / *.input_scale, output.bias) → identity
 *                          fill (1.0 for multipliers, 0.0 for bias) to match
 *                          the plain loader's "absent = identity" semantics
 *
 * Pack-only mode (TESS_PACK_ONLY=1): Uses the embedded GGUF header
 *   (__gguf_header__ entry) from the pack for metadata. Non-MoE tensor
 *   fallback reads from the pack data region. Requires DLL that supports
 *   gguf_init_from_buffer (not available in b9733 build).
 *
 * Both phases run the same greedy generation over the same prompt. Output:
 * load ms / decode tok/s per phase, plus BITWISE logits + token equality
 * between A and B — proving the pack-fed model is identical to the GGUF one.
 *
 * BUILD: make tess-stream-view
 * RUN:   ./build/tesspack_stream_view [gguf] [tesspack] [dll_dir] [prompt]
 *        env TESS_NGEN   = generated tokens (default 16)
 *        env TESS_PHASE  = "plain" | "stream" — run one phase only (default: both)
 *        env TESS_TRACE  = log zero-filled tensor names
 *        env TESS_PACK_ONLY = "1" — ignore GGUF arg, use embedded header from pack
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif
#include "llama.h"
#include "ggml-backend.h"

#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

/* ── per-tensor source bookkeeping ── */
typedef struct {
    TESS_PackIndex *pi;       /* .tesspack mmap */
    GgufReader     *src;      /* source GGUF mmap (header + tensor slices) */
    /* stats */
    uint32_t n_pack, n_src, n_zero;
    uint64_t b_pack, b_src;
    double   ms_pack, ms_src; /* wall time spent serving in callback */
    uint32_t errors;
} TensorHook;

/* cell bytes per ggml type — must agree with the pack bake mapping
 * (same table as tesspack_llama_view.c / tesspack_graft.c). */
static uint32_t cell_size_of(enum ggml_type type) {
    static const uint32_t C[16] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    int idx = (int)type;
    if (idx < 0 || idx >= 16) return 0;
    return C[idx];
}

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

/* Decode one full tensor from the pack (all capos, scatter) into dst. */
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
        uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS)
                       ? TESS_TOTAL_SLOTS : (uint32_t)cells_left;
        uint8_t *dst_c = dst + (uint64_t)c * TESS_TOTAL_SLOTS * cell_size;
        uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, dst_c);
        if (got != cells * cell_size) return -3;
        cells_left -= cells;
    }
    return (cells_left == 0) ? (int)capo_count : -4;
}

/* Source GGUF slice for a non-MoE tensor (memcpy out of the mmap). */
static int copy_from_source(TensorHook *h, const char *name, uint8_t *dst,
                            size_t need) {
    GgufReader *g = h->src;
    if (!g) return -1; /* pack-only mode: no source GGUF */
    for (uint32_t i = 0; i < g->n_tensors; i++) {
        if (strcmp(g->names[i], name) != 0) continue;
        uint64_t src_off = g->data_offset + g->offsets[i];
        if (src_off + need > g->base_sz) return -2;
        memcpy(dst, g->base + src_off, need);
        return 0;
    }
    return -1;
}

/* llama.cpp calls this once per model tensor at load time (load_mode NONE). */
static void provide_tensor(struct ggml_tensor *t, void *ud) {
    TensorHook *h = (TensorHook *)ud;
    const char *name = ggml_get_name(t);
    size_t need = (size_t)ggml_nbytes(t);
    uint8_t *dst = (uint8_t *)t->data;

    if (!name || name[0] == '\0' || need == 0) {
        if (dst && need) memset(dst, 0, need);
        h->n_zero++;
        return;
    }

    /* try the pack first — it holds every tensor of this model (capo scatter) */
    uint32_t cell_size = cell_size_of(t->type);
    uint64_t total_cells = (cell_size == 0) ? 0
        : (uint64_t)ggml_nelements(t) / ggml_blck_size(t->type);
    if (cell_size != 0 && total_cells * cell_size == need) {
        double t0 = now_ms();
        int rc = load_pack_tensor(h, name, cell_size, total_cells, dst);
        h->ms_pack += now_ms() - t0;
        if (rc > 0) { h->n_pack++; h->b_pack += need; return; }
        if (rc != -1) /* -1 = name not in pack; others = real decode errors */
            fprintf(stderr, "  [tensor] PACK-LOAD-FAIL %s rc=%d — falling back to GGUF\n",
                    name, rc);
    } else if (cell_size == 0) {
        fprintf(stderr, "  [tensor] TYPE-UNSUPPORTED %s type=%d need=%zu\n",
                name, (int)t->type, need);
        h->errors++;
    }

    double t0 = now_ms();
    int rc = copy_from_source(h, name, dst, need);
    h->ms_src += now_ms() - t0;
    if (rc == 0) { h->n_src++; h->b_src += need; return; }

    if (getenv("TESS_TRACE") && h->n_zero < 80)
        fprintf(stderr, "  [zero] %s need=%zu\n", name, need);
    /* Optional tensors that the GGUF does not carry: llama's plain loader
     * leaves them ABSENT, and absence means identity — scale/input_scale
     * multiply (identity 1.0), bias adds (identity 0.0). Zero-filling the
     * multipliers with 0.0 would corrupt every layer. */
    memset(dst, 0, need);
    const char *nm = name;
    size_t nl = strlen(nm);
    if ((nl >= 6 && strcmp(nm + nl - 6, ".scale") == 0) ||
        (nl >= 13 && strcmp(nm + nl - 13, ".input_scale") == 0)) {
        for (size_t k = 0; k + 4 <= need; k += 4)
            *(float *)(dst + k) = 1.0f;
    }
    h->n_zero++;
}

/* ── one inference run: prompt decode + greedy generation, logits dumped ── */
typedef struct {
    float       *logits;    /* (n_dumps) × n_vocab floats */
    int          n_dumps;
    int          n_vocab;
    llama_token *toks;      /* generated token ids */
    int          n_toks;    /* tokens actually generated */
    double       prompt_ms; /* prompt eval time */
    double       gen_ms;    /* total decode time for generated tokens */
} RunResult;

static void run_result_free(RunResult *r) {
    free(r->logits); r->logits = NULL;
    free(r->toks);   r->toks = NULL;
}

static int run_session(struct llama_model *model, const char *prompt,
                       int n_gen, RunResult *res) {
    memset(res, 0, sizeof(*res));
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    if (!vocab) return -1;
    res->n_vocab = llama_vocab_n_tokens(vocab);
    res->n_dumps = n_gen + 1; /* prompt position + one per generated token */
    res->logits = (float *)malloc((size_t)res->n_dumps * res->n_vocab * sizeof(float));
    res->toks   = (llama_token *)malloc((size_t)n_gen * sizeof(llama_token));
    if (!res->logits || !res->toks) { run_result_free(res); return -1; }

    struct llama_context_params cp = llama_context_default_params();
    cp.n_batch = 128;
    cp.n_ctx   = 256;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { run_result_free(res); return -2; }

    /* tokenize prompt (two-pass, mirroring gcube_token_run) */
    int n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                  NULL, 0, true, false);
    if (n_prompt < 0) n_prompt = -n_prompt;
    if (n_prompt == 0) { llama_free(ctx); run_result_free(res); return -3; }
    llama_token *ptoks = (llama_token *)malloc((size_t)(n_prompt + 8) * sizeof(llama_token));
    int n2 = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                            ptoks, n_prompt + 8, true, false);
    if (n2 < 0 || n2 > n_prompt + 8) { free(ptoks); llama_free(ctx); run_result_free(res); return -3; }
    n_prompt = n2;

    /* prompt eval */
    double t0 = now_ms();
    if (llama_decode(ctx, llama_batch_get_one(ptoks, n_prompt)) != 0) {
        free(ptoks); llama_free(ctx); run_result_free(res); return -4;
    }
    res->prompt_ms = now_ms() - t0;
    memcpy(res->logits, llama_get_logits(ctx), (size_t)res->n_vocab * sizeof(float));
    free(ptoks);

    /* greedy generation */
    struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    double t_gen = now_ms();
    for (int g = 0; g < n_gen; g++) {
        llama_token tok = llama_sampler_sample(smpl, ctx, -1);
        llama_sampler_accept(smpl, tok);
        res->toks[res->n_toks++] = tok;
        if (tok == llama_vocab_eos(vocab)) break;
        if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) break;
        memcpy(res->logits + (size_t)res->n_toks * res->n_vocab,
               llama_get_logits(ctx), (size_t)res->n_vocab * sizeof(float));
    }
    res->gen_ms = now_ms() - t_gen;
    res->n_dumps = res->n_toks + 1; /* final dump after last decode (may be short) */

    llama_sampler_free(smpl);
    llama_free(ctx);
    return 0;
}

/* compare two runs: bitwise logits + identical token stream */
static int compare_runs(const RunResult *a, const RunResult *b) {
    int pass = 1;
    uint64_t diffs = 0;
    float maxdiff = 0.0f;
    int nd = a->n_dumps < b->n_dumps ? a->n_dumps : b->n_dumps;
    for (int d = 0; d < nd; d++) {
        const float *pa = a->logits + (size_t)d * a->n_vocab;
        const float *pb = b->logits + (size_t)d * b->n_vocab;
        for (int i = 0; i < a->n_vocab; i++) {
            if (pa[i] != pb[i]) {
                diffs++;
                float dd = pa[i] > pb[i] ? pa[i] - pb[i] : pb[i] - pa[i];
                if (dd > maxdiff) maxdiff = dd;
            }
        }
    }
    if (a->n_toks != b->n_toks || diffs != 0) pass = 0;
    for (int i = 0; i < a->n_toks && i < b->n_toks && pass; i++)
        if (a->toks[i] != b->toks[i]) pass = 0;
    printf("  logits compared: %d positions × %d vocab — bitwise diffs=%llu maxdiff=%.6e\n",
           nd, a->n_vocab, (unsigned long long)diffs, maxdiff);
    printf("  tokens: A=%d B=%d %s\n", a->n_toks, b->n_toks,
           pass ? "IDENTICAL" : "MISMATCH");
    return pass && diffs == 0;
}

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] : "F:\\model\\qwen3-4b-moe-q4_k_m.gguf";
    const char *pack_path = (argc > 2) ? argv[2] : "F:\\model\\qwen3moe.tesspack";
    const char *dll_dir   = (argc > 3) ? argv[3] : "I:\\llama\\llama-b9733-bin-win-vulkan-x64";
    const char *prompt    = (argc > 4) ? argv[4] : "The capital of France is";
    const char *env_ng    = getenv("TESS_NGEN");
    int n_gen = env_ng ? atoi(env_ng) : 16;
    if (n_gen < 1) n_gen = 1;
    if (n_gen > 256) n_gen = 256;
    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
    SetDllDirectoryA(dll_dir);
#endif
    llama_backend_init();
    /* CPU-only: register just the CPU backend. On this Vulkan build,
     * load_all_from_path also registers Vulkan_Host as an extra CPU buft,
     * which llama then prefers for NONE-mode tensor buffers and which needs
     * GPU host-pinned memory — unavailable here. Plain CPU (malloc) avoids it. */
    static const char *cpu_plugins[] = {
        "ggml-cpu-zen4.dll", "ggml-cpu-alderlake.dll",
        "ggml-cpu-icelake.dll", "ggml-cpu-sapphirerapids.dll",
        "ggml-cpu-haswell.dll", "ggml-cpu-cannonlake.dll",
        "ggml-cpu-cooperlake.dll", "ggml-cpu-skylakex.dll",
        "ggml-cpu-cascadelake.dll", "ggml-cpu-piledriver.dll",
        "ggml-cpu-ivybridge.dll", "ggml-cpu-sandybridge.dll",
        "ggml-cpu-sse42.dll", NULL
    };
    char cpu_dll[1024];
    int cpu_loaded = 0;
    for (int i = 0; cpu_plugins[i]; i++) {
        snprintf(cpu_dll, sizeof(cpu_dll), "%s\\%s", dll_dir, cpu_plugins[i]);
        if (ggml_backend_load(cpu_dll)) { cpu_loaded = 1; break; }
    }
    if (!cpu_loaded) {
        printf("  (note: CPU-only backend load failed; falling back to load-all)\n");
        ggml_backend_load_all_from_path(dll_dir);
    }

    /* ── open .tesspack (always required) ── */
    TESS_PackIndex pi;
    if (tess_pack_open(&pi, pack_path) != 0) {
        printf("FAIL: tesspack open\n"); return 1;
    }
    printf("Tesspack Stream View — llama.cpp fed from .tesspack (no graft)\n");
    printf("Pack:  %s (%u entries, %.1f MB)\n", pack_path, pi.n_entries, (double)pi.file_sz / 1e6);

    if (!gguf_path || gguf_path[0] == '\0') {
        printf("FAIL: GGUF path required for metadata\n");
        tess_pack_close(&pi); return 1;
    }
    printf("GGUF:  %s\n", gguf_path);
    printf("Prompt: \"%s\"  n_gen=%d\n", prompt, n_gen);
    printf("═══════════════════════════════════════════════════════════════\n");

    const char *phase_only = getenv("TESS_PHASE"); /* "plain" | "stream" = run one phase */

    RunResult ra; memset(&ra, 0, sizeof(ra));
    RunResult rb; memset(&rb, 0, sizeof(rb));
    double loadA_ms = 0.0, loadB_ms = 0.0;

    if (!phase_only || strcmp(phase_only, "plain") == 0) {
        /* ── Phase A: baseline — plain GGUF load ── */
        double t0 = now_ms();
        struct llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        struct llama_model *mA = llama_model_load_from_file(gguf_path, mp);
        if (!mA) { printf("FAIL: phase A model load\n"); return 1; }
        loadA_ms = now_ms() - t0;
        printf("Phase A (plain GGUF): load %.1f ms\n", loadA_ms);

        if (run_session(mA, prompt, n_gen, &ra) != 0) {
            printf("FAIL: phase A session\n"); return 1;
        }
        printf("Phase A: prompt eval %.1f ms | gen %d tok in %.1f ms = %.2f tok/s\n",
               ra.prompt_ms, ra.n_toks, ra.gen_ms,
               ra.gen_ms > 0 ? ra.n_toks * 1000.0 / ra.gen_ms : 0.0);
        llama_model_free(mA); /* free before phase B (memory) */
    }

    if (phase_only && strcmp(phase_only, "plain") == 0) {
        printf("\n(TESS_PHASE=plain — phase B skipped)\n");
        run_result_free(&ra);
        tess_pack_close(&pi);
        return 0;
    }

    /* ── Phase B: streamed from .tesspack ── */
    GgufReader src;
    int src_opened = 0;
    if (gguf_open(gguf_path, &src) == 0) {
        src_opened = 1;
    } else {
        printf("FAIL: gguf_reader open\n"); tess_pack_close(&pi); return 1;
    }
    printf("Phase B: pack capos=%u (%.1f MB)\n",
           pi.n_entries, (double)pi.file_sz / 1e6);
    if (src_opened) {
        printf("  source tensors=%u (%.1f MB data)\n",
               src.n_tensors, (double)(src.base_sz - src.data_offset) / 1e6);
    }

    TensorHook hook; memset(&hook, 0, sizeof(hook));
    hook.pi = &pi;
    if (src_opened) hook.src = &src;

    double t0 = now_ms();
    struct gguf_init_params gip = { /*.no_alloc =*/ true, /*.ctx =*/ NULL };
    struct gguf_context *meta = gguf_init_from_file(gguf_path, gip);
    if (!meta) { printf("FAIL: gguf metadata init\n"); return 1; }

    struct llama_model_params mpB = llama_model_default_params();
    mpB.n_gpu_layers = 0;
    /* init_from_user ignores n_gpu_layers for buft choice — pin every tensor
     * to the CPU buffer type explicitly (no Vulkan_Host pinned allocs). */
    static struct llama_model_tensor_buft_override ovr[2];
    ovr[0].pattern = ".*";
    ovr[0].buft = ggml_backend_cpu_buffer_type(); /* true CPU malloc buft —
                dev_by_type(CPU) on this build reports the Vulkan_Host buft */
    ovr[1].pattern = NULL;
    if (ovr[0].buft) mpB.tensor_buft_overrides = ovr;

    struct llama_model *mB = llama_model_init_from_user(meta, provide_tensor,
                                                        &hook, mpB);
    if (!mB) {
        printf("FAIL: phase B model init (callback errors=%u pack=%u src=%u zero=%u)\n",
               hook.errors, hook.n_pack, hook.n_src, hook.n_zero);
        return 1;
    }
    loadB_ms = now_ms() - t0;
    printf("Phase B (streamed from pack): load %.1f ms\n", loadB_ms);
    printf("  callback: pack=%u tensors (%.1f MB, %.1f ms) | "
           "source=%u (%.1f MB, %.1f ms) | zero=%u | errors=%u\n",
           hook.n_pack, hook.b_pack / 1e6, hook.ms_pack,
           hook.n_src, hook.b_src / 1e6, hook.ms_src,
           hook.n_zero, hook.errors);

    if (run_session(mB, prompt, n_gen, &rb) != 0) {
        printf("FAIL: phase B session\n"); return 1;
    }
    printf("Phase B: prompt eval %.1f ms | gen %d tok in %.1f ms = %.2f tok/s\n",
           rb.prompt_ms, rb.n_toks, rb.gen_ms,
           rb.gen_ms > 0 ? rb.n_toks * 1000.0 / rb.gen_ms : 0.0);

    if (phase_only && strcmp(phase_only, "stream") == 0) {
        printf("\n(TESS_PHASE=stream — phase A not measured in this process)\n");
        run_result_free(&rb);
        llama_model_free(mB);
        gguf_free(meta);
        tess_pack_close(&pi);
        if (src_opened) gguf_close(&src);
        return 0;
    }

    /* ── verdict ── */
    int equal = compare_runs(&ra, &rb);
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("LOAD:  plain=%.1f ms   streamed=%.1f ms   (%+.1f ms)\n",
           loadA_ms, loadB_ms, loadB_ms - loadA_ms);
    printf("TOK/S: plain=%.2f       streamed=%.2f      (decode loop)\n",
           ra.gen_ms > 0 ? ra.n_toks * 1000.0 / ra.gen_ms : 0.0,
           rb.gen_ms > 0 ? rb.n_toks * 1000.0 / rb.gen_ms : 0.0);
    printf("RESULT: %s\n", equal ? "PASS — streamed model == plain GGUF (bitwise)"
                                 : "FAIL — models differ");
    printf("Pack-bytes avoided on the llama read path: %.1f MB (served from .tesspack, no graft file)\n",
           hook.b_pack / 1e6);

    run_result_free(&ra);
    run_result_free(&rb);
    llama_model_free(mB);
    gguf_free(meta);
    tess_pack_close(&pi);
    if (src_opened) gguf_close(&src);
    return equal ? 0 : 1;
}
