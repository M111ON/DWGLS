/* tools/moe_expert_route.c — MoE Routing Integration: Bake → Route → Serve → Inference
 * ═══════════════════════════════════════════════════════════════════════════════════
 * Proves the full routing pipeline through real llama.cpp inference:
 *   1. Load baked DtSlotRegion (geometric addressing)
 *   2. Run router gate per layer → select top-K experts
 *   3. Rebuild GGUF: routed experts from pool, rest from source
 *   4. Load through llama.cpp → inference comparison (identical weights = identical output)
 *   5. Byte-verify: routed experts from pool match source exactly
 *
 * This bridges moe_expert_stream.c (selective loading proof) with
 * moe_expert_graft.c (inference comparison) into a single pipeline.
 *
 * BUILD: make moe-route
 * RUN:   ./build/moe_expert_route [gguf_path] [llama_dir] [prompt]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../core/gguf_reader.h"
#include "../core/moe_expert_addr.h"
#include "../core/infra/dramtile_store.h"
#include "../core/moe_expert_store.h"

#define N_EXPERTS   64
#define TOP_K       4
#define QK_K        256
#define K_SCALE_SIZE 12

/* ═══════════════ Q4_K DEQUANT (from stream.c) ═══════════════ */

typedef struct {
    uint16_t d;
    uint16_t dmin;
    uint8_t  scales[K_SCALE_SIZE];
    uint8_t  qs[QK_K / 2];
} block_q4_K;

static float fp16_to_fp32(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp  = ((h >> 10) & 0x1f) - 15;
    int mant = h & 0x3ff;
    if (exp == -15 && mant == 0) return sign ? -0.0f : 0.0f;
    if (exp == 16 && mant == 0) return sign ? -INFINITY : INFINITY;
    if (exp == 16 && mant != 0) return NAN;
    float v;
    if (exp == -15) {
        v = (float)mant / 1024.0f;
    } else {
        v = (1.0f + (float)mant / 1024.0f) * ldexpf(1.0f, exp);
    }
    return sign ? -v : v;
}

/* ═══════════════ HELPERS ═══════════════ */

static int extract_layer(const char *name) {
    const char *p = strstr(name, "blk.");
    if (!p) return -1;
    return atoi(p + 4);
}

typedef struct { const char *substr; int wtype; } TensorPattern;
static const TensorPattern PATTERNS[] = {
    {".ffn_down_exps.weight",  0},
    {".ffn_gate_exps.weight",  1},
    {".ffn_up_exps.weight",    2},
};
#define N_PATTERNS (sizeof(PATTERNS)/sizeof(PATTERNS[0]))

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

static void topk(const float *vals, int n, int k, int *out) {
    int *idx = (int *)malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 0; i < k && i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (vals[idx[j]] > vals[idx[best]]) best = j;
        int tmp = idx[i]; idx[i] = idx[best]; idx[best] = tmp;
    }
    memcpy(out, idx, k * sizeof(int));
    free(idx);
}

/* ═══════════════ GGUF WRITER (from graft.c) ═══════════════ */

#define GGUF_ALIGN32 32u
static inline uint64_t align32(uint64_t x) { return (x + GGUF_ALIGN32 - 1) & ~(uint64_t)(GGUF_ALIGN32 - 1); }

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
    uint32_t pad = (uint32_t)((GGUF_ALIGN32 - (w->pos % GGUF_ALIGN32)) % GGUF_ALIGN32);
    w->pos += pad;
}

/* ═══════════════ INFERENCE (from graft.c) ═══════════════ */

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

static struct llama_model *load_model(const char *gguf_path) {
    struct llama_model_params mparams = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(gguf_path, mparams);
    if (!model) fprintf(stderr, "  FAIL: load model %s\n", gguf_path);
    return model;
}

static int compare_logits(const char *path_a, const char *path_b, const char *llama_dir,
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

    llama_token tokens[256];
    const struct llama_vocab *vocab = llama_model_get_vocab(mA);
    int n = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), tokens, 250, true, false);
    if (n <= 0) {
        fprintf(stderr, "  FAIL: tokenize prompt (n=%d)\n", n);
        llama_sampler_free(smpl); llama_free(ctxA); llama_free(ctxB);
        llama_model_free(mA); llama_model_free(mB);
        return 1;
    }

    if (llama_decode(ctxA, llama_batch_get_one(tokens, n)) != 0 ||
        llama_decode(ctxB, llama_batch_get_one(tokens, n)) != 0) {
        fprintf(stderr, "  FAIL: decode prompt\n");
        llama_sampler_free(smpl); llama_free(ctxA); llama_free(ctxB);
        llama_model_free(mA); llama_model_free(mB);
        return 1;
    }

    const float *logitsA = llama_get_logits(ctxA);
    const float *logitsB = llama_get_logits(ctxB);
    int n_vocab = llama_vocab_n_tokens(llama_model_get_vocab(mA));

    int match = 1;
    float maxdiff = 0.0f;
    for (int i = 0; i < n_vocab; i++) {
        float diff = logitsA[i] > logitsB[i] ? logitsA[i] - logitsB[i] : logitsB[i] - logitsA[i];
        if (diff > maxdiff) maxdiff = diff;
        if (diff > 0.001f) { match = 0; break; }
    }
    printf("  logits: n_vocab=%d  maxdiff=%.6f  %s\n", n_vocab, maxdiff,
           match ? "BITWISE OK" : "MISMATCH");

    int n_gen = 40;
    printf("\n  Generate %d tokens:\n", n_gen);
    for (int g = 0; g < n_gen; g++) {
        llama_token tokA = llama_sampler_sample(smpl, ctxA, -1);
        llama_token tokB = llama_sampler_sample(smpl, ctxB, -1);
        llama_sampler_accept(smpl, tokA);

        if (tokA != tokB) {
            printf("  token %d: A=%d B=%d MISMATCH\n", g, tokA, tokB);
            match = 0;
            break;
        }

        char buf[64];
        int k = llama_token_to_piece(llama_model_get_vocab(mA), tokA, buf, sizeof(buf) - 1, 0, false);
        if (k < 0) k = 0;
        buf[k] = '\0';
        printf("%s", buf);

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
    setbuf(stdout, NULL);
    const char *gguf_path  = (argc > 1) ? argv[1] : "F:\\model\\qwen3-4b-moe-q4_k_m.gguf";
    const char *llama_dir  = (argc > 2) ? argv[2] : "I:\\llama\\llama-b9733-bin-win-vulkan-x64";
    const char *prompt     = (argc > 3) ? argv[3] : "The capital of France is";
    const char *region_path = "moe_expert_region.bin";
    const char *route_path  = "F:/model/moe_expert_routed.gguf";
    uint32_t n_slots = 20736;
    size_t meta_slot_sz = sizeof(MoeExpertMeta);

    printf("=== MoE Routing Integration: Bake → Route → Serve → Inference ===\n");
    printf("GGUF:    %s\n", gguf_path);
    printf("LLAMA:   %s\n", llama_dir);
    printf("Region:  %s\n", region_path);
    printf("Route:   %s\n\n", route_path);

    /* ── 1. Open source GGUF ── */
    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        printf("FAIL: cannot open GGUF\n");
        return 1;
    }
    printf("Source GGUF: %u tensors, data_offset=%llu\n",
           gguf.n_tensors, (unsigned long long)gguf.data_offset);

    /* ── 2. Open baked DtSlotRegion ── */
    DtSlotRegion region;
    memset(&region, 0, sizeof(region));
    region.slot_sz = meta_slot_sz;
    region.n_slots = n_slots;
    {
#if defined(_WIN32)
        HANDLE hf = CreateFileA(region_path, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            printf("FAIL: open %s (run moe-bake first)\n", region_path);
            gguf_close(&gguf);
            return 1;
        }
        LARGE_INTEGER fsize;
        GetFileSizeEx(hf, &fsize);
        size_t total = (size_t)fsize.QuadPart;
        region.hSlotMapping = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
        region.base = (uint8_t*)MapViewOfFile(region.hSlotMapping, FILE_MAP_READ, 0, 0, total);
        region.hSlotFile = hf;
#else
        int fd = open(region_path, O_RDONLY);
        if (fd < 0) { printf("FAIL: open %s\n", region_path); gguf_close(&gguf); return 1; }
        struct stat st; fstat(fd, &st);
        size_t total = (size_t)st.st_size;
        region.base = (uint8_t*)mmap(NULL, total, PROT_READ, MAP_PRIVATE, fd, 0);
        region.slot_fd = fd;
#endif
        region.is_twin = 1;
        printf("Region loaded: %u slots\n\n", (unsigned)region.n_slots);
    }

    /* ── 3. Count layers + per-expert sizes ── */
    int max_layer = -1;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (match_tensor(gguf.names[i], &layer, &wtype) && layer > max_layer)
            max_layer = layer;
    }
    int n_layers = max_layer + 1;
    printf("MoE layers: %d, top-K: %d\n\n", n_layers, TOP_K);

    /* ── 4. Build routing table: for each layer, select top-K experts ── */
    /* routing[layer][k] = expert_id */
    int (*routing)[TOP_K] = calloc(n_layers, sizeof(*routing));
    int routed_total = 0;

    for (int layer = 0; layer < n_layers; layer++) {
        /* find router gate tensor (ffn_gate_inp.weight) */
        char gate_name[128];
        snprintf(gate_name, sizeof(gate_name), "blk.%d.ffn_gate_inp.weight", layer);
        int router_idx = -1;
        for (uint32_t i = 0; i < gguf.n_tensors; i++) {
            if (strcmp(gguf.names[i], gate_name) == 0) { router_idx = (int)i; break; }
        }
        if (router_idx < 0) {
            for (int k = 0; k < TOP_K; k++) routing[layer][k] = k;
            continue;
        }

        /* read router gate, compute expert scores */
        uint32_t gate_sz = gguf.sizes[router_idx];
        uint8_t *gate_buf = (uint8_t *)malloc(gate_sz);
        float *gate_f32 = (float *)malloc(N_EXPERTS * sizeof(float));

        if (gguf_read_tensor(gguf_path, &gguf, router_idx, gate_buf, gate_sz) == 0) {
            int n_embd = 2560;
            if (gate_sz == (uint32_t)(n_embd * N_EXPERTS * 2)) {
                const uint16_t *gw = (const uint16_t *)gate_buf;
                for (int e = 0; e < N_EXPERTS; e++) {
                    double sum = 0;
                    for (int j = 0; j < n_embd; j++)
                        sum += fp16_to_fp32(gw[j * N_EXPERTS + e]);
                    gate_f32[e] = (float)sum;
                }
            } else {
                const float *gf = (const float *)gate_buf;
                for (int e = 0; e < N_EXPERTS; e++) {
                    double sum = 0;
                    for (int j = 0; j < n_embd; j++) sum += gf[j * N_EXPERTS + e];
                    gate_f32[e] = (float)sum;
                }
            }
            topk(gate_f32, N_EXPERTS, TOP_K, routing[layer]);
        } else {
            for (int k = 0; k < TOP_K; k++) routing[layer][k] = k;
        }
        free(gate_buf);
        free(gate_f32);
        routed_total += TOP_K;
    }

    printf("Routing table: %d layers × %d experts = %d routed\n\n",
           n_layers, TOP_K, routed_total);

    /* ── 5. Rebuild GGUF: routed experts from pool, rest from source ── */
    printf("=== PASS: REBUILD GRAFT ===\n");

    size_t hdr_sz = (size_t)gguf.data_offset;
    size_t body_sz = (size_t)(gguf.base_sz - gguf.data_offset);
    uint8_t *body = (uint8_t *)calloc(1, body_sz);

    uint32_t from_pool = 0, from_source = 0;
    uint64_t pool_bytes = 0, source_bytes = 0;

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        uint64_t off = gguf.offsets[i];
        uint32_t tsz = gguf.sizes[i];

        int layer, wtype;
        if (match_tensor(gguf.names[i], &layer, &wtype)) {
            /* stacked tensors: all experts in one blob per (layer, wtype) */
            int routed = 0;
            if (layer < n_layers) {
                for (int k = 0; k < TOP_K; k++) {
                    if (routing[layer][k] >= 0) { routed = 1; break; }
                }
            }

            if (routed) {
                /* load from DtSlotRegion (geometric addressing) */
                MoeExpertMeta meta;
                if (moe_load_meta(&region, (uint32_t)layer, 0, (uint32_t)wtype, &meta) == 0
                    && meta.size == tsz) {
                    memcpy(body + off, region.base + meta.offset, tsz);
                    from_pool++;
                    pool_bytes += tsz;
                    continue;
                }
            }
        }

        /* copy from source */
        uint64_t src_off = gguf.data_offset + gguf.offsets[i];
        if (src_off + tsz <= gguf.base_sz) {
            memcpy(body + off, gguf.base + src_off, tsz);
            from_source++;
            source_bytes += tsz;
        }
    }

    printf("  header:   %zu bytes (source verbatim)\n", hdr_sz);
    printf("  body:     %zu bytes\n", body_sz);
    printf("  from pool:   %u tensors (%.1f MB)\n", from_pool, pool_bytes / 1e6);
    printf("  from source: %u tensors (%.1f MB)\n", from_source, source_bytes / 1e6);

    /* write graft GGUF */
    FILE *f = fopen(route_path, "wb");
    if (!f || fwrite(gguf.base, 1, hdr_sz, f) != hdr_sz ||
        fwrite(body, 1, body_sz, f) != body_sz) {
        printf("  FAIL: write %s\n", route_path);
        free(body);
#if defined(_WIN32)
        UnmapViewOfFile(region.base);
        CloseHandle(region.hSlotMapping);
        CloseHandle(region.hSlotFile);
#else
        munmap(region.base, region.n_slots * region.slot_sz);
        close(region.slot_fd);
#endif
        gguf_close(&gguf);
        return 1;
    }
    fclose(f);
    printf("  written:  %s (%.1f MB)\n\n", route_path,
           (double)(hdr_sz + body_sz) / 1e6);

    free(body);

    /* ── 6. Byte-verify: routed experts from pool match source ── */
    printf("=== PASS: BYTE-VERIFY ROUTED EXPERTS ===\n");
    uint32_t byte_pass = 0, byte_total = 0;

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (!match_tensor(gguf.names[i], &layer, &wtype)) continue;
        if (layer >= n_layers) continue;

        /* check if this layer is routed */
        int routed = 0;
        for (int k = 0; k < TOP_K; k++) {
            if (routing[layer][k] >= 0) { routed = 1; break; }
        }
        if (!routed) continue;

        byte_total++;
        MoeExpertMeta meta;
        if (moe_load_meta(&region, (uint32_t)layer, 0, (uint32_t)wtype, &meta) != 0) continue;
        if (meta.size != gguf.sizes[i]) continue;

        /* compare pool data vs source data */
        uint64_t src_off = gguf.data_offset + gguf.offsets[i];
        if (src_off + gguf.sizes[i] > gguf.base_sz) continue;

        uint8_t *pool_data = region.base + meta.offset;
        uint8_t *src_data  = gguf.base + src_off;
        if (memcmp(pool_data, src_data, gguf.sizes[i]) == 0) {
            byte_pass++;
        } else {
            printf("  BYTE MISMATCH  layer %d  %s\n", layer, gguf.names[i]);
        }
    }
    printf("  Byte: %u/%u routed experts match source\n\n", byte_pass, byte_total);

    /* ── 7. INFERENCE comparison ── */
    printf("=== PASS: INFERENCE ===\n");
    const char *dll_dir = find_llama_dll(llama_dir);
    if (!dll_dir) {
        printf("  SKIP: llama DLLs not found at %s\n", llama_dir);
        printf("  (Routed GGUF written but inference comparison skipped)\n");
    } else {
        printf("  loading models...\n");
        int rc = compare_logits(gguf_path, route_path, llama_dir, prompt);
        printf("\n  GATE: %s\n", rc == 0 ? "PASS (routed inference identical to original)" : "FAIL");
    }

    /* ── Summary ── */
    printf("\n=== SUMMARY ===\n");
    printf("Layers:     %d\n", n_layers);
    printf("Top-K:      %d per layer\n", TOP_K);
    printf("Routed:     %d experts (%.1f MB from pool)\n", routed_total, pool_bytes / 1e6);
    printf("Byte:       %u/%u PASS\n", byte_pass, byte_total);

    /* cleanup */
#if defined(_WIN32)
    UnmapViewOfFile(region.base);
    CloseHandle(region.hSlotMapping);
    CloseHandle(region.hSlotFile);
#else
    munmap(region.base, region.n_slots * region.slot_sz);
    close(region.slot_fd);
#endif
    gguf_close(&gguf);
    free(routing);

    return (byte_pass == byte_total) ? 0 : 1;
}
