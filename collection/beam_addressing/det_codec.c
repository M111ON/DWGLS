/*
 * det_codec.c — Deterministic Geometric Codec (Binary Expansion)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Formula (hexagonal symmetry, θ_k = 60°×k):
 *
 *   E_n = G + R × Σ(k=1..n) b_k × 2^(-k) × (cos(60°k), sin(60°k))
 *       centroid   ↑ radius  ↑ bit k   ↑ weight   ↑ direction
 *
 * weight → binary expansion b_k → deterministic position E_n
 * delta = E_n - nearest centroid (small correction)
 *
 * Decode: delta + E_n จาก tessellation → reconstruct weight
 *
 * Compile: gcc -O2 -I. det_codec.c -o det_codec.exe -lm
 * Run: ./det_codec.exe [model.gguf]
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gguf_reader.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════════════════════════════════════════════════════
   DETERMINISTIC POSITION FROM BINARY EXPANSION
   ══════════════════════════════════════════════════════════════
 *
 *   weight w (0..255 for Q8) → 8 bits b₀..b₇
 *
 *   Position E = centroid + R × Σ b_k × 2^-(k+1) × (cos(60°k), sin(60°k))
 *
 *   centroid = center of tessellation (0,0 for one cell)
 *   R = D/2 = circumradius
 *   k = 0..7 (bit position)
 *   θ_k = 60° × k  (hexagonal rotation per bit)
 */

typedef struct {
    double x, y;       /* deterministic position */
    double R;          /* circumradius */
    uint8_t weight_u8; /* original weight mapped to 0..255 */
    uint8_t bits[8];   /* binary expansion */
} DetPos;

/* Compute deterministic position from weight using hexagonal symmetry */
static DetPos det_compute(double weight, double R)
{
    DetPos pos;
    pos.R = R;

    /* Map Q8 weight (-128..127) to 0..255 */
    int w_int = (int)weight;
    if (w_int < -128) w_int = -128;
    if (w_int > 127) w_int = 127;
    pos.weight_u8 = (uint8_t)(w_int + 128);

    /* Binary expansion */
    uint8_t val = pos.weight_u8;
    for (int k = 0; k < 8; k++) {
        pos.bits[k] = (val >> (7 - k)) & 1;  /* b₀ = MSB */
    }

    /* Compute position E = Σ b_k × 2^-(k+1) × (cos(60°k), sin(60°k)) */
    double ex = 0.0, ey = 0.0;
    for (int k = 0; k < 8; k++) {
        if (pos.bits[k]) {
            double angle = M_PI / 3.0 * k;  /* 60° × k */
            double coeff = 1.0 / (double)(1 << (k + 1));  /* 2^-(k+1) */
            ex += coeff * cos(angle);
            ey += coeff * sin(angle);
        }
    }

    pos.x = ex * R;
    pos.y = ey * R;

    return pos;
}

/* ══════════════════════════════════════════════════════════════
   CENTROID GRID — nearest centroid to a position
   ══════════════════════════════════════════════════════════════
 *
 *   7 centroids of hexagon (Seed of Life):
 *     C0 at (0, 0)
 *     C1..C6 at R × (cos(60°i), sin(60°i))
 *
 *   Centroid spacing: adjacency = R
 */

#define N_CENT 7

/* Get i-th centroid position */
static void centroid_pos(int i, double R, double *cx, double *cy)
{
    if (i == 0) { *cx = 0; *cy = 0; return; }
    double a = M_PI / 3.0 * (i - 1);
    *cx = R * cos(a);
    *cy = R * sin(a);
}

/* Find nearest centroid index (0..6) */
static int nearest_centroid(double x, double y, double R)
{
    int best = 0;
    double best_d2 = x*x + y*y;  /* distance to C0 */
    for (int i = 1; i < 7; i++) {
        double cx, cy;
        centroid_pos(i, R, &cx, &cy);
        double dx = x - cx, dy = y - cy;
        double d2 = dx*dx + dy*dy;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

/* ══════════════════════════════════════════════════════════════
   ENCODE: weight → delta
   ══════════════════════════════════════════════════════════════
 *
 *   1. weight w → deterministic position E
 *   2. Find nearest centroid C_i
 *   3. delta = E - C_i  (vector, small magnitude)
 *   4. Store: i (3 bits) + delta_x (6 bits) + delta_y (6 bits) + sign (1 bit)
 *
 *   delta_x and delta_y are residuals in Q6 format (scaled by R)
 */

typedef struct {
    int    centroid;     /* 0..6 */
    int    dx_q6;        /* delta x in Q6: -32..31 */
    int    dy_q6;        /* delta y in Q6 */
    int    sign;         /* 0=pos, 1=neg */
} DetDelta;

static DetDelta det_encode(double weight, double R)
{
    DetDelta dd;
    DetPos pos = det_compute(weight, R);

    dd.centroid = nearest_centroid(pos.x, pos.y, R);
    double cx, cy;
    centroid_pos(dd.centroid, R, &cx, &cy);

    /* Delta = position - centroid (residual) */
    double dx = pos.x - cx;
    double dy = pos.y - cy;

    /* Quantize to Q6 (-32..31 in units of R/32) */
    double scale = 32.0 / R;
    int dx_i = (int)(dx * scale + 0.5);
    int dy_i = (int)(dy * scale + 0.5);
    if (dx_i < -32) dx_i = -32; if (dx_i > 31) dx_i = 31;
    if (dy_i < -32) dy_i = -32; if (dy_i > 31) dy_i = 31;

    dd.dx_q6 = dx_i;
    dd.dy_q6 = dy_i;
    dd.sign = (weight < 0) ? 1 : 0;

    return dd;
}

/* ══════════════════════════════════════════════════════════════
   DECODE: delta → approximate weight (using precomputed LUT)
   ══════════════════════════════════════════════════════════════
 *
 *   Precompute all 256 deterministic positions once.
 *   Decode = nearest neighbor in precomputed array.
 */

typedef struct {
    double x, y;
    int    weight;   /* -128..127 */
} LutEntry;

static LutEntry lut[256];
static int lut_ready = 0;
static double lut_R = 0;

static void lut_build(double R)
{
    if (lut_ready && lut_R == R) return;  /* cache hit */
    for (int w = 0; w < 256; w++) {
        DetPos p = det_compute((double)w - 128, R);
        lut[w].x = p.x;
        lut[w].y = p.y;
        lut[w].weight = w - 128;
    }
    lut_ready = 1;
    lut_R = R;
}

static double det_decode_fast(const DetDelta *dd, double R)
{
    lut_build(R);

    /* Reconstruct position from centroid + delta */
    double cx, cy;
    centroid_pos(dd->centroid, R, &cx, &cy);
    double scale = 32.0 / R;
    double x = cx + (double)dd->dx_q6 / scale;
    double y = cy + (double)dd->dy_q6 / scale;

    /* Nearest neighbor in LUT */
    int best_w = 0;
    double best_d2 = 1e9;
    for (int i = 0; i < 256; i++) {
        double dx = x - lut[i].x;
        double dy = y - lut[i].y;
        double d2 = dx*dx + dy*dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_w = lut[i].weight;
        }
    }

    return (double)(dd->sign ? -best_w : best_w);
}

/* ══════════════════════════════════════════════════════════════
   PACK/UNPACK 16-bit delta
   ══════════════════════════════════════════════════════════════
 *
 *   [centroid:3][dx_q6:6][dy_q6:6][sign:1] = 16 bits
 */

static uint16_t pack_delta(const DetDelta *dd)
{
    uint16_t d = 0;
    d |= (dd->centroid & 0x7) << 13;
    d |= ((dd->dx_q6 + 32) & 0x3F) << 7;
    d |= ((dd->dy_q6 + 32) & 0x3F) << 1;
    d |= (dd->sign & 1);
    return d;
}

static DetDelta unpack_delta(uint16_t d)
{
    DetDelta dd;
    dd.centroid = (int)((d >> 13) & 0x7);
    dd.dx_q6 = (int)((d >> 7) & 0x3F) - 32;
    dd.dy_q6 = (int)((d >> 1) & 0x3F) - 32;
    dd.sign = (int)(d & 1);
    return dd;
}

/* ══════════════════════════════════════════════════════════════
   HELPERS + TEST
   ══════════════════════════════════════════════════════════════ */

static uint64_t read_w(GGUF_File *gf, int ti, int8_t **out)
{
    GGUF_Tensor *t = &gf->tensors[ti];
    uint64_t nb = (t->n_weights + 31) / 32;
    uint64_t ds = gf->tensor_data_start + t->offset;
    ds = (ds + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)ds, SEEK_SET);
    int8_t *w = (int8_t *)malloc(nb * 32);
    if (!w) return 0;
    uint64_t tot = 0;
    for (uint64_t b = 0; b < nb; b++) {
        uint16_t sc;
        if (fread(&sc,2,1,gf->fp)!=1) break;
        if (fread(w+tot,1,32,gf->fp)!=32) break;
        tot += 32;
    }
    *out = w;
    return tot;
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    const char *mp = argc > 1 ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Deterministic Geo Codec — Binary Expansion           ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    double R = 2.0;  /* D=4, seed radius */

    /* ── Show the 256 deterministic positions ── */
    printf("─── Deterministic Positions (256 weights, R=%.1f) ───\n\n", R);
    printf("  weight  bits      E_x     E_y  nearest centroid\n");
    printf("  ──────  ────────  ──────  ──────  ────────────────\n");

    for (int w = 0; w < 256; w += 16) {  /* step 16 */
        DetPos pos = det_compute((double)w - 128, R);
        int nc = nearest_centroid(pos.x, pos.y, R);
        printf("  %6d  ", w - 128);
        for (int k = 0; k < 8; k++) printf("%d", pos.bits[k]);
        printf("  %6.3f  %6.3f  C%d\n", pos.x, pos.y, nc);
    }

    /* ── Encode/decode test ── */
    printf("\n─── Encode/Decode ───\n\n");
    int exact = 0, off1 = 0, total = 0, max_err = 0;

    for (int w = -128; w <= 127; w++) {
        DetDelta dd = det_encode((double)w, R);
        uint16_t packed = pack_delta(&dd);
        DetDelta dd2 = unpack_delta(packed);
        double w2 = det_decode_fast(&dd2, R);

        int err = abs((int)(w2 - (double)w));
        if (err == 0) exact++;
        else if (err <= 1) off1++;
        if (err > max_err) max_err = err;
        total++;
    }
    printf("  Full Q8 range (-128..127) with R=%.1f:\n", R);
    printf("    Exact:     %d/256 (%.1f%%)\n", exact, 100.0*exact/256);
    printf("    Off-by-1:  %d/256 (%.1f%%)\n", off1, 100.0*off1/256);
    printf("    Max err:   %d\n", max_err);

    /* ── Try different R values ── */
    printf("\n─── R sweep ───\n\n");
    double Rs[] = {0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 8.0, 16.0, 32.0};
    for (int ri = 0; ri < 10; ri++) {
        double rr = Rs[ri];
        int e = 0, o = 0, me = 0;
        for (int w = -128; w <= 127; w++) {
            DetDelta dd = det_encode((double)w, rr);
            uint16_t p = pack_delta(&dd);
            DetDelta d2 = unpack_delta(p);
            double w2 = det_decode_fast(&d2, rr);
            int err = abs((int)(w2 - (double)w));
            if (err == 0) e++;
            else if (err <= 1) o++;
            if (err > me) me = err;
        }
        printf("  R=%-6.2f | exact=%3d/256 off1=%3d max_err=%d\n",
               rr, e, o, me);
    }

    /* ── Real model test ── */
    printf("\n─── Real Model ───\n");
    GGUF_File *gf = gguf_open(mp);
    if (!gf) { fprintf(stderr,"FAIL: open\n"); return 1; }
    int ti = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == 8) { ti = (int)i; break; }
    if (ti < 0) { fprintf(stderr,"FAIL: no Q8\n"); return 1; }

    int8_t *w = NULL;
    uint64_t nw = read_w(gf, ti, &w);
    gguf_close(gf);
    if (!w || nw == 0) { fprintf(stderr,"FAIL: read\n"); return 1; }

    /* Find best R */
    int best_R = 0;
    double best_score = 1e9;
    for (int ri = 0; ri < 10; ri++) {
        double rr = Rs[ri];
        int64_t sum_err = 0;
        int me = 0;
        uint64_t N = (nw > 100000) ? 100000 : nw;
        for (uint64_t i = 0; i < N; i++) {
            DetDelta dd = det_encode((double)w[i], rr);
            uint16_t p = pack_delta(&dd);
            DetDelta d2 = unpack_delta(p);
            double w2 = det_decode_fast(&d2, rr);
            int err = abs((int)(w2 - (double)w[i]));
            sum_err += err;
            if (err > me) me = err;
        }
        double avg = (double)sum_err / N;
        printf("  R=%-6.2f | avg_err=%.4f max_err=%d\n", rr, avg, me);
        if (avg < best_score) { best_score = avg; best_R = ri; }
    }
    printf("\n  Best R = %.2f\n", Rs[best_R]);

    /* ── Size with best R ── */
    double best_rr = Rs[best_R];
    uint64_t nb = (nw + 31) / 32;
    uint64_t q8s = nb * 34;
    uint64_t geos = nw * 2;  /* 16-bit delta */

    printf("  Geo delta = 2 bytes/weight (16-bit)\n");
    printf("  Q8_0: %llu MB (%.4f B/w)\n",
           (unsigned long long)(q8s/(1024*1024)), (double)q8s/nw);
    printf("  Geo:  %llu MB (%.4f B/w)  ratio=%.2f×\n",
           (unsigned long long)(geos/(1024*1024)), (double)geos/nw,
           (double)geos/q8s);

    free(w);

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Complete                                               ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    return 0;
}
