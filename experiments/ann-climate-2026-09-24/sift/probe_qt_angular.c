/* experiments/ann-climate-2026-09-24/sift/probe_qt_angular.c — angular frustum routing.
 *
 * OWNER CORRECTION (2026-09-25): 1 is the APEX (projection eye), not a level.
 * Frustum levels are 4,16,64 (x4 each). Routing = ANGLE of projection from the
 * apex: one atan2 per query, quantized to sector. Hierarchy is free by shift:
 * leaf64>>4 = L16 id, leaf64>>2... (leaf64/16 = L4 id). Zero compares.
 *
 * CHOICE DOCUMENTED:
 * - Plane = PCA2, apex = (median x, median y) from probe_qt_fastpath
 *   (89.7, 1.02 — recomputed here, must match).
 * - Leaf64 = floor((atan2(dy,dx)+PI)/2PI*64). Pure angular wedges, radius
 *   undivided (bare version; radius split is a follow-up, not this probe).
 * - Finish = exact 128-dim top-10 inside wedge (same as fastpath).
 * - Baseline A = 0.8023 (same n/GT; not rerun — identical harness inputs).
 *
 * EXPECTATION (stated before run): wedge counts roughly equal for blob-like
 * PCA2 (~15.6k each, 4x smaller than QT-16 leaves -> cheaper finish) BUT every
 * angular boundary passes through the dense center, so boundary loss may be
 * worse than axis boxes. This probe prices exactly that trade.
 *
 * GATE: VIABLE iff recall >= 0.7523 (A-0.05) at ms/q < 2.55.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_qt_angular.exe experiments/ann-climate-2026-09-24/sift/probe_qt_angular.c -lm
 * RUN (repo root): build/probe_qt_angular.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DIM 128
#define PDIM 25
#define NQ 1000
#define NLEAF 64

static float *PC, *PM, *BASE, *QUERY;
static int32_t *GT;
static int64_t *LOFF;
static int32_t *LMEM;
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
static double recall_at10(const int *ans, const int32_t *gt) {
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (ans[i] == gt[j]) { hit++; break; }
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
    printf("load: base %d query %d gt-dim %d in %.1fs\n", NBASE, NQRY, GTD, (now_ms() - t0) / 1000.0);

    /* apex = PCA2 medians */
    float *tx = (float *)malloc((size_t)NBASE * 4);
    float *ty = (float *)malloc((size_t)NBASE * 4);
    for (int i = 0; i < NBASE; i++)
        pca2(BASE + (size_t)i * DIM, tx + i, ty + i);
    float *sx = (float *)malloc((size_t)NBASE * 4);
    float *sy = (float *)malloc((size_t)NBASE * 4);
    memcpy(sx, tx, (size_t)NBASE * 4);
    memcpy(sy, ty, (size_t)NBASE * 4);
    qsort(sx, (size_t)NBASE, 4, cmpf);
    qsort(sy, (size_t)NBASE, 4, cmpf);
    float ax = sx[NBASE / 2], ay = sy[NBASE / 2];
    free(sx); free(sy);
    printf("APEX (median): x=%.3g y=%.3g\n", ax, ay);

    /* assign 1M base to 64 angular wedges */
    int lcnt[NLEAF] = {0};
    int *wleaf = (int *)malloc((size_t)NBASE * 4);
    for (int i = 0; i < NBASE; i++) {
        double a = atan2((double)ty[i] - ay, (double)tx[i] - ax);
        int L = (int)((a + M_PI) / (2 * M_PI) * NLEAF);
        if (L < 0) L = 0;
        if (L >= NLEAF) L = NLEAF - 1;
        wleaf[i] = L;
        lcnt[L]++;
    }
    free(tx); free(ty);
    LOFF = (int64_t *)malloc(((size_t)NLEAF + 1) * 8);
    LOFF[0] = 0;
    for (int L = 0; L < NLEAF; L++) LOFF[L + 1] = LOFF[L] + lcnt[L];
    LMEM = (int32_t *)malloc((size_t)LOFF[NLEAF] * 4);
    int64_t fill[NLEAF];
    memcpy(fill, LOFF, sizeof(fill));
    for (int i = 0; i < NBASE; i++) LMEM[fill[wleaf[i]]++] = i;
    free(wleaf);
    int lmin = lcnt[0], lmax = lcnt[0], lemp = 0;
    for (int L = 0; L < NLEAF; L++) {
        if (lcnt[L] < lmin) lmin = lcnt[L];
        if (lcnt[L] > lmax) lmax = lcnt[L];
        if (lcnt[L] == 0) lemp++;
    }
    printf("WEDGE64: min=%d max=%d mean=%.0f empty=%d (ratio %.2f)\n",
           lmin, lmax, 1000000.0 / NLEAF, lemp, (double)lmax / (lmin ? lmin : 1));

    /* QUERY: one atan2 -> wedge -> exact top-10 inside */
    int ans[10];
    double rec = 0;
    long long nsc = 0;
    t0 = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        float qx, qy;
        pca2(QUERY + (size_t)qi * DIM, &qx, &qy);
        double a = atan2((double)qy - ay, (double)qx - ax);
        int L = (int)((a + M_PI) / (2 * M_PI) * NLEAF);
        if (L < 0) L = 0;
        if (L > NLEAF - 1) L = NLEAF - 1;
        int L4 = L >> 4, L16 = L >> 2; /* free hierarchy */
        (void)L4; (void)L16;
        const float *qq = QUERY + (size_t)qi * DIM;
        double best[10];
        for (int i = 0; i < 10; i++) { best[i] = 1e300; ans[i] = -1; }
        int64_t ns = 0;
        for (int64_t k = LOFF[L]; k < LOFF[L + 1]; k++) {
            const float *v = BASE + (size_t)LMEM[k] * DIM;
            double d = 0;
            for (int j = 0; j < DIM; j++) { double e = (double)qq[j] - v[j]; d += e * e; }
            ns++;
            if (d < best[9]) {
                int p = 9;
                while (p > 0 && d < best[p - 1]) { best[p] = best[p - 1]; ans[p] = ans[p - 1]; p--; }
                best[p] = d; ans[p] = LMEM[k];
            }
        }
        nsc += ns;
        rec += recall_at10(ans, GT + (size_t)qi * GTD);
    }
    double ms = (now_ms() - t0) / NQ;
    rec /= NQ;
    printf("ANGULAR-64 (n=%d, pure wedge, exact-in-wedge): recall=%.4f scan=%.0f (%.3f%%) %.2fms/q\n",
           NQ, rec, (double)nsc / NQ, (double)nsc / NQ / NBASE * 100.0, ms);
    printf("ROUTING: 1x atan2 + quantize (0 compares); hierarchy L4=leaf>>4 L16=leaf>>2 free\n");
    int win = (rec >= 0.7523) && (ms < 2.55);
    printf("GATE: angular-frustum %s (recall %.4f need >= 0.7523, ms %.2f need < 2.55)\n",
           win ? "VIABLE" : "LOSES", rec, ms);
    return 0;
}
