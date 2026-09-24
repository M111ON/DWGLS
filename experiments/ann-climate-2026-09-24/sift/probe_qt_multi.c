/* tools/probe_qt_multi.c — QT-vertical selection x top-2 multi-entry stacking probe.
 *
 * Q: do the two gains stack? QT-vertical bucket selection (probe_qt_vertical.c)
 * on top of top-2 double posting lists (probe_multientry.c).
 *
 * BUILD CHOICE DOCUMENTED: double-list assignment uses a coarse shortlist
 * (project to PCA25, top-4 of 256 coarse centroids, exact top-2 among used
 * fine buckets under those 4 coarse, ~<=64 candidates) instead of exact
 * brute force over all 2560 used fine centroids, to keep build under ~60s.
 *
 * Per query: QT-select top-L leaves (L in {8,16,32}) -> closest bucket per
 * leaf = seeds -> SAME greedy 8-NN anchor walk as maze/vertical with
 * BUD=2*L -> score members from DOUBLE lists with stamp dedupe + exact
 * 128-dim rerank -> top-10 vs GT. Fixed n=1000, no tuning.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I. -o build/probe_qt_multi.exe tools/probe_qt_multi.c -lm
 * RUN:   ./build/probe_qt_multi.exe [nq]   (default 1000)
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
#define NQ 1000

static float *C1, *PC, *PM, *FC;
static int64_t *OFF;
static int32_t *MEM;
static float *BASE, *QUERY;
static int32_t *GT;
static int NBASE, NQRY, GTD;
static int AID[NUSED];
static int KNBR[NUSED][KNN];

/* ---- quadtree (copy of probe_qt_vertical) ---- */
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
    float mn[PDIM], mx[PDIM];
    for (int j = 0; j < PDIM; j++) { mn[j] = 1e30f; mx[j] = -1e30f; }
    for (int i = 0; i < n; i++) {
        const float *p = FC + (size_t)AID[QPERM[l + i]] * PDIM;
        for (int j = 0; j < PDIM; j++) { if (p[j] < mn[j]) mn[j] = p[j]; if (p[j] > mx[j]) mx[j] = p[j]; }
    }
    int sd = 0; float sr = -1;
    for (int j = 0; j < PDIM; j++) { float r = mx[j] - mn[j]; if (r > sr) { sr = r; sd = j; } }
    if (sr <= 0) return id;
    float *tmp = (float *)malloc((size_t)n * 4);
    for (int i = 0; i < n; i++) tmp[i] = (FC + (size_t)AID[QPERM[l + i]] * PDIM)[sd];
    qsort(tmp, n, 4, cmpf);
    float med = tmp[n / 2];
    free(tmp);
    int m = l;
    for (int i = l; i < l + n; i++) {
        const float *p = FC + (size_t)AID[QPERM[i]] * PDIM;
        if (p[sd] < med) { int t = QPERM[i]; QPERM[i] = QPERM[m]; QPERM[m] = t; m++; }
    }
    if (m == l || m == l + n) return id;
    nd->leaf = 0; nd->dim = sd; nd->cut = med;
    nd->left = qbuild(l, m - l, depth - 1);
    nd->right = qbuild(m, l + n - m, depth - 1);
    return id;
}
static int qdescend(const float *zq, int L, int *leaves) {
    int cand[MAXNODES], nleaf = 0, nc = 1;
    double cd[MAXNODES];
    cand[0] = 0; cd[0] = 0;
    { double d = 0; for (int j = 0; j < PDIM; j++) { double e = zq[j] - QN[0].cx[j]; d += e * e; } cd[0] = d; }
    uint8_t done[MAXNODES]; memset(done, 0, sizeof(done));
    while (nleaf < L && nc > 0) {
        int bi = -1; double bd = 1e300;
        for (int i = 0; i < nc; i++) if (!done[cand[i]] && cd[i] < bd) { bd = cd[i]; bi = i; }
        if (bi < 0) break;
        int nid = cand[bi];
        done[nid] = 1;
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

/* ---- loaders ---- */
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
static void project(const float *x, float *z) {
    for (int i = 0; i < PDIM; i++) {
        double s = 0;
        const float *row = PC + (size_t)i * DIM;
        for (int j = 0; j < DIM; j++) s += (double)row[j] * (x[j] - PM[j]);
        z[i] = (float)s;
    }
}

int main(int argc, char **argv) {
    double t0 = now_ms();
    int dtmp;
    BASE = (float *)load_vecs("build/sift1m/sift/sift_base.fvecs", &NBASE, &dtmp);
    QUERY = (float *)load_vecs("build/sift1m/sift/sift_query.fvecs", &NQRY, &dtmp);
    int ngt;
    GT = (int32_t *)load_vecs("build/sift1m/sift/sift_groundtruth.ivecs", &ngt, &GTD);
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
    if (nu != NUSED) { fprintf(stderr, "used count %d != %d\n", nu, NUSED); return 1; }
    printf("load: base %d query %d gt-dim %d used %d in %.1fs\n",
           NBASE, NQRY, GTD, nu, (now_ms() - t0) / 1000.0);

    /* 1. double posting lists via coarse shortlist (top-4 coarse -> top-2 fine) */
    t0 = now_ms();
    static int cused[C1N][BK];
    static int cusedn[C1N];
    for (int u = 0; u < NUSED; u++) {
        int c = AID[u] / BK;
        cused[c][cusedn[c]++] = u;
    }
    int64_t *cnt = (int64_t *)calloc(NUSED, 8);
    int32_t *a1 = (int32_t *)malloc((size_t)NBASE * 4);
    int32_t *a2 = (int32_t *)malloc((size_t)NBASE * 4);
    float *z = (float *)malloc(PDIM * 4);
    double cdistv[C1N];
    int cord[C1N];
    for (int i = 0; i < NBASE; i++) {
        project(BASE + (size_t)i * DIM, z);
        for (int c = 0; c < C1N; c++) cdistv[c] = cdist(z, C1 + (size_t)c * PDIM);
        for (int c = 0; c < C1N; c++) cord[c] = c;
        /* partial select top-4 coarse */
        for (int k = 0; k < 4; k++) {
            int bi = k;
            for (int c = k + 1; c < C1N; c++)
                if (cdistv[cord[c]] < cdistv[cord[bi]]) bi = c;
            int t = cord[k]; cord[k] = cord[bi]; cord[bi] = t;
        }
        double bd1 = 1e300, bd2 = 1e300;
        int t1 = -1, t2 = -1;
        for (int k = 0; k < 4; k++) {
            int c = cord[k];
            for (int m = 0; m < cusedn[c]; m++) {
                int u = cused[c][m];
                double d = cdist(z, FC + (size_t)AID[u] * PDIM);
                if (d < bd1) { bd2 = bd1; t2 = t1; bd1 = d; t1 = u; }
                else if (d < bd2) { bd2 = d; t2 = u; }
            }
        }
        if (t1 < 0) { fprintf(stderr, "no assign %d\n", i); return 1; }
        if (t2 < 0) t2 = t1;
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
    double tdouble = (now_ms() - t0) / 1000.0;
    printf("DOUBLE build (coarse top-4 shortlist): entries=%ld mean=%.2f in %.1fs\n",
           (long)nent2, (double)nent2 / NUSED, tdouble);

    /* 2. 8-NN anchor graph (copy of maze/vertical) */
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

    /* 3. quadtree over used centroids */
    t0 = now_ms();
    for (int i = 0; i < NUSED; i++) QPERM[i] = i;
    qbuild(0, NUSED, QTDEPTH);
    double ttree = (now_ms() - t0) / 1000.0;
    int nleaves = 0;
    for (int i = 0; i < QNN; i++) if (QN[i].leaf) nleaves++;
    printf("build: graph %.1fs + quadtree depth%d nodes=%d leaves=%d in %.1fs\n",
           tgraph, QTDEPTH, QNN, nleaves, ttree);

    /* 4. queries: QT-select -> walk BUD=2L -> score DOUBLE lists w/ dedupe */
    int nq = argc > 1 ? atoi(argv[1]) : NQ;
    if (nq > NQRY) nq = NQRY;
    int Ls[3] = { 8, 16, 32 }, BUDs[3] = { 16, 32, 64 };
    float *zq = (float *)malloc(PDIM * 4);
    int *scan = (int *)malloc((size_t)NBASE * 4);
    int32_t *stamp = (int32_t *)calloc(NBASE, 4);
    int32_t cur = 1;
    uint8_t *seen = (uint8_t *)malloc(NUSED);

    for (int pi = 0; pi < 3; pi++) {
        int L = Ls[pi], BUD = BUDs[pi];
        double recall = 0;
        long long nscan = 0, nhops = 0;
        double tq0 = now_ms();
        int leaves[64];
        for (int qi = 0; qi < nq; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            project(q, zq);
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
            cur++;
            for (int i = 0; i < nv; i++) {
                int u = visited[i];
                for (int64_t k = off2[u]; k < off2[u + 1]; k++) {
                    int id = mem2[k];
                    if (stamp[id] != cur) { stamp[id] = cur; scan[ns++] = id; }
                }
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
            const int32_t *gt = GT + (size_t)qi * GTD;
            int hit = 0;
            for (int i = 0; i < 10; i++)
                for (int j = 0; j < 10; j++)
                    if (besi[i] == gt[j]) { hit++; break; }
            recall += hit / 10.0;
        }
        double ms = (now_ms() - tq0) / nq;
        printf("QT+DBL L=%d BUD=%d: recall@10=%.4f scan=%.0f (%.3f%%) hops=%.1f %.2fms/q\n",
               L, BUD, recall / nq, (double)nscan / nq, (double)nscan / nq / NBASE * 100.0,
               (double)nhops / nq, ms);
    }
    long bytes_double = (long)nent2 * 4 + (long)(NUSED + 1) * 8;
    printf("BYTES: double postings+off=%ld\n", bytes_double);
    return 0;
}
