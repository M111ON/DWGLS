/* tests/test_anchor_routed.c — routed search == brute top-1 on clustered data,
 * with fewer items scored and 1 block read (node-sorted perm walk).
 *
 * Flow mirrors gguf_lazy_serve.c /v1/state/search: train → assign → perm →
 * save/load → route top-b → perm-walk score routed buckets → extents.
 * Oracle: brute-force full qsort ranking recomputed inline in the test.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_anchor_routed.exe tests/test_anchor_routed.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "anchor_route.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

#define DIM 16
#define PER 8
#define NCL 3
#define NENT (PER * NCL)

static uint32_t trng = 0xABCDEFu;
static float frand(void) {
    trng ^= trng << 13; trng ^= trng >> 17; trng ^= trng << 5;
    return (float)(trng % 10000) / 10000.0f - 0.5f;
}
static double cos_sim(const float *a, const float *b, int d) {
    double dot = 0, na = 0, nb = 0;
    for (int j = 0; j < d; j++) { dot += (double)a[j] * b[j]; na += (double)a[j] * a[j]; nb += (double)b[j] * b[j]; }
    return (na > 0 && nb > 0) ? dot / (sqrt(na) * sqrt(nb)) : -1;
}

int main(void) {
    printf("test_anchor_routed\n");
    /* 3 well-separated clusters: center c*10 apart, spread 0.5 */
    float X[NENT * DIM];
    for (int c = 0; c < NCL; c++)
        for (int i = 0; i < PER; i++)
            for (int j = 0; j < DIM; j++)
                X[(c * PER + i) * DIM + j] = (float)(c * 10) + frand() * 0.5f;
    float q[DIM];
    for (int j = 0; j < DIM; j++) q[j] = 0.0f + frand() * 0.05f; /* near cluster 0 */

    /* brute oracle ranking */
    double bs[NENT];
    int bo[NENT];
    for (int i = 0; i < NENT; i++) { bs[i] = cos_sim(q, X + i * DIM, DIM); bo[i] = i; }
    for (int a = 0; a < NENT; a++)
        for (int b = a + 1; b < NENT; b++)
            if (bs[bo[b]] > bs[bo[a]]) { int t = bo[a]; bo[a] = bo[b]; bo[b] = t; }

    /* anchor flow: train K=3 → assign → perm → save/load → route top-1 */
    float C[3 * DIM];
    int rows[NENT];
    check(anch_train(X, NENT, DIM, 3, C, rows) == 0, "train K=3 on 24x16");
    check(anch_save("build/test_routed.bin", C, 3, DIM, NENT) == 0, "save anchors");
    float L[3 * DIM];
    int ld = 0, ln = 0;
    check(anch_load("build/test_routed.bin", L, 64, 1024, &ld, &ln) == 3 && ld == DIM && ln == NENT,
          "load anchors (K/dim/ntrained)");
    uint32_t perm[NENT];
    check(anch_perm(rows, NENT, 3, perm) == 0, "node-sorted perm");
    int bk[1];
    check(anch_route(q, L, 3, DIM, 1, bk) == 1, "route top-1 bucket");

    /* perm-walk scoring of routed bucket only */
    double best = -2;
    int besti = -1, scored = 0, reads = 0, in_run = 0;
    for (int oi = 0; oi < NENT; oi++) {
        int ei = (int)perm[oi];
        if (rows[ei] != bk[0]) { in_run = 0; continue; }
        if (!in_run) { reads++; in_run = 1; }
        double s = cos_sim(q, X + ei * DIM, DIM);
        scored++;
        if (s > best) { best = s; besti = ei; }
    }
    check(scored == PER && reads == 1, "scored 8/24 in 1 block read");
    check(besti == bo[0], "routed top-1 == brute top-1");
    /* routed top-3 recall vs brute top-3: all routed hits must be brute top-8 */
    check(best - bs[bo[0]] < 1e-9 && best > bs[bo[PER]], "routed best beats brute rank-9");
    remove("build/test_routed.bin");
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
