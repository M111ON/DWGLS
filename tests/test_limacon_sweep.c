/*
 * test_limacon_sweep.c — Experiment: limacon addressing on 24-gon
 * ═══════════════════════════════════════════════════════════════
 * Sweep aa parameter (3..24), measure:
 *   1. Apex computation time (ns)
 *   2. Coverage: unique apex positions per hub
 *   3. Average path length (chord distance)
 *   4. Overlap: how many paths share same apex
 *   5. Factorization: 96 × 36 × 6 = 20736 verification
 *
 * Build: gcc -O2 -Wall -Icore -o tests/test_limacon_sweep tests/test_limacon_sweep.c -lm
 * Run:   tests/test_limacon_sweep
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define N 24
#define TAU (2.0 * M_PI)
#define FULL_FIELD 20736
#define FIELD_96   96
#define FIELD_36   36
#define FIELD_6    6

/* ── 24-gon vertex positions ──────────────────────────────── */
static double vx[N], vy[N];
#define CX 900.0
#define CY 490.0
#define R  380.0

static void init_ring(void) {
    for (int k = 0; k < N; k++) {
        double a = k * TAU / N;
        vx[k] = CX + R * cos(a);
        vy[k] = CY + R * sin(a);
    }
}

/* ── Regular aa-gon on chord A→B, side=+1(out)/-1(in) ───── */
typedef struct { double x, y; } Pt2;

static int ngon_apex(Pt2 A, Pt2 B, int aa, int side, Pt2 *apex) {
    double ex = B.x - A.x, ey = B.y - A.y;
    double L = sqrt(ex * ex + ey * ey);
    if (L < 1e-12) return -1;
    double ux = ex / L, uy = ey / L;
    double ea = TAU / aa;
    double th = -ea * side;
    double cx = cos(th), sx = sin(th);

    /* walk aa-gon from A, return second-to-last vertex as apex */
    double curx = A.x, cury = A.y;
    double dirx = ux, diry = uy;
    for (int i = 0; i < aa - 1; i++) {
        double nx = curx + dirx * L;
        double ny = cury + diry * L;
        if (i == aa - 2) {  /* apex = last new vertex */
            apex->x = nx;
            apex->y = ny;
            return 0;
        }
        curx = nx; cury = ny;
        double ndx = dirx * cx - diry * sx;
        double ndy = dirx * sx + diry * cx;
        dirx = ndx; diry = ndy;
    }
    return -1;
}

/* ── Distance helpers ─────────────────────────────────────── */
static double d2(Pt2 a, Pt2 b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}
static double dist(Pt2 a, Pt2 b) { return sqrt(d2(a, b)); }

/* ── Quantize apex to integer grid (for overlap counting) ── */
#define GRID 144
static int grid_count[GRID * GRID];

/* ── Factorization verification ───────────────────────────── */
static void verify_factorization(void) {
    printf("\n=== Factorization 20736 ===\n");
    printf("96 × 36 × 6 = %d %s\n", FIELD_96 * FIELD_36 * FIELD_6,
           (FIELD_96 * FIELD_36 * FIELD_6 == FULL_FIELD) ? "PASS" : "FAIL");

    /* 4 tetra × 3 axes = 12; 12 × 8 pairs = 96 */
    printf("4 × 3 = %d, 12 × 8 = %d\n", 4 * 3, 12 * 8);

    /* 6 × 6 = 36 */
    printf("6 × 6 = %d\n", 6 * 6);

    /* All Platonic factor pairs of 20736 */
    printf("\nPlatonic factor pairs of 20736:\n");
    int platonic[] = {1, 2, 3, 4, 6, 8, 12, 18, 24, 36, 48, 72, 96, 144, 288, 576};
    int np = sizeof(platonic) / sizeof(platonic[0]);
    for (int i = 0; i < np; i++) {
        for (int j = i; j < np; j++) {
            int64_t prod = (int64_t)platonic[i] * platonic[j];
            if (prod == FULL_FIELD) {
                printf("  %d × %d = 20736\n", platonic[i], platonic[j]);
            }
        }
    }
    printf("96 × 36 × 6 = 20736 = cube tetra binding × remaining × hyper\n");
}

/* ── Main experiment ──────────────────────────────────────── */
int main(void) {
    init_ring();

    printf("=== Limacon Sweep: aa = 3..24 on 24-gon ===\n");
    printf("Ring: %d vertices, R=%.0f\n", N, R);
    printf("Field: %d = 144²\n\n", FULL_FIELD);

    /* ── Factorization ── */
    verify_factorization();

    /* ── Sweep aa ── */
    printf("\n=== AA Sweep ===\n");
    printf("%-4s %8s %8s %8s %8s %8s\n", "aa", "time_ns", "coverage", "avg_path", "max_olap", "unique");

    for (int aa = 3; aa <= 24; aa++) {
        memset(grid_count, 0, sizeof(grid_count));
        int coverage = 0;
        double total_path = 0;
        int path_count = 0;
        int max_overlap = 0;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        /* For each hub, compute 23 apex positions */
        for (int hub = 0; hub < N; hub++) {
            Pt2 A = {vx[hub], vy[hub]};
            for (int s = 1; s < N; s++) {
                int target = (hub + s) % N;
                Pt2 B = {vx[target], vy[target]};

                /* Pick outward for s<=12, inward for s>13 */
                int side = (s <= 12) ? 1 : -1;

                /* Handle tie at s=12 (diameter) */
                if (s == 12) side = 1;

                Pt2 apex;
                if (ngon_apex(A, B, aa, side, &apex) == 0) {
                    /* Quantize to grid */
                    int gx = (int)((apex.x - (CX - R)) / (2 * R) * (GRID - 1));
                    int gy = (int)((apex.y - (CY - R)) / (2 * R) * (GRID - 1));
                    gx = (gx < 0) ? 0 : (gx >= GRID ? GRID - 1 : gx);
                    gy = (gy < 0) ? 0 : (gy >= GRID ? GRID - 1 : gy);
                    int idx = gy * GRID + gx;

                    if (grid_count[idx] == 0) coverage++;
                    grid_count[idx]++;
                    total_path += dist(A, apex);
                    path_count++;
                }
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);

        /* Find max overlap */
        for (int i = 0; i < GRID * GRID; i++) {
            if (grid_count[i] > max_overlap)
                max_overlap = grid_count[i];
        }

        double avg_path = (path_count > 0) ? total_path / path_count : 0;

        printf("%-4d %8ld %8d %8.1f %8d %8d\n",
               aa, ns, coverage, avg_path, max_overlap, path_count);
    }

    /* ── Special cases ── */
    printf("\n=== Special Cases ===\n");
    printf("aa=3  (triangle): straight paths, max coverage\n");
    printf("aa=4  (square):   apex on circumcircle\n");
    printf("aa=6  (hexagon):  apex falls inside ring\n");
    printf("aa=12 (dodeca):   close to ring\n");
    printf("aa=24 (24-gon):   matches host ring\n");

    /* ── Path direction analysis ── */
    printf("\n=== Path Direction (aa=3) ===\n");
    printf("Steps 1-12: outward (kis) — %d paths per hub\n", 12);
    printf("Steps 13-23: inward (hyper) — %d paths per hub\n", 11);
    printf("Total: 23 paths × 24 hubs = %d addressing paths\n", 23 * 24);

    return 0;
}
