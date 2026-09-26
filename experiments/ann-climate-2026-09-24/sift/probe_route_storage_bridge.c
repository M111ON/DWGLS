/* probe_route_storage_bridge.c
 *
 * RouteField and TensorStore are deliberately separate.  The route is an
 * ephemeral 1->4->16->64 coordinate path; storage resolves its leaf to a
 * byte span in one tensor.  No bucket/tree/graph is persisted.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define DIM 128
#define LEAVES 64
#define TOPK 10
#define MAXQ 100
#define VEC_BYTES (DIM * sizeof(float))

typedef struct { int leaf, level, branch[3]; } Route;
typedef struct { const uint8_t *base; size_t bytes; int32_t *ids; int32_t *off; } TensorStore;

static float *base, *query, *pc, *pm;
static int32_t *gt;
static float *px, *py;
static int nb, nq, ngt, gd;

static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static void *load_fvecs(const char *path, int *n, int *d) {
    FILE *f = fopen(path, "rb"); int32_t dim; long size; void *out;
    if (!f) { perror(path); return NULL; }
    if (fread(&dim, 4, 1, f) != 1 || dim <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    *d = dim; *n = (int)(size / ((long)(dim + 1) * 4));
    out = malloc((size_t)*n * dim * 4);
    if (!out) { fclose(f); return NULL; }
    for (int i = 0; i < *n; i++) {
        int32_t got;
        if (fread(&got, 4, 1, f) != 1 || got != dim ||
            fread((char *)out + (size_t)i * dim * 4, 4, dim, f) != (size_t)dim) {
            free(out); fclose(f); return NULL;
        }
    }
    fclose(f); return out;
}

static void *load_bin(const char *path, size_t count) {
    FILE *f = fopen(path, "rb"); void *p = malloc(count * sizeof(float));
    if (!f || !p || fread(p, sizeof(float), count, f) != count) {
        if (f) fclose(f);
        free(p);
        return NULL;
    }
    fclose(f); return p;
}

static void project2(const float *v, float *x, float *y) {
    double sx = 0, sy = 0;
    for (int j = 0; j < DIM; j++) {
        double c = (double)v[j] - pm[j];
        sx += c * pc[j]; sy += c * pc[DIM + j];
    }
    *x = (float)sx; *y = (float)sy;
}

static int cell(float x, float y, float xmin, float xmax, float ymin, float ymax) {
    int ix = (int)(((double)x - xmin) / (xmax - xmin) * 8.0);
    int iy = (int)(((double)y - ymin) / (ymax - ymin) * 8.0);
    if (ix < 0) ix = 0;
    if (ix > 7) ix = 7;
    if (iy < 0) iy = 0;
    if (iy > 7) iy = 7;
    return iy * 8 + ix;
}

static Route route_make(int leaf) {
    Route r = { leaf, 3, { leaf >> 4, (leaf >> 2) & 3, leaf & 3 } };
    return r;
}

static uint64_t checksum(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static int recall10(const int *ans, const int32_t *truth) {
    int hit = 0;
    for (int i = 0; i < TOPK; i++) for (int j = 0; j < 10 && ans[i] >= 0; j++)
        if (ans[i] == truth[j]) { hit++; break; }
    return hit;
}

static double dist128(const float *a, const float *b) {
    double s = 0;
    for (int j = 0; j < DIM; j++) { double d = (double)a[j] - b[j]; s += d * d; }
    return s;
}

static int resolve_route(const Route *r, const float *q, int radius, const TensorStore *store,
                         int *seen, int *out, int *nscan, size_t *bytes,
                         uint64_t *span_hash) {
    double best[TOPK]; int best_id[TOPK], nbest = 0, n = 0;
    int cx = r->leaf & 7, cy = r->leaf >> 3;
    for (int dy = -radius; dy <= radius; dy++) for (int dx = -radius; dx <= radius; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x < 0 || x > 7 || y < 0 || y > 7) continue;
        int c = y * 8 + x;
        int32_t begin = store->off[c], end = store->off[c + 1];
        for (int32_t p = begin; p < end; p++) {
            int id = store->ids[p];
            if (seen[id]) continue;
            seen[id] = 1; n++; (*nscan)++;
            const uint8_t *tensor_span = store->base + (size_t)id * VEC_BYTES;
            *bytes += VEC_BYTES;
            *span_hash ^= checksum(tensor_span, VEC_BYTES);
            double d = dist128(q, (const float *)tensor_span);
            if (nbest < TOPK || d < best[TOPK - 1]) {
                int k = nbest < TOPK ? nbest++ : TOPK - 1;
                while (k > 0 && d < best[k - 1]) {
                    best[k] = best[k - 1]; best_id[k] = best_id[k - 1]; k--;
                }
                best[k] = d; best_id[k] = id;
            }
        }
    }
    for (int i = 0; i < TOPK; i++) out[i] = i < nbest ? best_id[i] : -1;
    return n;
}

int main(int argc, char **argv) {
    int d, qd, requested = MAXQ; float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
    base = load_fvecs("build/sift1m/sift/sift_base.fvecs", &nb, &d);
    query = load_fvecs("build/sift1m/sift/sift_query.fvecs", &nq, &qd);
    gt = (int32_t *)load_fvecs("build/sift1m/sift/sift_groundtruth.ivecs", &ngt, &gd);
    pc = load_bin("build/sift1m_c/pca_comp.bin", (size_t)2 * DIM);
    pm = load_bin("build/sift1m_c/pca_mean.bin", DIM);
    if (argc > 1) requested = atoi(argv[1]);
    if (requested > 0 && requested < nq) nq = requested;
    if (nq > MAXQ) nq = MAXQ;
    if (!base || !query || !gt || !pc || !pm || d != DIM || qd != DIM || gd != 100) return 1;

    px = malloc((size_t)nb * 4); py = malloc((size_t)nb * 4);
    for (int i = 0; i < nb; i++) {
        project2(base + (size_t)i * DIM, px + i, py + i);
        if (px[i] < xmin) xmin = px[i];
        if (px[i] > xmax) xmax = px[i];
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    int32_t *off = calloc(LEAVES + 1, 4), *ids = malloc((size_t)nb * 4), fill[LEAVES];
    for (int i = 0; i < nb; i++) off[cell(px[i], py[i], xmin, xmax, ymin, ymax) + 1]++;
    for (int c = 0; c < LEAVES; c++) off[c + 1] += off[c];
    memcpy(fill, off, sizeof(fill));
    for (int i = 0; i < nb; i++) ids[fill[cell(px[i], py[i], xmin, xmax, ymin, ymax)]++] = i;
    TensorStore store = { (const uint8_t *)base, (size_t)nb * VEC_BYTES, ids, off };
    printf("bridge: RouteField 1->4->16->64 | TensorStore tensor=base.fvecs bytes=%llu | q=%d\n",
           (unsigned long long)store.bytes, nq);

    for (int radius = 0; radius <= 1; radius++) {
        double rec = 0, elapsed = now_ms(); long long scans = 0, routes = 0; size_t bytes = 0; uint64_t hashes = 0;
        int *seen = calloc((size_t)nb, 4);
        for (int qi = 0; qi < nq; qi++) {
            float x, y; int ans[TOPK], nscan = 0; project2(query + (size_t)qi * DIM, &x, &y);
            Route r = route_make(cell(x, y, xmin, xmax, ymin, ymax));
            int n = resolve_route(&r, query + (size_t)qi * DIM, radius, &store, seen, ans, &nscan, &bytes, &hashes);
            scans += n; routes += 1 + (radius ? 8 : 0);
            rec += recall10(ans, gt + (size_t)qi * gd);
            memset(seen, 0, (size_t)nb * 4);
        }
        printf("R%d bridge: routes=%lld unique-span-bytes=%llu scan=%.0f (%.3f%%) recall@10=%.4f %.2fms/q checksum=%016llx\n",
               radius, routes, (unsigned long long)bytes, (double)scans / nq,
               (double)scans / nq / nb * 100.0, rec / (nq * 10.0),
               (now_ms() - elapsed) / nq, (unsigned long long)hashes);
        free(seen);
    }
    free(base); free(query); free(gt); free(pc); free(pm); free(px); free(py); free(ids); free(off);
    return 0;
}
