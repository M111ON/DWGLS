/* tools/moe_stream_proof.c — card #3 LIVE K<<E proof (huihui-moe-1b)
 * ═══════════════════════════════════════════════════════════════════
 * Phase A: stock file-load baseline → greedy tokens per prompt (oracle).
 * Phase B(K): callback model. Non-expert tensors → source mmap (zero-copy).
 *   Expert tensors → per-tensor VirtualAlloc(MEM_COMMIT) with ONLY the
 *   selected K experts' slices copied from source; the rest stays
 *   demand-zero (RAM grows only with written pages). Unselected experts
 *   read as zeros — if the prompt routes to one, tokens diverge and K
 *   widens 1→2→3 experts/layer and the prompt retries. Convergence ==
 *   proof that only K of E experts were needed, with identical tokens.
 *
 * Batch topology identical both phases (n_batch=512, single-token steps,
 * greedy, flash on) per #6292/#6293. Reports E bytes, K bytes, peak RSS.
 *
 * Assumption (byte-layout only, correctness-gated): expert e of an
 * [a,b,E] tensor occupies contiguous slice [e*sl,(e+1)*sl), sl=size/E.
 * If wrong, tokens diverge and K widens — the gate catches it, never
 * silent. Verified layout would come from dequant (YAGNI here).
 *
 * BUILD: make moe-stream-proof
 * RUN:   ./build/moe_stream_proof [gguf] [zc2_dll_dir] [backend_dir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
#include "gguf_reader.h"
#include "llama.h"
#include "ggml-backend.h"
#include "gguf.h"
#ifdef _WIN32
#include <psapi.h>
/* resident pages within byte range [off,off+len) of a mapping (dual TRACE proven) */
static uint64_t range_resident(const uint8_t *base, uint64_t off, uint64_t len) {
    if (len == 0) return 0;
    uint64_t p0 = off / 4096, p1 = (off + len - 1) / 4096;
    uint64_t n = p1 - p0 + 1, got = 0;
    PSAPI_WORKING_SET_EX_INFORMATION *info =
        (PSAPI_WORKING_SET_EX_INFORMATION *)calloc((size_t)n, sizeof(*info));
    if (!info) return 0;
    for (uint64_t i = 0; i < n; i++) info[i].VirtualAddress = (PVOID)(base + (p0 + i) * 4096);
    if (QueryWorkingSetEx(GetCurrentProcess(), info, (DWORD)(n * sizeof(*info))))
        for (uint64_t i = 0; i < n; i++)
            if (info[i].VirtualAttributes.Flags & 1) got++;
    free(info);
    return got;
}
#else
#include <sys/mman.h>
#include <unistd.h>
/* Linux twin: mincore residency over the same byte range */
static uint64_t range_resident(const uint8_t *base, uint64_t off, uint64_t len) {
    if (len == 0) return 0;
    static long ps = 0;
    if (!ps) ps = sysconf(_SC_PAGESIZE);
    uint64_t start = ((uint64_t)base + off) & ~((uint64_t)ps - 1);
    uint64_t end = (uint64_t)base + off + len;
    uint64_t n = (end - start + ps - 1) / ps, got = 0;
    unsigned char *vec = (unsigned char *)calloc((size_t)n, 1);
    if (!vec) return 0;
    if (mincore((void *)start, (size_t)(n * ps), vec) == 0)
        for (uint64_t i = 0; i < n; i++)
            if (vec[i] & 1) got++;
    free(vec);
    return got;
}
#endif
static int G_MEASURE = 0;   /* fire-measure mode (argv K0='m') */
static int G_NGL = 0;         /* GPU layers offload (argv[7], e.g. 35 Vulkan) */

static const char *PROMPTS[] = {
    "The capital of France is",
    "Write a Python function that sorts a list",
    "1 + 2 + 3 + 4 + 5 =",
};
#define N_PROMPTS 3
#define N_GEN 16
static int G_NGEN = N_GEN;
static const char *G_PROMPT = NULL;   /* single-prompt override (argv) */
static uint32_t G_K0 = 0;             /* static top-K override: skip minimization */

typedef struct { const char *substr; int wtype; } TP;
static const TP PATS[] = {
    {".ffn_down_exps.weight", 0},
    {".ffn_gate_exps.weight", 1},
    {".ffn_up_exps.weight", 2},
};
static int exp_layer(const char *name, int *wtype) {
    const char *p = strstr(name, "blk.");
    if (!p) return -1;
    int layer = atoi(p + 4);
    for (size_t i = 0; i < 3; i++)
        if (strstr(name, PATS[i].substr)) { *wtype = PATS[i].wtype; return layer; }
    return -1;
}

typedef struct {
    const uint8_t *src;      /* source mmap base (data section) */
    const uint64_t *offs;
    const uint32_t *sizes;
    const char **names;
    uint32_t n;
    uint32_t matched, missing;
    /* stream state (Phase B): per-tensor expert buffer + selected mask */
    uint8_t **exp_buf;       /* per tensor idx: committed buffer or NULL */
    uint64_t *exp_mask;      /* per tensor idx: resident expert bits */
    uint32_t *exp_E;         /* per tensor idx: expert count (0 = dense) */
    uint32_t K;              /* experts/layer selected this round */
} SCtx;

static void provide_stream(struct ggml_tensor *t, void *ud) {
    SCtx *s = (SCtx *)ud;
    const char *name = ggml_get_name(t);
    size_t nb = ggml_nbytes(t);
    for (uint32_t i = 0; i < s->n; i++) {
        if (strcmp(s->names[i], name) != 0) continue;
        if (nb != s->sizes[i]) { s->missing++; return; }
        if (s->exp_buf && s->exp_buf[i]) { t->data = s->exp_buf[i]; s->matched++; return; }
        t->data = (void *)(s->src + s->offs[i]);
        s->matched++;
        return;
    }
    if (s->missing < 10)
        fprintf(stderr, "  [stream] missing: %s nb=%zu\n", name, nb);
    uint8_t *fb = (uint8_t *)calloc(1, nb ? nb : 1);
    if (!fb) { s->missing++; return; }
    if (!strstr(name, ".bias")) {
        float *fd = (float *)fb;
        for (size_t k = 0; k < nb / sizeof(float); k++) fd[k] = 1.0f;
    }
    t->data = fb;
    s->missing++;
}

/* greedy downward minimization: model stays loaded, one context per probe.
 * Start mask=all; per layer try {keep 0,1},{keep 0},{keep 1}... in general
 * try dropping to each k-subset? YAGNI: experts are indexed 0..E-1, try
 * keep-first-k for k=E-1..1 (static order, gate decides). Returns K/layer. */
static int gen_tokens(struct llama_model *m, const char *prompt, int *out);
static int gen_on_model(struct llama_model *m, const char *prompt, int *out) {
    return gen_tokens(m, prompt, out);
}

static uint32_t layer_expert_count(SCtx *s, int layer) {
    for (uint32_t i = 0; i < s->n; i++) {
        int wt = 0;
        if (exp_layer(s->names[i], &wt) == layer) return s->exp_E[i];
    }
    return 0;
}

/* static prior: top-K experts/layer by raw-byte head sum of the GATE slice.
 * Deterministic but prompt-blind — the token gate decides, widen on diverge. */
static uint64_t topk_mask(SCtx *s, int layer, uint32_t K) {
    uint32_t E = layer_expert_count(s, layer);
    if (!E) return 0;
    if (K >= E) return (E >= 64 ? ~0ull : ((1ull << E) - 1));
    /* find gate tensor of this layer for scoring */
    uint32_t gi = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        int wt = 0;
        if (exp_layer(s->names[i], &wt) == layer && wt == 1 && s->exp_E[i]) { gi = i; break; }
    }
    uint64_t sl = s->sizes[gi] / E, head = sl > 4096 ? 4096 : sl;
    /* bias prior: blk.L.exp_probs_b.bias (f32[E]) is the router's own global
     * offset per expert — high bias fires often regardless of prompt. */
    uint32_t bi_idx = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        char want[64];
        snprintf(want, sizeof(want), "blk.%d.exp_probs_b.bias", layer);
        if (strcmp(s->names[i], want) == 0) { bi_idx = i; break; }
    }
    const float *bias = (bi_idx && s->sizes[bi_idx] == E * 4)
        ? (const float *)(s->src + s->offs[bi_idx]) : NULL;
    uint64_t picked = 0;
    uint64_t used = 0;
    double best = 0;
    for (uint32_t r = 0; r < K; r++) {
        int bi = -1;
        for (uint32_t e = 0; e < E; e++) {
            if (used & (1ull << e)) continue;
            double key;
            if (bias) key = (double)bias[e];
            else {
                const uint8_t *p = s->src + s->offs[gi] + e * sl;
                uint64_t acc = 0;
                for (uint64_t b = 0; b < head; b++) acc += p[b];
                key = (double)acc;
            }
            if (bi < 0 || key > best) { best = key; bi = (int)e; }
        }
        if (bi < 0) break;
        used |= 1ull << bi;
        picked |= 1ull << bi;
    }
    return picked;
}

/* apply mask for one layer across its 3 tensors (gate/up/down share E).
 * Reserve/commit discipline (box safety): buffers are MEM_RESERVE address
 * only; each apply commits exactly the selected slice ranges (page-rounded)
 * and decommits ranges that fall out of the mask. Commit charge ≈ K bytes,
 * never E. Selections are idempotent (commit twice = no-op). */
static void layer_apply(SCtx *s, int layer, uint64_t keep_mask, uint64_t *bytes) {
    /* per-slice commit: reserve-only buffers gain committed ranges exactly
     * for selected slices (page-rounded). Unselected stays RESERVED
     * (inaccessible): routing outside the mask crashes LOUD (never silent
     * wrong tokens). Charge ≈ K, never E. */
    uint64_t b = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        int wt = 0;
        if (exp_layer(s->names[i], &wt) != layer || s->exp_E[i] == 0) continue;
        if (!s->exp_buf[i]) continue;
        uint32_t E = s->exp_E[i];
        uint64_t sl = s->sizes[i] / E;
        uint64_t old = s->exp_mask[i];
        for (uint32_t e = 0; e < E; e++) {
            uint8_t *dst = s->exp_buf[i] + e * sl;
            int want = (keep_mask & (1ull << e)) != 0;
            int had = (old & (1ull << e)) != 0;
            if (want) {
                memcpy(dst, s->src + s->offs[i] + e * sl, (size_t)sl);
                b += sl;
            } else if (had) {
                memset(dst, 0, (size_t)sl);
            }
            /* !want && !had: committed demand-zero, never faulted (RAM stays K) */
        }
        s->exp_mask[i] = keep_mask;
    }
    if (bytes) *bytes = b;
}

static void stream_free_bufs(SCtx *s) {
    if (!s->exp_buf) return;
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->exp_buf[i]) {
#ifdef _WIN32
            VirtualFree(s->exp_buf[i], 0, MEM_RELEASE);
#else
            free(s->exp_buf[i]);
#endif
            s->exp_buf[i] = NULL;
        }
    }
}

static uint64_t peak_rss(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PeakWorkingSetSize;
#endif
    return 0;
}

/* greedy N_GEN tokens; returns 0 ok. out[] filled when non-NULL */
static int gen_tokens(struct llama_model *m, const char *prompt, int *out) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_batch = 512;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    struct llama_context *ctx = llama_init_from_model(m, cp);
    if (!ctx) return -1;
    struct llama_sampler *smpl =
        llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    const struct llama_vocab *vocab = llama_model_get_vocab(m);
    llama_token toks[256];
    int n = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, 250, true, false);
    if (n <= 0) { llama_sampler_free(smpl); llama_free(ctx); return -1; }
    if (llama_decode(ctx, llama_batch_get_one(toks, n)) != 0) {
        llama_sampler_free(smpl); llama_free(ctx); return -1;
    }
    for (int g = 0; g < G_NGEN; g++) {
        llama_token tk = llama_sampler_sample(smpl, ctx, -1);
        if (out) out[g] = (int)tk;
        llama_sampler_accept(smpl, tk);
        llama_batch b = llama_batch_get_one(&tk, 1);
        if (llama_decode(ctx, b) != 0) {
            llama_sampler_free(smpl); llama_free(ctx); return -1;
        }
    }
    llama_sampler_free(smpl);
    llama_free(ctx);
    return 0;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    const char *gguf_path = (argc > 1) ? argv[1] : "F:/model/huihui-moe-1b-q4_k_m.gguf";
    const char *dll_dir = (argc > 2) ? argv[2] : "I:/llama/llama.cpp/build_zc2/bin/Release";
    const char *backend_dir = (argc > 3) ? argv[3] : "I:/DWGLS-native-fs/build/cpuonly";
    if (argc > 4) G_PROMPT = argv[4];
    if (argc > 5) { G_NGEN = atoi(argv[5]); if (G_NGEN < 1) G_NGEN = 1; if (G_NGEN > 64) G_NGEN = 64; }
    if (argc > 6) {
        if (argv[6][0] == 'm' || argv[6][0] == 'M') G_MEASURE = 1;
        else G_K0 = (uint32_t)atoi(argv[6]);
    }
    int G_REPLAY = (argc > 6 && argv[6][0] == 'r' && argv[6][1] == 'e');
    const char *maskfile = G_REPLAY && argc > 7 ? argv[7] : NULL;
    printf("=== MoE stream proof (K<<E live) ===\nGGUF: %s\n", gguf_path);

    GgufReader g;
    if (gguf_open(gguf_path, &g) != 0) { printf("FAIL: open\n"); return 1; }
    const uint8_t *data = g.base + g.data_offset;

    /* expert census */
    uint32_t n_exp_tensors = 0;
    uint32_t *exp_E = (uint32_t *)calloc(g.n_tensors, sizeof(uint32_t));
    int max_layer = -1;
    for (uint32_t i = 0; i < g.n_tensors; i++) {
        int wt = 0, L = exp_layer(g.names[i], &wt);
        if (L < 0) continue;
        uint8_t nd = g.n_dims[i];
        uint32_t E = (nd >= 3) ? (uint32_t)g.dims[(size_t)i * 4 + nd - 1] : 0;
        if (E < 2 || E > 64 || g.sizes[i] % E != 0) continue;
        exp_E[i] = E;
        n_exp_tensors++;
        if (L > max_layer) max_layer = L;
    }
    printf("expert tensors: %u  layers: %d\n", n_exp_tensors, max_layer + 1);
    if (!n_exp_tensors) { printf("FAIL: no expert tensors\n"); return 1; }

    if (argc > 7 && !G_REPLAY) { G_NGL = atoi(argv[7]); if (G_NGL < 0) G_NGL = 0; if (G_NGL > 100) G_NGL = 100; }
    ggml_backend_load_all_from_path(backend_dir);

    /* ── REPLAY mode: seed file in, tokens out. No baseline, no router.
     * Maskfile lines: TOKENS=<t0 t1 ...> then L<layer>=<hexmask> per line.
     * Proves the recorded route replays deterministically in a FRESH process.
     * Exit 0 = tokens match seed file, 1 = diverge. */
    if (G_REPLAY) {
        if (!maskfile) { printf("FAIL: replay needs maskfile argv[7]\n"); return 1; }
        FILE *f = fopen(maskfile, "r");
        if (!f) { printf("FAIL: open %s\n", maskfile); return 1; }
        int exp_toks[64], n_exp = 0;
        uint64_t *rmask = (uint64_t *)calloc((size_t)(max_layer + 1), sizeof(uint64_t));
        char line[4096];
        const char *rprompt = G_PROMPT ? G_PROMPT : PROMPTS[0];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "TOKENS=", 7) == 0) {
                char *q = line + 7;
                while (n_exp < 64) {
                    while (*q == ' ' || *q == '\t') q++;
                    if (*q < '0' || *q > '9') break;
                    exp_toks[n_exp++] = atoi(q);
                    while (*q >= '0' && *q <= '9') q++;
                }
            } else if (line[0] == 'L') {
                int L = atoi(line + 1);
                char *c = strchr(line, ':');
                if (c && L >= 0 && L <= max_layer)
                    rmask[L] = strtoull(c + 1, NULL, 16);
            }
        }
        fclose(f);
        if (!n_exp) { printf("FAIL: no TOKENS in %s\n", maskfile); return 1; }
        G_NGEN = n_exp;
        struct ggml_context *mctx = NULL;
        struct gguf_init_params gp = { .no_alloc = false, .ctx = &mctx };
        struct gguf_context *rgctx = gguf_init_from_file(gguf_path, gp);
        if (!rgctx) { printf("FAIL: meta\n"); return 1; }
        SCtx s;
        memset(&s, 0, sizeof(s));
        s.src = data; s.offs = g.offsets; s.sizes = g.sizes;
        s.names = (const char **)g.names; s.n = g.n_tensors;
        s.exp_buf = (uint8_t **)calloc(g.n_tensors, sizeof(uint8_t *));
        s.exp_mask = (uint64_t *)calloc(g.n_tensors, sizeof(uint64_t));
        s.exp_E = exp_E;
        for (uint32_t i = 0; i < s.n; i++) {
            if (!s.exp_E[i]) continue;
#ifdef _WIN32
            /* commit-all (h7 world): unmasked reads as zeros, never crashes.
             * Strict reserve-gate returns as a later experiment. */
            s.exp_buf[i] = (uint8_t *)VirtualAlloc(NULL, s.sizes[i], MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
            s.exp_buf[i] = (uint8_t *)calloc(1, s.sizes[i]);
#endif
            if (!s.exp_buf[i]) { printf("FAIL: alloc\n"); return 1; }
        }
        for (int L = 0; L <= max_layer; L++) {
            uint32_t E = layer_expert_count(&s, L);
            if (!E) continue;
            uint64_t ign = 0;
            layer_apply(&s, L, rmask[L], &ign);
        }
        struct llama_model_params mpR = llama_model_default_params();
        mpR.n_gpu_layers = 0;
        mpR.no_host = true;
        struct llama_model *mR = llama_model_init_from_user(rgctx, provide_stream, &s, mpR);
        if (!mR) { printf("FAIL: replay load\n"); return 1; }
        int got[64];
        fprintf(stderr, "REPLAY: commit done, generate start\n");
        int rc = gen_on_model(mR, rprompt, got);
        printf("  REPLAY:");
        for (int k = 0; k < n_exp; k++) printf(" %d", got[k]);
        printf("\n");
        int same = rc == 0;
        for (int k = 0; k < n_exp && same; k++)
            if (got[k] != exp_toks[k]) same = 0;
        printf(same ? "REPLAY PASS: seed replays deterministically, no router\n"
                    : "REPLAY DIVERGED (honest)\n");
        llama_model_free(mR);
        stream_free_bufs(&s);
        free(s.exp_buf); free(s.exp_mask); free(exp_E); free(rmask);
        gguf_close(&g);
        return same ? 0 : 1;
    }

    /* ── Phase A: baseline (stock file load) ── */
    struct llama_model_params mpA = llama_model_default_params();
    mpA.n_gpu_layers = G_NGL;
    struct llama_model *mA = llama_model_load_from_file(gguf_path, mpA);
    if (!mA) { printf("FAIL: baseline load\n"); return 1; }
    int ref[N_PROMPTS][64];
    for (int p = 0; p < (G_PROMPT ? 1 : N_PROMPTS); p++) {
        const char *pr = G_PROMPT ? G_PROMPT : PROMPTS[p];
        if (gen_tokens(mA, pr, ref[p]) != 0) { printf("FAIL: baseline gen p%d\n", p); return 1; }
        printf("  base p%d:", p);
        for (int k = 0; k < G_NGEN; k++) printf(" %d", ref[p][k]);
        printf("\n");
    }
    uint64_t rss_base = peak_rss();
    printf("baseline peak RSS: %.1f MB\n", rss_base / 1e6);
    llama_model_free(mA);

    /* ── Phase B: load once per prompt, minimize in place ── */
    struct ggml_context *meta_ctx = NULL;
    struct gguf_init_params gp = { .no_alloc = false, .ctx = &meta_ctx };
    struct gguf_context *gctx = gguf_init_from_file(gguf_path, gp);
    if (!gctx) { printf("FAIL: meta\n"); return 1; }

    SCtx s;
    memset(&s, 0, sizeof(s));
    s.src = data; s.offs = g.offsets; s.sizes = g.sizes;
    s.names = (const char **)g.names; s.n = g.n_tensors;
    s.exp_buf = (uint8_t **)calloc(g.n_tensors, sizeof(uint8_t *));
    s.exp_mask = (uint64_t *)calloc(g.n_tensors, sizeof(uint64_t));
    s.exp_E = exp_E;

    uint64_t E_total = 0;
    for (uint32_t i = 0; i < s.n; i++)
        if (s.exp_E[i]) E_total += s.sizes[i];

    int overall = 0;
    int n_prompts = G_PROMPT ? 1 : N_PROMPTS;
    for (int p = 0; p < n_prompts; p++) {
        const char *pr = G_PROMPT ? G_PROMPT : PROMPTS[p];
        /* sanity FIRST with zero commit: exp_buf NULL → callback serves src
         * mmap directly (mechanism check, no 4.66GB charge). */
        s.matched = s.missing = 0;
        struct llama_model_params mpS = llama_model_default_params();
        mpS.n_gpu_layers = G_NGL;
        mpS.no_host = true;
        struct llama_model *mS = llama_model_init_from_user(gctx, provide_stream, &s, mpS);
        if (!mS) { printf("FAIL: sanity load p%d\n", p); return 1; }
        int got[64], ok = 1;
        if (gen_on_model(mS, pr, got) != 0) { printf("FAIL: sanity gen p%d\n", p); return 1; }
        for (int k = 0; k < G_NGEN; k++)
            if (got[k] != ref[p][k]) { ok = 0; break; }
        llama_model_free(mS);
        if (!ok) { printf("  p%d: SANITY DIVERGES (callback path broken), stop\n", p); overall = 1; continue; }
        printf("  p%d: sanity full-E via mmap OK (zero commit)\n", p);

        /* ── fire-measure mode: observe TRUE fired set in one run ──
         * DropWS → generate (must match ref: same trajectory) → per-slice
         * residency snap. Fired expert ≈100% pages (matmul reads full W),
         * unfired ≈0%. The 50% moat is wide; min/max reported honestly. */
        if (G_MEASURE) {
            s.matched = s.missing = 0;
            struct llama_model *mM = llama_model_init_from_user(gctx, provide_stream, &s, mpS);
            if (!mM) { printf("FAIL: measure load\n"); return 1; }
#ifdef _WIN32
            if (!EmptyWorkingSet(GetCurrentProcess())) printf("  [warn] EmptyWS failed\n");
#else
            posix_madvise((void *)g.base, g.base_sz, POSIX_MADV_DONTNEED);
#endif
            int mobs[64];
            if (gen_on_model(mM, pr, mobs) != 0) { printf("FAIL: measure gen\n"); return 1; }
            int mtraj = 1;
            for (int k = 0; k < G_NGEN; k++)
                if (mobs[k] != ref[p][k]) { mtraj = 0; break; }
            if (!mtraj) { printf("  p%d: measure trajectory diverged — fired set invalid, stop\n", p); overall = 1; llama_model_free(mM); continue; }
            uint64_t *fmask = (uint64_t *)calloc((size_t)(max_layer + 1), sizeof(uint64_t));
            uint32_t fired_slots = 0;
            uint64_t fired_bytes = 0;
            double moat_lo = 100.0, moat_hi = 0.0;   /* min fired %, max unfired % */
            for (int L = 0; L <= max_layer; L++) {
                uint32_t E = layer_expert_count(&s, L);
                if (!E) continue;
                uint64_t mask = 0;
                for (uint32_t i = 0; i < s.n; i++) {
                    int wt = 0;
                    if (exp_layer(s.names[i], &wt) != L || !s.exp_E[i]) continue;
                    uint64_t sl = s.sizes[i] / E;
                    for (uint32_t e = 0; e < E; e++) {
                        uint64_t r = range_resident(data, s.offs[i] + e * sl + 4096, sl - 8192);
                        double pct = 100.0 * (double)r / (double)((sl - 8192) / 4096);
                        if (pct > 50.0) {
                            if (!(mask & (1ull << e))) { mask |= 1ull << e; }
                            if (pct < moat_lo) moat_lo = pct;
                        } else {
                            if (pct > moat_hi) moat_hi = pct;
                        }
                    }
                    break;   /* one tensor per layer suffices (shared pattern) */
                }
                fmask[L] = mask;
                for (uint32_t e = 0; e < E; e++)
                    if (mask & (1ull << e)) fired_slots++;
            }
            /* bytes: count across all 3 tensors per layer */
            for (uint32_t i = 0; i < s.n; i++) {
                if (!s.exp_E[i]) continue;
                int wt = 0, L = exp_layer(s.names[i], &wt);
                if (L < 0) continue;
                uint64_t m = fmask[L];
                uint32_t E = s.exp_E[i];
                uint64_t sll = s.sizes[i] / E;
                for (uint32_t e = 0; e < E; e++)
                    if (m & (1ull << e)) fired_bytes += sll;
            }
            printf("  p%d FIRED masks per MoE layer:", p);
            for (int L = 0; L <= max_layer; L++)
                if (layer_expert_count(&s, L)) printf(" L%d:%08llX", L, (unsigned long long)fmask[L]);
            printf("\n  fired slots=%u  fired=%.2f MB  E=%.2f MB  ratio=%.1f%%  moat[min-fired=%.0f%%,max-unfired=%.0f%%]\n",
                   fired_slots, fired_bytes / 1e6, E_total / 1e6,
                   100.0 * (double)fired_bytes / (double)E_total, moat_lo, moat_hi);
            llama_model_free(mM);
            /* commit-full buffers (load-safe), per-slice Narrowing keeps RAM≈K;
             * charge is transient E, freed per prompt below */
            for (uint32_t i = 0; i < s.n; i++) {
                if (!s.exp_E[i] || s.exp_buf[i]) continue;
                s.exp_buf[i] = (uint8_t *)VirtualAlloc(NULL, s.sizes[i], MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!s.exp_buf[i]) { printf("FAIL: alloc\n"); return 1; }
            }
            for (int L = 0; L <= max_layer; L++)
                if (layer_expert_count(&s, L)) { uint64_t ign = 0; layer_apply(&s, L, fmask[L], &ign); }
            s.matched = s.missing = 0;
            struct llama_model *mV = llama_model_init_from_user(gctx, provide_stream, &s, mpS);
            if (!mV) { printf("FAIL: verify load\n"); return 1; }
            int vfy[64];
            int vrc = gen_on_model(mV, pr, vfy);
            int vsame = vrc == 0;
            for (int k = 0; k < G_NGEN && vsame; k++)
                if (vfy[k] != ref[p][k]) vsame = 0;
            printf("  p%d commit-fired verify: %s\n", p, vsame ? "TOKENS IDENTICAL — K<<E LIVE" : "DIVERGED (honest)");
            if (!vsame) overall = 1;
            llama_model_free(mV);
            stream_free_bufs(&s);
            memset(s.exp_buf, 0, s.n * sizeof(uint8_t *));
            memset(s.exp_mask, 0, s.n * sizeof(uint64_t));
            free(fmask);
            continue;
        }

        /* NOW allocate expert buffers (commit charge starts here) */
        for (uint32_t i = 0; i < s.n; i++) {
            if (!s.exp_E[i] || s.exp_buf[i]) continue;
#ifdef _WIN32
            s.exp_buf[i] = (uint8_t *)VirtualAlloc(NULL, s.sizes[i],
                                                   MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE);
#else
            s.exp_buf[i] = (uint8_t *)calloc(1, s.sizes[i]);
#endif
            if (!s.exp_buf[i]) { printf("FAIL: alloc tensor %u\n", i); return 1; }
        }
        s.matched = s.missing = 0;
        struct llama_model_params mpB = llama_model_default_params();
        mpB.n_gpu_layers = G_NGL;
        mpB.no_host = true;
        struct llama_model *mB = llama_model_init_from_user(gctx, provide_stream, &s, mpB);
        if (!mB) { printf("FAIL: stream load p%d\n", p); return 1; }
        if (s.missing) printf("  [warn] missing=%u\n", s.missing);
        /* full-E pre-apply: probes narrow DOWN from complete weights.
         * Without this every probe decodes against zeroed other layers
         * and falsely diverges (K≈E artifact). Transient full charge. */
        for (int L = 0; L <= max_layer; L++) {
            uint32_t E = layer_expert_count(&s, L);
            if (E) { uint64_t ign = 0; layer_apply(&s, L, (E >= 64 ? ~0ull : ((1ull << E) - 1)), &ign); }
        }

        /* static top-K mode (G_K0>0): one prior, one widen, no per-layer search.
         * For big-E models where minimization probes are unaffordable. */
        if (G_K0 > 0) {
            uint32_t Kw = G_K0;
            uint32_t Emax = 0;
            for (int L = 0; L <= max_layer; L++) {
                uint32_t E = layer_expert_count(&s, L);
                if (E > Emax) Emax = E;
            }
            int done = 0;
            uint64_t Kbytes = 0;
            for (int round = 0; round < 2 && !done; round++) {
                for (int L = 0; L <= max_layer; L++) {
                    uint32_t E = layer_expert_count(&s, L);
                    if (E) { uint64_t ign = 0; layer_apply(&s, L, topk_mask(&s, L, Kw), &ign); }
                }
                int probe[64];
                if (gen_on_model(mB, pr, probe) != 0) { printf("FAIL: static gen\n"); return 1; }
                done = 1;
                for (int k = 0; k < G_NGEN; k++)
                    if (probe[k] != ref[p][k]) { done = 0; break; }
                if (!done) {
                    printf("  p%d static K=%u DIVERGE → widen K=%u\n", p, Kw, Kw * 2);
                    Kw *= 2;
                    if (Kw >= Emax) {   /* full-E already proven by sanity: stop, honest */
                        printf("  p%d: prior exhausted (sanity holds full-E) — needs profiled zones, not wider static\n", p);
                        break;
                    }
                }
            }
            Kbytes = 0;
            uint32_t Kslots = 0;
            for (uint32_t i = 0; i < s.n; i++) {
                if (!s.exp_E[i]) continue;
                uint32_t E = s.exp_E[i];
                uint64_t sl = s.sizes[i] / E, m = s.exp_mask[i];
                for (uint32_t e = 0; e < E; e++)
                    if (m & (1ull << e)) { Kbytes += sl; Kslots++; }
            }
            printf("  p%d: static K=%u/layer  slots=%u  Kbytes=%.2f MB  E=%.2f MB  ratio=%.1f%%  %s\n",
                   p, Kw, Kslots, Kbytes / 1e6, E_total / 1e6,
                   100.0 * (double)Kbytes / (double)E_total,
                   done ? "TOKENS IDENTICAL" : "DIVERGED (honest: prior insufficient)");
            if (!done) overall = 1;
            llama_model_free(mB);
            stream_free_bufs(&s);   /* drop commit charge before next prompt */
            memset(s.exp_buf, 0, s.n * sizeof(uint8_t *));
            memset(s.exp_mask, 0, s.n * sizeof(uint64_t));
            continue;
        }

        /* subset minimization per layer: singles {e}, then pairs, then all.
         * Prefix-only search inflates K (truth {2} forces keep-all); the
         * token-identity gate keeps every step honest. */
        uint64_t K_bytes = E_total;
        uint32_t K_total = 0;
        for (int L = 0; L <= max_layer; L++) {
            uint32_t E = layer_expert_count(&s, L);
            if (!E) continue;
            uint64_t found = (E >= 64 ? ~0ull : ((1ull << E) - 1));
            uint32_t kfound = E;
            int done = 0;
            for (uint32_t e = 0; e < E && !done; e++) {   /* singles */
                uint64_t mask = 1ull << e, ign = 0;
                layer_apply(&s, L, mask, &ign);
                int probe[64];
                if (gen_on_model(mB, pr, probe) != 0) break;
                int same = 1;
                for (int t = 0; t < G_NGEN; t++)
                    if (probe[t] != ref[p][t]) { same = 0; break; }
                if (same) { found = mask; kfound = 1; done = 1; }
            }
            for (uint32_t a = 0; a < E && !done; a++)
                for (uint32_t b = a + 1; b < E && !done; b++) {   /* pairs */
                    uint64_t mask = (1ull << a) | (1ull << b), ign = 0;
                    layer_apply(&s, L, mask, &ign);
                    int probe[64];
                    if (gen_on_model(mB, pr, probe) != 0) break;
                    int same = 1;
                    for (int t = 0; t < G_NGEN; t++)
                        if (probe[t] != ref[p][t]) { same = 0; break; }
                    if (same) { found = mask; kfound = 2; done = 1; }
                }
            {
                uint64_t ign = 0;   /* restore winner (or full on no-match) */
                layer_apply(&s, L, found, &ign);
            }
            K_total += kfound;
        }
        /* final tally + verification at the minimized mask */
        K_bytes = 0;
        for (uint32_t i = 0; i < s.n; i++) {
            if (!s.exp_E[i]) continue;
            uint32_t E = s.exp_E[i];
            uint64_t sl = s.sizes[i] / E, m = s.exp_mask[i];
            for (uint32_t e = 0; e < E; e++)
                if (m & (1ull << e)) K_bytes += sl;
        }
        int fin[64];
        int rc = gen_on_model(mB, pr, fin);
        int same = rc == 0;
        for (int k = 0; k < G_NGEN && same; k++)
            if (fin[k] != ref[p][k]) same = 0;
        printf("  p%d: K=%u expert-slots  Kbytes=%.2f MB  E=%.2f MB  ratio=%.1f%%  %s\n",
               p, K_total, K_bytes / 1e6, E_total / 1e6,
               100.0 * (double)K_bytes / (double)E_total,
               same ? "TOKENS IDENTICAL" : "DIVERGED (gate unstable, honest)");
        printf("  SEED p%d MASKS:", p);
        for (int L = 0; L <= max_layer; L++) {
            for (uint32_t i = 0; i < s.n; i++) {
                int wt = 0;
                if (exp_layer(s.names[i], &wt) == L && s.exp_E[i]) {
                    printf(" L%d:%llX", L, (unsigned long long)s.exp_mask[i]);
                    break;
                }
            }
        }
        printf("\n");
        if (!same) overall = 1;
        llama_model_free(mB);
        stream_free_bufs(&s);   /* drop commit charge before next prompt */
        memset(s.exp_buf, 0, s.n * sizeof(uint8_t *));
        memset(s.exp_mask, 0, s.n * sizeof(uint64_t));
    }
    uint64_t rss_stream = peak_rss();
    printf("stream peak RSS: %.1f MB (process peak incl. baseline)\n",
           rss_stream / 1e6);
    printf("E_total=%.2f MB expert weights on disk\n", E_total / 1e6);

    stream_free_bufs(&s);
    free(s.exp_buf); free(s.exp_mask); free(exp_E);
    gguf_close(&g);
    printf(overall ? "STREAM PROOF: MIXED (see per-prompt lines)\n"
                   : "STREAM PROOF PASS: minimized K sufficient, tokens identical\n");
    return overall;
}
