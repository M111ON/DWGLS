/* test_gguf_graft_llama.c — Step ③: the cactus graft meets llama.cpp
 *
 * Cactus graft (reference-to-source):
 *   HEAD  = real GGUF header (magic + KV + tensor info) — the scion
 *   BODY  = the empty box — data served zero-copy from the source mmap
 *
 * We assemble a graft file from gguf_box's mmap:
 *     [header bytes 0..data_offset) + [body bytes data_offset..end)
 * then hand it to llama.cpp:
 *
 *   T1  graft is a valid GGUF (re-parses with gguf_reader, same tensors)
 *   T2  llama loads the graft — metadata matches Qwen2.5-0.5B (embd/layer/vocab)
 *   T3  decode 1 prompt on the graft vs on the ORIGINAL file →
 *       next-token logits BITWISE identical (deterministic CPU, same bytes)
 *   T4  reroute link: patch ONE tensor's data offset in the header
 *       (blk.0.attn_q.weight ← blk.1.attn_q.weight, same shape) →
 *       llama still loads but logits CHANGE — the scion controls routing
 *
 * BUILD (llama.cpp at I:/llama, DLLs at I:/llama/llama-b9733-bin-win-vulkan-x64):
 *   gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
 *       -I core -I I:/llama/include -o build/test_gguf_graft_llama \
 *       tests/test_gguf_graft_llama.c \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *       I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll \
 *       -lzstd -lm
 *   PATH="I:/llama/llama-b9733-bin-win-vulkan-x64:$PATH" ./build/test_gguf_graft_llama
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "llama.h"
#include "ggml-backend.h"
#include "../core/gguf_box.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── header walk: locate each tensor's offset field position ── */
typedef struct {
    char      name[128];
    uint64_t  offset;          /* value in the header (rel. to data section) */
    size_t    field_pos;       /* byte position of the 8-byte offset field */
    size_t    name_pos;        /* byte position of the name string */
    size_t    name_len;        /* name length (no null terminator in GGUF) */
} TField;

static int gbuf_skip_bytes(const uint8_t **p, const uint8_t *end, size_t n) {
    if ((size_t)(end - *p) < n) return -1;
    *p += n;
    return 0;
}
static int gbuf_get_u64(const uint8_t **p, const uint8_t *end, uint64_t *v) {
    if ((size_t)(end - *p) < 8) return -1;
    memcpy(v, *p, 8); *p += 8;
    return 0;
}
static int gbuf_get_u32(const uint8_t **p, const uint8_t *end, uint32_t *v) {
    if ((size_t)(end - *p) < 4) return -1;
    memcpy(v, *p, 4); *p += 4;
    return 0;
}

/* mirror gguf_reader's KV skip — walk the header and record tensor offsets */
static int walk_header(const uint8_t *hdr, size_t sz, TField *fields,
                       uint32_t *n_fields, uint64_t *data_offset_out) {
    const uint8_t *p = hdr, *end = hdr + sz;
    uint32_t magic, version;
    uint64_t n_tensors, n_kv;
    if (gbuf_get_u32(&p, end, &magic) || magic != GGUF_MAGIC) return -1;
    if (gbuf_get_u32(&p, end, &version)) return -1;
    if (gbuf_get_u64(&p, end, &n_tensors)) return -1;
    if (gbuf_get_u64(&p, end, &n_kv)) return -1;

    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen; uint32_t vtype;
        if (gbuf_get_u64(&p, end, &klen) || gbuf_skip_bytes(&p, end, (size_t)klen)) return -1;
        if (gbuf_get_u32(&p, end, &vtype)) return -1;
        if (vtype == 9) {
            uint32_t at; uint64_t narr;
            if (gbuf_get_u32(&p, end, &at) || gbuf_get_u64(&p, end, &narr)) return -1;
            for (uint64_t a = 0; a < narr; a++) {
                if (at == 8) { uint64_t sl; if (gbuf_get_u64(&p, end, &sl) || gbuf_skip_bytes(&p, end, (size_t)sl)) return -1; }
                else if (at < 13) { if (gbuf_skip_bytes(&p, end, (size_t)vsz[at] * (size_t)narr)) return -1; break; }
                else return -1;
            }
        } else if (vtype == 8) {
            uint64_t sl;
            if (gbuf_get_u64(&p, end, &sl) || gbuf_skip_bytes(&p, end, (size_t)sl)) return -1;
        } else if (vtype <= 12) {
            if (gbuf_skip_bytes(&p, end, vsz[vtype])) return -1;
        } else return -1;
    }

    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen;
        if (gbuf_get_u64(&p, end, &nlen) || nlen == 0 || nlen > 1000) return -1;
        if (p + nlen > end) return -1;
        if (i < *n_fields) {
            size_t cl = nlen < 127 ? (size_t)nlen : 127;
            memcpy(fields[i].name, p, cl);
            fields[i].name[cl] = 0;
            fields[i].name_pos = (size_t)(p - hdr);
            fields[i].name_len = (size_t)nlen;
        }
        if (gbuf_skip_bytes(&p, end, (size_t)nlen)) return -1;
        uint32_t nd;
        if (gbuf_get_u32(&p, end, &nd) || nd > 4) return -1;
        if (gbuf_skip_bytes(&p, end, (size_t)nd * 8)) return -1;
        uint32_t dtype;
        if (gbuf_get_u32(&p, end, &dtype)) return -1;
        if (i < *n_fields) fields[i].field_pos = (size_t)(p - hdr);
        uint64_t off;
        if (gbuf_get_u64(&p, end, &off)) return -1;
        if (i < *n_fields) fields[i].offset = off;
    }
    *n_fields = (uint32_t)n_tensors;
    *data_offset_out = (uint64_t)(p - hdr);
    return 0;
}

/* ── graft assembly: header + body, both from the box's mmap ── */
static int graft_build(const char *path, const GGUFBox *box, const uint8_t *hdr,
                       size_t hdr_sz) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rc = 0;
    if (fwrite(hdr, 1, hdr_sz, f) != hdr_sz) rc = -1;
    size_t body_sz = box->reader.base_sz - box->reader.data_offset;
    if (rc == 0 && fwrite(box->reader.base + box->reader.data_offset, 1, body_sz, f) != body_sz)
        rc = -1;
    fclose(f);
    return rc;
}

/* ── llama decode of one prompt → next-token logits ── */
static float *decode_next(GGUFBox *box, const char *gguf_path, const char *prompt,
                          uint32_t *n_vocab_out, int32_t *next_token_out,
                          int32_t *n_tokens_out) {
    (void)box;
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(gguf_path, mp);
    if (!model) return NULL;

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512;
    cp.n_batch = 64;
    cp.n_threads = 4;
    cp.n_threads_batch = 4;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { llama_model_free(model); return NULL; }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    uint32_t n_vocab = (uint32_t)llama_vocab_n_tokens(vocab);
    if (n_vocab_out) *n_vocab_out = n_vocab;

    llama_token toks[64];
    /* NOTE: this llama build crashes on text_len == -1 — always pass strlen */
    int32_t nt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, 64,
                                true, false);
    if (nt <= 0) { llama_free(ctx); llama_model_free(model); return NULL; }

    llama_batch batch = llama_batch_init(nt, 0, 1);
    static int32_t seq_ids[64][1];
    for (int32_t i = 0; i < nt; i++) {
        batch.token[i]  = toks[i];
        batch.pos[i]    = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i] = &seq_ids[i][0];
        batch.logits[i] = (i == nt - 1);
    }
    batch.n_tokens = nt;

    if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch); llama_free(ctx); llama_model_free(model);
        return NULL;
    }

    const float *logits = llama_get_logits_ith(ctx, nt - 1);
    float *out = (float *)malloc((size_t)n_vocab * sizeof(float));
    if (out) memcpy(out, logits, (size_t)n_vocab * sizeof(float));
    if (next_token_out) {
        int32_t best = 0;
        for (uint32_t i = 1; i < n_vocab; i++) if (logits[i] > logits[best]) best = (int32_t)i;
        *next_token_out = best;
    }
    if (n_tokens_out) *n_tokens_out = nt;

    /* NOTE: do NOT llama_batch_free(batch) here — this llama build crashes on
     * it after llama_decode (the scheduler swaps the batch's internal pointers
     * with its own buffers, so freeing the user-side batch frees scheduler
     * memory → access violation). A per-decode leak is fine for a test. */
    llama_free(ctx);
    llama_model_free(model);
    return out;
}

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *graft = "build/graft_qwen.gguf";
    const char *graft_r = "build/graft_qwen_reroute.gguf";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Step ③ — cactus graft meets llama.cpp (real inference)\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    llama_backend_init();
    /* backends live beside the llama DLLs, not beside this exe — load from the
     * explicit path so the CPU backend registers (load_all() only scans the
     * executable's own directory) */
    const char *dll_dir = (argc > 2) ? argv[2]
                                     : "I:/llama/llama-b9733-bin-win-vulkan-x64";
    ggml_backend_load_all_from_path(dll_dir);

    GGUFBox box;
    if (gguf_box_open(&box, gguf) != 0) {
        printf("  (cannot open %s — skipping; pass with note)\n", gguf);
        printf("  T: PASS — skipped (no GGUF available)\n");
        pass_count++;
        printf("\nRESULTS: %u/%u PASS\n", pass_count, pass_count + fail_count);
        return 0;
    }

    size_t hdr_sz = (size_t)box.reader.data_offset;
    const uint8_t *hdr = box.reader.base;

    /* T1: the graft file is a valid GGUF — re-parse with gguf_reader */
    {
        remove(graft);
        if (graft_build(graft, &box, hdr, hdr_sz) != 0) {
            printf("  T: FAIL — cannot write graft file\n");
            gguf_box_close(&box);
            return 1;
        }
        GgufReader r2;
        int rc = gguf_open(graft, &r2);
        int ok = (rc == 0) && (r2.n_tensors == box.reader.n_tensors) &&
                 (r2.data_offset == box.reader.data_offset);
        if (rc == 0) {
            for (uint32_t i = 0; i < r2.n_tensors && ok; i++)
                if (r2.sizes[i] != box.reader.sizes[i] ||
                    r2.offsets[i] != box.reader.offsets[i]) ok = 0;
            gguf_close(&r2);
        }
        printf("     graft: header %llu B + body %llu B (source %llu B)\n",
               (unsigned long long)hdr_sz,
               (unsigned long long)(box.reader.base_sz - box.reader.data_offset),
               (unsigned long long)box.reader.base_sz);
        CHECK("T1: graft re-parses as GGUF — same tensors, same data offsets", ok);
    }

    /* T2: llama loads the graft — metadata matches Qwen2.5-0.5B */
    {
        struct llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        struct llama_model *m = llama_model_load_from_file(graft, mp);
        char desc[128] = {0};
        if (m) llama_model_desc(m, desc, sizeof(desc));
        CHECK("T2: llama loads the graft (header scion + served body)", m != NULL);
        if (m) {
            int is_qwen = 0;
            for (char *c = desc; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
            if (strstr(desc, "qwen")) is_qwen = 1;
            printf("     model desc: %s\n", desc);
            CHECK("T2b: arch is qwen (architecture survived the graft)", is_qwen);
            CHECK("T2c: n_embd == 896, n_layer == 24 (Qwen2.5-0.5B)",
                  llama_model_n_embd(m) == 896 && llama_model_n_layer(m) == 24);
            const struct llama_vocab *v = llama_model_get_vocab(m);
            CHECK("T2d: vocab == 151936 tokens", llama_vocab_n_tokens(v) == 151936);
            llama_model_free(m);
        }
    }

    /* T3: inference equivalence — graft logits == original logits (bitwise) */
    {
        const char *prompt = "The capital of France is";
        uint32_t nv1 = 0, nv2 = 0;
        int32_t tok1 = -1, tok2 = -1, nt1 = 0, nt2 = 0;
        float *g = decode_next(&box, graft, prompt, &nv1, &tok1, &nt1);
        float *o = decode_next(&box, gguf, prompt, &nv2, &tok2, &nt2);
        CHECK("T3: graft decode produced logits", g != NULL && o != NULL);
        int same_len = (g && o && nv1 == nv2 && nt1 == nt2);
        CHECK("T3b: same vocab size + same prompt tokens", same_len);
        int identical = 0;
        if (same_len && g && o) {
            identical = memcmp(g, o, (size_t)nv1 * sizeof(float)) == 0;
            if (!identical) {
                uint64_t diffs = 0; double maxd = 0;
                for (uint32_t i = 0; i < nv1; i++) {
                    if (g[i] != o[i]) {
                        diffs++;
                        double d = (g[i] > o[i]) ? (double)(g[i] - o[i]) : (double)(o[i] - g[i]);
                        if (d > maxd) maxd = d;
                    }
                }
                printf("     logits differ: %llu/%u positions, max diff %.3e\n",
                       (unsigned long long)diffs, nv1, maxd);
            }
        }
        CHECK("T3c: next-token logits BITWISE identical to original file run",
              identical);
        if (g && o) {
            printf("     greedy next token: graft=%d original=%d (%d prompt tokens)\n",
                   tok1, tok2, nt1);
            CHECK("T3d: greedy next token identical", tok1 == tok2);
        }
        free(g); free(o);
    }

    /* T4: reroute link — swap the NAMES of two same-shape tensors in the
     * scion (blk.0.attn_q.weight ↔ blk.1.attn_q.weight). Offsets stay in
     * llama's required ascending order (its loader validates the running
     * data cursor — patching offsets outright is rejected, which is itself
     * an integrity feature), so llama loads happily — but the tensor called
     * blk.1.attn_q.weight now reads blk.0.attn_q's data: the scion controls
     * routing, exactly like a rerouted link on the timeline. */
    {
        uint32_t nf = 256;
        TField *fields = (TField *)calloc(nf, sizeof(TField));
        uint64_t data_off = 0;
        int wrc = walk_header(hdr, hdr_sz, fields, &nf, &data_off);
        int f0 = -1, f1 = -1;
        for (uint32_t i = 0; i < nf && wrc == 0; i++) {
            if (strcmp(fields[i].name, "blk.0.attn_q.weight") == 0) f0 = (int)i;
            if (strcmp(fields[i].name, "blk.1.attn_q.weight") == 0) f1 = (int)i;
        }
        int same_len = f0 >= 0 && f1 >= 0 &&
                       fields[f0].name_len == fields[f1].name_len;
        /* walk ends at the unaligned header end; reader stores the 32-aligned
         * value — they may differ by the alignment padding (< 32) */
        int aligned_ok = wrc == 0 &&
            data_off <= box.reader.data_offset &&
            box.reader.data_offset - data_off < 32u;
        CHECK("T4: header walk finds blk.0/blk.1 attn_q name fields",
              wrc == 0 && f0 >= 0 && f1 >= 0 && same_len && aligned_ok);

        remove(graft_r);
        uint8_t *hdr2 = (uint8_t *)malloc(hdr_sz);
        memcpy(hdr2, hdr, hdr_sz);
        if (wrc == 0 && f0 >= 0 && f1 >= 0 && same_len) {
            size_t L = fields[f0].name_len;
            uint8_t tmp[128];
            memcpy(tmp, hdr2 + fields[f0].name_pos, L);
            memcpy(hdr2 + fields[f0].name_pos, hdr2 + fields[f1].name_pos, L);
            memcpy(hdr2 + fields[f1].name_pos, tmp, L);
        }
        int grc = graft_build(graft_r, &box, hdr2, hdr_sz);
        free(hdr2); free(fields);

        struct llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        struct llama_model *m = grc == 0 ? llama_model_load_from_file(graft_r, mp) : NULL;
        CHECK("T4b: rerouted graft still LOADS (names swapped, offsets untouched)",
              m != NULL);
        if (m) llama_model_free(m);

        /* logits must CHANGE — llama really read the rerouted data */
        uint32_t nv1 = 0, nv2 = 0;
        int32_t tok1 = -1, tok2 = -1;
        float *g = decode_next(&box, graft, "The capital of France is", &nv1, &tok1, NULL);
        float *r = decode_next(&box, graft_r, "The capital of France is", &nv2, &tok2, NULL);
        int changed = 0;
        if (g && r && nv1 == nv2) {
            for (uint32_t i = 0; i < nv1; i++) {
                if (g[i] != r[i]) { changed = 1; break; }
            }
        }
        CHECK("T4c: rerouted logits DIFFER — scion controls where data comes from",
              changed);
        free(g); free(r);
    }

    remove(graft);
    remove(graft_r);
    gguf_box_close(&box);
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
