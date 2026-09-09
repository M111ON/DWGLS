/* test_hyp_fusion_e2e.c — End-to-end: real GGUF → Wang→GNN pipeline
 *
 * Opens a real GGUF, reads tensor bytes, extracts 3×3 grids via rank-fingerprint,
 * computes GNN+Fan24 features, and runs the full Wang→GNN scoring pipeline.
 *
 * Build: gcc -O2 -Wall -Wextra -Icore -o tests/test_hyp_fusion_e2e tests/test_hyp_fusion_e2e.c -lm
 * Run:   tests/test_hyp_fusion_e2e I:/model/SmolLM2-360M-Instruct.Q8_0.gguf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gguf_reader.h"
#include "hyp_fusion.h"

static int pass = 0, fail = 0;

static void check(int cond, const char *name) {
    if (cond) { pass++; printf("  PASS  %s\n", name); }
    else      { fail++; printf("  FAIL  %s\n", name); }
}

/* fp_rank: rank values by descending, assign position → grid[9]
 * Same as Python gnn_colab_train_fan24.py fp_rank()[:9] */
static void fp_rank_grid(const float *vals, int n, float grid[9]) {
    int idx[81];
    for (int i = 0; i < 81 && i < n; i++) idx[i] = i;
    /* descending sort by value */
    for (int i = 0; i < 80 && i < n-1; i++) {
        int mx = i;
        for (int j = i+1; j < 81 && j < n; j++)
            if (vals[idx[j]] > vals[idx[mx]]) mx = j;
        int tmp = idx[i]; idx[i] = idx[mx]; idx[mx] = tmp;
    }
    for (int i = 0; i < 9; i++) {
        int pos = 0;
        for (int j = 0; j < 81 && j < n; j++)
            if (idx[j] == i) { pos = j; break; }
        grid[i] = (float)(pos + 1);
    }
}

/* Compute 8 line-sums from a 3×3 grid */
static void line_sums(const float grid[9], float lsum[8]) {
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},  /* rows */
        {0,3,6},{1,4,7},{2,5,8},  /* cols */
        {0,4,8},{2,4,6},          /* diags */
    };
    for (int l = 0; l < 8; l++)
        lsum[l] = grid[lines[l][0]] + grid[lines[l][1]] + grid[lines[l][2]];
}

/* Dequantize a Q8_0 block: 2 bytes FP16 scale + 32 bytes int8
 * Simple approximation: skip FP16 decode, use raw int8 values */
static int dequant_q8_0_block(const uint8_t *block, float *out) {
    /* block[0..1] = FP16 scale (skip for now — use raw int8) */
    /* block[2..33] = 32 int8 values */
    for (int i = 0; i < 32; i++)
        out[i] = (float)(int8_t)block[2 + i];
    return 32;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf";
    printf("=== test_hyp_fusion_e2e: GGUF → Wang → GNN ===\n");
    printf("Model: %s\n\n", path);

    GgufReader gr;
    if (gguf_open(path, &gr) != 0) {
        printf("  FATAL: Cannot open %s\n", path);
        return 1;
    }
    printf("[1] GGUF opened: %u tensors, %.1f MB\n",
           gr.n_tensors, (double)gr.base_sz / 1e6);
    check(gr.n_tensors > 0, "has tensors");

    /* Find first weight tensor (skip token/embedding) */
    int target = -1;
    for (uint32_t i = 0; i < gr.n_tensors; i++) {
        if (gr.n_dims[i] >= 2 && gr.sizes[i] > 256) {
            target = (int)i;
            break;
        }
    }
    check(target >= 0, "found weight tensor");
    if (target < 0) { gguf_close(&gr); return 1; }

    printf("[2] Tensor #%d: \"%s\" dtype=%u dims=%u",
           target, gr.names[target], gr.dtypes[target], gr.n_dims[target]);
    for (int d = 0; d < gr.n_dims[target]; d++)
        printf(" [%u]", (unsigned)gr.dims[target*4+d]);
    printf("\n");

    /* Read raw bytes from tensor data region */
    uint64_t offset = gr.data_offset + gr.offsets[target];
    uint32_t sz = gr.sizes[target];
    printf("   offset=%u size=%u\n", (unsigned)offset, sz);
    check(sz >= 34, "tensor has data");

    /* Extract float values from raw Q8_0 blocks */
    const uint8_t *raw = gr.base + offset;
    float vals[256];
    int n_vals = 0;
    int block_sz = 34;  /* Q8_0: 2B scale + 32B data */
    if (gr.dtypes[target] == 0) block_sz = 4;  /* F32 */
    else if (gr.dtypes[target] == 1) block_sz = 2;  /* F16 */
    else block_sz = 34;  /* Q8_0 */

    for (uint32_t off = 0; off + (uint32_t)block_sz <= sz && n_vals < 256; off += block_sz) {
        if (gr.dtypes[target] == 0) {
            /* F32: read 4-byte float */
            float v;
            memcpy(&v, raw + off, 4);
            vals[n_vals++] = v;
        } else if (gr.dtypes[target] == 8) {
            /* Q8_0: skip 2-byte scale, read 32 int8 */
            float tmp[32];
            int cnt = dequant_q8_0_block(raw + off, tmp);
            for (int i = 0; i < cnt && n_vals < 256; i++)
                vals[n_vals++] = tmp[i];
        } else {
            /* Fallback: read raw byte as value */
            vals[n_vals++] = (float)raw[off];
        }
    }
    printf("[3] Extracted %d values from tensor\n", n_vals);
    check(n_vals >= 81, "enough values for 3x3 grid");

    /* ── Extract 3×3 grid via rank-fingerprint ── */
    float grid[9], lsum[8];
    fp_rank_grid(vals, n_vals, grid);
    line_sums(grid, lsum);
    printf("[4] Grid (rank-fingerprint): ");
    for (int i = 0; i < 9; i++) printf("%.0f ", grid[i]);
    printf("\n    Line sums: ");
    for (int i = 0; i < 8; i++) printf("%.0f ", lsum[i]);
    printf("\n");
    check(grid[0] > 0, "grid values > 0");

    /* ── Compute GNN node features ── */
    float nf_a[108], ev_a[4];
    gnn_f24_node_features(grid, lsum, 0, nf_a);
    gnn_f24_edges(lsum, ev_a);
    printf("[5] nf_a[0..5]: ");
    for (int i = 0; i < 6; i++) printf("%.4f ", nf_a[i]);
    printf("\n");
    check(nf_a[0] >= 0.0f, "nf non-negative (rank/9 can exceed 1.0)");

    /* ── Create second tile from different offset ── */
    float grid2[9], lsum2[8], nf_b[108], ev_b[4];
    if (n_vals >= 162) {
        fp_rank_grid(vals + 81, n_vals - 81, grid2);
    } else {
        /* shift and re-rank */
        float shifted[256];
        for (int i = 0; i < n_vals; i++) shifted[i] = vals[(i + 10) % n_vals];
        fp_rank_grid(shifted, n_vals, grid2);
    }
    line_sums(grid2, lsum2);
    gnn_f24_node_features(grid2, lsum2, 1, nf_b);
    gnn_f24_edges(lsum2, ev_b);
    printf("    nf_b[0..5]: ");
    for (int i = 0; i < 6; i++) printf("%.4f ", nf_b[i]);
    printf("\n");

    /* ── GNN score (two views of same tensor) ── */
    float score_tb = hyp_gnn_score(nf_a, ev_a, nf_b, ev_b, 1.0f);
    float score_lr = hyp_gnn_score(nf_a, ev_a, nf_b, ev_b, 0.0f);
    printf("[6] GNN scores (same tensor, two views):\n");
    printf("    top-bottom: %.4f\n", score_tb);
    printf("    left-right: %.4f\n", score_lr);
    check(score_tb >= 0.0f && score_tb <= 1.0f, "TB score in [0,1]");
    check(score_lr >= 0.0f && score_lr <= 1.0f, "LR score in [0,1]");

    /* ── Fan24 position score (no grid needed) ── */
    float sp0 = hyp_gnn_score_position(0, 1);
    float sp1 = hyp_gnn_score_position(0, 100);
    printf("[7] Fan24 position scores:\n");
    printf("    pos(0,1):   %.4f\n", sp0);
    printf("    pos(0,100):  %.4f\n", sp1);
    check(sp0 >= 0.0f && sp0 <= 1.0f, "position score in [0,1]");

    /* ── Cross-tensor comparison (different tensors) ── */
    if (gr.n_tensors >= 3) {
        int t2 = target + 1;
        if (t2 >= (int)gr.n_tensors) t2 = 0;
        uint64_t off2 = gr.data_offset + gr.offsets[t2];
        uint32_t sz2 = gr.sizes[t2];
        float vals2[256];
        int n2 = 0;
        for (uint32_t o = 0; o + (uint32_t)block_sz <= sz2 && n2 < 256; o += block_sz) {
            if (gr.dtypes[t2] == 0) {
                float v; memcpy(&v, gr.base+off2+o, 4); vals2[n2++] = v;
            } else if (gr.dtypes[t2] == 8) {
                float tmp[32]; int cnt = dequant_q8_0_block(gr.base+off2+o, tmp);
                for (int i = 0; i < cnt && n2 < 256; i++) vals2[n2++] = tmp[i];
            } else {
                vals2[n2++] = (float)gr.base[off2+o];
            }
        }
        float grid3[9], lsum3[8], nf_c[108], ev_c[4];
        fp_rank_grid(vals2, n2, grid3);
        line_sums(grid3, lsum3);
        gnn_f24_node_features(grid3, lsum3, 2, nf_c);
        gnn_f24_edges(lsum3, ev_c);

        float score_cross = hyp_gnn_score(nf_a, ev_a, nf_c, ev_c, 1.0f);
        printf("[8] Cross-tensor score (tensor %d → %d): %.4f\n", target, t2, score_cross);
        check(score_cross >= 0.0f && score_cross <= 1.0f, "cross-tensor in [0,1]");
    }

    gguf_close(&gr);

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
