/* experiments/ann-climate-2026-09-24/sift/probe_route_only.c — route-only (no rerank) probe.
 *
 * OWNER (2026-09-25): every probe so far plays THEIR game (assign -> scan ->
 * brute-force rerank as safety net). Our own structure = address IS the answer:
 * query -> anchor -> bucket members directly, zero distance compute past routing.
 * This probe measures the gap between where we stand and that goal.
 *
 * CHOICE DOCUMENTED:
 * - R1: top-1 bucket, answer = its first 10 member ids (purest address=answer).
 * - R16: top-16 buckets, answer = round-robin interleave, first 10
 *   (same routing cost as baseline Arm A, zero rerank).
 * - Baseline A (PCA25 + scan + exact rerank, expect 0.8023) rerun as sanity.
 * Member order = build order in MEM (arbitrary) — that arbitrariness IS the
 * thing measured: if route-only recall is far below A, the rerank net is
 * load-bearing and "our own structure" needs an ordering principle inside
 * buckets (Phase 3 Wang gate / line-sum address candidate).
 *
 * GATE: route-only is VIABLE iff R16 recall >= A-0.05 at strictly lower ms/q.
 * Else: gap recorded, rerank stays, ordering-inside-bucket becomes the open work.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -o build/probe_route_only.exe experiments/ann-climate-2026-09-24/sift/probe_route_only.c -lm
 * RUN (repo root): build/probe_route_only.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define DIM 128
#define PDIM 25
#define NB 4096
#define NUSED 2560
#define NQ 1000
#define TOPC 16

static float *FC, *PC, *PM, *BASE, *QUERY;
static int32_t *GT, *MEM;
static int64_t *OFF;
static int NBASE, NQRY, GTD;
static int AID[NUSED];

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

static void project(const float *x, float *z) {
    for (int i = 0; i < PDIM; i++) {
        double s = 0;
        const float *row = PC + (size_t)i * DIM;
        for (int j = 0; j < DIM; j++) s += (double)row[j] * (x[j] - PM[j]);
        z[i] = (float)s;
    }
}
static void topC_used(const float *z, int *out, double *dw) {
    for (int i = 0; i < NUSED; i++) {
        const float *c = FC + (size_t)AID[i] * PDIM;
        double d = 0;
        for (int j = 0; j < PDIM; j++) { double e = (double)z[j] - c[j]; d += e * e; }
        dw[i] = d;
    }
    for (int c = 0; c < TOPC; c++) {
        int bi = c;
        for (int i = c + 1; i < NUSED; i++)
            if (dw[i] < dw[bi]) bi = i;
        double td = dw[c]; dw[c] = dw[bi]; dw[bi] = td;
        out[c] = bi;
    }
}
static void rerank10(const float *q, const int *ids, int ns, int *besi) {
    double best[10];
    for (int i = 0; i < 10; i++) { best[i] = 1e300; besi[i] = -1; }
    for (int i = 0; i < ns; i++) {
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
    FC = load_bin("build/sift1m_c/F.bin", (size_t)NB * PDIM);
    { FILE *f = fopen("build/sift1m_c/off.bin", "rb");
      if (!f) { fprintf(stderr, "missing off.bin\n"); return 1; }
      OFF = (int64_t *)malloc(((size_t)NB + 1) * 8);
      if (fread(OFF, 8, NB + 1, f) != (size_t)NB + 1) { fprintf(stderr, "off read\n"); return 1; }
      fclose(f); }
    MEM = (int32_t *)load_bin("build/sift1m_c/mem.bin", 1000000);
    int nu = 0;
    for (int b = 0; b < NB && nu < NUSED; b++)
        if (OFF[b + 1] > OFF[b]) AID[nu++] = b;
    if (nu != NUSED) { fprintf(stderr, "used count %d != %d\n", nu, NUSED); return 1; }
    printf("load: base %d query %d gt-dim %d used %d in %.1fs\n",
           NBASE, NQRY, GTD, nu, (now_ms() - t0) / 1000.0);

    int *scan = (int *)malloc((size_t)NBASE * 4);
    int32_t *stamp = (int32_t *)calloc(NBASE, 4);
    int32_t cur = 1;
    double *dw = (double *)malloc((size_t)NUSED * 8);
    int top[TOPC], besi[10], ans[10];
    float *zq = (float *)malloc(PDIM * 4);
    double recA = 0, recR1 = 0, recR16 = 0;
    long long nscanA = 0;
    double tq;

    /* A: baseline with rerank (sanity, expect 0.8023) */
    tq = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        const float *q = QUERY + (size_t)qi * DIM;
        project(q, zq);
        topC_used(zq, top, dw);
        int ns = 0;
        cur++;
        for (int c = 0; c < TOPC; c++) {
            int b = AID[top[c]];
            for (int64_t k = OFF[b]; k < OFF[b + 1]; k++) {
                int id = MEM[k];
                if (stamp[id] != cur) { stamp[id] = cur; scan[ns++] = id; }
            }
        }
        rerank10(q, scan, ns, besi);
        nscanA += ns;
        recA += recall_at10(besi, GT + (size_t)qi * GTD);
    }
    double msA = (now_ms() - tq) / NQ;
    recA /= NQ;
    printf("A rerank : recall=%.4f scan=%.0f (%.3f%%) %.2fms/q\n",
           recA, (double)nscanA / NQ, (double)nscanA / NQ / NBASE * 100.0, msA);

    /* R1: top-1 bucket, first 10 members, zero distance compute past routing */
    tq = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        const float *q = QUERY + (size_t)qi * DIM;
        (void)q;
        project(QUERY + (size_t)qi * DIM, zq);
        topC_used(zq, top, dw);
        int b = AID[top[0]];
        int64_t nb = OFF[b + 1] - OFF[b];
        for (int i = 0; i < 10; i++)
            ans[i] = (i < nb) ? MEM[OFF[b] + i] : -1;
        recR1 += recall_at10(ans, GT + (size_t)qi * GTD);
    }
    double msR1 = (now_ms() - tq) / NQ;
    recR1 /= NQ;
    printf("R1 route-only top-1 first-10: recall=%.4f (d=%+.4f) %.2fms/q\n", recR1, recR1 - recA, msR1);

    /* R16: top-16 buckets round-robin interleave, first 10, zero rerank */
    tq = now_ms();
    for (int qi = 0; qi < NQ; qi++) {
        project(QUERY + (size_t)qi * DIM, zq);
        topC_used(zq, top, dw);
        int64_t pos[TOPC];
        for (int c = 0; c < TOPC; c++) pos[c] = OFF[AID[top[c]]];
        int n = 0, guard = 0;
        while (n < 10 && guard < 10 * TOPC * 1024) {
            for (int c = 0; c < TOPC && n < 10; c++) {
                int64_t end = OFF[AID[top[c]] + 1];
                if (pos[c] < end) ans[n++] = MEM[pos[c]++];
            }
            guard++;
            int alive = 0;
            for (int c = 0; c < TOPC; c++) if (pos[c] < OFF[AID[top[c]] + 1]) { alive = 1; break; }
            if (!alive) break;
        }
        while (n < 10) ans[n++] = -1;
        recR16 += recall_at10(ans, GT + (size_t)qi * GTD);
    }
    double msR16 = (now_ms() - tq) / NQ;
    recR16 /= NQ;
    printf("R16 route-only top-16 round-robin: recall=%.4f (d=%+.4f) %.2fms/q\n", recR16, recR16 - recA, msR16);

    int win = (recR16 >= recA - 0.05) && (msR16 < msA);
    printf("GATE: route-only %s (R16>=A-0.05 %s, ms<A %s) — rerank contributes %+.4f recall\n",
           win ? "VIABLE" : "NOT VIABLE",
           recR16 >= recA - 0.05 ? "yes" : "no", msR16 < msA ? "yes" : "no", recA - recR16);
    return 0;
}
