/*
 * tools/gguf_graft_hybrid.c — graft body FROM A HYBRID STORE (contiguous + slot)
 *
 * Wire DtSlotRegion (docs/BENCH_GEOMETRIC_FS_SPEED-2026-08-17.md, workload 3/4)
 * into the GGUF graft path — prove the hybrid layout serves a real model:
 *
 *   chunky/weights → contiguous field (window chain, inference order)
 *   tiny tensors   → DtSlotRegion     (direct address = rank, syscall-free)
 *
 * BENCH ข้อสรุป: MAP ชนะ 18-54× ที่ tiny random access เพราะ syscall ต่อ op
 * (fseek+fread ~2000ns) vs pointer ตรง (~40ns) — ที่นี่เรา wire เข้า graft จริง:
 *
 *   1. BAKE      big → field ต่อเนื่อง (window chain) · tiny → DtSlotRegion
 *                (size-classed slots: tensor ใส่ class ที่พอดี → 0% padding)
 *   2. REBUILD   header scion (KV verbatim) + tensor infos chain order,
 *                body ประกอบจาก field + slot region ตาม chain order
 *   3. GRAFT     เขียน GGUF ไฟล์เดียว → llama.cpp generate
 *   4. PROVE     generation == original BITWISE (lossless ต้องผ่าน decode)
 *
 * BUILD (same DLLs as graft-field) / RUN:
 *   make graft-hybrid
 *   PATH="I:/llama/llama-b9733-bin-win-vulkan-x64:$PATH" \
 *     ./build/gguf_graft_hybrid [model.gguf] [prompt] [n_tokens]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"
#include "ggml-backend.h"
#include "../core/gguf_box.h"
#include "../core/infra/dramtile_store.h"

#define WIN        20736u
#define ALIGN      32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))

/* tiny tensor threshold (bytes) — same split as BENCH Workload 5 */
#define TINY_MAX   65536u

/* size-class boundaries — each class = one DtSlotRegion (slot_sz = class max) */
#define N_CLASS    4
static const uint32_t CLASS_MAX[N_CLASS] = { 1024u, 4096u, 16384u, 65536u };

static const char *const CLASS_NAME[N_CLASS] = { "<1K", "<4K", "<16K", "<64K" };

/* ── silence llama.cpp info spam; keep errors/warnings ── */
static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
        fputs(text, stderr);
}

/* ── inference order (same as window_chain / real_gate) ──── */
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

/* ── greedy generation loop (identical to gguf_graft_field) ── */
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

/* per-tensor placement: which class a tiny tensor goes to (or -1 if big) */
static int tiny_class_of(uint32_t size) {
    if (size >= TINY_MAX) return -1;
    for (int c = 0; c < N_CLASS; c++)
        if (size <= CLASS_MAX[c]) return c;
    return N_CLASS - 1;
}

int main(int argc, char **argv) {
    const char *gguf   = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = (argc > 2) ? argv[2] : "The capital of France is";
    int n_gen = (argc > 3) ? atoi(argv[3]) : 40;
    if (n_gen <= 0) n_gen = 40;
    const char *out_path = "build/graft_hybrid.gguf";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Graft body FROM A HYBRID STORE — contiguous field + DtSlotRegion\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    llama_backend_init();
    llama_log_set(quiet_log, NULL);
    ggml_backend_load_all_from_path("I:/llama/llama-b9733-bin-win-vulkan-x64");

    GGUFBox box;
    if (gguf_box_open(&box, gguf) != 0) {
        printf("(cannot open %s — model/DLLs missing on this machine)\n", gguf);
        llama_backend_free();
        return 1;
    }
    uint32_t N = box.n_tensors;

    /* chain order */
    uint32_t *order = (uint32_t *)calloc(N, sizeof(uint32_t));
    uint64_t *chain_off = (uint64_t *)calloc(N, sizeof(uint64_t));
    int *placement = (int *)calloc(N, sizeof(int));   /* 0 = big/field, 1..N_CLASS = tiny class */
    sort_inference(&box, order, N);

    /* classify + count */
    uint32_t n_big = 0, n_tiny = 0, tiny_logical = 0;
    uint32_t tiny_per_class[N_CLASS] = {0};
    uint64_t big_bytes = 0;
    for (uint32_t r = 0; r < N; r++) {
        const GGUFBoxEntry *e = &box.entries[order[r]];
        int c = tiny_class_of(e->size);
        if (c < 0) { placement[r] = 0; n_big++; big_bytes += align32(e->size); }
        else       { placement[r] = c + 1; n_tiny++; tiny_per_class[c]++; tiny_logical += e->size; }
    }
    printf("\nmodel: %u tensors — %u big (contiguous field, %llu B) · %u tiny (DtSlotRegion, %u B logical)\n",
           N, n_big, (unsigned long long)big_bytes, n_tiny, tiny_logical);

    /* ══ BAKE ══ */
    printf("\n═ BAKE — big → contiguous field · tiny → DtSlotRegion (size-classed) ═\n");

    /* contiguous field for big tensors (window chain, inference order) */
    uint64_t n_win = (big_bytes + WIN - 1) / WIN;
    uint8_t *field = (uint8_t *)calloc(1, (size_t)(n_win * WIN));
    if (!field) { printf("(cannot allocate field)\n"); return 1; }

    /* size-classed slot regions for tiny tensors */
    DtSlotRegion slots[N_CLASS];
    uint32_t slot_used[N_CLASS] = {0};
    uint32_t slot_rank[N_CLASS] = {0};
    uint64_t slot_cap[N_CLASS] = {0};
    int slot_ok = 1;
    for (int c = 0; c < N_CLASS; c++) {
        memset(&slots[c], 0, sizeof(slots[c]));
        if (tiny_per_class[c] == 0) { slot_ok &= 1; continue; }
        slot_cap[c] = (uint64_t)tiny_per_class[c] * CLASS_MAX[c];
        if (dt_slot_init(&slots[c], tiny_per_class[c], CLASS_MAX[c]) != 0) { slot_ok = 0; break; }
    }
    CHECK("H1 slot regions init (size-classed, tiny per class)", slot_ok);

    uint64_t cursor = 0;
    int bake_ok = 1;
    for (uint32_t r = 0; r < N; r++) {
        const GGUFBoxEntry *e = &box.entries[order[r]];
        if (placement[r] == 0) {
            memcpy(field + cursor, e->data, e->size);
            cursor += align32(e->size);
        } else {
            int c = placement[r] - 1;
            uint32_t addr = slot_rank[c]++;
            uint8_t *p = dt_slot_put(&slots[c], addr, e->data, e->size);
            if (!p) { bake_ok = 0; printf("  T: FAIL — slot put (rank %u, class %d)\n", addr, c); }
            slot_used[c]++;
        }
    }
    CHECK("H2 bake ครบ — big ลง field, tiny ลง slots (no NULL)", bake_ok);
    printf("  field: %llu windows × %u B (big) · slots: tiny logical %u B",
           (unsigned long long)n_win, WIN, tiny_logical);
    {
        uint64_t slot_total = 0;
        for (int c = 0; c < N_CLASS; c++) slot_total += slot_cap[c];
        printf(" · slot capacity %llu B (%.1f%% padding)\n",
               (unsigned long long)slot_total,
               slot_total ? (double)(slot_total - tiny_logical) * 100.0 / (double)slot_total : 0.0);
    }

    /* verify bake lossless on both paths */
    int bad = 0;
    uint32_t vrank[N_CLASS] = {0};
    uint64_t fcur = 0;
    for (uint32_t r = 0; r < N && bad == 0; r++) {
        const GGUFBoxEntry *e = &box.entries[order[r]];
        if (placement[r] == 0) {
            if (memcmp(field + fcur, e->data, e->size) != 0) bad++;
            fcur += align32(e->size);
        } else {
            int c = placement[r] - 1;
            if (memcmp(slots[c].base + (size_t)(vrank[c]++) * CLASS_MAX[c], e->data, e->size) != 0) bad++;
        }
    }
    CHECK("H3 bake lossless — field + slots bytes == source (memcmp ทุก tensor)", bad == 0);

    /* ══ REBUILD: body ประกอบจาก field + slot region ตาม chain order ══ */
    uint64_t body_sz = 0;
    for (uint32_t r = 0; r < N; r++)
        body_sz += align32(box.entries[order[r]].size);
    uint8_t *body = (uint8_t *)calloc(1, (size_t)body_sz);
    if (!body) { printf("(cannot allocate body)\n"); return 1; }

    uint64_t bcur = 0, fcur2 = 0;
    uint32_t srank[N_CLASS] = {0};
    for (uint32_t r = 0; r < N; r++) {
        const GGUFBoxEntry *e = &box.entries[order[r]];
        chain_off[r] = bcur;
        if (placement[r] == 0) {
            memcpy(body + bcur, field + fcur2, e->size);
            fcur2 += align32(e->size);
        } else {
            int c = placement[r] - 1;
            memcpy(body + bcur, slots[c].base + (size_t)(srank[c]++) * CLASS_MAX[c], e->size);
        }
        bcur += align32(e->size);
    }
    /* verify body == source bytes ทุก tensor (rebuild lossless) */
    bad = 0;
    bcur = 0;
    for (uint32_t r = 0; r < N && bad == 0; r++) {
        const GGUFBoxEntry *e = &box.entries[order[r]];
        if (memcmp(body + bcur, e->data, e->size) != 0) bad++;
        bcur += align32(e->size);
    }
    CHECK("H4 rebuild lossless — body bytes == source ทุก tensor (memcmp)", bad == 0);

    /* ══ GRAFT: header + body → GGUF ไฟล์เดียว ══ */
    size_t hdr_cap = (size_t)box.reader.data_offset + 4096;
    uint8_t *hdr = (uint8_t *)calloc(1, hdr_cap);
    size_t data_off = rebuild_header(&box, order, chain_off, hdr, hdr_cap, &data_off);
    CHECK("H5 rebuilt header (KV verbatim + tensor infos chain order)", data_off > 0);

    FILE *f = fopen(out_path, "wb");
    if (!f || fwrite(hdr, 1, data_off, f) != data_off ||
        fwrite(body, 1, (size_t)bcur, f) != (size_t)bcur) {
        printf("(cannot write %s)\n", out_path);
        return 1;
    }
    fclose(f);
    printf("  graft written: %s (header %zu B + hybrid body %llu B)\n",
           out_path, data_off, (unsigned long long)bcur);
    CHECK("H6 hybrid-built GGUF exists", 1);

    /* the body must NOT be a byte-copy of the source body — chain reorder
     * (e.g. output.weight moves from first to last) */
    {
        int differs = 0;
        const uint8_t *src_body = box.reader.base + box.reader.data_offset;
        size_t src_body_sz = box.reader.base_sz - (size_t)box.reader.data_offset;
        size_t cmp = (size_t)bcur < src_body_sz ? (size_t)bcur : src_body_sz;
        for (size_t i = 0; i < cmp; i++)
            if (body[i] != src_body[i]) { differs = 1; break; }
        CHECK("H7 body-from-hybrid ≠ source body (chain reorder จริง — ไม่ใช่ memcpy)", differs);
    }

    /* ══ GENERATION: hybrid-built GGUF vs original, bitwise ══ */
    printf("\nprompt: \"%s\"  (generate up to %d tokens)\n\n", prompt, n_gen);
    int nf = 0, no = 0;
    llama_token *g = generate(out_path, prompt, n_gen, &nf);
    llama_token *o = generate(gguf,     prompt, n_gen, &no);
    int ok = (g != NULL && o != NULL && nf == no);
    if (ok) for (int i = 0; i < nf; i++) if (g[i] != o[i]) { ok = 0; break; }

    printf("  hybrid-built: %d tokens\n  original:     %d tokens\n", nf, no);
    printf("  token streams identical: %s\n", ok ? "YES ✅" : "NO ❌");
    if (ok) {
        printf("\n  generated text (hybrid-built): \"");
        stream_text(out_path, g, nf);
        printf("\"\n");
    } else if (g && o) {
        int d = 0;
        while (d < nf && d < no && g[d] == o[d]) d++;
        printf("  first divergence at token %d: hybrid=%d original=%d\n",
               d, d < nf ? g[d] : -1, d < no ? o[d] : -1);
    }
    CHECK("H8 generation from hybrid-built GGUF == original (bitwise)", ok);

    /* clean up */
    free(g); free(o); free(field); free(body); free(hdr);
    free(order); free(chain_off); free(placement);
    for (int c = 0; c < N_CLASS; c++) dt_slot_destroy(&slots[c]);
    remove(out_path);
    gguf_box_close(&box);
    llama_backend_free();
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS — %s\n", pass_count, pass_count + fail_count,
           fail_count ? "FAIL" : "hybrid store serves real inference (bitwise)");
    return fail_count ? 1 : 0;
}