/* tools/probe_qt_horizontal.c — Variant B HNSW-VERTICAL/QT-HORIZONTAL.
 * Routing IDENTICAL to maze_walk_cli (8-NN anchor graph, coarse top-2 seed,
 * best-first visit budgets). ONLY change: rerank via per-query quadtree over
 * PCA25 projections of VISITED members, best-first traversal with
 * centroid-distance bounds, exact 128-dim distances ONLY for members in
 * traversed leaves up to an exact-distance cap.
 * QT choice: per-query transient tree (no global QT, 0 extra persistent bytes).
 * BUILD: gcc -O2 -std=c11 -Wall -I. -o build/probe_qt_horizontal.exe tools/probe_qt_horizontal.c -lm
 * RUN: build/probe_qt_horizontal.exe [nq]  (fixed 3 points: visit16/cap7k, 32/14k, 64/27k)
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
#define LEAFCAP 256

static float *C1, *PC, *PM, *FC;
static int64_t *OFF;
static int32_t *MEM;
static float *BASE, *QUERY;
static int32_t *GT;
static int NBASE, NQRY;
static int KNBR[NUSED][KNN];
static int AID[NUSED];

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
    free(raw); *n_out = n; *d_out = d;
    return out;
}
static float *load_bin(const char *path, size_t nfloat) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    float *p = (float *)malloc(nfloat * 4);
    if (!p || fread(p, 4, nfloat, f) != nfloat) { fprintf(stderr, "read fail %s\n", path); exit(1); }
    fclose(f); return p;
}
static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }
static double cdist(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < PDIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}

/* ---- per-query QT over PCA25 ---- */
typedef struct { float c[PDIM]; float r; int start, count; int left, right; int leaf; double bound; } QN;
static int *QORD; static float *QPROJ; static QN *QNODE; static int QNN;
static int qsplit_dim; /* qsort helper */
static int cmp_ord(const void *a, const void *b) {
    float x = QPROJ[(size_t)(*(const int *)a) * PDIM + qsplit_dim];
    float y = QPROJ[(size_t)(*(const int *)b) * PDIM + qsplit_dim];
    return (x > y) - (x < y);
}
static int qt_build(int start, int count, int depth) {
    int ni = QNN++;
    QN *nd = &QNODE[ni];
    nd->start = start; nd->count = count; nd->left = nd->right = -1;
    double mn[PDIM], mx[PDIM];
    for (int j = 0; j < PDIM; j++) { mn[j] = 1e30; mx[j] = -1e30; }
    double cen[PDIM] = {0};
    for (int i = 0; i < count; i++) {
        float *p = QPROJ + (size_t)QORD[start + i] * PDIM;
        for (int j = 0; j < PDIM; j++) { double v = p[j]; cen[j] += v; if (v < mn[j]) mn[j] = v; if (v > mx[j]) mx[j] = v; }
    }
    for (int j = 0; j < PDIM; j++) { cen[j] /= count; nd->c[j] = (float)cen[j]; }
    double r2 = 0;
    for (int i = 0; i < count; i++) {
        float *p = QPROJ + (size_t)QORD[start + i] * PDIM;
        double d = 0;
        for (int j = 0; j < PDIM; j++) { double e = p[j] - cen[j]; d += e * e; }
        if (d > r2) r2 = d;
    }
    nd->r = (float)sqrt(r2);
    if (count <= LEAFCAP || depth >= 12) { nd->leaf = 1; return ni; }
    int sd = 0; double spread = -1;
    for (int j = 0; j < PDIM; j++) { double s = mx[j] - mn[j]; if (s > spread) { spread = s; sd = j; } }
    qsplit_dim = sd;
    qsort(QORD + start, (size_t)count, sizeof(int), cmp_ord);
    int half = count / 2;
    nd->leaf = 0;
    nd->left = qt_build(start, half, depth + 1);
    nd->right = qt_build(start + half, count - half, depth + 1);
    return ni;
}

int main(int argc, char **argv) {
    const char *D = "build/sift1m/sift/";
    char pb[256], qb[256], gb[256];
    snprintf(pb, sizeof(pb), "%ssift_base.fvecs", D);
    snprintf(qb, sizeof(qb), "%ssift_query.fvecs", D);
    snprintf(gb, sizeof(gb), "%ssift_groundtruth.ivecs", D);
    int dn = 0, gd = 0, gn = 0;
    double t0 = now_ms();
    BASE = (float *)load_vecs(pb, &NBASE, &dn);
    QUERY = (float *)load_vecs(qb, &NQRY, &dn);
    GT = (int32_t *)load_vecs(gb, &gn, &gd);
    C1 = load_bin("build/sift1m_c/C1.bin", (size_t)C1N * PDIM);
    PC = load_bin("build/sift1m_c/pca_comp.bin", (size_t)PDIM * DIM);
    PM = load_bin("build/sift1m_c/pca_mean.bin", DIM);
    FC = load_bin("build/sift1m_c/F.bin", (size_t)NB * PDIM);
    { FILE *f = fopen("build/sift1m_c/off.bin", "rb");
      OFF = (int64_t *)malloc(((size_t)NB + 1) * 8);
      if (fread(OFF, 8, NB + 1, f) != (size_t)NB + 1) { fprintf(stderr, "off read\n"); return 1; }
      fclose(f); }
    MEM = (int32_t *)load_bin("build/sift1m_c/mem.bin", 1000000);
    int nu = 0;
    for (int b = 0; b < NB && nu < NUSED; b++) if (OFF[b+1] > OFF[b]) AID[nu++] = b;
    double load_s = (now_ms() - t0) / 1000.0;

    t0 = now_ms();
    static uint8_t adj[NUSED][NUSED];
    for (int i = 0; i < NUSED; i++) {
        double bd[KNN]; int bi[KNN];
        for (int k = 0; k < KNN; k++) { bd[k] = 1e300; bi[k] = -1; }
        const float *ci = FC + (size_t)AID[i] * PDIM;
        for (int j = 0; j < NUSED; j++) {
            if (j == i) continue;
            double d = cdist(ci, FC + (size_t)AID[j] * PDIM);
            if (d < bd[KNN-1]) { int p = KNN-1; while (p > 0 && d < bd[p-1]) { bd[p]=bd[p-1]; bi[p]=bi[p-1]; p--; } bd[p]=d; bi[p]=j; }
        }
        for (int k = 0; k < KNN; k++) adj[i][bi[k]] = 1;
    }
    for (int i = 0; i < NUSED; i++) for (int j = 0; j < NUSED; j++) if (adj[i][j]) adj[j][i] = 1;
    for (int i = 0; i < NUSED; i++) {
        int c = 0;
        for (int j = 0; j < NUSED && c < KNN; j++) if (adj[i][j]) KNBR[i][c++] = j;
        for (int j = 0; j < NUSED && c < KNN; j++) if (j != i && !adj[i][j]) { KNBR[i][c++] = j; break; }
    }
    double build_s = (now_ms() - t0) / 1000.0;
    long idx_bytes = (long)C1N*PDIM*4 + (long)PDIM*DIM*4 + (long)DIM*4 + (long)NB*PDIM*4 + ((long)NB+1)*8 + 1000000L*4;
    printf("load %.1fs build(graph) %.1fs idx_bytes %ld\n", load_s, build_s, idx_bytes);

    int nq = argc > 1 ? atoi(argv[1]) : 1000;
    if (nq > NQRY) nq = NQRY;
    int visits[3] = {16, 32, 64};
    int caps[3] = {7000, 14000, 27000};

    float *zq = malloc(PDIM * 4);
    int *topc = malloc(C1N * sizeof(int));
    int *scan = malloc(1000000 * sizeof(int));
    uint8_t *seen = malloc(NUSED);
    QPROJ = malloc(1000000 * (size_t)PDIM * 4); /* worst-case reuse */
    QORD = malloc(1000000 * sizeof(int));
    QNODE = malloc(8192 * sizeof(QN));

    for (int pi = 0; pi < 3; pi++) {
        int BUD = visits[pi], CAP = caps[pi];
        double recall = 0; long long nexact = 0;
        double tq0 = now_ms();
        for (int qi = 0; qi < nq; qi++) {
            const float *q = QUERY + (size_t)qi * DIM;
            for (int i = 0; i < PDIM; i++) {
                double s = 0;
                for (int j = 0; j < DIM; j++) s += (double)PC[(size_t)i*DIM+j] * (q[j]-PM[j]);
                zq[i] = (float)s;
            }
            /* IDENTICAL seeding to maze */
            anch_route(zq, C1, C1N, PDIM, 2, topc);
            int seeds[2] = {-1,-1};
            for (int s = 0; s < 2; s++) {
                int seedb = topc[s]*BK; double bestd = 1e300;
                for (int j = 0; j < BK; j++) {
                    int b = seedb+j; if (OFF[b+1]==OFF[b]) continue;
                    double d = cdist(zq, FC+(size_t)b*PDIM);
                    if (d < bestd) { bestd = d; seeds[s] = b; }
                }
            }
            int seedidx[2] = {-1,-1};
            for (int s = 0; s < 2; s++) { if (seeds[s]<0) continue;
                for (int i = 0; i < NUSED; i++) if (AID[i]==seeds[s]) { seedidx[s]=i; break; } }
            /* IDENTICAL best-first expansion */
            memset(seen, 0, NUSED);
            int visited[64]; double vdist[64]; int nv = 0;
            for (int s = 0; s < 2 && nv < BUD; s++) {
                if (seedidx[s]<0 || seen[seedidx[s]]) continue;
                seen[seedidx[s]] = 1; visited[nv]=seedidx[s];
                vdist[nv]=cdist(zq, FC+(size_t)AID[seedidx[s]]*PDIM); nv++;
            }
            uint8_t expanded[2560]; memset(expanded, 0, NUSED);
            while (nv < BUD) {
                int b2=-1; double bd2=1e300;
                for (int i = 0; i < nv; i++) if (!expanded[visited[i]] && vdist[i]<bd2) { bd2=vdist[i]; b2=i; }
                if (b2<0) break;
                expanded[visited[b2]]=1;
                int added=0;
                for (int k = 0; k < KNN && nv < BUD; k++) {
                    int nb2 = KNBR[visited[b2]][k];
                    if (nb2<0||nb2>=NUSED||seen[nb2]) continue;
                    seen[nb2]=1; visited[nv]=nb2;
                    vdist[nv]=cdist(zq, FC+(size_t)AID[nb2]*PDIM); nv++; added++;
                }
                if (!added) {
                    int jb=-1; double jd=1e300;
                    for (int i = 0; i < NUSED; i++) { if (seen[i]) continue;
                        double d = cdist(zq, FC+(size_t)AID[i]*PDIM);
                        if (d<jd){jd=d;jb=i;} }
                    if (jb<0) break;
                    seen[jb]=1; visited[nv]=jb; vdist[nv]=jd; nv++;
                }
            }
            int ns = 0;
            for (int i = 0; i < nv; i++) { int b = AID[visited[i]];
                for (int64_t k = OFF[b]; k < OFF[b+1]; k++) scan[ns++] = MEM[k]; }
            /* project visited members to PCA25 */
            for (int i = 0; i < ns; i++) {
                const float *v = BASE + (size_t)scan[i]*DIM;
                float *po = QPROJ + (size_t)i*PDIM;
                for (int a = 0; a < PDIM; a++) {
                    double s = 0;
                    for (int j = 0; j < DIM; j++) s += (double)PC[(size_t)a*DIM+j]*(v[j]-PM[j]);
                    po[a]=(float)s;
                }
                QORD[i]=i;
            }
            QNN = 0;
            int root = qt_build(0, ns, 0);
            /* best-first over QT nodes */
            static int heap[8192]; int hn = 0;
            for (int j = 0; j < PDIM; j++) {}
            { double d=cdist(zq, QNODE[root].c); double lb = sqrt(d)-QNODE[root].r; if(lb<0)lb=0; QNODE[root].bound=lb*lb; }
            heap[hn++]=root;
            double best[10]; int besi[10];
            for (int i = 0; i < 10; i++) { best[i]=1e300; besi[i]=-1; }
            int done = 0, scored = 0;
            uint8_t *mseen = (uint8_t*)calloc((size_t)ns, 1);
            while (hn > 0 && scored < CAP) {
                int bi2=0; for(int i=1;i<hn;i++) if(QNODE[heap[i]].bound<QNODE[heap[bi2]].bound) bi2=i;
                int ni = heap[bi2]; heap[bi2]=heap[--hn];
                QN *nd = &QNODE[ni];
                if (nd->leaf) {
                    for (int i = 0; i < nd->count && scored < CAP; i++) {
                        int oi = QORD[nd->start+i];
                        if (mseen[oi]) continue; mseen[oi]=1;
                        int id = scan[oi];
                        const float *v = BASE + (size_t)id*DIM;
                        double d=0; for(int j=0;j<DIM;j++){double e=(double)q[j]-v[j];d+=e*e;}
                        scored++;
                        if (d<best[9]){int p=9;while(p>0&&d<best[p-1]){best[p]=best[p-1];besi[p]=besi[p-1];p--;}best[p]=d;besi[p]=id;}
                    }
                } else {
                    for (int c = 0; c < 2; c++) {
                        int ch = c ? nd->right : nd->left;
                        double d=cdist(zq, QNODE[ch].c); double lb=sqrt(d)-QNODE[ch].r; if(lb<0)lb=0;
                        QNODE[ch].bound=lb*lb; heap[hn++]=ch;
                    }
                }
                (void)done;
            }
            free(mseen);
            nexact += scored;
            const int32_t *gt = GT + (size_t)qi*gd;
            int hit=0;
            for(int i=0;i<10;i++) for(int j=0;j<10;j++) if(besi[i]==gt[j]){hit++;break;}
            recall += hit/10.0;
        }
        double ms = (now_ms()-tq0)/nq;
        printf("QT visit=%d cap=%d: recall@10=%.4f exact=%.0f (%.3f%%) %.2fms/q\n",
            BUD, CAP, recall/nq, (double)nexact/nq, (double)nexact/nq/NBASE*100.0, ms);
    }
    return 0;
}
