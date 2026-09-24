/* tools/probe_grover_elim.c — Grover-flavor ELIMINATION probe vs maze routing.
 *
 * Routing (anchor-graph walk + seeding) copied VERBATIM from tools/maze_walk_cli.c,
 * so the visited-member pool per query is identical; ONLY the refine step differs.
 *
 * BOUND CHOICE: centroid-distance proxy. Each visited member's cheap bound is the
 * PCA25 centroid distance of its bucket (vdist[], already computed during the walk
 * — zero extra flops). Per-member PCA25 projection was rejected: 25x128 MACs/member
 * is 25x MORE than one exact 128-dim distance, so it cannot honestly be called
 * "cheap". NOTE: with a total-order bound, R=2 halving (50% then 50%) selects the
 * same top-25% set as one-shot top-25%; challenger vs B1 then tests whether the
 * iterative procedure itself matters (it should tie, modulo qsort tie-breaks).
 *
 * Per query per visit budget {16,32,64}:
 *   B0 maze-full:    exact 128-dim on ALL visited members -> top-10.
 *   B1 centroid-topK: sort by cheap bound once, exact ONLY on top 25% -> top-10.
 *   CH challenger:   R=2 rounds of sort+discard-bottom-50%, exact on survivors.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I. -o build/probe_grover_elim.exe tools/probe_grover_elim.c -lm
 * RUN:   ./build/probe_grover_elim.exe [nq]   (default 1000, budgets fixed 16 32 64)
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
#define NUSED 2560
#define KNN 8

static float *C1, *PC, *PM, *FC;
static int64_t *OFF;
static int32_t *MEM;
static float *BASE, *QUERY;
static int32_t *GT;
static int NBASE, NQRY;
static int KNBR[NUSED][KNN];
static int AID[NUSED];

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
    (void)is_float;
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

typedef struct { double b; int id; } BO;
static int bo_cmp(const void *a, const void *b) {
    double x = ((const BO *)a)->b, y = ((const BO *)b)->b;
    if (x < y) return -1;
    if (x > y) return 1;
    return ((const BO *)a)->id - ((const BO *)b)->id;
}

/* exact 128-dim rank of ids[0..n) -> top-10 ids in besi[] */
static void exact_top10(const float *q, const int *ids, int n, int besi[10]) {
    double best[10];
    for (int i = 0; i < 10; i++) { best[i] = 1e300; besi[i] = -1; }
    for (int i = 0; i < n; i++) {
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
static double recall10(const int besi[10], const int32_t *gt) {
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (besi[i] == gt[j]) { hit++; break; }
    return hit / 10.0;
}

int main(int argc, char **argv) {
    const char *D = "build/sift1m/sift/";
    char pb[256], qb[256], gb[256];
    snprintf(pb, sizeof(pb), "%ssift_base.fvecs", D);
    snprintf(qb, sizeof(qb), "%ssift_query.fvecs", D);
    snprintf(gb, sizeof(gb), "%ssift_groundtruth.ivecs", D);
    int dn = 0, gd = 0, gn = 0;
    (void)gn;
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
    int nu = 0;
    for (int b = 0; b < NB && nu < NUSED; b++)
        if (OFF[b + 1] > OFF[b]) AID[nu++] = b;
    printf("load: base %d query %d used-buckets %d in %.1fs\n", NBASE, NQRY, nu, (now_ms() - t0) / 1000.0);
    if (nu != NUSED) { fprintf(stderr, "used count %d != %d\n", nu, NUSED); return 1; }

    /* kNN graph over used anchors (centroid L2, bidirectional) — verbatim maze */
    t0 = now_ms();
    static uint8_t adj[NUSED][NUSED];
    for (int i = 0; i < NUSED; i++) {
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
    for (int i = 0; i < NUSED; i++)
        for (int j = 0; j < NUSED; j++)
            if (adj[i][j]) adj[j][i] = 1;
    int maxdeg = 0;
    for (int i = 0; i < NUSED; i++) {
        int c = 0;
        for (int j = 0; j < NUSED && c < KNN; j++)
            if (adj[i][j]) KNBR[i][c++] = j;
        for (int j = 0; j < NUSED && c < KNN; j++)
            if (j != i && !adj[i][j]) { KNBR[i][c++] = j; break; }
        if (c > maxdeg) maxdeg = c;
    }
    printf("graph: kNN=%d maxdeg-used=%d built in %.1fs\n", KNN, maxdeg, (now_ms() - t0) / 1000.0);
    printf("bound: centroid-distance proxy (bucket vdist, 0 extra flops)\n");

    int nq = argc > 1 ? atoi(argv[1]) : 1000;
    if (nq > NQRY) nq = NQRY;
    int budgets[3] = { 16, 32, 64 };

    float *zq = (float *)malloc(PDIM * 4);
    int *topc = (int *)malloc(C1N * sizeof(int));
    int *scan = (int *)malloc(1000000 * sizeof(int));
    uint8_t *seen = (uint8_t *)malloc(NUSED);
    BO *ord = (BO *)malloc(1000000 * sizeof(BO));
    int *sub = (int *)malloc(1000000 * sizeof(int));

    for (int bi = 0; bi < 3; bi++) {
        int BUD = budgets[bi];
        double rB0 = 0, rB1 = 0, rCH = 0;
        long long eB0 = 0, eB1 = 0, eCH = 0;
        double walkT = 0, tB0 = 0, tB1 = 0, tCH = 0;
        for (int qi = 0; qi < nq; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            double w0 = now_ms();
            for (int i = 0; i < PDIM; i++) {
                double s = 0;
                for (int j = 0; j < DIM; j++) s += (double)PC[(size_t)i * DIM + j] * (q[j] - PM[j]);
                zq[i] = (float)s;
            }
            /* seed: nearest fine of top-2 coarse (diversity) — verbatim maze */
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
            /* best-first (beam) expansion over the graph, budget BUD buckets — verbatim maze */
            memset(seen, 0, NUSED);
            int visited[64];
            double vdist[64];
            int nv = 0;
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
                int bi2 = -1;
                double bd2 = 1e300;
                for (int i = 0; i < nv; i++)
                    if (!expanded[visited[i]] && vdist[i] < bd2) { bd2 = vdist[i]; bi2 = i; }
                if (bi2 < 0) break;
                expanded[visited[bi2]] = 1;
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
            /* visited-member pool + per-member cheap bound (bucket vdist) */
            int ns = 0;
            for (int i = 0; i < nv; i++) {
                int b = AID[visited[i]];
                for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) {
                    scan[ns] = MEM[k];
                    ord[ns].b = vdist[i];
                    ord[ns].id = MEM[k];
                    ns++;
                }
            }
            walkT += now_ms() - w0;
            const int32_t *gt = GT + (size_t)qi * gd;
            int besi[10];

            /* B0: exact on ALL visited */
            double s0 = now_ms();
            exact_top10(q, scan, ns, besi);
            tB0 += now_ms() - s0;
            rB0 += recall10(besi, gt);
            eB0 += ns;

            /* B1: one-shot top-25% by cheap bound, exact only there */
            s0 = now_ms();
            qsort(ord, (size_t)ns, sizeof(BO), bo_cmp);
            int k1 = ns / 4;
            if (k1 < 10) k1 = ns < 10 ? ns : 10;
            for (int i = 0; i < k1; i++) sub[i] = ord[i].id;
            exact_top10(q, sub, k1, besi);
            tB1 += now_ms() - s0;
            rB1 += recall10(besi, gt);
            eB1 += k1;

            /* CH: R=2 elimination rounds (keep top 50%, then top 50% again).
               ord[] is already bound-sorted (ascending); each round keeps the
               top half of survivors, so round 2 re-sorting is a documented no-op. */
            s0 = now_ms();
            int n1 = ns / 2;
            if (n1 < 10) n1 = ns < 10 ? ns : 10;
            for (int i = 0; i < n1; i++) sub[i] = ord[i].id; /* round-1 survivors */
            int ncur = n1 / 2;                                /* round-2 survivors */
            if (ncur < 10) ncur = n1 < 10 ? n1 : 10;
            exact_top10(q, sub, ncur, besi);
            tCH += now_ms() - s0;
            rCH += recall10(besi, gt);
            eCH += ncur;
        }
        double exPct0 = (double)eB0 / nq / NBASE * 100.0;
        double exPct1 = (double)eB1 / nq / NBASE * 100.0;
        double exPctC = (double)eCH / nq / NBASE * 100.0;
        printf("visit=%d | CH recall=%.4f exact%%=%.4f ms/q=%.2f | "
               "B0 recall=%.4f exact%%=%.4f ms/q=%.2f | "
               "B1 recall=%.4f exact%%=%.4f ms/q=%.2f\n",
               BUD,
               rCH / nq, exPctC, (walkT + tCH) / nq,
               rB0 / nq, exPct0, (walkT + tB0) / nq,
               rB1 / nq, exPct1, (walkT + tB1) / nq);
    }
    return 0;
}
