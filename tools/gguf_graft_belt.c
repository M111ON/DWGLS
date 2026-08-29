/*
 * tools/gguf_graft_belt.c — the +37 belt as the SERIAL ORDER of the model's
 * output stream (step ⑤ of the graft chain)
 *
 *   ④ field   = the field MATERIALIZES the model (body baked in window chain)
 *   ⑤ belt    = the model's OUTPUT (logits + token stream) goes BACK into the
 *                field, laid out in +37 belt order, and comes back bitwise
 *
 * Flow:
 *   1. bake model body into the field (window chain, inference order) — same
 *      as gguf_graft_field; rebuild header; write the graft GGUF
 *   2. real llama.cpp generation on the graft vs on the ORIGINAL, capturing
 *      BOTH the token stream and the FULL logits vector at EVERY step
 *      → token streams equal + logits BITWISE identical (multi-step proof)
 *   3. embed the graft's captured output sequence into the field via the
 *      +37 belt (test_tess_belt serial order):
 *        token stream  (n × int32)  → one window, belt order (n·4 ≤ 20736)
 *        logits stream (n × vocab × f32) → window chain, belt order within
 *        every window; read back in the same +37 walk → BITWISE identical
 *   4. belt invariants: full 20736-cycle (no collision → no overwrite) and
 *      enter-anywhere (read from another start = rotated stream)
 *
 * BUILD (same DLLs as graft-field) / RUN:
 *   make graft-belt
 *   PATH="I:/llama/llama-b9733-bin-win-vulkan-x64:$PATH" \
 *     ./build/gguf_graft_belt [model.gguf] [prompt] [n_tokens]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"
#include "ggml-backend.h"
#include "../core/gguf_box.h"
#include "../core/geo_belt.h"

/* P2: WIN/BELT_STRIDE/belt_addr now live in core/geo_belt.h (single source).
 * Local aliases keep the rest of this file readable. */
#define WIN         BELT_WIN
#define ALIGN       32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))

/* ── wall clock (QueryPerformanceCounter on Windows, CLOCK_MONOTONIC else) ── */
static double now_sec(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    static int init = 0;
    if (!init) { QueryPerformanceFrequency(&f); init = 1; }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* ── silence llama.cpp info spam; keep errors/warnings ── */
static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
        fputs(text, stderr);
}

/* ── inference order (same as window_chain / real_gate / graft_field) ── */
static int cat_of(const char *name, unsigned *block) {
    *block = 0;
    if (strncmp(name, "token_embd", 10) == 0) return 0;
    if (strncmp(name, "blk.", 4) == 0) { *block = (unsigned)atoi(name + 4); return 1; }
    if (strncmp(name, "output_norm", 11) == 0) return 2;
    return 3;
}
static void sort_inference(const GGUFBox *box, uint32_t *order, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++) {
            unsigned ba = 0, bb = 0;
            int ca = cat_of(box->entries[order[i]].name, &ba);
            int cb = cat_of(box->entries[order[j]].name, &bb);
            int less = (ca < cb) || (ca == cb && (ba < bb || (ba == bb && order[i] < order[j])));
            if (!less) { uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }
        }
}

/* ── header walk: skip KV verbatim, return where tensor infos start ── */
static int header_kv_end(const uint8_t *base, size_t sz, const uint8_t **kv_end) {
    if (sz < 24) return -1;
    const uint8_t *p = base + 24, *end = base + sz;
    uint64_t n_kv;
    memcpy(&n_kv, base + 16, 8);
    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen; uint32_t vtype;
        if ((size_t)(end - p) < 8) return -1;
        memcpy(&klen, p, 8); p += 8;
        if ((size_t)(end - p) < klen) return -1;
        p += klen;
        if ((size_t)(end - p) < 4) return -1;
        memcpy(&vtype, p, 4); p += 4;
        if (vtype == 9) {
            uint32_t at; uint64_t narr;
            if ((size_t)(end - p) < 12) return -1;
            memcpy(&at, p, 4); p += 4;
            memcpy(&narr, p, 8); p += 8;
            if (at == 8) {
                for (uint64_t a = 0; a < narr; a++) {
                    uint64_t sl;
                    if ((size_t)(end - p) < 8) return -1;
                    memcpy(&sl, p, 8); p += 8;
                    if ((size_t)(end - p) < sl) return -1;
                    p += sl;
                }
            } else if (at < 13) {
                if ((size_t)(end - p) < (size_t)vsz[at] * narr) return -1;
                p += (size_t)vsz[at] * narr;
            } else return -1;
        } else if (vtype == 8) {
            uint64_t sl;
            if ((size_t)(end - p) < 8) return -1;
            memcpy(&sl, p, 8); p += 8;
            if ((size_t)(end - p) < sl) return -1;
            p += sl;
        } else if (vtype <= 12) {
            if ((size_t)(end - p) < (size_t)vsz[vtype]) return -1;
            p += vsz[vtype];
        } else return -1;
    }
    *kv_end = p;
    return 0;
}

/* ── rebuild header: KV verbatim + tensor infos in CHAIN order ── */
static size_t rebuild_header(const GGUFBox *box, const uint32_t *order,
                             const uint64_t *chain_off, uint8_t *hdr,
                             size_t hdr_cap, size_t *data_offset_out) {
    const uint8_t *kv_end = NULL;
    if (header_kv_end(box->reader.base, box->reader.base_sz, &kv_end) != 0)
        return 0;
    size_t kv_sz = (size_t)(kv_end - box->reader.base);
    if (kv_sz > hdr_cap) return 0;
    memcpy(hdr, box->reader.base, kv_sz);
    size_t pos = kv_sz;

    for (uint32_t r = 0; r < box->n_tensors; r++) {
        const GGUFBoxEntry *e = &box->entries[order[r]];
        uint64_t nlen = strlen(e->name);
        size_t need = 8 + (size_t)nlen + 4 + (size_t)e->n_dims * 8 + 4 + 8;
        if (pos + need > hdr_cap) return 0;
        memcpy(hdr + pos, &nlen, 8); pos += 8;
        memcpy(hdr + pos, e->name, nlen); pos += nlen;
        uint32_t nd = e->n_dims;
        memcpy(hdr + pos, &nd, 4); pos += 4;
        for (uint32_t d = 0; d < nd; d++) {
            int64_t dv = (int64_t)e->dims[d];
            memcpy(hdr + pos, &dv, 8); pos += 8;
        }
        memcpy(hdr + pos, &e->dtype, 4); pos += 4;
        memcpy(hdr + pos, &chain_off[r], 8); pos += 8;
    }
    size_t data_off = align32(pos);
    if (data_off > hdr_cap) return 0;
    memset(hdr + pos, 0, data_off - pos);
    *data_offset_out = data_off;
    return data_off;
}

/* ── captured generation: token stream + FULL logits at every step ── */
typedef struct {
    llama_token *toks;
    int          n;         /* steps actually taken */
    uint32_t     n_vocab;
    float       *logits;    /* n × n_vocab floats, memcpy'd per step */
    double       decode_ms; /* total llama_decode wall time (the real cost) */
} Capture;

static Capture *capture_generate(const char *gguf_path, const char *prompt,
                                 int n_gen) {
    Capture *c = NULL;
    llama_token *toks = NULL, *out = NULL;
    float *lg = NULL;
    struct llama_model *model = NULL;
    struct llama_context *ctx = NULL;
    int total = 0, n_prompt = 0;
    double dms = 0.0;
    uint32_t n_vocab = 0;

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    model = llama_model_load_from_file(gguf_path, mp);
    if (!model) return NULL;
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.n_threads = 4; cp.n_threads_batch = 4;
    ctx = llama_init_from_model(model, cp);
    if (!ctx) goto fail;
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    n_vocab = (uint32_t)llama_vocab_n_tokens(vocab);
    llama_token eos = llama_vocab_eos(vocab);

    n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    if (n_prompt < 0) n_prompt = -n_prompt;
    toks = (llama_token *)malloc((size_t)(n_prompt + 1) * sizeof(llama_token));
    if (!toks) goto fail;
    int n2 = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, n_prompt, true, false);
    if (n2 < 0) n2 = -n2;
    n_prompt = n2;

    out = (llama_token *)malloc((size_t)(n_gen + 1) * sizeof(llama_token));
    lg  = (float *)malloc((size_t)n_gen * (size_t)n_vocab * sizeof(float));
    if (!out || !lg) goto fail;

    {
        double t0 = now_sec();
        int rc = llama_decode(ctx, llama_batch_get_one(toks, n_prompt));
        dms += (now_sec() - t0) * 1e3;
        if (rc != 0) goto fail;
    }
    for (int i = 0; i < n_gen; i++) {
        const float *logits = (i == 0) ? llama_get_logits_ith(ctx, n_prompt - 1)
                                       : llama_get_logits(ctx);
        memcpy(lg + (size_t)total * n_vocab, logits, (size_t)n_vocab * sizeof(float));
        llama_token best = 0; float best_v = logits[0];
        for (uint32_t t = 1; t < n_vocab; t++) if (logits[t] > best_v) { best_v = logits[t]; best = (llama_token)t; }
        out[total++] = best;
        if (best == eos) break;
        double t1 = now_sec();
        int rc = llama_decode(ctx, llama_batch_get_one(&best, 1));
        dms += (now_sec() - t1) * 1e3;
        if (rc != 0) break;
    }
    c = (Capture *)calloc(1, sizeof(Capture));
    if (!c) goto fail;
    c->toks = out; c->n = total; c->n_vocab = n_vocab; c->logits = lg;
    c->decode_ms = dms;
    /* ownership transferred to c; prevent fail-label free */
    out = NULL; lg = NULL;
fail:
    free(toks);
    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
    /* free only on the failure path — on success out/lg belong to c */
    free(out);
    free(lg);
    return c;
}

static void stream_text(const char *gguf_path, llama_token *toks, int n) {
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *m = llama_model_load_from_file(gguf_path, mp);
    if (!m) return;
    const struct llama_vocab *vocab = llama_model_get_vocab(m);
    char buf[64];
    for (int i = 0; i < n; i++) {
        int k = llama_token_to_piece(vocab, toks[i], buf, sizeof(buf), 0, false);
        if (k < 0) k = 0;
        fwrite(buf, 1, (size_t)k, stdout);
    }
    llama_model_free(m);
}

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(int argc, char **argv) {
    /* P0: env-var fallbacks (DWGLS_GGUF / DWGLS_LLAMA_DLL) — no more
     * machine-locked hardcoded paths. The legacy I:/... values survive as
     * last-resort defaults so the original dev box still runs unconfigured. */
    const char *gguf_def = getenv("DWGLS_GGUF");
    if (!gguf_def || !*gguf_def) gguf_def = "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *dll_def = getenv("DWGLS_LLAMA_DLL");
    if (!dll_def || !*dll_def) dll_def = "I:/llama/llama-b9733-bin-win-vulkan-x64";
    const char *gguf   = (argc > 1) ? argv[1] : gguf_def;
    const char *prompt = (argc > 2) ? argv[2] : "The capital of France is";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 40;
    if (n_gen <= 0) n_gen = 40;
    const char *out_path = "build/graft_belt.gguf";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("The +37 belt as the SERIAL ORDER of the model's output stream\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    llama_backend_init();
    llama_log_set(quiet_log, NULL);
    ggml_backend_load_all_from_path(dll_def);

    GGUFBox box;
    if (gguf_box_open(&box, gguf) != 0) {
        printf("(cannot open %s — model/DLLs missing on this machine)\n", gguf);
        llama_backend_free();
        return 1;
    }
    uint32_t N = box.n_tensors;

    /* ── step ①: bake model body into the field (chain order) ── */
    uint32_t *order = (uint32_t *)calloc(N, sizeof(uint32_t));
    uint64_t *chain_off = (uint64_t *)calloc(N, sizeof(uint64_t));
    if (!order || !chain_off) { printf("(cannot allocate order/chain_off)\n"); return 1; }
    sort_inference(&box, order, N);

    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < N; i++) total_bytes += align32(box.entries[i].size);
    uint64_t n_win = (total_bytes + WIN - 1) / WIN;
    uint64_t field_sz = n_win * WIN;
    uint8_t *field = (uint8_t *)calloc(1, (size_t)field_sz);
    if (!field) { printf("(cannot allocate %llu-byte field)\n", (unsigned long long)field_sz); return 1; }

    printf("\n═ ① BAKE — model body → KIS field (chain order) ═\n");
    uint64_t cursor = 0;
    int lossless = 1;
    for (uint32_t r = 0; r < N; r++) {
        const GGUFBoxEntry *e = &box.entries[order[r]];
        chain_off[r] = cursor;
        memcpy(field + cursor, e->data, e->size);
        if (memcmp(field + cursor, e->data, e->size) != 0) lossless = 0;
        cursor += align32(e->size);
    }
    CHECK("F1: bake lossless — field bytes == source bytes ทุก tensor (memcmp)", lossless);
    printf("  field: %llu windows × %u B = %.1f MB\n",
           (unsigned long long)n_win, WIN, (double)field_sz / 1048576.0);

    size_t hdr_cap = (size_t)box.reader.data_offset + 4096;
    uint8_t *hdr = (uint8_t *)calloc(1, hdr_cap);
    if (!hdr) { printf("(cannot allocate %zu-byte header buffer)\n", hdr_cap); return 1; }
    size_t data_off = rebuild_header(&box, order, chain_off, hdr, hdr_cap, &data_off);
    CHECK("F2: rebuilt header (KV verbatim + tensor infos in chain order)", data_off > 0);

    FILE *f = fopen(out_path, "wb");
    if (!f || fwrite(hdr, 1, data_off, f) != data_off ||
        fwrite(field, 1, (size_t)cursor, f) != (size_t)cursor) {
        printf("(cannot write %s)\n", out_path);
        return 1;
    }
    fclose(f);
    printf("  graft written: %s (header %zu B + body from field %llu B)\n",
           out_path, data_off, (unsigned long long)cursor);
    CHECK("F3: field-built GGUF (graft ตัวจริง) exists", 1);
    {
        int differs = 0;
        const uint8_t *src_body = box.reader.base + box.reader.data_offset;
        size_t src_body_sz = box.reader.base_sz - (size_t)box.reader.data_offset;
        size_t cmp = (size_t)cursor < src_body_sz ? (size_t)cursor : src_body_sz;
        for (size_t i = 0; i < cmp; i++)
            if (field[i] != src_body[i]) { differs = 1; break; }
        CHECK("F4: body-from-field ≠ source body (chain reorder จริง — ไม่ใช่ memcpy)", differs);
    }

    /* ── step ②: real generation — graft vs original, capture logits ── */
    printf("\nprompt: \"%s\"  (generate up to %d tokens)\n\n", prompt, n_gen);
    Capture *g = capture_generate(out_path, prompt, n_gen);
    Capture *o = capture_generate(gguf,     prompt, n_gen);

    int t_ok = (g && o && g->n == o->n);
    if (t_ok) for (int i = 0; i < g->n; i++) if (g->toks[i] != o->toks[i]) { t_ok = 0; break; }
    printf("  field-built: %d tokens\n  original:    %d tokens\n", g ? g->n : -1, o ? o->n : -1);
    CHECK("T1: graft token stream == original token stream (multi-token)", t_ok);

    int l_ok = (g && o && g->n == o->n && g->n_vocab == o->n_vocab);
    if (l_ok)
        l_ok = memcmp(g->logits, o->logits,
                      (size_t)g->n * (size_t)g->n_vocab * sizeof(float)) == 0;
    CHECK("T2: graft per-step logits == original logits BITWISE (ทุก step, memcmp เต็มเวกเตอร์)",
          l_ok);
    if (t_ok) {
        printf("  generated text (field-built): \"");
        stream_text(out_path, g->toks, g->n);
        printf("\"\n");
    }

    if (!g) { printf("(generation failed — aborting belt section)\n"); return 1; }

    /* ── step ③: embed the graft's OUTPUT on the +37 belt ── */
    printf("\n═ ③ OUTPUT → +37 BELT (serial order) → read back ═\n");

    /* token stream = n × int32 (little-endian, as captured) */
    size_t tok_bytes = (size_t)g->n * sizeof(llama_token);
    uint8_t *tok_stream = (uint8_t *)g->toks;
    CHECK("T3a: token stream fits one belt window (n·4 ≤ 20736)", tok_bytes <= WIN);

    /* embed into one window in +37 belt order, read back, compare */
    {
        uint8_t *win = (uint8_t *)calloc(1, WIN);
        if (!win) { printf("(cannot allocate belt window)\n"); return 1; }
        for (size_t k = 0; k < tok_bytes; k++) win[belt_addr(0u, (uint32_t)k)] = tok_stream[k];
        int ok = 1;
        for (size_t k = 0; k < tok_bytes; k++)
            if (win[belt_addr(0u, (uint32_t)k)] != tok_stream[k]) { ok = 0; break; }
        CHECK("T3b: token stream roundtrip via belt BITWISE (ทุก byte ของทุก token)",
              ok);
        free(win);
    }

    /* logits stream = n × n_vocab × f32 → window chain, belt order within */
    {
        size_t n_log = (size_t)g->n * (size_t)g->n_vocab * sizeof(float);
        size_t n_lw = (n_log + WIN - 1) / WIN;
        uint8_t *lf = (uint8_t *)calloc(1, n_lw * WIN);
        if (!lf) { printf("(cannot allocate logits field for belt)\n"); return 1; }
        for (size_t w = 0; w < n_lw; w++)
            for (uint32_t j = 0; j < WIN; j++) {
                size_t src = w * WIN + j;
                uint8_t b = (src < n_log) ? ((uint8_t *)g->logits)[src] : 0;
                lf[w * WIN + belt_addr(0u, j)] = b;
            }
        int ok = 1;
        for (size_t w = 0; w < n_lw && ok; w++)
            for (uint32_t j = 0; j < WIN; j++) {
                size_t src = w * WIN + j;
                uint8_t b = (src < n_log) ? ((uint8_t *)g->logits)[src] : 0;
                if (lf[w * WIN + belt_addr(0u, j)] != b) { ok = 0; break; }
            }
        CHECK("T4: logits stream roundtrip via belt BITWISE (ทุก step × ทุก vocab × f32)",
              ok);
        free(lf);
    }

    /* ── step ④: belt invariants ── */
    {
        uint8_t *seen = (uint8_t *)calloc(1, WIN);   /* 20KB — heap, not stack */
        if (!seen) { printf("(cannot allocate belt-invariant seen)\n"); return 1; }
        int full = 1;
        for (uint32_t k = 0; k < WIN; k++) {
            uint32_t a = belt_addr(0u, k);
            if (seen[a]) { full = 0; break; }
            seen[a] = 1;
        }
        free(seen);
        CHECK("T5: belt walk visits all 20736 slots exactly once (gcd(37,20736)=1 — no collision, no overwrite)",
              full);
    }
    {
        /* enter anywhere: embed at start s1, read at s2 → rotated stream
         * read pos k returns slot s2+37k which holds stream byte k′ with
         * s2+37k ≡ s1+37k′ → k′ = k + Δ, Δ = (s2−s1)·37⁻¹ mod 20736 */
        uint32_t s1 = 11u, s2 = 5000u;
        int64_t d = (int64_t)s2 - (int64_t)s1;
        if (d < 0) d += (int64_t)WIN;
        /* 37⁻¹ mod 20736 via extended Euclid: s0·37 + t0·20736 = gcd = 1.
         * P3: inner coeff renamed s_coeff (was s1_) to avoid shadowing s1. */
        int64_t r0 = 37, r1 = (int64_t)WIN, s0 = 1, s_coeff = 0, t0 = 0, t1 = 1;
        while (r1) {
            int64_t q = r0 / r1;
            int64_t nr = r0 - q * r1; r0 = r1; r1 = nr;
            int64_t ns = s0 - q * s_coeff; s0 = s_coeff; s_coeff = ns;
            int64_t nt = t0 - q * t1;  t0 = t1;  t1 = nt;
        }
        int64_t inv = s0; if (inv < 0) inv += (int64_t)WIN;
        /* sanity: 37·inv ≡ 1 (mod 20736) */
        int inv_ok = ((37 * (inv % 20736)) % 20736 == 1);
        CHECK("T6a: 37⁻¹ mod 20736 ถูกต้อง (extended Euclid — 37·16813 ≡ 1)",
              inv_ok && inv == 16813);
        int64_t delta = (d * inv) % (int64_t)WIN;
        if (delta < 0) delta += (int64_t)WIN;
        size_t n = tok_bytes;
        uint8_t *win = (uint8_t *)calloc(1, WIN);
        if (!win) { printf("(cannot allocate belt enter-anywhere window)\n"); return 1; }
        for (size_t k = 0; k < n; k++) win[belt_addr(s1, (uint32_t)k)] = tok_stream[k];
        int ok = 1;
        for (size_t k = 0; k < n && ok; k++) {
            int64_t kk = (int64_t)k + delta;
            uint8_t expect = (kk < (int64_t)n) ? tok_stream[kk] : 0;
            if (win[belt_addr(s2, (uint32_t)k)] != expect) ok = 0;
        }
        CHECK("T6b: enter anywhere — อ่านจาก start อื่น = stream หมุน Δ ก้าว (ไม่มี origin, deterministic)",
              ok);
        free(win);
    }

    /* ── step ⑤: LOCALITY — linear cursor vs +37 belt on the real stream ── */
    {
        size_t n_log = (size_t)g->n * (size_t)g->n_vocab * sizeof(float);
        size_t n_lw = (n_log + WIN - 1) / WIN;
        printf("\n═ ⑤ LOCALITY — linear cursor vs +37 belt (real logits %zu B, %zu windows) ═\n",
               n_log, n_lw);

        /* build both placements of the SAME stream */
        uint8_t *fl = (uint8_t *)calloc(1, n_lw * WIN);   /* linear: byte k → k */
        uint8_t *fb = (uint8_t *)calloc(1, n_lw * WIN);   /* belt:   byte k → w·WIN + (37j)%WIN */
        if (!fl || !fb) { printf("(cannot allocate locality buffers)\n"); return 1; }
        memcpy(fl, g->logits, n_log);
        for (size_t w = 0; w < n_lw; w++)
            for (uint32_t j = 0; j < WIN; j++) {
                size_t src = w * WIN + j;
                if (src < n_log) fb[w * WIN + belt_addr(0u, j)] = ((uint8_t *)g->logits)[src];
            }

        /* read-back sweeps: 8 independent accumulators break the dep chain.
         * REAL read paths: linear = byte cursor (increment); belt = walk
         * (slot += 37, one conditional subtract — NOT a % division per step) */
        const int NSW = 300;
        volatile uint32_t acc[8];
        memset((void *)acc, 0, sizeof(acc));
        double best_l = 1e9, best_b = 1e9, tot_l = 0, tot_b = 0;
        for (int it = 0; it < NSW; it++) {
            double t0 = now_sec();
            for (size_t w = 0; w < n_lw; w++) {
                size_t base = w * WIN, lim = n_log - base < WIN ? n_log - base : WIN;
                for (size_t j = 0; j < lim; j++) acc[j & 7] ^= fl[base + j];
            }
            double d = (now_sec() - t0) * 1e3;
            if (d < best_l) best_l = d; tot_l += d;

            t0 = now_sec();
            for (size_t w = 0; w < n_lw; w++) {
                size_t base = w * WIN, lim = n_log - base < WIN ? n_log - base : WIN;
                uint32_t slot = 0;
                for (size_t j = 0; j < lim; j++) {
                    acc[j & 7] ^= fb[base + slot];
                    slot += BELT_STRIDE;
                    if (slot >= WIN) slot -= WIN;   /* slot+37 < 2·WIN → one subtract */
                }
            }
            d = (now_sec() - t0) * 1e3;
            if (d < best_b) best_b = d; tot_b += d;
        }

        /* distinct cache lines (64 B) touched per full sweep, both placements */
        uint8_t *lset = (uint8_t *)calloc(1, (n_lw * WIN) / 64);
        uint8_t *bset = (uint8_t *)calloc(1, (n_lw * WIN) / 64);
        if (!lset || !bset) { printf("(cannot allocate cache-line sets)\n"); return 1; }
        size_t nl = 0, nb = 0;
        size_t n_full_win = n_log / WIN;   /* full windows: both cover all 324 lines */
        for (size_t w = 0; w < n_lw; w++) {
            size_t base = w * WIN, lim = n_log - base < WIN ? n_log - base : WIN;
            for (size_t j = 0; j < lim; j++) {
                size_t a = base + j;                         /* linear slot */
                size_t b = base + belt_addr(0u, (uint32_t)j); /* belt slot   */
                if (!lset[a / 64]) { lset[a / 64] = 1; nl++; }
                if (!bset[b / 64]) { bset[b / 64] = 1; nb++; }
            }
        }
        size_t lin_tail = nl - n_full_win * 324;   /* ragged tail: contiguous → 112 */
        size_t belt_tail = nb - n_full_win * 324;  /* tail: walk spread over window */
        free(lset); free(bset);

        double lin_mbs   = (double)n_log / (best_l * 1e-3) / 1048576.0;
        double belt_mbs  = (double)n_log / (best_b * 1e-3) / 1048576.0;
        double win_per_s = (double)n_lw / (best_l * 1e-3);
        printf("  linear: best %8.3f ms/sweep  %8.0f MB/s  %8.0f windows/s\n",
               best_l, lin_mbs, win_per_s);
        printf("  belt:   best %8.3f ms/sweep  %8.0f MB/s  %8.0f windows/s\n",
               best_b, belt_mbs, (double)n_lw / (best_b * 1e-3));
        printf("  (avg over %d sweeps: linear %.3f ms, belt %.3f ms — belt/linear %.2f×)\n",
               NSW, tot_l / NSW, tot_b / NSW, tot_b / tot_l);
        printf("  64B-lines/sweep: linear %zu | belt %zu  (full %zu windows: 324/324; tail: %zu vs %zu)\n",
               nl, nb, n_full_win, lin_tail, belt_tail);

        /* generation impact: per-step logits read-back vs the REAL decode cost */
        double step_read_l = best_l * ((double)g->n_vocab * 4.0 / (double)n_log);
        double step_read_b = best_b * ((double)g->n_vocab * 4.0 / (double)n_log);
        double decode_per  = g->decode_ms / (double)g->n;
        printf("  per-step logits read-back: linear %.3f ms | belt %.3f ms  vs  real llama_decode %.0f ms/step\n",
               step_read_l, step_read_b, decode_per);
        printf("  → placement impact on generation: linear %.4f%% | belt %.4f%% of decode\n",
               step_read_l / decode_per * 100.0, step_read_b / decode_per * 100.0);

        CHECK("L1: full windows — both placements cover ครบ 324 cache lines/window",
              nl >= n_full_win * 324 && nb >= n_full_win * 324 &&
              nl == (n_log + 63) / 64 && nb - n_full_win * 324 <= 324);
        CHECK("L2: belt read-back ≤ 2× linear (stride-37 prefetch ไม่เต็มที่ — ราคาจริงของการสลับลำดับ)",
              best_b <= best_l * 2.0 + 0.001);
        CHECK("L3: read-back เป็นเศษเล็กน้อยของ decode (< 1% ต่อ step)",
              step_read_b / decode_per < 0.01);
        free(fl); free(fb);
    }

    free(g->toks); free(g->logits); free(g);
    if (o) { free(o->toks); free(o->logits); free(o); }
    if (remove(out_path) != 0)
        printf("(warn: could not remove %s)\n", out_path);
    gguf_box_close(&box);
    llama_backend_free();

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS%s\n", pass_count, pass_count + fail_count,
           fail_count ? "" : " — the belt carries the model's output bitwise");
    return fail_count ? 1 : 0;
}
