/* exp_partition_ray.c — measure partition variance + ray order on real ANN index
 * Build: gcc -O2 -o build/exp_partition_ray.exe tools/exp_partition_ray.c -lm
 * Run: ./build/exp_partition_ray.exe I:\tools\ann\data\memcore\kv_index.jsonl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define MAX_LINE 2000000
#define DIM 384
#define MAX_K 32

typedef float Vec[DIM];

static double dot384(const float *a, const float *b) {
    double s = 0;
    for (int i = 0; i < DIM; i++) s += (double)a[i] * (double)b[i];
    return s;
}

static double norm384(const float *a) {
    double s = 0;
    for (int i = 0; i < DIM; i++) s += (double)a[i] * (double)a[i];
    return sqrt(s);
}

static double cosine(const float *a, const float *b) {
    double na = norm384(a), nb = norm384(b);
    if (na == 0 || nb == 0) return 0;
    return dot384(a, b) / (na * nb);
}

/* parse one JSONL line, extract vec[384] into out (returns 1 on success) */
static int parse_vec(const char *line, float *out) {
    /* "vec":[...] and "vec": [...] are both valid JSONL: the C writer emits the
     * compact form, python json.dumps emits the spaced one. Match the key, then
     * skip punctuation/whitespace instead of hard-coding the compact bytes. */
    const char *p = strstr(line, "\"vec\"");
    if (!p) return 0;
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return 0;
    p++;
    for (int i = 0; i < DIM; i++) {
        while (*p == ' ' || *p == '\t') p++;
        char *end;
        out[i] = strtof(p, &end);
        if (end == p) return 0;
        p = end;
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
    return 1;
}

/* k-means++ init + Lloyd (returns assignments in assign[], centroids in cents[]) */
static void kmeans_pp(Vec *data, int n, int K, Vec *cents, int *assign) {
    /* init: first centroid random */
    memcpy(cents[0], data[rand() % n], sizeof(Vec));
    for (int k = 1; k < K; k++) {
        double *d2 = (double*)malloc(n * sizeof(double));
        for (int i = 0; i < n; i++) {
            double best = 1e100;
            for (int j = 0; j < k; j++) {
                double d = 1.0 - cosine(data[i], cents[j]);
                if (d < best) best = d;
            }
            d2[i] = best;
        }
        double sum = 0; for (int i = 0; i < n; i++) sum += d2[i];
        double r = (double)rand() / RAND_MAX * sum;
        int idx = 0; while (r > 0 && idx < n) r -= d2[idx++];
        memcpy(cents[k], data[idx == n ? n-1 : idx], sizeof(Vec));
        free(d2);
    }
    /* Lloyd iterations */
    int *counts = (int*)calloc(K, sizeof(int));
    for (int iter = 0; iter < 10; iter++) {
        memset(counts, 0, K * sizeof(int));
        for (int i = 0; i < n; i++) {
            double best = 1e100; int bestk = 0;
            for (int k = 0; k < K; k++) {
                double d = 1.0 - cosine(data[i], cents[k]);
                if (d < best) { best = d; bestk = k; }
            }
            assign[i] = bestk;
            counts[bestk]++;
        }
        for (int k = 0; k < K; k++) {
            if (counts[k] == 0) continue;
            for (int d = 0; d < DIM; d++) cents[k][d] = 0;
            for (int i = 0; i < n; i++) if (assign[i] == k)
                for (int d = 0; d < DIM; d++) cents[k][d] += data[i][d];
            for (int d = 0; d < DIM; d++) cents[k][d] /= counts[k];
        }
    }
    free(counts);
}

/* measure cell stats for one K */
static void measure_partition(Vec *data, int n, int K, Vec *cents, int *assign) {
    int *counts = (int*)calloc(K, sizeof(int));
    for (int i = 0; i < n; i++) counts[assign[i]]++;
    
    int max_c = 0, min_c = n;
    double sum = 0, sumsq = 0;
    for (int k = 0; k < K; k++) {
        if (counts[k] > max_c) max_c = counts[k];
        if (counts[k] < min_c) min_c = counts[k];
        sum += counts[k];
        sumsq += (double)counts[k] * counts[k];
    }
    double mean = sum / K;
    double variance = sumsq / K - mean * mean;
    double ratio = (double)max_c / mean;
    
    printf("K=%2d | max=%6d | mean=%7.1f | ratio=%.3f | variance=%.1f\n",
           K, max_c, mean, ratio, variance);
    free(counts);
}

/* ray order test: in each cell, sort by cos(vec, centroid), sweep top-k */
static void measure_ray_order(Vec *data, int n, int K, Vec *cents, int *assign) {
    int *counts = (int*)calloc(K, sizeof(int));
    for (int i = 0; i < n; i++) counts[assign[i]]++;
    
    int *offsets = (int*)malloc(K * sizeof(int));
    offsets[0] = 0;
    for (int k = 1; k < K; k++) offsets[k] = offsets[k-1] + counts[k-1];
    
    int *indices = (int*)malloc(n * sizeof(int));
    /* bucket fill must not consume the offsets: the sweep below needs them.
     * (The original incremented offsets in place and then "recomputed" them from
     *  the already-incremented values, so start/end ran past n.) */
    int *cursor = (int*)malloc(K * sizeof(int));
    memcpy(cursor, offsets, K * sizeof(int));
    for (int i = 0; i < n; i++) indices[cursor[assign[i]]++] = i;
    free(cursor);
    
    /* for each cell, sort by cos(vec, centroid) desc */
    float *cos_sim = (float*)malloc(n * sizeof(float));
    for (int k = 0; k < K; k++) {
        int start = offsets[k], end = start + counts[k];
        for (int i = start; i < end; i++) {
            int idx = indices[i];
            cos_sim[i] = (float)cosine(data[idx], cents[k]);
        }
        /* simple insertion sort for small arrays */
        for (int i = start + 1; i < end; i++) {
            int j = i;
            int idx = indices[i];
            float cs = cos_sim[i];
            while (j > start && cos_sim[j-1] < cs) {
                indices[j] = indices[j-1];
                cos_sim[j] = cos_sim[j-1];
                j--;
            }
            indices[j] = idx;
            cos_sim[j] = cs;
        }
    }
    
    /* leave-one-out: for each vector, rank in its cell after sorting */
    int rank_hist[8002] = {0};  // rank 1..8000, plus the 8001 overflow bucket
    for (int k = 0; k < K; k++) {
        int start = offsets[k], end = start + counts[k];
        for (int pos = start; pos < end; pos++) {
            int rank = pos - start + 1;
            if (rank > 8000) rank = 8001;
            rank_hist[rank]++;
        }
    }
    
    /* compute overlap@k: fraction of vectors with rank <= k */
    int total = n;
    static const int ray_ks[] = {200, 500, 1000, 2000, 4000, 8000};
    for (size_t rk = 0; rk < sizeof(ray_ks) / sizeof(ray_ks[0]); rk++) {
        int k = ray_ks[rk];
        int covered = 0;
        for (int r = 1; r <= k; r++) covered += rank_hist[r];
        printf("  ray k=%4d: items_scored=%d/%d (%.1f%%) overlap=%.3f\n",
               k, k, total, 100.0 * k / total, (double)covered / total);
    }
    free(counts); free(offsets); free(indices); free(cos_sim);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <kv_index.jsonl>\n", argv[0]);
        return 1;
    }
    
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("open"); return 1; }
    
    /* count lines */
    int n = 0;
    char *line = (char*)malloc(MAX_LINE);
    while (fgets(line, MAX_LINE, f)) n++;
    rewind(f);
    printf("Loaded %d vectors from %s\n", n, argv[1]);
    
    Vec *data = (Vec*)malloc(n * sizeof(Vec));
    int loaded = 0;
    while (fgets(line, MAX_LINE, f) && loaded < n) {
        if (parse_vec(line, data[loaded])) loaded++;
    }
    fclose(f);
    free(line);
    printf("Parsed %d vec[384]\n", loaded);
    n = loaded;
    if (n <= 0) {
        fprintf(stderr, "no vectors parsed from %s - refusing to run kmeans on 0 rows\n", argv[1]);
        return 1;
    }
    
    int *assign = (int*)malloc(n * sizeof(int));
    Vec cents[MAX_K];
    
    static const int ks[] = {8, 13, 16, 24};
    for (size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
        int K = ks[ki];
        printf("\n=== K=%d ===\n", K);
        kmeans_pp(data, n, K, cents, assign);
        measure_partition(data, n, K, cents, assign);
        measure_ray_order(data, n, K, cents, assign);
    }
    
    free(data); free(assign);
    return 0;
}