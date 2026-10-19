/*
 * ann_knobs.c — the two open knobs from HANDOFF-2026-10-14d §3.2 + §3.3
 * ══════════════════════════════════════════════════════════════════════════
 * §3.2  regrow 9.9 s is mostly kmeans 25 iters -> what is the LOWEST iteration
 *       count that keeps recall?  (sweep iters, K=8, topb=2)
 * §3.3  raise recall 0.94 -> higher by widening the route (K and/or topb).
 *       (sweep K in {8,16,32,64} x topb in {1,2,4})
 *
 * HONEST ORACLE: exact top-10 over the whole corpus EXCLUDING the query itself
 * (the earlier bench left self in, which inflates recall@10 by one slot).
 * Queries are 200 random rows of the same corpus (self-query protocol) — the
 * same protocol the server serves, so the numbers are comparable, but the
 * oracle no longer gives a free hit.
 *
 * Everything measured on the real 61,241 x 384 e5-small corpus.
 *
 * Build: gcc -O2 -std=c11 -I I:/DWGLS-native-fs/core -o build/ann_knobs ann_knobs.c -lm
 * Run:   ./build/ann_knobs X.f32 61241 384
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

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "X.f32";
    int n   = (argc > 2) ? atoi(argv[2]) : 61241;
    int dim = (argc > 3) ? atoi(argv[3]) : 384;

    printf("=== ANN knobs: kmeans iters (§3.2) and route width (§3.3) ===\n");
    printf("corpus %d x %d, oracle = exact top-%d excluding self, %d queries\n\n",
           n, dim, TOPK, NQ);

    float *X = (float *)malloc((size_t)n * dim * sizeof(float));
    if (!X) { printf("OOM X\n"); return 1; }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    size_t got = fread(X, sizeof(float), (size_t)n * dim, f);
    fclose(f);
    if (got != (size_t)n * dim) { printf("short read %zu\n", got); return 1; }

    /* ── pick queries (deterministic) ─────────────────────────────────── */
    static int qidx[NQ];
    srand(12345);
    for (int q = 0; q < NQ; q++) qidx[q] = rand() % n;

    /* ── oracle: exact top-10 excluding self, once ────────────────────── */
    double t0 = now_s();
    static int gt[NQ][TOPK];
    for (int q = 0; q < NQ; q++) {
        const float *qv = X + (size_t)qidx[q] * dim;
        double best[TOPK]; int bid[TOPK];
        for (int k = 0; k < TOPK; k++) { best[k] = 1e300; bid[k] = -1; }
        for (int i = 0; i < n; i++) {
            if (i == qidx[q]) continue;              /* exclude self */
            const float *xi = X + (size_t)i * dim;
            double d = 0;
            for (int j = 0; j < dim; j++) { double e = (double)qv[j] - xi[j]; d += e * e; }
            if (d >= best[TOPK-1]) continue;
            int k = TOPK - 1;
            while (k > 0 && best[k-1] > d) { best[k] = best[k-1]; bid[k] = bid[k-1]; k--; }
            best[k] = d; bid[k] = i;
        }
        for (int k = 0; k < TOPK; k++) gt[q][k] = bid[k];
    }
    printf("oracle built in %.1f s\n\n", now_s() - t0);

    int *lab = (int *)malloc((size_t)n * sizeof(int));
    if (!lab) { printf("OOM lab\n"); return 1; }

    /* ── sweep ────────────────────────────────────────────────────────── */
    printf("A) ITERS SWEEP  (K=8, topb=2) — how low can regrow go?\n");
    printf("%-6s %-9s %-9s %-9s %-8s %-8s %-7s\n",
           "iters", "train_s", "assign_s", "regrow_s", "R@10", "scan%", "q_ms");
    FILE *csv = fopen("knobs_iters.csv", "w");
    fprintf(csv, "iters,train_s,assign_s,regrow_s,recall10,scan_frac,q_ms\n");

    int iters_list[] = {2, 3, 4, 5, 8, 12, 18, 25};
    int nit = (int)(sizeof(iters_list)/sizeof(iters_list[0]));
    int   K = 8, topb = 2;
    float *C = (float *)malloc((size_t)ANCHR_MAXK * dim * sizeof(float));

    for (int a = 0; a < nit; a++) {
        int it = iters_list[a];
        /* anch_train has a fixed ANCHR_ITERS cap, so we re-implement the loop
         * here with `it` iterations — same code path, same init. */
        for (int k = 0; k < K; k++)
            memcpy(C + (size_t)k * dim, X + (size_t)(k * n / K) * dim,
                   (size_t)dim * sizeof(float));
        double *sums = (double *)calloc((size_t)K * dim, sizeof(double));
        int *cnt = (int *)calloc((size_t)K, sizeof(int));

        double tt = now_s();
        for (int e = 0; e < it; e++) {
            for (int i = 0; i < n; i++) lab[i] = anch_assign(X + (size_t)i*dim, C, K, dim);
            memset(sums, 0, (size_t)K * dim * sizeof(double));
            memset(cnt, 0, (size_t)K * sizeof(int));
            for (int i = 0; i < n; i++) {
                int k = lab[i]; if (k < 0) continue;
                cnt[k]++;
                for (int j = 0; j < dim; j++) sums[(size_t)k*dim + j] += X[(size_t)i*dim + j];
            }
            for (int k = 0; k < K; k++) {
                if (!cnt[k]) continue;
                for (int j = 0; j < dim; j++)
                    C[(size_t)k*dim + j] = (float)(sums[(size_t)k*dim + j] / cnt[k]);
            }
        }
        double t_train = now_s() - tt;

        double ta = now_s();
        for (int i = 0; i < n; i++) lab[i] = anch_assign(X + (size_t)i*dim, C, K, dim);
        double t_assign = now_s() - ta;

        /* evaluate */
        double sumrec = 0, sumscan = 0, sumq = 0;
        for (int q = 0; q < NQ; q++) {
            const float *qv = X + (size_t)qidx[q] * dim;
            int rb[ANCHR_MAXK];
            double tq = now_s();
            int ngot = anch_route(qv, C, K, dim, topb, rb);
            int scanned = 0, hit = 0;
            for (int t = 0; t < ngot; t++) {
                for (int i = 0; i < n; i++) {
                    if (lab[i] != rb[t]) continue;
                    scanned++;
                    const float *xi = X + (size_t)i * dim;
                    double d = 0;
                    for (int j = 0; j < dim; j++) { double e = (double)qv[j] - xi[j]; d += e*e; }
                    for (int k = 0; k < TOPK; k++) if (gt[q][k] == i) { hit++; break; }
                }
            }
            sumq += now_s() - tq;
            sumrec += (double)hit / TOPK;
            sumscan += (double)scanned / n;
        }
        double regrow = t_train + t_assign + 0.008 + 0.001; /* + perm + save */
        printf("%-6d %-9.3f %-9.3f %-9.3f %-8.4f %-8.3f %-7.2f\n",
               it, t_train, t_assign, regrow, sumrec/NQ, 100.0*sumscan/NQ,
               1000.0*sumq/NQ);
        fprintf(csv, "%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.2f\n",
                it, t_train, t_assign, regrow, sumrec/NQ, sumscan/NQ, 1000.0*sumq/NQ);
        fflush(csv);
        free(sums); free(cnt);
    }
    fclose(csv);

    /* ── B) route width ────────────────────────────────────────────────── */
    printf("\nB) ROUTE WIDTH  (iters=25 fixed) — buy recall with K / topb\n");
    printf("%-5s %-5s %-9s %-9s %-8s %-8s %-7s\n",
           "K", "topb", "train_s", "assign_s", "R@10", "scan%", "q_ms");
    csv = fopen("knobs_width.csv", "w");
    fprintf(csv, "K,topb,train_s,assign_s,recall10,scan_frac,q_ms\n");

    int Ks[]    = {8, 16, 32, 64};
    int topbs[] = {1, 2, 4};
    int nK = 4, nT = 3;

    for (int ki = 0; ki < nK; ki++) {
        int KK = Ks[ki];
        double tt = now_s();
        if (anch_train(X, n, dim, KK, C, lab) != 0) { printf("train fail K=%d\n", KK); return 1; }
        double t_train = now_s() - tt;
        double ta = now_s();
        for (int i = 0; i < n; i++) lab[i] = anch_assign(X + (size_t)i*dim, C, KK, dim);
        double t_assign = now_s() - ta;

        for (int ti = 0; ti < nT; ti++) {
            int tb = topbs[ti];
            if (tb > KK) continue;
            double sumrec = 0, sumscan = 0, sumq = 0;
            for (int q = 0; q < NQ; q++) {
                const float *qv = X + (size_t)qidx[q] * dim;
                int rb[ANCHR_MAXK];
                double tq = now_s();
                int ngot = anch_route(qv, C, KK, dim, tb, rb);
                int scanned = 0, hit = 0;
                for (int t = 0; t < ngot; t++) {
                    for (int i = 0; i < n; i++) {
                        if (lab[i] != rb[t]) continue;
                        scanned++;
                        const float *xi = X + (size_t)i * dim;
                        double d = 0;
                        for (int j = 0; j < dim; j++) { double e = (double)qv[j] - xi[j]; d += e*e; }
                        for (int k = 0; k < TOPK; k++) if (gt[q][k] == i) { hit++; break; }
                    }
                }
                sumq += now_s() - tq;
                sumrec += (double)hit / TOPK;
                sumscan += (double)scanned / n;
            }
            printf("%-5d %-5d %-9.3f %-9.3f %-8.4f %-8.3f %-7.2f\n",
                   KK, tb, t_train, t_assign, sumrec/NQ, 100.0*sumscan/NQ,
                   1000.0*sumq/NQ);
            fprintf(csv, "%d,%d,%.3f,%.3f,%.4f,%.4f,%.2f\n",
                    KK, tb, t_train, t_assign, sumrec/NQ, sumscan/NQ, 1000.0*sumq/NQ);
            fflush(csv);
        }
    }
    fclose(csv);

    free(X); free(lab); free(C);
    printf("\nDONE\n");
    return 0;
}
