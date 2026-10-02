/* Geometry probe only: equal triangle with inward semicircles.
 * This does not define an address space or store any payload.
 */
#include <math.h>
#include <stdio.h>

typedef struct { double x, y; } Pt;
typedef struct { Pt c; double r; } Circle;

static Pt add(Pt a, Pt b) { return (Pt){a.x + b.x, a.y + b.y}; }
static Pt sub(Pt a, Pt b) { return (Pt){a.x - b.x, a.y - b.y}; }
static Pt mul(Pt a, double k) { return (Pt){a.x * k, a.y * k}; }
static double dot(Pt a, Pt b) { return a.x * b.x + a.y * b.y; }
static double norm(Pt a) { return sqrt(dot(a, a)); }
static Pt midpoint(Pt a, Pt b) { return mul(add(a, b), 0.5); }

static int near(double a, double b) { return fabs(a - b) < 1e-8; }
static int same(Pt a, Pt b) { return near(a.x, b.x) && near(a.y, b.y); }

/* Other circle intersection, excluding the shared endpoint of two sides. */
static int other_intersection(Circle a, Circle b, Pt shared, Pt *out)
{
    Pt dvec = sub(b.c, a.c);
    double d = norm(dvec);
    if (d < 1e-12 || d > a.r + b.r || d < fabs(a.r - b.r)) return 0;
    Pt ex = mul(dvec, 1.0 / d);
    double along = (a.r * a.r - b.r * b.r + d * d) / (2.0 * d);
    double h2 = a.r * a.r - along * along;
    if (h2 < -1e-10) return 0;
    double h = sqrt(h2 < 0.0 ? 0.0 : h2);
    Pt base = add(a.c, mul(ex, along));
    Pt ey = (Pt){-ex.y, ex.x};
    Pt p = add(base, mul(ey, h));
    Pt q = sub(base, mul(ey, h));
    if (same(p, shared)) *out = q;
    else if (same(q, shared)) *out = p;
    else *out = p;
    return !same(*out, shared);
}

static int equal_triangle(Pt t[3], double *side)
{
    double a = norm(sub(t[1], t[0]));
    double b = norm(sub(t[2], t[1]));
    double c = norm(sub(t[0], t[2]));
    *side = a;
    return fabs(a - b) < 1e-8 && fabs(b - c) < 1e-8;
}

static int inward(Pt p, Pt a, Pt b, Pt center)
{
    Pt m = midpoint(a, b);
    /* Point is on the semicircle side facing the triangle centroid. */
    return dot(sub(p, m), sub(center, m)) >= -1e-8;
}

static int next_triangle(Pt t[3], Pt next[3])
{
    Pt center = mul(add(add(t[0], t[1]), t[2]), 1.0 / 3.0);
    Circle c[3];
    Pt a[3] = {t[0], t[1], t[2]};
    for (int i = 0; i < 3; i++) {
        Pt u = a[i], v = a[(i + 1) % 3];
        c[i].c = midpoint(u, v);
        c[i].r = norm(sub(u, v)) * 0.5;
    }
    /* Adjacent side circles share t[1], t[2], t[0] respectively. */
    if (!other_intersection(c[0], c[1], t[1], &next[0])) return 0;
    if (!other_intersection(c[1], c[2], t[2], &next[1])) return 0;
    if (!other_intersection(c[2], c[0], t[0], &next[2])) return 0;
    for (int i = 0; i < 3; i++)
        if (!inward(next[i], a[(i + 1) % 3], a[(i + 2) % 3], center)) return 0;
    return 1;
}

static int check(int ok, const char *name, int *pass, int *fail)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) (*pass)++; else (*fail)++;
    return ok;
}

int main(void)
{
    const double s = 2.0;
    Pt t[3] = {{-s / 2.0, 0.0}, {s / 2.0, 0.0}, {0.0, -s * sqrt(3.0) / 2.0}};
    int pass = 0, fail = 0;
    double outer_side = 0.0, inner_side = 0.0;
    Pt inner[3], deeper[3];
    check(equal_triangle(t, &outer_side), "outer triangle is equilateral", &pass, &fail);
    check(next_triangle(t, inner), "three inward semicircles produce an inner triangle", &pass, &fail);
    check(equal_triangle(inner, &inner_side), "first intersection triangle is equilateral", &pass, &fail);
    check(inner_side < outer_side, "intersection triangle contracts", &pass, &fail);
    check(next_triangle(inner, deeper), "same rule produces a second layer", &pass, &fail);
    check(equal_triangle(deeper, &inner_side), "second intersection triangle is equilateral", &pass, &fail);
    printf("outer side=%.9f inner side=%.9f\n", outer_side, norm(sub(inner[1], inner[0])));
    printf("result: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
