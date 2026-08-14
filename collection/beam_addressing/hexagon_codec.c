/*
 * hexagon_codec.c — Geometric Beam Codec on 7-Centroid Hexagon
 * ═══════════════════════════════════════════════════════════════════
 *
 * "beam ขึ้นอยู่กับ weight"
 *
 * Geometry: 7 centroids (Seed of Life), parameter D = circumcircle Ø
 *
 * weight w → beam = (radius, angle) where:
 *   radius = |w|  (beam length)
 *   angle  = derived from w  (beam direction depends on w itself)
 *
 * On hexagon: beam starts at center centroid → drifts in computed
 * direction → lands at position within one of 6 triangles.
 *
 * delta = encode(position) = (triangle, within)
 *
 * Compile: gcc -O2 hexagon_codec.c -o hexagon_codec.exe -lm
 * Run: ./hexagon_codec.exe
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════════════════════════════════════════════════════
   GEOMETRY
   ══════════════════════════════════════════════════════════════ */

typedef struct { double x, y; } Vec2;

#define N_CENTROIDS 7
#define N_TRIANGLES 6

/* Hexagon: 7 centroids, D = diameter parameter */
typedef struct {
    Vec2 centroids[N_CENTROIDS];  /* C0=center, C1..C6=outer */
    Vec2 vertices[N_TRIANGLES][3];/* 6 triangles, each has 3 vertices */
    double D;  /* circumcircle diameter */
    double R;  /* D/2 */
} HexGrid;

/* Build hexagon from diameter D */
static HexGrid hex_build(double D)
{
    HexGrid g;
    g.D = D;
    g.R = D / 2.0;

    /* Centroids */
    g.centroids[0] = (Vec2){0, 0};
    for (int i = 0; i < 6; i++) {
        double a = M_PI / 3.0 * i;
        g.centroids[i + 1] = (Vec2){g.R * cos(a), g.R * sin(a)};
    }

    /* Triangles: Ti = (C0, Ci, C{i+1}) */
    for (int t = 0; t < 6; t++) {
        int i1 = t + 1;
        int i2 = ((t + 1) % 6) + 1;
        g.vertices[t][0] = g.centroids[0];
        g.vertices[t][1] = g.centroids[i1];
        g.vertices[t][2] = g.centroids[i2];
    }

    return g;
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT → BEAM (beam ขึ้นอยู่กับ weight)
   ══════════════════════════════════════════════════════════════
 *
 *   weight w กำหนด:
 *     1. beam radius = |w|
 *     2. beam angle  = f(w)  โดย f มาจาก geometry ของ grid
 *
 *   f(w) simple: ใช้ w mod 6 → triangle index (0..5)
 *   radius = |w| / R → ระยะที่ beam drift
 *
 *   ดังนั้น beam 1 ตัว = ตำแหน่งบน hexagon ที่ w กำหนด
 *   โดย w เหมือน pointer ใน 2D space
 */

typedef struct {
    int    triangle;   /* 0..5 (which of 6 triangles) */
    double fraction;   /* 0..1 position within triangle edge */
    double radius;     /* |w| / R */
} BeamPos;

/* Derive beam from weight only (no separate direction) */
static BeamPos beam_from_weight(double w)
{
    BeamPos bp;
    double abs_w = fabs(w);
    double R = 1.0;  /* normalized — hex_build uses R to scale */

    /* Angle: use weight value to determine direction */
    /* Simple: weight decimal part × 6 → triangle (0..5) */
    double norm = fmod(fabs(w), 6.0) / 6.0;  /* 0..1 */
    double angle = norm * 2.0 * M_PI;

    bp.triangle = (int)(norm * 6.0) % 6;
    bp.radius = abs_w / R;

    /* fraction = fine position within the triangle edge (0..1) */
    /* Use 2nd order decimal for fine positioning */
    double frac = fmod(norm * 6.0, 1.0);
    bp.fraction = frac;

    return bp;
}

/* ══════════════════════════════════════════════════════════════
   ENCODE: weight → delta (32-bit)
   ══════════════════════════════════════════════════════════════
 *
 *   delta = sign | (triangle << 16) | (fraction as Q12)
 *   sign: sign of weight
 *   triangle: 0..5 (3 bits)
 *   fraction: Q12 (12 bits)
 *   total: 16 bits used, 16 wasted (for now)
 */

static uint32_t encode_weight(const HexGrid *g, double w)
{
    BeamPos bp = beam_from_weight(w);
    (void)g;

    /* fraction → Q12 */
    int32_t frac_q12 = (int32_t)(bp.fraction * 4096.0);
    if (frac_q12 < 0) frac_q12 = 0;
    if (frac_q12 > 4095) frac_q12 = 4095;

    uint32_t packed = ((uint32_t)bp.triangle & 0x7) << 16;
    packed |= (uint32_t)(frac_q12 & 0xFFFF);

    if (w < 0) packed |= 0x80000000u;  /* sign bit */
    return packed;
}

/* ══════════════════════════════════════════════════════════════
   DECODE: delta → weight
   ══════════════════════════════════════════════════════════════ */

static double decode_weight(const HexGrid *g, uint32_t packed)
{
    (void)g;
    int sign = (packed & 0x80000000u) ? -1 : 1;
    int triangle = (int)((packed >> 16) & 0x7);
    double fraction = (double)(packed & 0xFFFF) / 4096.0;

    /* Reconstruct weight from (triangle, fraction) */
    double angle = (double)triangle / 6.0 + fraction / 6.0;
    double R = 1.0;
    double w = (angle * 6.0) * R;  /* map back */

    return sign * w;
}

/* ══════════════════════════════════════════════════════════════
   TEST
   ══════════════════════════════════════════════════════════════ */

static void test_one_weight(const HexGrid *g, double w)
{
    uint32_t enc = encode_weight(g, w);
    double dec = decode_weight(g, enc);

    int t = (int)((enc >> 16) & 0x7);
    double frac = (double)(enc & 0xFFFF) / 4096.0;

    printf("  w=%-8.4f → enc=0x%08X (T%d, f=%.4f) → dec=%-8.4f  %s\n",
           w, enc, t, frac, dec,
           fabs(dec - w) < 0.01 ? "✓" : "✗");
}

static void test_roundtrip(const HexGrid *g)
{
    printf("─── Roundtrip Test ───\n\n");
    double test_w[] = {0, 0.5, 1.0, 1.5, 2.0, 3.0, 5.0, -1.0, -3.0};
    int ok = 0, total = 0;
    for (int i = 0; i < 9; i++) {
        uint32_t e = encode_weight(g, test_w[i]);
        double d = decode_weight(g, e);
        if (fabs(d - test_w[i]) < 0.01) ok++;
        total++;
    }
    for (double w = -6.0; w <= 6.0; w += 0.125) {
        uint32_t e = encode_weight(g, w);
        double d = decode_weight(g, e);
        if (fabs(d - w) < 0.01) ok++;
        total++;
    }
    printf("  Roundtrip: %d/%d PASS\n\n", ok, total);
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(void)
{
    double D = 4.0;
    HexGrid g = hex_build(D);

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   Hexagon Codec — beam ขึ้นอยู่กับ weight           ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    printf("  Hexagon D=%.1f\n", D);
    printf("  7 centroids: C0(center) + 6 shared outer\n");
    printf("  6 triangles: each with circumcircle D=%.1f\n", D);
    printf("\n");

    printf("─── Beam from weight ───\n\n");
    printf("  weight → (triangle, fraction) = position on hexagon\n");
    printf("  triangle = floor(fmod(|w|, 6))  (which of 6)\n");
    printf("  fraction = frac(fmod(|w|, 6))   (fine position)\n\n");

    for (double w = 0.0; w <= 6.0; w += 0.5) {
        BeamPos bp = beam_from_weight(w);
        char *names[] = {"E","NE","NW","W","SW","SE"};
        printf("  w=%-5.1f → T%d(%s) frac=%.4f radius=%.4f\n",
               w, bp.triangle, names[bp.triangle % 6],
               bp.fraction, bp.radius);
    }

    printf("\n");
    test_one_weight(&g, 0.0);
    test_one_weight(&g, 1.0);
    test_one_weight(&g, 1.5);
    test_one_weight(&g, 2.7);
    test_one_weight(&g, 5.3);
    test_one_weight(&g, -1.0);

    printf("\n");
    test_roundtrip(&g);

    printf("─── Key Insight ───\n");
    printf("  beam = f(weight) only\n");
    printf("  weight w → angle = fmod(|w|, 6)/6 × 2π\n");
    printf("  → position on hexagon → triangle + fraction = delta\n");
    printf("  → decode returns approximate w\n");
    printf("\n");
    printf("  Still open: mapping w → angle ต้อง stable พอ\n");
    printf("  สำหรับ cross-dimension transform (3D→1D→2D)\n");
    printf("\n");

    return 0;
}
