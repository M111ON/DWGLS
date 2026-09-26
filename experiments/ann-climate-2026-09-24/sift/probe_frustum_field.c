/* probe_frustum_field.c -- ephemeral 1->4->16->64 field routes.
 *
 * The grid is a query-time coordinate view, not a stored tree or centroid
 * index.  Base vectors are placed into the 8x8 leaf address space once for
 * this measurement; each query derives its route from the same PCA2 bounds.
 * R0 is the route leaf, R1/R2 add square rings around that leaf.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define DIM 128
#define NLEAF 64
#define NQ 100
#define TOPK 10

static float *base, *query;
static float *pc, *pm;
static int32_t *gt;
static int nb, nq, ngt, gd;
static float *px, *py;
static int32_t *cell_ids;
static int32_t *cell_off;

static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static void *load_fvecs(const char *path, int *n, int *d) {
    FILE *f = fopen(path, "rb");
    int32_t dim;
    long size;
    void *out;
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
    FILE *f = fopen(path, "rb");
    void *p = malloc(count * sizeof(float));
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
        double centered = (double)v[j] - pm[j];
        sx += centered * pc[j];
        sy += centered * pc[DIM + j];
    }
    *x = (float)sx; *y = (float)sy;
}

static int coord(float x, float y, float xmin, float xmax, float ymin, float ymax) {
    int ix = (int)(((double)x - xmin) / (xmax - xmin) * 8.0);
    int iy = (int)(((double)y - ymin) / (ymax - ymin) * 8.0);
    if (ix < 0) ix = 0;
    if (ix > 7) ix = 7;
    if (iy < 0) iy = 0;
    if (iy > 7) iy = 7;
    return iy * 8 + ix;
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

static int run_ring(const float *q, int leaf, int radius, int *ans) {
    double best[TOPK]; int ids[TOPK], n = 0;
    int scanned = 0;
    int cx = leaf & 7, cy = leaf >> 3;
    for (int dy = -radius; dy <= radius; dy++) for (int dx = -radius; dx <= radius; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x < 0 || x > 7 || y < 0 || y > 7) continue;
        int c = y * 8 + x;
        for (int32_t p = cell_off[c]; p < cell_off[c + 1]; p++) {
            scanned++;
            int id = cell_ids[p]; double d = dist128(q, base + (size_t)id * DIM);
            if (n < TOPK) { int k = n++; while (k > 0 && d < best[k - 1]) { best[k] = best[k - 1]; ids[k] = ids[k - 1]; k--; } best[k] = d; ids[k] = id; }
            else if (d < best[TOPK - 1]) { int k = TOPK - 1; while (k > 0 && d < best[k - 1]) { best[k] = best[k - 1]; ids[k] = ids[k - 1]; k--; } best[k] = d; ids[k] = id; }
        }
    }
    for (int i = 0; i < TOPK; i++) ans[i] = i < n ? ids[i] : -1;
    return scanned;
}

int main(int argc, char **argv) {
    int d, qd;
    float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
    base = load_fvecs("build/sift1m/sift/sift_base.fvecs", &nb, &d);
    query = load_fvecs("build/sift1m/sift/sift_query.fvecs", &nq, &qd);
    gt = (int32_t *)load_fvecs("build/sift1m/sift/sift_groundtruth.ivecs", &ngt, &gd);
    pc = load_bin("build/sift1m_c/pca_comp.bin", (size_t)2 * DIM);
    pm = load_bin("build/sift1m_c/pca_mean.bin", DIM);
    if (!base || !query || !gt || !pc || !pm || d != DIM || qd != DIM || gd != 100) return 1;
    if (argc > 1) { int requested = atoi(argv[1]); if (requested > 0 && requested < nq) nq = requested; }
    if (nq > NQ) nq = NQ;
    px = malloc((size_t)nb * 4); py = malloc((size_t)nb * 4);
    for (int i = 0; i < nb; i++) { project2(base + (size_t)i * DIM, px + i, py + i); if (px[i] < xmin) xmin = px[i]; if (px[i] > xmax) xmax = px[i]; if (py[i] < ymin) ymin = py[i]; if (py[i] > ymax) ymax = py[i]; }
    cell_off = calloc(NLEAF + 1, sizeof(*cell_off));
    for (int i = 0; i < nb; i++) cell_off[coord(px[i], py[i], xmin, xmax, ymin, ymax) + 1]++;
    for (int c = 0; c < NLEAF; c++) cell_off[c + 1] += cell_off[c];
    cell_ids = malloc((size_t)nb * 4); int32_t fill[NLEAF]; memcpy(fill, cell_off, sizeof(fill));
    for (int i = 0; i < nb; i++) cell_ids[fill[coord(px[i], py[i], xmin, xmax, ymin, ymax)]++] = i;
    printf("field: base=%d query=%d route=1->4->16->64 leaves=%d PCA2 bounds=[%.3g,%.3g]x[%.3g,%.3g]\n", nb, nq, NLEAF, xmin, xmax, ymin, ymax);
    for (int r = 0; r <= 2; r++) {
        double rec = 0, ms = now_ms(); long long scan = 0;
        for (int qi = 0; qi < nq; qi++) {
            float x, y; int ans[TOPK]; project2(query + (size_t)qi * DIM, &x, &y);
            int leaf = coord(x, y, xmin, xmax, ymin, ymax);
            scan += run_ring(query + (size_t)qi * DIM, leaf, r, ans);
            rec += recall10(ans, gt + (size_t)qi * gd);
        }
        printf("R%d ring: recall@10=%.4f scan=%.0f (%.3f%%) %.2fms/q\n", r, rec / (nq * 10.0), (double)scan / nq, (double)scan / nq / nb * 100.0, (now_ms() - ms) / nq);
    }
    printf("GATE: field routes are measurable; R0 is direct route, R1/R2 are junction surrounds.\n");
    free(base); free(query); free(gt); free(pc); free(pm); free(px); free(py); free(cell_ids); free(cell_off);
    return 0;
}
