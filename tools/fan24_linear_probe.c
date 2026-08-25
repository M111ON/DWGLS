/*
 * fan24_linear_probe.c — Construction L (Linear carrier)
 *
 * 24-gon unfolded ("กางออก") into a straight line: 24 dots, spacing = 1 cell.
 * Per reference image: one regular aa-gon per dot —
 *   - vertex 0 of the shape pinned ON the dot (anchor = carrier point)
 *   - body hangs BELOW the line (y <= 0)
 *   - shape circumradius R = 1 cell (= spacing)
 *
 * Placement math (int-only, SCALE=10000):
 *   center  C = (ix_k, iy_k − R)          [directly under the dot]
 *   vertex j: theta = pi/2 + 2*pi*j/aa    [j=0 points UP to the dot]
 *   x_j = ix_k − R*sin(2*pi*j/aa)
 *   y_j = iy_k − R + R*cos(2*pi*j/aa)     [= iy_k iff j==0]
 *
 * Checks F1-F5 hard, contact map (shared verts between neighbours) reported.
 * Foundation layer for construction B (edge-mounted ring) and C (hub fan).
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N      24
#define SCALE  10000           /* 1 cell */
#define RADIUS 10000           /* shape circumradius = 1 cell */
#define MAXV   (N * 24)

static int ix[N], iy[N];
static int sx[N][24], sy[N][24];
static int counts[N];

static void init_carrier(void) {
    for (int k = 0; k < N; k++) { ix[k] = k * SCALE; iy[k] = 0; }
}

static void place_shapes(int aa) {
    for (int k = 0; k < N; k++) {
        counts[k] = aa;
        double cx = (double)ix[k], cy = (double)iy[k] - RADIUS;
        for (int j = 0; j < aa; j++) {
            double t = 2.0 * M_PI * j / aa;
            double x = cx - RADIUS * sin(t);
            double y = cy + RADIUS * cos(t);
            sx[k][j] = (int)(x + 0.5);
            sy[k][j] = (int)(y + 0.5);
        }
    }
}

static int near(int ax, int ay, int bx, int by) {
    int dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy <= 4;             /* <= 2 units = rounding noise */
}

/* all sides of shape k equal, within int-rounding tolerance (~8*side/SCALE-ish) */
static int check_regular(int k, int aa) {
    if (aa < 3) return 1;
    long long l0 = -1;
    for (int j = 0; j < aa; j++) {
        int j2 = (j + 1) % aa;
        long long dx = sx[k][j2] - sx[k][j], dy = sy[k][j2] - sy[k][j];
        long long l = dx * dx + dy * dy;
        if (l0 < 0) { l0 = l; continue; }
        long long tol = 8 * (long long)sqrt((double)l0) + 16;
        if (llabs(l - l0) > tol) return 0;
    }
    return 1;
}

/* contact map: shared vertices between shape k and shape k+1 */
static int contact_pair(int k) {
    int c = 0;
    for (int j = 0; j < counts[k]; j++)
        for (int m = 0; m < counts[k + 1]; m++)
            if (near(sx[k][j], sy[k][j], sx[k + 1][m], sy[k + 1][m])) c++;
    return c;
}

static int run_probe(int aa) {
    place_shapes(aa);
    int pass = 0, fail = 0;
    char b[80];
    #define CHECK(name, cond) do { \
        if (cond) { pass++; } else { fail++; printf("  FAIL %s\n", name); } \
    } while (0)

    printf("aa=%2d:\n", aa);

    /* F1 carrier: 24 dots, uniform spacing, on the line */
    snprintf(b, sizeof(b), "F1 carrier uniform");
    int f1 = 1;
    for (int k = 0; k < N; k++)
        f1 &= (iy[k] == 0) && (k == 0 || ix[k] - ix[k-1] == SCALE);
    CHECK(b, f1);

    /* F2 anchor: vertex0 of shape k sits exactly on dot k */
    snprintf(b, sizeof(b), "F2 anchor=dot");
    int f2 = 1;
    for (int k = 0; k < N; k++)
        f2 &= (sx[k][0] == ix[k]) && (sy[k][0] == iy[k]);
    CHECK(b, f2);

    /* F3 census: every shape carries exactly aa vertices */
    snprintf(b, sizeof(b), "F3 census aa each");
    int f3 = 1;
    for (int k = 0; k < N; k++) f3 &= (counts[k] == aa);
    CHECK(b, f3);

    /* F4 regularity */
    snprintf(b, sizeof(b), "F4 regular");
    int f4 = 1;
    for (int k = 0; k < N; k++) f4 &= check_regular(k, aa);
    CHECK(b, f4);

    /* F5 half-plane: every non-anchor vertex below the line */
    snprintf(b, sizeof(b), "F5 hang below");
    int f5 = 1;
    for (int k = 0; k < N; k++)
        for (int j = 1; j < counts[k]; j++)
            if (sy[k][j] > 2) f5 = 0;
    CHECK(b, f5);

    /* F6 contact map (informational): neighbour shapes sharing vertices */
    int total_contact = 0;
    printf("  contact:");
    for (int k = 0; k < N - 1; k++) {
        int c = contact_pair(k);
        total_contact += c;
        printf(" %d", c);
    }
    printf("   | total=%d\n", total_contact);

    printf("  %d/%d GREEN\n\n", pass, pass + fail);
    return fail;
}

int main(void) {
    printf("fan24_linear_probe — Construction L (unfolded 24-gon)\n");
    printf("anchor=v0 on dot, hang_below, R=1cell, spacing=1cell\n\n");

    int total_fail = 0;
    int tests[] = {3, 4, 5, 6, 7, 8, 10, 12, 24};
    int nt = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < nt; i++) {
        init_carrier();
        total_fail += run_probe(tests[i]);
    }

    printf("TOTAL: %s (%d aa values)\n",
           total_fail == 0 ? "ALL GREEN" : "HAS FAILS", nt);
    return total_fail ? 1 : 0;
}
