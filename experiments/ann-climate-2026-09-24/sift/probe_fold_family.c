/* experiments/ann-climate-2026-09-24/sift/probe_fold_family.c — scaled equation family probe.
 *
 * OWNER (2026-09-25): the equation scales: smallest 4x3=12, square 16x9=144,
 * full 128x162=20736. Large forms measurable two ways: batch-128 fold into
 * 9x9=81 (ijk square), or full 128x18x9 field-per-vector.
 *
 * CHOICE DOCUMENTED:
 * - F81 (128->81): y[i] = x[i] + (i<47 ? x[i+81] : 0). 128 = 81+47, every dim
 *   used exactly once. Anchors = same fold applied to lifted 128-dim centroids.
 * - F12 (128->12, the 4x3 smallest): y[k] = sum_m x[k+12m]. 128 = 12x10+8
 *   (first 8 groups 11 members). Anchors folded identically. Int-friendly.
 * - 128x18x9 full-field-per-vector PARKED, not measured: fill rule undefined
 *   AND cost impossible (1000q x 1M base x 20736 = 2e13 MACs, days). Needs a
 *   shortlist-first design + a fill rule before it becomes a probe.
 *
 * METHOD: same top-16 single-assign recall@10 vs PCA25 baseline (Arm A from
 * probe_lane128, expect 0.8023 again as sanity). Per fold: assignment flips
 * vs baseline (expect NONZERO here — folds leave the PCA subspace, unlike
 * the lift), recall, ms/q, assign bytes.
 *
 * GATE per fold: recall >= baseline-0.01 AND (bytes < A OR ms < A) -> WINS.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_fold_family.exe experiments/ann-climate-2026-09-24/sift/probe_fold_family.c -lm
 * RUN (repo root): build/probe_fold_family.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define DIM 128
#define PDIM 25
#define D81 81
#define D12 12
#define NB 4096
#define NUSED 2560
#define NQ 1000
#define TOPC 16

static float *FC, *PC, *PM, *BASE, *QUERY, *LC;
static float *A81, *A12; /* folded anchors: NUSED x D */
static int32_t *GT, *MEM;
static int64_t *OFF;
static int NBASE, NQRY, GTD;
static int AID[NUSED];

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

static void project(const float *x, float *z) {
    for (int i = 0; i < PDIM; i++) {
        double s = 0;
        const float *row = PC + (size_t)i * DIM;
        for (int j = 0; j < DIM; j++) s += (double)row[j] * (x[j] - PM[j]);
        z[i] = (float)s;
    }
}
/* folds: every input dim used exactly once */
static void fold81(const float *x, float *y) {
    for (int i = 0; i < D81; i++)
        y[i] = x[i] + (i < 47 ? x[i + 81] : 0.0f);
}
static void fold12(const float *x, float *y) {
    for (int k = 0; k < D12; k++) {
        double s = 0;
        for (int m = k; m < DIM; m += D12) s += x[m];
        y[k] = (float)s;
    }
}
static double cdist(const float *a, const float *b, int d) {
    double s = 0;
    for (int j = 0; j < d; j++) { double e = (double)a[j] - b[j]; s += e * e; }
    return s;
}
/* top-TOPC used-idx nearest to key in space of dim d; centroids matrix C (NUSED x d) */
static void topC_used(const float *key, const float *C, int d, int *out, double *dw) {
    for (int i = 0; i < NUSED; i++)
        dw[i] = cdist(key, C + (size_t)i * d, d);
    for (int c = 0; c < TOPC; c++) {
        int bi = c;
        for (int i = c + 1; i < NUSED; i++)
            if (dw[i] < dw[bi]) bi = i;
        double td = dw[c]; dw[c] = dw[bi]; dw[bi] = td;
        out[c] = bi;
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
static double recall_at10(const int *besi, const int32_t *gt) {
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (besi[i] == gt[j]) { hit++; break; }
    return hit / 10.0;
}
/* scan single-assign buckets selected by top[] (used-idx) -> ids in scan[], returns ns */
static int scan_top(const int *top, int *scan, int32_t *stamp, int32_t cur) {
    int ns = 0;
    for (int c = 0; c < TOPC; c++) {
        int b = AID[top[c]];
        for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) {
            int id = MEM[k];
            if (stamp[id] != cur) { stamp[id] = cur; scan[ns++] = id; }
        }
    }
    return ns;
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

    /* LIFT 25-dim used centroids -> 128, then FOLD to 81 / 12 anchor spaces */
    t0 = now_ms();
    LC = (float *)malloc((size_t)NUSED * DIM * 4);
    A81 = (float *)malloc((size_t)NUSED * D81 * 4);
    A12 = (float *)malloc((size_t)NUSED * D12 * 4);
    for (int u = 0; u < NUSED; u++) {
        const float *z = FC + (size_t)AID[u] * PDIM;
        float *x = LC + (size_t)u * DIM;
        for (int j = 0; j < DIM; j++) {
            double s = PM[j];
            for (int i = 0; i < PDIM; i++) s += (double)z[i] * PC[(size_t)i * DIM + j];
            x[j] = (float)s;
        }
        fold81(x, A81 + (size_t)u * D81);
        fold12(x, A12 + (size_t)u * D12);
    }
    printf("LIFT+FOLD: %d centroids -> 128 -> {81,12} in %.2fs\n", NUSED, (now_ms() - t0) / 1000.0);

    /* centroid bank for baseline arm: gather used FC rows into contiguous NUSED x 25 */
    float *C25 = (float *)malloc((size_t)NUSED * PDIM * 4);
    for (int u = 0; u < NUSED; u++)
        memcpy(C25 + (size_t)u * PDIM, FC + (size_t)AID[u] * PDIM, PDIM * 4);

    int *scan = (int *)malloc((size_t)NBASE * 4);
    int32_t *stamp = (int32_t *)calloc(NBASE, 4);
    int32_t cur = 1;
    double *dw = (double *)malloc((size_t)NUSED * 8);
    int topA[TOPC], topF[TOPC], besi[10];
    float *zq = (float *)malloc(PDIM * 4);
    float *fq = (float *)malloc(D81 * 4); /* max fold dim */

    /* BASELINE arm (PCA25): also records reference top-16 sets for flip counts */
    static int refTop[NQ][TOPC];
    double recA = 0;
    long long nscanA = 0;
    double tq = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        const float *q = QUERY + (size_t)qi * DIM;
        project(q, zq);
        topC_used(zq, C25, PDIM, topA, dw);
        memcpy(refTop[qi], topA, sizeof(topA));
        int ns = scan_top(topA, scan, stamp, ++cur);
        rerank10(q, scan, ns, besi);
        nscanA += ns;
        recA += recall_at10(besi, GT + (size_t)qi * GTD);
    }
    double msA = (now_ms() - tq) / NQ;
    recA /= NQ;
    printf("A PCA25: recall=%.4f scan=%.0f (%.3f%%) %.2fms/q\n",
           recA, (double)nscanA / NQ, (double)nscanA / NQ / NBASE * 100.0, msA);

    /* FOLD arms */
    struct { const char *name; int d; float *C; void (*fold)(const float *, float *); } arms[] = {
        { "F81 9x9 ", D81, A81, fold81 },
        { "F12 4x3 ", D12, A12, fold12 },
    };
    long bytesA = (long)NUSED * PDIM * 4 + (long)PDIM * DIM * 4 + DIM * 4;
    for (int a = 0; a < 2; a++) {
        int d = arms[a].d;
        double rec = 0;
        long long nsc = 0;
        int flips = 0;
        tq = now_ms();
        for (int qi = 0; qi < NQ; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            arms[a].fold(q, fq);
            topC_used(fq, arms[a].C, d, topF, dw);
            int same = 1;
            for (int c = 0; c < TOPC && same; c++) {
                int f = 0;
                for (int k = 0; k < TOPC; k++) if (topF[k] == refTop[qi][c]) { f = 1; break; }
                if (!f) same = 0;
            }
            if (!same) flips++;
            int ns = scan_top(topF, scan, stamp, ++cur);
            rerank10(q, scan, ns, besi);
            nsc += ns;
            rec += recall_at10(besi, GT + (size_t)qi * GTD);
        }
        double ms = (now_ms() - tq) / NQ;
        rec /= NQ;
        long bytes = (long)NUSED * d * 4;
        int win = (rec >= recA - 0.01) && (bytes < bytesA || ms < msA);
        printf("%s: recall=%.4f (d=%+.4f) scan=%.0f (%.3f%%) %.2fms/q flips=%d/%d bytes=%ld (x%.2f) GATE %s\n",
               arms[a].name, rec, rec - recA, (double)nsc / NQ,
               (double)nsc / NQ / NBASE * 100.0, ms, flips, NQ,
               bytes, (double)bytes / bytesA, win ? "WINS" : "LOSES");
    }
    return 0;
}
