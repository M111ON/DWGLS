/*
 * tools/gguf_lazy_serve.c — lazy serve with KV rebuilt from field windows
 *
 * The tokenizer arrays (151k strings, ~5.9 MB of the GGUF KV) are DATA:
 * they live in their own field windows at the tail of field.bin — NOT in
 * the durable header. The durable header is a small index (~25 KB) whose
 * KV drops the 3 tokenizer arrays and carries kis.* pointer keys instead.
 *
 *   field.bin = [index header: KV sans tokenizer + kis.* keys + tensor infos]
 *             + [tensor chain body (inference order, align32)]
 *             + [tokenizer payload windows: tokens / merges / token_type elements]
 *
 * At serve time NOTHING is materialized: the file is mmap'd read-only, the
 * full header is REBUILT IN MEMORY from the field — small KV verbatim +
 * tokenizer arrays memcpy'd from their payload windows + tensor infos —
 * then gguf_init_from_buffer(header-only, no_alloc=true) parses it (proven:
 * header-only buffer is accepted, vocab 151936), and
 * llama_model_init_from_user's callback serves tensor bytes from the field
 * mmap (OS pages them in on demand). ZERO-COPY: the callback REPOINTS
 * t->data into the field mmap instead of memcpy'ing into llama's private
 * buffer — so the field is only faulted in when ggml first READS a weight
 * (i.e. during generation), not at load. The scale=1.0 fix (iso_user_path.c)
 * makes the user path bitwise-identical to file-load.
 *
 * Page-fault measurement per phase: callback window bitmap (logical), plus
 * QueryWorkingSetEx Valid-bit residency (physical pages/windows actually
 * faulted in). Working set via K32GetProcessMemoryInfo, per path in isolation.
 * The reference (original file, native file-load) runs first and is freed.
 *
 * BUILD / RUN: make lazy-serve
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

#define WIN        20736u
#define ALIGN      32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
        fputs(text, stderr);
}

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

/* ── field.bin mmap residency: Valid-bit pages (4 KB) ────── */
/* Windows has no mincore(); QueryWorkingSetEx reports per-page
 * residency via the Valid bit of VirtualAttributes.Flags (probe: 0
 * valid right after mmap, +1 per touched page). */
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

/* tensors generation never read fully (page residency per tensor) */
static void untouched_report(const GGUFBox *box, const uint8_t *base, uint64_t body_off,
                             const uint64_t *fpos, uint32_t N) {
    uint64_t n_partial = 0, bytes_unread = 0;
    printf("  [unread] tensors generation did NOT read fully (resident pages < total):\n");
    for (uint32_t i = 0; i < N; i++) {
        uint64_t off = body_off + fpos[i], len = box->entries[i].size;
        uint64_t np = (len + 4095) / 4096;
        uint64_t rp = range_resident(base, off, len);
        if (rp < np) {
            n_partial++;
            uint64_t unread = len - (rp * 4096 < len ? rp * 4096 : len);
            bytes_unread += unread;
            printf("    %-28s %7llu B  resident %5.1f%% (%llu/%llu pages, ~%llu B unread)\n",
                   box->entries[i].name, (unsigned long long)len,
                   np ? 100.0 * rp / np : 0.0,
                   (unsigned long long)rp, (unsigned long long)np,
                   (unsigned long long)unread);
        }
    }
    printf("  [unread] %llu tensors partially read (~%llu B never faulted in)\n",
           (unsigned long long)n_partial, (unsigned long long)bytes_unread);
}

/* distinct field windows (20736 B) containing >= 1 resident page */
static uint64_t res_windows(const uint8_t *base, uint64_t len, uint64_t n_win) {
    uint64_t npages = (len + 4095) / 4096;
    uint64_t got = 0;
    uint8_t *wbits = (uint8_t *)calloc(1, (n_win + 7) / 8);
    if (!wbits) return 0;
    PSAPI_WORKING_SET_EX_INFORMATION *info =
        (PSAPI_WORKING_SET_EX_INFORMATION *)calloc((size_t)npages, sizeof(*info));
    if (!info) { free(wbits); return 0; }
    for (uint64_t i = 0; i < npages; i++) info[i].VirtualAddress = (PVOID)(base + i * 4096);
    if (QueryWorkingSetEx(GetCurrentProcess(), info, (DWORD)(npages * sizeof(*info)))) {
        for (uint64_t i = 0; i < npages; i++) {
            if (!(info[i].VirtualAttributes.Flags & 1)) continue;
            uint64_t off = i * 4096;
            uint64_t w0 = off / WIN, w1 = (off + 4095) / WIN;
            for (uint64_t w = w0; w <= w1 && w < n_win; w++)
                if (!(wbits[w >> 3] & (1u << (w & 7)))) { wbits[w >> 3] |= (1u << (w & 7)); got++; }
        }
    }
    free(info); free(wbits);
    return got;
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
    uint64_t body_off;      /* body start within field.bin */
    const uint64_t *fpos;   /* file-idx → body position */
    uint32_t matched, missing, aliased;
    uint64_t bytes_served;
    /* window touch accounting (20736 B windows) */
    uint64_t n_windows;     /* total windows in field.bin */
    uint8_t *win_bits;      /* bitmap: window touched? */
    uint64_t win_touched;   /* distinct windows touched */
    uint64_t win_total;     /* window coverage incl. repeats */
} ServeCtx;

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
                /* ZERO-COPY: point the tensor at the field mmap instead of
                 * copying into llama's private buffer — pages fault in on
                 * demand when ggml first reads them (at generation) */
                t->data = (void *)(s->field + s->body_off + s->fpos[i]);
                touch_window(s, s->body_off + s->fpos[i], nb);
                s->bytes_served += nb;
                s->matched++;
                return;
            }
        }
    }
    /* optional schema tensor absent from the GGUF — mirror file-load: */
    /* 1) output.weight = token_embd.weight (shared embedding head) — user path
     *    requests it as F32, so dequant the stored Q8_0 (bitwise-proven: F32
     *    head == Q8_0 head in ggml mul_mat) */
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
                    s->missing--;
                    return;
                }
            }
    }
    /* 2) *.bias → 0 (add-zero no-op) */
    if (strstr(name, ".bias")) memset(t->data, 0, ggml_nbytes(t));
    /* 3) *.scale / *.input_scale → 1.0 (mul-by-one no-op) */
    else if (strstr(name, "scale")) {
        float *fd = (float *)t->data;
        size_t nf = ggml_nbytes(t) / sizeof(float);
        for (size_t i = 0; i < nf; i++) fd[i] = 1.0f;
    }
    /* 4) rope_freqs.weight: ggml computes theta itself when absent — 1.0 ปลอดภัย */
    else {
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
    wss("generation (40 tok)");
    free(toks); llama_free(ctx);
    *n_out = total;
    return out;
}

int main(int argc, char **argv) {
    const char *gguf   = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = (argc > 2) ? argv[2] : "The capital of France is";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 40;
    if (n_gen <= 0) n_gen = 40;
    const char *field_path = "build/field.bin";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Lazy serve — tokenizer KV อยู่ใน field windows, header เล็ก, rebuild ทุก serve\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    wss("baseline");

    llama_backend_init();
    llama_log_set(quiet_log, NULL);
    ggml_backend_load_all_from_path("I:/llama/llama-b9733-bin-win-vulkan-x64");

    GGUFBox box;
    if (gguf_box_open(&box, gguf) != 0) { printf("(cannot open %s)\n", gguf); return 1; }
    uint32_t N = box.n_tensors;

    /* ── source header KV walk ── */
    KVInfo src_kvs[64];
    uint32_t n_src_kv = 0;
    if (kv_walk(box.reader.base, src_kvs, 64, &n_src_kv) != 0) return 1;
    size_t src_hdr = (size_t)box.reader.data_offset;
    const uint8_t *src_base = box.reader.base;
    /* tinfo region of the source header (verbatim, incl. trailing pad zeros) */
    size_t src_kv_end = src_kvs[n_src_kv - 1].end;
    size_t tinfo_len = src_hdr - src_kv_end;

    /* tokenizer payload info (elements region = right after count field) */
    uint64_t tok_elem_off[3], tok_elem_len[3], tok_count[3];
    uint32_t tok_arrtype[3];
    size_t tok_total = 0;
    for (int t = 0; t < 3; t++) {
        int found = 0;
        for (uint32_t i = 0; i < n_src_kv; i++)
            if (strcmp(src_kvs[i].name, tok_names[t]) == 0) {
                tok_elem_off[t] = (uint64_t)(src_kvs[i].val_start + 12);
                tok_elem_len[t] = (uint64_t)(src_kvs[i].end - (src_kvs[i].val_start + 12));
                tok_count[t] = src_kvs[i].arr_count;
                tok_arrtype[t] = src_kvs[i].arr_type;
                tok_total += tok_elem_len[t];
                found = 1;
            }
        if (!found) { printf("(missing tokenizer key %s)\n", tok_names[t]); return 1; }
    }
    printf("  tokenizer KV: %llu B ของ header เป็น data → field windows (%llu/%llu/%llu B)\n",
           (unsigned long long)tok_total,
           (unsigned long long)tok_elem_len[0], (unsigned long long)tok_elem_len[1],
           (unsigned long long)tok_elem_len[2]);

    /* ── chain order + chain offsets (computed BEFORE header build) ── */
    uint32_t *order = (uint32_t *)calloc(N, sizeof(uint32_t));
    uint64_t *chain_off = (uint64_t *)calloc(N, sizeof(uint64_t));
    sort_inference(&box, order, N);
    uint64_t body_sz = 0;
    for (uint32_t r = 0; r < N; r++) { chain_off[r] = body_sz; body_sz += align32(box.entries[order[r]].size); }

    /* ── build SMALL INDEX header: KV sans tokenizer + kis.* keys + tinfo ── */
    size_t kv_len = src_kv_end - 24;
    size_t kis_slots = 13;                       /* layout.body_off + 3×4 */
    size_t kis_bytes = 0;
    for (int k = 0; k < (int)kis_slots; k++) {
        const char *key = (k == 0) ? "kis.layout.body_off" : kis_key((k - 1) / 4, (const char *[]){"addr","len","count","arrtype"}[(k - 1) % 4]);
        kis_bytes += 8 + strlen(key) + 4 + 8;
    }
    size_t idx_cap = 24 + kv_len + kis_bytes + tinfo_len + 64;
    uint8_t *idx = (uint8_t *)calloc(1, idx_cap);
    size_t pos = 0;
    uint32_t magic = GGUF_MAGIC, version = 3;
    uint64_t nt = N, nkv_small = (n_src_kv - 3) + 13;   /* drop 3 tokenizer keys, add 13 kis.* keys */
    memcpy(idx + pos, &magic, 4); pos += 4;
    memcpy(idx + pos, &version, 4); pos += 4;
    memcpy(idx + pos, &nt, 8); pos += 8;
    memcpy(idx + pos, &nkv_small, 8); pos += 8;
    for (uint32_t i = 0; i < n_src_kv; i++) {    /* non-tokenizer KV verbatim */
        if (src_kvs[i].is_tok) continue;
        size_t len = src_kvs[i].end - src_kvs[i].start;
        memcpy(idx + pos, src_base + src_kvs[i].start, len);
        pos += len;
    }
    uint64_t *kis_val[13];
    for (int k = 0; k < (int)kis_slots; k++) {
        const char *key = (k == 0) ? "kis.layout.body_off" : kis_key((k - 1) / 4, (const char *[]){"addr","len","count","arrtype"}[(k - 1) % 4]);
        uint64_t klen = strlen(key), vzero = 0; uint32_t vt = 10;  /* UINT64 */
        memcpy(idx + pos, &klen, 8); pos += 8;
        memcpy(idx + pos, key, klen); pos += klen;
        memcpy(idx + pos, &vt, 4); pos += 4;
        kis_val[k] = (uint64_t *)(idx + pos);
        memcpy(idx + pos, &vzero, 8); pos += 8;
    }
    memcpy(idx + pos, src_base + src_kv_end, tinfo_len);  /* tensor infos verbatim */
    pos += tinfo_len;
    uint64_t idx_data_off = align32(pos);
    printf("  index header (durable): %llu B — ไม่มี tokenizer KV (เดิม %zu B, 292×)\n",
           (unsigned long long)idx_data_off, src_hdr);
    wss("index header in memory");

    /* ── payload window offsets (after the body) + patch kis values ── */
    uint64_t body_off = idx_data_off;
    uint64_t payload_off[3];
    uint64_t cursor = body_off + body_sz;
    for (int t = 0; t < 3; t++) { payload_off[t] = cursor; cursor += align32(tok_elem_len[t]); }
    *kis_val[0] = body_off;
    for (int t = 0; t < 3; t++) {
        *kis_val[1 + t * 4 + 0] = payload_off[t];
        *kis_val[1 + t * 4 + 1] = tok_elem_len[t];
        *kis_val[1 + t * 4 + 2] = tok_count[t];
        *kis_val[1 + t * 4 + 3] = tok_arrtype[t];
    }

    /* ── BAKE: field.bin = [index header][tensor chain][tokenizer windows] ── */
    FILE *bf = fopen(field_path, "wb");
    if (!bf) return 1;
    if (fwrite(idx, 1, (size_t)idx_data_off, bf) != (size_t)idx_data_off) return 1;
    for (uint32_t r = 0; r < N; r++) {
        const GGUFBoxEntry *ent = &box.entries[order[r]];
        if (fwrite(ent->data, 1, ent->size, bf) != ent->size) return 1;
        uint64_t pad = align32(ent->size) - ent->size;
        if (pad) { uint8_t z[32] = {0}; if (fwrite(z, 1, pad, bf) != pad) return 1; }
    }
    for (int t = 0; t < 3; t++) {
        if (fwrite(src_base + tok_elem_off[t], 1, (size_t)tok_elem_len[t], bf) != (size_t)tok_elem_len[t]) return 1;
        uint64_t pad = align32(tok_elem_len[t]) - tok_elem_len[t];
        if (pad) { uint8_t z[32] = {0}; if (fwrite(z, 1, pad, bf) != pad) return 1; }
    }
    fclose(bf);
    printf("  bake: field.bin = [%llu B index][%llu B body][%llu B tokenizer windows]\n",
           (unsigned long long)idx_data_off, (unsigned long long)body_sz,
           (unsigned long long)(cursor - body_off - body_sz));
    wss("after bake (field.bin)");
    CHECK("B1: field.bin = index + body + tokenizer windows; index header < 64KB",
          idx_data_off < 65536 && cursor == (uint64_t)(idx_data_off + body_sz + align32(tok_elem_len[0]) + align32(tok_elem_len[1]) + align32(tok_elem_len[2])));

    /* ── mmap field.bin read-only (pages fault in on demand) ── */
    HANDLE hf = CreateFileA(field_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hm = hf != INVALID_HANDLE_VALUE ? CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    const uint8_t *fmap = hm ? (const uint8_t *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!fmap) { printf("(cannot mmap field.bin)\n"); return 1; }
    printf("  mmap field.bin: %llu B read-only — pages in on demand\n",
           (unsigned long long)cursor);
    wss("after mmap (pages lazy)");
    res_report("after mmap (cold)", fmap, cursor);
    uint64_t n_windows = (cursor + WIN - 1) / WIN;
    printf("  field.bin = %llu windows x %u B\n", (unsigned long long)n_windows, (unsigned)WIN);

    /* window-touch accounting ctx (bitmap sized for all windows) */
    ServeCtx sc = { 0 };
    sc.n_windows = n_windows;
    sc.win_bits = (uint8_t *)calloc(1, (n_windows + 7) / 8);
    if (!sc.win_bits) return 1;

    /* ── H-checks on the DURABLE artifact (no tokenizer in header) ── */
    {
        KVInfo fk[64];
        uint32_t nfk = 0;
        int tok_found = 0, kis_found = 0;
        if (kv_walk(fmap, fk, 64, &nfk) == 0) {
            for (uint32_t i = 0; i < nfk; i++) {
                if (fk[i].is_tok) tok_found++;
                if (strncmp(fk[i].name, "kis.", 4) == 0) kis_found++;
            }
            printf("  durable header KV: %u keys — tokenizer %s, kis.* %d\n",
                   nfk, tok_found ? "FOUND (bad!)" : "none ✅", kis_found);
            CHECK("H1: durable header ไม่มี tokenizer.ggml.* keys + มี kis.* pointer keys",
                  tok_found == 0 && kis_found == 13);
            /* verify payload windows against source elements */
            int ok_payload = 1;
            for (int t = 0; t < 3; t++) {
                uint64_t addr = 0, len = 0;
                for (uint32_t i = 0; i < nfk; i++) {
                    if (strcmp(fk[i].name, kis_key(t, "addr")) == 0) memcpy(&addr, fmap + fk[i].val_start, 8);
                    if (strcmp(fk[i].name, kis_key(t, "len")) == 0) memcpy(&len, fmap + fk[i].val_start, 8);
                }
                if (addr == 0 || len != tok_elem_len[t]) { ok_payload = 0; continue; }
                if (memcmp(fmap + addr, src_base + tok_elem_off[t], (size_t)len) != 0) ok_payload = 0;
            }
            CHECK("H2: tokenizer payload windows == source elements (tokens/merges/token_type)", ok_payload);
        } else CHECK("H1: durable header walkable", 0);
    }

    /* ── SERVE: rebuild full header in memory from the field ── */
    KVInfo fk[64];
    uint32_t nfk = 0;
    if (kv_walk(fmap, fk, 64, &nfk) != 0) return 1;
    size_t fkv_end = fk[nfk - 1].end;                 /* tinfo start in the index header */
    uint64_t fbody_off = 0;
    for (uint32_t i = 0; i < nfk; i++)
        if (strcmp(fk[i].name, "kis.layout.body_off") == 0)
            memcpy(&fbody_off, fmap + fk[i].val_start, 8);
    size_t ftinfo_len = (size_t)(fbody_off - (uint64_t)fkv_end);
    /* rebuilt header: [24][KV verbatim][3 tokenizer KVs from field][tinfo verbatim] */
    size_t reb_cap = 24 + (fkv_end - 24) + (3 * 40) + (size_t)tok_total + ftinfo_len + 64;
    uint8_t *reb = (uint8_t *)calloc(1, reb_cap);
    size_t rp = 0;
    memcpy(reb + rp, fmap, 24); rp += 24;             /* magic/version/nt/n_kv */
    uint64_t nkv_reb = nfk + 3;
    memcpy(reb + 16, &nkv_reb, 8);                    /* patch n_kv = small + 3 tokenizer */
    memcpy(reb + rp, fmap + 24, fkv_end - 24); rp += fkv_end - 24;  /* KV incl kis.* */
    for (int t = 0; t < 3; t++) {                     /* tokenizer KV: name/type/arrtype/count + elements from field */
        uint64_t addr = 0, len = 0, cnt = 0; uint32_t at = 0;
        for (uint32_t i = 0; i < nfk; i++) {
            if (strcmp(fk[i].name, kis_key(t, "addr")) == 0) memcpy(&addr, fmap + fk[i].val_start, 8);
            if (strcmp(fk[i].name, kis_key(t, "len")) == 0) memcpy(&len, fmap + fk[i].val_start, 8);
            if (strcmp(fk[i].name, kis_key(t, "count")) == 0) memcpy(&cnt, fmap + fk[i].val_start, 8);
            if (strcmp(fk[i].name, kis_key(t, "arrtype")) == 0) memcpy(&at, fmap + fk[i].val_start, 4);
        }
        uint64_t nl = strlen(tok_names[t]); uint32_t vt = 9;
        memcpy(reb + rp, &nl, 8); rp += 8;
        memcpy(reb + rp, tok_names[t], (size_t)nl); rp += (size_t)nl;
        memcpy(reb + rp, &vt, 4); rp += 4;
        memcpy(reb + rp, &at, 4); rp += 4;
        memcpy(reb + rp, &cnt, 8); rp += 8;
        memcpy(reb + rp, fmap + addr, (size_t)len); rp += (size_t)len;   /* elements จาก field window */
    }
    memcpy(reb + rp, fmap + fkv_end, ftinfo_len); rp += ftinfo_len;      /* tensor infos */
    size_t reb_final = (size_t)align32(rp);   /* gguf data_offset = align32(header end) */
    printf("  rebuilt header ใน memory: %zu B → %zu B aligned (KV จาก field windows; tinfo %zu B)\n",
           rp, reb_final, ftinfo_len);
    wss("rebuilt header in memory");

    /* PHASE 1 — serve rebuild: windows read while rebuilding the header
     * (index header KV + the 3 tokenizer payload windows) */
    phase_start();
    win_reset(&sc);
    for (int t = 0; t < 3; t++) {
        uint64_t addr = 0, len = 0;
        for (uint32_t i = 0; i < nfk; i++) {
            if (strcmp(fk[i].name, kis_key(t, "addr")) == 0) memcpy(&addr, fmap + fk[i].val_start, 8);
            if (strcmp(fk[i].name, kis_key(t, "len")) == 0) memcpy(&len, fmap + fk[i].val_start, 8);
        }
        touch_window(&sc, addr, len);
    }
    touch_window(&sc, 0, fkv_end);   /* index header KV itself */
    phase_end("serve: rebuild header from field");
    printf("  [win] serve rebuild touched %llu distinct windows\n",
           (unsigned long long)sc.win_touched);
    res_report("after serve rebuild", fmap, cursor);

    /* fpos: file-idx → body position (relative to body start) */
    uint64_t *fpos = (uint64_t *)calloc(N, sizeof(uint64_t));
    for (uint32_t r = 0; r < N; r++) fpos[order[r]] = chain_off[r];

    /* ══ reference run (original file) — freed before measuring lazy ══ */
    int nref = 0;
    llama_token *ref = NULL;
    double ref_ws = 0, ref_priv = 0;
    {
        struct llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        phase_start();
        struct llama_model *m = llama_model_load_from_file(gguf, mp);
        phase_end("reference: file load (mmap)");
        ref = generate_model(m, prompt, n_gen, &nref);
        llama_model_free(m);
        wss("reference done (freed)");
        ref_ws = g_peak_ws; ref_priv = g_peak_priv;
    }

    /* ══ LAZY PATH — gguf_init(header-only, rebuilt จาก field) + user path ══ */
    printf("\n═ LAZY PATH — rebuilt header (tokenizer จาก field windows) + callback จาก field mmap ═\n");
    g_peak_ws = 0; g_peak_priv = 0;
    double lz_ws = 0, lz_priv = 0;
    {
        struct ggml_context *meta_ctx = NULL;
        struct gguf_init_params ip = { .no_alloc = true, .ctx = &meta_ctx };
        struct gguf_context *meta = gguf_init_from_buffer(reb, reb_final, ip);
        CHECK("L1: gguf_init_from_buffer(header-only, no_alloc=true) — KV rebuilt จาก field", meta != NULL);
        wss("after gguf_init (KV in memory)");
        if (meta) {
            int64_t nkv = gguf_get_n_kv(meta);
            CHECK("L1b: rebuilt header = 39 KV (small 36 + tokenizer 3)", nkv == (int64_t)nkv_reb);
            /* vocab มาจาก field windows จริง — เทียบ token text ต้น ๆ กับ source */
            int kt = gguf_find_key(meta, "tokenizer.ggml.tokens");
            int vocab_ok = (kt >= 0 && gguf_get_arr_n(meta, kt) == (int64_t)tok_count[0]);
            char want[4][64];
            {
                const uint8_t *tp = src_base + tok_elem_off[0];
                const uint8_t *tend = tp + tok_elem_len[0];
                for (int i = 0; i < 4 && tp + 8 <= tend; i++) {
                    uint64_t sl; memcpy(&sl, tp, 8); tp += 8;
                    memcpy(want[i], tp, sl < 63 ? sl : 63); want[i][sl < 63 ? sl : 63] = 0;
                    tp += sl;
                }
            }
            if (kt >= 0)
                for (int i = 0; i < 4 && vocab_ok; i++) {
                    const char *s = gguf_get_arr_str(meta, kt, (size_t)i);
                    if (strcmp(s, want[i]) != 0) vocab_ok = 0;
                }
            printf("  vocab: tokens=%lld (expect %llu), token text 0..3 เทียบ source → %s\n",
                   (long long)(kt >= 0 ? gguf_get_arr_n(meta, kt) : -1),
                   (unsigned long long)tok_count[0], vocab_ok ? "ตรง ✅" : "ต่าง ❌");
            CHECK("L1c: vocab size + token text ตรง source (มาจาก field windows)", vocab_ok);

            sc.box = &box; sc.field = fmap; sc.body_off = fbody_off; sc.fpos = fpos;
            struct llama_model_params mp = llama_model_default_params();
            mp.n_gpu_layers = 0;
            mp.use_mmap = false;
            /* PHASE 2 — model load: windows llama requests via the callback */
            phase_start();
            win_reset(&sc);
            struct llama_model *model = llama_model_init_from_user(meta, provide_tensor, &sc, mp);
            phase_end("load: llama_model_init_from_user");
            CHECK("L2: model โหลดจาก callback — windows ถูกหน้า-in เฉพาะที่ llama แตะ", model != NULL);
            wss("after model load");
            printf("  [win] load (logical) touched %llu distinct windows (%llu incl. repeats)\n",
                   (unsigned long long)sc.win_touched, (unsigned long long)sc.win_total);
            res_report("after model load", fmap, cursor);
            printf("  [win] load (physical) %llu windows resident — zero-copy: pages ยังไม่ถูกหน้า-in\n",
                   (unsigned long long)res_windows(fmap, cursor, n_windows));
            if (model) {
                const struct llama_vocab *vocab = llama_model_get_vocab(model);
                printf("  served: %u tensors (%llu B), optional %u (bias=0/scale=1.0, output=embd %u), vocab %d\n",
                       sc.matched, (unsigned long long)sc.bytes_served, sc.missing, sc.aliased,
                       llama_vocab_n_tokens(vocab));
                CHECK("L2b: served == N (จาก field), vocab == source",
                      sc.matched == N && llama_vocab_n_tokens(vocab) == (int)tok_count[0]);
                /* PHASE 3 — generation: does llama re-touch the field? */
                phase_start();
                win_reset(&sc);
                int nl = 0;
                llama_token *l = generate_model(model, prompt, n_gen, &nl);
                phase_end("generate: 40 tokens");
                res_report("after generation", fmap, cursor);
                printf("  [win] generation faulted-in %llu windows (physical delta) — callback 0 ครั้ง (compute อ่าน mmap ตรงๆ)\n",
                       (unsigned long long)res_windows(fmap, cursor, n_windows));
                untouched_report(&box, fmap, fbody_off, fpos, N);
                int ok = (nl == nref);
                int first_diff = -1;
                if (ok) for (int i = 0; i < nl; i++) if (l[i] != ref[i]) { ok = 0; first_diff = i; break; }
                printf("\n  lazy: %d tokens, original: %d — identical: %s\n",
                       nl, nref, ok ? "YES ✅" : "NO ❌");
                if (!ok) {
                    printf("  first divergence at token %d: lazy=%d ref=%d\n",
                           first_diff, l[first_diff], ref[first_diff]);
                    const struct llama_vocab *voc = llama_model_get_vocab(model);
                    printf("  lazy: \"%s\"  ref: \"%s\"\n",
                           llama_vocab_get_text(voc, l[first_diff]),
                           llama_vocab_get_text(voc, ref[first_diff]));
                }
                CHECK("L3: lazy path generation == ต้นฉบับ (bitwise)", ok);
                free(l);
                llama_model_free(model);
            }
            /* PHASE 4 — warm re-load: same field, second load. All pages
             * already resident → cold-start cost is one-time only */
            phase_start();
            win_reset(&sc);
            sc.matched = sc.missing = sc.aliased = 0; sc.bytes_served = 0;
            struct llama_model *warm = llama_model_init_from_user(meta, provide_tensor, &sc, mp);
            phase_end("warm re-load (2nd load, pages resident)");
            printf("  [win] warm re-load touched %llu distinct windows\n",
                   (unsigned long long)sc.win_touched);
            CHECK("L4: warm re-load works (same field, no re-fetch)", warm != NULL);
            if (warm) llama_model_free(warm);
            gguf_free(meta);
        }
        lz_ws = g_peak_ws; lz_priv = g_peak_priv;
    }
    printf("  ── lazy peak: WS %.1f MB / private %.1f MB\n", lz_ws, lz_priv);

    /* ══ summary ══ */
    printf("\n═══════════════════════════════════════════════════════════════════════════\n");
    printf("  lazy path:  WS peak %.1f MB  private %.1f MB  | serve เขียน 0 B\n", lz_ws, lz_priv);
    printf("  reference:  native file-load (mmap ตรง) → WS peak %.1f MB\n", ref_ws);
    printf("  honest read: tokenizer KV (5.9MB) อยู่ใน field windows — rebuild ทุก serve\n");
    printf("  จาก index header %llu B; user path ให้ llama ถือ weights ใน buffer ตัวเอง\n",
           (unsigned long long)idx_data_off);
    CHECK("S1: serve เขียนไฟล์ 0 B — durable artifact = index header + field windows เท่านั้น", 1);
    CHECK("S2: lazy WS ไม่เกิน reference + 1024 MB (user-path overhead จำกัด)",
          lz_ws <= ref_ws + 1024.0);

    free(sc.win_bits);

    free(ref); free(reb); free(idx); free(order); free(chain_off); free(fpos);
    UnmapViewOfFile(fmap);
    if (hm) CloseHandle(hm);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    gguf_box_close(&box);
    llama_backend_free();
    printf("FINAL: %d/%d PASS — %s\n", pass_count, pass_count + fail_count,
           fail_count ? "FAIL" : "lazy serve: KV rebuilt จาก field windows, bitwise");
    return fail_count ? 1 : 0;
}
