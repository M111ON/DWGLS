/*
 * ann_robust.c — is "iters=2 is as good as iters=25" real, or one lucky seed?
 * ══════════════════════════════════════════════════════════════════════════
 * ann_knobs.c showed recall flat across iters 2..25 (0.9265..0.9310) on ONE
 * query set (seed 12345). A 0.005 spread could be noise, or the whole claim
 * could be a lucky query draw. This run repeats the comparison across 5
 * independent query seeds (200 queries each) and reports mean +/- spread.
 *
 * Also reports the recall/scan frontier per seed so the Pareto shape is not
 * an artifact of one draw.
 *
 * Build: gcc -O2 -std=c11 -I I:/DWGLS-native-fs/core -o build/ann_robust ann_robust.c -lm
 * Run:   ./build/ann_robust X.f32 61241 384
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "anchor_route.h"

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }
#define TOPK 10
#define NQ   200
#define NSEED 5
#define K     8

static int *g_lab;
static const float *g_X;
static int g_n, g_dim;

/* recall@10 + scan fraction for a given (C, topb) over a query list */
static void eval_cfg(const float *C, int topb, const int *qidx, int nq,
                 const int (*gt)[TOPK], double *rec_out, double *scan_out) {
    double sr = 0, ss = 0;
    for (int q = 0; q < nq; q++) {
        const float *qv = g_X + (size_t)qidx[q]*g_dim;
        int rb[ANCHR_MAXK];
        int ng = anch_route(qv, C, K, g_dim, topb, rb);
        int scanned = 0, hit = 0;
        for (int t = 0; t < ng; t++)
            for (int i = 0; i < g_n; i++) {
                if (g_lab[i] != rb[t]) continue;
                scanned++;
                const float *xi = g_X + (size_t)i*g_dim;
                double d = 0;
                for (int j = 0; j < g_dim; j++) { double e = (double)qv[j]-xi[j]; d += e*e; }
                for (int k = 0; k < TOPK; k++) if (gt[q][k] == i) { hit++; break; }
            }
        sr += (double)hit/TOPK;
        ss += (double)scanned/g_n;
    }
    *rec_out = sr/nq;
    *scan_out = ss/nq;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "X.f32";
    int n   = (argc > 2) ? atoi(argv[2]) : 61241;
    int dim = (argc > 3) ? atoi(argv[3]) : 384;
    g_n = n; g_dim = dim;

    printf("=== is iters=2 real? %d query seeds x %d queries ===\n\n", NSEED, NQ);

    float *X = (float *)malloc((size_t)n*dim*sizeof(float));
    FILE *f = fopen(path, "rb");
    if (!X || !f) { printf("load fail\n"); return 1; }
    if (fread(X, sizeof(float), (size_t)n*dim, f) != (size_t)n*dim) { printf("short read\n"); return 1; }
    fclose(f);
    g_X = X;

    g_lab = (int *)malloc((size_t)n*sizeof(int));
    float *C = (float *)malloc((size_t)ANCHR_MAXK*dim*sizeof(float));
    double *sums = (double *)calloc((size_t)K*dim, sizeof(double));
    int *cnt = (int *)calloc((size_t)K, sizeof(int));

    /* ── train the two candidates once each ──────────────────────────── */
    int iters_set[] = {2, 25};
    float *Cset[2];
    double train_s[2];
    for (int a = 0; a < 2; a++) {
        int it = iters_set[a];
        Cset[a] = (float *)malloc((size_t)K*dim*sizeof(float));
        for (int k = 0; k < K; k++)
            memcpy(Cset[a] + (size_t)k*dim, X + (size_t)(k*n/K)*dim, (size_t)dim*sizeof(float));
        double t0 = now_s();
        for (int e = 0; e < it; e++) {
            for (int i = 0; i < n; i++) g_lab[i] = anch_assign(X + (size_t)i*dim, Cset[a], K, dim);
            memset(sums, 0, (size_t)K*dim*sizeof(double));
            memset(cnt, 0, (size_t)K*sizeof(int));
            for (int i = 0; i < n; i++) {
                int k = g_lab[i]; if (k < 0) continue;
                cnt[k]++;
                for (int j = 0; j < dim; j++) sums[(size_t)k*dim+j] += X[(size_t)i*dim+j];
            }
            for (int k = 0; k < K; k++) {
                if (!cnt[k]) continue;
                for (int j = 0; j < dim; j++)
                    Cset[a][(size_t)k*dim+j] = (float)(sums[(size_t)k*dim+j]/cnt[k]);
            }
        }
        train_s[a] = now_s() - t0;
    }
    printf("train: iters=2 %.3f s   iters=25 %.3f s  (%.1fx)\n\n",
           train_s[0], train_s[1], train_s[1]/train_s[0]);

    /* ── per-seed loop ───────────────────────────────────────────────── */
    FILE *csv = fopen("robust.csv", "w");
    fprintf(csv, "seed,iters,topb,recall10,scan_frac\n");
    double agg[2][3][NSEED]; /* [iters][topb idx][seed] */
    int topbs[] = {2, 4, 6};

    for (int s = 0; s < NSEED; s++) {
        int seed = 1000 + s*777;
        int *qidx = (int *)malloc((size_t)NQ*sizeof(int));
        srand(seed);
        for (int q = 0; q < NQ; q++) qidx[q] = rand() % n;

        int (*gt)[TOPK] = malloc((size_t)NQ*TOPK*sizeof(int));
        for (int q = 0; q < NQ; q++) {
            const float *qv = X + (size_t)qidx[q]*dim;
            double best[TOPK]; int bid[TOPK];
            for (int k = 0; k < TOPK; k++) { best[k] = 1e300; bid[k] = -1; }
            for (int i = 0; i < n; i++) {
                if (i == qidx[q]) continue;
                const float *xi = X + (size_t)i*dim;
                double d = 0;
                for (int j = 0; j < dim; j++) { double e = (double)qv[j]-xi[j]; d += e*e; }
                if (d >= best[TOPK-1]) continue;
                int k = TOPK-1;
                while (k > 0 && best[k-1] > d) { best[k]=best[k-1]; bid[k]=bid[k-1]; k--; }
                best[k]=d; bid[k]=i;
            }
            for (int k = 0; k < TOPK; k++) gt[q][k] = bid[k];
        }

        printf("seed %d\n", seed);
        printf("  %-7s %-5s %-9s %-9s\n", "iters", "topb", "recall10", "scan%");
        for (int a = 0; a < 2; a++) {
            /* relabel with this candidate's centroids */
            for (int i = 0; i < n; i++) g_lab[i] = anch_assign(X + (size_t)i*dim, Cset[a], K, dim);
            for (int ti = 0; ti < 3; ti++) {
                double r, sc;
                eval_cfg(Cset[a], topbs[ti], qidx, NQ, (const int (*)[TOPK])gt, &r, &sc);
                agg[a][ti][s] = r;
                printf("  %-7d %-5d %-9.4f %-9.3f\n", iters_set[a], topbs[ti], r, 100.0*sc);
                fprintf(csv, "%d,%d,%d,%.4f,%.4f\n", seed, iters_set[a], topbs[ti], r, sc);
                fflush(csv);
            }
        }
        free(qidx); free(gt);
    }
    fclose(csv);

    /* ── aggregate ───────────────────────────────────────────────────── */
    printf("\n=== aggregate over %d seeds ===\n", NSEED);
    printf("%-7s %-5s %-9s %-9s %-9s %-8s\n",
           "iters", "topb", "mean", "min", "max", "spread");
    for (int a = 0; a < 2; a++) for (int ti = 0; ti < 3; ti++) {
        double mn = 2, mx = -1, sum = 0;
        for (int s = 0; s < NSEED; s++) {
            double v = agg[a][ti][s];
            sum += v; if (v < mn) mn = v; if (v > mx) mx = v;
        }
        printf("%-7d %-5d %-9.4f %-9.4f %-9.4f %-8.4f\n",
               iters_set[a], topbs[ti], sum/NSEED, mn, mx, mx-mn);
    }

    for (int a = 0; a < 2; a++) free(Cset[a]);
    free(X); free(g_lab); free(C); free(sums); free(cnt);
    printf("\nDONE\n");
    return 0;
}
