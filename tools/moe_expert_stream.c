/* tools/moe_expert_stream.c — MoE Expert Streaming + FFN Matmul Demo
 * ═══════════════════════════════════════════════════════════════════════
 * Proves selective expert loading + dequantized MoE FFN computation:
 *   1. Stream only router-selected experts (top-4 of 64)
 *   2. Dequant Q4_K → f32
 *   3. Run SwiGLU FFN: SiLU(x*gate) * (x*up) then *down
 *   4. Compare against full-tensor reference (same experts, full load)
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I core -I core/infra -I I:/llama/include
 *        -o build/moe-stream.exe tools/moe_expert_stream.c -lm
 * RUN:   ./build/moe-stream.exe [gguf_path] [layer]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#endif

#include "../core/gguf_reader.h"
#include "../core/moe_expert_addr.h"
#include "../core/infra/dramtile_store.h"
#include "../core/moe_expert_store.h"

#define N_EXPERTS   64
#define TOP_K       4
#define QK_K        256
#define K_SCALE_SIZE 12

/* ═══════════════ Q4_K DEQUANTIZATION ═══════════════ */
/* Replicates ggml's block_q4_K layout:
 *   d (f16) — super-block scale for quantized scales
 *   dmin (f16) — super-block scale for quantized mins
 *   scales[12] — 12 bytes, scales and mins quantized with 6 bits
 *   qs[128] — 128 bytes of Q4 data (256 values)
 * Total: 144 bytes per 256 elements */
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

static inline void get_scale_min_k4(int j, const uint8_t *q, uint8_t *d, uint8_t *m) {
    if (j < 4) {
        *d = q[j] & 63; *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

static void dequant_q4_k(const block_q4_K *blocks, float *out, int64_t k) {
    assert(k % QK_K == 0);
    int nb = (int)(k / QK_K);
    for (int i = 0; i < nb; i++) {
        const uint8_t *q = blocks[i].qs;
        float d   = fp16_to_fp32(blocks[i].d);
        float min = fp16_to_fp32(blocks[i].dmin);
        int is = 0;
        uint8_t sc, m;
        for (int j = 0; j < QK_K; j += 64) {
            get_scale_min_k4(is + 0, blocks[i].scales, &sc, &m);
            float d1 = d * sc, m1 = min * m;
            get_scale_min_k4(is + 1, blocks[i].scales, &sc, &m);
            float d2 = d * sc, m2 = min * m;
            for (int l = 0; l < 32; l++) *out++ = d1 * (q[l] & 0xF) - m1;
            for (int l = 0; l < 32; l++) *out++ = d2 * (q[l] >> 4) - m2;
            q += 32; is += 2;
        }
    }
}

/* ═══════════════ HELPERS ═══════════════ */

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

/* Simple seeded RNG for reproducible input */
static uint32_t rng_state = 42;
static float rng_f32(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return ((float)(rng_state >> 8) / 16777216.0f) * 2.0f - 1.0f;
}

/* MoE SwiGLU FFN: out[n_embd] = down( SiLU(x[n_embd] @ gate[n_embd,n_ff]) * (x @ up[n_embd,n_ff]) ) */
static void ffn_swiglu(const float *x, const float *gate, const float *up,
                       const float *down, float *out,
                       int n_embd, int n_ff) {
    /* intermediate = SiLU(x @ gate) * (x @ up) */
    float *inter = (float *)malloc(n_ff * sizeof(float));
    for (int j = 0; j < n_ff; j++) {
        float g = 0, u = 0;
        for (int i = 0; i < n_embd; i++) {
            g += x[i] * gate[i * n_ff + j];
            u += x[i] * up[i * n_ff + j];
        }
        inter[j] = (g / (1.0f + expf(-g))) * u;  /* SiLU(g) * u */
    }
    /* out = inter @ down */
    for (int j = 0; j < n_embd; j++) {
        float s = 0;
        for (int i = 0; i < n_ff; i++)
            s += inter[i] * down[i * n_embd + j];
        out[j] = s;
    }
    free(inter);
}

/* ═══════════════ MAIN ═══════════════ */

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    const char *gguf_path = (argc > 1) ? argv[1] : "F:\\model\\qwen3-4b-moe-q4_k_m.gguf";
    int target_layer = (argc > 2) ? atoi(argv[2]) : 0;
    const char *region_path = "moe_expert_region.bin";

    printf("=== MoE Expert Streaming + FFN Matmul ===\n");
    printf("GGUF:   %s\n", gguf_path);
    printf("Layer:  %d\n", target_layer);
    printf("Region: %s\n\n", region_path);

    /* ── 1. Open baked region ── */
    DtSlotRegion region;
    memset(&region, 0, sizeof(region));
    region.slot_sz = sizeof(MoeExpertMeta);
    region.n_slots = 20736;
    {
#if defined(_WIN32)
        HANDLE hf = CreateFileA(region_path, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            printf("FAIL: open %s (run moe-bake first)\n", region_path);
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
        if (fd < 0) { printf("FAIL: open %s\n", region_path); return 1; }
        struct stat st; fstat(fd, &st);
        size_t total = (size_t)st.st_size;
        region.base = (uint8_t*)mmap(NULL, total, PROT_READ, MAP_PRIVATE, fd, 0);
        region.slot_fd = fd;
#endif
        region.is_twin = 1;
        printf("Region loaded: %u slots, total file %.1f MB\n",
               (unsigned)region.n_slots, (double)total / 1e6);
    }

    /* ── 2. Load stacked tensor metadata ── */
    MoeExpertMeta meta_gate, meta_up, meta_down;
    if (moe_load_meta(&region, target_layer, 0, 0, &meta_down) != 0 ||
        moe_load_meta(&region, target_layer, 0, 1, &meta_gate) != 0 ||
        moe_load_meta(&region, target_layer, 0, 2, &meta_up) != 0) {
        printf("FAIL: load metadata for layer %d\n", target_layer);
        dt_slot_destroy(&region);
        return 1;
    }
    printf("Stacked tensor sizes: down=%u gate=%u up=%u bytes\n",
           meta_down.size, meta_gate.size, meta_up.size);

    uint32_t per_expert_down = meta_down.size / N_EXPERTS;
    uint32_t per_expert_gate = meta_gate.size / N_EXPERTS;
    uint32_t per_expert_up   = meta_up.size / N_EXPERTS;

    /* ── 3. Open GGUF for reference + router gate ── */
    GgufReader gguf;
    printf("Opening GGUF: %s\n", gguf_path);
    int rc = gguf_open(gguf_path, &gguf);
    if (rc != 0) {
        printf("FAIL: open GGUF (rc=%d)\n", rc);
        dt_slot_destroy(&region);
        return 1;
    }
    printf("GGUF opened: %u tensors, data_offset=%llu\n", gguf.n_tensors, (unsigned long long)gguf.data_offset);

    /* find gate + expert tensors in GGUF */
    char gate_inp_name[128], gate_exp_name[128], down_name[128], up_name[128];
    snprintf(gate_inp_name, sizeof(gate_inp_name), "blk.%d.ffn_gate_inp.weight", target_layer);
    snprintf(gate_exp_name, sizeof(gate_exp_name), "blk.%d.ffn_gate_exps.weight", target_layer);
    snprintf(down_name, sizeof(down_name), "blk.%d.ffn_down_exps.weight", target_layer);
    snprintf(up_name,   sizeof(up_name),   "blk.%d.ffn_up_exps.weight",   target_layer);

    int router_idx = -1, gate_exp_idx = -1, down_idx = -1, up_idx = -1;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (strcmp(gguf.names[i], gate_inp_name) == 0) router_idx = (int)i;
        if (strcmp(gguf.names[i], gate_exp_name) == 0) gate_exp_idx = (int)i;
        if (strcmp(gguf.names[i], down_name) == 0) down_idx = (int)i;
        if (strcmp(gguf.names[i], up_name) == 0)   up_idx   = (int)i;
    }
    printf("Tensor indices: router=%d gate_exp=%d down=%d up=%d\n", router_idx, gate_exp_idx, down_idx, up_idx);
    if (down_idx >= 0)      printf("  down size=%u bytes\n", gguf.sizes[down_idx]);
    if (gate_exp_idx >= 0)  printf("  gate_exp size=%u bytes\n", gguf.sizes[gate_exp_idx]);
    if (up_idx >= 0)        printf("  up size=%u bytes\n", gguf.sizes[up_idx]);

    /* ── 4. Top-k selection (uses router gate = ffn_gate_inp) ── */
    int selected[TOP_K];
    if (router_idx >= 0) {
        uint32_t gate_sz = gguf.sizes[router_idx];
        float *gate_f32 = (float *)malloc(N_EXPERTS * sizeof(float));
        uint8_t *gate_buf = (uint8_t *)malloc(gate_sz);
        if (gguf_read_tensor(gguf_path, &gguf, router_idx, gate_buf, gate_sz) == 0) {
            int n_embd = 2560;
            if (gate_sz == (uint32_t)(n_embd * N_EXPERTS * 2)) {
                const uint16_t *gate_u16 = (const uint16_t *)gate_buf;
                for (int e = 0; e < N_EXPERTS; e++) {
                    double sum = 0;
                    for (int j = 0; j < n_embd; j++)
                        sum += fp16_to_fp32(gate_u16[j * N_EXPERTS + e]);
                    gate_f32[e] = (float)sum;
                }
                printf("Router gate: f16\n");
            } else {
                const float *gf = (const float *)gate_buf;
                for (int e = 0; e < N_EXPERTS; e++) {
                    double sum = 0;
                    for (int j = 0; j < n_embd; j++) sum += gf[j * N_EXPERTS + e];
                    gate_f32[e] = (float)sum;
                }
                printf("Router gate: f32\n");
            }
            topk(gate_f32, N_EXPERTS, TOP_K, selected);
        } else {
            for (int i = 0; i < TOP_K; i++) selected[i] = i;
        }
        free(gate_buf);
        free(gate_f32);
    } else {
        for (int i = 0; i < TOP_K; i++) selected[i] = i;
    }

    printf("Selected experts: [");
    for (int i = 0; i < TOP_K; i++) printf("%d%s", selected[i], i < TOP_K-1 ? ", " : "");
    printf("]\n\n");

    /* ── 5. Load full stacked tensors from GGUF for reference ── */
    uint8_t *full_down_raw = NULL, *full_gate_raw = NULL, *full_up_raw = NULL;
    if (down_idx >= 0) {
        full_down_raw = (uint8_t *)malloc(gguf.sizes[down_idx]);
        if (!full_down_raw) { printf("FAIL: malloc down %u\n", gguf.sizes[down_idx]); return 1; }
        printf("Reading full down tensor (%u bytes)...\n", gguf.sizes[down_idx]);
        rc = gguf_read_tensor(gguf_path, &gguf, down_idx, full_down_raw, gguf.sizes[down_idx]);
        printf("  rc=%d\n", rc);
    }
    if (gate_exp_idx >= 0) {
        full_gate_raw = (uint8_t *)malloc(gguf.sizes[gate_exp_idx]);
        if (!full_gate_raw) { printf("FAIL: malloc gate_exp %u\n", gguf.sizes[gate_exp_idx]); return 1; }
        printf("Reading full gate_exp tensor (%u bytes)...\n", gguf.sizes[gate_exp_idx]);
        rc = gguf_read_tensor(gguf_path, &gguf, gate_exp_idx, full_gate_raw, gguf.sizes[gate_exp_idx]);
        printf("  rc=%d\n", rc);
    }
    if (up_idx >= 0) {
        full_up_raw = (uint8_t *)malloc(gguf.sizes[up_idx]);
        if (!full_up_raw) { printf("FAIL: malloc up %u\n", gguf.sizes[up_idx]); return 1; }
        printf("Reading full up tensor (%u bytes)...\n", gguf.sizes[up_idx]);
        rc = gguf_read_tensor(gguf_path, &gguf, up_idx, full_up_raw, gguf.sizes[up_idx]);
        printf("  rc=%d\n", rc);
    }

    /* infer dimensions from sizes */
    int n_embd = 2560;
    /* per_expert_gate = Q4_K blocks for n_embd * n_ff_per_expert values */
    /* blocks = per_expert_gate / sizeof(block_q4_K), elements = blocks * QK_K */
    /* gate shape per expert: [n_embd, n_ff] → n_ff = total_elements / n_embd */
    int total_per_expert = (int)(per_expert_gate / sizeof(block_q4_K)) * QK_K;
    int n_ff = total_per_expert / n_embd;
    printf("Dimensions: n_embd=%d, n_ff=%d (total_per_expert=%d)\n", n_embd, n_ff, total_per_expert);

    /* ── 6. FFN matmul: streamed experts vs full-tensor experts ── */
    printf("\n=== FFN MATMUL (SwiGLU) ===\n");
    int pass = 0, fail = 0;

    for (int ki = 0; ki < TOP_K; ki++) {
        int e = selected[ki];

        /* random input */
        rng_state = 42 + target_layer * 1000 + e;
        float *x = (float *)malloc(n_embd * sizeof(float));
        for (int i = 0; i < n_embd; i++) x[i] = rng_f32();

        /* ── STREAMED: dequant from pool ── */
        int64_t gate_elems = (int64_t)n_embd * n_ff;
        int64_t down_elems = (int64_t)n_ff * n_embd;
        float *gate_f = (float *)malloc(gate_elems * sizeof(float));
        float *up_f   = (float *)malloc(gate_elems * sizeof(float));
        float *down_f = (float *)malloc(down_elems * sizeof(float));

        const block_q4_K *pool_gate = (const block_q4_K *)(region.base + meta_gate.offset + (uint64_t)e * per_expert_gate);
        const block_q4_K *pool_up   = (const block_q4_K *)(region.base + meta_up.offset   + (uint64_t)e * per_expert_up);
        dequant_q4_k(pool_gate, gate_f, n_embd * n_ff);
        dequant_q4_k(pool_up,   up_f,   n_embd * n_ff);
        /* down is F16 — dequant f16→f32 */
        {
            const uint16_t *pool_down_f16 = (const uint16_t *)(region.base + meta_down.offset + (uint64_t)e * per_expert_down);
            int64_t n_down = (int64_t)n_ff * n_embd;
            for (int64_t i = 0; i < n_down; i++) down_f[i] = fp16_to_fp32(pool_down_f16[i]);
        }

        float *out_stream = (float *)malloc(n_embd * sizeof(float));
        ffn_swiglu(x, gate_f, up_f, down_f, out_stream, n_embd, n_ff);
        free(gate_f); free(up_f); free(down_f);

        /* ── REFERENCE: dequant from full GGUF ── */
        float *rgate_f = (float *)malloc(gate_elems * sizeof(float));
        float *rup_f   = (float *)malloc(gate_elems * sizeof(float));
        float *rdown_f = (float *)malloc(down_elems * sizeof(float));

        const block_q4_K *ref_gate = (const block_q4_K *)(full_gate_raw + (uint64_t)e * per_expert_gate);
        const block_q4_K *ref_up   = (const block_q4_K *)(full_up_raw   + (uint64_t)e * per_expert_up);
        dequant_q4_k(ref_gate, rgate_f, n_embd * n_ff);
        dequant_q4_k(ref_up,   rup_f,   n_embd * n_ff);
        /* down is F16 — convert f16→f32 */
        {
            const uint16_t *ref_down_f16 = (const uint16_t *)(full_down_raw + (uint64_t)e * per_expert_down);
            int64_t n_down = (int64_t)n_ff * n_embd;
            for (int64_t i = 0; i < n_down; i++) rdown_f[i] = fp16_to_fp32(ref_down_f16[i]);
        }

        float *out_ref = (float *)malloc(n_embd * sizeof(float));
        ffn_swiglu(x, rgate_f, rup_f, rdown_f, out_ref, n_embd, n_ff);
        free(rgate_f); free(rup_f); free(rdown_f);

        /* ── compare ── */
        float maxdiff = 0;
        for (int i = 0; i < n_embd; i++) {
            float d = fabsf(out_stream[i] - out_ref[i]);
            if (d > maxdiff) maxdiff = d;
        }
        /* cosine similarity */
        double dot = 0, norm_s = 0, norm_r = 0;
        for (int i = 0; i < n_embd; i++) {
            dot     += out_stream[i] * out_ref[i];
            norm_s  += out_stream[i] * out_stream[i];
            norm_r  += out_ref[i] * out_ref[i];
        }
        float cos_sim = (float)(dot / (sqrt(norm_s) * sqrt(norm_r) + 1e-12));

        if (maxdiff < 0.001f) {
            printf("  PASS  expert %2d  maxdiff=%.6f  cos=%.9f\n", e, maxdiff, cos_sim);
            pass++;
        } else {
            printf("  FAIL  expert %2d  maxdiff=%.6f  cos=%.9f\n", e, maxdiff, cos_sim);
            fail++;
        }

        free(x); free(out_stream); free(out_ref);
    }

    /* ── 7. Byte-compare (still in code for sanity) ── */
    printf("\n=== BYTE-COMPARE (pool vs GGUF) ===\n");
    uint32_t byte_pass = 0, byte_total = 0;
    uint8_t *ref_buf = (uint8_t *)malloc(64 * 1024 * 1024);
    for (int ki = 0; ki < TOP_K; ki++) {
        int e = selected[ki];
        const char *wtype_names[] = {"ffn_down_exps", "ffn_gate_exps", "ffn_up_exps"};
        uint32_t per_expert_sz[] = {per_expert_down, per_expert_gate, per_expert_up};
        MoeExpertMeta *metas[] = {&meta_down, &meta_gate, &meta_up};
        int ref_idxs[] = {down_idx, gate_exp_idx, up_idx};

        for (int wt = 0; wt < 3; wt++) {
            byte_total++;
            if (ref_idxs[wt] < 0) continue;
            uint32_t offset = e * per_expert_sz[wt];
            uint8_t *streamed = region.base + metas[wt]->offset + offset;
            uint8_t *ref = (uint8_t *)(wt == 0 ? full_down_raw : wt == 1 ? full_gate_raw : full_up_raw) + offset;
            if (memcmp(streamed, ref, per_expert_sz[wt]) == 0) {
                byte_pass++;
            } else {
                printf("  BYTE FAIL  expert %2d  %s\n", e, wtype_names[wt]);
            }
        }
    }
    printf("Byte: %u/%u match\n", byte_pass, byte_total);

    /* ── Summary ── */
    printf("\n=== SUMMARY ===\n");
    printf("FFN matmul: %d/%d PASS\n", pass, TOP_K);
    printf("Byte:      %u/%u PASS\n", byte_pass, byte_total);

    /* cleanup */
    free(full_down_raw); free(full_gate_raw); free(full_up_raw); free(ref_buf);
#if defined(_WIN32)
    UnmapViewOfFile(region.base);
    CloseHandle(region.hSlotMapping);
    CloseHandle(region.hSlotFile);
#else
    munmap(region.base, region.n_slots * region.slot_sz);
    close(region.slot_fd);
#endif
    gguf_close(&gguf);

    return (pass == TOP_K && byte_pass == byte_total) ? 0 : 1;
}
