/*
 * ann_kernel.c — where the query latency actually goes
 * ═══════════════════════════════════════════════════════════════════════════
 * Finding that made this file necessary: ann_lat.c measured topb=2 at
 * 32.5 ms mean vs brute 57.6 ms = only 1.8x, i.e. routing away 65% of the
 * corpus bought almost nothing. The scan count fell 3x but the time did not,
 * so the bottleneck is NOT how many vectors we visit — it is the cost of
 * visiting one.
 *
 * This file isolates the kernel. Same corpus, same queries, same top-k
 * insertion loop; only the distance kernel changes:
 *   A  double scalar dot + double norm     (what ann_lat.c / the server do)
 *   B  float  scalar dot + float  norm
 *   C  float  4-accumulator dot, norm skipped (corpus is unit-norm)
 *   D  float  4-accumulator dot + float norm
 *   E  double 4-accumulator dot + double norm   <-- stays in double throughout
 *
 * Variant E exists to answer "does the speedup require leaving double?".
 * A is slow because `s += a[j]*b[j]` is a serial dependency chain (4-cycle add
 * latency on this G4400): 384 dims ~ 1500 cycles before any throughput. The fix
 * is INDEPENDENT accumulators so the adds overlap — NOT lower precision. E has
 * the same 4-accumulator structure as C/D but every accumulator is double, so if
 * E is near C's speed the double path is what the server should adopt: exact
 * arithmetic, no unit-norm precondition, and no change in the scoring type.
 *
 * Correctness gate: every variant must return the SAME top-5 as variant A.
 * C is only valid because all 61,241 rows are unit-norm (verified: |x|-1 <
 * 1e-7 for 100.00% of rows), which makes cosine == dot. E needs no such
 * precondition.
 *
 * Build: gcc -O2 -std=c11 -I I:/DWGLS-native-fs/core -o ann_kernel ann_kernel.c -lm
 * Run:   ./ann_kernel X.f32 61241 384
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
#define KV    5

static double g_freq;
static double now_us(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / g_freq;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double dot_double(const float *a, const float *b, int d) {
    double s = 0; for (int j = 0; j < d; j++) s += (double)a[j] * (double)b[j]; return s;
}
static double dot_float(const float *a, const float *b, int d) {
    float s = 0; for (int j = 0; j < d; j++) s += a[j] * b[j]; return (double)s;
}
static double dot_f4(const float *a, const float *b, int d) {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0; int j = 0;
    for (; j + 4 <= d; j += 4) {
        s0 += a[j]*b[j]; s1 += a[j+1]*b[j+1];
        s2 += a[j+2]*b[j+2]; s3 += a[j+3]*b[j+3];
    }
    for (; j < d; j++) s0 += a[j]*b[j];
    return (double)((s0 + s1) + (s2 + s3));
}
/* same 4-accumulator structure, all-double: answers "is the win from breaking
 * the dependency chain, or from dropping to float?" */
static double dot_d4(const float *a, const float *b, int d) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0; int j = 0;
    for (; j + 4 <= d; j += 4) {
        s0 += (double)a[j]*(double)b[j]; s1 += (double)a[j+1]*(double)b[j+1];
        s2 += (double)a[j+2]*(double)b[j+2]; s3 += (double)a[j+3]*(double)b[j+3];
    }
    for (; j < d; j++) s0 += (double)a[j]*(double)b[j];
    return (s0 + s1) + (s2 + s3);
}

static double norm_double(const float *a, int d) {
    double s = 0; for (int j = 0; j < d; j++) s += (double)a[j]*a[j];
    return s > 0 ? sqrt(s) : 1;
}
static double norm_float(const float *a, int d) {
    float s = 0; for (int j = 0; j < d; j++) s += a[j]*a[j];
    return s > 0 ? sqrt((double)s) : 1;
}

/* Production shape: the per-entry norm is computed ONCE at index-load time
 * (the server stores ents[ei].norm and never recomputes it per query), so the
 * hot loop is pure dot. Same 4-double-accumulator dot as E/F => exact. */
static float *g_norms = NULL;      /* precomputed ||x|| per row */
static const float *g_X0 = NULL;   /* corpus base, to recover the row index */
static int g_dim0 = 0;
static double score_d4_precomp(const float *qv, const float *xi, int dim, double qn) {
    int idx = (int)((xi - g_X0) / g_dim0);
    return dot_d4(qv, xi, dim) / (qn * (double)g_norms[idx]);
}
/* H = the ACTUAL server shape: 1-accumulator double dot + the per-entry norm
 * the loader stored at index-load time (ann_search.c parses `norm` into
 * LZEntry.norm once and never recomputes it per query). G is H with the dot
 * widened to 4 independent accumulators, so H->G isolates the accumulator
 * change alone — the exact patch under consideration. */
static double score_h_precomp(const float *qv, const float *xi, int dim, double qn) {
    int idx = (int)((xi - g_X0) / g_dim0);
    return dot_double(qv, xi, dim) / (qn * (double)g_norms[idx]);
}

/* score one vector with the chosen kernel */
static double score(const float *qv, const float *xi, int dim, int kernel, double qn) {
    double dot, xn = 1;
    switch (kernel) {
    case 0: dot = dot_double(qv, xi, dim); xn = norm_double(xi, dim); break;
    case 1: dot = dot_float(qv, xi, dim);  xn = norm_float(xi, dim);  break;
    case 2: dot = dot_f4(qv, xi, dim);     xn = 1;                    break;
    case 3: dot = dot_f4(qv, xi, dim);     xn = norm_float(xi, dim);  break;
    case 4: dot = dot_d4(qv, xi, dim);     xn = norm_double(xi, dim); break;
    case 5: dot = dot_d4(qv, xi, dim);     xn = 1;                    break;
    case 6: return score_d4_precomp(qv, xi, dim, qn);
    case 7: return score_h_precomp(qv, xi, dim, qn);
    default:dot = dot_f4(qv, xi, dim);     xn = 1;                    break;
    }
    return dot / (qn * xn);
}

static int serve_routed(const float *X, int n, int dim, const float *C, int topb,
                        const int *lab, const float *qv, int kernel, int *out_k) {
    int rb[ANCHR_MAXK];
    int ng = anch_route(qv, C, K, dim, topb, rb);
    double qn = (kernel == 2) ? 1 : norm_double(qv, dim);
    double bs[32]; int bi[32];
    for (int i = 0; i < KV; i++) { bs[i] = -2; bi[i] = -1; }
    int scored = 0;
    for (int t = 0; t < ng; t++) for (int i = 0; i < n; i++) {
        if (lab[i] != rb[t]) continue;
        double sc = score(qv, X + (size_t)i*dim, dim, kernel, qn);
        scored++;
        if (sc <= bs[KV-1]) continue;
        for (int m = 0; m < KV; m++) if (sc > bs[m]) {
            for (int p = KV-1; p > m; p--) { bs[p]=bs[p-1]; bi[p]=bi[p-1]; }
            bs[m]=sc; bi[m]=i; break;
        }
    }
    for (int i = 0; i < KV; i++) out_k[i] = bi[i];
    return scored;
}

static void serve_brute(const float *X, int n, int dim, const float *qv,
                        int kernel, int *out_k) {
    double qn = (kernel == 2) ? 1 : norm_double(qv, dim);
    double bs[32]; int bi[32];
    for (int i = 0; i < KV; i++) { bs[i] = -2; bi[i] = -1; }
    for (int i = 0; i < n; i++) {
        double sc = score(qv, X + (size_t)i*dim, dim, kernel, qn);
        if (sc <= bs[KV-1]) continue;
        for (int m = 0; m < KV; m++) if (sc > bs[m]) {
            for (int p = KV-1; p > m; p--) { bs[p]=bs[p-1]; bi[p]=bi[p-1]; }
            bs[m]=sc; bi[m]=i; break;
        }
    }
    for (int i = 0; i < KV; i++) out_k[i] = bi[i];
}

/* one experiment cell */
typedef struct { char name[20]; double mean, p50, p95, mx; long scanned; int match; } Cell;

static Cell run_cell(const char *name, int kernel, int topb,
                     const float *X, int n, int dim, const float *C, const int *lab,
                     const int *qi, int nq, const int (*ref)[KV]) {
    Cell c; snprintf(c.name, sizeof(c.name), "%s", name);
    double *lat = (double *)malloc((size_t)nq*sizeof(double));
    int (*tk)[KV] = malloc((size_t)nq*KV*sizeof(int));
    c.scanned = 0;
    /* warm-up */
    if (topb > 0) serve_routed(X,n,dim,C,topb,lab,X+(size_t)qi[0]*dim,kernel,tk[0]);
    else          serve_brute(X,n,dim,X+(size_t)qi[0]*dim,kernel,tk[0]);
    for (int q = 0; q < nq; q++) {
        const float *qv = X + (size_t)qi[q]*dim;
        double a = now_us();
        if (topb > 0) c.scanned += serve_routed(X,n,dim,C,topb,lab,qv,kernel,tk[q]);
        else          serve_brute(X,n,dim,qv,kernel,tk[q]);
        lat[q] = now_us() - a;
    }
    if (topb <= 0) c.scanned = n;
    /* correctness vs reference */
    int ok = 0;
    for (int q = 0; q < nq; q++) {
        int same = 1;
        for (int k = 0; k < KV; k++) if (tk[q][k] != ref[q][k]) { same = 0; break; }
        ok += same;
    }
    c.match = ok;
    double sum = 0, mx = 0;
    for (int q = 0; q < nq; q++) { sum += lat[q]; if (lat[q] > mx) mx = lat[q]; }
    double *srt = (double *)malloc((size_t)nq*sizeof(double));
    memcpy(srt, lat, (size_t)nq*sizeof(double));
    qsort(srt, nq, sizeof(double), cmp_d);
    c.mean = sum/nq/1000.0; c.p50 = srt[nq/2]/1000.0;
    c.p95 = srt[(int)(nq*0.95)]/1000.0; c.mx = mx/1000.0;
    free(srt); free(lat); free(tk);
    return c;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "X.f32";
    int n   = (argc > 2) ? atoi(argv[2]) : 61241;
    int dim = (argc > 3) ? atoi(argv[3]) : 384;

    LARGE_INTEGER fr; QueryPerformanceFrequency(&fr); g_freq = (double)fr.QuadPart;

    printf("=== distance kernel: where the ms go (K=%d, iters=%d, top-5) ===\n", K, ITERS);
    printf("corpus %d x %d, %d queries, -O2, single-thread\n\n", n, dim, NQ);

    float *X = (float *)malloc((size_t)n*dim*sizeof(float));
    FILE *f = fopen(path, "rb");
    if (!X || !f) { printf("load fail: %s\n", path); return 1; }
    if (fread(X, sizeof(float), (size_t)n*dim, f) != (size_t)n*dim) { printf("short read\n"); return 1; }
    fclose(f);

    float *C = (float *)malloc((size_t)K*dim*sizeof(float));
    int   *lab = (int *)malloc((size_t)n*sizeof(int));
    double *sums = (double *)calloc((size_t)K*dim, sizeof(double));
    int   *cnt  = (int *)calloc((size_t)K, sizeof(int));
    for (int k = 0; k < K; k++)
        memcpy(C + (size_t)k*dim, X + (size_t)(k*n/K)*dim, (size_t)dim*sizeof(float));
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

    /* kernel G: precompute per-row norms once, like the server does at load */
    g_norms = (float *)malloc((size_t)n*sizeof(float));
    g_X0 = X; g_dim0 = dim;
    for (int i = 0; i < n; i++) g_norms[i] = (float)norm_double(X + (size_t)i*dim, dim);

    int *qi = (int *)malloc((size_t)NQ*sizeof(int));
    srand(11);
    for (int q = 0; q < NQ; q++) qi[q] = rand() % n;

    /* reference = variant A (double), topb=2 */
    int (*ref)[KV] = malloc((size_t)NQ*KV*sizeof(int));
    for (int q = 0; q < NQ; q++)
        serve_routed(X,n,dim,C,2,lab,X+(size_t)qi[q]*dim,0,ref[q]);

    const char *kname[] = {"A dbl+norm", "B flt+norm", "C f4(unit)", "D f4+norm", "E d4+norm", "F d4(unit)", "G d4+pre", "H dbl+pre"};
    FILE *csv = fopen("kernel.csv", "w");
    fprintf(csv, "kernel,config,mean_ms,p50_ms,p95_ms,max_ms,scanned,top5_match\n");

    printf("%-11s %-8s %-9s %-9s %-9s %-8s %-8s %-s\n",
           "kernel", "config", "mean_ms", "p50_ms", "p95_ms", "scanned", "top5", "vs A");
    double baseA[2] = {0, 0};
    for (int kd = 0; kd < 8; kd++) {
        /* topb=2 */
        Cell c1 = run_cell("topb2", kd, 2, X,n,dim,C,lab,qi,NQ,(const int (*)[KV])ref);
        if (kd == 0) baseA[0] = c1.mean;
        printf("%-11s %-8s %-9.2f %-9.2f %-9.2f %-8ld %d/%-6d %.2fx\n",
               kname[kd], c1.name, c1.mean, c1.p50, c1.p95, c1.scanned, c1.match, NQ, baseA[0]/c1.mean);
        fprintf(csv, "%s,%s,%.2f,%.2f,%.2f,%.2f,%ld,%d\n",
                kname[kd], "topb2", c1.mean, c1.p50, c1.p95, c1.mx, c1.scanned, c1.match);

        /* brute */
        Cell cb = run_cell("brute", kd, 0, X,n,dim,C,lab,qi,NQ,(const int (*)[KV])ref);
        if (kd == 0) baseA[1] = cb.mean;
        printf("%-11s %-8s %-9.2f %-9.2f %-9.2f %-8ld %d/%-6d %.2fx\n",
               kname[kd], cb.name, cb.mean, cb.p50, cb.p95, cb.scanned, cb.match, NQ, baseA[1]/cb.mean);
        fprintf(csv, "%s,%s,%.2f,%.2f,%.2f,%.2f,%ld,%d\n",
                kname[kd], "brute", cb.mean, cb.p50, cb.p95, cb.mx, cb.scanned, cb.match);
        fflush(csv);
    }
    fclose(csv);

    free(X); free(C); free(lab); free(sums); free(cnt); free(qi); free(ref);
    printf("\nDONE\n");
    return 0;
}