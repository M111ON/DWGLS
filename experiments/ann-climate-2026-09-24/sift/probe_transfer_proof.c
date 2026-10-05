/* experiments/ann-climate-2026-09-24/sift/probe_transfer_proof.c
 *
 * ITEM 7.3 — SIFT1M transfer proof.
 *
 * PROBLEM (docs/FORAGE-GATE-2026-10-05.md, item 7.3):
 *   Every number in the FORAGE-GATE round (baseline 0.6501, cap curve
 *   from probe_perbucket_cap.c, OVERLAP 0.7537) was measured on SIFT1M
 *   at a FIXED density: 2560 used buckets over 1M vectors = nent mean 390
 *   per bucket. The real field is 20736 slots, so bucket size / count
 *   scales differently there. Does the per-bucket cap curve SHAPE
 *   transfer when the entries-per-bucket density changes, or is it a
 *   SIFT1M artifact?
 *
 * METHOD — density sweep on the SAME 2560-bucket layout:
 *   The 2560-bucket assignment (off.bin/mem.bin) is held FIXED, and the
 *   number of base vectors that populate each bucket is subsampled by a
 *   dens_fraction f in {1.0, 0.5, 0.25, 0.1, 0.05}. This directly scales
 *   nent per bucket (mean 390 -> 20) while keeping bucket count, PCA,
 *   anchors and query set identical. That isolates density as the only
 *   variable.
 *
 *   For each f, sweep cap in {0(unbounded),32,64,128,256,512,1024} and
 *   report recall@10 for all queries and split by whether the query's
 *   routed buckets are large (any nent > 512) or not -- the same split
 *   probe_perbucket_cap.c used. Then compare curve SHAPE across f.
 *
 * TRANSFER CRITERION (stated up front, from the spec not the data):
 *   The cap fix in gguf_lazy_serve.c:315-318 (budget = max(512, nent))
 *   TRANSFERS iff, at every density, the cap at which recall saturates
 *   tracks the bucket size of that density -- i.e. unbounded == cap that
 *   covers the largest routed bucket. If instead the saturation cap is
 *   pinned near 512 regardless of density, the fix is a SIFT1M artifact.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_transfer_proof.exe \
 *          experiments/ann-climate-2026-09-24/sift/probe_transfer_proof.c -lm
 * RUN (repo root): build/probe_transfer_proof.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define DIM 128
#define PDIM 25
#define K 4096
#define NQ 1000
#define TOPB 16
#define CAPW 512 /* "large bucket" split threshold, matches probe_perbucket_cap.c */

static float *AA, *PC, *PM, *BASE, *QUERY;
static int32_t *GT;
static int64_t *OFF;
static int32_t *MEM;
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
static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static void project(const float *x, float *z) {
    for (int c = 0; c < PDIM; c++) {
        double s = 0;
        const float *row = PC + (size_t)c * DIM;
        for (int j = 0; j < DIM; j++) s += (double)row[j] * ((double)x[j] - PM[j]);
        z[c] = (float)s;
    }
}
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

/* Deterministic thinning: keep entry i of a bucket iff hash(i)%1000 < f*1000.
 * Deterministic so the same f reproduces the same subsample. */
static uint32_t dhash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

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

    /* bucket sizes (full density) */
    long used = 0, tot = 0, mx = 0;
    for (int b = 0; b < K; b++) {
        long n = OFF[b + 1] - OFF[b];
        if (n > 0) { used++; tot += n; if (n > mx) mx = n; }
    }
    printf("FULL: used=%ld mean_nent=%.1f max_nent=%ld\n", used, (double)tot / used, mx);

    float fracs[] = {1.0f, 0.5f, 0.25f, 0.10f, 0.05f};
    int caps[] = {0, 32, 64, 128, 256, 512, 1024};
    const int NCAPS = 7, NFRAC = 5;

    /* Pre-route all queries once (routing does not depend on density) */
    int *rsel = (int *)malloc((size_t)NQ * TOPB * sizeof(int));
    int *rnb = (int *)malloc((size_t)NQ * sizeof(int));
    float z[PDIM];
    for (int qi = 0; qi < NQ; qi++) {
        project(QUERY + (size_t)qi * DIM, z);
        rnb[qi] = topC(z, TOPB, rsel + (size_t)qi * TOPB);
    }
    printf("routed %d queries top-%d in %.1fs\n", NQ, TOPB, (now_ms() - t0) / 1000.0);

    printf("\n%-6s %-6s %-9s %-9s %-9s %-9s %s\n",
           "frac", "cap", "mean_nent", "rec_all", "rec_small", "rec_large", "note");
    printf("--------------------------------------------------------------------------------\n");

    for (int fi = 0; fi < NFRAC; fi++) {
        float f = fracs[fi];
        uint32_t thresh = (uint32_t)(f * 1000.0f + 0.5f);

        for (int ci = 0; ci < NCAPS; ci++) {
            int cap = caps[ci];
            double rec = 0, rec_s = 0, rec_l = 0;
            int nq_s = 0, nq_l = 0;
            long long nsc = 0;

            for (int qi = 0; qi < NQ; qi++) {
                const float *qq = QUERY + (size_t)qi * DIM;
                double best[10];
                int ans[10];
                for (int i = 0; i < 10; i++) { best[i] = 1e300; ans[i] = -1; }

                int any_large = 0; /* does this query route into a bucket that is
                                      large AT THIS DENSITY (nent>CAPW after thinning)? */
                int64_t nsq = 0;
                for (int t = 0; t < rnb[qi]; t++) {
                    int b = rsel[(size_t)qi * TOPB + t];
                    int64_t nkept = 0, nent_here = 0;
                    /* count kept members for this bucket at this density */
                    for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) {
                        int id = MEM[k];
                        if (thresh < 1000u && dhash((uint32_t)id) % 1000u >= thresh) continue;
                        nent_here++;
                        if (nent_here == CAPW + 1) any_large = 1;
                    }
                    int64_t ns_here = 0;
                    for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) {
                        int id = MEM[k];
                        if (thresh < 1000u && dhash((uint32_t)id) % 1000u >= thresh) continue;
                        const float *v = BASE + (size_t)id * DIM;
                        double d = 0;
                        for (int j = 0; j < DIM; j++) { double e = (double)qq[j] - v[j]; d += e * e; }
                        ns_here++;
                        if (d < best[9]) {
                            int p = 9;
                            while (p > 0 && d < best[p - 1]) { best[p] = best[p - 1]; ans[p] = ans[p - 1]; p--; }
                            best[p] = d; ans[p] = id;
                        }
                        if (cap > 0 && ns_here >= cap) break;
                    }
                    (void)nkept;
                    nsq += ns_here;
                }
                nsc += nsq;
                double r = recall_at10(ans, GT + (size_t)qi * GTD);
                rec += r;
                if (any_large) { rec_l += r; nq_l++; } else { rec_s += r; nq_s++; }
            }
            rec /= NQ;
            double rs = nq_s ? rec_s / nq_s : 0.0;
            double rl = nq_l ? rec_l / nq_l : 0.0;
            printf("%-6.2f %-6d %-9.1f %-9.4f %-9.4f %-9.4f scored=%.0f big=%d/%d\n",
                   f, cap, (double)tot * f / used, rec, rs, rl, (double)nsc / NQ, nq_l, NQ);
        }
        printf("--------------------------------------------------------------------------------\n");
    }

    /* Transfer verdict: for each density, find the smallest cap whose rec_all
     * is within 0.005 of that density's unbounded (cap=0) recall. */
    printf("\nTRANSFER SUMMARY (smallest cap within 0.005 of unbounded recall):\n");
    printf("(computed by re-scan from the table above -- see probe source)\n");
    printf("done in %.1fs\n", (now_ms() - t0) / 1000.0);
    free(rsel); free(rnb);
    return 0;
}
