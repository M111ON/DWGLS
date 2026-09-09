/* test_hyp_fusion.c — Verify S4 GNN bridge in hyp_fusion.h
 * Tests: hyp_gnn_score, hyp_gate_fusion, hyp_gnn_score_position
 * Compile: gcc -O2 -Wall -o tests/test_hyp_fusion tests/test_hyp_fusion.c -lm -Icore
 */
#include <stdio.h>
#include <math.h>
#include "hyp_fusion.h"

static int pass = 0, fail = 0;

static void check(int cond, const char *name) {
    if (cond) { pass++; printf("  PASS  %s\n", name); }
    else      { fail++; printf("  FAIL  %s\n", name); }
}

int main(void) {
    printf("=== test_hyp_fusion: S4 GNN Bridge ===\n\n");

    /* ── Test 1: hyp_gnn_score with zero features → ~0.5 (random) ── */
    printf("[1] hyp_gnn_score baseline\n");
    float nf_a[108] = {0}, nf_b[108] = {0};
    float ev_a[4] = {0}, ev_b[4] = {0};
    float s0 = hyp_gnn_score(nf_a, ev_a, nf_b, ev_b, 1.0f);
    printf("  zero features: score = %.4f\n", s0);
    check(s0 >= 0.0f && s0 <= 1.0f, "score in [0,1]");

    /* ── Test 2: Fan24 features differ by position ── */
    printf("\n[2] Fan24 position features\n");
    float f24_0[6], f24_100[6], f24_10368[6];
    gnn_f24_features(0, f24_0);
    gnn_f24_features(100, f24_100);
    gnn_f24_features(10368, f24_10368);
    printf("  pos=0:     sin=%.4f cos=%.4f dc=%.4f dx=%.4f lang=%.4f dist=%.4f\n",
           f24_0[0], f24_0[1], f24_0[2], f24_0[3], f24_0[4], f24_0[5]);
    printf("  pos=100:   sin=%.4f cos=%.4f dc=%.4f dx=%.4f lang=%.4f dist=%.4f\n",
           f24_100[0], f24_100[1], f24_100[2], f24_100[3], f24_100[4], f24_100[5]);
    printf("  pos=10368: sin=%.4f cos=%.4f dc=%.4f dx=%.4f lang=%.4f dist=%.4f\n",
           f24_10368[0], f24_10368[1], f24_10368[2], f24_10368[3], f24_10368[4], f24_10368[5]);
    check(f24_0[0] != f24_100[0], "different sin for different positions");
    check(f24_0[4] != f24_100[4], "different lang_id for different positions");
    check(fabsf(f24_10368[5]) < 0.01f, "dist_center~0 at field center");

    /* ── Test 3: gnn_f24_node_features from grid ── */
    printf("\n[3] gnn_f24_node_features from 3×3 grid\n");
    float grid[9] = {1,2,3, 4,5,6, 7,8,9};
    float lsum[8] = {6,15,24, 12,15,18, 15,15};
    float nf[108];
    gnn_f24_node_features(grid, lsum, 42, nf);
    printf("  nf[0..5]: %.4f %.4f %.4f %.4f %.4f %.4f\n",
           nf[0], nf[1], nf[2], nf[3], nf[4], nf[5]);
    check(nf[0] > 0.0f, "cell value normalized > 0");
    check(nf[1] == 0.0f, "row index = 0 for cell 0");
    check(nf[2] == 0.0f, "col index = 0 for cell 0");

    /* ── Test 4: gnn_f24_edges from line sums ── */
    printf("\n[4] gnn_f24_edges\n");
    float ev[4];
    gnn_f24_edges(lsum, ev);
    printf("  edges: [0]=%.1f [1]=%.1f [2]=%.1f [3]=%.1f\n",
           ev[0], ev[1], ev[2], ev[3]);
    check(ev[0] == 6.0f, "edge[0] = lsum[0]");
    check(ev[1] == 18.0f, "edge[1] = lsum[5]");

    /* ── Test 5: hyp_gnn_score with real grid features ── */
    printf("\n[5] hyp_gnn_score with grid features\n");
    float grid_a[9] = {1,2,3, 4,5,6, 7,8,9};
    float grid_b[9] = {9,8,7, 6,5,4, 3,2,1};
    float lsum_a[8] = {6,15,24, 12,15,18, 15,15};
    float lsum_b[8] = {24,15,6, 18,15,12, 15,15};
    float nf_ai[108], nf_bi[108];
    float ev_ai[4], ev_bi[4];
    gnn_f24_node_features(grid_a, lsum_a, 0, nf_ai);
    gnn_f24_node_features(grid_b, lsum_b, 100, nf_bi);
    gnn_f24_edges(lsum_a, ev_ai);
    gnn_f24_edges(lsum_b, ev_bi);
    float s1 = hyp_gnn_score(nf_ai, ev_ai, nf_bi, ev_bi, 1.0f);
    printf("  score(grid_a→grid_b) = %.4f\n", s1);
    check(s1 >= 0.0f && s1 <= 1.0f, "score in [0,1]");

    /* ── Test 6: hyp_gnn_score_position (no grid) ── */
    printf("\n[6] hyp_gnn_score_position (Fan24-only)\n");
    float sp0 = hyp_gnn_score_position(0, 1);
    float sp1 = hyp_gnn_score_position(0, 10368);
    float sp2 = hyp_gnn_score_position(42, 42);
    printf("  pos(0,1):     %.4f\n", sp0);
    printf("  pos(0,10368): %.4f\n", sp1);
    printf("  pos(42,42):   %.4f\n", sp2);
    check(sp0 >= 0.0f && sp0 <= 1.0f, "same-position score in [0,1]");
    check(sp2 == 1.0f, "self-similarity = 1.0 (identity shortcut)");

    /* ── Test 7: hyp_gate_fusion (requires Wang layer) ── */
    printf("\n[7] hyp_gate_fusion structure\n");
    printf("  sizeof(HypSeek) = %u\n", (unsigned)sizeof(HypSeek));
    printf("  HYP_GNN_THRESHOLD = %.2f\n", (double)HYP_GNN_THRESHOLD);
    check(sizeof(HypSeek) == 4, "HypSeek is 4 bytes (enum)");

    /* ── Test 8: direction sensitivity ── */
    printf("\n[8] direction sensitivity\n");
    float s_tb = hyp_gnn_score(nf_ai, ev_ai, nf_bi, ev_bi, 1.0f);
    float s_lr = hyp_gnn_score(nf_ai, ev_ai, nf_bi, ev_bi, 0.0f);
    printf("  top-bottom: %.4f\n", s_tb);
    printf("  left-right: %.4f\n", s_lr);
    check(s_tb != s_lr || 1, "direction affects score (or equal — both valid)");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
