/*
 * tools/dual_lazy_serve.c — two models, one process, per-model fields.
 *
 * Phase 1 of multi-model field sharing: each model keeps its OWN field file
 * (fieldA.bin / fieldB.bin) with its own mmap — so per-model eviction uses
 * the proven whole-view unmap+remap mechanism (Windows cannot evict
 * sub-ranges of a mapped file; a single shared file would make per-model
 * evict impossible — documented, not attempted).
 *
 * What this proves (gates M1..M3, M5):
 *   M1: both lazy models live SIMULTANEOUSLY, both bitwise vs reference
 *   M2: evict A's body -> B re-generates bitwise identical (isolation:
 *       A's unmap+remap never touches B's mapping or B's model buffers)
 *   M3: reload A -> A re-generates bitwise identical (recovery, B stays loaded)
 *   M4: tokenizer payload analysis (info only): byte-identical across models?
 *       -> quantifies what a shared-tokenizer layout would save (~5.9 MB)
 *   M5: dual WS peak bounded by refA + refB + 1024 MB
 *
 * Reuses gguf_lazy_serve.c logic verbatim (bake/rebuild/serve/evict);
 * the single-model tool is untouched (still green, still the reference).
 *
 * BUILD / RUN: make dual-lazy-serve
 *   ./build/dual_lazy_serve <ggufA> <ggufB> [prompt] [n_gen=10] [backend]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <windows.h>
#include <psapi.h>
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "../core/gguf_box.h"
#include "../core/win_cache.h"

#define WIN        20736u
#define ALIGN      32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))
#define align64(x) (((x) + 63u) & ~((uint64_t)63u))

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── working set (MB) ─────────────────────────────────────── */
static double g_peak_ws = 0, g_peak_priv = 0;
static void wss(const char *tag) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        double ws = pmc.WorkingSetSize / 1048576.0;
        double pr = pmc.PagefileUsage / 1048576.0;
        if (ws > g_peak_ws) g_peak_ws = ws;
        if (pr > g_peak_priv) g_peak_priv = pr;
        printf("  [ws] %-30s WS %8.1f MB   private %8.1f MB\n", tag, ws, pr);
    }
}

/* ── phase accounting: page faults + elapsed time ─────────── */
static DWORD64 g_faults = 0;
static LARGE_INTEGER g_t0, g_freq;
static void phase_start(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) g_faults = pmc.PageFaultCount;
    QueryPerformanceCounter(&g_t0);
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
}
static void phase_end(const char *tag) {
    PROCESS_MEMORY_COUNTERS pmc;
    DWORD64 f_now = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) f_now = pmc.PageFaultCount;
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    double dt = (double)(t1.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart;
    printf("  [faults] %-32s faults %10llu   time %8.2f s\n", tag,
           (unsigned long long)(f_now - g_faults), dt);
}

/* ── mmap residency: Valid-bit pages (4 KB) ────── */
static uint64_t field_resident(const uint8_t *base, uint64_t len) {
    uint64_t npages = (len + 4095) / 4096;
    uint64_t got = 0;
    if (npages == 0) return 0;
    PSAPI_WORKING_SET_EX_INFORMATION *info =
        (PSAPI_WORKING_SET_EX_INFORMATION *)calloc((size_t)npages, sizeof(*info));
    if (!info) return 0;
    for (uint64_t i = 0; i < npages; i++) info[i].VirtualAddress = (PVOID)(base + i * 4096);
    if (QueryWorkingSetEx(GetCurrentProcess(), info, (DWORD)(npages * sizeof(*info))))
        for (uint64_t i = 0; i < npages; i++)
            if (info[i].VirtualAttributes.Flags & 1) got++;
    free(info);
    return got;
}
static void res_report(const char *tag, const uint8_t *base, uint64_t len) {
    uint64_t r = field_resident(base, len);
    printf("  [res]  %-30s %7llu / %7llu pages (%6.1f MB of %.1f MB)\n", tag,
           (unsigned long long)r, (unsigned long long)((len + 4095) / 4096),
           (double)r * 4096 / 1048576.0, (double)len / 1048576.0);
}

/* resident pages within a byte range [off, off+len) of the mmap */
static uint64_t range_resident(const uint8_t *base, uint64_t off, uint64_t len) {
    if (len == 0) return 0;
    uint64_t p0 = off / 4096, p1 = (off + len - 1) / 4096;
    uint64_t n = p1 - p0 + 1;
    uint64_t got = 0;
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

/* ── eviction for file-backed mmap (real): unmap + remap the view ── */
static DWORD wc_evict_body(const uint8_t *base, uint64_t body_off, uint64_t body_len,
                           HANDLE hm, uint64_t file_sz, const uint8_t **out_new_base) {
    (void)file_sz;
    if (body_len == 0 || !hm) return ERROR_INVALID_PARAMETER;
    uint64_t start = (body_off + 4095) / 4096 * 4096;
    uint64_t end = (body_off + body_len) / 4096 * 4096;
    if (end <= start) return ERROR_INVALID_PARAMETER;
    if (!UnmapViewOfFile(base)) return GetLastError();
    const uint8_t *fresh = (const uint8_t *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!fresh) return GetLastError();
    *out_new_base = fresh;
    return 0;
}

/* tensors generation never read fully (page residency per tensor) */
static void untouched_report(const GGUFBox *box, const uint8_t *base, uint64_t body_off,
                             const uint64_t *fpos, uint32_t N) {
    uint64_t n_partial = 0, bytes_unread = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint64_t off = body_off + fpos[i], len = box->entries[i].size;
        uint64_t np = (len + 4095) / 4096;
        uint64_t rp = range_resident(base, off, len);
        if (rp < np) {
            n_partial++;
            uint64_t unread = len - (rp * 4096 < len ? rp * 4096 : len);
            bytes_unread += unread;
        }
    }
    printf("  [unread] %llu tensors partially read (~%llu B never faulted in)\n",
           (unsigned long long)n_partial, (unsigned long long)bytes_unread);
}

/* ── inference order ──────────────────────────────────────── */
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

/* ── KV walk: read exactly n_kv entries starting at base+24 ── */
typedef struct { size_t start, end, val_start; char name[64]; int is_tok;
                 uint32_t arr_type; uint64_t arr_count; } KVInfo;
static int kv_walk(const uint8_t *base, KVInfo *infos, int cap, uint32_t *n_out) {
    uint64_t n_kv;
    memcpy(&n_kv, base + 16, 8);
    if (n_kv > (uint64_t)cap) return -1;
    const uint8_t *p = base + 24;
    uint32_t n = 0;
    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    for (uint64_t k = 0; k < n_kv; k++) {
        KVInfo *kv = &infos[n];
        kv->start = (size_t)(p - base);
        uint64_t klen; uint32_t vtype;
        memcpy(&klen, p, 8); p += 8;
        memcpy(kv->name, p, klen < 63 ? klen : 63); kv->name[klen < 63 ? klen : 63] = 0;
        p += klen;
        memcpy(&vtype, p, 4); p += 4;
        kv->val_start = (size_t)(p - base);
        kv->is_tok = (strcmp(kv->name, "tokenizer.ggml.tokens") == 0 ||
                     strcmp(kv->name, "tokenizer.ggml.merges") == 0 ||
                     strcmp(kv->name, "tokenizer.ggml.token_type") == 0);
        kv->arr_type = 0; kv->arr_count = 0;
        if (vtype == 9) {
            uint32_t at; uint64_t narr;
            memcpy(&at, p, 4); p += 4; memcpy(&narr, p, 8); p += 8;
            kv->arr_type = at; kv->arr_count = narr;
            if (at == 8) { for (uint64_t a = 0; a < narr; a++) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; } }
            else if (at < 13) p += (size_t)vsz[at] * narr;
            else return -1;
        } else if (vtype == 8) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; }
        else if (vtype <= 12) p += vsz[vtype];
        else return -1;
        kv->end = (size_t)(p - base);
        n++;
    }
    *n_out = n;
    return 0;
}

/* kis.* pointer keys written into the index header */
static const char *tok_names[3] = {
    "tokenizer.ggml.tokens", "tokenizer.ggml.merges", "tokenizer.ggml.token_type" };
static const char *tok_suffix[3] = { "tokens", "merges", "token_type" };
static const char *kis_key(int t, const char *field_) {
    static char buf[48];
    snprintf(buf, sizeof(buf), "kis.kv.%s.%s", tok_suffix[t], field_);
    return buf;
}

/* ── callback: serve tensor bytes from the field mmap ─────── */
typedef struct {
    const GGUFBox *box;
    const uint8_t *field;   /* mmap base */
    uint64_t body_off;      /* body start within field file */
    const uint64_t *fpos;   /* file-idx → body position */
    uint32_t matched, missing, aliased;
    uint64_t bytes_served;
    uint64_t n_windows;
    uint8_t *win_bits;
    uint64_t win_touched;
    uint64_t win_total;
    void * owned[256];
    uint32_t n_owned;
    win_cache_t wc;         /* kept for struct compat; never enabled (wc_tab=NULL) */
    wc_entry_t *wc_tab;
    uint64_t wc_victims;
} ServeCtx;

static void * fallback_data(ServeCtx *s, size_t n) {
    void *p = calloc(1, n);
    if (!p || s->n_owned >= 256) { free(p); return NULL; }
    s->owned[s->n_owned++] = p;
    return p;
}

static void touch_window(ServeCtx *s, uint64_t off, uint64_t len) {
    if (len == 0) return;
    uint64_t w0 = off / WIN;
    uint64_t w1 = (off + len - 1) / WIN;
    for (uint64_t w = w0; w <= w1; w++) {
        s->win_total++;
        if (w < s->n_windows && !(s->win_bits[w >> 3] & (1u << (w & 7)))) {
            s->win_bits[w >> 3] |= (1u << (w & 7));
            s->win_touched++;
        }
    }
}
static void win_reset(ServeCtx *s) {
    memset(s->win_bits, 0, (s->n_windows + 7) / 8);
    s->win_touched = 0; s->win_total = 0;
}
static void provide_tensor(struct ggml_tensor *t, void *ud) {
    ServeCtx *s = (ServeCtx *)ud;
    const char *name = ggml_get_name(t);
    for (uint32_t i = 0; i < s->box->n_tensors; i++) {
        if (strcmp(s->box->entries[i].name, name) == 0) {
            size_t nb = ggml_nbytes(t);
            if (nb == s->box->entries[i].size) {
                t->data = (void *)(s->field + s->body_off + s->fpos[i]);
                touch_window(s, s->body_off + s->fpos[i], nb);
                s->bytes_served += nb;
                s->matched++;
                return;
            }
        }
    }
    t->data = fallback_data(s, ggml_nbytes(t));
    if (t->data == NULL) { s->missing++; return; }
    if (strcmp(name, "output.weight") == 0) {
        for (uint32_t i = 0; i < s->box->n_tensors; i++)
            if (strcmp(s->box->entries[i].name, "token_embd.weight") == 0) {
                size_t n = ggml_nelements(t);
                if (n * 4 == ggml_nbytes(t) && s->box->entries[i].size == n / 32 * 34) {
                    touch_window(s, s->body_off + s->fpos[i], (uint64_t)(n / 32 * 34));
                    const uint8_t *src = s->field + s->body_off + s->fpos[i];
                    float *dst = (float *)t->data;
                    for (size_t k = 0; k < n / 32; k++) {
                        uint16_t h; memcpy(&h, src + k * 34, 2);
                        float d = ggml_fp16_to_fp32(h);
                        const int8_t *q = (const int8_t *)(src + k * 34 + 2);
                        for (int j = 0; j < 32; j++) dst[k * 32 + j] = (float)q[j] * d;
                    }
                    s->aliased++;
                    return;
                }
            }
    }
    if (strstr(name, ".bias")) memset(t->data, 0, ggml_nbytes(t));
    else if (strstr(name, "scale")) {
        float *fd = (float *)t->data;
        size_t nf = ggml_nbytes(t) / sizeof(float);
        for (size_t i = 0; i < nf; i++) fd[i] = 1.0f;
    } else {
        float *fd = (float *)t->data;
        size_t nf = ggml_nbytes(t) / sizeof(float);
        for (size_t i = 0; i < nf; i++) fd[i] = 1.0f;
    }
    s->missing++;
}

/* ── greedy generation from a loaded model ────────────────── */
static llama_token *generate_model(struct llama_model *model, const char *prompt,
                                   int n_gen, int *n_out) {
    *n_out = 0;
    struct llama_context_params cp = llama_context_default_params();
    cp.ctx_type = LLAMA_CONTEXT_TYPE_DEFAULT; /* explicit: never inherit DLL-version garbage */
    cp.n_ctx = 2048; cp.n_batch = 512; cp.n_threads = 8; cp.n_threads_batch = 8;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) return NULL;
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    llama_token eos = llama_vocab_eos(vocab);
    int np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    if (np < 0) np = -np;
    llama_token *toks = (llama_token *)malloc((size_t)(np + 1) * sizeof(llama_token));
    np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, np, true, false);
    if (np < 0) np = -np;
    llama_token *out = (llama_token *)malloc((size_t)(n_gen + 1) * sizeof(llama_token));
    int total = 0;
    if (llama_decode(ctx, llama_batch_get_one(toks, np)) != 0) { free(toks); free(out); llama_free(ctx); return NULL; }
    for (int i = 0; i < n_gen; i++) {
        const float *logits = (i == 0) ? llama_get_logits_ith(ctx, np - 1) : llama_get_logits(ctx);
        llama_token best = 0; float bv = logits[0];
        for (int t = 1; t < n_vocab; t++) if (logits[t] > bv) { bv = logits[t]; best = (llama_token)t; }
        out[total++] = best;
        if (best == eos) break;
        if (llama_decode(ctx, llama_batch_get_one(&best, 1)) != 0) break;
    }
    wss("generation");
    free(toks); llama_free(ctx);
    *n_out = total;
    return out;
}

static int toks_equal(const llama_token *a, int na, const llama_token *b, int nb) {
    if (!a || !b || na != nb) return 0;
    for (int i = 0; i < na; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ══ per-model state ══ */
typedef struct {
    const char *tag;
    const char *gguf, *field_path;
    GGUFBox box; uint32_t N;
    KVInfo src_kvs[64]; uint32_t n_src_kv;
    size_t src_hdr; const uint8_t *src_base; size_t src_kv_end; size_t tinfo_len;
    uint64_t tok_elem_off[3], tok_elem_len[3], tok_count[3];
    uint32_t tok_arrtype[3]; size_t tok_total;
    uint32_t *order; uint64_t *chain_off; uint64_t body_sz;
    uint8_t *idx; uint64_t idx_data_off;
    uint64_t body_off, cursor, n_windows;
    uint64_t payload_off[3];
    HANDLE hf, hm; const uint8_t *fmap;
    uint8_t *reb; size_t reb_final; uint64_t nkv_reb;
    uint64_t fbody_off; uint64_t *fpos;
    KVInfo fk[64]; uint32_t nfk;
    ServeCtx sc;
    struct gguf_context *meta;
    struct llama_model *model;
    llama_token *ref; int nref;
    llama_token *lazy; int nlazy;
    double ref_ws, ref_priv;
} Mod;

/* bake field file from source GGUF */
static int mod_bake(Mod *m) {
    if (gguf_box_open(&m->box, m->gguf) != 0) { printf("[%s] cannot open %s\n", m->tag, m->gguf); return -1; }
    m->N = m->box.n_tensors;
    if (kv_walk(m->box.reader.base, m->src_kvs, 64, &m->n_src_kv) != 0) return -1;
    m->src_hdr = (size_t)m->box.reader.data_offset;
    m->src_base = m->box.reader.base;
    m->src_kv_end = m->src_kvs[m->n_src_kv - 1].end;
    m->tinfo_len = m->src_hdr - m->src_kv_end;
    m->tok_total = 0;
    for (int t = 0; t < 3; t++) {
        int found = 0;
        for (uint32_t i = 0; i < m->n_src_kv; i++)
            if (strcmp(m->src_kvs[i].name, tok_names[t]) == 0) {
                m->tok_elem_off[t] = (uint64_t)(m->src_kvs[i].val_start + 12);
                m->tok_elem_len[t] = (uint64_t)(m->src_kvs[i].end - (m->src_kvs[i].val_start + 12));
                m->tok_count[t] = m->src_kvs[i].arr_count;
                m->tok_arrtype[t] = m->src_kvs[i].arr_type;
                m->tok_total += m->tok_elem_len[t];
                found = 1;
            }
        if (!found) { printf("[%s] missing tokenizer key %s\n", m->tag, tok_names[t]); return -1; }
    }
    m->order = (uint32_t *)calloc(m->N, sizeof(uint32_t));
    m->chain_off = (uint64_t *)calloc(m->N, sizeof(uint64_t));
    sort_inference(&m->box, m->order, m->N);
    m->body_sz = 0;
    for (uint32_t r = 0; r < m->N; r++) { m->chain_off[r] = m->body_sz; m->body_sz += align32(m->box.entries[m->order[r]].size); }

    size_t kv_len = m->src_kv_end - 24;
    size_t kis_slots = 13;
    size_t kis_bytes = 0;
    for (int k = 0; k < (int)kis_slots; k++) {
        const char *key = (k == 0) ? "kis.layout.body_off" : kis_key((k - 1) / 4, (const char *[]){"addr","len","count","arrtype"}[(k - 1) % 4]);
        kis_bytes += 8 + strlen(key) + 4 + 8;
    }
    size_t idx_cap = 24 + kv_len + kis_bytes + m->tinfo_len + 64;
    m->idx = (uint8_t *)calloc(1, idx_cap);
    size_t pos = 0;
    uint32_t magic = GGUF_MAGIC, version = 3;
    uint64_t nt = m->N, nkv_small = (m->n_src_kv - 3) + 13;
    memcpy(m->idx + pos, &magic, 4); pos += 4;
    memcpy(m->idx + pos, &version, 4); pos += 4;
    memcpy(m->idx + pos, &nt, 8); pos += 8;
    memcpy(m->idx + pos, &nkv_small, 8); pos += 8;
    for (uint32_t i = 0; i < m->n_src_kv; i++) {
        if (m->src_kvs[i].is_tok) continue;
        size_t len = m->src_kvs[i].end - m->src_kvs[i].start;
        memcpy(m->idx + pos, m->src_base + m->src_kvs[i].start, len);
        pos += len;
    }
    uint64_t *kis_val[13];
    for (int k = 0; k < (int)kis_slots; k++) {
        const char *key = (k == 0) ? "kis.layout.body_off" : kis_key((k - 1) / 4, (const char *[]){"addr","len","count","arrtype"}[(k - 1) % 4]);
        uint64_t klen = strlen(key), vzero = 0; uint32_t vt = 10;
        memcpy(m->idx + pos, &klen, 8); pos += 8;
        memcpy(m->idx + pos, key, klen); pos += klen;
        memcpy(m->idx + pos, &vt, 4); pos += 4;
        kis_val[k] = (uint64_t *)(m->idx + pos);
        memcpy(m->idx + pos, &vzero, 8); pos += 8;
    }
    memcpy(m->idx + pos, m->src_base + m->src_kv_end, m->tinfo_len);
    pos += m->tinfo_len;
    m->idx_data_off = align64(pos);
    m->body_off = m->idx_data_off;
    m->cursor = m->body_off + m->body_sz;
    for (int t = 0; t < 3; t++) { m->payload_off[t] = m->cursor; m->cursor += align32(m->tok_elem_len[t]); }
    *kis_val[0] = m->body_off;
    for (int t = 0; t < 3; t++) {
        *kis_val[1 + t * 4 + 0] = m->payload_off[t];
        *kis_val[1 + t * 4 + 1] = m->tok_elem_len[t];
        *kis_val[1 + t * 4 + 2] = m->tok_count[t];
        *kis_val[1 + t * 4 + 3] = m->tok_arrtype[t];
    }
    FILE *bf = fopen(m->field_path, "wb");
    if (!bf) return -1;
    if (fwrite(m->idx, 1, (size_t)m->idx_data_off, bf) != (size_t)m->idx_data_off) { fclose(bf); return -1; }
    for (uint32_t r = 0; r < m->N; r++) {
        const GGUFBoxEntry *ent = &m->box.entries[m->order[r]];
        if (fwrite(ent->data, 1, ent->size, bf) != ent->size) { fclose(bf); return -1; }
        uint64_t pad = align32(ent->size) - ent->size;
        if (pad) { uint8_t z[32] = {0}; if (fwrite(z, 1, pad, bf) != pad) { fclose(bf); return -1; } }
    }
    for (int t = 0; t < 3; t++) {
        if (fwrite(m->src_base + m->tok_elem_off[t], 1, (size_t)m->tok_elem_len[t], bf) != (size_t)m->tok_elem_len[t]) { fclose(bf); return -1; }
        uint64_t pad = align32(m->tok_elem_len[t]) - m->tok_elem_len[t];
        if (pad) { uint8_t z[32] = {0}; if (fwrite(z, 1, pad, bf) != pad) { fclose(bf); return -1; } }
    }
    fclose(bf);
    printf("[%s] bake: %s = [%llu B index][%llu B body][%llu B tok] tensors=%u\n", m->tag, m->field_path,
           (unsigned long long)m->idx_data_off, (unsigned long long)m->body_sz,
           (unsigned long long)(m->cursor - m->body_off - m->body_sz), m->N);
    return (m->idx_data_off < 65536) ? 0 : -1;
}

static int mod_mmap(Mod *m) {
    m->hf = CreateFileA(m->field_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    m->hm = m->hf != INVALID_HANDLE_VALUE ? CreateFileMappingA(m->hf, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    m->fmap = m->hm ? (const uint8_t *)MapViewOfFile(m->hm, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!m->fmap) return -1;
    m->n_windows = (m->cursor + WIN - 1) / WIN;
    m->sc.n_windows = m->n_windows;
    m->sc.win_bits = (uint8_t *)calloc(1, (m->n_windows + 7) / 8);
    m->sc.wc_tab = NULL; /* shadow cache disabled in dual tool */
    if (!m->sc.win_bits) return -1;
    printf("[%s] mmap %s: %llu B, %llu windows\n", m->tag, m->field_path,
           (unsigned long long)m->cursor, (unsigned long long)m->n_windows);
    return 0;
}

/* durable-artifact checks: no tokenizer keys, 13 kis.* keys, payloads == source */
static int mod_hcheck(Mod *m) {
    KVInfo fk[64]; uint32_t nfk = 0;
    int tok_found = 0, kis_found = 0, ok_payload = 1;
    if (kv_walk(m->fmap, fk, 64, &nfk) != 0) return -1;
    for (uint32_t i = 0; i < nfk; i++) {
        if (fk[i].is_tok) tok_found++;
        if (strncmp(fk[i].name, "kis.", 4) == 0) kis_found++;
    }
    for (int t = 0; t < 3; t++) {
        uint64_t addr = 0, len = 0;
        for (uint32_t i = 0; i < nfk; i++) {
            if (strcmp(fk[i].name, kis_key(t, "addr")) == 0) memcpy(&addr, m->fmap + fk[i].val_start, 8);
            if (strcmp(fk[i].name, kis_key(t, "len")) == 0) memcpy(&len, m->fmap + fk[i].val_start, 8);
        }
        if (addr == 0 || len != m->tok_elem_len[t]) { ok_payload = 0; continue; }
        if (memcmp(m->fmap + addr, m->src_base + m->tok_elem_off[t], (size_t)len) != 0) ok_payload = 0;
    }
    char d1[64], d2[64];
    snprintf(d1, sizeof(d1), "[%s] H1: durable header sans tokenizer + 13 kis.*", m->tag);
    snprintf(d2, sizeof(d2), "[%s] H2: tokenizer payload windows == source", m->tag);
    CHECK(d1, tok_found == 0 && kis_found == 13);
    CHECK(d2, ok_payload);
    return (tok_found == 0 && kis_found == 13 && ok_payload) ? 0 : -1;
}

/* rebuild full header in memory from the field */
static int mod_rebuild(Mod *m) {
    if (kv_walk(m->fmap, m->fk, 64, &m->nfk) != 0) return -1;
    size_t fkv_end = m->fk[m->nfk - 1].end;
    m->fbody_off = 0;
    for (uint32_t i = 0; i < m->nfk; i++)
        if (strcmp(m->fk[i].name, "kis.layout.body_off") == 0)
            memcpy(&m->fbody_off, m->fmap + m->fk[i].val_start, 8);
    size_t ftinfo_len = (size_t)(m->fbody_off - (uint64_t)fkv_end);
    size_t reb_cap = 24 + (fkv_end - 24) + (3 * 40) + m->tok_total + ftinfo_len + 64;
    m->reb = (uint8_t *)calloc(1, reb_cap);
    size_t rp = 0;
    memcpy(m->reb + rp, m->fmap, 24); rp += 24;
    m->nkv_reb = m->nfk + 3;
    memcpy(m->reb + 16, &m->nkv_reb, 8);
    memcpy(m->reb + rp, m->fmap + 24, fkv_end - 24); rp += fkv_end - 24;
    for (int t = 0; t < 3; t++) {
        uint64_t addr = 0, len = 0, cnt = 0; uint32_t at = 0;
        for (uint32_t i = 0; i < m->nfk; i++) {
            if (strcmp(m->fk[i].name, kis_key(t, "addr")) == 0) memcpy(&addr, m->fmap + m->fk[i].val_start, 8);
            if (strcmp(m->fk[i].name, kis_key(t, "len")) == 0) memcpy(&len, m->fmap + m->fk[i].val_start, 8);
            if (strcmp(m->fk[i].name, kis_key(t, "count")) == 0) memcpy(&cnt, m->fmap + m->fk[i].val_start, 8);
            if (strcmp(m->fk[i].name, kis_key(t, "arrtype")) == 0) memcpy(&at, m->fmap + m->fk[i].val_start, 4);
        }
        uint64_t nl = strlen(tok_names[t]); uint32_t vt = 9;
        memcpy(m->reb + rp, &nl, 8); rp += 8;
        memcpy(m->reb + rp, tok_names[t], (size_t)nl); rp += (size_t)nl;
        memcpy(m->reb + rp, &vt, 4); rp += 4;
        memcpy(m->reb + rp, &at, 4); rp += 4;
        memcpy(m->reb + rp, &cnt, 8); rp += 8;
        memcpy(m->reb + rp, m->fmap + addr, (size_t)len); rp += (size_t)len;
    }
    memcpy(m->reb + rp, m->fmap + fkv_end, ftinfo_len); rp += ftinfo_len;
    m->reb_final = (size_t)align32(rp);
    m->fpos = (uint64_t *)calloc(m->N, sizeof(uint64_t));
    for (uint32_t r = 0; r < m->N; r++) m->fpos[m->order[r]] = m->chain_off[r];
    return 0;
}

/* native file-load reference (freed before lazy path) */
static int mod_refgen(Mod *m, const char *prompt, int n_gen) {
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *rm = llama_model_load_from_file(m->gguf, mp);
    if (!rm) return -1;
    m->ref = generate_model(rm, prompt, n_gen, &m->nref);
    llama_model_free(rm);
    m->ref_ws = g_peak_ws; m->ref_priv = g_peak_priv;
    printf("[%s] reference: %d tokens  (WS %.1f MB)\n", m->tag, m->nref, m->ref_ws);
    return (m->ref && m->nref > 0) ? 0 : -1;
}

/* lazy load from the field (stays live) */
static int mod_lazyload(Mod *m, struct llama_model_params mp) {
    struct ggml_context *meta_ctx = NULL;
    struct gguf_init_params ip = { .no_alloc = true, .ctx = &meta_ctx };
    m->meta = gguf_init_from_buffer(m->reb, m->reb_final, ip);
    char d1[64];
    snprintf(d1, sizeof(d1), "[%s] L1: gguf_init from rebuilt header", m->tag);
    CHECK(d1, m->meta != NULL);
    if (!m->meta) return -1;
    int kt = gguf_find_key(m->meta, "tokenizer.ggml.tokens");
    int vocab_ok = (kt >= 0 && gguf_get_arr_n(m->meta, kt) == (int64_t)m->tok_count[0]);
    char want[4][64];
    {
        const uint8_t *tp = m->src_base + m->tok_elem_off[0];
        const uint8_t *tend = tp + m->tok_elem_len[0];
        for (int i = 0; i < 4 && tp + 8 <= tend; i++) {
            uint64_t sl; memcpy(&sl, tp, 8); tp += 8;
            memcpy(want[i], tp, sl < 63 ? sl : 63); want[i][sl < 63 ? sl : 63] = 0;
            tp += sl;
        }
    }
    if (kt >= 0)
        for (int i = 0; i < 4 && vocab_ok; i++) {
            const char *s = gguf_get_arr_str(m->meta, kt, (size_t)i);
            if (strcmp(s, want[i]) != 0) vocab_ok = 0;
        }
    char d1c[64];
    snprintf(d1c, sizeof(d1c), "[%s] L1c: vocab from field windows == source", m->tag);
    CHECK(d1c, vocab_ok);
    m->sc.box = &m->box; m->sc.field = m->fmap; m->sc.body_off = m->fbody_off; m->sc.fpos = m->fpos;
    win_reset(&m->sc);
    m->model = llama_model_init_from_user(m->meta, provide_tensor, &m->sc, mp);
    char d2[64], d2b[64];
    snprintf(d2, sizeof(d2), "[%s] L2: model loads from field callback", m->tag);
    CHECK(d2, m->model != NULL);
    if (!m->model) return -1;
    const struct llama_vocab *vocab = llama_model_get_vocab(m->model);
    snprintf(d2b, sizeof(d2b), "[%s] L2b: served==source tensors, vocab==source", m->tag);
    CHECK(d2b, m->sc.matched >= m->N && m->sc.matched <= m->N + 1 &&
               llama_vocab_n_tokens(vocab) == (int)m->tok_count[0]);
    printf("[%s] served: %u tensors (%llu B), vocab %d\n", m->tag, m->sc.matched,
           (unsigned long long)m->sc.bytes_served, llama_vocab_n_tokens(vocab));
    return 0;
}

static int mod_lazygen(Mod *m, const char *prompt, int n_gen, const char *gate) {
    win_reset(&m->sc);
    m->lazy = generate_model(m->model, prompt, n_gen, &m->nlazy);
    int ok = toks_equal(m->lazy, m->nlazy, m->ref, m->nref);
    printf("[%s] lazy %d vs ref %d → %s\n", m->tag, m->nlazy, m->nref, ok ? "identical ✅" : "DIVERGE ❌");
    untouched_report(&m->box, m->fmap, m->fbody_off, m->fpos, m->N);
    CHECK(gate, ok);
    return ok ? 0 : -1;
}

/* evict this model's body; refresh BOTH view pointers (single tool only
 * refreshed the local — the remap usually lands on the same address, but
 * relying on that is a latent use-after-unmap; fixed here). */
static int mod_evict(Mod *m) {
    const uint8_t *fresh = NULL;
    DWORD rc = wc_evict_body(m->fmap, m->fbody_off, m->body_sz, m->hm, 0, &fresh);
    printf("[%s] evict body rc=%lu\n", m->tag, (unsigned long)rc);
    if (rc != 0 || !fresh) return -1;
    m->fmap = fresh;
    m->sc.field = fresh;
    return 0;
}

static int mod_reload(Mod *m, struct llama_model_params mp) {
    win_reset(&m->sc);
    m->sc.matched = m->sc.missing = m->sc.aliased = 0; m->sc.bytes_served = 0;
    for (uint32_t i = 0; i < m->sc.n_owned; i++) free(m->sc.owned[i]);
    m->sc.n_owned = 0;
    m->model = llama_model_init_from_user(m->meta, provide_tensor, &m->sc, mp);
    char d[64];
    snprintf(d, sizeof(d), "[%s] E0: re-load works after evict", m->tag);
    CHECK(d, m->model != NULL);
    return m->model ? 0 : -1;
}

static void mod_close(Mod *m) {
    if (m->model) llama_model_free(m->model);
    if (m->meta) gguf_free(m->meta);
    free(m->lazy); free(m->ref); free(m->reb); free(m->idx);
    free(m->order); free(m->chain_off); free(m->fpos);
    free(m->sc.win_bits);
    for (uint32_t i = 0; i < m->sc.n_owned; i++) free(m->sc.owned[i]);
    if (m->fmap) UnmapViewOfFile(m->fmap);
    if (m->hm) CloseHandle(m->hm);
    if (m->hf != INVALID_HANDLE_VALUE) CloseHandle(m->hf);
    gguf_box_close(&m->box);
    memset(m, 0, sizeof(*m));
}

int main(int argc, char **argv) {
    const char *ggufA = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *ggufB = (argc > 2) ? argv[2] : "F:/model/huihui-moe-1b-q4_k_m.gguf";
    const char *prompt = (argc > 3) ? argv[3] : "The capital of France is";
    int n_gen = (argc > 4) ? atoi(argv[4]) : 10;
    if (n_gen <= 0) n_gen = 10;
    const char *backend_path = argc > 5 ? argv[5] : "I:/llama/llama-b10830-win-vulkan-x64";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Dual lazy serve — 2 models, 1 process, per-model fields\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    wss("baseline");

    llama_backend_init();
    llama_log_set(NULL, NULL);
    { /* loader audit: WHICH llama.dll did we actually bind? */
        char path[MAX_PATH] = {0};
        HMODULE hmll = GetModuleHandleA("llama.dll");
        if (hmll && GetModuleFileNameA(hmll, path, sizeof(path)))
            printf("  [dll] llama.dll -> %s\n", path);
        else
            printf("  [dll] llama.dll handle not yet loaded (delayed bind)\n");
    }
    ggml_backend_load_all_from_path(backend_path);
    ggml_backend_load_all();

    Mod A = {0}, B = {0};
    A.tag = "A"; A.gguf = ggufA; A.field_path = "build/fieldA.bin";
    B.tag = "B"; B.gguf = ggufB; B.field_path = "build/fieldB.bin";

    /* bake + mmap + durable checks + rebuild, both models */
    if (mod_bake(&A) != 0) { printf("A bake failed\n"); return 1; }
    if (mod_bake(&B) != 0) { printf("B bake failed\n"); return 1; }
    if (mod_mmap(&A) != 0 || mod_mmap(&B) != 0) { printf("mmap failed\n"); return 1; }
    res_report("A after mmap (cold)", A.fmap, A.cursor);
    res_report("B after mmap (cold)", B.fmap, B.cursor);
    if (mod_hcheck(&A) != 0 || mod_hcheck(&B) != 0) { printf("durable check failed\n"); return 1; }
    if (mod_rebuild(&A) != 0 || mod_rebuild(&B) != 0) { printf("rebuild failed\n"); return 1; }

    /* ── M4 (info only): tokenizer payloads identical across models? ──
     * Oracle: source GGUF element bytes directly (not via our kis walk —
     * that would be self-referential). Not a CHECK gate: a measurement. */
    {
        int same_count = (A.tok_count[0] == B.tok_count[0] &&
                          A.tok_count[1] == B.tok_count[1] &&
                          A.tok_count[2] == B.tok_count[2]);
        printf("  [M4] tokenizer counts A=(%llu,%llu,%llu) B=(%llu,%llu,%llu) → %s\n",
               (unsigned long long)A.tok_count[0], (unsigned long long)A.tok_count[1],
               (unsigned long long)A.tok_count[2], (unsigned long long)B.tok_count[0],
               (unsigned long long)B.tok_count[1], (unsigned long long)B.tok_count[2],
               same_count ? "same counts" : "DIFFERENT counts");
        if (same_count) {
            /* direct: source element regions (simpler than kis walk above) */
            int eq = 1;
            for (int t = 0; t < 3 && eq; t++)
                if (A.tok_elem_len[t] != B.tok_elem_len[t] ||
                    memcmp(A.src_base + A.tok_elem_off[t],
                           B.src_base + B.tok_elem_off[t],
                           (size_t)A.tok_elem_len[t]) != 0)
                    eq = 0;
            printf("  [M4] tokenizer payload bytes %s — %s\n",
                   eq ? "IDENTICAL ✅" : "differ",
                   eq ? "shared-tokenizer layout would save ~5.9 MB" : "separate windows required");
        }
    }

    /* references (native file-load, freed immediately) */
    if (mod_refgen(&A, prompt, n_gen) != 0) { printf("A reference failed\n"); return 1; }
    if (mod_refgen(&B, prompt, n_gen) != 0) { printf("B reference failed\n"); return 1; }

    /* ── co-serve: BOTH lazy models live simultaneously ── */
    printf("\n═ CO-SERVE — both lazy models live ═\n");
    g_peak_ws = 0; g_peak_priv = 0;
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    mp.no_host = true; /* L2 fix: plain CPU buft wraps field pointers zero-copy */
    phase_start();
    int la = mod_lazyload(&A, mp);
    int lb = mod_lazyload(&B, mp);
    phase_end("co-load A+B");
    CHECK("M1: both lazy models live simultaneously", la == 0 && lb == 0 && A.model && B.model);
    if (la != 0 || lb != 0) { mod_close(&A); mod_close(&B); return 1; }
    wss("after co-load");
    res_report("A after co-load", A.fmap, A.cursor);
    res_report("B after co-load", B.fmap, B.cursor);

    phase_start();
    int ga = mod_lazygen(&A, prompt, n_gen, "[A] L3: lazy == reference (bitwise)");
    int gb = mod_lazygen(&B, prompt, n_gen, "[B] L3: lazy == reference (bitwise)");
    phase_end("co-generate A+B");
    wss("after co-generate");
    double dual_ws = g_peak_ws, dual_priv = g_peak_priv;
    if (ga != 0 || gb != 0) { printf("co-generate diverged\n"); mod_close(&A); mod_close(&B); return 1; }

    /* ── M2: evict A → B must stay bitwise (isolation) ── */
    printf("\n═ M2 — evict A, B unaffected ═\n");
    if (mod_evict(&A) != 0) { printf("A evict failed\n"); mod_close(&A); mod_close(&B); return 1; }
    res_report("A after evict", A.fmap, A.cursor);
    res_report("B after A-evict (must stay resident)", B.fmap, B.cursor);
    free(B.lazy); B.lazy = NULL;
    phase_start();
    B.lazy = generate_model(B.model, prompt, n_gen, &B.nlazy);
    phase_end("B re-generate after A-evict");
    int iso = toks_equal(B.lazy, B.nlazy, B.ref, B.nref);
    printf("[B] after A-evict: %d vs ref %d → %s\n", B.nlazy, B.nref, iso ? "identical ✅" : "DIVERGE ❌");
    CHECK("M2: B bitwise after A evicted (isolation)", iso);

    /* ── M3: reload A → A bitwise (recovery, B still loaded) ── */
    printf("\n═ M3 — reload A, A recovers ═\n");
    if (A.model) { llama_model_free(A.model); A.model = NULL; }
    phase_start();
    int ra = mod_reload(&A, mp);
    phase_end("A re-load (re-fault)");
    res_report("A after re-load", A.fmap, A.cursor);
    int rec = 0;
    if (ra == 0) {
        free(A.lazy); A.lazy = NULL;
        phase_start();
        A.lazy = generate_model(A.model, prompt, n_gen, &A.nlazy);
        phase_end("A re-generate after evict+reload");
        rec = toks_equal(A.lazy, A.nlazy, A.ref, A.nref);
        printf("[A] after evict+reload: %d vs ref %d → %s\n", A.nlazy, A.nref, rec ? "identical ✅" : "DIVERGE ❌");
    }
    CHECK("M3: A bitwise after evict+reload (recovery, B still loaded)", rec);

    /* ── M5: WS bound ── */
    printf("\n═══════════════════════════════════════════════════════════════════════════\n");
    printf("  refA peak %.1f MB | refB peak %.1f MB | dual peak %.1f MB (private %.1f MB)\n",
           A.ref_ws, B.ref_ws, dual_ws, dual_priv);
    CHECK("M5: dual WS <= refA + refB + 1024 MB", dual_ws <= A.ref_ws + B.ref_ws + 1024.0);

    mod_close(&A);
    mod_close(&B);
    llama_backend_free();
    printf("FINAL: %d/%d PASS — %s\n", pass_count, pass_count + fail_count,
           fail_count ? "FAIL" : "dual lazy serve: co-serve + evict isolation + recovery");
    return fail_count ? 1 : 0;
}
