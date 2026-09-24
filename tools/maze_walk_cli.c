/* tools/maze_walk_cli.c — greedy descent over the anchor kNN graph.
 *
 * Graph: 2560 fine anchors (25-dim centroids), each linked to its 8
 * nearest anchors (bidirectional, built at load, deterministic). Walk per
 * query: project → seed = nearest fine of top-1 coarse → hill-climb to
 * the neighbor closest to the query until local min or bucket budget
 * spent → exact 128-dim rank of visited-bucket members → top-10.
 * Reports TRUE recall@10 vs GT + scan% + hops + ms/q at matched budgets
 * against flat topC/topF routing (anchor_route_cli).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/maze_walk_cli tools/maze_walk_cli.c -lm
 * RUN:   ./build/maze_walk_cli [nq] [budgets...]   (default 1000, budgets 8 16 32)
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
#define NUSED 2560
#define KNN 8

static float *C1, *PC, *PM, *FC;
static int64_t *OFF;
static int32_t *MEM;
static float *BASE, *QUERY;
static int32_t *GT;
static int NBASE, NQRY;
static int KNBR[NUSED][KNN];   /* neighbor anchor indices (0..NUSED) */
static int AID[NUSED];         /* NUSED-th used bucket -> global bucket id */

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

static double cdist(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < PDIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}

int main(int argc, char **argv) {
    const char *D = "build/sift1m/sift/";
    char pb[256], qb[256], gb[256];
    snprintf(pb, sizeof(pb), "%ssift_base.fvecs", D);
    snprintf(qb, sizeof(qb), "%ssift_query.fvecs", D);
    snprintf(gb, sizeof(gb), "%ssift_groundtruth.ivecs", D);
    int dn = 0, gd = 0, gn = 0;
    double t0 = now_ms();
    BASE = (float *)load_vecs(pb, &NBASE, &dn, 1);
    QUERY = (float *)load_vecs(qb, &NQRY, &dn, 1);
    GT = (int32_t *)load_vecs(gb, &gn, &gd, 0);
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
    /* used-bucket list */
    int nu = 0;
    for (int b = 0; b < NB && nu < NUSED; b++)
        if (OFF[b + 1] > OFF[b]) AID[nu++] = b;
    printf("load: base %d query %d used-buckets %d in %.1fs\n", NBASE, NQRY, nu, (now_ms() - t0) / 1000.0);
    if (nu != NUSED) { fprintf(stderr, "used count %d != %d\n", nu, NUSED); return 1; }

    /* kNN graph over used anchors (centroid L2, bidirectional) */
    t0 = now_ms();
    static uint8_t adj[NUSED][NUSED];
    for (int i = 0; i < NUSED; i++) {
        /* partial select KNN nearest (exclude self) */
        double bd[KNN];
        int bi[KNN];
        for (int k = 0; k < KNN; k++) { bd[k] = 1e300; bi[k] = -1; }
        const float *ci = FC + (size_t)AID[i] * PDIM;
        for (int j = 0; j < NUSED; j++) {
            if (j == i) continue;
            double d = cdist(ci, FC + (size_t)AID[j] * PDIM);
            if (d < bd[KNN - 1]) {
                int p = KNN - 1;
                while (p > 0 && d < bd[p - 1]) { bd[p] = bd[p - 1]; bi[p] = bi[p - 1]; p--; }
                bd[p] = d; bi[p] = j;
            }
        }
        for (int k = 0; k < KNN; k++) adj[i][bi[k]] = 1;
    }
    /* symmetrize + fill rows to KNN */
    for (int i = 0; i < NUSED; i++)
        for (int j = 0; j < NUSED; j++)
            if (adj[i][j]) adj[j][i] = 1;
    int maxdeg = 0;
    for (int i = 0; i < NUSED; i++) {
        int c = 0;
        for (int j = 0; j < NUSED && c < KNN; j++)
            if (adj[i][j]) KNBR[i][c++] = j;
        /* top up with nearest (may duplicate if degree < KNN — count real) */
        for (int j = 0; j < NUSED && c < KNN; j++)
            if (j != i && !adj[i][j]) { KNBR[i][c++] = j; break; }
        if (c > maxdeg) maxdeg = c;
    }
    printf("graph: kNN=%d maxdeg-used=%d built in %.1fs\n", KNN, maxdeg, (now_ms() - t0) / 1000.0);

    int nq = argc > 1 ? atoi(argv[1]) : 1000;
    if (nq > NQRY) nq = NQRY;
    int budgets[8] = { 8, 16, 32, 0, 0, 0, 0, 0 };
    int nbud = 3;
    if (argc > 2) {
        nbud = 0;
        for (int i = 2; i < argc && nbud < 8; i++) budgets[nbud++] = atoi(argv[i]);
    }

    float *zq = (float *)malloc(PDIM * 4);
    int *topc = (int *)malloc(C1N * sizeof(int));
    int *scan = (int *)malloc(1000000 * sizeof(int));
    uint8_t *seen = (uint8_t *)malloc(NUSED);

    for (int bi = 0; bi < nbud; bi++) {
        int BUD = budgets[bi];
        double recall = 0;
        long long nscan = 0, nhops = 0;
        double tq0 = now_ms();
        for (int qi = 0; qi < nq; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            for (int i = 0; i < PDIM; i++) {
                double s = 0;
                for (int j = 0; j < DIM; j++) s += (double)PC[(size_t)i * DIM + j] * (q[j] - PM[j]);
                zq[i] = (float)s;
            }
            /* seed: nearest fine of top-2 coarse (diversity) */
            int na = anch_route(zq, C1, C1N, PDIM, 2, topc);
            (void)na;
            int seeds[2] = { -1, -1 };
            for (int s = 0; s < 2; s++) {
                int seedb = topc[s] * BK;
                double bestd = 1e300;
                for (int j = 0; j < BK; j++) {
                    int b = seedb + j;
                    if (OFF[b + 1] == OFF[b]) continue;
                    double d = cdist(zq, FC + (size_t)b * PDIM);
                    if (d < bestd) { bestd = d; seeds[s] = b; }
                }
            }
            int seedidx[2] = { -1, -1 };
            for (int s = 0; s < 2; s++) {
                if (seeds[s] < 0) continue;
                for (int i = 0; i < NUSED; i++)
                    if (AID[i] == seeds[s]) { seedidx[s] = i; break; }
            }
            /* best-first (beam) expansion over the graph, budget BUD buckets */
            memset(seen, 0, NUSED);
            int visited[64];
            double vdist[64];
            int nv = 0, hops = 0;
            for (int s = 0; s < 2 && nv < BUD; s++) {
                if (seedidx[s] < 0 || seen[seedidx[s]]) continue;
                seen[seedidx[s]] = 1;
                visited[nv] = seedidx[s];
                vdist[nv] = cdist(zq, FC + (size_t)AID[seedidx[s]] * PDIM);
                nv++;
            }
            uint8_t expanded[2560];
            memset(expanded, 0, NUSED);
            while (nv < BUD) {
                /* closest visited-but-unexpanded */
                int bi2 = -1;
                double bd2 = 1e300;
                for (int i = 0; i < nv; i++)
                    if (!expanded[visited[i]] && vdist[i] < bd2) { bd2 = vdist[i]; bi2 = i; }
                if (bi2 < 0) break;
                expanded[visited[bi2]] = 1;
                hops++;
                int added = 0;
                for (int k = 0; k < KNN && nv < BUD; k++) {
                    int nb2 = KNBR[visited[bi2]][k];
                    if (nb2 < 0 || nb2 >= NUSED || seen[nb2]) continue;
                    seen[nb2] = 1;
                    visited[nv] = nb2;
                    vdist[nv] = cdist(zq, FC + (size_t)AID[nb2] * PDIM);
                    nv++;
                    added++;
                }
                if (!added) {
                    /* dead end: jump to closest unvisited used anchor */
                    int jb = -1;
                    double jd = 1e300;
                    for (int i = 0; i < NUSED; i++) {
                        if (seen[i]) continue;
                        double d = cdist(zq, FC + (size_t)AID[i] * PDIM);
                        if (d < jd) { jd = d; jb = i; }
                    }
                    if (jb < 0) break;
                    seen[jb] = 1;
                    visited[nv] = jb;
                    vdist[nv] = jd;
                    nv++;
                }
            }
            nhops += hops;
            /* exact rank over visited buckets */
            int ns = 0;
            for (int i = 0; i < nv; i++) {
                int b = AID[visited[i]];
                for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) scan[ns++] = MEM[k];
            }
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
            nscan += ns;
            const int32_t *gt = GT + (size_t)qi * gd;
            int hit = 0;
            for (int i = 0; i < 10; i++)
                for (int j = 0; j < 10; j++)
                    if (besi[i] == gt[j]) { hit++; break; }
            recall += hit / 10.0;
        }
        double ms = (now_ms() - tq0) / nq;
        printf("WALK budget=%d: recall@10=%.4f scan=%.0f (%.3f%%) hops=%.1f %.2fms/q\n",
               BUD, recall / nq, (double)nscan / nq, (double)nscan / nq / NBASE * 100.0,
               (double)nhops / nq, ms);
    }
    return 0;
}
