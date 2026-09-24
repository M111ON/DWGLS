/* experiments/ann-climate-2026-09-24/sift/bench_serve_overlap.c — bench the SHIPPED
 * serve logic (logical top-2 overlap + budget cutoff) in-process on SIFT1M.
 *
 * WHY in-process: full HTTP bench needs an embedding server on 127.0.0.1:8095
 * (lz_embed) + model serve + populated kv_index — none live here (port CLOSED).
 * The routing math shipped in gguf_lazy_serve.c (anch_route top-16 buckets,
 * OR-match anchor/anchor2, stop at LZ_SEARCH_BUDGET) is identical here; only
 * the distance metric differs (serve=cosine over stored vecs, here exact L2 —
 * same selection structure). This prices overlap-delta + cutoff curve.
 *
 * SETUP: trained anchors F.bin (4096x25) + single posting OFF/MEM (sift1m_c);
 * anchor2[i] = 2nd-nearest bucket per entry (one-time ~1-3 min, printed).
 * QUERY (n=1000): route top-16 buckets -> candidates = anchor-in-sel OR
 * anchor2-in-sel (logical overlap, mirrors :1609-1613) -> exact 128-dim top-10
 * with budget sweep {0=none,512,128,32} (mirrors :1637 break).
 * REF: single posting (anchor only), no cutoff (= baseline A, expect 0.8023).
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/bench_serve_overlap.exe experiments/ann-climate-2026-09-24/sift/bench_serve_overlap.c -lm
 * RUN (repo root): build/bench_serve_overlap.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define DIM 128
#define PDIM 25
#define K 4096
#define NQ 1000
#define TOPB 16

static float *AA, *PC, *PM, *BASE, *QUERY;
static int32_t *GT;
static int64_t *OFF;
static int32_t *MEM;
static int NBASE, NQRY, GTD;
static int16_t *A1, *A2; /* anchor, anchor2 per entry (K<4096 fits u16; -1 none) */
static int32_t *USEDB;
static int NUSED;

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
static void *load_raw(const char *path, size_t nbytes) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long fz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if ((size_t)fz != nbytes) { fprintf(stderr, "size mismatch %s: got %ld want %ld\n", path, fz, (long)nbytes); exit(1); }
    void *p = malloc(nbytes ? nbytes : 1);
    if (!p || fread(p, 1, nbytes, f) != nbytes) { fprintf(stderr, "read fail %s\n", path); exit(1); }
    fclose(f);
    return p;
}

/* project x to PCA25 into z */
static void project(const float *x, float *z) {
    for (int c = 0; c < PDIM; c++) {
        double s = 0;
        const float *row = PC + (size_t)c * DIM;
        for (int j = 0; j < DIM; j++) s += (double)row[j] * ((double)x[j] - PM[j]);
        z[c] = (float)s;
    }
}
/* top-2 buckets of z over K anchors (exact, brute force, float math) */
static void top2(const float *z, int *b1, int *b2) {
    float d1 = 1e30f, d2 = 1e30f;
    int i1 = -1, i2 = -1;
    for (int b = 0; b < K; b++) {
        const float *c = AA + (size_t)b * PDIM;
        float d = 0;
        for (int j = 0; j < PDIM; j++) { float e = z[j] - c[j]; d += e * e; }
        if (d < d1) { d2 = d1; i2 = i1; d1 = d; i1 = b; }
        else if (d < d2) { d2 = d; i2 = b; }
    }
    *b1 = i1; *b2 = i2;
}
/* top-TOPB buckets of z */
static int topC(const float *z, int C, int *out) {
    static double dd[K];
    for (int b = 0; b < K; b++) {
        const float *c = AA + (size_t)b * PDIM;
        double d = 0;
        for (int j = 0; j < PDIM; j++) { double e = (double)z[j] - c[j]; d += e * e; }
        dd[b] = d;
    }
    int n = 0;
    for (int t = 0; t < C; t++) {
        double bd = 1e300; int bi = -1;
        for (int b = 0; b < K; b++) if (dd[b] < bd) { bd = dd[b]; bi = b; }
        if (bi < 0) break;
        out[n++] = bi; dd[bi] = 1e300;
    }
    return n;
}
static double recall_at10(const int *ans, const int32_t *gt) {
    int hit = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            if (ans[i] == gt[j]) { hit++; break; }
    return hit / 10.0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: progress survives */
    double t0 = now_ms();
    int dtmp;
    BASE = (float *)load_vecs("build/sift1m/sift/sift_base.fvecs", &NBASE, &dtmp);
    QUERY = (float *)load_vecs("build/sift1m/sift/sift_query.fvecs", &NQRY, &dtmp);
    int ngt;
    GT = (int32_t *)load_vecs("build/sift1m/sift/sift_groundtruth.ivecs", &ngt, &GTD);
    PC = load_bin("build/sift1m_c/pca_comp.bin", (size_t)PDIM * DIM);
    PM = load_bin("build/sift1m_c/pca_mean.bin", DIM);
    AA = load_bin("build/sift1m_c/F.bin", (size_t)K * PDIM);
    OFF = (int64_t *)load_raw("build/sift1m_c/off.bin", ((size_t)K + 1) * 8);
    MEM = (int32_t *)load_raw("build/sift1m_c/mem.bin", (size_t)NBASE * 4);
    printf("load: base %d query %d gt-dim %d in %.1fs\n", NBASE, NQRY, GTD, (now_ms() - t0) / 1000.0);

    USEDB = (int32_t *)malloc((size_t)K * 4);
    NUSED = 0;
    for (int b = 0; b < K; b++) if (OFF[b + 1] > OFF[b]) USEDB[NUSED++] = b;
    printf("used buckets: %d\n", NUSED);

    /* anchor2 per entry (one-time; mirrors lz_anchor_refresh top-2 posting) */
    t0 = now_ms();
    A1 = (int16_t *)malloc((size_t)NBASE * 2);
    A2 = (int16_t *)malloc((size_t)NBASE * 2);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < NBASE; i++) {
        float zz[PDIM];
        project(BASE + (size_t)i * DIM, zz);
        int b1, b2;
        top2(zz, &b1, &b2);
        A1[i] = (int16_t)b1; A2[i] = (int16_t)b2;
        if ((i % 200000) == 0) printf("  anchor2 %d/1M (%.1fs)\n", i, (now_ms() - t0) / 1000.0);
    }
    printf("anchor2 build: 1M entries in %.1fs\n", (now_ms() - t0) / 1000.0);

    /* arms */
    float z[PDIM];
    int sel[TOPB], ans[10];
    int budgets[4] = {0, 512, 128, 32};
    const char *bnames[4] = {"none", "512", "128", "32"};
    for (int ov = 0; ov <= 1; ov++) {
        for (int bi = 0; bi < 4; bi++) {
            int budget = budgets[bi];
            double rec = 0;
            long long nsc = 0;
            int trunc = 0;
            t0 = now_ms();
            for (int qi = 0; qi < NQ; qi++) {
                project(QUERY + (size_t)qi * DIM, z);
                int nb = topC(z, TOPB, sel);
                const float *qq = QUERY + (size_t)qi * DIM;
                double best[10];
                for (int i = 0; i < 10; i++) { best[i] = 1e300; ans[i] = -1; }
                int64_t ns = 0;
                int stop = 0;
                for (int t = 0; t < nb && !stop; t++) {
                    int b = sel[t];
                    for (int64_t k = OFF[b]; k < OFF[b + 1] && !stop; k++) {
                        int id = MEM[k];
                        if (ov && A1[id] != b && A2[id] != b) continue; /* logical overlap: reachable via either */
                        if (!ov && A1[id] != b) continue; /* single posting (OFF may hold trained assigns; enforce A1) */
                        const float *v = BASE + (size_t)id * DIM;
                        double d = 0;
                        for (int j = 0; j < DIM; j++) { double e = (double)qq[j] - v[j]; d += e * e; }
                        ns++;
                        if (d < best[9]) {
                            int p = 9;
                            while (p > 0 && d < best[p - 1]) { best[p] = best[p - 1]; ans[p] = ans[p - 1]; p--; }
                            best[p] = d; ans[p] = id;
                        }
                        if (budget > 0 && ns >= budget) { trunc++; stop = 1; }
                    }
                }
                nsc += ns;
                rec += recall_at10(ans, GT + (size_t)qi * GTD);
            }
            double ms = (now_ms() - t0) / NQ;
            rec /= NQ;
            printf("%s budget=%s: recall=%.4f scored=%.0f (%.3f%%) trunc=%d/1000 %.2fms/q\n",
                   ov ? "OVERLAP" : "SINGLE ", bnames[bi], rec,
                   (double)nsc / NQ, (double)nsc / NQ / NBASE * 100.0, trunc, ms);
        }
    }
    return 0;
}
