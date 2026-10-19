/*
 * ann_sse.c — does SSE2 brute beat routed scalar?
 * ═══════════════════════════════════════════════════════════════════════════
 * ann_kernel.c proved the cost is the kernel, not the scan count:
 *   A double scalar + norm   routed 45.5 ms / brute 113.9 ms
 *   C float 4-acc, no norm   routed  9.5 ms / brute  20.3 ms
 * Routing (topb=2) scans ~35% of the rows, so its ceiling is ~2.9x brute with
 * the SAME kernel — which is exactly what C shows (2.1x). So routing cannot be
 * judged against a slow kernel.
 *
 * The remaining question: with SSE2 (baseline x86-64, always safe here — AVX×
 * are disabled on this box) how fast is a full exact scan? If SSE2-brute lands
 * near routed-scalar, routing buys nothing on a 61k corpus and the honest
 * recommendation is "make the scan fast, keep it exact".
 *
 * Variants:
 *   C   float 4-accumulator scalar, no norm   (best from ann_kernel.c)
 *   E   SSE2 4x unrolled, 4 accumulators, no norm
 *   F   SSE2 + cache-blocked, tile suggestion vectors into L1 line groups
 *
 * Correctness: identical top-5 index list vs variant C for every query.
 *
 * Build: gcc -O2 -std=c11 -msse2 -I I:/DWGLS-native-fs/core -o ann_sse ann_sse.c -lm
 * Run:   ./ann_sse X.f32 61241 384
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <emmintrin.h>
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

/* ── kernels: all return cosine == dot (corpus is unit-norm) ──────────── */

static double dot_f4(const float *a, const float *b, int d) {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0; int j = 0;
    for (; j + 4 <= d; j += 4) {
        s0 += a[j]*b[j]; s1 += a[j+1]*b[j+1];
        s2 += a[j+2]*b[j+2]; s3 += a[j+3]*b[j+3];
    }
    for (; j < d; j++) s0 += a[j]*b[j];
    return (double)((s0 + s1) + (s2 + s3));
}

static double dot_sse(const float *a, const float *b, int d) {
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    __m128 s2 = _mm_setzero_ps(), s3 = _mm_setzero_ps();
    int j = 0;
    for (; j + 16 <= d; j += 16) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a+j),    _mm_loadu_ps(b+j)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a+j+4),  _mm_loadu_ps(b+j+4)));
        s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_loadu_ps(a+j+8),  _mm_loadu_ps(b+j+8)));
        s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_loadu_ps(a+j+12), _mm_loadu_ps(b+j+12)));
    }
    for (; j + 4 <= d; j += 4)
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a+j), _mm_loadu_ps(b+j)));
    s0 = _mm_add_ps(_mm_add_ps(s0, s1), _mm_add_ps(s2, s3));
    s1 = _mm_add_ps(s0, _mm_movehl_ps(s0, s0));
    s1 = _mm_add_ss(s1, _mm_shuffle_ps(s1, s1, 1));
    float r; _mm_store_ss(&r, s1);
    for (; j < d; j++) r += a[j]*b[j];
    return (double)r;
}

/* top-k insertion shared by every config */
#define TRY(score, idx)                                                    \
    do {                                                                   \
        const double _sc = (score); int _id = (idx);                        \
        if (_sc > bs[KV-1]) {                                              \
            for (int _m = 0; _m < KV; _m++) if (_sc > bs[_m]) {            \
                for (int _p = KV-1; _p > _m; _p--) {                       \
                    bs[_p]=bs[_p-1]; bi[_p]=bi[_p-1];                      \
                }                                                          \
                bs[_m]=_sc; bi[_m]=_id; break;                             \
            }                                                              \
        }                                                                  \
    } while (0)

/* variant C/D: full exact scan, scalar */
static void brute_scalar(const float *X, int n, int dim, const float *qv, int *out_k) {
    double bs[32]; int bi[32];
    for (int i = 0; i < KV; i++) { bs[i] = -2; bi[i] = -1; }
    for (int i = 0; i < n; i++) TRY(dot_f4(qv, X + (size_t)i*dim, dim), i);
    for (int i = 0; i < KV; i++) out_k[i] = bi[i];
}

/* variant E: full exact scan, SSE2 */
static void brute_sse(const float *X, int n, int dim, const float *qv, int *out_k) {
    double bs[32]; int bi[32];
    for (int i = 0; i < KV; i++) { bs[i] = -2; bi[i] = -1; }
    for (int i = 0; i < n; i++) TRY(dot_sse(qv, X + (size_t)i*dim, dim), i);
    for (int i = 0; i < KV; i++) out_k[i] = bi[i];
}

/* variant F: SSE2, query broadcast once, block over rows to reuse L1 */
static void brute_sse_block(const float *X, int n, int dim, const float *qv, int *out_k) {
    double bs[32]; int bi[32];
    for (int i = 0; i < KV; i++) { bs[i] = -2; bi[i] = -1; }
    const int BLK = 64;                       /* 64 rows x 384 floats = 96KB */
    for (int base = 0; base < n; base += BLK) {
        int hi = base + BLK; if (hi > n) hi = n;
        for (int i = base; i < hi; i++) {
            const float *xi = X + (size_t)i*dim;
            __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
            __m128 s2 = _mm_setzero_ps(), s3 = _mm_setzero_ps();
            int j = 0;
            for (; j + 16 <= dim; j += 16) {
                s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(qv+j),    _mm_loadu_ps(xi+j)));
                s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(qv+j+4),  _mm_loadu_ps(xi+j+4)));
                s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_loadu_ps(qv+j+8),  _mm_loadu_ps(xi+j+8)));
                s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_loadu_ps(qv+j+12), _mm_loadu_ps(xi+j+12)));
            }
            s0 = _mm_add_ps(_mm_add_ps(s0, s1), _mm_add_ps(s2, s3));
            s1 = _mm_add_ps(s0, _mm_movehl_ps(s0, s0));
            s1 = _mm_add_ss(s1, _mm_shuffle_ps(s1, s1, 1));
            float r; _mm_store_ss(&r, s1);
            for (; j < dim; j++) r += qv[j]*xi[j];
            TRY((double)r, i);
        }
    }
    for (int i = 0; i < KV; i++) out_k[i] = bi[i];
}

/* routed (topb=2) with the SSE kernel */
static int routed_sse(const float *X, int n, int dim, const float *C, int topb,
                      const int *lab, const float *qv, int *out_k) {
    int rb[ANCHR_MAXK];
    int ng = anch_route(qv, C, K, dim, topb, rb);
    double bs[32]; int bi[32];
    for (int i = 0; i < KV; i++) { bs[i] = -2; bi[i] = -1; }
    int scored = 0;
    for (int t = 0; t < ng; t++) for (int i = 0; i < n; i++) {
        if (lab[i] != rb[t]) continue;
        TRY(dot_sse(qv, X + (size_t)i*dim, dim), i);
        scored++;
    }
    for (int i = 0; i < KV; i++) out_k[i] = bi[i];
    return scored;
}

typedef struct { const char *name; double mean, p50, p95, mx; long scanned; int match; } Cell;

static Cell bench(const char *name, int cfg, const float *X, int n, int dim,
                  const float *C, const int *lab, const int *qi, int nq,
                  const int (*ref)[KV]) {
    Cell c; c.name = name;
    double *lat = (double *)malloc((size_t)nq*sizeof(double));
    int (*tk)[KV] = malloc((size_t)nq*KV*sizeof(int));
    c.scanned = 0;
    /* 3 warm-up queries so the first timed query is not a cold-cache outlier */
    for (int w = 0; w < 3; w++) {
        const float *qv = X + (size_t)qi[w]*dim;
        switch (cfg) {
        case 0: brute_scalar(X,n,dim,qv,tk[w]); break;
        case 1: brute_sse(X,n,dim,qv,tk[w]); break;
        case 2: brute_sse_block(X,n,dim,qv,tk[w]); break;
        default: routed_sse(X,n,dim,C,2,lab,qv,tk[w]); break;
        }
    }
    for (int q = 0; q < nq; q++) {
        const float *qv = X + (size_t)qi[q]*dim;
        double a = now_us();
        switch (cfg) {
        case 0: brute_scalar(X,n,dim,qv,tk[q]); break;
        case 1: brute_sse(X,n,dim,qv,tk[q]); break;
        case 2: brute_sse_block(X,n,dim,qv,tk[q]); break;
        default: c.scanned += routed_sse(X,n,dim,C,2,lab,qv,tk[q]); break;
        }
        lat[q] = now_us() - a;
    }
    if (cfg == 3) { /* routed */ } else c.scanned = n;
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

    printf("=== SSE2 scan vs routed scalar (K=%d, iters=%d, top-5) ===\n", K, ITERS);
    printf("corpus %d x %d, %d queries, -O2 -msse2, single-thread\n\n", n, dim, NQ);

    float *X = (float *)malloc((size_t)n*dim*sizeof(float));
    FILE *f = fopen(path, "rb");
    if (!X || !f) { printf("load fail: %s\n", path); return 1; }
    if (fread(X, sizeof(float), (size_t)n*dim, f) != (size_t)n*dim) { printf("short read\n"); return 1; }
    fclose(f);

    /* unit-norm gate: variant C/E/F are only valid if this holds */
    double worst = 0;
    for (int i = 0; i < n; i++) {
        const float *xi = X + (size_t)i*dim;
        float s = 0; for (int j = 0; j < dim; j++) s += xi[j]*xi[j];
        double e = fabs(sqrt((double)s) - 1.0);
        if (e > worst) worst = e;
    }
    printf("unit-norm check: max | |x| - 1 | = %.3e  %s\n\n",
           worst, worst < 1e-5 ? "OK -> cosine == dot" : "FAIL -> norm required");

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

    int *qi = (int *)malloc((size_t)NQ*sizeof(int));
    srand(11);
    for (int q = 0; q < NQ; q++) qi[q] = rand() % n;

    /* reference = variant C (scalar brute), the exact answer */
    int (*ref)[KV] = malloc((size_t)NQ*KV*sizeof(int));
    for (int q = 0; q < NQ; q++)
        brute_scalar(X,n,dim,X+(size_t)qi[q]*dim,ref[q]);

    struct { const char *name; int cfg; } cfgs[] = {
        {"C  scalar f4",        0},
        {"E  SSE2",             1},
        {"F  SSE2 blocked",     2},
        {"R  routed topb2 SSE", 3},
    };

    FILE *csv = fopen("sse.csv", "w");
    fprintf(csv, "variant,mean_ms,p50_ms,p95_ms,max_ms,scanned,top5_match\n");

    printf("%-20s %-9s %-9s %-9s %-9s %-8s %-s\n",
           "variant", "mean_ms", "p50_ms", "p95_ms", "scanned", "top5", "speedup");
    double base = 0;
    for (int c = 0; c < 4; c++) {
        Cell r = bench(cfgs[c].name, cfgs[c].cfg, X,n,dim,C,lab,qi,NQ,(const int (*)[KV])ref);
        if (c == 0) base = r.mean;
        printf("%-20s %-9.2f %-9.2f %-9.2f %-9ld %d/%-6d %.2fx\n",
               r.name, r.mean, r.p50, r.p95, r.scanned, r.match, NQ, base/r.mean);
        fprintf(csv, "%s,%.2f,%.2f,%.2f,%.2f,%ld,%d\n",
                r.name, r.mean, r.p50, r.p95, r.mx, r.scanned, r.match);
        fflush(csv);
    }
    fclose(csv);

    free(X); free(C); free(lab); free(sums); free(cnt); free(qi); free(ref);
    printf("\nDONE\n");
    return 0;
}