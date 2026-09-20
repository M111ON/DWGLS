/*
 * tools/field_qa.c — Q&A over a baked field file (no GGUF, no rebuild-to-disk).
 *
 * Opens build/fieldA.bin (mmap), rebuilds the header in memory, serves
 * tensor bytes from the field via llama_model_init_from_user, generates
 * greedy text per prompt and DETOKENIZES it to stdout.
 *
 * Reuses dual_lazy_serve.c serve logic (bake skipped: field already on disk).
 *
 * BUILD (MSVC required — GCC/MSVC struct layout mismatch for llama_model_params):
 *   call vcvars64.bat, then:
 *   cl.exe /O2 /W3 /std:c11 /D_CRT_SECURE_NO_WARNINGS /I core /I <llama>/include
 *     /I <llama>/ggml/include /Fe:build/field_qa.exe tools/field_qa.c
 *     <llama>/llama.lib ggml.lib ggml-base.lib ggml-cpu-x64.lib psapi.lib advapi32.lib
 *
 * RUN:   PATH="<dll>:$PATH" ./build/field_qa <field.bin> <backend_dir> [n_gen]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <psapi.h>
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "../core/gguf_box.h"

#define WIN        20736u
#define ALIGN      32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))
#define align64(x) (((x) + 63u) & ~((uint64_t)63u))

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

static const char *tok_names[3] = {
    "tokenizer.ggml.tokens", "tokenizer.ggml.merges", "tokenizer.ggml.token_type" };
static const char *tok_suffix[3] = { "tokens", "merges", "token_type" };
static const char *kis_key(int t, const char *field_) {
    static char buf[48];
    snprintf(buf, sizeof(buf), "kis.kv.%s.%s", tok_suffix[t], field_);
    return buf;
}

typedef struct {
    const GGUFBox *box;
    const uint8_t *field;
    uint64_t body_off;
    const uint64_t *fpos;
    const uint8_t *is_delta;
    const uint32_t *d_E;
    const uint64_t *d_sl;
    const uint64_t *d_boff;
    uint64_t *const *d_voff;
    const uint64_t *d_rlen;
    uint64_t n_windows;
    uint8_t *win_bits;
    void * owned[256];
    uint32_t n_owned;
} ServeCtx;

static void * fallback_data(ServeCtx *s, size_t n) {
    void *p = calloc(1, n);
    if (!p || s->n_owned >= 256) { free(p); return NULL; }
    s->owned[s->n_owned++] = p;
    return p;
}

static void touch_window(ServeCtx *s, uint64_t off, uint64_t len) {
    if (len == 0 || !s->win_bits) return;
    uint64_t w0 = off / WIN, w1 = (off + len - 1) / WIN;
    for (uint64_t w = w0; w <= w1; w++)
        if (w < s->n_windows) s->win_bits[w >> 3] |= (uint8_t)(1u << (w & 7));
}
static void provide_tensor(struct ggml_tensor *t, void *ud) {
    ServeCtx *s = (ServeCtx *)ud;
    const char *name = ggml_get_name(t);
    for (uint32_t i = 0; i < s->box->n_tensors; i++) {
        if (strcmp(s->box->entries[i].name, name) == 0) {
            if (s->is_delta && s->is_delta[i]) {
                size_t total = s->box->entries[i].size;
                uint8_t *rawbuf = (uint8_t *)fallback_data(s, total + 64);
                if (!rawbuf) return;
                uint8_t *out = (uint8_t *)(((uintptr_t)rawbuf + 63) & ~(uintptr_t)63);
                uint64_t sl = s->d_sl[i];
                uint32_t E = s->d_E[i];
                uint64_t bm_sz = (sl + 7) / 8;
                const uint8_t *reg = s->field + s->body_off + s->fpos[i];
                memcpy(out, reg, (size_t)sl);
                for (uint32_t e = 1; e < E; e++) {
                    const uint8_t *bm = s->field + s->body_off + s->d_boff[i] + (uint64_t)(e - 1) * bm_sz;
                    const uint8_t *vals = s->field + s->body_off + s->d_voff[i][e];
                    uint8_t *dst = out + (uint64_t)e * sl;
                    memcpy(dst, reg, (size_t)sl);
                    uint64_t vi = 0;
                    for (uint64_t k = 0; k < sl; k++)
                        if (bm[k >> 3] & (1u << (k & 7))) dst[k] ^= vals[vi++];
                }
                t->data = (void *)out;
                return;
            }
            size_t nb = ggml_nbytes(t);
            if (nb == s->box->entries[i].size) {
                t->data = (void *)(s->field + s->body_off + s->fpos[i]);
                touch_window(s, s->body_off + s->fpos[i], nb);
                return;
            }
        }
    }
    t->data = fallback_data(s, ggml_nbytes(t));
    if (!t->data) return;
    if (strcmp(name, "output.weight") == 0) {
        for (uint32_t i = 0; i < s->box->n_tensors; i++)
            if (strcmp(s->box->entries[i].name, "token_embd.weight") == 0) {
                size_t n = ggml_nelements(t);
                if (n * 4 == ggml_nbytes(t) && s->box->entries[i].size == n / 32 * 34) {
                    const uint8_t *src = s->field + s->body_off + s->fpos[i];
                    float *dst = (float *)t->data;
                    for (size_t k = 0; k < n / 32; k++) {
                        uint16_t h; memcpy(&h, src + k * 34, 2);
                        float d = ggml_fp16_to_fp32(h);
                        const int8_t *q = (const int8_t *)(src + k * 34 + 2);
                        for (int j = 0; j < 32; j++) dst[k * 32 + j] = (float)q[j] * d;
                    }
                    return;
                }
            }
    }
    if (strstr(name, ".bias")) memset(t->data, 0, ggml_nbytes(t));
    else {
        float *fd = (float *)t->data;
        size_t nf = ggml_nbytes(t) / sizeof(float);
        for (size_t i = 0; i < nf; i++) fd[i] = 1.0f;
    }
}

/* greedy generate + detokenize to text */
static int qa_one(struct llama_model *model, const char *prompt, int n_gen,
                  char *out, int cap) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.n_threads = 8; cp.n_threads_batch = 8;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) return -1;
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    int np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    if (np < 0) np = -np;
    llama_token *toks = (llama_token *)malloc((size_t)(np + 1) * sizeof(llama_token));
    np = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, np, true, false);
    if (np < 0) np = -np;
    llama_token *gen = (llama_token *)malloc((size_t)(n_gen + 1) * sizeof(llama_token));
    int got = 0;
    if (llama_decode(ctx, llama_batch_get_one(toks, np)) == 0) {
        for (int i = 0; i < n_gen; i++) {
            const float *logits = (i == 0) ? llama_get_logits_ith(ctx, np - 1) : llama_get_logits(ctx);
            llama_token best = 0; float bv = logits[0];
            for (int t = 1; t < n_vocab; t++) if (logits[t] > bv) { bv = logits[t]; best = (llama_token)t; }
            gen[got++] = best;
            if (llama_vocab_is_eog(vocab, best)) break;
            if (llama_decode(ctx, llama_batch_get_one(&best, 1)) != 0) break;
        }
    }
    int nch = llama_detokenize(vocab, gen, got, out, cap - 1, false, false);
    free(toks); free(gen); llama_free(ctx);
    if (nch < 0) { out[0] = 0; return got; }
    out[nch < cap ? nch : cap - 1] = 0;
    return got;
}

int main(int argc, char **argv) {
    const char *field_path = (argc > 1) ? argv[1] : "build/fieldA.bin";
    const char *backend = (argc > 2) ? argv[2] : "I:/llama/llama-v040-bin-win-vulkan-x64";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 60;
    if (n_gen <= 0) n_gen = 60;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    HANDLE hf = CreateFileA(field_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { printf("(cannot open %s)\n", field_path); return 1; }
    HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    const uint8_t *fmap = hm ? (const uint8_t *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!fmap) { printf("(mmap failed)\n"); return 1; }

    /* field index header layout == dual_lazy_serve Mod baked in memory:
     * [KV sans tokenizer][13 kis.*][tinfo] then body_off = align64 end.
     * Recover body_off + tokenizer addrs from kis.* keys. */
    KVInfo fk[64]; uint32_t nfk = 0;
    if (kv_walk(fmap, fk, 64, &nfk) != 0) { printf("(kv walk failed)\n"); return 1; }
    uint64_t body_off = 0;
    for (uint32_t i = 0; i < nfk; i++)
        if (strcmp(fk[i].name, "kis.layout.body_off") == 0)
            memcpy(&body_off, fmap + fk[i].val_start, 8);
    size_t fkv_end = fk[nfk - 1].end;
    size_t ftinfo_len = (size_t)(body_off - (uint64_t)fkv_end);

    /* tokenizer payload windows */
    uint64_t tok_addr[3] = {0,0,0}, tok_len[3] = {0,0,0}, tok_cnt[3] = {0,0,0};
    uint32_t tok_at[3] = {0,0,0};
    size_t tok_total = 0;
    for (int t = 0; t < 3; t++)
        for (uint32_t i = 0; i < nfk; i++) {
            if (strcmp(fk[i].name, kis_key(t, "addr")) == 0) memcpy(&tok_addr[t], fmap + fk[i].val_start, 8);
            if (strcmp(fk[i].name, kis_key(t, "len")) == 0) memcpy(&tok_len[t], fmap + fk[i].val_start, 8);
            if (strcmp(fk[i].name, kis_key(t, "count")) == 0) memcpy(&tok_cnt[t], fmap + fk[i].val_start, 8);
            if (strcmp(fk[i].name, kis_key(t, "arrtype")) == 0) memcpy(&tok_at[t], fmap + fk[i].val_start, 4);
        }
    for (int t = 0; t < 3; t++) tok_total += (size_t)tok_len[t];

    /* rebuild full header in memory (tokenizer KV restored from windows) */
    size_t reb_cap = 24 + (fkv_end - 24) + (3 * 40) + tok_total + ftinfo_len + 64;
    uint8_t *reb = (uint8_t *)calloc(1, reb_cap);
    size_t rp = 0;
    memcpy(reb + rp, fmap, 24); rp += 24;
    uint64_t nkv_reb = nfk + 3;
    memcpy(reb + 16, &nkv_reb, 8);
    memcpy(reb + rp, fmap + 24, fkv_end - 24); rp += fkv_end - 24;
    for (int t = 0; t < 3; t++) {
        uint64_t nl = strlen(tok_names[t]); uint32_t vt = 9;
        memcpy(reb + rp, &nl, 8); rp += 8;
        memcpy(reb + rp, tok_names[t], (size_t)nl); rp += (size_t)nl;
        memcpy(reb + rp, &vt, 4); rp += 4;
        memcpy(reb + rp, &tok_at[t], 4); rp += 4;
        memcpy(reb + rp, &tok_cnt[t], 8); rp += 8;
        memcpy(reb + rp, fmap + tok_addr[t], (size_t)tok_len[t]); rp += (size_t)tok_len[t];
    }
    memcpy(reb + rp, fmap + fkv_end, ftinfo_len); rp += ftinfo_len;
    size_t reb_final = (size_t)align32(rp);
    fprintf(stderr, "[qa] rebuilt header %zu B (kv=%u+3 tinfo=%zu)\n", reb_final, nfk, ftinfo_len);

    /* tensor infos live in tinfo region of the rebuilt header: parse via gguf meta */
    llama_backend_init();
    ggml_backend_load_all_from_path(backend);
    ggml_backend_load_all();
    struct ggml_context *meta_ctx = NULL;
    struct gguf_init_params ip = { .no_alloc = true, .ctx = &meta_ctx };
    struct gguf_context *meta = gguf_init_from_buffer(reb, reb_final, ip);
    if (!meta) { printf("(gguf_init from rebuilt header failed)\n"); return 1; }
    int64_t N = gguf_get_n_tensors(meta);

    /* chain order: fp32-order info not in field; recover from tinfo order ==
       file order, body positions need box entries. Recover the same order from names. */
    typedef struct { char name[128]; size_t size; } TI;
    TI *tis = (TI *)calloc((size_t)N, sizeof(TI));
    for (int64_t i = 0; i < N; i++) {
        const char *nm = gguf_get_tensor_name(meta, i);
        strncpy(tis[i].name, nm, 127);
        struct ggml_tensor *tt = ggml_get_tensor(meta_ctx, nm);
        tis[i].size = tt ? ggml_nbytes(tt) : 0;
    }
    /* inference sort (same as sort_inference) */
    uint32_t *order = (uint32_t *)calloc((size_t)N, sizeof(uint32_t));
    for (int64_t i = 0; i < N; i++) order[i] = (uint32_t)i;
    for (int64_t i = 0; i < N; i++)
        for (int64_t j = i + 1; j < N; j++) {
            unsigned ba = 0, bb = 0; int ca = 3, cb = 3;
            if (strncmp(tis[order[i]].name, "token_embd", 10) == 0) ca = 0;
            else if (strncmp(tis[order[i]].name, "blk.", 4) == 0) { ca = 1; ba = (unsigned)atoi(tis[order[i]].name + 4); }
            else if (strncmp(tis[order[i]].name, "output_norm", 11) == 0) ca = 2;
            if (strncmp(tis[order[j]].name, "token_embd", 10) == 0) cb = 0;
            else if (strncmp(tis[order[j]].name, "blk.", 4) == 0) { cb = 1; bb = (unsigned)atoi(tis[order[j]].name + 4); }
            else if (strncmp(tis[order[j]].name, "output_norm", 11) == 0) cb = 2;
            int less = (ca < cb) || (ca == cb && (ba < bb || (ba == bb && order[i] < order[j])));
            if (!less) { uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }
        }
    uint64_t *fpos = (uint64_t *)calloc((size_t)N, sizeof(uint64_t));
    { uint64_t cur = 0;
      for (int64_t r = 0; r < N; r++) { fpos[order[r]] = cur; cur += align32(tis[order[r]].size); } }

    /* GGUFBox shim for provide_tensor (name+size lookup) */
    GGUFBox box; memset(&box, 0, sizeof(box));
    box.n_tensors = (uint32_t)N;
    box.entries = (GGUFBoxEntry *)calloc((size_t)N, sizeof(GGUFBoxEntry));
    for (int64_t i = 0; i < N; i++) {
        box.entries[i].name = strdup(tis[i].name);
        box.entries[i].size = tis[i].size;
    }
    ServeCtx sc; memset(&sc, 0, sizeof(sc));
    sc.box = &box; sc.field = fmap; sc.body_off = body_off; sc.fpos = fpos;
    /* win_bits for touch_window (matching dual_lazy_serve pattern) */
    {
        DWORD fsz_hi = 0;
        DWORD fsz_lo = GetFileSize(hf, &fsz_hi);
        uint64_t file_sz = ((uint64_t)fsz_hi << 32) | fsz_lo;
        sc.n_windows = (file_sz + WIN - 1) / WIN;
        sc.win_bits = (uint8_t *)calloc(1, (sc.n_windows + 7) / 8);
    }

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    mp.no_host = true;  /* bypass Vulkan host-pinned staging (field ptrs incompatible) */
    struct llama_model *model = llama_model_init_from_user(meta, provide_tensor, &sc, mp);
    if (!model) { printf("(model init from field failed)\n"); return 1; }
    printf("field_qa ready: %s (%lld tensors) — stdin prompts, blank line quits\n", field_path, (long long)N);

    char line[4096], ans[8192];
    while (fgets(line, sizeof(line), stdin)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L) break;
        int got = qa_one(model, line, n_gen, ans, sizeof(ans));
        printf("Q: %s\nA (%d tok): %s\n\n", line, got, ans);
    }
    llama_model_free(model);
    gguf_free(meta);
    free(sc.win_bits);
    for (uint32_t i = 0; i < sc.n_owned; i++) free(sc.owned[i]);
    UnmapViewOfFile(fmap); CloseHandle(hm); CloseHandle(hf);
    llama_backend_free();
    return 0;
}
