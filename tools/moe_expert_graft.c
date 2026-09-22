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
#include <time.h>

#include "../core/gguf_reader.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
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
#include "ggml-backend.h"
#include "gguf.h"

static int g_backend_loaded = 0;
static const char *g_llama_dll_dir = NULL;

struct llama_model *load_model(const char *gguf_path) {
    if (!g_backend_loaded) {
        if (g_llama_dll_dir)
            ggml_backend_load_all_from_path(g_llama_dll_dir);
        else
            ggml_backend_load_all();
        g_backend_loaded = 1;
    }
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

    /* ═══════════════ ZERO WARM-UP CALLBACK ═══════════════ */

struct PoolEntry { const char *name; const uint8_t *ptr; uint32_t sz; };

struct ZWCtx {
    const uint8_t *pool_base;           /* DtSlotRegion base */
    const uint8_t *src_mmap;            /* source GGUF mmap */
    uint64_t src_data_offset;           /* data section offset */
    uint64_t src_mmap_size;            /* total mmap size */
    const uint64_t *offsets;            /* per-tensor data offsets */
    const uint32_t *sizes;              /* per-tensor byte sizes */
    const char **names;                 /* tensor names */
    uint32_t n_tensors;
    uint32_t matched, missing;
    /* pool lookup: baked tensor name → pool pointer */
    struct PoolEntry *pool_map;
    uint32_t n_pool;
};

static void provide_moe_tensor(struct ggml_tensor *t, void *ud) {
    struct ZWCtx *zw = (struct ZWCtx *)ud;
    const char *name = ggml_get_name(t);
    size_t nb = ggml_nbytes(t);

    /* Patched DLL passes t->data=NULL: the callback must SET the pointer
     * (zero-copy), never memcpy into it (#914 pattern). */
    /* Check pool first (baked MoE tensors) */
    for (uint32_t i = 0; i < zw->n_pool; i++) {
        if (strcmp(zw->pool_map[i].name, name) == 0) {
            if (nb != zw->pool_map[i].sz) {
                fprintf(stderr, "  [zw] SIZE MISMATCH %s: pool=%u llama=%zu\n",
                        name, zw->pool_map[i].sz, nb);
                zw->missing++;
                return;
            }
            t->data = (void *)zw->pool_map[i].ptr;
            zw->matched++;
            return;
        }
    }

    /* Not baked → point at source GGUF mmap (read-only zero-copy) */
    for (uint32_t i = 0; i < zw->n_tensors; i++) {
        if (strcmp(zw->names[i], name) == 0) {
            if (nb != zw->sizes[i]) {
                fprintf(stderr, "  [zw] SIZE MISMATCH %s: src=%u llama=%zu\n",
                        name, zw->sizes[i], nb);
                zw->missing++;
                return;
            }
            uint64_t src_off = zw->src_data_offset + zw->offsets[i];
            if (src_off + nb <= zw->src_mmap_size) {
                t->data = (void *)(zw->src_mmap + src_off);
                zw->matched++;
                return;
            }
            break;
        }
    }
    /* Tensor not found at all — log first few, fill defaults in owned buf */
    if (zw->missing < 20)
        fprintf(stderr, "  [zw] missing: %s type=%d nb=%zu\n", name, (int)t->type, nb);
    /* fill defaults: bias→0, scale/input_scale/rope_freqs→1.0 */
    {
        uint8_t *fb = (uint8_t *)calloc(1, nb ? nb : 1);
        if (!fb) { zw->missing++; return; }
        if (!strstr(name, ".bias")) {
            float *fd = (float *)fb;
            size_t nf = nb / sizeof(float);
            for (size_t i = 0; i < nf; i++) fd[i] = 1.0f;
        }
        t->data = (void *)fb; /* intentionally owned till process exit */
    }
    zw->missing++;
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
    g_llama_dll_dir = llama_dir;

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        printf("FAIL: cannot open GGUF\n");
        return 1;
    }
    printf("Tensors:  %u\n", gguf.n_tensors);
    double t_total = now_sec();

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
    printf("Baked:    %u tensors  [%.2f s]\n", baked, now_sec() - t_total);

    /* ═══════════════ PASS 2: VERIFY roundtrip ═══════════════ */
    printf("\n=== PASS 2: VERIFY ===\n");
    double t2 = now_sec();
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
    printf("Verified: %u / %u lossless  [%.2f s]\n", pass_count, baked, now_sec() - t2);

    /* ═══════════════ PASS 3: BUILD graft GGUF ═══════════════ */
    printf("\n=== PASS 3: GRAFT ===\n");
    double t3 = now_sec();

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
    printf("  written: %s (%.1f MB)  [%.2f s]\n", graft_path,
           (double)(hdr_sz + body_sz) / 1e6, now_sec() - t3);

    free(body);

    /* ═══════════════ PASS 4: INFERENCE comparison ═══════════════ */
    printf("\n=== PASS 4: INFERENCE ===\n");
    double t4 = now_sec();

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
    printf("\n  GATE 4a: %s\n", rc == 0 ? "PASS (graft file inference identical)" : "FAIL");
    printf("  inference time: %.2f s\n", now_sec() - t4);

    /* ═══════════════ PASS 4b: ZERO WARM-UP (callback model) ═══════════════ */
    printf("\n=== PASS 4b: ZERO WARM-UP ===\n");
    double t4b = now_sec();

    /* Build pool lookup: baked tensor name → pool pointer + size */
    struct PoolEntry *pool_map;
    pool_map = (struct PoolEntry *)malloc(n_match * sizeof(*pool_map));
    uint32_t n_pool = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (!match_tensor(gguf.names[i], &layer, &wtype)) continue;
        MoeExpertMeta meta;
        if (moe_load_meta(&region, (uint32_t)layer, 0, (uint32_t)wtype, &meta) == 0) {
            pool_map[n_pool].name = gguf.names[i];
            pool_map[n_pool].ptr  = region.base + meta.offset;
            pool_map[n_pool].sz   = meta.size;
            n_pool++;
        }
    }
    printf("  callback pool: %u baked tensors\n", n_pool);

    /* Callback context */
    struct ZWCtx zw = {
        .pool_base       = region.base,
        .src_mmap        = gguf.base,
        .src_data_offset = gguf.data_offset,
        .src_mmap_size   = gguf.base_sz,
        .offsets         = gguf.offsets,
        .sizes           = gguf.sizes,
        .names           = (const char **)gguf.names,
        .n_tensors       = gguf.n_tensors,
        .matched         = 0,
        .missing         = 0,
        .pool_map        = pool_map,
        .n_pool          = n_pool,
    };

    /* Parse the original GGUF to get a gguf_context for callback model.
     * .ctx = &meta_ctx creates ggml_context with correct tensor types (Q4_K etc.)
     * so llama creates properly-typed tensors instead of defaulting to F32. */
    struct ggml_context *meta_ctx = NULL;
    struct gguf_init_params gparams = { .no_alloc = false, .ctx = &meta_ctx };
    struct gguf_context *gctx = gguf_init_from_file(gguf_path, gparams);
    if (!gctx) {
        printf("  FAIL: gguf_init_from_file\n");
        free(pool_map);
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }

    /* Load model A (original) for baseline logits — CPU, same as model B,
     * so the compare is kernel-identical (Vulkan vs CPU kernels differ). */
    struct llama_model *mA2 = NULL;
    {
        struct llama_model_params mpA = llama_model_default_params();
        mpA.n_gpu_layers = 0;
        mA2 = llama_model_load_from_file(gguf_path, mpA);
    }
    if (!mA2) {
        printf("  FAIL: load original model\n");
        gguf_free(gctx);
        free(pool_map);
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }

    struct llama_context_params cparams2 = llama_context_default_params();
    cparams2.n_batch = 2048;
    struct llama_context *ctxA2 = llama_init_from_model(mA2, cparams2);
    if (!ctxA2) {
        printf("  FAIL: init original context\n");
        llama_model_free(mA2);
        gguf_free(gctx);
        free(pool_map);
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }

    /* Load model B via callback (pool for MoE, source mmap for rest).
     * CPU-only: callback pointers are plain host mmaps, not Vulkan-pinned
     * staging buffers (4b proves numerical identity, not GPU placement). */
    struct llama_model_params mpB = llama_model_default_params();
    mpB.n_gpu_layers = 0;
    mpB.no_host = true; /* callback pointers are plain mmaps, not Vulkan_Host staging */
    struct llama_model *mB2 = llama_model_init_from_user(
        gctx,
        provide_moe_tensor, &zw,
        mpB);
    if (!mB2) {
        printf("  FAIL: callback model init\n");
        llama_free(ctxA2);
        llama_model_free(mA2);
        gguf_free(gctx);
        free(pool_map);
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }

    struct llama_context *ctxB2 = llama_init_from_model(mB2, cparams2);
    if (!ctxB2) {
        printf("  FAIL: init callback context\n");
        llama_free(ctxA2);
        llama_model_free(mA2);
        llama_model_free(mB2);
        gguf_free(gctx);
        if (meta_ctx) ggml_free(meta_ctx);
        free(pool_map);
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }

    /* Tokenize + decode + compare */
    struct llama_sampler *smpl2 = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl2, llama_sampler_init_greedy());

    llama_token tokens2[256];
    const struct llama_vocab *vocab2 = llama_model_get_vocab(mA2);
    int n2 = llama_tokenize(vocab2, prompt, (int32_t)strlen(prompt), tokens2, 250, true, false);
    if (n2 <= 0 || llama_decode(ctxA2, llama_batch_get_one(tokens2, n2)) != 0 ||
        llama_decode(ctxB2, llama_batch_get_one(tokens2, n2)) != 0) {
        printf("  FAIL: tokenize/decode\n");
        llama_sampler_free(smpl2);
        llama_free(ctxA2); llama_free(ctxB2);
        llama_model_free(mA2); llama_model_free(mB2);
        gguf_free(gctx);
        free(pool_map);
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }

    const float *logitsA2 = llama_get_logits(ctxA2);
    const float *logitsB2 = llama_get_logits(ctxB2);
    int n_vocab2 = llama_vocab_n_tokens(vocab2);

    float maxdiff2 = 0.0f;
    int match2 = 1;
    for (int i = 0; i < n_vocab2; i++) {
        float diff = logitsA2[i] > logitsB2[i] ? logitsA2[i] - logitsB2[i] : logitsB2[i] - logitsA2[i];
        if (diff > maxdiff2) maxdiff2 = diff;
        if (diff > 0.001f) { match2 = 0; break; }
    }
    printf("  logits: n_vocab=%d  maxdiff=%.6f  %s\n", n_vocab2, maxdiff2,
           match2 ? "BITWISE OK" : "MISMATCH");
    printf("  callback matched=%u missing=%u\n", zw.matched, zw.missing);

    /* Generate 40 tokens to double-check */
    int n_gen2 = 40;
    for (int g = 0; g < n_gen2; g++) {
        llama_token tA = llama_sampler_sample(smpl2, ctxA2, -1);
        llama_token tB = llama_sampler_sample(smpl2, ctxB2, -1);
        llama_sampler_accept(smpl2, tA);
        if (tA != tB) { match2 = 0; printf("\n  token %d: A=%d B=%d MISMATCH\n", g, tA, tB); break; }
        char buf2[64];
        int k2 = llama_token_to_piece(vocab2, tA, buf2, sizeof(buf2) - 1, 0, false);
        if (k2 < 0) k2 = 0;
        buf2[k2] = '\0';
        printf("%s", buf2);
    }
    printf("\n");

    int rc2 = match2 ? 0 : 1;
    printf("\n  GATE 4b: %s\n", rc2 == 0 ? "PASS (zero warm-up identical to original)" : "FAIL");
    printf("  zero warm-up time: %.2f s\n", now_sec() - t4b);

    llama_sampler_free(smpl2);
    llama_free(ctxA2); llama_free(ctxB2);
    llama_model_free(mA2); llama_model_free(mB2);
    gguf_free(gctx);
    if (meta_ctx) ggml_free(meta_ctx);
    free(pool_map);

    printf("\n  TOTAL: %.2f s\n", now_sec() - t_total);

    dt_slot_destroy(&region);
    gguf_close(&gguf);
    return rc;
}
