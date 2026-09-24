/* tools/anchor_route_cli.c — C port of the SIFT-proven anchor-bucket router.
 *
 * Pipeline per query: PCA project (128→25, matrix from sklearn) → route
 * top-a coarse (256) → top-b fine (pooled) → exact 128-dim L2 rank of
 * scanned members → top-10. Reports TRUE recall@10 vs official GT +
 * scan% + ms/q + distance computations. Artifacts: build/sift1m_c/.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/anchor_route_cli.exe tools/anchor_route_cli.c -lm
 * RUN:   ./build/anchor_route_cli [nq] [a,b pairs...]
 *        default: nq=1000, pairs 4x8 4x16 8x16 8x32 4x32
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#include "anchor_route.h"

#define DIM 128
#define PDIM 25
#define C1N 256
#define BK 16
#define NB (C1N * BK)

static float *C1, *PC, *PM, *FC;
static int64_t *OFF;
static int32_t *MEM;
static float *BASE, *QUERY;
static int32_t *GT;
static int NBASE, NQRY;

/* fvecs: [dim:int32][dim floats]... ; ivecs likewise with int32 */
static void *load_vecs(const char *path, int *n_out, int *d_out, int is_float) {
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
    void *out = malloc((size_t)n * d * (is_float ? 4 : 4));
    for (int i = 0; i < n; i++) {
        memcpy((char *)out + (size_t)i * d * 4, raw + (size_t)i * stride + 4, (size_t)d * 4);
        int32_t dd = *(int32_t *)(raw + (size_t)i * stride);
        if (dd != d) { fprintf(stderr, "ragged row %d\n", i); exit(1); }
    }
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
static int64_t *load_bin64(const char *path, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    int64_t *p = (int64_t *)malloc(n * 8);
    if (!p || fread(p, 8, n, f) != n) { fprintf(stderr, "read fail %s\n", path); exit(1); }
    fclose(f);
    return p;
}
static int32_t *load_bin32(const char *path, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    int32_t *p = (int32_t *)malloc(n * 4);
    if (!p || fread(p, 4, n, f) != n) { fprintf(stderr, "read fail %s\n", path); exit(1); }
    fclose(f);
    return p;
}

static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

int main(int argc, char **argv) {
    const char *D = "build/sift1m/sift/";
    const char *H = "build/sift1m_c/";
    char pb[256], qb[256], gb[256];
    snprintf(pb, sizeof(pb), "%ssift_base.fvecs", D);
    snprintf(qb, sizeof(qb), "%ssift_query.fvecs", D);
    snprintf(gb, sizeof(gb), "%ssift_groundtruth.ivecs", D);
    int dn = 0;
    double t0 = now_ms();
    BASE = (float *)load_vecs(pb, &NBASE, &dn, 1);
    QUERY = (float *)load_vecs(qb, &NQRY, &dn, 1);
    int gd = 0, gn = 0;
    GT = (int32_t *)load_vecs(gb, &gn, &gd, 0);
    C1 = load_bin("build/sift1m_c/C1.bin", (size_t)C1N * PDIM);
    PC = load_bin("build/sift1m_c/pca_comp.bin", (size_t)PDIM * DIM);
    PM = load_bin("build/sift1m_c/pca_mean.bin", DIM);
    FC = load_bin("build/sift1m_c/F.bin", (size_t)NB * PDIM);
    OFF = load_bin64("build/sift1m_c/off.bin", NB + 1);
    MEM = load_bin32("build/sift1m_c/mem.bin", 1000000);
    (void)H;
    printf("load: base %d query %d gt %d (dim %d/%d) in %.1fs\n",
           NBASE, NQRY, gn, dn, gd, (now_ms() - t0) / 1000.0);
    size_t idx_mem = ((size_t)C1N * PDIM + (size_t)PDIM * DIM + DIM + (size_t)NB * PDIM) * 4
                   + (size_t)(NB + 1) * 8 + (size_t)1000000 * 4;
    printf("index memory: %.2f MB (centroids+pca+buckets, excl. base vectors)\n", idx_mem / 1048576.0);

    int nq = argc > 1 ? atoi(argv[1]) : 1000;
    if (nq > NQRY) nq = NQRY;
    /* pairs from argv or default */
    int pairs[16][2], npair = 0;
    if (argc > 2) {
        for (int i = 2; i < argc && npair < 16; i++) {
            int a = 0, b = 0;
            if (sscanf(argv[i], "%dx%d", &a, &b) == 2) { pairs[npair][0] = a; pairs[npair][1] = b; npair++; }
        }
    }
    if (!npair) {
        int dflt[][2] = { {4,8},{4,16},{8,16},{8,32},{4,32} };
        for (int i = 0; i < 5; i++) { pairs[i][0] = dflt[i][0]; pairs[i][1] = dflt[i][1]; }
        npair = 5;
    }

    float *zq = (float *)malloc(PDIM * 4);
    float *ftmp = (float *)malloc((size_t)C1N * BK * PDIM * 4);
    int *idmap = (int *)malloc((size_t)C1N * BK * sizeof(int));
    int *topc = (int *)malloc(C1N * sizeof(int));
    int *topf = (int *)malloc(C1N * BK * sizeof(int));
    int *scan = (int *)malloc(1000000 * sizeof(int));

    for (int pi = 0; pi < npair; pi++) {
        int A = pairs[pi][0], B = pairs[pi][1];
        double recall = 0;
        long long nscan = 0, ndist = 0;
        double tq0 = now_ms();
        for (int qi = 0; qi < nq; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            /* project */
            for (int i = 0; i < PDIM; i++) {
                double s = 0;
                for (int j = 0; j < DIM; j++) s += (double)PC[(size_t)i * DIM + j] * (q[j] - PM[j]);
                zq[i] = (float)s;
            }
            /* coarse route */
            int na = anch_route(zq, C1, C1N, PDIM, A, topc);
            ndist += C1N;
            /* pool fine centroids of top-a coarse */
            int nf = 0;
            for (int i = 0; i < na; i++)
                for (int j = 0; j < BK; j++) {
                    int b = topc[i] * BK + j;
                    if (OFF[b + 1] == OFF[b]) continue; /* empty bucket */
                    memcpy(ftmp + (size_t)nf * PDIM, FC + (size_t)b * PDIM, PDIM * 4);
                    idmap[nf++] = b;
                }
            int nb = anch_route(zq, ftmp, nf, PDIM, B < nf ? B : nf, topf);
            ndist += nf;
            /* gather members */
            int ns = 0;
            for (int i = 0; i < nb; i++) {
                int b = idmap[topf[i]];
                for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) scan[ns++] = MEM[k];
            }
            /* exact 128-dim rank, top-10 */
            double best[10];
            int besi[10];
            for (int i = 0; i < 10; i++) { best[i] = 1e300; besi[i] = -1; }
            for (int i = 0; i < ns; i++) {
                const float *v = BASE + (size_t)scan[i] * DIM;
                double d = 0;
                for (int j = 0; j < DIM; j++) { double e = (double)q[j] - v[j]; d += e * e; }
                ndist++;
                if (d < best[9]) {
                    int p = 9;
                    while (p > 0 && d < best[p - 1]) { best[p] = best[p - 1]; besi[p] = besi[p - 1]; p--; }
                    best[p] = d; besi[p] = scan[i];
                }
            }
            nscan += ns;
            const int32_t *gt = GT + (size_t)qi * gd;
            int hit = 0;
            for (int i = 0; i < 10; i++)
                for (int j = 0; j < 10; j++)
                    if (besi[i] == gt[j]) { hit++; break; }
            recall += hit / 10.0;
        }
        double ms = (now_ms() - tq0) / nq;
        printf("C topC=%d topF=%d: recall@10=%.4f scan=%.0f (%.3f%%) %.2fms/q dists=%.0f/q\n",
               A, B, recall / nq, (double)nscan / nq, (double)nscan / nq / NBASE * 100.0,
               ms, (double)ndist / nq);
    }
    return 0;
}
