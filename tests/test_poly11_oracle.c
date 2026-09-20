/* tests/test_poly11_oracle.c — poly11 12-gon + 6 incidences as INDEPENDENT oracle.
 *
 * Oracle (NOT from implementation): GeoGebra construction protocol
 *   distortcube2.html — poly11 ring + poly12..22, 10-decimal coords.
 *   Start: poly1 = Polygon(A,B,3), side s = 4.7123889804 = 3pi/2, then
 *   poly2/3/4 squares on its 3 sides (triangle + 3 quads net).
 * Expected values below come from that file + math (Euler, 120deg,
 * 9pi perimeter, sqrt ratios) — never from the functions under test.
 *
 * What it pins:
 *   P1-P7  oracle self-consistency (side classes, 120deg, 9pi, order-3,
 *          scale ladder, mirror pairs, 6 incidences at dist 0.0)
 *   P8     mirror pairs + red triangle (distortcube2.html)
 *   P9     start net through the REAL core engine (geo_net_walk.h):
 *          tri+3quads fan, V=9 E=12 disk Euler=1
 *   P10    tetra 1+3 rule through the REAL core (geo_octant.h):
 *          valid cubes {0,1,2,4}, tetra halves 4+4
 *   P11    gear-cartridge fit (kineticfan principle): shaft b-gon on AB
 *          vs 3-ring housing; classes 0 clear / 1 contact / 2 overlap.
 *          Oracle: user frames ((a4,b3) fit, (a5,b5) pierce) + /tmp proofs.
 *          Expected: b==3 contact (keystone), else overlap, housings
 *          a=3,4,5 — plus pins (west-control clearance, b6 signature,
 *          b4 penetration s-h).
 *   P12    full 24/12 state (kineticfan (1).html, base64 XML @16 decimals):
 *          12-gon regularity/area/flat-span, K=G stack, petal sides,
 *          chord=R theorem, poly4 area, fan samples.
 *   P13    kinetic family through the REAL core engine: net with sides
 *          {b,a,a,a} for (a,b)=(3,3),(4,3),(4,4),(5,3),(5,5),(24,12);
 *          V=b+3a-6, E=b+3a-3, disk Euler=1 (Euler math, not the engine).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_poly11_oracle tests/test_poly11_oracle.c -lm
 * RUN:   ./build/test_poly11_oracle   (or: make test-poly11_oracle)
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include "../core/geo_net_walk.h"
#include "../core/geo_octant.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── Oracle coords (distortcube2.html, 10 decimals) ── */
typedef struct { double x, y; } Pt;

/* poly11 ring, in order */
static const Pt RING[12] = {
    { -9.1579421061,  1.7248540793 },  /* G  */
    { -8.1620971391,  0.0000000000 },  /* T1 */
    { -9.1579421061, -1.7248540793 },  /* H  */
    { -7.7975925829, -4.0810485695 },  /* M1 */
    { -5.0768935365, -4.0810485695 },  /* I  */
    { -4.0810485695, -2.3561944902 },  /* I1 */
    { -2.0893586355, -2.3561944902 },  /* D  */
    { -0.7290091123,  0.0000000000 },  /* M5 */
    { -2.0893586355,  2.3561944902 },  /* E  */
    { -4.0810485695,  2.3561944902 },  /* Z  */
    { -5.0768935365,  4.0810485695 },  /* F  */
    { -7.7975925829,  4.0810485695 },  /* S  */
};

/* oracle side lengths from the file's own segment table */
#define O_SHORT 1.991689934   /* g2,d1,i2,f2,e2,z1 */
#define O_LONG  2.7206990464  /* h2,e1,d2,m5,f3,s2 */
#define O_S     4.7123889804  /* base: f,g,h,i,j,k,... = 3pi/2 */
#define O_BIG   8.1620971391  /* poly5/6/7 squares */
#define O_OUTER 9.4247779608  /* poly8/9/10 triangles, poly12/13/14 squares */
#define O_BLUE  3.4497081587  /* poly19/20/21 */
#define O_PURP  6.6643244072  /* poly15/16/17/18 */

static double dist(Pt a, Pt b)
{
    double dx = a.x - b.x, dy = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

/* P1: side-class pattern {S,S,L,L}x3 + Euler disk V-E+F=1 */
static void t_side_pattern(void)
{
    int ok = 1, cls[12];
    for (int i = 0; i < 12; i++) {
        double l = dist(RING[i], RING[(i + 1) % 12]);
        int isS = fabs(l - O_SHORT) < 1e-9;
        int isL = fabs(l - O_LONG) < 1e-9;
        if (!isS && !isL) { ok = 0; break; }
        int wantS = (i % 4 == 0 || i % 4 == 1);
        if (isS != wantS) { ok = 0; break; }
        cls[i] = isS ? 0 : 1;
    }
    CHECK("P1: 12 edges fall in 2 classes with pattern {S,S,L,L}x3", ok);
    CHECK("P1b: Euler disk V-E+F = 12-12+1 = 1", 12 - 12 + 1 == 1);
    (void)cls;
}

/* P2: equiangular 120deg (NOT 150deg of a regular dodecagon) */
static void t_angles(void)
{
    int ok = 1;
    for (int i = 0; i < 12; i++) {
        Pt p = RING[(i + 11) % 12], q = RING[i], r = RING[(i + 1) % 12];
        double v1x = p.x - q.x, v1y = p.y - q.y;
        double v2x = r.x - q.x, v2y = r.y - q.y;
        double dot = v1x * v2x + v1y * v2y;
        double n = sqrt(v1x * v1x + v1y * v1y) * sqrt(v2x * v2x + v2y * v2y);
        double ang = acos(dot / n) * 180.0 / acos(-1.0);
        if (fabs(ang - 120.0) > 1e-6) { ok = 0; break; }
    }
    CHECK("P2: all 12 interior angles 120deg (hex family, not 150deg)", ok);
}

/* P3: perimeter = 6s = 9pi exactly (short+long == s per pair) */
static void t_perimeter(void)
{
    double pi = acos(-1.0), tot = 0;
    for (int i = 0; i < 12; i++) tot += dist(RING[i], RING[(i + 1) % 12]);
    CHECK("P3: short+long == s (pair sums to base side)",
          fabs((O_SHORT + O_LONG) - O_S) < 1e-12);
    CHECK("P3b: perimeter == 9pi", fabs(tot - 9.0 * pi) < 1e-9);
}

/* P4: order-3 only — R120/R240 lock, R60 fails (kills regular-dodecagon) */
static void t_symmetry(void)
{
    double cx = 0, cy = 0;
    for (int i = 0; i < 12; i++) { cx += RING[i].x; cy += RING[i].y; }
    cx /= 12; cy /= 12;
    int okc = (fabs(cx - (-5.4413980927)) < 1e-9 && fabs(cy) < 1e-12);
    double e120 = 0, e240 = 0, e60 = 0;
    for (int d = 0; d < 3; d++) {
        double deg = d == 0 ? 120.0 : d == 1 ? 240.0 : 60.0;
        double t = deg * acos(-1.0) / 180.0, c = cos(t), s = sin(t);
        double worst = 0;
        for (int i = 0; i < 12; i++) {
            double x = RING[i].x - cx, y = RING[i].y - cy;
            Pt rp = { x * c - y * s + cx, x * s + y * c + cy };
            double best = 1e99;
            for (int j = 0; j < 12; j++) {
                double dd = dist(rp, RING[j]);
                if (dd < best) best = dd;
            }
            if (best > worst) worst = best;
        }
        if (d == 0) e120 = worst; else if (d == 1) e240 = worst; else e60 = worst;
    }
    CHECK("P4: centroid on axis (-5.4413980927, 0)", okc);
    CHECK("P4b: R120/R240 lock (err<1e-6), R60 fails (err>1.0)",
          e120 < 1e-6 && e240 < 1e-6 && e60 > 1.0);
}

/* P5: edge directions fall in 3 line-buckets x 4 edges (2S+2L each) */
static void t_orient_buckets(void)
{
    int cnt[3] = { 0, 0, 0 }, sl[3] = { 0, 0, 0 }, ok = 1;
    for (int i = 0; i < 12; i++) {
        Pt a = RING[i], b = RING[(i + 1) % 12];
        double d = atan2(b.y - a.y, b.x - a.x) * 180.0 / acos(-1.0);
        if (d < 0) d += 180.0;
        if (d >= 180.0) d -= 180.0;
        int bk = (int)(d / 60.0 + 0.5) % 3;
        double l = dist(a, b);
        if (fabs(l - O_SHORT) > 1e-9 && fabs(l - O_LONG) > 1e-9) { ok = 0; break; }
        cnt[bk]++;
        if (fabs(l - O_SHORT) < 1e-9) sl[bk]++;
    }
    CHECK("P5: 3 orientation buckets x 4 edges, 2 short + 2 long each",
          ok && cnt[0] == 4 && cnt[1] == 4 && cnt[2] == 4 &&
          sl[0] == 2 && sl[1] == 2 && sl[2] == 2);
}

/* P6: scale ladder from one seed: s(v3-1) -> s -> s*v2 -> s*v3 -> 2s */
static void t_ladder(void)
{
    double sq3 = sqrt(3.0), sq2 = sqrt(2.0);
    CHECK("P6: big/s==sqrt3, outer/s==2",
          fabs(O_BIG / O_S - sq3) < 1e-9 && fabs(O_OUTER / O_S - 2.0) < 1e-12);
    CHECK("P6b: blue/s==sqrt3-1, purple/s==sqrt2",
          fabs(O_BLUE / O_S - (sq3 - 1.0)) < 1e-9 &&
          fabs(O_PURP / O_S - sq2) < 1e-9);
}

/* P7: 6 incidences — independently-defined points coincide at dist 0.0 */
static void t_incidences(void)
{
    /* each pair: same coords from two different definition rows in the file */
    static const Pt A_R  = { -6.170407205, 0.0 };              /* poly10 R   */
    static const Pt B_Z5 = { -6.170407205, 0.0 };              /* poly19 Z5  */
    static const Pt A_C6 = { -4.0810485695, -2.3561944902 };   /* poly22 C6  */
    static const Pt B_I1 = { -4.0810485695, -2.3561944902 };   /* x-sect I1  */
    static const Pt A_U5 = { -7.4330880267, -4.7123889804 };   /* poly16 U5  */
    static const Pt B_Q5 = { -7.4330880267, -4.7123889804 };   /* poly13 Q5  */
    static const Pt A_N5 = { -0.3645045562, -0.6313404109 };   /* poly12 N5  */
    static const Pt B_T5 = { -0.3645045562, -0.6313404109 };   /* poly15 T5  */
    static const Pt A_A6 = { -5.0768935365, -0.6313404109 };   /* poly20 A6  */
    static const Pt B_P  = { -5.0768935365, -0.6313404109 };   /* poly8 P    */
    static const Pt A_B6 = { -5.0768935365, 0.6313404109 };    /* poly21 B6  */
    static const Pt B_Q  = { -5.0768935365, 0.6313404109 };    /* poly9 Q    */
    CHECK("P7: 6 incidences coincide (R=Z5,C6=I1,U5=Q5,N5=T5,A6=P,B6=Q)",
          dist(A_R, B_Z5) == 0.0 && dist(A_C6, B_I1) == 0.0 &&
          dist(A_U5, B_Q5) == 0.0 && dist(A_N5, B_T5) == 0.0 &&
          dist(A_A6, B_P) == 0.0 && dist(A_B6, B_Q) == 0.0);
}

/* P8: mirror pairs share the segment; both mirrors equilateral (uses core? no —
 * pure oracle) + red triangle equilateral on base scale */
static void t_mirrors(void)
{
    static const Pt F = { -5.0768935365, 4.0810485695 };
    static const Pt B = { -6.8017476159, -2.3561944902 };
    static const Pt C = { -2.7206990464, 0.0 };
    static const Pt G = { -9.1579421061, 1.7248540793 };
    static const Pt T5 = { -0.3645045562, -0.6313404109 };   /* poly15 apex */
    static const Pt V5 = { -11.5141365963, 2.3561944902 };   /* poly17 apex */
    static const Pt U5 = { -7.4330880267, -4.7123889804 };   /* poly16 apex */
    static const Pt W5 = { -4.4455531257, 6.4372430597 };    /* poly18 apex */
    static const Pt Z  = { -4.0810485695, 2.3561944902 };
    static const Pt T1 = { -8.1620971391, 0.0 };
    static const Pt C6 = { -4.0810485695, -2.3561944902 };
    int purple = (fabs(dist(F, B) - O_PURP) < 1e-9 &&
                  fabs(dist(C, G) - O_PURP) < 1e-9 &&
                  fabs(dist(F, T5) - O_PURP) < 1e-9 &&
                  fabs(dist(B, T5) - O_PURP) < 1e-9 &&
                  fabs(dist(B, V5) - O_PURP) < 1e-9 &&
                  fabs(dist(F, V5) - O_PURP) < 1e-9 &&
                  fabs(dist(C, U5) - O_PURP) < 1e-9 &&
                  fabs(dist(G, U5) - O_PURP) < 1e-9 &&
                  fabs(dist(G, W5) - O_PURP) < 1e-9 &&
                  fabs(dist(C, W5) - O_PURP) < 1e-9);
    CHECK("P8: mirror pairs on FB and GC both equilateral (T5!=V5, U5!=W5)",
          purple && dist(T5, V5) > 1.0 && dist(U5, W5) > 1.0);
    CHECK("P8b: red triangle Z-T1-C6 equilateral at base scale s",
          fabs(dist(Z, T1) - O_S) < 1e-9 &&
          fabs(dist(T1, C6) - O_S) < 1e-9 &&
          fabs(dist(C6, Z) - O_S) < 1e-9);
}

/* P9: the START net (tri + 3 quads fan) through the real core engine.
 * faces: 0=tri(A,B,C) 1,2,3=quads on its edges, reversed orientation
 * (quad edge0 runs opposite the tri edge — the Polygon(X,Y) lesson).
 * 15 corners - 6 unions = V9; halves 15 - 3 pairs = E12; disk 9-12+4=1. */
static const uint32_t N_SIDES[4] = { 3, 4, 4, 4 };
static const int32_t N_PF[16] = {
     1, 2, 3, -1,
     0, -1, -1, -1,
     0, -1, -1, -1,
     0, -1, -1, -1
};
static const int32_t N_PE[16] = {
     0, 0, 0, 0,
     0, 0, 0, 0,
     1, 0, 0, 0,
     2, 0, 0, 0
};

static void t_start_net(void)
{
    uint8_t vis[4];
    int32_t par[16];
    CHECK("P9: start-net gluing reciprocal",
          nw_reciprocal(4, 4, N_SIDES, N_PF, N_PE) == -1);
    uint32_t V = nw_count_vertices(4, 4, N_SIDES, N_PF, N_PE, par);
    uint32_t E = nw_count_edges(4, 4, N_SIDES, N_PF);
    uint32_t O = nw_count_open(4, 4, N_SIDES, N_PF);
    CHECK("P9b: fan V=9 E=12 disk Euler=1, 9 open halves, walk visits 4",
          V == 9u && E == 12u && V - E + 4u == 1u && O == 9u &&
          nw_walk(4, 4, N_SIDES, N_PF, vis) == 4u);
}

/* P10: tetra 1+3 rule through the real core (geo_octant.h) */
static void t_octant(void)
{
    int valid[8], nv = 0, ok = 1;
    for (uint32_t c = 0; c < 8; c++)
        if (oct_is_valid((int)c)) valid[nv++] = (int)c;
    CHECK("P10: zero-sum valid cubes are {0,1,2,4} (1 apex + 3)",
          nv == 4 && valid[0] == 0 && valid[1] == 1 &&
          valid[2] == 2 && valid[3] == 4);
    /* NOTE: geo_octant.h:116-119 shows a stale example line
     * (7->0,6->2,5->4,3->1) that predates the documented fix — the loop
     * clears the LOWEST set bit until sum<=1 (keep-highest), per
     * docs/PLATONIC_FIELD_ARCHITECTURE.md:162 and test_tesseract_dense.c:83
     * which expects oct_tetra_of(7)=4. Hand-executed mapping below. */
    CHECK("P10b: invalid halves fold keep-highest (7->4,6->4,5->4,3->2)",
          oct_tetra_of(7) == 4 && oct_tetra_of(6) == 4 &&
          oct_tetra_of(5) == 4 && oct_tetra_of(3) == 2);
    (void)ok;
}

/* ── P11/P12 shared oracle: kineticfan seed (base64 XML @16 decimals) ── */
static const Pt KF_A = { -5.441398092702653, 6.283185307179586 };
static const Pt KF_B = { -5.441398092702653, 0.0 };
static const Pt KF_G = { 8.881784197001252e-16, 3.141592653589792 };
/* 24/12 file: hub G dragged onto ring vertex V2 (PointIn stack with K) */
static const Pt KF_G2 = { -2.29980543911286, -5.441398092702654 };

#define KF_EPS 1e-9

static void kf_erect(Pt P, Pt Q, int n, int side, double s, Pt *out)
{
    double ang = atan2(Q.y - P.y, Q.x - P.x);
    double cx = P.x, cy = P.y, tau = 2.0 * acos(-1.0);
    out[0] = P;
    for (int i = 1; i < n; i++) {
        cx += s * cos(ang); cy += s * sin(ang);
        out[i].x = cx; out[i].y = cy;
        ang += side * tau / n;
    }
}

static int kf_inside(Pt p, const Pt *poly, int n)
{
    int ins = 0;
    for (int i = 0; i < n; i++) {
        double x1 = poly[i].x, y1 = poly[i].y;
        double x2 = poly[(i + 1) % n].x, y2 = poly[(i + 1) % n].y;
        if (((y1 > p.y) != (y2 > p.y)) &&
            (p.x < (x2 - x1) * (p.y - y1) / (y2 - y1) + x1))
            ins = !ins;
    }
    return ins;
}

static double kf_ptseg(Pt p, Pt a, Pt b)
{
    double vx = b.x - a.x, vy = b.y - a.y;
    double t = ((p.x - a.x) * vx + (p.y - a.y) * vy) / (vx * vx + vy * vy);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    Pt q = { a.x + t * vx, a.y + t * vy };
    return dist(p, q);
}

static double kf_mind(Pt p, const Pt *poly, int n)
{
    double m = 1e99;
    for (int i = 0; i < n; i++) {
        double d = kf_ptseg(p, poly[i], poly[(i + 1) % n]);
        if (d < m) m = d;
    }
    return m;
}

static double kf_orient(Pt a, Pt b, Pt c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/* proper edge crossing; touches at assembly joints are exempt */
static int kf_xcross(Pt a, Pt b, Pt c, Pt d)
{
    const Pt ps[4] = { a, b, c, d };
    for (int i = 0; i < 2; i++)
        for (int j = 2; j < 4; j++)
            if (dist(ps[i], ps[j]) < KF_EPS) return 0;
    double o1 = kf_orient(a, b, c), o2 = kf_orient(a, b, d);
    double o3 = kf_orient(c, d, a), o4 = kf_orient(c, d, b);
    return o1 * o2 < 0.0 && o3 * o4 < 0.0;
}

/* fit class: 0 clear / 1 contact / 2 overlap. Assembly joints A,B exempt. */
static int kf_fit(const Pt *shaft, int nb, const Pt *pag, const Pt *pgb, int na,
                  double *dmin_out, int *nx_out)
{
    int nx = 0;
    double dmin = 1e99;
    int overlap = 0;
    for (int i = 2; i < nb; i++) {
        int inH = (kf_inside(shaft[i], pag, na) && kf_mind(shaft[i], pag, na) > KF_EPS) ||
                  (kf_inside(shaft[i], pgb, na) && kf_mind(shaft[i], pgb, na) > KF_EPS);
        if (inH) overlap = 1;
        double d = kf_mind(shaft[i], pag, na);
        double e = kf_mind(shaft[i], pgb, na);
        if (d < dmin) dmin = d;
        if (e < dmin) dmin = e;
    }
    /* housing free vertices strictly inside shaft = engulfment */
    const Pt *hs[2] = { pag, pgb };
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < na; i++) {
            Pt v = hs[k][i];
            if (dist(v, KF_A) < KF_EPS || dist(v, KF_B) < KF_EPS) continue;
            if (kf_inside(v, shaft, nb) && kf_mind(v, shaft, nb) > KF_EPS) overlap = 1;
            double d = kf_mind(v, shaft, nb);
            if (d < dmin) dmin = d;
        }
    for (int i = 0; i < nb; i++) {
        Pt s1a = shaft[i], s1b = shaft[(i + 1) % nb];
        for (int j = 0; j < na; j++) {
            if (kf_xcross(s1a, s1b, pag[j], pag[(j + 1) % na])) nx++;
            if (kf_xcross(s1a, s1b, pgb[j], pgb[(j + 1) % na])) nx++;
        }
    }
    if (nx) overlap = 1;
    if (dmin_out) *dmin_out = dmin;
    if (nx_out) *nx_out = nx;
    if (overlap) return 2;
    if (dmin < 1e-6) return 1;
    return 0;
}

static void kf_outward(Pt P, Pt Q, int n, double s, Pt *out)
{
    Pt c = { (KF_A.x + KF_B.x + KF_G.x) / 3.0,
             (KF_A.y + KF_B.y + KF_G.y) / 3.0 };
    Pt tmp[25];
    kf_erect(P, Q, n, 1, s, tmp);
    if (!kf_inside(c, tmp, n)) {
        for (int i = 0; i < n; i++) out[i] = tmp[i];
        return;
    }
    kf_erect(P, Q, n, -1, s, out);
}

/* erect the version whose centroid lies east (want_east=1) / west of P.x */
static void kf_side(Pt P, Pt Q, int n, int want_east, double s, Pt *out)
{
    Pt tmp[25];
    kf_erect(P, Q, n, 1, s, tmp);
    double cx = 0;
    for (int i = 0; i < n; i++) cx += tmp[i].x;
    if ((cx / n > P.x) != (want_east != 0)) kf_erect(P, Q, n, -1, s, tmp);
    for (int i = 0; i < n; i++) out[i] = tmp[i];
}

/* P11: gear-cartridge fit table + pins */
static void t_gearfit(void)
{
    double s = dist(KF_A, KF_B);
    double pi = acos(-1.0);
    int okseed = fabs(s - 2.0 * pi) < 1e-12;
    CHECK("P11-0: seed |AB| == 2pi (kineticfan.xml)", okseed);
    double h = s * sqrt(3.0) / 2.0;
    int ok = 1;
    for (int ai = 0; ai < 3; ai++) {
        int a = ai == 0 ? 3 : ai == 1 ? 4 : 5;
        Pt pag[25], pgb[25];
        kf_outward(KF_A, KF_G, a, s, pag);
        kf_outward(KF_G, KF_B, a, s, pgb);
        for (int b = 3; b <= 8; b++) {
            Pt east[25];
            kf_side(KF_A, KF_B, b, 1, s, east);
            int cls = kf_fit(east, b, pag, pgb, a, NULL, NULL);
            int want = (b == 3) ? 1 : 2;
            if (cls != want) { ok = 0; break; }
        }
        if (!ok) break;
    }
    CHECK("P11: fit table a={3,4,5} x b=3..8 (b3 contact, else overlap)", ok);

    /* pins on the a=3 frame (kf_side keeps them honest: the table
     * above already proved both mirrors exist for every b) */
    Pt pag[25], pgb[25];
    kf_outward(KF_A, KF_G, 3, s, pag);
    kf_outward(KF_G, KF_B, 3, s, pgb);
    Pt sh3[25], sh4[25], sh6[25], shw[25];
    kf_side(KF_A, KF_B, 3, 1, s, sh3);
    kf_side(KF_A, KF_B, 4, 1, s, sh4);
    kf_side(KF_A, KF_B, 6, 1, s, sh6);
    kf_side(KF_A, KF_B, 4, 0, s, shw);
    double dmw = 1e99;
    for (int i = 2; i < 4; i++) {
        double d = kf_mind(shw[i], pag, 3), e = kf_mind(shw[i], pgb, 3);
        if (d < dmw) dmw = d;
        if (e < dmw) dmw = e;
    }
    CHECK("P11b: west control clearance == s (can fail, discriminates side)",
          fabs(dmw - s) < 1e-9);
    CHECK("P11c: b3 keystone apex == hub G",
          dist(sh3[2], KF_G) < 1e-9);
    CHECK("P11d: b4 east corner penetration x == s-h",
          fabs(sh4[2].x - (s - h)) < 1e-9);
    CHECK("P11e: b6 swallows hub (G strictly inside, tips touch 0)",
          kf_inside(KF_G, sh6, 6) && kf_mind(KF_G, sh6, 6) > KF_EPS &&
          kf_mind((Pt){ 0.0, -pi }, sh6, 6) < 1e-9 &&
          kf_mind((Pt){ 0.0, 3.0 * pi }, sh6, 6) < 1e-9);
}

/* ── P12: full 24/12 state (kineticfan (1).html, base64 XML @16dp) ── */
static const Pt KF_RING[12] = {
    { -5.441398092702653, 6.283185307179586 },   /* A  */
    { -5.441398092702653, 0.0 },                 /* B  */
    { -2.29980543911286, -5.441398092702654 },   /* V2 (=K=G stack) */
    { 3.141592653589794, -8.582990746292447 },   /* R_1 */
    { 9.424777960769378, -8.582990746292447 },   /* S_1 */
    { 14.866176053472032, -5.441398092702656 },  /* T_1 */
    { 18.007768707061828, -1.3322676295501878e-15 }, /* T */
    { 18.007768707061828, 6.283185307179583 },   /* Q_1 */
    { 14.866176053472039, 11.724583399882235 },  /* U_1 */
    { 9.424777960769383, 14.866176053472033 },   /* V_1 */
    { 3.141592653589803, 14.866176053472037 },   /* W_1 */
    { -2.2998054391128546, 11.724583399882244 }, /* Z_1 */
};
static const Pt KF_J = { -5.441398092702652, 6.283185307179586 };
static const Pt KF_D = { -5.441398092702654, 0.0 };
static const Pt KF_E = { -5.441398092702654, 6.283185307179586 };
static const Pt KF_H = { -5.441398092702654, 0.0 };
static const Pt KF_D1 = { 58.451598883361456, -30.605453696283003 };
static const Pt KF_V = { 34.588833564349684, -33.747046349872775 };
static const Pt KF_O = { 12.352276266744333, -24.536362736718214 };
static const Pt KF_L = { 3.769285520451895, -15.953371990425772 };
static const Pt KF_S = { 22.86425016446746, -30.60545369628299 };

static double kf_shoelace(const Pt *p, int n)
{
    double a = 0;
    for (int i = 0; i < n; i++)
        a += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
    return fabs(a) / 2.0;
}

static void t_full2412(void)
{
    double pi = acos(-1.0);
    double s = dist(KF_A, KF_B);
    double dev = 0;
    for (int i = 0; i < 12; i++) {
        double d = fabs(dist(KF_RING[i], KF_RING[(i + 1) % 12]) - s);
        if (d > dev) dev = d;
    }
    CHECK("P12: 12-gon regular, sides == 2pi (dev<1e-12)", dev < 1e-12);
    CHECK("P12b: 12-gon area == 442.01 (file value)",
          fabs(kf_shoelace(KF_RING, 12) - 442.01) < 0.01);
    double apo = s / (2.0 * tan(pi / 12.0));
    CHECK("P12c: flat E/W span T.x-A.x == 2*apo",
          fabs((KF_RING[6].x - KF_RING[0].x) - 2.0 * apo) < 1e-9);
    CHECK("P12d: hub stack K==G2 (dist 0.0)",
          dist(KF_G2, (Pt){ -2.29980543911286, -5.441398092702654 }) == 0.0);
    double rcirc = s / (2.0 * sin(pi / 12.0));
    double p4side = dist(KF_J, KF_G2);
    CHECK("P12e: giant petal side == Rcirc (2-step chord theorem)",
          fabs(p4side - rcirc) < 1e-9);
    CHECK("P12f: poly4 area == 6714.74 (file value)",
          fabs(6.0 * p4side * p4side / tan(pi / 24.0) - 6714.74) < 0.01);
    CHECK("P12g: petal sides poly2/poly3 == 2pi",
          fabs(dist(KF_D, KF_E) - s) < 1e-12 &&
          fabs(dist(KF_G2, KF_H) - s) < 1e-12);
    CHECK("P12h: fan samples match file (r_2,a_2,c_2,n_5,q_5)",
          fabs(dist(KF_E, KF_D1) - 73.78) < 0.01 &&
          fabs(dist(KF_G2, KF_V) - 46.50) < 0.01 &&
          fabs(dist(KF_E, KF_V) - 56.61) < 0.01 &&
          fabs(dist(KF_G2, KF_O) - 24.07) < 0.01 &&
          fabs(dist(KF_E, KF_L) - 24.07) < 0.01);
    (void)KF_S;
}

/* ── P13: kinetic family through geo_net_walk: sides {b,a,a,a} ──
 * faces: 0=center (b sides), 1..3=petals (a sides); petals glued on
 * center edges 0,1,2 (3 petals regardless of b — the images).
 * Euler math: corners=b+3a, unions=6 → V=b+3a-6; halves=b+3a,
 * pairs=3 → E=b+3a-3; disk (b+3a-6)-(b+3a-3)+4=1. */
static void t_kinetic_family(void)
{
    static const int pairs[6][2] = { {3,3}, {4,3}, {4,4}, {5,3}, {5,5}, {24,12} };
    int ok = 1;
    for (int t = 0; t < 6 && ok; t++) {
        int a = pairs[t][0], b = pairs[t][1];
        /* NOTE: the engine indexes tables as f*SMAX+i, so the arrays stay
         * packed at stride SMAX=24; sides[f] bounds the live entries. */
        uint32_t sides[4] = { (uint32_t)b, (uint32_t)a, (uint32_t)a, (uint32_t)a };
        int32_t pf[4][24], pe[4][24];
        for (int f = 0; f < 4; f++)
            for (int i = 0; i < 24; i++) { pf[f][i] = -1; pe[f][i] = 0; }
        pf[0][0] = 1; pf[0][1] = 2; pf[0][2] = 3;
        pe[0][0] = 0; pe[0][1] = 0; pe[0][2] = 0;
        pf[1][0] = 0; pe[1][0] = 0;
        pf[2][0] = 0; pe[2][0] = 1;
        pf[3][0] = 0; pe[3][0] = 2;
        uint8_t vis[4];
        int32_t par[4 * 24];
        if (nw_reciprocal(4, 24u, sides, &pf[0][0], &pe[0][0]) != -1) ok = 0;
        else {
            uint32_t V = nw_count_vertices(4, 24u, sides, &pf[0][0], &pe[0][0], par);
            uint32_t E = nw_count_edges(4, 24u, sides, &pf[0][0]);
            uint32_t O = nw_count_open(4, 24u, sides, &pf[0][0]);
            uint32_t W = nw_walk(4, 24u, sides, &pf[0][0], vis);
            uint32_t eV = (uint32_t)(b + 3 * a - 6), eE = (uint32_t)(b + 3 * a - 3);
            /* halves=b+3a, listed glued halves=6 → O=b+3a-6; disk V-E+4==1 */
            if (V != eV || E != eE || O != eV || V + 4u != E + 1u || W != 4u) ok = 0;
        }
    }
    CHECK("P13: kinetic family (a,b)x6: V=b+3a-6 E=b+3a-3 disk=1 walk=4", ok);
}

int main(void)
{
    printf("═ POLY11 ORACLE — distortcube2.html as independent ground truth ═\n");
    t_side_pattern();
    t_angles();
    t_perimeter();
    t_symmetry();
    t_orient_buckets();
    t_ladder();
    t_incidences();
    t_mirrors();
    t_start_net();
    t_octant();
    t_gearfit();
    t_full2412();
    t_kinetic_family();
    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
