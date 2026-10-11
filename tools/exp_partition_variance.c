/* exp_partition_variance.c — A2: does partition count K change routing
 * quality on REAL vectors? Read kv_index.jsonl (sid/dim/vec), train K
 * centroids with anch_train on a sample, assign ALL rows, report:
 *   - bucket occupancy (min/max/mean/std)
 *   - ambiguity = mean count of buckets whose squared-L2 is within 5% of best
 *     (the "overlap" number: how contested a query is across partitions)
 * K sweep 8/13/16/24. No floating verdict without a printed receipt.
 *
 * Build: gcc -O2 -std=c11 -Iinclude -Icore tools/exp_partition_variance.c -o build/exp_partition_variance.exe -lm
 * Run:   ./build/exp_partition_variance.exe [path] [sample]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "anchor_route.h"

#define DIM 384

/* minimal JSONL reader: pull dim-384 "vec":[...] floats from one line.
 * Returns count parsed (0 if line had no vec). Caller frees line via getline. */
static int parse_vec(const char *line, float *out) {
    const char *p = strstr(line, "\"vec\":[");
    if (!p) return 0;
    p += 7;
    int n = 0;
    while (n < DIM && *p && *p != ']') {
        char *end;
        double v = strtod(p, &end);
        if (end == p) break;
        out[n++] = (float)v;
        p = end;
        if (*p == ',') p++;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1]
                     : "I:/tools/ann/data/memcore/kv_index.jsonl";
    int sample = (argc > 2) ? atoi(argv[2]) : 8000;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open fail: %s\n", path); return 2; }

    float *X = (float *)malloc((size_t)sample * DIM * sizeof(float));
    int n = 0;
    static char line[1 << 20];   /* one jsonl row: dim-384 vec ~ 8 KB; 1 MB cap */
    while (n < sample && fgets(line, sizeof(line), f)) {
        if (parse_vec(line, X + (size_t)n * DIM) == DIM) n++;
    }
    long total = n;
    /* count the real corpus size for the assign sweep */
    while (fgets(line, sizeof(line), f)) { if (strstr(line, "\"vec\":[")) total++; }
    fclose(f);

    printf("=== A2 partition variance ===\n");
    printf("path=%s  sample=%d  dim=%d  corpus_rows=%ld\n", path, n, DIM, total);
    if (n < 8) { fprintf(stderr, "too few rows\n"); free(X); return 2; }

    int Ks[] = {8, 13, 16, 24};
    for (int ki = 0; ki < 4; ki++) {
        int K = Ks[ki];
        float *C = (float *)malloc((size_t)K * DIM * sizeof(float));
        int *lab = (int *)malloc((size_t)n * sizeof(int));
        int rc = anch_train(X, n, DIM, K, C, lab);
        if (rc != 0) { printf("K=%d anch_train rc=%d\n", K, rc); free(C); free(lab); continue; }

        long *occ = (long *)calloc(K, sizeof(long));
        double amb_sum = 0; int amb_n = 0;
        for (int i = 0; i < n; i++) {
            const float *q = X + (size_t)i * DIM;
            int best = anch_assign(q, C, K, DIM);
            occ[best]++;
            /* ambiguity: count buckets within 5% of best sq-L2 */
            double bd = DBL_MAX;
            for (int k = 0; k < K; k++) {
                double d = 0; const float *c = C + (size_t)k * DIM;
                for (int j = 0; j < DIM; j++) { double e = q[j]-c[j]; d += e*e; }
                if (d < bd) bd = d;
            }
            int near = 0;
            for (int k = 0; k < K; k++) {
                double d = 0; const float *c = C + (size_t)k * DIM;
                for (int j = 0; j < DIM; j++) { double e = q[j]-c[j]; d += e*e; }
                if (d <= bd * 1.05) near++;
            }
            amb_sum += near; amb_n++;
        }
        long mn = occ[0], mx = occ[0]; double sum = 0;
        for (int k = 0; k < K; k++) { if (occ[k]<mn) mn=occ[k]; if (occ[k]>mx) mx=occ[k]; sum += occ[k]; }
        double mean = sum / K, var = 0;
        for (int k = 0; k < K; k++) { double d = occ[k]-mean; var += d*d; }
        var /= K;
        printf("K=%-3d  occ min=%-6ld max=%-6ld mean=%-8.1f std=%-8.1f  ambiguity=%.3f  (n=%d)\n",
               K, mn, mx, mean, sqrt(var), amb_sum/amb_n, amb_n);
        free(occ); free(C); free(lab);
    }
    free(X);
    printf("RESULT: DONE\n");
    return 0;
}
