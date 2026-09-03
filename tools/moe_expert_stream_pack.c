/* tools/moe_expert_stream_pack.c — MoE Streaming from .tesspack + FFN Matmul
 * ═══════════════════════════════════════════════════════════════════════════
 * Path B: reads experts directly from .tesspack single-file container.
 * Loads only top-K selected experts (not full tensor), dequants, runs SwiGLU.
 *
 * Flow: .tesspack → capo load range → dequant Q4_K/F16 → SwiGLU FFN matmul
 *       → compare against full GGUF reference
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I core -I core/infra -I I:/llama/include
 *        -o build/moe-stream-pack.exe tools/moe_expert_stream_pack.c -lm
 * RUN:   ./build/moe-stream-pack.exe [gguf_path] [tesspack_path] [layer]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

#define N_EXPERTS   64
#define TOP_K       4
#define QK_K        256
#define K_SCALE_SIZE 12

/* ═══════════════ Q4_K DEQUANTIZATION (identical to moe_expert_stream.c) ═══════════════ */
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

static uint32_t rng_state = 42;
static float rng_f32(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return ((float)(rng_state >> 8) / 16777216.0f) * 2.0f - 1.0f;
}

static void ffn_swiglu(const float *x, const float *gate, const float *up,
                       const float *down, float *out,
                       int n_embd, int n_ff) {
    float *inter = (float *)malloc(n_ff * sizeof(float));
    for (int j = 0; j < n_ff; j++) {
        float g = 0, u = 0;
        for (int i = 0; i < n_embd; i++) {
            g += x[i] * gate[i * n_ff + j];
            u += x[i] * up[i * n_ff + j];
        }
        inter[j] = (g / (1.0f + expf(-g))) * u;
    }
    for (int j = 0; j < n_embd; j++) {
        float s = 0;
        for (int i = 0; i < n_ff; i++)
            s += inter[i] * down[i * n_embd + j];
        out[j] = s;
    }
    free(inter);
}

/* ═══════════════ LOAD EXPERT FROM .TESSPACK ═══════════════
 * Load expert `expert_id` of tensor `tensor_name` from .tesspack.
 * Uses TESS_PackIndex (mmap'd, index pre-loaded) for fast access.
 * Returns bytes loaded, or -1 on error. */
static int64_t pack_load_expert_idx(TESS_PackIndex *pi, const char *tensor_name,
                                    uint32_t n_experts, uint32_t expert_id,
                                    uint32_t cell_size, uint32_t total_cells,
                                    uint8_t *out_buf) {
    uint32_t bpe = total_cells / n_experts;
    uint32_t start_cell = expert_id * bpe;
    uint32_t end_cell = start_cell + bpe;
    int64_t out_off = 0;

    for (uint32_t cell = start_cell; cell < end_cell; ) {
        uint32_t capo_id = cell / TESS_TOTAL_SLOTS;
        uint32_t capo_local = cell % TESS_TOTAL_SLOTS;
        uint32_t capo_limit = (capo_id + 1) * TESS_TOTAL_SLOTS;
        uint32_t chunk = (end_cell < capo_limit) ? end_cell - cell : capo_limit - cell;

        TESS_CapoReader cr;
        int pack_rc = tess_pack_get_capo(pi, &cr, tensor_name, capo_id);
        if (pack_rc != 0) return -1;
        if (tess_capo_load_range(&cr, capo_local, chunk, out_buf + out_off) == 0) return -1;
        out_off += (int64_t)chunk * cell_size;
        cell += chunk;
    }
    return out_off;
}

/* ═══════════════ MAIN ═══════════════ */

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    const char *gguf_path = (argc > 1) ? argv[1] : "F:\\model\\qwen3-4b-moe-q4_k_m.gguf";
    const char *pack_path = (argc > 2) ? argv[2] : "build\\qwen3moe_moe.tesspack";
    int target_layer = (argc > 3) ? atoi(argv[3]) : 0;

    printf("=== MoE Streaming from .tesspack + FFN Matmul ===\n");
    printf("GGUF:   %s\n", gguf_path);
    printf("Pack:   %s\n", pack_path);
    printf("Layer:  %d\n\n", target_layer);

    /* ── 1. Open GGUF for router gate + tensor metadata + reference ── */
    GgufReader gguf;
    int rc = gguf_open(gguf_path, &gguf);
    if (rc != 0) { printf("FAIL: open GGUF (rc=%d)\n", rc); return 1; }
    printf("GGUF: %u tensors\n", gguf.n_tensors);

    /* ── 1b. Open .tesspack index (mmap once, scan index once) ── */
    TESS_PackIndex pack;
    int prc = tess_pack_open(&pack, pack_path);
    if (prc != 0) { printf("FAIL: open .tesspack (rc=%d)\n", prc); gguf_close(&gguf); return 1; }
    printf("Pack: %s (%u capos, %u index entries)\n", pack_path, pack.n_capos, pack.n_entries);

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
    printf("Tensor indices: router=%d gate_exp=%d down=%d up=%d\n",
           router_idx, gate_exp_idx, down_idx, up_idx);
    fflush(stdout);

    /* cell size from GGUF dtype, total cells = size / cell_size */
    static const uint32_t GGUF_CELL_SZ[] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    uint32_t cell_gate = 0, cell_up = 0, cell_down = 0;
    uint32_t total_gate = 0, total_up = 0, total_down = 0;
    if (gate_exp_idx >= 0) {
        cell_gate = GGUF_CELL_SZ[gguf.dtypes[gate_exp_idx]];
        total_gate = (uint32_t)(gguf.sizes[gate_exp_idx] / cell_gate);
    }
    if (up_idx >= 0) {
        cell_up = GGUF_CELL_SZ[gguf.dtypes[up_idx]];
        total_up = (uint32_t)(gguf.sizes[up_idx] / cell_up);
    }
    if (down_idx >= 0) {
        cell_down = GGUF_CELL_SZ[gguf.dtypes[down_idx]];
        total_down = (uint32_t)(gguf.sizes[down_idx] / cell_down);
    }
    printf("Cells: gate=%u(csz=%u) up=%u(csz=%u) down=%u(csz=%u)\n",
           total_gate, cell_gate, total_up, cell_up, total_down, cell_down);
    fflush(stdout);

    uint32_t per_expert_gate = total_gate / N_EXPERTS * cell_gate;
    uint32_t per_expert_up   = total_up / N_EXPERTS * cell_up;
    uint32_t per_expert_down = total_down / N_EXPERTS * cell_down;
    printf("Per expert: gate=%u up=%u down=%u bytes\n",
           per_expert_gate, per_expert_up, per_expert_down);

    /* ── 2. Top-K selection from router ── */
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
            } else {
                const float *gf = (const float *)gate_buf;
                for (int e = 0; e < N_EXPERTS; e++) {
                    double sum = 0;
                    for (int j = 0; j < n_embd; j++) sum += gf[j * N_EXPERTS + e];
                    gate_f32[e] = (float)sum;
                }
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

    /* ── 3. Load full tensors from GGUF for reference ── */
    uint8_t *full_gate_raw = NULL, *full_up_raw = NULL, *full_down_raw = NULL;
    if (gate_exp_idx >= 0) {
        full_gate_raw = (uint8_t *)malloc(gguf.sizes[gate_exp_idx]);
        gguf_read_tensor(gguf_path, &gguf, gate_exp_idx, full_gate_raw, gguf.sizes[gate_exp_idx]);
    }
    if (up_idx >= 0) {
        full_up_raw = (uint8_t *)malloc(gguf.sizes[up_idx]);
        gguf_read_tensor(gguf_path, &gguf, up_idx, full_up_raw, gguf.sizes[up_idx]);
    }
    if (down_idx >= 0) {
        full_down_raw = (uint8_t *)malloc(gguf.sizes[down_idx]);
        gguf_read_tensor(gguf_path, &gguf, down_idx, full_down_raw, gguf.sizes[down_idx]);
    }

    int n_embd = 2560;
    int total_per_expert = (int)(per_expert_gate / sizeof(block_q4_K)) * QK_K;
    int n_ff = total_per_expert / n_embd;
    printf("Dimensions: n_embd=%d, n_ff=%d\n\n", n_embd, n_ff);

    /* ── 4. Stream experts from .tesspack + FFN matmul ── */
    printf("=== STREAMING FROM .TESSPACK ===\n");
    int pass = 0, fail = 0;
    int64_t total_streamed = 0;

    for (int ki = 0; ki < TOP_K; ki++) {
        int e = selected[ki];
        printf("Expert %d: loading gate(%s)... ", e, gate_exp_name);

        /* allocate expert buffers */
        int64_t gate_elems = (int64_t)n_embd * n_ff;
        int64_t down_elems = (int64_t)n_ff * n_embd;
        uint8_t *gate_raw = (uint8_t *)malloc(per_expert_gate);
        uint8_t *up_raw   = (uint8_t *)malloc(per_expert_up);
        uint8_t *down_raw = (uint8_t *)malloc(per_expert_down);

        /* load from .tesspack */
        int64_t loaded_gate = pack_load_expert_idx(&pack, gate_exp_name,
                                               N_EXPERTS, e, cell_gate, total_gate, gate_raw);
        printf("gate=%lld ", (long long)loaded_gate);

        int64_t loaded_up = pack_load_expert_idx(&pack, up_name,
                                             N_EXPERTS, e, cell_up, total_up, up_raw);
        printf("up=%lld ", (long long)loaded_up);

        int64_t loaded_down = pack_load_expert_idx(&pack, down_name,
                                               N_EXPERTS, e, cell_down, total_down, down_raw);
        printf("down=%lld\n", (long long)loaded_down);

        if (loaded_gate < 0 || loaded_up < 0 || loaded_down < 0) {
            printf("  FAIL: pack load expert %d\n", e);
            fail++;
            free(gate_raw); free(up_raw); free(down_raw);
            continue;
        }
        total_streamed += loaded_gate + loaded_up + loaded_down;

        /* dequant from .tesspack */
        float *gate_f = (float *)malloc(gate_elems * sizeof(float));
        float *up_f   = (float *)malloc(gate_elems * sizeof(float));
        float *down_f = (float *)malloc(down_elems * sizeof(float));

        dequant_q4_k((const block_q4_K *)gate_raw, gate_f, gate_elems);
        dequant_q4_k((const block_q4_K *)up_raw, up_f, gate_elems);
        {
            const uint16_t *down_f16 = (const uint16_t *)down_raw;
            for (int64_t i = 0; i < down_elems; i++) down_f[i] = fp16_to_fp32(down_f16[i]);
        }

        /* random input (same seed as reference) */
        rng_state = 42 + target_layer * 1000 + e;
        float *x = (float *)malloc(n_embd * sizeof(float));
        for (int i = 0; i < n_embd; i++) x[i] = rng_f32();

        float *out_stream = (float *)malloc(n_embd * sizeof(float));
        ffn_swiglu(x, gate_f, up_f, down_f, out_stream, n_embd, n_ff);
        free(gate_f); free(up_f); free(down_f);
        free(gate_raw); free(up_raw); free(down_raw);

        /* reference from full GGUF */
        float *rgate_f = (float *)malloc(gate_elems * sizeof(float));
        float *rup_f   = (float *)malloc(gate_elems * sizeof(float));
        float *rdown_f = (float *)malloc(down_elems * sizeof(float));

        dequant_q4_k((const block_q4_K *)(full_gate_raw + (uint64_t)e * per_expert_gate),
                     rgate_f, gate_elems);
        dequant_q4_k((const block_q4_K *)(full_up_raw + (uint64_t)e * per_expert_up),
                     rup_f, gate_elems);
        {
            const uint16_t *ref_down_f16 = (const uint16_t *)(full_down_raw + (uint64_t)e * per_expert_down);
            for (int64_t i = 0; i < down_elems; i++) rdown_f[i] = fp16_to_fp32(ref_down_f16[i]);
        }

        float *out_ref = (float *)malloc(n_embd * sizeof(float));
        ffn_swiglu(x, rgate_f, rup_f, rdown_f, out_ref, n_embd, n_ff);
        free(rgate_f); free(rup_f); free(rdown_f);

        /* compare */
        float maxdiff = 0;
        for (int i = 0; i < n_embd; i++) {
            float d = fabsf(out_stream[i] - out_ref[i]);
            if (d > maxdiff) maxdiff = d;
        }
        double dot = 0, norm_s = 0, norm_r = 0;
        for (int i = 0; i < n_embd; i++) {
            dot    += out_stream[i] * out_ref[i];
            norm_s += out_stream[i] * out_stream[i];
            norm_r += out_ref[i] * out_ref[i];
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

    /* ── 5. Byte-compare: streamed data vs GGUF ── */
    printf("\n=== BYTE-COMPARE (pack vs GGUF) ===\n");
    uint32_t byte_pass = 0, byte_total = 0;
    for (int ki = 0; ki < TOP_K; ki++) {
        int e = selected[ki];
        const char *wtype_names[] = {"ffn_down_exps", "ffn_gate_exps", "ffn_up_exps"};
        uint32_t per_expert_sz[] = {per_expert_down, per_expert_gate, per_expert_up};
        const char *tensor_names[] = {down_name, gate_exp_name, up_name};
        uint32_t cells[] = {cell_down, cell_gate, cell_up};
        uint32_t totals[] = {total_down, total_gate, total_up};
        uint8_t *ref_bufs[] = {full_down_raw, full_gate_raw, full_up_raw};
        int ref_idxs[] = {down_idx, gate_exp_idx, up_idx};

        for (int wt = 0; wt < 3; wt++) {
            byte_total++;
            if (ref_idxs[wt] < 0) continue;

            /* load expert from pack */
            uint8_t *streamed = (uint8_t *)malloc(per_expert_sz[wt]);
            int64_t loaded = pack_load_expert_idx(&pack, tensor_names[wt],
                                              N_EXPERTS, e, cells[wt], totals[wt], streamed);
            if (loaded < 0) {
                printf("  BYTE FAIL  expert %2d  %s (pack load error)\n", e, wtype_names[wt]);
                free(streamed);
                continue;
            }

            uint8_t *ref = ref_bufs[wt] + (uint64_t)e * per_expert_sz[wt];
            if (memcmp(streamed, ref, per_expert_sz[wt]) == 0) {
                byte_pass++;
            } else {
                printf("  BYTE FAIL  expert %2d  %s\n", e, wtype_names[wt]);
            }
            free(streamed);
        }
    }
    printf("Byte: %u/%u match\n", byte_pass, byte_total);

    /* ── Summary ── */
    printf("\n=== SUMMARY ===\n");
    printf("FFN matmul: %d/%d PASS\n", pass, TOP_K);
    printf("Byte:      %u/%u PASS\n", byte_pass, byte_total);
    printf("Streamed:  %.1f MB from .tesspack (vs %.1f MB full tensor)\n",
           (double)total_streamed / 1e6,
           (double)((per_expert_gate + per_expert_up + per_expert_down) * N_EXPERTS) / 1e6);
    printf("Savings:   %.1f%% bandwidth (loaded %.1f%% of full)\n",
           100.0 * (1.0 - (double)total_streamed / ((per_expert_gate + per_expert_up + per_expert_down) * N_EXPERTS)),
           100.0 * (double)total_streamed / ((per_expert_gate + per_expert_up + per_expert_down) * N_EXPERTS));

    /* cleanup */
    free(full_gate_raw); free(full_up_raw); free(full_down_raw);
    tess_pack_close(&pack);
    gguf_close(&gguf);

    return (pass == TOP_K && byte_pass == byte_total) ? 0 : 1;
}
