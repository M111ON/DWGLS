/* tools/probe_geo_cells.c
 * FRAME UNDER TEST: horizontal search field bounded by GEOMETRY (cube-octree
 * cells), not by k-means bucket membership lists.
 * METHOD: PCA3 projection (first 3 PCs only — keeps the octree cheap),
 * iterative median 8-split to ~2560 leaves, route top-L leaves by centroid
 * distance, exact 128-dim rank of members. Tetra variant SKIPPED (would cost
 * >30 extra lines for 6-tetra-per-cube ordering + centroid table).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define D_FULL 128
#define D_USE 3
#define N_BASE 1000000
#define N_QALL 10000
#define N_Q 1000
#define TARGET_LEAVES 2560
#define MAX_LEAVES 8192
#define LS 16

static int cmpf(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

typedef struct { int start, count, active; float cx, cy, cz; } Leaf;

static float *g_base;   /* N_BASE x 128 raw (for exact re-rank) */
static float *g_proj;   /* N_BASE x 3 PCA3 projection */
static int *g_perm;     /* member id permutation, leaves = ranges */
static Leaf g_leaf[MAX_LEAVES];
static int g_nleaf = 0, g_nactive = 0;

int main(void) {
  clock_t t0 = clock();
  /* ---- PCA artifacts (row-major: proj[i] = sum_j comp[i*128+j]*(v[j]-mean[j])) */
  FILE *f = fopen("build/sift1m_c/pca_comp.bin", "rb");
  if (!f) { perror("pca_comp.bin"); return 1; }
  static float comp[25 * 128];
  if (fread(comp, 4, 25 * 128, f) != 25 * 128) return 1;
  fclose(f);
  f = fopen("build/sift1m_c/pca_mean.bin", "rb");
  if (!f) { perror("pca_mean.bin"); return 1; }
  float mean[128];
  if (fread(mean, 4, 128, f) != 128) return 1;
  fclose(f);

  /* ---- base vectors (record = int32 dim + 128 float) */
  g_base = (float *)malloc((size_t)N_BASE * D_FULL * 4);
  g_proj = (float *)malloc((size_t)N_BASE * D_USE * 4);
  g_perm = (int *)malloc((size_t)N_BASE * 4);
  if (!g_base || !g_proj || !g_perm) { printf("OOM\n"); return 1; }
  f = fopen("build/sift1m/sift/sift_base.fvecs", "rb");
  if (!f) { perror("sift_base.fvecs"); return 1; }
  static unsigned char blk[4096 * (4 + 128 * 4)];
  int need = N_BASE, done = 0;
  while (need > 0) {
    int b = need > 4096 ? 4096 : need;
    if (fread(blk, 4 + 128 * 4, b, f) != (size_t)b) { printf("short base\n"); return 1; }
    for (int i = 0; i < b; i++) {
      float *row = g_base + (size_t)(done + i) * D_FULL;
      memcpy(row, blk + (size_t)i * (4 + 128 * 4) + 4, 128 * 4);
    }
    done += b; need -= b;
  }
  fclose(f);

  /* ---- project to PCA3 */
  for (int i = 0; i < N_BASE; i++) {
    float *v = g_base + (size_t)i * D_FULL, *p = g_proj + (size_t)i * D_USE;
    for (int k = 0; k < D_USE; k++) {
      double s = 0;
      float *c = comp + k * D_FULL;
      for (int j = 0; j < D_FULL; j++) s += (double)c[j] * (v[j] - mean[j]);
      p[k] = (float)s;
    }
    g_perm[i] = i;
  }
  double t_proj = (double)(clock() - t0) / CLOCKS_PER_SEC;

  /* ---- cube-octree: iterative largest-leaf median 8-split to ~2560 leaves */
  clock_t tb = clock();
  g_leaf[0].start = 0; g_leaf[0].count = N_BASE; g_leaf[0].active = 1;
  g_nleaf = 1; g_nactive = 1;
  float *ax = (float *)malloc((size_t)N_BASE * 4);
  float *ay = (float *)malloc((size_t)N_BASE * 4);
  float *az = (float *)malloc((size_t)N_BASE * 4);
  int *tbuf = (int *)malloc((size_t)N_BASE * 4);
  if (!ax || !ay || !az || !tbuf) { printf("OOM tmp\n"); return 1; }
  while (g_nactive + 7 <= TARGET_LEAVES) {
    int bi = -1, bc = 8;
    for (int i = 0; i < g_nleaf; i++)
      if (g_leaf[i].active && g_leaf[i].count > bc) { bc = g_leaf[i].count; bi = i; }
    if (bi < 0) break;
    Leaf P = g_leaf[bi];
    int n = P.count;
    for (int i = 0; i < n; i++) {
      float *p = g_proj + (size_t)g_perm[P.start + i] * D_USE;
      ax[i] = p[0]; ay[i] = p[1]; az[i] = p[2];
    }
    qsort(ax, n, 4, cmpf); qsort(ay, n, 4, cmpf); qsort(az, n, 4, cmpf);
    float mx = ax[n / 2], my = ay[n / 2], mz = az[n / 2];
    int cnt[8] = {0};
    for (int i = 0; i < n; i++) {
      float *p = g_proj + (size_t)g_perm[P.start + i] * D_USE;
      cnt[((p[0] > mx) << 2) | ((p[1] > my) << 1) | (p[2] > mz)]++;
    }
    int off[8], acc = 0, nkids = 0;
    for (int o = 0; o < 8; o++) { off[o] = acc; acc += cnt[o]; if (cnt[o]) nkids++; }
    if (nkids < 2) break; /* degenerate: cannot split */
    int cur[8]; memcpy(cur, off, sizeof off);
    for (int i = 0; i < n; i++) {
      int id = g_perm[P.start + i];
      float *p = g_proj + (size_t)id * D_USE;
      int o = ((p[0] > mx) << 2) | ((p[1] > my) << 1) | (p[2] > mz);
      tbuf[cur[o]++] = id;
    }
    memcpy(g_perm + P.start, tbuf, (size_t)n * 4);
    g_leaf[bi].active = 0; g_nactive--;
    int first = 1;
    for (int o = 0; o < 8; o++) {
      if (!cnt[o] || g_nleaf >= MAX_LEAVES) continue;
      int slot = first ? bi : g_nleaf++;
      first = 0;
      g_leaf[slot].start = P.start + off[o];
      g_leaf[slot].count = cnt[o];
      g_leaf[slot].active = 1;
      g_nactive++;
    }
  }
  /* centroids */
  int maxc = 0; long long totc = 0;
  for (int i = 0; i < g_nleaf; i++) {
    if (!g_leaf[i].active) continue;
    double sx = 0, sy = 0, sz = 0;
    for (int k = 0; k < g_leaf[i].count; k++) {
      float *p = g_proj + (size_t)g_perm[g_leaf[i].start + k] * D_USE;
      sx += p[0]; sy += p[1]; sz += p[2];
    }
    g_leaf[i].cx = (float)(sx / g_leaf[i].count);
    g_leaf[i].cy = (float)(sy / g_leaf[i].count);
    g_leaf[i].cz = (float)(sz / g_leaf[i].count);
    if (g_leaf[i].count > maxc) maxc = g_leaf[i].count;
    totc += g_leaf[i].count;
  }
  double t_build = (double)(clock() - tb) / CLOCKS_PER_SEC;
  free(ax); free(ay); free(az); free(tbuf);
  /* compact active leaf index */
  static int act[MAX_LEAVES];
  int na = 0;
  for (int i = 0; i < g_nleaf; i++) if (g_leaf[i].active) act[na++] = i;
  double idx_mb = ((double)N_BASE * 4 + (double)N_BASE * 12 + (double)na * 28) / (1 << 20);

  /* ---- queries + ground truth (first N_Q) */
  float *qbase = (float *)malloc((size_t)N_Q * D_FULL * 4);
  int (*gt)[100] = malloc((size_t)N_Q * 100 * 4);
  if (!qbase || !gt) { printf("OOM q\n"); return 1; }
  f = fopen("build/sift1m/sift/sift_query.fvecs", "rb");
  if (!f) { perror("query"); return 1; }
  for (int i = 0; i < N_Q; i++) {
    int d; if (fread(&d, 4, 1, f) != 1 || d != 128) { printf("bad qdim\n"); return 1; }
    if (fread(qbase + (size_t)i * D_FULL, 4, 128, f) != 128) return 1;
  }
  fclose(f);
  f = fopen("build/sift1m/sift/sift_groundtruth.ivecs", "rb");
  if (!f) { perror("gt"); return 1; }
  for (int i = 0; i < N_Q; i++) {
    int d; if (fread(&d, 4, 1, f) != 1 || d != 100) { printf("bad gdim\n"); return 1; }
    if (fread(gt[i], 4, 100, f) != 100) return 1;
  }
  fclose(f);

  /* ---- per query: route top-64 leaves, exact-rank prefixes 16/32/64 */
  int *cand = (int *)malloc((size_t)N_BASE * 4);
  int *topid = (int *)malloc(10 * 4);
  float *topd = (float *)malloc(10 * 4);
  if (!cand || !topid || !topd) { printf("OOM c\n"); return 1; }
  const int LL[3] = {16, 32, 64};
  long long hits[3] = {0, 0, 0}, scanned[3] = {0, 0, 0};
  double t_route = 0, t_rank[3] = {0, 0, 0};
  float qp[3];
  static float ld[MAX_LEAVES];
  static int lo[64]; static float lb[64];
  for (int qi = 0; qi < N_Q; qi++) {
    float *v = qbase + (size_t)qi * D_FULL;
    for (int k = 0; k < D_USE; k++) {
      double s = 0; float *c = comp + k * D_FULL;
      for (int j = 0; j < D_FULL; j++) s += (double)c[j] * (v[j] - mean[j]);
      qp[k] = (float)s;
    }
    clock_t tr = clock();
    for (int i = 0; i < na; i++) {
      Leaf *L = &g_leaf[act[i]];
      float dx = qp[0] - L->cx, dy = qp[1] - L->cy, dz = qp[2] - L->cz;
      ld[i] = dx * dx + dy * dy + dz * dz;
    }
    for (int k = 0; k < 64; k++) { lo[k] = -1; lb[k] = 1e30f; }
    for (int i = 0; i < na; i++) {
      float d = ld[i];
      for (int k = 0; k < 64; k++) {
        if (d < lb[k]) {
          for (int m = 63; m > k; m--) { lb[m] = lb[m - 1]; lo[m] = lo[m - 1]; }
          lb[k] = d; lo[k] = act[i]; break;
        }
      }
    }
    int nc = 0, bounds[3];
    for (int k = 0; k < 64; k++) {
      Leaf *L = &g_leaf[lo[k]];
      memcpy(cand + nc, g_perm + L->start, (size_t)L->count * 4);
      nc += L->count;
      if (k == 15) bounds[0] = nc;
      if (k == 31) bounds[1] = nc;
      if (k == 63) bounds[2] = nc;
    }
    t_route += (double)(clock() - tr) / CLOCKS_PER_SEC;
    for (int li = 0; li < 3; li++) {
      clock_t tk = clock();
      for (int k = 0; k < 10; k++) { topid[k] = -1; topd[k] = 1e30f; }
      int nn = bounds[li];
      for (int c = 0; c < nn; c++) {
        float *u = g_base + (size_t)cand[c] * D_FULL;
        double s = 0;
        for (int j = 0; j < D_FULL; j++) { double d = (double)v[j] - u[j]; s += d * d; }
        if (s < topd[9]) {
          int k = 9; while (k > 0 && s < topd[k - 1]) { topd[k] = topd[k - 1]; topid[k] = topid[k - 1]; k--; }
          topd[k] = (float)s; topid[k] = cand[c];
        }
      }
      int h = 0;
      for (int k = 0; k < 10; k++)
        for (int m = 0; m < 10; m++) if (topid[k] == gt[qi][m]) { h++; break; }
      hits[li] += h; scanned[li] += nn;
      t_rank[li] += (double)(clock() - tk) / CLOCKS_PER_SEC;
    }
  }
  printf("proj=PCA3(first-3-PCs) leaves=%d avg_leaf=%.1f max_leaf=%d\n",
         na, (double)totc / na, maxc);
  printf("index_bytes=%.2fMB(excl_raw_base) build_tree=%.1fs proj=%.1fs\n",
         idx_mb, t_build, t_proj);
  for (int li = 0; li < 3; li++) {
    double rec = (double)hits[li] / (N_Q * 10);
    double scan = (double)scanned[li] / N_Q / N_BASE * 100.0;
    double msq = (t_route + t_rank[li]) / N_Q * 1000.0;
    printf("L=%d recall@10=%.4f scan=%.2f%% ms/q=%.2f\n", LL[li], rec, scan, msq);
  }
  printf("tetra_variant=SKIPPED(>30-line-cost)\n");
  return 0;
}
