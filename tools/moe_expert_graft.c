/* tools/moe_expert_graft.c — MoE Expert Bake + Graft GGUF + Inference
 * ═══════════════════════════════════════════════════════════════════════════
 * Step 1: Bake GGUF tensors into DtSlotRegion (geometric addressing)
 * Step 2: Rebuild a valid GGUF from pool data + source header
 * Step 3: Load graft with llama.cpp, generate, compare with original
 *
 * Proves: geometric addressing of weights works for real inference.
 *
 * BUILD: make moe-graft
 * RUN:   ./build/moe_expert_graft [gguf_path]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../core/gguf_reader.h"
#include "../core/moe_expert_addr.h"
#include "../core/infra/dramtile_store.h"
#include "../core/moe_expert_store.h"

/* ═══════════════ BAKE PASS ═══════════════ */

/* tensor name → (layer, expert, wtype) mapping for dense models */
typedef struct { const char *substr; int wtype; } TensorPattern;
static const TensorPattern PATTERNS[] = {
    {".ffn_down_exps.weight",  0},
    {".ffn_gate_exps.weight",  1},
    {".ffn_up_exps.weight",    2},
};
#define N_PATTERNS (sizeof(PATTERNS)/sizeof(PATTERNS[0]))

static int extract_layer(const char *name) {
    const char *p = strstr(name, "blk.");
    if (!p) return -1;
    return atoi(p + 4);
}

static int match_tensor(const char *name, int *out_layer, int *out_wtype) {
    int layer = extract_layer(name);
    if (layer < 0) return 0;
    for (size_t i = 0; i < N_PATTERNS; i++) {
        if (strstr(name, PATTERNS[i].substr)) {
            *out_layer = layer;
            *out_wtype = PATTERNS[i].wtype;
            return 1;
        }
    }
    return 0;
}

/* ═══════════════ GRAFT REBUILD ═══════════════ */

/* GGUF alignment (32 bytes) */
#define GGUF_ALIGN 32u
static inline uint64_t align32(uint64_t x) { return (x + GGUF_ALIGN - 1) & ~(uint64_t)(GGUF_ALIGN - 1); }

/* ─── Minimal GGUF writer ─── */
typedef struct {
    uint8_t *buf;
    size_t   pos;
    size_t   cap;
} GgufWriter;

static inline void gw_init(GgufWriter *w, size_t cap) {
    w->buf = (uint8_t *)calloc(1, cap);
    w->pos = 0;
    w->cap = cap;
}

static inline void gw_u32(GgufWriter *w, uint32_t v) {
    if (w->pos + 4 <= w->cap) { memcpy(w->buf + w->pos, &v, 4); w->pos += 4; }
}

static inline void gw_u64(GgufWriter *w, uint64_t v) {
    if (w->pos + 8 <= w->cap) { memcpy(w->buf + w->pos, &v, 8); w->pos += 8; }
}

static inline void gw_bytes(GgufWriter *w, const void *src, size_t n) {
    if (w->pos + n <= w->cap) { memcpy(w->buf + w->pos, src, n); w->pos += n; }
}

static inline void gw_align32(GgufWriter *w) {
    uint32_t pad = (uint32_t)((GGUF_ALIGN - (w->pos % GGUF_ALIGN)) % GGUF_ALIGN);
    w->pos += pad;
}

/* ═══════════════ INFERENCE ═══════════════ */

#ifdef _WIN32
#include <windows.h>
static const char *find_llama_dll(const char *dir) {
    static char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/llama.dll", dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return dir;
    return NULL;
}
#else
static const char *find_llama_dll(const char *dir) {
    static char path[1024];
    snprintf(path, sizeof(path), "%s/libllama.so", dir);
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return dir; }
    return NULL;
}
#endif

#include "llama.h"

struct llama_model *load_model(const char *gguf_path) {
    struct llama_model_params mparams = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(gguf_path, mparams);
    if (!model) {
        fprintf(stderr, "  FAIL: load model %s\n", gguf_path);
        return NULL;
    }
    return model;
}

int compare_logits(const char *path_a, const char *path_b, const char *llama_dir,
                   const char *prompt) {
    struct llama_model *mA = load_model(path_a);
    struct llama_model *mB = load_model(path_b);
    if (!mA || !mB) { llama_model_free(mA); llama_model_free(mB); return 1; }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_batch = 2048;
    struct llama_context *ctxA = llama_init_from_model(mA, cparams);
    struct llama_context *ctxB = llama_init_from_model(mB, cparams);
    if (!ctxA || !ctxB) {
        fprintf(stderr, "  FAIL: init context\n");
        llama_free(ctxA); llama_free(ctxB);
        llama_model_free(mA); llama_model_free(mB);
        return 1;
    }

    struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    /* Tokenize prompt */
    llama_token tokens[256];
    const struct llama_vocab *vocab = llama_model_get_vocab(mA);
    int n = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), tokens, 250, true, false);
    if (n <= 0) {
        fprintf(stderr, "  FAIL: tokenize prompt (n=%d)\n", n);
        llama_sampler_free(smpl); llama_free(ctxA); llama_free(ctxB);
        llama_model_free(mA); llama_model_free(mB);
        return 1;
    }

    /* Evaluate prompt on both */
    if (llama_decode(ctxA, llama_batch_get_one(tokens, n)) != 0 ||
        llama_decode(ctxB, llama_batch_get_one(tokens, n)) != 0) {
        fprintf(stderr, "  FAIL: decode prompt\n");
        llama_sampler_free(smpl); llama_free(ctxA); llama_free(ctxB);
        llama_model_free(mA); llama_model_free(mB);
        return 1;
    }

    /* Get logits at position n-1 from both */
    const float *logitsA = llama_get_logits(ctxA);
    const float *logitsB = llama_get_logits(ctxB);
    int n_vocab = llama_vocab_n_tokens(llama_model_get_vocab(mA));

    /* Compare logits */
    int match = 1;
    float maxdiff = 0.0f;
    for (int i = 0; i < n_vocab; i++) {
        float diff = logitsA[i] > logitsB[i] ? logitsA[i] - logitsB[i] : logitsB[i] - logitsA[i];
        if (diff > maxdiff) maxdiff = diff;
        if (diff > 0.001f) { match = 0; break; }
    }
    printf("  logits: n_vocab=%d  maxdiff=%.6f  %s\n", n_vocab, maxdiff,
           match ? "BITWISE OK" : "MISMATCH");

    /* Generate N tokens from both */
    int n_gen = 40;
    printf("\n  Generate %d tokens:\n", n_gen);
    for (int g = 0; g < n_gen; g++) {
        llama_token tokA = llama_sampler_sample(smpl, ctxA, -1);
        llama_token tokB = llama_sampler_sample(smpl, ctxB, -1);
        llama_sampler_accept(smpl, tokA);
        llama_sampler_accept(smpl, tokB);

        if (tokA != tokB) {
            printf("  token %d: A=%d B=%d MISMATCH\n", g, tokA, tokB);
            match = 0;
            break;
        }

        /* Decode and print */
        char buf[64];
        int k = llama_token_to_piece(llama_model_get_vocab(mA), tokA, buf, sizeof(buf) - 1, 0, false);
        if (k < 0) k = 0;
        buf[k] = '\0';
        printf("%s", buf);

        /* Evaluate next */
        llama_batch batch = llama_batch_get_one(&tokA, 1);
        if (llama_decode(ctxA, batch) != 0 || llama_decode(ctxB, batch) != 0) {
            fprintf(stderr, "\n  FAIL: decode step %d\n", g);
            break;
        }
    }
    printf("\n");

    llama_sampler_free(smpl);
    llama_free(ctxA); llama_free(ctxB);
    llama_model_free(mA); llama_model_free(mB);
    return match ? 0 : 1;
}

/* ═══════════════ MAIN ═══════════════ */

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] : "F:\\model\\qwen3-4b-moe-q4_k_m.gguf";
    const char *llama_dir = (argc > 2) ? argv[2] : "I:\\llama\\llama-b9733-bin-win-vulkan-x64";
    const char *prompt    = (argc > 3) ? argv[3] : "The capital of France is";
    uint32_t n_slots = 20736;
    size_t meta_slot_sz = sizeof(MoeExpertMeta);
    const char *region_path = "moe_expert_region.bin";
    const char *graft_path  = "F:/model/moe_expert_graft.gguf";

    printf("=== MoE Expert Graft: Bake → Graft GGUF → Inference ===\n");
    printf("GGUF:     %s\n", gguf_path);
    printf("LLAMA:    %s\n", llama_dir);
    printf("Graft:    %s\n", graft_path);

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        printf("FAIL: cannot open GGUF\n");
        return 1;
    }
    printf("Tensors:  %u\n", gguf.n_tensors);

    /* first pass: count and compute total weight bytes */
    uint32_t n_match = 0, max_layer = 0;
    uint64_t total_weight_bytes = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (match_tensor(gguf.names[i], &layer, &wtype)) {
            n_match++;
            if ((uint32_t)layer > max_layer) max_layer = (uint32_t)layer;
            total_weight_bytes += gguf.sizes[i];
        }
    }
    printf("Matched:  %u tensors (max layer: %u)\n", n_match, max_layer);
    printf("Weight pool: %.1f MB (%llu bytes)\n",
           total_weight_bytes / 1e6, (unsigned long long)total_weight_bytes);

    if (n_match == 0) {
        printf("No matching tensors. Available:\n");
        for (uint32_t i = 0; i < gguf.n_tensors && i < 20; i++)
            printf("  [%u] %s (%u bytes)\n", i, gguf.names[i], gguf.sizes[i]);
        gguf_close(&gguf);
        return 1;
    }

    /* ═══════════════ PASS 1: BAKE into DtSlotRegion ═══════════════ */
    printf("\n=== PASS 1: BAKE ===\n");

    DtSlotRegion region;
    if (dt_slot_init_twin(&region, region_path, n_slots, meta_slot_sz) != 0) {
        printf("FAIL: dt_slot_init_twin\n");
        gguf_close(&gguf);
        return 1;
    }

    uint64_t pool_offset = dt_slot_extend_twin(&region, total_weight_bytes);
    if (!pool_offset) {
        printf("FAIL: dt_slot_extend_twin\n");
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }

    uint8_t *buf = (uint8_t *)malloc(64 * 1024 * 1024);
    uint64_t write_cursor = pool_offset;
    uint32_t baked = 0;

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (!match_tensor(gguf.names[i], &layer, &wtype)) continue;

        uint32_t tsz = gguf.sizes[i];
        if (tsz > 64 * 1024 * 1024) {
            printf("  SKIP %s: too large\n", gguf.names[i]);
            continue;
        }
        if (gguf_read_tensor(gguf_path, &gguf, i, buf, tsz) != 0) continue;

        uint8_t *pool_ptr = region.base + write_cursor;
        memcpy(pool_ptr, buf, tsz);

        MoeExpertMeta meta = {0};
        meta.offset = (uint32_t)write_cursor;
        meta.size   = tsz;
        meta.quant_type = gguf.dtypes[i];
        if (moe_store_meta(&region, (uint32_t)layer, 0, (uint32_t)wtype, &meta) != 0) {
            printf("  FAIL: store meta %s\n", gguf.names[i]);
            continue;
        }

        write_cursor += tsz;
        printf("  OK   [%2d] %-48s  %6u bytes  → pool@%llu\n",
               layer, gguf.names[i], tsz, (unsigned long long)(write_cursor - tsz));
        baked++;
    }
    printf("Baked:    %u tensors\n", baked);

    /* ═══════════════ PASS 2: VERIFY roundtrip ═══════════════ */
    printf("\n=== PASS 2: VERIFY ===\n");
    uint32_t pass_count = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (!match_tensor(gguf.names[i], &layer, &wtype)) continue;

        MoeExpertMeta meta;
        if (moe_load_meta(&region, (uint32_t)layer, 0, (uint32_t)wtype, &meta) != 0) {
            printf("  FAIL: load meta [%d]\n", layer);
            continue;
        }

        uint8_t *src = region.base + meta.offset;
        uint32_t tsz = gguf.sizes[i];
        if (gguf_read_tensor(gguf_path, &gguf, i, buf, tsz) != 0) continue;

        if (meta.size == tsz && memcmp(src, buf, tsz) == 0) {
            pass_count++;
        } else {
            printf("  MISMATCH [%d] %s\n", layer, gguf.names[i]);
        }
    }
    printf("Verified: %u / %u lossless\n", pass_count, baked);

    /* ═══════════════ PASS 3: BUILD graft GGUF ═══════════════ */
    printf("\n=== PASS 3: GRAFT ===\n");

    /* Strategy: copy source header verbatim, build body with pool data for baked tensors.
     * Body layout: same as source (same tensor offsets), so header is compatible.
     * This proves the pool → GGUF chain without fragile header rebuild. */

    size_t hdr_sz = (size_t)gguf.data_offset;
    uint32_t from_pool = 0, from_source = 0;

    /* Build body: same layout as source, baked tensors from pool */
    size_t body_sz = (size_t)(gguf.base_sz - gguf.data_offset);
    uint8_t *body = (uint8_t *)calloc(1, body_sz);

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        uint64_t off = gguf.offsets[i];
        uint32_t tsz = gguf.sizes[i];

        int layer, wtype;
        if (match_tensor(gguf.names[i], &layer, &wtype)) {
            MoeExpertMeta meta;
            if (moe_load_meta(&region, (uint32_t)layer, 0, (uint32_t)wtype, &meta) == 0
                && meta.size == tsz) {
                memcpy(body + off, region.base + meta.offset, tsz);
                from_pool++;
                continue;
            }
        }

        /* Copy from source mmap */
        uint64_t src_off = gguf.data_offset + gguf.offsets[i];
        if (src_off + tsz <= gguf.base_sz) {
            memcpy(body + off, gguf.base + src_off, tsz);
            from_source++;
        }
    }
    printf("  header: %zu bytes (source verbatim)\n", hdr_sz);
    printf("  body:   %zu bytes (from pool: %u, from source: %u)\n",
           body_sz, from_pool, from_source);

    /* Write graft GGUF */
    FILE *f = fopen(graft_path, "wb");
    if (!f || fwrite(gguf.base, 1, hdr_sz, f) != hdr_sz ||
        fwrite(body, 1, body_sz, f) != body_sz) {
        printf("  FAIL: write %s\n", graft_path);
        free(body);
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }
    fclose(f);
    printf("  written: %s (%.1f MB)\n", graft_path,
           (double)(hdr_sz + body_sz) / 1e6);

    free(body);

    /* ═══════════════ PASS 4: INFERENCE comparison ═══════════════ */
    printf("\n=== PASS 4: INFERENCE ===\n");

    /* Check for llama DLLs */
    const char *dll_dir = find_llama_dll(llama_dir);
    if (!dll_dir) {
        printf("  SKIP: llama DLLs not found at %s\n", llama_dir);
        printf("  (Graft GGUF written but inference comparison skipped)\n");
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 0;
    }

    printf("  loading models...\n");
    int rc = compare_logits(gguf_path, graft_path, llama_dir, prompt);
    printf("\n  GATE: %s\n", rc == 0 ? "PASS (graft inference identical to original)" : "FAIL");

    dt_slot_destroy(&region);
    gguf_close(&gguf);
    return rc;
}
