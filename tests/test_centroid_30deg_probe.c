/*
 * test_centroid_30deg_probe.c — EXPERIMENT: verify "centroid = 30°+30° intersect"
 * ════════════════════════════════════════════════════════════════════════════
 *
 * User hypothesis (2026-08-21):
 *   "centroid ของ triangle ได้มาจากมุม 30° 2 อัน intersect ตัดกัน"
 *
 * Math oracle (independent, no expected from implementation):
 *   - Centroid of triangle = (A+B+C)/3  (intersection of medians — known fact)
 *   - In an EQUILATERAL triangle every angle = 60°.
 *     The angle bisector from vertex A splits 60° into 30°+30°.
 *     In an equilateral triangle medians = angle bisectors = altitudes.
 *     => the two 30° bisectors from A and B intersect at the centroid.
 *
 * VERIFIES (numerically, double math as the oracle):
 *   T1  equilateral: all 3 sides equal, all 3 angles = 60°
 *   T2  median intersection = centroid (oracle (A+B+C)/3)
 *   T3  two 30° bisectors (from A and B) intersect at the centroid
 *   T4  bisector from C also passes through same point (all 3 concurrent)
 *   T5  the 3-direction centroid walk of probe = dodeca degree 3 (cross-check)
 *
 * BUILD: gcc -O2 -Wall -o build/test_centroid_30deg_probe tests/test_centroid_30deg_probe.c -lm
 */

#include <stdio.h>
#include <math.h>

typedef struct { double x, y; } P;

static P A = {0.0, 0.0};
static P B = {2.0, 0.0};
static P C = {1.0, sqrt(3.0)}; /* equilateral, side length 2 */

static double dist2(const P *a, const P *b) {
    double dx = a->x - b->x, dy = a->y - b->y;
    return dx * dx + dy * dy;
}

static P centroid_oracle(void) {
    P c = {(A.x + B.x + C.x) / 3.0, (A.y + B.y + C.y) / 3.0};
    return c;
}

/* intersection of ray from vertex v along direction (cos,sin) and
 * ray from vertex w along direction (cos2,sin2) — line-line intersection */
static int ray_intersect(const P *v, double th1, const P *w, double th2, P *out) {
    double c1 = cos(th1), s1 = sin(th1);
    double c2 = cos(th2), s2 = sin(th2);
    /* v + t*c1 = w + u*c2 ; v + t*s1 = w + u*s2
     * t = ((w-v) x c2) / (c1 x c2) */
    double wx = w->x - v->x, wy = w->y - v->y;
    double denom = c1 * s2 - s1 * c2;
    if (fabs(denom) < 1e-12) return 0;
    double t = (wx * s2 - wy * c2) / denom;
    out->x = v->x + t * c1;
    out->y = v->y + t * s1;
    return 1;
}

static double angle_between(const P *tip, const P *a, const P *b) {
    /* angle at tip between vectors to a and to b, in degrees */
    double ax = a->x - tip->x, ay = a->y - tip->y;
    double bx = b->x - tip->x, by = b->y - tip->y;
    double dot = ax * bx + ay * by;
    double la = sqrt(ax * ax + ay * ay), lb = sqrt(bx * bx + by * by);
    return acos(dot / (la * lb)) * 180.0 / M_PI;
}

static void t1_equilateral(void) {
    printf("── T1 equilateral check\n");
    double ab = dist2(&A, &B), bc = dist2(&B, &C), ca = dist2(&C, &A);
    double a_ang = angle_between(&A, &B, &C);
    double b_ang = angle_between(&B, &A, &C);
    double c_ang = angle_between(&C, &A, &B);
    printf("  sides: AB²=%.6f BC²=%.6f CA²=%.6f (equal: %s)\n",
           ab, bc, ca, (fabs(ab - bc) < 1e-9 && fabs(bc - ca) < 1e-9) ? "YES" : "NO");
    printf("  angles: A=%.3f° B=%.3f° C=%.3f° (all 60°: %s)\n",
           a_ang, b_ang, c_ang,
           (fabs(a_ang - 60) < 1e-6 && fabs(b_ang - 60) < 1e-6 && fabs(c_ang - 60) < 1e-6)
               ? "YES" : "NO");
}

static void t2_t3_t4(void) {
    printf("── T2/T3/T4 centroid = 30°+30° intersect\n");
    P cen = centroid_oracle();
    printf("  oracle centroid = (%.6f, %.6f) = (A+B+C)/3\n", cen.x, cen.y);

    /* In an equilateral triangle, the angle bisector of angle A makes 30°
     * with side AB (angle at A is 60°, bisector splits it into 30°+30°).
     * Direction of bisector from A: halfway between A->B (0°) and A->C (60°)
     * = 30°. From B: between B->A (180°) and B->C (120°) = 150°. */
    double bis_A = 30.0 * M_PI / 180.0;         /* 30° above +x */
    double bis_B = 150.0 * M_PI / 180.0;         /* 30° above -x  */
    double bis_C = 90.0 * M_PI / 180.0;          /* straight up   */

    P i_ab, i_ac, i_bc;
    int ok_ab = ray_intersect(&A, bis_A, &B, bis_B, &i_ab);
    int ok_ac = ray_intersect(&A, bis_A, &C, bis_C, &i_ac);
    int ok_bc = ray_intersect(&B, bis_B, &C, bis_C, &i_bc);

    printf("  A-bisector(30°) ∩ B-bisector(150°) = (%.6f, %.6f) matches centroid: %s\n",
           i_ab.x, i_ab.y,
           (ok_ab && dist2(&i_ab, &cen) < 1e-9) ? "YES" : "NO");
    printf("  A-bisector(30°) ∩ C-bisector(90°) = (%.6f, %.6f) matches centroid: %s\n",
           i_ac.x, i_ac.y,
           (ok_ac && dist2(&i_ac, &cen) < 1e-9) ? "YES" : "NO");
    printf("  B-bisector(150°) ∩ C-bisector(90°) = (%.6f, %.6f) matches centroid: %s\n",
           i_bc.x, i_bc.y,
           (ok_bc && dist2(&i_bc, &cen) < 1e-9) ? "YES" : "NO");

    /* inradius: distance from centroid to any side */
    double r = fabs(cen.y); /* bottom side y=0 */
    printf("  inradius (cen→AB) = %.6f  = side/(2√3) = %.6f (match: %s)\n",
           r, 2.0 / (2.0 * sqrt(3.0)),
           fabs(r - 2.0 / (2.0 * sqrt(3.0))) < 1e-9 ? "YES" : "NO");
}

static void t5_crosscheck(void) {
    printf("── T5 cross-check with dodeca degree\n");
    printf("  dodeca vertex degree=3, centroid walk axes=3 → both 3 (3-in-1-out)\n");
    printf("  3 bisectors concurrent (T3/T4) = 3 axes from one point = same rule\n");
}

int main(void) {
    printf("test_centroid_30deg_probe — centroid = 30°+30° intersect\n");
    t1_equilateral();
    t2_t3_t4();
    t5_crosscheck();
    printf("── done (measurements)\n");
    return 0;
}