/* tools/probe_coherence.c — route-coherence probe (owner idea: near-duplicate
 * queries share one route prefix, diverging only at the last segment).
 *
 * METHOD (n=1000, queries 0..999): exact-128-dim nearest-neighbor query p per q;
 * top-C fine buckets via coarse top-8 -> fine rank (copied from
 * tools/anchor_route_cli.c lines 141-165, NOT imported); Jaccard(q,p) per C;
 * prefix-cache simulation (p's top-32 pool scored by q) vs own top-32 recall.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I. -o build/probe_coherence.exe tools/probe_coherence.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#include "core/anchor_route.h"

#define DIM 128
#define PDIM 25
#define C1N 256
#define BK 16
#define NB (C1N * BK)
#define NQ 1000

static float *C1, *PC, *PM, *FC;
static int64_t *OFF;
static int32_t *MEM;
static float *BASE, *QUERY;
static int32_t *GT;
static int NBASE, NQRY, GTD;

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
    if (!out) { fprintf(stderr, "oom %s\n", path); exit(1); }
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

static void project(const float *q, float *z) {
    for (int i = 0; i < PDIM; i++) {
        double s = 0;
        for (int j = 0; j < DIM; j++) s += (double)PC[(size_t)i * DIM + j] * (q[j] - PM[j]);
        z[i] = (float)s;
    }
}

/* coarse top-8 -> pool fine centroids (skip empty) -> fine rank top-C.
 * Returns #buckets written (= min(C, nf)). Copy of anchor_route_cli.c 147-158. */
static int select_buckets(const float *q, float *zq, float *ftmp, int *idmap,
                          int *topc, int *topf, int C, int *out) {
    project(q, zq);
    int na = anch_route(zq, C1, C1N, PDIM, 8, topc);
    int nf = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < BK; j++) {
            int b = topc[i] * BK + j;
            if (OFF[b + 1] == OFF[b]) continue;
            memcpy(ftmp + (size_t)nf * PDIM, FC + (size_t)b * PDIM, PDIM * 4);
            idmap[nf++] = b;
        }
    if (nf == 0) return 0;
    int want = C < nf ? C : nf;
    int nb = anch_route(zq, ftmp, nf, PDIM, want, topf);
    for (int i = 0; i < nb; i++) out[i] = idmap[topf[i]];
    return nb;
}

static double jaccard(const int *a, int na, const int *b, int nb) {
    int inter = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++)
            if (a[i] == b[j]) { inter++; break; }
    int uni = na + nb - inter;
    return uni ? (double)inter / uni : 1.0;
}

/* exact 128-dim top-10 recall of q over member pool scan[0..ns) vs gt[0..10) */
static double recall_top10(const float *q, const int *scan, int ns, const int32_t *gt) {
    double best[10];
    int besi[10];
    for (int i = 0; i < 10; i++) { best[i] = 1e300; besi[i] = -1; }
    for (int i = 0; i < ns; i++) {
        const float *v = BASE + (size_t)scan[i] * DIM;
        double d = 0;
        for (int j = 0; j < DIM; j++) { double e = (double)q[j] - v[j]; d += e * e; }
        if (d < best[9]) {
            int p = 9;
            while (p > 0 && d < best[p - 1]) { best[p] = best[p - 1]; besi[p] = besi[p - 1]; p--; }
            best[p] = d; besi[p] = scan[i];
        }
    }
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (besi[i] == gt[j]) { hit++; break; }
    return hit / 10.0;
}

typedef struct { double d; double j32; } BinRow;
static int cmp_row(const void *a, const void *b) {
    double x = ((const BinRow *)a)->d, y = ((const BinRow *)b)->d;
    return (x > y) - (x < y);
}

int main(void) {
    double t0 = now_ms();
    int dn = 0, gd = 0, gn = 0;
    BASE = (float *)load_vecs("build/sift1m/sift/sift_base.fvecs", &NBASE, &dn);
    QUERY = (float *)load_vecs("build/sift1m/sift/sift_query.fvecs", &NQRY, &dn);
    GT = (int32_t *)load_vecs("build/sift1m/sift/sift_groundtruth.ivecs", &gn, &gd);
    GTD = gd;
    C1 = load_bin("build/sift1m_c/C1.bin", (size_t)C1N * PDIM);
    PC = load_bin("build/sift1m_c/pca_comp.bin", (size_t)PDIM * DIM);
    PM = load_bin("build/sift1m_c/pca_mean.bin", DIM);
    FC = load_bin("build/sift1m_c/F.bin", (size_t)NB * PDIM);
    {
        FILE *f = fopen("build/sift1m_c/off.bin", "rb");
        if (!f) { fprintf(stderr, "missing off.bin\n"); return 1; }
        OFF = (int64_t *)malloc(((size_t)NB + 1) * 8);
        if (fread(OFF, 8, NB + 1, f) != (size_t)NB + 1) { fprintf(stderr, "off read\n"); return 1; }
        fclose(f);
    }
    MEM = (int32_t *)load_bin("build/sift1m_c/mem.bin", 1000000);
    printf("load: base %d query %d gt-dim %d in %.1fs\n", NBASE, NQRY, GTD, (now_ms() - t0) / 1000.0);

    /* 1. nearest-neighbor query per q (exact 128-dim L2 over 10k, excl. self) */
    static int pidx[NQ];
    static double pdist2[NQ]; /* squared L2 to neighbor */
    t0 = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        const float *q = QUERY + (size_t)qi * DIM;
        double bd = 1e300;
        int bi = -1;
        for (int cj = 0; cj < NQRY; cj++) {
            if (cj == qi) continue;
            const float *v = QUERY + (size_t)cj * DIM;
            double d = 0;
            for (int j = 0; j < DIM; j++) { double e = (double)q[j] - v[j]; d += e * e; }
            if (d < bd) { bd = d; bi = cj; }
        }
        pidx[qi] = bi; pdist2[qi] = bd;
    }
    printf("nn-search: %.1fs\n", (now_ms() - t0) / 1000.0);
    double mean_d = 0;
    for (int qi = 0; qi < NQ; qi++) mean_d += sqrt(pdist2[qi]);
    mean_d /= NQ;
    printf("mean neighbor L2 distance: %.2f\n", mean_d);

    /* 2. top-64 buckets for q and p; Jaccard at 16/32/64 via prefix */
    float *zq = (float *)malloc(PDIM * 4);
    float *ftmp = (float *)malloc((size_t)C1N * BK * PDIM * 4);
    int *idmap = (int *)malloc((size_t)C1N * BK * sizeof(int));
    int *topc = (int *)malloc(C1N * sizeof(int));
    int *topf = (int *)malloc((size_t)C1N * BK * sizeof(int));
    static int QB[NQ][64], PB[NQ][64];
    static int QN[NQ], PN[NQ];
    static double J32[NQ];
    double sj16 = 0, sj32 = 0, sj64 = 0;
    t0 = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        QN[qi] = select_buckets(QUERY + (size_t)qi * DIM, zq, ftmp, idmap, topc, topf, 64, QB[qi]);
        PN[qi] = select_buckets(QUERY + (size_t)pidx[qi] * DIM, zq, ftmp, idmap, topc, topf, 64, PB[qi]);
        int c16 = 16 < QN[qi] ? 16 : QN[qi], c16p = 16 < PN[qi] ? 16 : PN[qi];
        int c32 = 32 < QN[qi] ? 32 : QN[qi], c32p = 32 < PN[qi] ? 32 : PN[qi];
        sj16 += jaccard(QB[qi], c16, PB[qi], c16p);
        J32[qi] = jaccard(QB[qi], c32, PB[qi], c32p);
        sj32 += J32[qi];
        sj64 += jaccard(QB[qi], QN[qi], PB[qi], PN[qi]);
    }
    printf("bucket-select: %.1fs\n", (now_ms() - t0) / 1000.0);
    printf("Jaccard C=16: %.4f  C=32: %.4f  C=64: %.4f\n", sj16 / NQ, sj32 / NQ, sj64 / NQ);

    /* distance curve: equal-count quintiles, mean Jaccard@32 per bin */
    static BinRow rows[NQ];
    for (int qi = 0; qi < NQ; qi++) { rows[qi].d = sqrt(pdist2[qi]); rows[qi].j32 = J32[qi]; }
    qsort(rows, NQ, sizeof(BinRow), cmp_row);
    printf("curve (quintile bins by neighbor L2, mean Jacc32):\n");
    for (int b = 0; b < 5; b++) {
        double sj = 0;
        for (int i = b * 200; i < (b + 1) * 200; i++) sj += rows[i].j32;
        printf("  bin%d L2[%.1f,%.1f]: %.4f\n", b, rows[b * 200].d, rows[(b + 1) * 200 - 1].d, sj / 200.0);
    }

    /* 3. prefix-cache simulation: p's top-32 pool scored by q vs own top-32 */
    int *scan = (int *)malloc(1000000 * sizeof(int));
    double rcross = 0, rown = 0, rsize = 0;
    t0 = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        const float *q = QUERY + (size_t)qi * DIM;
        const int32_t *gt = GT + (size_t)qi * GTD;
        int c32 = 32 < QN[qi] ? 32 : QN[qi], c32p = 32 < PN[qi] ? 32 : PN[qi];
        int ns = 0;
        for (int i = 0; i < c32p; i++) {
            int b = PB[qi][i];
            for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) scan[ns++] = MEM[k];
        }
        int nscross = ns;
        rcross += recall_top10(q, scan, ns, gt);
        ns = 0;
        for (int i = 0; i < c32; i++) {
            int b = QB[qi][i];
            for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) scan[ns++] = MEM[k];
        }
        rown += recall_top10(q, scan, ns, gt);
        rsize += ns ? (double)nscross / ns : 1.0;
    }
    double mspq = (now_ms() - t0) / NQ;
    printf("cross-recall@10 (neighbor pool): %.4f\n", rcross / NQ);
    printf("own-recall@10   (own top-32)   : %.4f\n", rown / NQ);
    printf("pool-size ratio cross/own: %.4f\n", rsize / NQ);
    printf("simulation: %.2f ms/q\n", mspq);

    /* GATE */
    double mj32 = sj32 / NQ, mcr = rcross / NQ, mor = rown / NQ;
    printf("GATE: Jacc32=%.4f (>0.5? %s) cross-own=%.4f (>= -0.05? %s) => %s\n",
           mj32, mj32 > 0.5 ? "yes" : "no",
           mcr - mor, (mcr >= mor - 0.05) ? "yes" : "no",
           (mj32 > 0.5 && mcr >= mor - 0.05) ? "CONFIRMED" : "WEAK");
    return 0;
}
