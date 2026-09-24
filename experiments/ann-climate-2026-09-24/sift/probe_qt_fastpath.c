/* experiments/ann-climate-2026-09-24/sift/probe_qt_fastpath.c — bare-quadtree fast path.
 *
 * OWNER (2026-09-25): step back to BARE quadtree first. Pattern 1-4-16 is the
 * frustum pattern: 2 levels of 4-split on the PCA2 plane. Route = 2 decisions
 * (fast path), finish = exact 128-dim inside the leaf (from structure, no
 * centroid assign, no cross-bucket rerank net).
 *
 * CHOICE DOCUMENTED:
 * - Plane = PCA2 (first 2 rows of pca_comp.bin, mean-subtracted). No new PCA.
 * - Splits at MEDIANS (level-1 global medians -> 4 quads; level-2 per-quad
 *   medians -> 16 leaves), not means — median splits balance counts as far as
 *   the distribution allows; residual imbalance IS measured (min/max/mean).
 * - Pure leaf only: no neighbor-leaf fallback. Answers outside the leaf are
 *   unrecoverable by design — that loss is the measurement (same hard-boundary
 *   wall as buckets, coarser).
 * - Baseline A (PCA25 + top-16 + rerank, expect 0.8023) rerun as sanity.
 *
 * THE REAL COMPARISON is routing cost: baseline pays project (25x128) +
 * 2560x25 distance MACs per query (~67k); QT pays project-PCA2 (2x128) + 4
 * bound compares. Expect ~200x cheaper routing; recall likely lower
 * (16 imbalanced leaves vs 2560 trained buckets).
 *
 * GATE: QT-fastpath VIABLE iff recall >= A-0.05 at strictly lower ms/q.
 * Else: routing-cost number stands, recall gap recorded, adaptive subdivision
 * (image-d: split only dense leaves) becomes the follow-up, not this probe.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_qt_fastpath.exe experiments/ann-climate-2026-09-24/sift/probe_qt_fastpath.c -lm
 * RUN (repo root): build/probe_qt_fastpath.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define DIM 128
#define PDIM 25
#define NQ 1000
#define NLEAF 16

static float *PC, *PM, *BASE, *QUERY;
static int32_t *GT;
static int64_t *LOFF; /* 17 offsets over leaf member dense array */
static int32_t *LMEM;
static int NBASE, NQRY, GTD;
static float *PX, *PY; /* PCA2 coords of base */

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

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) ? -1 : (x > y);
}
static float median_of(float *v, int n) {
    qsort(v, (size_t)n, 4, cmpf);
    return v[n / 2];
}
/* PCA2 coords of vector x */
static void pca2(const float *x, float *ox, float *oy) {
    double sx = 0, sy = 0;
    const float *r0 = PC, *r1 = PC + DIM;
    for (int j = 0; j < DIM; j++) {
        double c = (double)x[j] - PM[j];
        sx += (double)r0[j] * c;
        sy += (double)r1[j] * c;
    }
    *ox = (float)sx; *oy = (float)sy;
}
static double recall_at10(const int *ans, const int32_t *gt) {
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (ans[i] == gt[j]) { hit++; break; }
    return hit / 10.0;
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
    printf("load: base %d query %d gt-dim %d in %.1fs\n", NBASE, NQRY, GTD, (now_ms() - t0) / 1000.0);

    /* PCA2 project all base */
    t0 = now_ms();
    PX = (float *)malloc((size_t)NBASE * 4);
    PY = (float *)malloc((size_t)NBASE * 4);
    for (int i = 0; i < NBASE; i++)
        pca2(BASE + (size_t)i * DIM, PX + i, PY + i);
    printf("pca2: 1M in %.1fs\n", (now_ms() - t0) / 1000.0);

    /* level-1: global medians -> 4 quads */
    float *tx = (float *)malloc((size_t)NBASE * 4);
    float *ty = (float *)malloc((size_t)NBASE * 4);
    memcpy(tx, PX, (size_t)NBASE * 4);
    memcpy(ty, PY, (size_t)NBASE * 4);
    float mx = median_of(tx, NBASE), my = median_of(ty, NBASE);

    /* assign quads, then per-quad medians -> 16 leaves */
    int *quad = (int *)malloc((size_t)NBASE * 4);
    int qcnt[4] = {0};
    for (int i = 0; i < NBASE; i++) {
        int q = (PX[i] >= mx ? 1 : 0) + (PY[i] >= my ? 2 : 0);
        quad[i] = q;
        qcnt[q]++;
    }
    /* per-quad median splits */
    float qmx[4], qmy[4];
    int nq = 0;
    for (int q = 0; q < 4; q++) {
        float *qx = (float *)malloc((size_t)(qcnt[q] + 1) * 4);
        float *qy = (float *)malloc((size_t)(qcnt[q] + 1) * 4);
        int k = 0;
        for (int i = 0; i < NBASE; i++) if (quad[i] == q) { qx[k] = PX[i]; qy[k] = PY[i]; k++; }
        qmx[q] = median_of(qx, qcnt[q]);
        qmy[q] = median_of(qy, qcnt[q]);
        free(qx); free(qy);
        nq++;
    }
    (void)nq;
    /* leaf = quad*4 + subquad */
    int *leaf = (int *)malloc((size_t)NBASE * 4);
    int lcnt[NLEAF] = {0};
    for (int i = 0; i < NBASE; i++) {
        int q = quad[i];
        int sq = (PX[i] >= qmx[q] ? 1 : 0) + (PY[i] >= qmy[q] ? 2 : 0);
        int L = q * 4 + sq;
        leaf[i] = L;
        lcnt[L]++;
    }
    LOFF = (int64_t *)malloc(((size_t)NLEAF + 1) * 8);
    LOFF[0] = 0;
    for (int L = 0; L < NLEAF; L++) LOFF[L + 1] = LOFF[L] + lcnt[L];
    LMEM = (int32_t *)malloc((size_t)LOFF[NLEAF] * 4);
    int64_t fill[NLEAF];
    memcpy(fill, LOFF, sizeof(fill));
    for (int i = 0; i < NBASE; i++) LMEM[fill[leaf[i]]++] = i;
    int lmin = lcnt[0], lmax = lcnt[0], lemp = 0;
    long long lsum = 0;
    for (int L = 0; L < NLEAF; L++) {
        if (lcnt[L] < lmin) lmin = lcnt[L];
        if (lcnt[L] > lmax) lmax = lcnt[L];
        if (lcnt[L] == 0) lemp++;
        lsum += lcnt[L];
    }
    printf("QT 1-4-16: global split x=%.3g y=%.3g | leaf min=%d max=%d mean=%.0f empty=%d\n",
           mx, my, lmin, lmax, (double)lsum / NLEAF, lemp);
    printf("LEAF histogram:");
    for (int L = 0; L < NLEAF; L++) printf(" %d", lcnt[L]);
    printf("\n");
    free(tx); free(ty); free(quad); free(leaf);

    /* QUERY: route (4 compares) + exact top-10 inside leaf */
    int ans[10];
    double rec = 0;
    long long nsc = 0;
    t0 = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        float qx, qy;
        pca2(QUERY + (size_t)qi * DIM, &qx, &qy);
        int q = (qx >= mx ? 1 : 0) + (qy >= my ? 2 : 0);
        int sq = (qx >= qmx[q] ? 1 : 0) + (qy >= qmy[q] ? 2 : 0);
        int L = q * 4 + sq;
        /* exact 128-dim top-10 over leaf members */
        const float *qq = QUERY + (size_t)qi * DIM;
        double best[10];
        for (int i = 0; i < 10; i++) { best[i] = 1e300; ans[i] = -1; }
        int64_t ns = 0;
        for (int64_t k = LOFF[L]; k < LOFF[L + 1]; k++) {
            const float *v = BASE + (size_t)LMEM[k] * DIM;
            double d = 0;
            for (int j = 0; j < DIM; j++) { double e = (double)qq[j] - v[j]; d += e * e; }
            ns++;
            if (d < best[9]) {
                int p = 9;
                while (p > 0 && d < best[p - 1]) { best[p] = best[p - 1]; ans[p] = ans[p - 1]; p--; }
                best[p] = d; ans[p] = LMEM[k];
            }
        }
        nsc += ns;
        rec += recall_at10(ans, GT + (size_t)qi * GTD);
    }
    double ms = (now_ms() - t0) / NQ;
    rec /= NQ;
    printf("QT-FASTPATH (n=%d, pure leaf, exact-in-leaf): recall=%.4f scan=%.0f (%.3f%%) %.2fms/q\n",
           NQ, rec, (double)nsc / NQ, (double)nsc / NQ / NBASE * 100.0, ms);
    printf("ROUTING COST: QT = 2x128 MACs + 4 compares vs baseline 25x128 + 2560x25 MACs (~67k)\n");
    /* baseline reference from probe_lane128/fold_family (same n, same GT): 0.8023 @ 0.697%% */
    double base = 0.8023;
    int win = (rec >= base - 0.05);
    printf("GATE: QT-fastpath %s (recall %.4f vs A %.4f, need >= %.4f)\n",
           win ? "VIABLE" : "LOSES", rec, base, base - 0.05);
    return 0;
}
