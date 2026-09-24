/* tools/probe_qt_vertical.c — VARIANT A: QT-VERTICAL / WALK-HORIZONTAL.
 *
 * Q: does QUADTREE selection of which buckets to visit beat flat top-C routing?
 * - Quadtree (recursive median split, depth 8 -> <=256 leaves) over the 2560 USED
 *   fine centroids in PCA25 space (F.bin + used list from off.bin, same as maze).
 * - Per query: project to PCA25, best-first descend tree to top-L leaves
 *   (L in {8,16,32}), closest bucket per leaf = seeds.
 * - SAME greedy 8-NN anchor walk as maze_walk_cli (copied), expanded to total
 *   visited BUD in {16,32,64} (BUD = 2*L), then exact 128-dim rerank.
 * - Reports recall@10 vs GT + scan% + ms/q, index bytes, build seconds.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I. -o build/probe_qt_vertical.exe tools/probe_qt_vertical.c -lm
 * RUN:   ./build/probe_qt_vertical.exe [nq]   (default 1000)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#define DIM 128
#define PDIM 25
#define C1N 256
#define BK 16
#define NB (C1N * BK)
#define NUSED 2560
#define KNN 8
#define QTDEPTH 8
#define MAXNODES 600

static float *PC, *PM, *FC;
static int64_t *OFF;
static int32_t *MEM;
static float *BASE, *QUERY;
static int32_t *GT;
static int NBASE, NQRY;
static int KNBR[NUSED][KNN];
static int AID[NUSED];

/* ---- quadtree ---- */
typedef struct { int dim; float cut; int left, right; int start, count; int leaf; float cx[PDIM]; } QNode;
static QNode QN[MAXNODES];
static int QNN = 0;
static int QPERM[NUSED];

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void qcent(int l, int n, float *c) {
    for (int j = 0; j < PDIM; j++) c[j] = 0;
    for (int i = 0; i < n; i++) {
        const float *p = FC + (size_t)AID[QPERM[l + i]] * PDIM;
        for (int j = 0; j < PDIM; j++) c[j] += p[j];
    }
    for (int j = 0; j < PDIM; j++) c[j] /= (n ? n : 1);
}

static int qbuild(int l, int n, int depth) {
    int id = QNN++;
    if (id >= MAXNODES) { fprintf(stderr, "qnodes overflow\n"); exit(1); }
    QNode *nd = &QN[id];
    nd->start = l; nd->count = n; nd->left = nd->right = -1; nd->leaf = 1; nd->dim = -1;
    qcent(l, n, nd->cx);
    if (depth <= 0 || n <= 10) return id;
    /* split dim = max range */
    float mn[PDIM], mx[PDIM];
    for (int j = 0; j < PDIM; j++) { mn[j] = 1e30f; mx[j] = -1e30f; }
    for (int i = 0; i < n; i++) {
        const float *p = FC + (size_t)AID[QPERM[l + i]] * PDIM;
        for (int j = 0; j < PDIM; j++) { if (p[j] < mn[j]) mn[j] = p[j]; if (p[j] > mx[j]) mx[j] = p[j]; }
    }
    int sd = 0; float sr = -1;
    for (int j = 0; j < PDIM; j++) { float r = mx[j] - mn[j]; if (r > sr) { sr = r; sd = j; } }
    if (sr <= 0) return id;
    /* median on sd */
    float *tmp = (float *)malloc((size_t)n * 4);
    for (int i = 0; i < n; i++) tmp[i] = (FC + (size_t)AID[QPERM[l + i]] * PDIM)[sd];
    qsort(tmp, n, 4, cmpf);
    float med = tmp[n / 2];
    free(tmp);
    /* partition */
    int m = l;
    for (int i = l; i < l + n; i++) {
        const float *p = FC + (size_t)AID[QPERM[i]] * PDIM;
        if (p[sd] < med) { int t = QPERM[i]; QPERM[i] = QPERM[m]; QPERM[m] = t; m++; }
    }
    if (m == l || m == l + n) return id; /* degenerate -> leaf */
    nd->leaf = 0; nd->dim = sd; nd->cut = med;
    nd->left = qbuild(l, m - l, depth - 1);
    nd->right = qbuild(m, l + n - m, depth - 1);
    return id;
}

/* best-first descend to top-L leaves (by node-centroid dist) */
static int qdescend(const float *zq, int L, int *leaves) {
    int cand[MAXNODES], nleaf = 0, nc = 1;
    double cd[MAXNODES];
    cand[0] = 0; cd[0] = 0;
    /* dist of root */
    { double d = 0; for (int j = 0; j < PDIM; j++) { double e = zq[j] - QN[0].cx[j]; d += e * e; } cd[0] = d; }
    uint8_t done[MAXNODES]; memset(done, 0, sizeof(done));
    while (nleaf < L && nc > 0) {
        int bi = -1; double bd = 1e300;
        for (int i = 0; i < nc; i++) if (!done[cand[i]] && cd[i] < bd) { bd = cd[i]; bi = i; }
        if (bi < 0) break;
        int nid = cand[bi];
        done[nid] = 1;
        /* remove from cand */
        cand[bi] = cand[--nc]; cd[bi] = cd[nc];
        QNode *nd = &QN[nid];
        if (nd->leaf) { leaves[nleaf++] = nid; continue; }
        for (int s = 0; s < 2; s++) {
            int ch = s ? nd->right : nd->left;
            QNode *c = &QN[ch];
            double d = 0;
            for (int j = 0; j < PDIM; j++) { double e = zq[j] - c->cx[j]; d += e * e; }
            cand[nc] = ch; cd[nc] = d; nc++;
        }
    }
    return nleaf;
}

/* ---- loaders (same formats as maze_walk_cli) ---- */
static void *load_vecs(const char *path, int *n_out, int *d_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long fz = ftell(f); fseek(f, 0, SEEK_SET);
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
    double t0 = now_ms();
    BASE = (float *)load_vecs("build/sift1m/sift/sift_base.fvecs", &NBASE, &(int){0});
    QUERY = (float *)load_vecs("build/sift1m/sift/sift_query.fvecs", &NQRY, &(int){0});
    int gd = 0, gn = 0;
    GT = (int32_t *)load_vecs("build/sift1m/sift/sift_groundtruth.ivecs", &gn, &gd);
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
    if (nu != NUSED) { fprintf(stderr, "used count %d != %d\n", nu, NUSED); return 1; }
    printf("load: base %d query %d gt-dim %d used %d in %.1fs\n", NBASE, NQRY, gd, nu, (now_ms() - t0) / 1000.0);

    /* 8-NN anchor graph (copy of maze) */
    t0 = now_ms();
    static uint8_t adj[NUSED][NUSED];
    for (int i = 0; i < NUSED; i++) {
        double bd[KNN]; int bi[KNN];
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
    for (int i = 0; i < NUSED; i++) {
        int c = 0;
        for (int j = 0; j < NUSED && c < KNN; j++)
            if (adj[i][j]) KNBR[i][c++] = j;
        for (int j = 0; j < NUSED && c < KNN; j++)
            if (j != i && !adj[i][j]) { KNBR[i][c++] = j; break; }
    }
    double tgraph = (now_ms() - t0) / 1000.0;

    /* quadtree over used centroids */
    t0 = now_ms();
    for (int i = 0; i < NUSED; i++) QPERM[i] = i;
    qbuild(0, NUSED, QTDEPTH);
    double ttree = (now_ms() - t0) / 1000.0;
    int nleaves = 0; size_t leafmem = 0;
    for (int i = 0; i < QNN; i++) if (QN[i].leaf) { nleaves++; leafmem += (size_t)QN[i].count; }
    size_t idx_bytes = (size_t)QNN * (4 + 4 + 4 + 4 + 4) + (size_t)QNN * PDIM * 4
                     + (size_t)NUSED * 4 + (size_t)NUSED * KNN * 4;
    printf("build: graph %.1fs + quadtree depth%d nodes=%d leaves=%d leafmem=%lu in %.1fs | index_bytes=%lu (nodes+centroids+perm+graph)\n",
           tgraph, QTDEPTH, QNN, nleaves, (unsigned long)leafmem, ttree, (unsigned long)idx_bytes);

    int nq = argc > 1 ? atoi(argv[1]) : 1000;
    if (nq > NQRY) nq = NQRY;
    int Ls[3] = { 8, 16, 32 }, BUDs[3] = { 16, 32, 64 };
    float *zq = (float *)malloc(PDIM * 4);
    int *scan = (int *)malloc(1000000 * sizeof(int));
    uint8_t *seen = (uint8_t *)malloc(NUSED);

    for (int pi = 0; pi < 3; pi++) {
        int L = Ls[pi], BUD = BUDs[pi];
        double recall = 0;
        long long nscan = 0, nhops = 0;
        double tq0 = now_ms();
        int leaves[64];
        for (int qi = 0; qi < nq; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            for (int i = 0; i < PDIM; i++) {
                double s = 0;
                for (int j = 0; j < DIM; j++) s += (double)PC[(size_t)i * DIM + j] * (q[j] - PM[j]);
                zq[i] = (float)s;
            }
            /* QT seeds: top-L leaves -> closest bucket per leaf */
            int nl = qdescend(zq, L, leaves);
            int seeds[64]; int ns0 = 0;
            uint8_t sseen[NUSED]; memset(sseen, 0, NUSED);
            for (int li = 0; li < nl && ns0 < BUD; li++) {
                QNode *nd = &QN[leaves[li]];
                int bestu = -1; double bestd = 1e300;
                for (int k = 0; k < nd->count; k++) {
                    int u = QPERM[nd->start + k];
                    double d = cdist(zq, FC + (size_t)AID[u] * PDIM);
                    if (d < bestd) { bestd = d; bestu = u; }
                }
                if (bestu >= 0 && !sseen[bestu]) { sseen[bestu] = 1; seeds[ns0++] = bestu; }
            }
            /* SAME best-first walk as maze, budget BUD buckets */
            memset(seen, 0, NUSED);
            int visited[64]; double vdist[64]; int nv = 0, hops = 0;
            for (int s = 0; s < ns0 && nv < BUD; s++) {
                if (seen[seeds[s]]) continue;
                seen[seeds[s]] = 1;
                visited[nv] = seeds[s];
                vdist[nv] = cdist(zq, FC + (size_t)AID[seeds[s]] * PDIM);
                nv++;
            }
            uint8_t expanded[2560]; memset(expanded, 0, NUSED);
            while (nv < BUD) {
                int bi2 = -1; double bd2 = 1e300;
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
                    nv++; added++;
                }
                if (!added) {
                    int jb = -1; double jd = 1e300;
                    for (int i = 0; i < NUSED; i++) {
                        if (seen[i]) continue;
                        double d = cdist(zq, FC + (size_t)AID[i] * PDIM);
                        if (d < jd) { jd = d; jb = i; }
                    }
                    if (jb < 0) break;
                    seen[jb] = 1; visited[nv] = jb; vdist[nv] = jd; nv++;
                }
            }
            nhops += hops;
            int ns = 0;
            for (int i = 0; i < nv; i++) {
                int b = AID[visited[i]];
                for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) scan[ns++] = MEM[k];
            }
            double best[10]; int besi[10];
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
        printf("QT L=%d BUD=%d: recall@10=%.4f scan=%.0f (%.3f%%) hops=%.1f %.2fms/q\n",
               L, BUD, recall / nq, (double)nscan / nq, (double)nscan / nq / NBASE * 100.0,
               (double)nhops / nq, ms);
    }
    return 0;
}
