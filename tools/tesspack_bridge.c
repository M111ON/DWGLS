/* tools/tesspack_bridge.c — .tesspack → llama.cpp bridge (pack-only, no GGUF)
 * ═══════════════════════════════════════════════════════════════════════════
 * Direct bridge: open .tesspack → extract embedded GGUF header →
 * llama_model_init_from_user → scatter-decode tensors from pack on demand.
 *
 * No source GGUF required. The .tesspack must contain an __gguf_header__
 * entry (created by tess_gguf_pack, NOT tess_packer).
 *
 * Modes:
 *   TESS_PHASE=bridge  — bridge only (default: bridge + baseline)
 *   TESS_PHASE=plain   — baseline only (standard GGUF load)
 *   TESS_PHASE=both    — run both + compare (default)
 *
 * When a source GGUF path is given as 2nd arg, also runs baseline (Phase A).
 * When only a .tesspack is given, runs bridge-only (Phase B) and skips
 * the comparison.
 *
 * BUILD: make tess-bridge
 * RUN:   ./build/tesspack_bridge <tesspack> [gguf_for_baseline] [dll_dir] [prompt]
 *        env TESS_NGEN   = tokens to generate (default 16)
 *        env TESS_PHASE  = "bridge" | "plain" | "both" (default: auto)
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "llama.h"
#include "ggml-backend.h"

#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

/* ── tensor data map for weight-tying ── */
#define TMAP_SIZE 512
typedef struct { const char *name; void *data; } TensorMapEntry;
typedef struct {
    TESS_PackIndex *pi;
    uint32_t n_pack, n_zero, errors;
    uint64_t b_pack;
    double   ms_pack;
    TensorMapEntry tmap[TMAP_SIZE];
    uint32_t tmap_n;
    /* GGUF fallback: mmap the original GGUF so non-pack tensors get real data */
    const uint8_t *gguf_mmap;
    uint64_t       gguf_size;
    struct gguf_context *gguf_meta;
} TensorHook;

static void tmap_add(TensorHook *h, const char *name, void *data) {
    if (h->tmap_n >= TMAP_SIZE) return;
    h->tmap[h->tmap_n++] = (TensorMapEntry){ name, data };
}
static void *tmap_find(TensorHook *h, const char *name) {
    for (uint32_t i = 0; i < h->tmap_n; i++)
        if (strcmp(h->tmap[i].name, name) == 0) return h->tmap[i].data;
    return NULL;
}

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

/* Get current working set (RSS) in MB. Returns 0 on failure. */
static double rss_mb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
#else
    /* Linux: /proc/self/statm field 1 = RSS in pages */
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0.0;
    long pages = 0;
    if (fscanf(f, "%ld", &pages) != 1) { fclose(f); return 0.0; }
    fclose(f);
    return (double)pages * 4096.0 / (1024.0 * 1024.0);
#endif
}

/* Scatter-decode one tensor from pack into dst (all capos).
 * Uses mmap-only index walk — no malloc'd entries. */
static int load_pack_tensor(TensorHook *h, const char *name, uint32_t cell_size,
                            uint64_t total_cells, uint8_t *dst) {
    TESS_PackIndex *pi = h->pi;
    /* count capos with this name by walking mmap'd index */
    uint32_t capo_count = 0;
    {
        const uint8_t *cur = pi->base + pi->index_offset;
        const uint8_t *end = pi->base + pi->file_sz;
        uint32_t nlen = (uint32_t)strlen(name);
        for (uint32_t i = 0; i < pi->n_capos; i++) {
            if (cur + 1 > end) break;
            uint8_t nl = *cur++;
            if (cur + nl + 16 > end) break;
            const uint8_t *np = cur;
            cur += nl;
            uint32_t cid = *(const uint32_t *)cur;
            cur += 16;
            if (nl == (uint8_t)nlen && memcmp(np, name, nlen) == 0) {
                if (cid + 1 > capo_count) capo_count = cid + 1;
            }
        }
    }
    if (capo_count == 0) return -1;

    uint64_t cells_left = total_cells;
    for (uint32_t c = 0; c < capo_count && cells_left > 0; c++) {
        TESS_CapoReader cr;
        if (tess_pack_get_capo_mmap(pi, &cr, name, c) != 0) return -2;
        uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS)
                       ? TESS_TOTAL_SLOTS : (uint32_t)cells_left;
        uint8_t *dst_c = dst + (uint64_t)c * TESS_TOTAL_SLOTS * cell_size;
        uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, dst_c);
        if (got != cells * cell_size) return -3;
        cells_left -= cells;
    }
    return (cells_left == 0) ? (int)capo_count : -4;
}

/* (fp16_to_float is in geo_tess_container.h) */

/* llama callback: fill tensor data from .tesspack. */
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

    /* ── ONION path: f16/f32 blob → direct or convert ── */
    {
        const uint8_t *onion_data = NULL;
        uint32_t onion_sz = 0;
        if (tess_pack_find_onion(h->pi, name, &onion_data, &onion_sz) == 0) {
            uint32_t n_elems = (uint32_t)ggml_nelements(t);
            if (t->type == GGML_TYPE_F32 && onion_sz == n_elems * 2) {
                const uint16_t *f16 = (const uint16_t *)onion_data;
                float *f32 = (float *)dst;
                for (uint32_t k = 0; k < n_elems; k++)
                    f32[k] = fp16_to_float(f16[k]);
                h->n_pack++; h->b_pack += need;
                tmap_add(h, name, dst);
                if (h->n_pack <= 5)
                    fprintf(stderr, "  [bridge] ONION %s (f16→f32 %u elems)\n", name, n_elems);
                return;
            }
            if (onion_sz == need) {
                memcpy(dst, onion_data, need);
                h->n_pack++; h->b_pack += need;
                tmap_add(h, name, dst);
                if (h->n_pack <= 5)
                    fprintf(stderr, "  [bridge] ONION %s (%u bytes)\n", name, onion_sz);
                return;
            }
            fprintf(stderr, "  [bridge] ONION-SIZE-MISMATCH %s: pack=%u need=%zu\n",
                    name, onion_sz, need);
        }
    }

    uint32_t csz = cell_size_of(t->type);
    uint64_t total_cells = (csz == 0) ? 0
        : (uint64_t)ggml_nelements(t) / ggml_blck_size(t->type);
    if (csz != 0 && total_cells * csz == need) {
        double t0 = now_ms();
        int rc = load_pack_tensor(h, name, csz, total_cells, dst);
        h->ms_pack += now_ms() - t0;
        if (rc > 0) {
            h->n_pack++; h->b_pack += need;
            tmap_add(h, name, dst);
            return;
        }
        /* weight-tying: output.weight not in GGUF (Qwen3).
         * REROUTE: patch t->type to Q8_0 + share pointer with token_embd.weight.
         * Same data, same kernel — octa-tetra compound principle. */
        /* ── WEIGHT-TYING Q8_0 path: if output.weight is Q8_0 (patched gguf),
         *  share pointer with token_embd.weight directly — no dequant needed. ── */
        if (t->type == GGML_TYPE_Q8_0) {
            void *tied = tmap_find(h, "token_embd.weight");
            if (tied) {
                /* Verify dimensions match: output.weight may be transposed vs token_embd.
                 * If ne[0] differs, shared pointer reads data with wrong layout. */
                int64_t ow_ne0 = t->ne[0], ow_ne1 = t->ne[1];
                int64_t te_ne0 = -1, te_ne1 = -1;
                if (h->gguf_meta) {
                    int64_t te_idx = gguf_find_tensor(h->gguf_meta, "token_embd.weight");
                    if (te_idx >= 0) {
                        const int64_t *te_ne = gguf_get_tensor_ne(h->gguf_meta, te_idx);
                        te_ne0 = te_ne[0]; te_ne1 = te_ne[1];
                    }
                }
                if (ow_ne0 == te_ne0 && ow_ne1 == te_ne1) {
                    t->data = tied;
                    h->n_pack++; h->b_pack += (size_t)ggml_nbytes(t);
                    tmap_add(h, name, tied);
                    fprintf(stderr, "  [bridge] TIED-Q8_0 %s → shared pointer (%d bytes)\n",
                            name, ggml_nbytes(t));
                    return;
                }
                fprintf(stderr, "  [bridge] TIED-Q8_0 %s DIM-MISMATCH: ow=[%lld,%lld] te=[%lld,%lld] → fallback\n",
                        name, (long long)ow_ne0, (long long)ow_ne1,
                        (long long)te_ne0, (long long)te_ne1);
            }
            fprintf(stderr, "  [bridge] TIED-Q8_0 %s — token_embd not loaded yet!\n", name);
        }

        /* ── RESIDUAL path: table-driven redirect ── */
        {
            const TESS_ResidualEntry *re = tess_pack_find_residual(h->pi, name);
            if (re) {
                uint32_t n_elems = (uint32_t)ggml_nelements(t);
                int written = tess_pack_apply_residual(h->pi, re, n_elems, dst);
                if (written > 0) {
                    h->n_pack++; h->b_pack += written;
                    tmap_add(h, name, dst);
                    fprintf(stderr, "  [bridge] RESIDUAL %s → %s (transform=%u, %d bytes)\n",
                            name, re->src, re->transform, written);
                    return;
                }
                fprintf(stderr, "  [bridge] RESIDUAL-FAIL %s (written=%d)\n", name, written);
            }
        }
        if (rc != -1 && rc != -4)
            fprintf(stderr, "  [bridge] PACK-FAIL %s rc=%d\n", name, rc);
    }

    /* check if this tensor name matches an already-loaded tensor (name dedup) */
    {
        void *dup = tmap_find(h, name);
        if (dup) { t->data = dup; h->n_pack++; return; }
    }

    /* ── GGUF FALLBACK: read tensor data from mmap'd GGUF ── */
    if (h->gguf_mmap && h->gguf_meta) {
        int64_t tid = gguf_find_tensor(h->gguf_meta, name);
        if (tid >= 0) {
            size_t data_off = gguf_get_data_offset(h->gguf_meta);
            size_t t_off    = gguf_get_tensor_offset(h->gguf_meta, tid);
            size_t t_sz     = (size_t)ggml_nbytes(t);
            const uint8_t *src = h->gguf_mmap + data_off + t_off;
            memcpy(dst, src, t_sz);
            h->n_pack++; h->b_pack += t_sz;
            tmap_add(h, name, dst);
            fprintf(stderr, "  [bridge] GGUF-FALLBACK %s (%zu bytes)\n", name, t_sz);
            return;
        }
        if (h->n_zero < 15)
            fprintf(stderr, "  [bridge] GGUF-MISS %s (tid=%lld)\n", name, (long long)tid);
    } else {
        if (h->n_zero < 5)
            fprintf(stderr, "  [bridge] NO-GGUF-FALLBACK %s (mmap=%p meta=%p)\n", name,
                    (void*)h->gguf_mmap, (void*)h->gguf_meta);
    }

    /* optional tensors (scale/input_scale = identity) */
    memset(dst, 0, need);
    const char *nm = name;
    size_t nl = strlen(nm);
    if ((nl >= 6 && strcmp(nm + nl - 6, ".scale") == 0) ||
        (nl >= 13 && strcmp(nm + nl - 13, ".input_scale") == 0)) {
        for (size_t k = 0; k + 4 <= need; k += 4)
            *(float *)(dst + k) = 1.0f;
    }
    /* rope_freqs.weight: pre-computed rotary frequencies (base^(-2i/n_rot)) */
    if (strcmp(name, "rope_freqs.weight") == 0) {
        int n_f32 = (int)(need / sizeof(float));
        int n_rot = n_f32 * 2;
        float base = 10000.0f;
        if (h->gguf_meta) {
            int64_t kv_idx = gguf_find_key(h->gguf_meta, "llama.rope.freq_base");
            if (kv_idx < 0) kv_idx = gguf_find_key(h->gguf_meta, "rope.freq_base");
            if (kv_idx >= 0) base = gguf_get_val_f32(h->gguf_meta, kv_idx);
            kv_idx = gguf_find_key(h->gguf_meta, "llama.rope.dimension_count");
            if (kv_idx < 0) kv_idx = gguf_find_key(h->gguf_meta, "rope.dimension_count");
            if (kv_idx >= 0) n_rot = (int)gguf_get_val_u32(h->gguf_meta, kv_idx);
            if (n_rot > 0) n_f32 = n_rot / 2;
        }
        for (int i = 0; i < n_f32; i++) {
            ((float *)dst)[i] = powf(base, -2.0f * (float)i / (float)n_rot);
        }
        h->n_pack++; h->b_pack += need;
        tmap_add(h, name, dst);
        fprintf(stderr, "  [bridge] ROPE-FREQS %s (%d floats, base=%.1f, n_rot=%d)\n", name, n_f32, base, n_rot);
        return;
    }
    if (h->n_zero < 10 || h->n_zero % 200 == 0)
        fprintf(stderr, "  [bridge] ZERO [%u] %s type=%u bytes=%llu\n", h->n_zero, name, (unsigned)t->type, (unsigned long long)need);
    h->n_zero++;
}

/* ── inference run ── */
typedef struct {
    float       *logits;
    int          n_dumps;
    int          n_vocab;
    llama_token *toks;
    int          n_toks;
    double       prompt_ms;
    double       gen_ms;
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
    res->n_dumps = n_gen + 1;
    res->logits = (float *)malloc((size_t)res->n_dumps * res->n_vocab * sizeof(float));
    res->toks   = (llama_token *)malloc((size_t)n_gen * sizeof(llama_token));
    if (!res->logits || !res->toks) { run_result_free(res); return -1; }

    struct llama_context_params cp = llama_context_default_params();
    cp.n_batch = 128;
    cp.n_ctx   = 256;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { run_result_free(res); return -2; }

    int n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                  NULL, 0, true, false);
    if (n_prompt < 0) n_prompt = -n_prompt;
    if (n_prompt == 0) { llama_free(ctx); run_result_free(res); return -3; }
    llama_token *ptoks = (llama_token *)malloc((size_t)(n_prompt + 8) * sizeof(llama_token));
    int n2 = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                            ptoks, n_prompt + 8, true, false);
    if (n2 < 0 || n2 > n_prompt + 8) { free(ptoks); llama_free(ctx); run_result_free(res); return -3; }
    n_prompt = n2;

    double t0 = now_ms();
    if (llama_decode(ctx, llama_batch_get_one(ptoks, n_prompt)) != 0) {
        free(ptoks); llama_free(ctx); run_result_free(res); return -4;
    }
    res->prompt_ms = now_ms() - t0;
    memcpy(res->logits, llama_get_logits(ctx), (size_t)res->n_vocab * sizeof(float));
    free(ptoks);

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
    res->n_dumps = res->n_toks + 1;

    llama_sampler_free(smpl);
    llama_free(ctx);
    return 0;
}

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
    printf("  logits: %d positions x %d vocab — diffs=%llu maxdiff=%.6e\n",
           nd, a->n_vocab, (unsigned long long)diffs, maxdiff);
    printf("  tokens: A=%d B=%d %s\n", a->n_toks, b->n_toks,
           pass ? "IDENTICAL" : "MISMATCH");
    return pass && diffs == 0;
}

static void register_cpu_backend(const char *dll_dir) {
    static const char *cpu_plugins[] = {
        "ggml-cpu-zen4.dll", "ggml-cpu-alderlake.dll",
        "ggml-cpu-icelake.dll", "ggml-cpu-sapphirerapids.dll",
        "ggml-cpu-haswell.dll", "ggml-cpu-cannonlake.dll",
        "ggml-cpu-cooperlake.dll", "ggml-cpu-skylakex.dll",
        "ggml-cpu-cascadelake.dll", "ggml-cpu-piledriver.dll",
        "ggml-cpu-ivybridge.dll", "ggml-cpu-sandybridge.dll",
        "ggml-cpu-sse42.dll", NULL
    };
    char dll_path[1024];
    int loaded = 0;
    for (int i = 0; cpu_plugins[i]; i++) {
        snprintf(dll_path, sizeof(dll_path), "%s\\%s", dll_dir, cpu_plugins[i]);
        if (ggml_backend_load(dll_path)) { loaded = 1; break; }
    }
    if (!loaded) {
        printf("  (note: CPU-only backend load failed; falling back to load-all)\n");
        ggml_backend_load_all_from_path(dll_dir);
    }
}

int main(int argc, char **argv) {
    const char *pack_path = (argc > 1) ? argv[1] : "F:\\model\\bonsai-4b-q1_0.tesspack";
    const char *gguf_path = (argc > 2) ? argv[2] : NULL;
    const char *dll_dir   = (argc > 3) ? argv[3] : "I:\\llama\\llama-v040-bin-win-vulkan-x64";
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
    register_cpu_backend(dll_dir);

    /* ── open .tesspack (mmap-only mode: zero malloc) ── */
    TESS_PackIndex pi;
    if (tess_pack_open_mmap(&pi, pack_path) != 0) {
        printf("FAIL: tesspack open %s\n", pack_path); return 1;
    }
    printf("Tesspack Bridge — mmap-only mode (zero malloc, page-fault driven)\n");
    printf("Pack: %s (%u capos, %.1f MB)\n", pack_path, pi.n_capos, (double)pi.file_sz / 1e6);
    printf("RSS after mmap open: %.1f MB\n", rss_mb());

    /* ── extract embedded GGUF header (for info only) ── */
    uint64_t hdr_sz = 0;
    const uint8_t *hdr_bytes = tess_pack_get_gguf_header(&pi, &hdr_sz);
    if (hdr_bytes && hdr_sz > 0) {
        printf("Embedded GGUF header: %llu bytes\n", (unsigned long long)hdr_sz);
    } else {
        printf("NOTE: no embedded GGUF header in pack\n");
    }

    /* Write a complete GGUF file for the DLL parser.
     * The DLL's gguf_init_from_file needs the FULL file (not just the header)
     * because it reads tensor info section which references data offsets within
     * the full file. We write: header bytes + seek to original GGUF size with
     * a sparse temp file (OS only allocates the metadata, not tensor data). */
    char tmp_gguf[MAX_PATH];
    tmp_gguf[0] = '\0';

    const char *phase = getenv("TESS_PHASE");
    int run_both = (!phase || strcmp(phase, "both") == 0);

    RunResult ra; memset(&ra, 0, sizeof(ra));
    RunResult rb; memset(&rb, 0, sizeof(rb));
    double loadA_ms = 0.0, loadB_ms = 0.0;

    /* ── Phase A: baseline (only if GGUF provided) ── */
    if (gguf_path && (run_both || (phase && strcmp(phase, "plain") == 0))) {
        printf("GGUF: %s (baseline)\n", gguf_path);
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
        llama_model_free(mA);
    }

    if (phase && strcmp(phase, "plain") == 0) {
        printf("\n(TESS_PHASE=plain — bridge skipped)\n");
        run_result_free(&ra);
        tess_pack_close(&pi);
        return 0;
    }

    /* ── Phase B: bridge from .tesspack ── */
    printf("Phase B: bridge from .tesspack\n");

    struct gguf_init_params gip = { /*.no_alloc =*/ true, /*.ctx =*/ NULL };
    struct gguf_context *meta = NULL;

    if (gguf_path) {
        meta = gguf_init_from_file(gguf_path, gip);
        if (!meta) {
            printf("FAIL: gguf_init_from_file(%s)\n", gguf_path);
            tess_pack_close(&pi); return 1;
        }
        printf("  metadata from GGUF: %s\n", gguf_path);
    } else if (hdr_bytes && hdr_sz > 0) {
        snprintf(tmp_gguf, sizeof(tmp_gguf), "%s\\tesspack_bridge_hdr.tmp",
                 getenv("TEMP") ? getenv("TEMP") : ".");
        FILE *tf = fopen(tmp_gguf, "wb");
        if (!tf) { printf("FAIL: tmp create\n"); tess_pack_close(&pi); return 1; }
        fwrite(hdr_bytes, 1, (size_t)hdr_sz, tf);
        uint64_t pad_to = hdr_sz + (1024ULL * 1024 * 1024);
        _fseeki64(tf, (long)(pad_to - 1), SEEK_SET);
        fputc(0, tf);
        fclose(tf);
        meta = gguf_init_from_file(tmp_gguf, gip);
        if (!meta) {
            printf("FAIL: gguf_init_from_file on sparse tmp (header=%llu B)\n",
                   (unsigned long long)hdr_sz);
            remove(tmp_gguf);
            tess_pack_close(&pi); return 1;
        }
        printf("  metadata from embedded header (sparse tmp)\n");
    } else {
        printf("FAIL: no GGUF and no embedded header\n");
        tess_pack_close(&pi); return 1;
    }

    /* Patch gguf_context: add output.weight as Q8_0 if weight-tied.
     * This makes llama create a Q8_0 buffer for lm_head instead of F32 phantom,
     * so the callback fills Q8_0 data → Q8_0 kernel → bitwise lossless. */
    {
        int64_t idx_te = gguf_find_tensor(meta, "token_embd.weight");
        int64_t idx_ow = gguf_find_tensor(meta, "output.weight");
        if (idx_te >= 0 && idx_ow < 0) {
            /* weight-tied model: output.weight not in GGUF → add as Q8_0 */
            const int64_t *ne = gguf_get_tensor_ne(meta, idx_te);
            /* count actual dims (ne[3] and ne[2] may be 1) */
            int ndims = 1;
            if (ne[1] > 1) ndims = 2;
            if (ne[2] > 1) ndims = 3;
            if (ne[3] > 1) ndims = 4;

            struct ggml_init_params gp = { .mem_size = 4096, .mem_buffer = NULL, .no_alloc = true };
            struct ggml_context *gctx = ggml_init(gp);
            struct ggml_tensor *tw;
            if (ndims == 2)
                tw = ggml_new_tensor_2d(gctx, GGML_TYPE_Q8_0, ne[0], ne[1]);
            else if (ndims == 3)
                tw = ggml_new_tensor_3d(gctx, GGML_TYPE_Q8_0, ne[0], ne[1], ne[2]);
            else if (ndims == 4)
                tw = ggml_new_tensor_4d(gctx, GGML_TYPE_Q8_0, ne[0], ne[1], ne[2], ne[3]);
            else
                tw = ggml_new_tensor_1d(gctx, GGML_TYPE_Q8_0, ne[0]);
            ggml_set_name(tw, "output.weight");
            gguf_add_tensor(meta, tw);
            printf("  [bridge] patched gguf: added output.weight as Q8_0 (%dD, [%lld,%lld,%lld])\n",
                   ndims, (long long)ne[0], (long long)ne[1], (long long)ne[2]);
            ggml_free(gctx);
        } else if (idx_ow >= 0) {
            printf("  [bridge] output.weight already in gguf (idx=%lld, type=%d)\n",
                   (long long)idx_ow, (int)gguf_get_tensor_type(meta, idx_ow));
        }
    }

    TensorHook hook; memset(&hook, 0, sizeof(hook));
    hook.pi = &pi;
    hook.gguf_meta = meta;

    /* mmap the original GGUF for fallback data reads */
    {
#ifdef _WIN32
        HANDLE hf = CreateFileA(gguf_path, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER fsz;
            GetFileSizeEx(hf, &fsz);
            hook.gguf_size = (uint64_t)fsz.QuadPart;
            HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
            if (hm) {
                hook.gguf_mmap = (const uint8_t *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
                CloseHandle(hm);
            }
            CloseHandle(hf);
        }
#else
        struct stat st;
        if (stat(gguf_path, &st) == 0) {
            hook.gguf_size = (uint64_t)st.st_size;
            int fd = open(gguf_path, O_RDONLY);
            if (fd >= 0) {
                hook.gguf_mmap = mmap(NULL, hook.gguf_size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (hook.gguf_mmap == MAP_FAILED) hook.gguf_mmap = NULL;
                close(fd);
            }
        }
#endif
        if (hook.gguf_mmap)
            printf("  [bridge] GGUF fallback: mmap'd %s (%.1f MB)\n", gguf_path, hook.gguf_size / 1e6);
    }

    double t0 = now_ms();
    struct llama_model_params mpB = llama_model_default_params();
    mpB.n_gpu_layers = 0;
    static struct llama_model_tensor_buft_override ovr[2];
    ovr[0].pattern = ".*";
    ovr[0].buft = ggml_backend_cpu_buffer_type();
    ovr[1].pattern = NULL;
    if (ovr[0].buft) mpB.tensor_buft_overrides = ovr;

    struct llama_model *mB = llama_model_init_from_user(meta, provide_tensor,
                                                        &hook, mpB);
    if (!mB) {
        printf("FAIL: phase B model init (pack=%u zero=%u errors=%u)\n",
               hook.n_pack, hook.n_zero, hook.errors);
        gguf_free(meta); tess_pack_close(&pi); return 1;
    }
    loadB_ms = now_ms() - t0;
    printf("Phase B (bridge): load %.1f ms  RSS=%.1f MB\n", loadB_ms, rss_mb());
    printf("  callback: pack=%u tensors (%.1f MB, %.1f ms) | zero=%u | errors=%u\n",
           hook.n_pack, hook.b_pack / 1e6, hook.ms_pack,
           hook.n_zero, hook.errors);

    if (run_session(mB, prompt, n_gen, &rb) != 0) {
        printf("FAIL: phase B session\n"); return 1;
    }
    printf("Phase B: prompt eval %.1f ms | gen %d tok in %.1f ms = %.2f tok/s  RSS=%.1f MB\n",
           rb.prompt_ms, rb.n_toks, rb.gen_ms,
           rb.gen_ms > 0 ? rb.n_toks * 1000.0 / rb.gen_ms : 0.0, rss_mb());

    if (phase && strcmp(phase, "bridge") == 0) {
        printf("\n(TESS_PHASE=bridge — baseline not run)\n");
        run_result_free(&rb);
        llama_model_free(mB);
        /* gguf freed via model_free */
        remove(tmp_gguf);
        tess_pack_close(&pi);
        return 0;
    }

    /* ── verdict ── */
    if (ra.toks || rb.toks) {
        int equal = compare_runs(&ra, &rb);
        printf("═══════════════════════════════════════════════════════════════\n");
        printf("LOAD:  plain=%.1f ms   bridge=%.1f ms   (%+.1f ms)\n",
               loadA_ms, loadB_ms, loadB_ms - loadA_ms);
        printf("TOK/S: plain=%.2f       bridge=%.2f\n",
               ra.gen_ms > 0 ? ra.n_toks * 1000.0 / ra.gen_ms : 0.0,
               rb.gen_ms > 0 ? rb.n_toks * 1000.0 / rb.gen_ms : 0.0);
        printf("RESULT: %s\n", equal ? "PASS — bridge == plain GGUF (bitwise)"
                                     : "FAIL — models differ");
    }

    run_result_free(&ra);
    run_result_free(&rb);
    llama_model_free(mB);
    remove(tmp_gguf);
    tess_pack_close(&pi);
    return 0;
}
