#include <anchor_route.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int old_route(const float *q, const float *C, int K, int dim, int topb, int *out) {
    if (!q || !C || !out || K <= 0 || dim <= 0 || topb <= 0) return 0;
    if (topb > K) topb = K;
    double *bd = (double *)malloc((size_t)K * sizeof(double));
    if (!bd) return 0;
    for (int k = 0; k < K; k++) {
        double d = 0;
        const float *c = C + (size_t)k * dim;
        for (int j = 0; j < dim; j++) {
            double e = (double)q[j] - c[j];
            d += e * e;
        }
        bd[k] = d;
        out[k] = k;
    }
    for (int a = 0; a < topb; a++)
        for (int b = a + 1; b < K; b++)
            if (bd[out[b]] < bd[out[a]]) { int t = out[a]; out[a] = out[b]; out[b] = t; }
    free(bd);
    return topb;
}

int main(void) {
    srand(7);
    for (int t = 0; t < 2000; t++) {
        int K = 1 + rand() % 70;
        int dim = 1 + rand() % 8;
        int topb = 1 + rand() % 10;
        if (topb > K) topb = K;
        float *C = malloc((size_t)K * dim * sizeof(float));
        float *q = malloc((size_t)dim * sizeof(float));
        for (int i = 0; i < K * dim; i++) C[i] = (float)(rand() % 4);
        for (int i = 0; i < dim; i++) q[i] = (float)(rand() % 4);
        int *oldout = malloc((size_t)K * sizeof(int));
        int *newout = malloc((size_t)topb * sizeof(int));
        int nold = old_route(q, C, K, dim, topb, oldout);
        int nnew = anch_route(q, C, K, dim, topb, newout);
        if (nold != nnew) { printf("count mismatch %d %d\n", nold, nnew); return 1; }
        if (memcmp(oldout, newout, (size_t)topb * sizeof(int)) != 0) { printf("data mismatch\n"); return 1; }
        free(C); free(q); free(oldout); free(newout);
    }
    printf("ok\n");
    return 0;
}
