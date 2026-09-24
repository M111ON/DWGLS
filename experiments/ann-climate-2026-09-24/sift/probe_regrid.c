/* experiments/ann-climate-2026-09-24/sift/probe_regrid.c — re-grid stability probe.
 *
 * OWNER ONTOLOGY (2026-09-25, confirmed): geometry pictures are communication
 * analogies for STRUCTURAL boundaries. Buckets/QT cells don't exist as tanks —
 * the GRID arises in the field once a base is plugged in; retrieval surrounds
 * (loms) the query with nearby intersections. Consequence: changing the base
 * (re-grid) is NOT a rebuild — a new grid arises at assign-only cost, no merge.
 *
 * CHOICE DOCUMENTED:
 * - Grid = QT-16 on PCA2 (same as probe_qt_fastpath). Base = split positions.
 * - G0: 50th-percentile splits (median; expect recall 0.6257 as sanity).
 * - G1: 40th-percentile splits (shifted base). G2: 60th-percentile splits.
 * - Per arm: reassign 1M (timed separately = the "arise" cost), exact-in-leaf
 *   recall@10 (n=1000), top-10 set stability vs G0 (% queries identical).
 * - No merge step exists in the code path by construction; the timing proves
 *   the cost claim on 1M real vectors.
 *
 * GATE: field property HOLDS iff recall(G1),recall(G2) within +-0.03 of G0
 * AND reassign < 2s each AND stability reported (no threshold — first number).
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_regrid.exe experiments/ann-climate-2026-09-24/sift/probe_regrid.c -lm
 * RUN (repo root): build/probe_regrid.exe
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
static int NBASE, NQRY, GTD;
static float *PX, *PY;
static int ANS0[NQ][10]; /* G0 top-10 per query for stability */

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
/* p-th percentile of v[0..n) (sorts in place) */
static float pct(float *v, int n, double p) {
    qsort(v, (size_t)n, 4, cmpf);
    int k = (int)(n * p);
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    return v[k];
}
static double recall_at10(const int *ans, const int32_t *gt) {
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (ans[i] == gt[j]) { hit++; break; }
    return hit / 10.0;
}
static int same_set(const int *a, const int *b) {
    for (int i = 0; i < 10; i++) {
        int f = 0;
        for (int k = 0; k < 10; k++) if (b[k] == a[i]) { f = 1; break; }
        if (!f) return 0;
    }
    return 1;
}

/* run one grid at percentile p: returns recall; out assign_ms, scan, stability-vs-G0 (-1 for G0) */
static double run_grid(double p, int is_ref, double *assign_ms, double *msq, long long *nsc) {
    double t0 = now_ms();
    /* global splits */
    float *tx = (float *)malloc((size_t)NBASE * 4);
    float *ty = (float *)malloc((size_t)NBASE * 4);
    memcpy(tx, PX, (size_t)NBASE * 4);
    memcpy(ty, PY, (size_t)NBASE * 4);
    float mx = pct(tx, NBASE, p), my = pct(ty, NBASE, p);
    int *quad = (int *)malloc((size_t)NBASE * 4);
    int qcnt[4] = {0};
    for (int i = 0; i < NBASE; i++) {
        int q = (PX[i] >= mx ? 1 : 0) + (PY[i] >= my ? 2 : 0);
        quad[i] = q;
        qcnt[q]++;
    }
    float qmx[4], qmy[4];
    for (int q = 0; q < 4; q++) {
        float *qx = (float *)malloc((size_t)(qcnt[q] + 1) * 4);
        float *qy = (float *)malloc((size_t)(qcnt[q] + 1) * 4);
        int k = 0;
        for (int i = 0; i < NBASE; i++) if (quad[i] == q) { qx[k] = PX[i]; qy[k] = PY[i]; k++; }
        qmx[q] = pct(qx, qcnt[q], p);
        qmy[q] = pct(qy, qcnt[q], p);
        free(qx); free(qy);
    }
    int lcnt[NLEAF] = {0};
    int *leaf = (int *)malloc((size_t)NBASE * 4);
    for (int i = 0; i < NBASE; i++) {
        int q = quad[i];
        int sq = (PX[i] >= qmx[q] ? 1 : 0) + (PY[i] >= qmy[q] ? 2 : 0);
        leaf[i] = q * 4 + sq;
        lcnt[q * 4 + sq]++;
    }
    free(quad);
    int64_t loff[NLEAF + 1];
    loff[0] = 0;
    for (int L = 0; L < NLEAF; L++) loff[L + 1] = loff[L] + lcnt[L];
    int32_t *lmem = (int32_t *)malloc((size_t)loff[NLEAF] * 4);
    int64_t fl[NLEAF];
    memcpy(fl, loff, sizeof(fl));
    for (int i = 0; i < NBASE; i++) lmem[fl[leaf[i]]++] = i;
    free(leaf);
    *assign_ms = (now_ms() - t0); /* arise cost: assign-only, no merge anywhere */
    free(tx); free(ty);

    int ans[10];
    double rec = 0;
    long long ns = 0;
    int stab = 0;
    t0 = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        float qx, qy;
        pca2(QUERY + (size_t)qi * DIM, &qx, &qy);
        int q = (qx >= mx ? 1 : 0) + (qy >= my ? 2 : 0);
        int sq = (qx >= qmx[q] ? 1 : 0) + (qy >= qmy[q] ? 2 : 0);
        int L = q * 4 + sq;
        const float *qq = QUERY + (size_t)qi * DIM;
        double best[10];
        for (int i = 0; i < 10; i++) { best[i] = 1e300; ans[i] = -1; }
        for (int64_t k = loff[L]; k < loff[L + 1]; k++) {
            const float *v = BASE + (size_t)lmem[k] * DIM;
            double d = 0;
            for (int j = 0; j < DIM; j++) { double e = (double)qq[j] - v[j]; d += e * e; }
            ns++;
            if (d < best[9]) {
                int b = 9;
                while (b > 0 && d < best[b - 1]) { best[b] = best[b - 1]; ans[b] = ans[b - 1]; b--; }
                best[b] = d; ans[b] = lmem[k];
            }
        }
        if (is_ref) memcpy(ANS0[qi], ans, sizeof(ans));
        else if (same_set(ans, ANS0[qi])) stab++;
        rec += recall_at10(ans, GT + (size_t)qi * GTD);
    }
    *msq = (now_ms() - t0) / NQ;
    *nsc = ns;
    free(lmem);
    rec /= NQ;
    if (!is_ref) printf("  stability vs G0: %d/%d identical top-10 (%.1f%%)\n", stab, NQ, 100.0 * stab / NQ);
    return rec;
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

    PX = (float *)malloc((size_t)NBASE * 4);
    PY = (float *)malloc((size_t)NBASE * 4);
    for (int i = 0; i < NBASE; i++)
        pca2(BASE + (size_t)i * DIM, PX + i, PY + i);

    double pcts[3] = {0.50, 0.40, 0.60};
    const char *names[3] = {"G0 p50", "G1 p40", "G2 p60"};
    double recs[3], ams[3], msq[3];
    long long nsc[3];
    for (int g = 0; g < 3; g++) {
        printf("%s:\n", names[g]);
        recs[g] = run_grid(pcts[g], g == 0, &ams[g], &msq[g], &nsc[g]);
        printf("  recall=%.4f arise(assign-only,1M)=%.2fs scan=%.0f (%.3f%%) %.2fms/q\n",
               recs[g], ams[g] / 1000.0, (double)nsc[g] / NQ,
               (double)nsc[g] / NQ / NBASE * 100.0, msq[g]);
    }
    int hold = 1;
    for (int g = 1; g < 3; g++) {
        double d = recs[g] - recs[0]; if (d < 0) d = -d;
        if (d > 0.03) hold = 0;
        if (ams[g] > 2000.0) hold = 0;
    }
    printf("GATE: field-property %s (recall drift vs G0 within 0.03 %s, arise<2s %s)\n",
           hold ? "HOLDS" : "BREAKS", hold ? "yes" : "no", hold ? "yes" : "no");
    return 0;
}
