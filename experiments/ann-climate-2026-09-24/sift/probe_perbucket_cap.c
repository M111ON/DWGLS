/* experiments/ann-climate-2026-09-24/sift/probe_perbucket_cap.c
 * ITEM 7.2 — per-bucket cap curve.
 *
 * QUESTION: bench_serve_overlap showed the shipped LZ_SEARCH_BUDGET=512 cutoff
 * craters recall 0.65 -> 0.26 (999/1000 queries truncated). Campaign section 10
 * says the fix "should become max(512, nent)" — but that is a GUESS with no
 * curve behind it. This probe measures the actual cap curve PER BUCKET SIZE and
 * decides what cap policy is recall-safe.
 *
 * METHOD (in-process, mirrors bench_serve_overlap candidate order exactly):
 *   route top-16 buckets -> SINGLE posting (A1 only) -> exact L2 top-10.
 *   Cap semantics: 0 means unbounded; any C > 0 caps members scored per bucket.
 *   For every cap C in CAPS[]: scan members until ns >= C, but the cap applies
 *   PER BUCKET (not global): each of the 16 buckets may score up to C members,
 *   in OFF order (walk order, as shipped).
 *   Bucket sizes come from off.bin (nent = off[b+1]-off[b]).
 *
 * KEY DIFFERENTIATOR vs bench_serve_overlap: cap is PER-BUCKET, and every
 * query's 16 buckets are classified by their max nent so the curve is reported
 * split into "small buckets" (all 16 <= median) vs "large buckets" (any > 512).
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_perbucket_cap.exe \
 *        experiments/ann-climate-2026-09-24/sift/probe_perbucket_cap.c -lm
 * RUN (repo root): build/probe_perbucket_cap.exe
 * Needs: build/sift1m/sift/*, build/sift1m_c/{F,pca_comp,pca_mean,off,mem}.bin
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
static int16_t *A1;
static int32_t *USEDB;
static int NUSED;

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
    free(raw); *n_out = n; *d_out = d; return out;
}
static float *load_bin(const char *path, size_t nfloat) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    float *p = (float *)malloc(nfloat * 4);
    if (!p || fread(p, 4, nfloat, f) != nfloat) { fprintf(stderr, "read fail %s\n", path); exit(1); }
    fclose(f); return p;
}
static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }
static void *load_raw(const char *path, size_t nbytes) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long fz = ftell(f); fseek(f, 0, SEEK_SET);
    if ((size_t)fz != nbytes) { fprintf(stderr, "size mismatch %s: got %ld want %ld\n", path, fz, (long)nbytes); exit(1); }
    void *p = malloc(nbytes ? nbytes : 1);
    if (!p || fread(p, 1, nbytes, f) != nbytes) { fprintf(stderr, "read fail %s\n", path); exit(1); }
    fclose(f); return p;
}
static void project(const float *x, float *z) {
    for (int c = 0; c < PDIM; c++) {
        double s = 0; const float *row = PC + (size_t)c * DIM;
        for (int j = 0; j < DIM; j++) s += (double)row[j] * ((double)x[j] - PM[j]);
        z[c] = (float)s;
    }
}
static void top2(const float *z, int *b1, int *b2) {
    float d1 = 1e30f, d2 = 1e30f; int i1 = -1, i2 = -1;
    for (int b = 0; b < K; b++) {
        const float *c = AA + (size_t)b * PDIM; float d = 0;
        for (int j = 0; j < PDIM; j++) { float e = z[j] - c[j]; d += e * e; }
        if (d < d1) { d2 = d1; i2 = i1; d1 = d; i1 = b; }
        else if (d < d2) { d2 = d; i2 = b; }
    }
    *b1 = i1; *b2 = i2;
}
static int topC(const float *z, int C, int *out) {
    static double dd[K];
    for (int b = 0; b < K; b++) {
        const float *c = AA + (size_t)b * PDIM; double d = 0;
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

/* cap boundaries to sweep (0 = unbounded) */
static const int CAPS[] = {0, 64, 128, 256, 366, 512, 768, 1024, 2048};
#define NCAPS ((int)(sizeof(CAPS)/sizeof(CAPS[0])))

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
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

    USEDB = (int32_t *)malloc((size_t)K * 4); NUSED = 0;
    for (int b = 0; b < K; b++) if (OFF[b + 1] > OFF[b]) USEDB[NUSED++] = b;
    printf("used buckets: %d\n", NUSED);

    /* bucket-size stats */
    {
        static int sz[K];
        for (int b = 0; b < K; b++) sz[b] = (int)(OFF[b + 1] - OFF[b]);
        int gt512 = 0;
        for (int i = 0; i < NUSED; i++) if (sz[USEDB[i]] > 512) gt512++;
        printf("buckets >512: %d/%d (%.1f%%)\n", gt512, NUSED, 100.0 * gt512 / NUSED);
    }

    /* A1 per entry (single posting, as shipped anchor) */
    t0 = now_ms();
    A1 = (int16_t *)malloc((size_t)NBASE * 2);
    for (int i = 0; i < NBASE; i++) {
        float zz[PDIM]; project(BASE + (size_t)i * DIM, zz);
        int b1, b2; top2(zz, &b1, &b2);
        A1[i] = (int16_t)b1;
    }
    printf("A1 build: 1M entries in %.1fs\n", (now_ms() - t0) / 1000.0);

    /* route all queries once, record their 16 buckets */
    static int selq[NQ][TOPB];
    static int qmaxnent[NQ];
    for (int qi = 0; qi < NQ; qi++) {
        float z[PDIM]; project(QUERY + (size_t)qi * DIM, z);
        int nb = topC(z, TOPB, selq[qi]);
        int mx = 0;
        for (int t = 0; t < nb; t++) {
            int n = (int)(OFF[selq[qi][t] + 1] - OFF[selq[qi][t]]);
            if (n > mx) mx = n;
        }
        qmaxnent[qi] = mx;
        for (int t = nb; t < TOPB; t++) selq[qi][t] = -1;
    }

    /* split queries: small (all 16 buckets <= 512) vs large (any > 512) */
    int nsmall = 0, nlarge = 0;
    for (int qi = 0; qi < NQ; qi++) { if (qmaxnent[qi] > 512) nlarge++; else nsmall++; }
    printf("query split by max bucket nent: small(<=512)=%d large(>512)=%d\n", nsmall, nlarge);

    /* sweep caps; cap applies PER BUCKET */
    printf("\n%-6s %-8s %-8s %-8s %-8s\n", "cap", "rec_all", "rec_small", "rec_large", "scored");
    for (int ci = 0; ci < NCAPS; ci++) {
        int cap = CAPS[ci];
        double rec = 0, recS = 0, recL = 0;
        long long nsc = 0;
        t0 = now_ms();
        for (int qi = 0; qi < NQ; qi++) {
            const float *qq = QUERY + (size_t)qi * DIM;
            double best[10]; int ans[10];
            for (int i = 0; i < 10; i++) { best[i] = 1e300; ans[i] = -1; }
            long long ns = 0;
            for (int t = 0; t < TOPB && selq[qi][t] >= 0; t++) {
                int b = selq[qi][t];
                int cnt = 0; /* PER-BUCKET scored counter */
                for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) {
                    int id = MEM[k];
                    if (A1[id] != b) continue;
                    const float *v = BASE + (size_t)id * DIM;
                    double d = 0;
                    for (int j = 0; j < DIM; j++) { double e = (double)qq[j] - v[j]; d += e * e; }
                    ns++; cnt++;
                    if (d < best[9]) {
                        int p = 9;
                        while (p > 0 && d < best[p - 1]) { best[p] = best[p - 1]; ans[p] = ans[p - 1]; p--; }
                        best[p] = d; ans[p] = id;
                    }
                    if (cap > 0 && cnt >= cap) break; /* per-bucket cutoff */
                }
            }
            double r = recall_at10(ans, GT + (size_t)qi * GTD);
            rec += r;
            if (qmaxnent[qi] > 512) recL += r; else recS += r;
            nsc += ns;
        }
        printf("%-6d %-8.4f %-8.4f %-8.4f %.0f\n",
               cap, rec / NQ, nsmall ? recS / nsmall : 0.0,
               nlarge ? recL / nlarge : 0.0, (double)nsc / NQ);
    }
    printf("\n(baseline repro: unbounded SINGLE = 0.6501, per campaign section 10)\n");
    return 0;
}
