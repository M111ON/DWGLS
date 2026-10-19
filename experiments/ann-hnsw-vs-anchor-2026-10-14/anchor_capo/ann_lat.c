/*
 * ann_lat.c — HONEST latency for the frontier (QPC, not clock())
 * ═══════════════════════════════════════════════════════════════════════
 * Why this file exists: ann_knobs.c / ann_frontier.c timed per-query with
 * clock()/CLOCKS_PER_SEC, which on MinGW is 1 ms granularity -> the q_ms
 * column was quantized noise (0.85 ms "measured" vs 16 ms real). Recall and
 * scan% from those runs are exact integer counts and still stand.
 *
 * Here every query is timed with QueryPerformanceCounter (sub-microsecond).
 * Config: K=8, iters=2 (the regrow winner), topb in {2,4,6}, plus a full
 * brute-force scan as the reference the index must beat.
 *
 * Cosine scoring, same as the server (lz_mem_search): dot/(|q||x|).
 *
 * Build: gcc -O2 -std=c11 -I I:/DWGLS-native-fs/core -o build/ann_lat ann_lat.c -lm
 * Run:   ./build/ann_lat X.f32 61241 384
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "anchor_route.h"

#define TOPK 10
#define NQ   200
#define ITERS 2
#define K     8

static double g_freq = 0.0;
static double now_us(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / g_freq;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* scan the members of the routed buckets, cosine score, keep top-k */
static int serve_one(const float *X, int n, int dim, const float *C, int topb,
                     const int *lab, const float *qv, int k, int *out_k) {
    int rb[ANCHR_MAXK];
    int got = anch_route(qv, C, K, dim, topb, rb);
    double qn = 0; for (int j = 0; j < dim; j++) qn += (double)qv[j]*qv[j];
    qn = qn > 0 ? sqrt(qn) : 1;
    double bs[32]; int bi[32];
    for (int i = 0; i < k; i++) { bs[i] = -2; bi[i] = -1; }
    int scored = 0;
    for (int t = 0; t < got; t++) for (int i = 0; i < n; i++) {
        if (lab[i] != rb[t]) continue;
        const float *xi = X + (size_t)i*dim;
        double dot = 0, xn = 0;
        for (int j = 0; j < dim; j++) { dot += (double)qv[j]*xi[j]; xn += (double)xi[j]*xi[j]; }
        xn = xn > 0 ? sqrt(xn) : 1;
        double sc = dot/(qn*xn);
        scored++;
        for (int m = 0; m < k; m++) if (sc > bs[m]) {
            for (int p = k-1; p > m; p--) { bs[p]=bs[p-1]; bi[p]=bi[p-1]; }
            bs[m]=sc; bi[m]=i; break;
        }
    }
    for (int i = 0; i < k; i++) out_k[i] = bi[i];
    return scored;
}

/* brute force: exact top-k cosine over the whole corpus */
static void brute_one(const float *X, int n, int dim, const float *qv, int k, int *out_k) {
    double qn = 0; for (int j = 0; j < dim; j++) qn += (double)qv[j]*qv[j];
    qn = qn > 0 ? sqrt(qn) : 1;
    double bs[32]; int bi[32];
    for (int i = 0; i < k; i++) { bs[i] = -2; bi[i] = -1; }
    for (int i = 0; i < n; i++) {
        const float *xi = X + (size_t)i*dim;
        double dot = 0, xn = 0;
        for (int j = 0; j < dim; j++) { dot += (double)qv[j]*xi[j]; xn += (double)xi[j]*xi[j]; }
        xn = xn > 0 ? sqrt(xn) : 1;
        double sc = dot/(qn*xn);
        for (int m = 0; m < k; m++) if (sc > bs[m]) {
            for (int p = k-1; p > m; p--) { bs[p]=bs[p-1]; bi[p]=bi[p-1]; }
            bs[m]=sc; bi[m]=i; break;
        }
    }
    for (int i = 0; i < k; i++) out_k[i] = bi[i];
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "X.f32";
    int n   = (argc > 2) ? atoi(argv[2]) : 61241;
    int dim = (argc > 3) ? atoi(argv[3]) : 384;

    LARGE_INTEGER fr; QueryPerformanceFrequency(&fr); g_freq = (double)fr.QuadPart;

    printf("=== latency, QPC-timed (K=%d, iters=%d, cosine, top-5) ===\n", K, ITERS);
    printf("corpus %d x %d, %d queries\n\n", n, dim, NQ);

    float *X = (float *)malloc((size_t)n*dim*sizeof(float));
    FILE *f = fopen(path, "rb");
    if (!X || !f) { printf("load fail\n"); return 1; }
    if (fread(X, sizeof(float), (size_t)n*dim, f) != (size_t)n*dim) { printf("short read\n"); return 1; }
    fclose(f);

    /* train K=8 at ITERS (regrow winner) */
    float *C = (float *)malloc((size_t)K*dim*sizeof(float));
    int *lab = (int *)malloc((size_t)n*sizeof(int));
    double *sums = (double *)calloc((size_t)K*dim, sizeof(double));
    int *cnt = (int *)calloc((size_t)K, sizeof(int));
    for (int k = 0; k < K; k++)
        memcpy(C + (size_t)k*dim, X + (size_t)(k*n/K)*dim, (size_t)dim*sizeof(float));
    double t0 = now_us();
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
    double t_train = now_us() - t0;
    t0 = now_us();
    for (int i = 0; i < n; i++) lab[i] = anch_assign(X + (size_t)i*dim, C, K, dim);
    double t_assign = now_us() - t0;
    printf("regrow: train %.3f s + assign %.3f s + perm/save ~0.009 s = %.3f s\n\n",
           t_train/1e6, t_assign/1e6, (t_train+t_assign)/1e6 + 0.009);

    /* queries */
    int nq = NQ;
    int *qi = (int *)malloc((size_t)nq*sizeof(int));
    srand(11);
    for (int q = 0; q < nq; q++) qi[q] = rand() % n;

    int (*tk)[5] = malloc((size_t)nq*5*sizeof(int));
    double *lat = (double *)malloc((size_t)nq*sizeof(double));

    printf("%-10s %-9s %-9s %-9s %-9s %-8s\n",
           "config", "mean_ms", "p50_ms", "p95_ms", "max_ms", "scanned");
    FILE *csv = fopen("lat.csv", "w");
    fprintf(csv, "config,mean_ms,p50_ms,p95_ms,max_ms,scanned_per_q\n");

    int topbs[] = {2, 4, 6};
    for (int ti = 0; ti < 3; ti++) {
        int tb = topbs[ti];
        /* warm up once */
        serve_one(X, n, dim, C, tb, lab, X + (size_t)qi[0]*dim, 5, tk[0]);
        long scanned = 0;
        for (int q = 0; q < nq; q++) {
            double a = now_us();
            scanned += serve_one(X, n, dim, C, tb, lab, X + (size_t)qi[q]*dim, 5, tk[q]);
            lat[q] = now_us() - a;
        }
        double sum = 0, mx = 0;
        for (int q = 0; q < nq; q++) { sum += lat[q]; if (lat[q] > mx) mx = lat[q]; }
        double *srt = (double *)malloc((size_t)nq*sizeof(double));
        memcpy(srt, lat, (size_t)nq*sizeof(double));
        qsort(srt, nq, sizeof(double), cmp_d);
        char name[32]; snprintf(name, sizeof(name), "K8_topb%d", tb);
        printf("%-10s %-9.2f %-9.2f %-9.2f %-9.2f %-8ld\n",
               name, sum/nq/1000.0, srt[nq/2]/1000.0,
               srt[(int)(nq*0.95)]/1000.0, mx/1000.0, scanned/nq);
        fprintf(csv, "%s,%.2f,%.2f,%.2f,%.2f,%ld\n", name,
                sum/nq/1000.0, srt[nq/2]/1000.0, srt[(int)(nq*0.95)]/1000.0,
                mx/1000.0, scanned/nq);
        free(srt);
    }

    /* brute reference */
    brute_one(X, n, dim, X + (size_t)qi[0]*dim, 5, tk[0]);
    for (int q = 0; q < nq; q++) {
        double a = now_us();
        brute_one(X, n, dim, X + (size_t)qi[q]*dim, 5, tk[q]);
        lat[q] = now_us() - a;
    }
    double sum = 0, mx = 0;
    for (int q = 0; q < nq; q++) { sum += lat[q]; if (lat[q] > mx) mx = lat[q]; }
    double *srt = (double *)malloc((size_t)nq*sizeof(double));
    memcpy(srt, lat, (size_t)nq*sizeof(double));
    qsort(srt, nq, sizeof(double), cmp_d);
    printf("%-10s %-9.2f %-9.2f %-9.2f %-9.2f %-8d\n",
           "brute", sum/nq/1000.0, srt[nq/2]/1000.0,
           srt[(int)(nq*0.95)]/1000.0, mx/1000.0, n);
    fprintf(csv, "brute,%.2f,%.2f,%.2f,%.2f,%d\n",
            sum/nq/1000.0, srt[nq/2]/1000.0, srt[(int)(nq*0.95)]/1000.0, mx/1000.0, n);
    free(srt);
    fclose(csv);

    free(X); free(C); free(lab); free(sums); free(cnt); free(qi); free(tk); free(lat);
    printf("\nDONE\n");
    return 0;
}
