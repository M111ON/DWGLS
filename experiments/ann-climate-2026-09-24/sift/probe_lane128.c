/* experiments/ann-climate-2026-09-24/sift/probe_lane128.c — native 128-lane assign probe.
 *
 * IDEA (owner 2026-09-25): SIFT vectors are 128-dim = Lane A width exactly
 * (128x162 = 144x144 = 20736: one field cell holds 162 raw SIFT vectors with
 * zero projection). Can we drop the PCA25 projection and assign natively in
 * 128-dim? Lift the 25-dim fine centroids back to 128-dim once
 * (x_hat = mean + sum z_i * PC_row_i), then assign queries directly in 128-dim.
 *
 * MATH (falsifiable): ||q-x_hat||^2 = ||z_q-z_c||^2 + ||r||^2 where r is the
 * query's out-of-subspace residual, CONSTANT across centroids (cross term is
 * zero: r is orthogonal to range(P^T)). So 128-dim ranking MUST equal 25-dim
 * ranking exactly. Any flip = our geometry understanding is wrong (float noise
 * aside). Expect: recall identical, assign ~5x slower (2560x128 vs 2560x25),
 * centroid bytes 5x (1.25MB vs 256KB).
 *
 * GATE: PASS (adopt native) iff recall equal within +-0.001 AND 128-dim assign
 * ms/q <= PCA25 path. Predicted FAIL on cost -> PCA25 stays as the compressed
 * assign path; 128-lane native valid as STORAGE placement only, not metric.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_lane128.exe experiments/ann-climate-2026-09-24/sift/probe_lane128.c -lm
 * RUN (repo root): build/probe_lane128.exe   (fixed n=1000, top-16 single arm)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define DIM 128
#define PDIM 25
#define NB 4096
#define NUSED 2560
#define NQ 1000
#define TOPC 16

static float *FC, *PC, *PM, *BASE, *QUERY, *LC;
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
static double cdist25(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < PDIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}
static double cdist128(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < DIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}
/* top-TOPC used-idx nearest to z (25-dim centroids); work array provided */
static void topC_used25(const float *z, int *out, double *dw) {
    for (int i = 0; i < NUSED; i++)
        dw[i] = cdist25(z, FC + (size_t)AID[i] * PDIM);
    for (int c = 0; c < TOPC; c++) {
        int bi = c;
        for (int i = c + 1; i < NUSED; i++)
            if (dw[i] < dw[bi]) bi = i;
        double td = dw[c]; dw[c] = dw[bi]; dw[bi] = td;
        out[c] = bi;
    }
}
/* top-TOPC used-idx nearest to q (128-dim lifted centroids) */
static void topC_used128(const float *q, int *out, double *dw) {
    for (int i = 0; i < NUSED; i++)
        dw[i] = cdist128(q, LC + (size_t)i * DIM);
    for (int c = 0; c < TOPC; c++) {
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
static double recall_at10(const int *besi, const int32_t *gt) {
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (besi[i] == gt[j]) { hit++; break; }
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

    /* LIFT: 25-dim used centroids -> 128-dim (x_hat = mean + P^T z), one-time */
    t0 = now_ms();
    LC = (float *)malloc((size_t)NUSED * DIM * 4);
    for (int u = 0; u < NUSED; u++) {
        const float *z = FC + (size_t)AID[u] * PDIM;
        float *x = LC + (size_t)u * DIM;
        for (int j = 0; j < DIM; j++) {
            double s = PM[j];
            for (int i = 0; i < PDIM; i++) s += (double)z[i] * PC[(size_t)i * DIM + j];
            x[j] = (float)s;
        }
    }
    /* SANITY: lifted centroid must re-project to itself (P x_hat == z) */
    double maxrej = 0;
    float *zt = (float *)malloc(PDIM * 4);
    for (int u = 0; u < NUSED; u++) {
        project(LC + (size_t)u * DIM, zt);
        const float *z = FC + (size_t)AID[u] * PDIM;
        for (int i = 0; i < PDIM; i++) {
            double e = zt[i] - z[i] < 0 ? z[i] - zt[i] : zt[i] - z[i];
            if (e > maxrej) maxrej = e;
        }
    }
    free(zt);
    printf("LIFT: %d centroids 25->128 in %.2fs, max reproject err=%.3g\n",
           NUSED, (now_ms() - t0) / 1000.0, maxrej);

    /* IDENTITY CHECK + recall arms at top-16 */
    int *scan = (int *)malloc((size_t)NBASE * 4);
    int32_t *stamp = (int32_t *)calloc(NBASE, 4);
    int32_t cur = 1;
    double *dw = (double *)malloc((size_t)NUSED * 8);
    int topA[TOPC], topB[TOPC], besi[10];
    float *zq = (float *)malloc(PDIM * 4);
    int flips = 0;
    long long nscanA = 0, nscanB = 0;
    double recA = 0, recB = 0;

    double tq = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        const float *q = QUERY + (size_t)qi * DIM;
        project(q, zq);
        topC_used25(zq, topA, dw);
        int ns = 0;
        cur++;
        for (int c = 0; c < TOPC; c++) {
            int b = AID[topA[c]];
            for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) {
                int id = MEM[k];
                if (stamp[id] != cur) { stamp[id] = cur; scan[ns++] = id; }
            }
        }
        rerank10(q, scan, ns, besi);
        nscanA += ns;
        recA += recall_at10(besi, GT + (size_t)qi * GTD);
    }
    double msA = (now_ms() - tq) / NQ;

    tq = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        const float *q = QUERY + (size_t)qi * DIM;
        project(q, zq);
        topC_used25(zq, topA, dw);
        topC_used128(q, topB, dw);
        /* flip = top-16 SET mismatch (order-insensitive) */
        int same = 1;
        for (int c = 0; c < TOPC && same; c++) {
            int f = 0;
            for (int k = 0; k < TOPC; k++) if (topB[k] == topA[c]) { f = 1; break; }
            if (!f) same = 0;
        }
        if (!same) flips++;
        int ns = 0;
        cur++;
        for (int c = 0; c < TOPC; c++) {
            int b = AID[topB[c]];
            for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) {
                int id = MEM[k];
                if (stamp[id] != cur) { stamp[id] = cur; scan[ns++] = id; }
            }
        }
        rerank10(q, scan, ns, besi);
        nscanB += ns;
        recB += recall_at10(besi, GT + (size_t)qi * GTD);
    }
    double msB = (now_ms() - tq) / NQ;
    recA /= NQ; recB /= NQ;

    printf("ARMS (n=%d, top-%d, exact 128-dim rerank, recall@10):\n", NQ, TOPC);
    printf(" A PCA25 : recall=%.4f scan=%.0f (%.3f%%) %.2fms/q\n",
           recA, (double)nscanA / NQ, (double)nscanA / NQ / NBASE * 100.0, msA);
    printf(" B NATIVE: recall=%.4f scan=%.0f (%.3f%%) %.2fms/q\n",
           recB, (double)nscanB / NQ, (double)nscanB / NQ / NBASE * 100.0, msB);
    printf("IDENTITY: top-%d set flips %d/%d (%.2f%%), recall delta=%+.4f\n",
           TOPC, flips, NQ, 100.0 * flips / NQ, recB - recA);
    long bytesA = (long)NUSED * PDIM * 4 + (long)PDIM * DIM * 4 + DIM * 4;
    long bytesB = (long)NUSED * DIM * 4;
    printf("BYTES (assign-time resident): A centroids+PC+mean=%ld B lifted=%ld (x%.1f)\n",
           bytesA, bytesB, (double)bytesB / bytesA);
    int pass = (recB - recA > -0.001) && (msB <= msA);
    printf("GATE: native-128 %s (recall>=A-0.001 %s, ms<=A %s)\n",
           pass ? "WINS" : "LOSES",
           (recB - recA > -0.001) ? "yes" : "no",
           msB <= msA ? "yes" : "no");
    return 0;
}
