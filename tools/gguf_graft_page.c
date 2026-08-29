/*
 * tools/gguf_graft_page.c — tokenizer KV → page field, header 5.9MB → ~180KB
 *
 * Step ⑤: the tokenizer (151k strings) is DATA — it moves into the KIS field
 * (window chain) exactly like tensor data. The graft header no longer carries
 * it. The header's tokenizer arrays shrink 5,947,744 B → ~180,000 B:
 *
 *   BAKE     tensor bytes → field (chain, step ④)
 *            tokenizer tokens/merges/token_type → field payloads (GGUF arr
 *            element layout, verbatim), address = win×WIN+slot recorded
 *   HEADER   small graft artifact: arch KV verbatim (no tokenizer) + pointer
 *            KV kis.tokenizer.<x>.addr/count + tensor infos → ~32× smaller
 *   SERVE    in-memory full GGUF = small KV + tokenizer arrays gathered FROM
 *            THE FIELD + tensor infos + data section = field bytes
 *            → gguf_init_from_buffer → llama_model_init_from_user
 *            (callback serves weights from the field — gcube_token_run pattern)
 *   GENERATE bitwise == original (llama never touches the source file)
 *
 * Why not a separate vocab file: this llama build (b9733) has no vocab_file
 * API — the vocab can only be built from the KV of the gguf context it loads.
 * So the tokenizer KV is materialized from the field into the in-memory
 * context; the on-disk graft is small.
 *
 * BUILD / RUN: make graft-page
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
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

/* ── KV walk: classify each pair, locate the 3 tokenizer arrays ── */
typedef struct {
    size_t start, end;      /* byte range of the whole pair (incl. name+type) */
    size_t val_start;       /* where the value starts (after type u32) */
    char   name[64];
    int    is_tok;          /* 1 = tokens/merges/token_type array */
    uint32_t arr_type;      /* GGUF element type (8=str, 4=i32) */
    uint64_t arr_count;
} KVInfo;

static int kv_walk(const uint8_t *base, const uint8_t *kv_end_abs,
                   KVInfo *infos, int cap, uint32_t *n_out) {
    const uint8_t *p = base + 24, *end = kv_end_abs;
    uint32_t n = 0;
    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    while (p < end && n < (uint32_t)cap) {
        KVInfo *k = &infos[n];
        k->start = (size_t)(p - base);
        uint64_t klen; uint32_t vtype;
        memcpy(&klen, p, 8); p += 8;
        size_t kpos = (size_t)(p - base);
        memcpy(k->name, p, klen < 63 ? klen : 63); k->name[klen < 63 ? klen : 63] = 0;
        p += klen;
        memcpy(&vtype, p, 4); p += 4;
        k->val_start = (size_t)(p - base);
        k->is_tok = 0;
        k->arr_type = 0; k->arr_count = 0;
        if (strcmp(k->name, "tokenizer.ggml.tokens") == 0 ||
            strcmp(k->name, "tokenizer.ggml.merges") == 0 ||
            strcmp(k->name, "tokenizer.ggml.token_type") == 0) k->is_tok = 1;
        if (vtype == 9) {
            uint32_t at; uint64_t narr;
            memcpy(&at, p, 4); p += 4;
            memcpy(&narr, p, 8); p += 8;
            k->arr_type = at; k->arr_count = narr;
            if (at == 8) {
                for (uint64_t a = 0; a < narr; a++) {
                    uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl;
                }
            } else if (at < 13) {
                p += (size_t)vsz[at] * narr;
            } else return -1;
        } else if (vtype == 8) {
            uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl;
        } else if (vtype <= 12) {
            p += vsz[vtype];
        } else return -1;
        k->end = (size_t)(p - base);
        (void)kpos;
        n++;
    }
    *n_out = n;
    return 0;
}

/* ── tokenizer payload → field (GGUF arr elements verbatim) ── */
static uint64_t bake_blob(uint8_t *field, uint64_t cursor, const uint8_t *src,
                          size_t n) {
    memcpy(field + cursor, src, n);
    return cursor + n;
}

/* ── greedy generation (same as the other graft tools) ── */
static llama_token *generate(const char *gguf_path, const char *prompt,
                             int n_gen, int *n_out) {
    *n_out = 0;
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(gguf_path, mp);
    if (!model) return NULL;
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.n_threads = 4; cp.n_threads_batch = 4;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { llama_model_free(model); return NULL; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    llama_token eos = llama_vocab_eos(vocab);
    int n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    if (n_prompt < 0) n_prompt = -n_prompt;
    llama_token *toks = (llama_token *)malloc((size_t)(n_prompt + 1) * sizeof(llama_token));
    int n2 = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, n_prompt, true, false);
    if (n2 < 0) n2 = -n2;
    n_prompt = n2;
    llama_token *out = (llama_token *)malloc((size_t)(n_gen + 1) * sizeof(llama_token));
    int total = 0;
    if (llama_decode(ctx, llama_batch_get_one(toks, n_prompt)) != 0) {
        free(toks); free(out); llama_free(ctx); llama_model_free(model); return NULL;
    }
    for (int i = 0; i < n_gen; i++) {
        const float *logits = (i == 0) ? llama_get_logits_ith(ctx, n_prompt - 1)
                                       : llama_get_logits(ctx);
        llama_token best = 0; float best_v = logits[0];
        for (int t = 1; t < n_vocab; t++) if (logits[t] > best_v) { best_v = logits[t]; best = (llama_token)t; }
        out[total++] = best;
        if (best == eos) break;
        if (llama_decode(ctx, llama_batch_get_one(&best, 1)) != 0) break;
    }
    free(toks); llama_free(ctx); llama_model_free(model);
    *n_out = total;
    return out;
}

/* ── serve weights from the field (llama_model_init_from_user callback) ── */
typedef struct {
    const GGUFBox *box;
    const uint8_t *field;
    const uint64_t *fpos;   /* file-idx → field position */
    uint32_t matched, missing;
} ServeCtx;

static void provide_tensor(struct ggml_tensor *t, void *ud) {
    ServeCtx *s = (ServeCtx *)ud;
    const char *name = ggml_get_name(t);
    for (uint32_t i = 0; i < s->box->n_tensors; i++) {
        if (strcmp(s->box->entries[i].name, name) == 0) {
            size_t nb = ggml_nbytes(t);
            if (nb != s->box->entries[i].size) {
                fprintf(stderr, "  [serve] SIZE MISMATCH %s: field=%u llama=%zu\n",
                        name, s->box->entries[i].size, nb);
                s->missing++;
                return;
            }
            memcpy(t->data, s->field + s->fpos[i], nb);
            s->matched++;
            return;
        }
    }
    /* llama's schema tensor absent from this GGUF (bias/scale) — zero-fill */
    if (s->missing < 12)
        fprintf(stderr, "  [serve] MISSING %s (%lld elems, %s)\n",
                name, (long long)ggml_nelements(t), ggml_type_name(t->type));
    memset(t->data, 0, ggml_nbytes(t));
    s->missing++;
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

int main(int argc, char **argv) {
    const char *gguf   = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = (argc > 2) ? argv[2] : "The capital of France is";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 40;
    if (n_gen <= 0) n_gen = 40;
    const char *page_path = "build/graft_page.gguf";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Tokenizer KV → page field — graft header 5.9MB → ~180KB\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    llama_backend_init();
    llama_log_set(quiet_log, NULL);
    ggml_backend_load_all_from_path("I:/llama/llama-b9733-bin-win-vulkan-x64");

    GGUFBox box;
    if (gguf_box_open(&box, gguf) != 0) {
        printf("(cannot open %s — model/DLLs missing)\n", gguf);
        llama_backend_free();
        return 1;
    }
    uint32_t N = box.n_tensors;

    /* ── measure the header: KV section vs the 3 tokenizer arrays ── */
    const uint8_t *kv_end_p = NULL;
    size_t hdr_sz = (size_t)box.reader.data_offset;
    {
        const uint8_t *p = box.reader.base + 24, *e = box.reader.base + hdr_sz;
        uint64_t nkv; memcpy(&nkv, box.reader.base + 16, 8);
        static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
        for (uint64_t k = 0; k < nkv; k++) {
            uint64_t klen; uint32_t vt;
            memcpy(&klen, p, 8); p += 8; p += klen;
            memcpy(&vt, p, 4); p += 4;
            if (vt == 9) {
                uint32_t at; uint64_t narr; memcpy(&at, p, 4); p += 4; memcpy(&narr, p, 8); p += 8;
                if (at == 8) { for (uint64_t a = 0; a < narr; a++) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; } }
                else p += (size_t)vsz[at] * narr;
            } else if (vt == 8) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; }
            else p += vsz[vt];
        }
        kv_end_p = p;
    }
    KVInfo kvs[64];
    uint32_t n_kv = 0;
    if (kv_walk(box.reader.base, kv_end_p, kvs, 64, &n_kv) != 0) {
        printf("(KV walk failed)\n"); return 1;
    }
    uint64_t kv_total = 0, tok_total = 0, small_total = 0;
    for (uint32_t i = 0; i < n_kv; i++) {
        kv_total += kvs[i].end - kvs[i].start;
        if (kvs[i].is_tok) tok_total += kvs[i].end - kvs[i].start;
        else small_total += kvs[i].end - kvs[i].start;
    }
    printf("  header %zu B = KV %llu B (tokenizer %llu B, other %llu B) + tensor infos %llu B\n",
           hdr_sz, (unsigned long long)kv_total, (unsigned long long)tok_total,
           (unsigned long long)small_total,
           (unsigned long long)(hdr_sz - kv_total));

    /* ── bake: tensors → field (chain) + tokenizer blobs → field ── */
    uint32_t *order = (uint32_t *)calloc(N, sizeof(uint32_t));
    uint64_t *chain_off = (uint64_t *)calloc(N, sizeof(uint64_t));
    sort_inference(&box, order, N);

    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < N; i++) total_bytes += align32(box.entries[i].size);
    uint64_t field_sz = ((total_bytes + tok_total + WIN - 1) / WIN) * WIN;
    uint8_t *field = (uint8_t *)calloc(1, (size_t)field_sz);
    if (!field) return 1;

    uint64_t cursor = 0;
    int lossless = 1;
    for (uint32_t r = 0; r < N; r++) {
        const GGUFBoxEntry *e = &box.entries[order[r]];
        chain_off[r] = cursor;
        memcpy(field + cursor, e->data, e->size);
        if (memcmp(field + cursor, e->data, e->size) != 0) lossless = 0;
        cursor += align32(e->size);
    }
    CHECK("P1: bake lossless — tensor bytes == field (memcmp)", lossless);

    /* tokenizer payloads → field (arr elements verbatim, addresses recorded) */
    uint64_t tok_addr[3]; uint64_t tok_count[3]; uint32_t tok_type[3];
    size_t   tok_elen[3];
    const char *tok_names[3] = { "tokenizer.ggml.tokens", "tokenizer.ggml.merges",
                                 "tokenizer.ggml.token_type" };
    const uint8_t *src = box.reader.base;
    int found = 0;
    for (uint32_t i = 0; i < n_kv; i++) {
        for (int t = 0; t < 3; t++) {
            if (strcmp(kvs[i].name, tok_names[t]) == 0) {
                /* value = u32 arr_type + u64 count + elements */
                const uint8_t *val = src + kvs[i].val_start;
                size_t elems_off = kvs[i].val_start + 12;
                size_t elems_len = kvs[i].end - elems_off;
                tok_addr[t] = cursor;
                tok_count[t] = kvs[i].arr_count;
                tok_type[t] = kvs[i].arr_type;
                tok_elen[t] = elems_len;
                cursor = bake_blob(field, cursor, val + 12, elems_len);
                printf("  %-28s → field win %llu slot %llu (%llu elems, %zu B)\n",
                       tok_names[t],
                       (unsigned long long)(tok_addr[t] / WIN),
                       (unsigned long long)(tok_addr[t] % WIN),
                       (unsigned long long)kvs[i].arr_count, elems_len);
                found++;
            }
        }
    }
    CHECK("P2: tokenizer 3 arrays อยู่ใน field (tokens/merges/token_type)", found == 3);

    /* ── small graft header (NO tokenizer KV + pointer keys) ── */
    uint64_t n_win = (cursor + WIN - 1) / WIN;
    printf("  field: %llu windows (cursor %llu B, tokenizer %llu B)\n",
           (unsigned long long)n_win, (unsigned long long)cursor,
           (unsigned long long)(tok_total - 3 * 12));
    {
        /* KV: copy all non-tokenizer pairs verbatim, in order */
        size_t small_hdr_cap = (size_t)box.reader.data_offset;
        uint8_t *hdr = (uint8_t *)calloc(1, small_hdr_cap);
        size_t pos = 0;
        /* magic/version/n_tensors/n_kv = 24 B, n_kv = small + 6 pointer keys */
        uint32_t magic = GGUF_MAGIC, version = 3;
        uint64_t nt = N;
        uint64_t nkv_small = (uint64_t)(n_kv - 3) + 6;
        memcpy(hdr + pos, &magic, 4); pos += 4;
        memcpy(hdr + pos, &version, 4); pos += 4;
        memcpy(hdr + pos, &nt, 8); pos += 8;
        memcpy(hdr + pos, &nkv_small, 8); pos += 8;
        for (uint32_t i = 0; i < n_kv; i++) {
            if (kvs[i].is_tok) continue;
            size_t len = kvs[i].end - kvs[i].start;
            memcpy(hdr + pos, src + kvs[i].start, len);
            pos += len;
        }
        /* pointer keys: kis.tokenizer.<x>.addr / .count (u64) */
        for (int t = 0; t < 3; t++) {
            char kname[64];
            uint64_t v = tok_addr[t];
            snprintf(kname, sizeof(kname), "kis.tokenizer.%s.addr",
                     t == 0 ? "tokens" : t == 1 ? "merges" : "token_type");
            uint64_t klen = strlen(kname);
            memcpy(hdr + pos, &klen, 8); pos += 8;
            memcpy(hdr + pos, kname, klen); pos += klen;
            uint32_t vt = 10; /* UINT64 */
            memcpy(hdr + pos, &vt, 4); pos += 4;
            memcpy(hdr + pos, &v, 8); pos += 8;
            v = tok_count[t];
            snprintf(kname, sizeof(kname), "kis.tokenizer.%s.count",
                     t == 0 ? "tokens" : t == 1 ? "merges" : "token_type");
            klen = strlen(kname);
            memcpy(hdr + pos, &klen, 8); pos += 8;
            memcpy(hdr + pos, kname, klen); pos += klen;
            memcpy(hdr + pos, &vt, 4); pos += 4;
            memcpy(hdr + pos, &v, 8); pos += 8;
        }
        /* tensor infos in chain order */
        for (uint32_t r = 0; r < N; r++) {
            const GGUFBoxEntry *e = &box.entries[order[r]];
            uint64_t nlen = strlen(e->name);
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
        size_t page_sz = align32(pos);
        /* write the small artifact */
        FILE *f = fopen(page_path, "wb");
        if (!f || fwrite(hdr, 1, page_sz, f) != page_sz) { printf("(write fail)\n"); return 1; }
        fclose(f);
        printf("  graft page header: %zu B (was %zu B — %.0f× smaller)\n",
               page_sz, hdr_sz, (double)hdr_sz / (double)page_sz);
        CHECK("P3: header ถูกตัด — tokenizer ไม่อยู่ใน header แล้ว (5.9MB → <1MB)",
              page_sz < 1000000u);
        /* re-parse the small artifact: walk ITS KV — no tokenizer, pointers */
        {
            FILE *rf = fopen(page_path, "rb");
            uint8_t *abuf = NULL; size_t asz = 0;
            if (rf) {
                fseek(rf, 0, SEEK_END); asz = (size_t)ftell(rf); fseek(rf, 0, SEEK_SET);
                abuf = (uint8_t *)malloc(asz);
                if (fread(abuf, 1, asz, rf) != asz) { free(abuf); abuf = NULL; }
                fclose(rf);
            }
            int rc = -1, no_tok = 1, has_ptr = 0;
            if (abuf && asz >= 24) {
                const uint8_t *p = abuf + 24, *e = abuf + asz;
                uint64_t nkv; memcpy(&nkv, abuf + 16, 8);
                static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
                rc = 0;
                for (uint64_t k = 0; k < nkv && rc == 0; k++) {
                    uint64_t klen; uint32_t vt;
                    if ((size_t)(e - p) < 8) { rc = -1; break; }
                    memcpy(&klen, p, 8); p += 8;
                    if ((size_t)(e - p) < klen) { rc = -1; break; }
                    char nm[64] = {0};
                    memcpy(nm, p, klen < 63 ? klen : 63); p += klen;
                    if (strstr(nm, "tokenizer.ggml.tokens") || strstr(nm, "tokenizer.ggml.merges") ||
                        strstr(nm, "tokenizer.ggml.token_type")) no_tok = 0;
                    if (strstr(nm, "kis.tokenizer.tokens.addr")) has_ptr = 1;
                    if ((size_t)(e - p) < 4) { rc = -1; break; }
                    memcpy(&vt, p, 4); p += 4;
                    if (vt == 9) {
                        uint32_t at; uint64_t narr;
                        if ((size_t)(e - p) < 12) { rc = -1; break; }
                        memcpy(&at, p, 4); p += 4; memcpy(&narr, p, 8); p += 8;
                        if (at == 8) {
                            for (uint64_t a = 0; a < narr && rc == 0; a++) {
                                uint64_t sl;
                                if ((size_t)(e - p) < 8) { rc = -1; break; }
                                memcpy(&sl, p, 8); p += 8;
                                if ((size_t)(e - p) < sl) { rc = -1; break; }
                                p += sl;
                            }
                        } else if (at < 13) {
                            if ((size_t)(e - p) < (size_t)vsz[at] * narr) { rc = -1; break; }
                            p += (size_t)vsz[at] * narr;
                        } else rc = -1;
                    } else if (vt == 8) {
                        uint64_t sl;
                        if ((size_t)(e - p) < 8) { rc = -1; break; }
                        memcpy(&sl, p, 8); p += 8;
                        if ((size_t)(e - p) < sl) { rc = -1; break; }
                        p += sl;
                    } else if (vt <= 12) {
                        if ((size_t)(e - p) < (size_t)vsz[vt]) { rc = -1; break; }
                        p += vsz[vt];
                    } else rc = -1;
                }
                free(abuf);
            }
            CHECK("P3b: small artifact เป็น GGUF valid — ไม่มี tokenizer, มี pointer key", rc == 0 && no_tok && has_ptr);
        }
        free(hdr);
    }

    /* ── serve: build full GGUF from the FIELD → temp file → llama loads it ──
     * NOTE: we do NOT use llama_model_init_from_user here. Isolated test
     * (build/iso_user_path.c): even with meta from the ORIGINAL file, the
     * b9733 user path creates 337 optional tensors (output.bias + *.scale +
     * *.input_scale) which the callback zero-fills — and the resulting
     * graph output DIFFERS from file-load. The file path (proven in step ④)
     * serves identical weights; the durable artifact stays the small page
     * header. ── */
    printf("\n═ SERVE — full GGUF ประกอบจาก field → temp file → llama ═\n");
    {
        /* full buffer = header (24) + small KV + 3 tokenizer arrays + tensor
         * infos + pad + field bytes */
        size_t small_kv_sz = (size_t)(small_total + 6 * 24);
        size_t tinfo_sz = 0;
        for (uint32_t i = 0; i < N; i++) {
            const GGUFBoxEntry *e = &box.entries[i];
            tinfo_sz += 8 + strlen(e->name) + 4 + (size_t)e->n_dims * 8 + 4 + 8;
        }
        size_t arr_hdr = 12; /* u32 type + u64 count per array */
        size_t buf_sz = 24 + small_kv_sz + 3 * arr_hdr + (size_t)(tok_total - 3 * 12)
                      + tinfo_sz + ALIGN + (size_t)cursor;
        uint8_t *buf = (uint8_t *)calloc(1, buf_sz);
        size_t pos = 0;
        uint32_t magic = GGUF_MAGIC, version = 3;
        uint64_t nt = N, nkv_full = n_kv;
        memcpy(buf + pos, &magic, 4); pos += 4;
        memcpy(buf + pos, &version, 4); pos += 4;
        memcpy(buf + pos, &nt, 8); pos += 8;
        memcpy(buf + pos, &nkv_full, 8); pos += 8;
        for (uint32_t i = 0; i < n_kv; i++) {
            if (kvs[i].is_tok) continue;
            size_t len = kvs[i].end - kvs[i].start;
            memcpy(buf + pos, src + kvs[i].start, len);
            pos += len;
        }
        for (int t = 0; t < 3; t++) {
            /* find the tok KV slot to get its name + arr type */
            uint64_t klen = strlen(tok_names[t]);
            memcpy(buf + pos, &klen, 8); pos += 8;
            memcpy(buf + pos, tok_names[t], klen); pos += klen;
            uint32_t vt = 9; /* ARRAY */
            memcpy(buf + pos, &vt, 4); pos += 4;
            memcpy(buf + pos, &tok_type[t], 4); pos += 4;
            memcpy(buf + pos, &tok_count[t], 8); pos += 8;
            /* elements verbatim from the field */
            memcpy(buf + pos, field + tok_addr[t], tok_elen[t]);
            pos += tok_elen[t];
        }
        for (uint32_t r = 0; r < N; r++) {
            const GGUFBoxEntry *e = &box.entries[order[r]];
            uint64_t nlen = strlen(e->name);
            memcpy(buf + pos, &nlen, 8); pos += 8;
            memcpy(buf + pos, e->name, nlen); pos += nlen;
            uint32_t nd = e->n_dims;
            memcpy(buf + pos, &nd, 4); pos += 4;
            for (uint32_t d = 0; d < nd; d++) {
                int64_t dv = (int64_t)e->dims[d];
                memcpy(buf + pos, &dv, 8); pos += 8;
            }
            memcpy(buf + pos, &e->dtype, 4); pos += 4;
            memcpy(buf + pos, &chain_off[r], 8); pos += 8;
        }
        size_t data_off = align32(pos);
        memset(buf + pos, 0, data_off - pos);
        /* data section = field bytes */
        memcpy(buf + data_off, field, (size_t)cursor);
        printf("  in-memory GGUF: %zu B (header %zu B + field body %llu B)\n",
               buf_sz, data_off, (unsigned long long)cursor);

        (void)buf_sz;
        /* write the full GGUF (header from field-tokenizer KV + field body) */
        const char *full_path = "build/graft_full.gguf";
        FILE *f = fopen(full_path, "wb");
        if (!f || fwrite(buf, 1, data_off + (size_t)cursor, f) != data_off + (size_t)cursor) {
            printf("(cannot write %s)\n", full_path);
            return 1;
        }
        fclose(f);
        struct llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        struct llama_model *model = llama_model_load_from_file(full_path, mp);
        CHECK("P4: llama โหลด GGUF ที่ประกอบจาก field (vocab + weights จาก field)", model != NULL);
        if (model) {
            const struct llama_vocab *vocab = llama_model_get_vocab(model);
            printf("  vocab: %d tokens (tokenizer มาจาก field)\n", llama_vocab_n_tokens(vocab));
            printf("  vocab[0..3] = \"%s\" \"%s\" \"%s\" \"%s\" (expect ! \" # $)\n",
                   llama_vocab_get_text(vocab, 0), llama_vocab_get_text(vocab, 1),
                   llama_vocab_get_text(vocab, 2), llama_vocab_get_text(vocab, 3));
            CHECK("P4b: vocab ครบ 151936 + token text ตรง",
                  llama_vocab_n_tokens(vocab) == 151936 &&
                  strcmp(llama_vocab_get_text(vocab, 0), "!") == 0 &&
                  strcmp(llama_vocab_get_text(vocab, 1), "\"") == 0);

                struct llama_context_params cp = llama_context_default_params();
                cp.n_ctx = 2048; cp.n_batch = 512; cp.n_threads = 4; cp.n_threads_batch = 4;
                struct llama_context *ctx = llama_init_from_model(model, cp);
                CHECK("P6: llama_init_from_model — context จาก field-built model", ctx != NULL);
                if (ctx) {
                    int n_vocab = llama_vocab_n_tokens(vocab);
                    llama_token eos = llama_vocab_eos(vocab);
                    int n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
                    if (n_prompt < 0) n_prompt = -n_prompt;
                    llama_token *toks = (llama_token *)malloc((size_t)(n_prompt + 1) * sizeof(llama_token));
                    int n2 = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, n_prompt, true, false);
                    if (n2 < 0) n2 = -n2;
                    n_prompt = n2;
                    llama_token *out = (llama_token *)malloc((size_t)(n_gen + 1) * sizeof(llama_token));
                    int total = 0;
                    int ok1 = llama_decode(ctx, llama_batch_get_one(toks, n_prompt)) == 0;
                    for (int i = 0; i < n_gen && ok1; i++) {
                        const float *logits = (i == 0) ? llama_get_logits_ith(ctx, n_prompt - 1)
                                                       : llama_get_logits(ctx);
                        llama_token best = 0; float best_v = logits[0];
                        for (int t = 1; t < n_vocab; t++) if (logits[t] > best_v) { best_v = logits[t]; best = (llama_token)t; }
                        out[total++] = best;
                        if (best == eos) break;
                        if (llama_decode(ctx, llama_batch_get_one(&best, 1)) != 0) break;
                    }
                    /* compare with the original file run */
                    int no = 0;
                    llama_token *o = generate(gguf, prompt, n_gen, &no);
                    int ok = (total == no);
                    if (ok) for (int i = 0; i < total; i++) if (out[i] != o[i]) { ok = 0; break; }
                    printf("\nprompt: \"%s\"  (generate up to %d tokens)\n\n", prompt, n_gen);
                    printf("  field-served: %d tokens\n  original:     %d tokens\n", total, no);
                    printf("  token streams identical: %s\n", ok ? "YES ✅" : "NO ❌");
                    if (!ok) {
                        printf("  field:     ");
                        for (int i = 0; i < total && i < 10; i++) printf(" %d", out[i]);
                        printf("\n  original:  ");
                        for (int i = 0; i < no && i < 10; i++) printf(" %d", o[i]);
                        printf("\n");
                    }
                    if (ok) {
                        printf("\n  generated text (field-served): \"");
                        stream_text(gguf, out, total);
                        printf("\"\n");
                    }
                    CHECK("P7: generation จาก field-served model == ต้นฉบับ (bitwise)", ok);
                    free(o); free(out); free(toks);
                    llama_free(ctx);
                }
                llama_model_free(model);
            }
            remove(full_path);
            free(buf);
        }

    /* clean up */
    free(field); free(order); free(chain_off);
    remove(page_path);
    gguf_box_close(&box);
    llama_backend_free();
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS — %s\n", pass_count, pass_count + fail_count,
           fail_count ? "FAIL" : "tokenizer lives in the field; header cut");
    return fail_count ? 1 : 0;
}
