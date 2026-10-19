/*
 * ann_frontier.c — fill the Pareto frontier at 2-iteration regrow
 * ════════════════════════════════════════════════════════════════════
 * ann_knobs.c proved recall is insensitive to kmeans iterations (flat 2..25)
 * but strongly sensitive to topb. This run pins iters=2 (regrow ~0.9 s) and
 * sweeps the region we did not cover: K in {8,16}, topb in {2,4,6,8}.
 *
 * Same honest oracle: exact top-10 over the corpus EXCLUDING self.
 * Same 200 deterministic queries (seed 12345) so numbers are comparable to
 * knobs_iters.csv / knobs_width.csv.
 *
 * Build: gcc -O2 -std=c11 -I I:/DWGLS-native-fs/core -o build/ann_frontier ann_frontier.c -lm
 * Run:   ./build/ann_frontier X.f32 61241 384
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "anchor_route.h"

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }
#define TOPK 10
#define NQ   200
#define ITERS 2

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "X.f32";
    int n   = (argc > 2) ? atoi(argv[2]) : 61241;
    int dim = (argc > 3) ? atoi(argv[3]) : 384;

    printf("=== frontier at iters=%d (regrow ~0.9 s) ===\n", ITERS);
    printf("corpus %d x %d, oracle exact top-%d excl self, %d queries\n\n", n, dim, TOPK, NQ);

    float *X = (float *)malloc((size_t)n * dim * sizeof(float));
    FILE *f = fopen(path, "rb");
    if (!X || !f) { printf("load fail\n"); return 1; }
    if (fread(X, sizeof(float), (size_t)n * dim, f) != (size_t)n * dim) { printf("short read\n"); return 1; }
    fclose(f);

    static int qidx[NQ];
    srand(12345);
    for (int q = 0; q < NQ; q++) qidx[q] = rand() % n;

    static int gt[NQ][TOPK];
    for (int q = 0; q < NQ; q++) {
        const float *qv = X + (size_t)qidx[q] * dim;
        double best[TOPK]; int bid[TOPK];
        for (int k = 0; k < TOPK; k++) { best[k] = 1e300; bid[k] = -1; }
        for (int i = 0; i < n; i++) {
            if (i == qidx[q]) continue;
            const float *xi = X + (size_t)i * dim;
            double d = 0;
            for (int j = 0; j < dim; j++) { double e = (double)qv[j] - xi[j]; d += e*e; }
            if (d >= best[TOPK-1]) continue;
            int k = TOPK - 1;
            while (k > 0 && best[k-1] > d) { best[k]=best[k-1]; bid[k]=bid[k-1]; k--; }
            best[k] = d; bid[k] = i;
        }
        for (int k = 0; k < TOPK; k++) gt[q][k] = bid[k];
    }

    int *lab = (int *)malloc((size_t)n * sizeof(int));
    float *C = (float *)malloc((size_t)ANCHR_MAXK * dim * sizeof(float));
    double *sums = (double *)calloc((size_t)ANCHR_MAXK * dim, sizeof(double));
    int *cnt = (int *)calloc((size_t)ANCHR_MAXK, sizeof(int));

    printf("%-4s %-5s %-9s %-9s %-8s %-8s %-7s\n",
           "K", "topb", "regrow_s", "bucketsz", "R@10", "scan%", "q_ms");
    FILE *csv = fopen("knobs_frontier.csv", "w");
    fprintf(csv, "K,topb,regrow_s,bucket_sz,recall10,scan_frac,q_ms\n");

    int Ks[]    = {8, 16};
    int topbs[] = {2, 4, 6, 8};

    for (int ki = 0; ki < 2; ki++) {
        int K = Ks[ki];
        /* train: ITERS passes, deterministic even-spaced init (same as anch_train) */
        for (int k = 0; k < K; k++)
            memcpy(C + (size_t)k*dim, X + (size_t)(k*n/K)*dim, (size_t)dim*sizeof(float));
        double t0 = now_s();
        for (int e = 0; e < ITERS; e++) {
            for (int i = 0; i < n; i++) lab[i] = anch_assign(X + (size_t)i*dim, C, K, dim);
            memset(sums, 0, (size_t)K*dim*sizeof(double));
            memset(cnt, 0, (size_t)K*sizeof(int));
            for (int i = 0; i < n; i++) {
                int k = lab[i]; if (k < 0) continue;
                cnt[k]++;
                for (int j = 0; j < dim; j++) sums[(size_t)k*dim+j] += X[(size_t)i*dim+j];
            }
            for (int k = 0; k < K; k++) {
                if (!cnt[k]) continue;
                for (int j = 0; j < dim; j++) C[(size_t)k*dim+j] = (float)(sums[(size_t)k*dim+j]/cnt[k]);
            }
        }
        double t_train = now_s() - t0;
        double ta = now_s();
        for (int i = 0; i < n; i++) lab[i] = anch_assign(X + (size_t)i*dim, C, K, dim);
        double regrow = t_train + (now_s() - ta) + 0.009;

        for (int ti = 0; ti < 4; ti++) {
            int tb = topbs[ti];
            double sumrec = 0, sumscan = 0, sumq = 0;
            for (int q = 0; q < NQ; q++) {
                const float *qv = X + (size_t)qidx[q]*dim;
                int rb[ANCHR_MAXK];
                double tq = now_s();
                int ng = anch_route(qv, C, K, dim, tb, rb);
                int scanned = 0, hit = 0;
                for (int t = 0; t < ng; t++) {
                    for (int i = 0; i < n; i++) {
                        if (lab[i] != rb[t]) continue;
                        scanned++;
                        const float *xi = X + (size_t)i*dim;
                        double d = 0;
                        for (int j = 0; j < dim; j++) { double e = (double)qv[j]-xi[j]; d += e*e; }
                        for (int k = 0; k < TOPK; k++) if (gt[q][k] == i) { hit++; break; }
                    }
                }
                sumq += now_s() - tq;
                sumrec += (double)hit/TOPK;
                sumscan += (double)scanned/n;
            }
            printf("%-4d %-5d %-9.3f %-9d %-8.4f %-8.3f %-7.2f\n",
                   K, tb, regrow, n/K, sumrec/NQ, 100.0*sumscan/NQ, 1000.0*sumq/NQ);
            fprintf(csv, "%d,%d,%.3f,%d,%.4f,%.4f,%.2f\n",
                    K, tb, regrow, n/K, sumrec/NQ, sumscan/NQ, 1000.0*sumq/NQ);
            fflush(csv);
        }
    }
    fclose(csv);
    free(X); free(lab); free(C); free(sums); free(cnt);
    printf("\nDONE\n");
    return 0;
}
