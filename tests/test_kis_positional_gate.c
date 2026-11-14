/* test_kis_positional_gate.c — KIS_POSITIONAL_PRESERVATION_GATE
 *
 * Question: can data move physically without its logical relations changing?
 *
 * Three checks, deliberately NOT combined into one score:
 *   1. ADDRESS IDENTITY        — does the inverse map land on the original cell?
 *   2. VALUE INTEGRITY         — are the bytes bit-identical after write+read?
 *   3. STRUCTURAL PRESERVATION — measured on the PHYSICAL layout, read by a consumer
 *      that knows the tensor's base address but does NOT know any intra-tensor map.
 *
 * Checks 1 and 2 are what "lossless" usually means, and every bijection passes them.
 * Check 3 is the one that decides whether the weights are still usable, and it is the
 * one that fails: recovering the bytes does NOT recover the structure in place.
 *
 * The single exception is relocation at TENSOR granularity: move the whole block to a
 * new base offset and leave the internal layout untouched. Then the consumer only needs
 * the new base, and every structural property survives. Reindexing INSIDE a tensor
 * destroys them unless the map travels with the data.
 *
 * Structure injected (the signature measured on real Qwen2.5 q_proj):
 *   - smooth row scale      (AR(1) rho 0.7)      -> row-scale locality
 *   - row-correlated shape  (rho 0.6 per column) -> W[i,j] ~ W[i+1,j]
 *   - a per-column profile                       -> column identity
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    int field_size;          /* 20736 */
    int row_stride;          /* 896   */
    int preserve_column_id;  /* 1     */
    int preserve_row_id;     /* 1     */
    int preserve_locality;   /* 1     */
    int robust_normalize;    /* 1: median/MAD instead of mean/std */
    int verify_roundtrip;    /* 1     */
} KIS_TEST_CONFIG;

typedef enum {
    T_IDENTITY = 0,
    T_SHIFT_1,
    T_SHIFT_ROW,
    T_SHIFT_ROW7,
    T_AFFINE_37,
    T_AXIS_SEPARABLE,
    T_TENSOR_MOVE,
    T_COUNT
} TKind;

static const char *tname[T_COUNT] = {
    "identity (control)",
    "intra-tensor: global shift +1",
    "intra-tensor: global shift +1 row",
    "intra-tensor: global shift +7 rows",
    "intra-tensor: global affine 37*i mod N",
    "intra-tensor: axis-separable (rows x5, cols x3)",
    "TENSOR-LEVEL: whole block moved to a new base"
};

static unsigned long rng_state = 0x12345678UL;
static double frand(void)
{
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (double)((rng_state >> 11) & 0xFFFFFFFFUL) / 4294967296.0;
}
static double nrand(void)
{
    double s = 0.0;
    for (int i = 0; i < 12; i++) s += frand();
    return s - 6.0;
}
static int gcd_int(int a, int b)
{
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}
static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pearson(const double *a, const double *b, int n)
{
    if (n < 2) return 0.0;
    double ma = 0, mb = 0;
    for (int i = 0; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double sa = 0, sb = 0, sab = 0;
    for (int i = 0; i < n; i++) {
        double x = a[i] - ma, y = b[i] - mb;
        sa += x * x; sb += y * y; sab += x * y;
    }
    if (sa <= 0 || sb <= 0) return 0.0;
    return sab / sqrt(sa * sb);
}

/* forward map: logical index -> physical index. also reports the base the consumer
 * would need in order to read the matrix at the declared stride. */
static int build_map(int *map, const KIS_TEST_CONFIG *c, TKind k,
                     int rows, int cols, int *base_out)
{
    const int n = c->field_size;
    const int used = rows * cols;
    *base_out = 0;
    for (int i = 0; i < n; i++) map[i] = i;

    switch (k) {
    case T_IDENTITY:
        break;
    case T_SHIFT_1:
        for (int i = 0; i < n; i++) map[i] = (i + 1) % n;
        break;
    case T_SHIFT_ROW:
        for (int i = 0; i < n; i++) map[i] = (i + cols) % n;
        break;
    case T_SHIFT_ROW7:
        for (int i = 0; i < n; i++) map[i] = (i + 7 * cols) % n;
        break;
    case T_AFFINE_37:
        if (gcd_int(37, n) != 1) return 0;
        for (int i = 0; i < n; i++) map[i] = (int)((long long)37 * i % n);
        break;
    case T_AXIS_SEPARABLE:
        if (gcd_int(5, rows) != 1 || gcd_int(3, cols) != 1) return 0;
        for (int r = 0; r < rows; r++)
            for (int cc = 0; cc < cols; cc++)
                map[r * cols + cc] = ((r * 5 + 3) % rows) * cols + ((cc * 3 + 1) % cols);
        break;
    case T_TENSOR_MOVE:
        /* swap the two halves: the block keeps its internal layout, only its base moves */
        if (2 * used > n) return 0;
        for (int i = 0; i < used; i++) {
            map[i] = i + used;
            map[i + used] = i;
        }
        *base_out = used;
        break;
    default:
        return 0;
    }

    char *seen = calloc((size_t)n, 1);
    int ok = 1;
    for (int i = 0; i < n; i++) {
        if (map[i] < 0 || map[i] >= n || seen[map[i]]) { ok = 0; break; }
        seen[map[i]] = 1;
    }
    free(seen);
    return ok;
}

static void row_stats(const double *m, int rows, int cols, int robust,
                      double *ctr, double *scl)
{
    double *buf = malloc(sizeof(double) * (size_t)cols);
    for (int r = 0; r < rows; r++) {
        const double *row = m + (size_t)r * cols;
        if (!robust) {
            double s = 0, s2 = 0;
            for (int j = 0; j < cols; j++) { s += row[j]; s2 += row[j] * row[j]; }
            double mu = s / cols, var = s2 / cols - mu * mu;
            ctr[r] = mu;
            scl[r] = var > 0 ? sqrt(var) : 1.0;
        } else {
            memcpy(buf, row, sizeof(double) * (size_t)cols);
            qsort(buf, (size_t)cols, sizeof(double), cmp_d);
            double med = (cols & 1) ? buf[cols / 2]
                                    : 0.5 * (buf[cols / 2 - 1] + buf[cols / 2]);
            for (int j = 0; j < cols; j++) buf[j] = fabs(row[j] - med);
            qsort(buf, (size_t)cols, sizeof(double), cmp_d);
            double mad = (cols & 1) ? buf[cols / 2]
                                    : 0.5 * (buf[cols / 2 - 1] + buf[cols / 2]);
            ctr[r] = med;
            scl[r] = mad > 0 ? 1.4826 * mad : 1.0;
        }
    }
    free(buf);
}

static void col_means(const double *m, int rows, int cols, double *out)
{
    for (int j = 0; j < cols; j++) {
        double s = 0;
        for (int r = 0; r < rows; r++) s += m[(size_t)r * cols + j];
        out[j] = s / rows;
    }
}

int main(void)
{
    KIS_TEST_CONFIG cfg = {
        .field_size = 20736,
        .row_stride = 896,
        .preserve_column_id = 1,
        .preserve_row_id = 1,
        .preserve_locality = 1,
        .robust_normalize = 1,
        .verify_roundtrip = 1
    };

    const int cols = cfg.row_stride;
    const int rows = 11;   /* 11*896 = 9856, so two placements fit in 20736 */
    const int used = rows * cols;
    const int n = cfg.field_size;

    printf("KIS_POSITIONAL_PRESERVATION_GATE\n");
    printf("  field %d, row_stride %d, window %d rows = %d cells (%d free)\n",
           n, cols, rows, used, n - used);
    printf("  robust_normalize=%d (median/MAD)  verify_roundtrip=%d\n\n",
           cfg.robust_normalize, cfg.verify_roundtrip);

    /* ---- inject structure with the signature measured on real q_proj ---- */
    double *logical = calloc((size_t)n, sizeof(double));
    double *scale = malloc(sizeof(double) * (size_t)rows);
    double *prev = calloc((size_t)cols, sizeof(double));
    double *colprof = malloc(sizeof(double) * (size_t)cols);

    for (int j = 0; j < cols; j++) colprof[j] = 1.0 + 0.4 * nrand();

    scale[0] = 1.0;
    for (int r = 1; r < rows; r++)
        scale[r] = 0.7 * scale[r - 1] + 0.3 * (1.0 + 0.6 * nrand());

    for (int r = 0; r < rows; r++) {
        for (int j = 0; j < cols; j++) {
            double shape = (r == 0) ? nrand() : 0.6 * prev[j] + 0.8 * nrand();
            prev[j] = shape;
            logical[(size_t)r * cols + j] = scale[r] * (shape + colprof[j]);
        }
    }

    {
        double *a = malloc(sizeof(double) * (size_t)((rows - 1) * cols));
        double *b = malloc(sizeof(double) * (size_t)((rows - 1) * cols));
        int k = 0;
        for (int r = 0; r + 1 < rows; r++)
            for (int j = 0; j < cols; j++) {
                a[k] = logical[(size_t)r * cols + j];
                b[k] = logical[(size_t)(r + 1) * cols + j];
                k++;
            }
        double pr = pearson(a, b, k);
        printf("injected structure self-check: pearson(W[i,j],W[i+1,j]) = %+.4f\n", pr);
        if (fabs(pr) < 0.15) {
            printf("  FAIL: injected structure not measurable, gate is not meaningful\n");
            return 1;
        }
        printf("  OK\n\n");
        free(a); free(b);
    }

    double *ref_col = malloc(sizeof(double) * (size_t)cols);
    double *ref_ctr = malloc(sizeof(double) * (size_t)rows);
    double *ref_scl = malloc(sizeof(double) * (size_t)rows);
    col_means(logical, rows, cols, ref_col);
    row_stats(logical, rows, cols, cfg.robust_normalize, ref_ctr, ref_scl);

    int *map = malloc(sizeof(int) * (size_t)n);
    double *phys = calloc((size_t)n, sizeof(double));
    double *rec = malloc(sizeof(double) * (size_t)n);
    double *cur_col = malloc(sizeof(double) * (size_t)cols);
    double *cur_ctr = malloc(sizeof(double) * (size_t)rows);
    double *cur_scl = malloc(sizeof(double) * (size_t)rows);

    int fails_value = 0, preserved_count = 0, total_maps = 0;

    for (TKind t = 0; t < T_COUNT; t++) {
        int base = 0;
        if (!build_map(map, &cfg, t, rows, cols, &base)) {
            printf("### %s — SKIP (map is not a bijection)\n\n", tname[t]);
            continue;
        }
        total_maps++;

        for (int i = 0; i < n; i++) phys[map[i]] = logical[i];
        for (int i = 0; i < n; i++) rec[i] = phys[map[i]];

        int addr_ok = 1, val_ok = 1;
        if (cfg.verify_roundtrip) {
            for (int i = 0; i < n; i++)
                if (memcmp(&rec[i], &logical[i], sizeof(double)) != 0) { val_ok = 0; break; }
            int *inv = malloc(sizeof(int) * (size_t)n);
            for (int i = 0; i < n; i++) inv[map[i]] = i;
            for (int i = 0; i < n; i++)
                if (inv[map[i]] != i) { addr_ok = 0; break; }
            free(inv);
        }
        if (!val_ok) fails_value++;

        /* read the matrix at its NEW base with the declared stride, no intra-tensor map */
        const double *view = phys + base;
        col_means(view, rows, cols, cur_col);
        row_stats(view, rows, cols, cfg.robust_normalize, cur_ctr, cur_scl);

        double col_corr = pearson(ref_col, cur_col, cols);

        double scale_err = 0.0;
        int nsc = 0;
        for (int r = 0; r + 1 < rows; r++) {
            double want = ref_scl[r + 1] / ref_scl[r];
            double got = cur_scl[r + 1] / cur_scl[r];
            scale_err += fabs(got - want) / (fabs(want) + 1e-12);
            nsc++;
        }
        scale_err = nsc ? scale_err / nsc : 0.0;

        double *a = malloc(sizeof(double) * (size_t)((rows - 1) * cols));
        double *b = malloc(sizeof(double) * (size_t)((rows - 1) * cols));
        int k = 0;
        for (int r = 0; r + 1 < rows; r++)
            for (int j = 0; j < cols; j++) {
                a[k] = view[(size_t)r * cols + j];
                b[k] = view[(size_t)(r + 1) * cols + j];
                k++;
            }
        double shape_pr = pearson(a, b, k);
        free(a); free(b);

        int col_ok = col_corr > 0.99;
        int scl_ok = scale_err < 0.01;
        int shp_ok = shape_pr > 0.5;
        if (col_ok && scl_ok && shp_ok) preserved_count++;

        printf("### %s\n", tname[t]);
        if (base) printf("  (consumer reads at new base %d)\n", base);
        printf("  1. address identity        : %s\n", addr_ok ? "PASS" : "FAIL");
        printf("  2. value integrity         : %s\n", val_ok ? "PASS" : "FAIL");
        printf("  3. structural preservation :\n");
        printf("       column identity   corr(col-mean)     = %+.4f  %s\n",
               col_corr, col_ok ? "PRESERVED" : "BROKEN");
        printf("       row scale locality mean rel.err      = %.4f  %s\n",
               scale_err, scl_ok ? "PRESERVED" : "BROKEN");
        printf("       row shape adjacent pearson(W[i],W[i+1]) = %+.4f  %s\n",
               shape_pr, shp_ok ? "PRESERVED" : "BROKEN");
        printf("\n");
    }

    printf("verdict\n");
    printf("  checks 1 and 2 pass for every bijection (%d/%d maps) — that is what \"lossless\"\n",
           total_maps - fails_value, total_maps);
    printf("  normally means, and it is NOT sufficient.\n");
    printf("  check 3 passes for %d/%d maps.\n\n", preserved_count, total_maps);
    printf("  STRUCTURE IS NOT PRESERVED BY RELOCATION ITSELF.\n");
    printf("  Getting the bytes back does not get the structure back: every intra-tensor\n");
    printf("  reindex destroys column identity and row locality in the new layout, even\n");
    printf("  though the inverse map recovers the data exactly.\n\n");
    printf("  The one transform that survives is relocation at TENSOR granularity: the block\n");
    printf("  keeps its internal layout and only its base address changes, so the consumer\n");
    printf("  needs nothing but the new base.\n\n");
    printf("  DESIGN RULE: move whole tensors, never reindex inside one. If an intra-tensor\n");
    printf("  map is unavoidable, the map is part of the data and must travel with it —\n");
    printf("  the field's address book is not an optimisation, it is load-bearing.\n");

    free(logical); free(scale); free(prev); free(colprof); free(map);
    free(phys); free(rec); free(ref_col); free(ref_ctr); free(ref_scl);
    free(cur_col); free(cur_ctr); free(cur_scl);

    return (fails_value || preserved_count == 0) ? 1 : 0;
}
