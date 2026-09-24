/* experiments/ann-climate-2026-09-24/sift/probe_beam_descent.c — beam-width-N descent over the anchor kNN graph.
 *
 * IDEA (owner 2026-09-25, from HNSW layered-graph image): HNSW descends ONE
 * greedy path per layer; OUR arrowhead splits into N routes at every position.
 * This probe measures beam-width-N descent over the anchor kNN graph vs
 * greedy single-path (N=1 = the old single-arrow).
 *
 * CHOICE DOCUMENTED:
 * - Anchor set: 2560 USED buckets from build/sift1m_c/{F.bin,off.bin,mem.bin}
 *   (used = OFF[b+1]>OFF[b], same pattern as probe_route_only.c).
 *   build/sift1m_hier/ artifacts are NumPy .npy/.npz (C1/fine_cent/
 *   fine_members/perm/pca/meta.json) — not directly C-loadable — so the
 *   C-ready sift1m_c fallback is used (explicitly allowed by the task).
 * - Graph: exact brute-force kNN=8 over 25-dim centroids, maze_walk_cli.c
 *   pattern (lines ~107-139): directed top-8 by centroid L2, symmetrize,
 *   top-up short rows from the directed list so every row holds exactly 8.
 * - Start: nearest anchor overall (1x2560 entry scan, counted in visited).
 * - Stop: max 3 expansion steps OR beam set stops changing (convergence);
 *   per-arm counters report which rule fired most.
 * - Finish: exact 128-dim top-10 over UNION of final-beam buckets' members
 *   (OFF/MEM member lists, stamp-deduped) — structure-bounded rerank.
 * - All in PCA25 space, deterministic, no RNG. n=1000 queries.
 *
 * GATE: beam-N VIABLE iff N=16 recall >= 0.7523 (A-0.05) with mean
 * visited-centroids << 2560 AND ms/q < 2.55. Else report actuals.
 * Reference baseline A = 0.8023 @ 6974 scan @ ~2.6ms/q (cited, not rerun).
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_beam_descent.exe experiments/ann-climate-2026-09-24/sift/probe_beam_descent.c -lm
 * RUN (repo root I:\DWGLS-native-fs): build/probe_beam_descent.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define DIM 128
#define PDIM 25
#define NB 4096
#define NUSED 2560
#define KNN 8
#define NQ 1000
#define MAXSTEPS 3
#define NARMS 3

static float *FC, *PC, *PM, *BASE, *QUERY;
static int32_t *GT, *MEM;
static int64_t *OFF;
static int NBASE, NQRY, GTD;
static int AID[NUSED];
static int KNBR[NUSED][KNN];
static uint8_t ADJ[NUSED][NUSED];

static void *load_vecs(const char *path, int *n_out, int *d_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long fz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = (uint8_t *)malloc((size_t)fz);
    if (!raw || fread(raw, 1, (size_t)fz, f) != (size_t)fz) { fprintf(stderr, "read fail %s\n", path); exit(1); }
    fclose(f);
    int32_t d = *(int32_t *)raw;
    size_t stride = 4 + (size_t)d * 4;
    int n = (int)((size_t)fz / stride);
    void *out = malloc((size_t)n * d * 4);
    for (int i = 0; i < n; i++)
        memcpy((char *)out + (size_t)i * d * 4, raw + (size_t)i * stride + 4, (size_t)d * 4);
    free(raw);
    *n_out = n; *d_out = d;
    return out;
}
static float *load_bin(const char *path, size_t nfloat) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    float *p = (float *)malloc(nfloat * 4);
    if (!p || fread(p, 4, nfloat, f) != nfloat) { fprintf(stderr, "read fail %s\n", path); exit(1); }
    fclose(f);
    return p;
}
static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static double cdist25(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < PDIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}
static void project(const float *x, float *z) {
    for (int i = 0; i < PDIM; i++) {
        double s = 0;
        const float *row = PC + (size_t)i * DIM;
        for (int j = 0; j < DIM; j++) s += (double)row[j] * (x[j] - PM[j]);
        z[i] = (float)s;
    }
}
static void rerank10(const float *q, const int *ids, int ns, int *besi) {
    double best[10];
    for (int i = 0; i < 10; i++) { best[i] = 1e300; besi[i] = -1; }
    for (int i = 0; i < ns; i++) {
        const float *v = BASE + (size_t)ids[i] * DIM;
        double d = 0;
        for (int j = 0; j < DIM; j++) { double e = (double)q[j] - v[j]; d += e * e; }
        if (d < best[9]) {
            int p = 9;
            while (p > 0 && d < best[p - 1]) { best[p] = best[p - 1]; besi[p] = besi[p - 1]; p--; }
            best[p] = d; besi[p] = ids[i];
        }
    }
}
static double recall_at10(const int *ans, const int32_t *gt) {
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (ans[i] == gt[j]) { hit++; break; }
    return hit / 10.0;
}
/* insertion sort of small int array (beam-set compare) */
static void isort(int *a, int n) {
    for (int i = 1; i < n; i++) {
        int t = a[i], p = i;
        while (p > 0 && a[p - 1] > t) { a[p] = a[p - 1]; p--; }
        a[p] = t;
    }
}

int main(void) {
    double t0 = now_ms();
    int dtmp;
    BASE = (float *)load_vecs("build/sift1m/sift/sift_base.fvecs", &NBASE, &dtmp);
    QUERY = (float *)load_vecs("build/sift1m/sift/sift_query.fvecs", &NQRY, &dtmp);
    int ngt;
    GT = (int32_t *)load_vecs("build/sift1m/sift/sift_groundtruth.ivecs", &ngt, &GTD);
    PC = load_bin("build/sift1m_c/pca_comp.bin", (size_t)PDIM * DIM);
    PM = load_bin("build/sift1m_c/pca_mean.bin", DIM);
    FC = load_bin("build/sift1m_c/F.bin", (size_t)NB * PDIM);
    { FILE *f = fopen("build/sift1m_c/off.bin", "rb");
      if (!f) { fprintf(stderr, "missing off.bin\n"); return 1; }
      OFF = (int64_t *)malloc(((size_t)NB + 1) * 8);
      if (fread(OFF, 8, NB + 1, f) != (size_t)NB + 1) { fprintf(stderr, "off read\n"); return 1; }
      fclose(f); }
    MEM = (int32_t *)load_bin("build/sift1m_c/mem.bin", 1000000);
    int nu = 0;
    for (int b = 0; b < NB && nu < NUSED; b++)
        if (OFF[b + 1] > OFF[b]) AID[nu++] = b;
    if (nu != NUSED) { fprintf(stderr, "used count %d != %d\n", nu, NUSED); return 1; }
    printf("load: base %d query %d gt-dim %d used %d in %.1fs\n",
           NBASE, NQRY, GTD, nu, (now_ms() - t0) / 1000.0);
    printf("choice: sift1m_c 2560 used-bucket anchors (hier .npy not C-loadable); kNN=8 exact brute-force graph; start=nearest-anchor-overall; stop=3-steps-or-converged\n");

    /* 1. kNN=8 graph over used anchors (maze_walk_cli pattern) */
    t0 = now_ms();
    static int DIR[NUSED][KNN]; /* directed top-8, for top-up after symmetrize */
    for (int i = 0; i < NUSED; i++) {
        double bd[KNN];
        int bi[KNN];
        for (int k = 0; k < KNN; k++) { bd[k] = 1e300; bi[k] = -1; }
        const float *ci = FC + (size_t)AID[i] * PDIM;
        for (int j = 0; j < NUSED; j++) {
            if (j == i) continue;
            double d = cdist25(ci, FC + (size_t)AID[j] * PDIM);
            if (d < bd[KNN - 1]) {
                int p = KNN - 1;
                while (p > 0 && d < bd[p - 1]) { bd[p] = bd[p - 1]; bi[p] = bi[p - 1]; p--; }
                bd[p] = d; bi[p] = j;
            }
        }
        for (int k = 0; k < KNN; k++) { DIR[i][k] = bi[k]; ADJ[i][bi[k]] = 1; }
    }
    for (int i = 0; i < NUSED; i++)
        for (int j = 0; j < NUSED; j++)
            if (ADJ[i][j]) ADJ[j][i] = 1;
    int maxdeg = 0;
    for (int i = 0; i < NUSED; i++) {
        int c = 0;
        for (int j = 0; j < NUSED && c < KNN; j++)
            if (ADJ[i][j]) KNBR[i][c++] = j;
        for (int k = 0; k < KNN && c < KNN; k++) { /* top-up short rows */
            int j = DIR[i][k], dup = 0;
            for (int t = 0; t < c; t++) if (KNBR[i][t] == j) { dup = 1; break; }
            if (!dup) KNBR[i][c++] = j;
        }
        if (c > maxdeg) maxdeg = c;
    }
    printf("graph: kNN=%d maxdeg-used=%d built in %.1fs\n", KNN, maxdeg, (now_ms() - t0) / 1000.0);

    static const int WIDTHS[NARMS] = { 1, 4, 16 };
    int *scan = (int *)malloc((size_t)NBASE * 4);
    int32_t *stamp = (int32_t *)calloc(NBASE, 4);
    int32_t cur = 1;
    double *edist = (double *)malloc((size_t)NUSED * 8);
    uint8_t *added = (uint8_t *)malloc(NUSED);
    float *zq = (float *)malloc(PDIM * 4);
    int besi[10];

    double arm_rec[NARMS], arm_ms[NARMS], arm_vis[NARMS], arm_scan[NARMS];
    long long arm_conv[NARMS], arm_maxs[NARMS];

    for (int a = 0; a < NARMS; a++) {
        int W = WIDTHS[a];
        double rec = 0;
        long long nvis = 0, nscan = 0, nconv = 0, nmaxs = 0;
        double tq = now_ms();
        for (int qi = 0; qi < NQ; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            project(q, zq);
            /* 2a. entry: nearest anchor overall (1x2560 scan, counted) */
            int s0 = 0;
            for (int i = 0; i < NUSED; i++) {
                double d = cdist25(zq, FC + (size_t)AID[i] * PDIM);
                edist[i] = d;
                if (d < edist[s0]) s0 = i;
            }
            long long vis = NUSED;
            /* 2b. beam descent */
            int beam[16], nbeam = 1;
            double beamd[16];
            beam[0] = s0; beamd[0] = edist[s0];
            memset(added, 0, NUSED);
            added[s0] = 1;
            int fired_conv = 0;
            for (int st = 0; st < MAXSTEPS; st++) {
                int cand[16 * 8], nc = 0;
                double candd[16 * 8];
                for (int b = 0; b < nbeam; b++)
                    for (int k = 0; k < KNN; k++) {
                        int j = KNBR[beam[b]][k];
                        if (!added[j]) {
                            added[j] = 1;
                            cand[nc] = j; candd[nc] = edist[j]; nc++;
                        }
                    }
                vis += nc;
                /* pool = beam + candidates, keep top-W */
                int pool[16 + 16 * 8], pn = 0;
                double poold[16 + 16 * 8];
                for (int b = 0; b < nbeam; b++) { pool[pn] = beam[b]; poold[pn] = beamd[b]; pn++; }
                for (int c = 0; c < nc; c++) { pool[pn] = cand[c]; poold[pn] = candd[c]; pn++; }
                int nnew = pn < W ? pn : W;
                int nbeam2[16];
                double beamd2[16];
                for (int c = 0; c < nnew; c++) { /* select c-th smallest */
                    int bi2 = c;
                    for (int i = c + 1; i < pn; i++)
                        if (poold[i] < poold[bi2]) bi2 = i;
                    double td = poold[c]; poold[c] = poold[bi2]; poold[bi2] = td;
                    int ti = pool[c]; pool[c] = pool[bi2]; pool[bi2] = ti;
                    nbeam2[c] = pool[c]; beamd2[c] = poold[c];
                }
                /* converged iff beam set unchanged */
                int old[16], nw[16];
                for (int b = 0; b < nbeam; b++) old[b] = beam[b];
                for (int b = 0; b < nnew; b++) nw[b] = nbeam2[b];
                isort(old, nbeam); isort(nw, nnew);
                int same = (nbeam == nnew);
                for (int b = 0; same && b < nbeam; b++) if (old[b] != nw[b]) same = 0;
                nbeam = nnew;
                for (int b = 0; b < nbeam; b++) { beam[b] = nbeam2[b]; beamd[b] = beamd2[b]; }
                if (same) { fired_conv = 1; break; }
            }
            if (fired_conv) nconv++; else nmaxs++;
            /* 3. finish: exact 128-dim top-10 over UNION of final-beam buckets */
            cur++;
            int ns = 0;
            for (int b = 0; b < nbeam; b++) {
                int bk = AID[beam[b]];
                for (int64_t k = OFF[bk]; k < OFF[bk + 1]; k++) {
                    int id = MEM[k];
                    if (stamp[id] != cur) { stamp[id] = cur; scan[ns++] = id; }
                }
            }
            rerank10(q, scan, ns, besi);
            nscan += ns;
            nvis += vis; /* visited-centroids incl. entry scan */
            rec += recall_at10(besi, GT + (size_t)qi * GTD);
        }
        arm_ms[a] = (now_ms() - tq) / NQ;
        arm_rec[a] = rec / NQ;
        arm_vis[a] = (double)nvis / NQ;
        arm_scan[a] = (double)nscan / NQ;
        arm_conv[a] = nconv; arm_maxs[a] = nmaxs;
    }

    printf("reference A (cited, not rerun): recall=0.8023 scan=6974 (~0.70%%) ~2.6ms/q\n");
    for (int a = 0; a < NARMS; a++) {
        const char *rule = arm_conv[a] >= arm_maxs[a] ? "converged" : "max-3-steps";
        printf("N=%-2d (beam%s): recall=%.4f mean-visited=%.0f mean-union-scan=%.0f (%.3f%%) %.2fms/q stop[conv=%d max=%d] mostly-%s\n",
               WIDTHS[a], WIDTHS[a] == 1 ? "/greedy" : "",
               arm_rec[a], arm_vis[a], arm_scan[a], arm_scan[a] / NBASE * 100.0, arm_ms[a],
               (int)arm_conv[a], (int)arm_maxs[a], rule);
    }
    /* neighbor dists reuse the entry-scan table (no extra centroid computes);
       visited = 2560 entry + distinct anchors added during beam expansion. */
    int ok_rec = arm_rec[2] >= 0.7523;
    int ok_vis = arm_vis[2] < NUSED / 2; /* "<<" 2560: needs < 1280; entry scan alone is 2560 */
    int ok_ms = arm_ms[2] < 2.55;
    printf("GATE: beam-N %s (N=16 recall>=0.7523 %s [%.4f], visited<<2560 %s [%.0f], ms/q<2.55 %s [%.2f])\n",
           (ok_rec && ok_vis && ok_ms) ? "VIABLE" : "NOT VIABLE",
           ok_rec ? "yes" : "no", arm_rec[2],
           ok_vis ? "yes" : "no", arm_vis[2],
           ok_ms ? "yes" : "no", arm_ms[2]);
    return 0;
}
