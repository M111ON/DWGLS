/* tools/probe_multientry.c — multi-entry (top-2) overlap probe.
 *
 * IDEA: boundary vectors belong in BOTH adjacent buckets. Assign every base
 * vector to its top-2 nearest used fine buckets; route queries at half the
 * bucket budget to match scan cost, test if the hard-bucket wall softens.
 *
 * CHOICE DOCUMENTED: exact brute force over 2560 used fine centroids per
 * base vector (1M*2560*25), no coarse shortlist — exact, ~64B FLOP.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I. -o build/probe_multientry.exe tools/probe_multientry.c -lm
 * RUN:   build/probe_multientry.exe   (fixed n=1000, fixed pairs, top-2 only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define DIM 128
#define PDIM 25
#define C1N 256
#define NB 4096
#define NUSED 2560
#define NQ 1000

static float *FC, *PC, *PM, *BASE, *QUERY;
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
static double cdist(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < PDIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}

/* top-C used-idx (0..NUSED) nearest to z by centroid dist; dist work array provided */
static void topC_used(const float *z, int C, int *out, double *dw) {
    for (int i = 0; i < NUSED; i++)
        dw[i] = cdist(z, FC + (size_t)AID[i] * PDIM);
    for (int c = 0; c < C; c++) {
        int bi = c;
        for (int i = c + 1; i < NUSED; i++)
            if (dw[i] < dw[bi]) bi = i;
        double td = dw[c]; dw[c] = dw[bi]; dw[bi] = td;
        out[c] = bi;
    }
}

/* exact 128-dim rerank of ids[0..ns) vs query q -> top-10 ids in besi */
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

    /* 1. top-2 assignment (exact brute force) + double posting lists */
    t0 = now_ms();
    int32_t *a1 = (int32_t *)malloc((size_t)NBASE * 4);
    int32_t *a2 = (int32_t *)malloc((size_t)NBASE * 4);
    int64_t *cnt = (int64_t *)calloc(NUSED, 8);
    float *z = (float *)malloc(PDIM * 4);
    for (int i = 0; i < NBASE; i++) {
        project(BASE + (size_t)i * DIM, z);
        double bd1 = 1e300, bd2 = 1e300;
        int t1 = -1, t2 = -1;
        for (int u = 0; u < NUSED; u++) {
            double d = cdist(z, FC + (size_t)AID[u] * PDIM);
            if (d < bd1) { bd2 = bd1; t2 = t1; bd1 = d; t1 = u; }
            else if (d < bd2) { bd2 = d; t2 = u; }
        }
        a1[i] = t1; a2[i] = t2;
        cnt[t1]++; cnt[t2]++;
    }
    int64_t *off2 = (int64_t *)malloc(((size_t)NUSED + 1) * 8);
    off2[0] = 0;
    for (int u = 0; u < NUSED; u++) off2[u + 1] = off2[u] + cnt[u];
    int64_t nent2 = off2[NUSED];
    int32_t *mem2 = (int32_t *)malloc((size_t)nent2 * 4);
    int64_t *fill = (int64_t *)malloc((size_t)NUSED * 8);
    memcpy(fill, off2, (size_t)NUSED * 8);
    for (int i = 0; i < NBASE; i++) {
        mem2[fill[a1[i]]++] = i;
        mem2[fill[a2[i]]++] = i;
    }
    free(fill);
    double build_s = (now_ms() - t0) / 1000.0;
    double mean_single = (double)NBASE / NUSED;
    double mean_double = (double)nent2 / NUSED;
    printf("SANITY: mean bucket single=%.2f double=%.2f entries single=1000000 double=%ld build=%.1fs\n",
           mean_single, mean_double, (long)nent2, build_s);

    /* 2. CEILING: GT top-10 bucket spread under SINGLE assignment */
    int h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    for (int qi = 0; qi < NQ; qi++) {
        const int32_t *gt = GT + (size_t)qi * GTD;
        int seen_b[10], nsb = 0;
        for (int k = 0; k < 10; k++) {
            int b = a1[gt[k]];
            int f = 0;
            for (int s = 0; s < nsb; s++) if (seen_b[s] == b) { f = 1; break; }
            if (!f) seen_b[nsb++] = b;
        }
        if (nsb == 1) h1++;
        else if (nsb == 2) h2++;
        else if (nsb == 3) h3++;
        else h4++;
    }
    printf("CEILING (n=%d, single assign, GT top-10 bucket spread): 1bkt=%d (%.1f%%) 2bkt=%d (%.1f%%) 3bkt=%d (%.1f%%) 4+bkt=%d (%.1f%%)\n",
           NQ, h1, 100.0 * h1 / NQ, h2, 100.0 * h2 / NQ, h3, 100.0 * h3 / NQ, h4, 100.0 * h4 / NQ);

    /* 3. recall pairs */
    int *scan = (int *)malloc((size_t)NBASE * 4);
    int32_t *stamp = (int32_t *)calloc(NBASE, 4);
    int32_t cur = 1;
    double *dw = (double *)malloc((size_t)NUSED * 8);
    int *topu = (int *)malloc(64 * sizeof(int));
    int besi[10];
    int pairsD[3] = { 8, 16, 32 }, pairsS[3] = { 16, 32, 64 };
    float *zq = (float *)malloc(PDIM * 4);
    double drec[3] = {0}, srec[3] = {0}, dscan[3] = {0}, sscan[3] = {0}, dms[3] = {0}, sms[3] = {0};

    for (int p = 0; p < 3; p++) {
        /* DOUBLE at top-C/2 */
        double tq = now_ms();
        long long nscan = 0;
        double rec = 0;
        int C = pairsD[p];
        for (int qi = 0; qi < NQ; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            project(q, zq);
            topC_used(zq, C, topu, dw);
            int ns = 0;
            cur++;
            for (int c = 0; c < C; c++) {
                int u = topu[c];
                for (int64_t k = off2[u]; k < off2[u + 1]; k++) {
                    int id = mem2[k];
                    if (stamp[id] != cur) { stamp[id] = cur; scan[ns++] = id; }
                }
            }
            rerank10(q, scan, ns, besi);
            nscan += ns;
            const int32_t *gt = GT + (size_t)qi * GTD;
            int hit = 0;
            for (int i = 0; i < 10; i++)
                for (int j = 0; j < 10; j++)
                    if (besi[i] == gt[j]) { hit++; break; }
            rec += hit / 10.0;
        }
        dms[p] = (now_ms() - tq) / NQ;
        drec[p] = rec / NQ; dscan[p] = (double)nscan / NQ;
        /* SINGLE at top-C */
        tq = now_ms();
        nscan = 0; rec = 0;
        C = pairsS[p];
        for (int qi = 0; qi < NQ; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            project(q, zq);
            topC_used(zq, C, topu, dw);
            int ns = 0;
            for (int c = 0; c < C; c++) {
                int b = AID[topu[c]];
                for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) scan[ns++] = MEM[k];
            }
            rerank10(q, scan, ns, besi);
            nscan += ns;
            const int32_t *gt = GT + (size_t)qi * GTD;
            int hit = 0;
            for (int i = 0; i < 10; i++)
                for (int j = 0; j < 10; j++)
                    if (besi[i] == gt[j]) { hit++; break; }
            rec += hit / 10.0;
        }
        sms[p] = (now_ms() - tq) / NQ;
        srec[p] = rec / NQ; sscan[p] = (double)nscan / NQ;
    }
    printf("PAIRS (n=%d, exact 128-dim rerank, recall@10 vs GT top-10):\n", NQ);
    for (int p = 0; p < 3; p++)
        printf(" pair%d: DOUBLE top%-2d recall=%.4f scan=%.0f (%.3f%%) %.2fms/q | SINGLE top%-2d recall=%.4f scan=%.0f (%.3f%%) %.2fms/q | d_recall-s_recall=%+.4f\n",
               p + 1, pairsD[p], drec[p], dscan[p], dscan[p] / NBASE * 100.0, dms[p],
               pairsS[p], srec[p], sscan[p], sscan[p] / NBASE * 100.0, sms[p], drec[p] - srec[p]);

    long bytes_single = (long)1000000 * 4 + (long)(NB + 1) * 8;
    long bytes_double = (long)nent2 * 4 + (long)(NUSED + 1) * 8;
    printf("BYTES: single postings+off=%ld (4000000+32776) double postings+off=%ld build=%.1fs\n",
           bytes_single, bytes_double, build_s);
    int wins = 0;
    for (int p = 0; p < 3; p++) if (drec[p] >= srec[p]) wins++;
    printf("GATE: overlap %s (%d/3 pairs double>=single at matched scan)\n",
           wins >= 2 ? "WINS" : "LOSES", wins);
    return 0;
}
