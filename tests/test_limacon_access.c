/*
 * test_limacon_access.c — Limacon addressing access patterns + coverage heatmap
 * ═════════════════════════════════════════════════════════════════════════════
 * 1. Random/sequential access timing for aa=3,6
 * 2. Kis vs Hyper apex distance distribution for aa=6
 * 3. Coverage heatmap for aa=3,4,5,6,8,12
 * 4. Best aa ranking (score = coverage_ratio / (compute_time * max_overlap))
 *
 * Build: gcc -O2 -Wall -Icore -o tests/test_limacon_access tests/test_limacon_access.c -lm
 * Run:   tests/test_limacon_access
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define N      24
#define TAU    (2.0 * M_PI)
#define FULL   20736
#define GRID   144
#define CX     900.0
#define CY     490.0
#define R      380.0
#define STEPS  23

static double vx[N], vy[N];

static void init_ring(void) {
    for (int k = 0; k < N; k++) {
        double a = k * TAU / N;
        vx[k] = CX + R * cos(a);
        vy[k] = CY + R * sin(a);
    }
}

/* ── ngon apex: walk aa-gon on chord A→B, return apex vertex ── */
static int ngon_apex(double Ax, double Ay, double Bx, double By,
                     int aa, int side, double *ox, double *oy) {
    double ex = Bx - Ax, ey = By - Ay;
    double L = sqrt(ex * ex + ey * ey);
    if (L < 1e-12) return -1;
    double ux = ex / L, uy = ey / L;
    double ea = TAU / aa;
    double th = -ea * side;
    double co = cos(th), si = sin(th);
    double curx = Ax, cury = Ay, dirx = ux, diry = uy;
    for (int i = 0; i < aa - 1; i++) {
        double nx = curx + dirx * L, ny = cury + diry * L;
        if (i == aa - 2) { *ox = nx; *oy = ny; return 0; }
        curx = nx; cury = ny;
        double ndx = dirx * co - diry * si, ndy = dirx * si + diry * co;
        dirx = ndx; diry = ndy;
    }
    return -1;
}

/* ── Quantize (x,y) to grid index ── */
static int to_grid(double x, double y) {
    int gx = (int)((x - (CX - R)) / (2 * R) * (GRID - 1));
    int gy = (int)((y - (CY - R)) / (2 * R) * (GRID - 1));
    if (gx < 0) gx = 0;
    if (gx >= GRID) gx = GRID - 1;
    if (gy < 0) gy = 0;
    if (gy >= GRID) gy = GRID - 1;
    return gy * GRID + gx;
}

/* ── Lookup table entry ── */
typedef struct { int hub, step, side; } AddrEntry;

/* ══════════════════════════════════════════════════════════════ */
/* SECTION 1: Access pattern test (aa=3 and aa=6)               */
/* ══════════════════════════════════════════════════════════════ */
static void access_test(void) {
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  SECTION 1: Access Pattern Test\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int aa_list[] = {3, 6};
    for (int ai = 0; ai < 2; ai++) {
        int aa = aa_list[ai];
        printf("\n─── aa=%d ───\n", aa);

        /* Build lookup table: flat_addr → (hub, step, side) */
        /* We have 24×23 = 552 addresses, but flat addresses span 0..20735.
           Build a sparse map: for each address we compute, record it. */
        AddrEntry lut[FULL];
        memset(lut, 0xFF, sizeof(lut));  /* -1 sentinel */
        uint8_t occupied[FULL];
        memset(occupied, 0, sizeof(occupied));

        int total_addrs = 0;
        for (int hub = 0; hub < N; hub++) {
            for (int s = 1; s <= STEPS; s++) {
                int target = (hub + s) % N;
                int side = (s <= 12) ? 1 : -1;
                if (s == 12) side = 1;

                double ox, oy;
                if (ngon_apex(vx[hub], vy[hub], vx[target], vy[target],
                              aa, side, &ox, &oy) == 0) {
                    int flat = to_grid(ox, oy);
                    occupied[flat] = 1;
                    lut[flat].hub = hub;
                    lut[flat].step = s;
                    lut[flat].side = side;
                    total_addrs++;
                }
            }
        }
        printf("  Generated %d addresses (%d occupied cells out of %d)\n",
               total_addrs, total_addrs, FULL);

        /* Random access test: 100k random flat addresses */
        srand(42);
        #define NACC 100000
        uint32_t rnd_addrs[NACC];
        for (int i = 0; i < NACC; i++)
            rnd_addrs[i] = (uint32_t)(rand()) % FULL;

        struct timespec t0, t1;
        int hits = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        volatile int sink = 0;
        for (int i = 0; i < NACC; i++) {
            int a = rnd_addrs[i];
            if (occupied[a]) {
                sink += lut[a].hub + lut[a].step;
                hits++;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long rnd_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
        printf("  Random access:  %d lookups, %d hits, %ld ns total, %.1f ns/lookup\n",
               NACC, hits, rnd_ns, (double)rnd_ns / NACC);

        /* Sequential access: walk 0..20735 */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int seq_hits = 0;
        for (int a = 0; a < FULL; a++) {
            if (occupied[a]) {
                sink += lut[a].hub + lut[a].step;
                seq_hits++;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long seq_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
        printf("  Sequential:     %d lookups, %d hits, %ld ns total, %.1f ns/addr\n",
               FULL, seq_hits, seq_ns, (double)seq_ns / FULL);
        (void)sink;
    }
}

/* ══════════════════════════════════════════════════════════════ */
/* SECTION 2: Kis vs Hyper distance distribution (aa=6)         */
/* ══════════════════════════════════════════════════════════════ */
static void kis_hyper_compare(void) {
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  SECTION 2: Kis vs Hyper Apex Distance (aa=6, hub=0)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int aa = 6;
    int hub = 0;
    double ring_cx = CX, ring_cy = CY;

    printf("\n  %-6s %-8s %-10s %-12s %s\n", "Step", "Type", "Direction", "Dist_from_R", "Band");
    printf("  %-6s %-8s %-10s %-12s %s\n", "────", "────", "─────────", "───────────", "────");

    int band_count[4] = {0}; /* 0-25%, 25-50%, 50-75%, 75-100% */

    for (int s = 1; s <= STEPS; s++) {
        int target = (hub + s) % N;
        int side = (s <= 12) ? 1 : -1;
        if (s == 12) side = 1;

        double ox, oy;
        if (ngon_apex(vx[hub], vy[hub], vx[target], vy[target],
                      aa, side, &ox, &oy) != 0)
            continue;

        double dx = ox - ring_cx, dy = oy - ring_cy;
        double dist_from_center = sqrt(dx * dx + dy * dy);
        double pct = dist_from_center / R * 100.0;

        int band;
        if      (pct <= 25.0) band = 0;
        else if (pct <= 50.0) band = 1;
        else if (pct <= 75.0) band = 2;
        else                  band = 3;
        band_count[band]++;

        const char *type = (s <= 12) ? "KIS" : "HYPER";
        printf("  %-6d %-8s %-10s %8.1f px  (%5.1f%% of R)  [%d]\n",
               s, type, (s <= 12) ? "outward" : "inward",
               dist_from_center, pct, band);
    }

    printf("\n  Distance band distribution (aa=6):\n");
    const char *labels[] = {"  0-25%% (near center)", " 25-50%% (mid-inner)",
                            " 50-75%% (mid-outer)",  "75-100%% (near ring)"};
    for (int b = 0; b < 4; b++)
        printf("    %s: %d steps\n", labels[b], band_count[b]);
}

/* ══════════════════════════════════════════════════════════════ */
/* SECTION 3: Coverage heatmap for multiple aa                   */
/* ══════════════════════════════════════════════════════════════ */
static void coverage_heatmap(void) {
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  SECTION 3: Coverage Heatmap (aa=3,4,5,6,8,12)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int aa_vals[] = {3, 4, 5, 6, 8, 12};
    int n_aa = sizeof(aa_vals) / sizeof(aa_vals[0]);

    static int grid[GRID * GRID];

    printf("\n  %-6s %8s %8s %8s %8s %8s %8s\n",
           "aa", "unique", "empty", "1-hit", "2-5", "6+", "coverage%");
    printf("  %-6s %8s %8s %8s %8s %8s %8s\n",
           "────", "──────", "─────", "─────", "────", "───", "─────────");

    for (int ai = 0; ai < n_aa; ai++) {
        int aa = aa_vals[ai];
        memset(grid, 0, sizeof(grid));
        int total_apex = 0;

        for (int hub = 0; hub < N; hub++) {
            for (int s = 1; s <= STEPS; s++) {
                int target = (hub + s) % N;
                int side = (s <= 12) ? 1 : -1;
                if (s == 12) side = 1;

                double ox, oy;
                if (ngon_apex(vx[hub], vy[hub], vx[target], vy[target],
                              aa, side, &ox, &oy) == 0) {
                    int idx = to_grid(ox, oy);
                    grid[idx]++;
                    total_apex++;
                }
            }
        }

        int n_empty = 0, n_one = 0, n_few = 0, n_many = 0, n_unique = 0;
        for (int i = 0; i < GRID * GRID; i++) {
            if (grid[i] == 0)      { n_empty++; }
            else if (grid[i] == 1) { n_one++; n_unique++; }
            else if (grid[i] <= 5) { n_few++; n_unique++; }
            else                   { n_many++; n_unique++; }
        }

        double cov_pct = (double)n_unique / FULL * 100.0;
        printf("  %-6d %8d %8d %8d %8d %8d %7.1f%%\n",
               aa, n_unique, n_empty, n_one, n_few, n_many, cov_pct);
    }
}

/* ══════════════════════════════════════════════════════════════ */
/* SECTION 4: Best aa ranking                                   */
/* ══════════════════════════════════════════════════════════════ */
static void rank_aa(void) {
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  SECTION 4: Best aa Ranking\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Score = coverage_ratio / (compute_time_us × max_overlap)\n");

    static int grid[GRID * GRID];

    typedef struct { int aa; double score; double cov; double time_us; int max_olap; } Result;
    Result results[25]; /* aa 3..24, index by aa */
    int n_results = 0;

    for (int aa = 3; aa <= 24; aa++) {
        memset(grid, 0, sizeof(grid));
        int unique = 0;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        for (int hub = 0; hub < N; hub++) {
            for (int s = 1; s <= STEPS; s++) {
                int target = (hub + s) % N;
                int side = (s <= 12) ? 1 : -1;
                if (s == 12) side = 1;

                double ox, oy;
                if (ngon_apex(vx[hub], vy[hub], vx[target], vy[target],
                              aa, side, &ox, &oy) == 0) {
                    int idx = to_grid(ox, oy);
                    if (grid[idx] == 0) unique++;
                    grid[idx]++;
                }
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);

        int max_olap = 0;
        for (int i = 0; i < GRID * GRID; i++)
            if (grid[i] > max_olap) max_olap = grid[i];

        double cov_ratio = (double)unique / FULL;
        double time_us = ns / 1000.0;
        double denom = time_us * (max_olap > 0 ? max_olap : 1);
        double score = cov_ratio / denom * 1e6; /* scale up for readability */

        results[n_results].aa = aa;
        results[n_results].score = score;
        results[n_results].cov = cov_ratio;
        results[n_results].time_us = time_us;
        results[n_results].max_olap = max_olap;
        n_results++;
    }

    /* Sort by score descending */
    for (int i = 0; i < n_results - 1; i++) {
        for (int j = i + 1; j < n_results; j++) {
            if (results[j].score > results[i].score) {
                Result tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    printf("\n  %-6s %10s %8s %10s %8s %10s\n",
           "Rank", "aa", "cov%", "time_us", "max_olap", "score");
    printf("  %-6s %10s %8s %10s %8s %10s\n",
           "────", "──", "────", "───────", "────────", "─────");
    for (int i = 0; i < n_results; i++) {
        printf("  %-6d %10d %7.1f%% %9.1f %8d %10.2f\n",
               i + 1, results[i].aa, results[i].cov * 100.0,
               results[i].time_us, results[i].max_olap, results[i].score);
    }

    printf("\n  Winner: aa=%d (score=%.2f, coverage=%.1f%%, max_overlap=%d)\n",
           results[0].aa, results[0].score, results[0].cov * 100.0, results[0].max_olap);
}

/* ══════════════════════════════════════════════════════════════ */
int main(void) {
    init_ring();

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Limacon Access Patterns — 24-gon, R=%.0f, field=%d\n", R, FULL);
    printf("═══════════════════════════════════════════════════════════\n");

    access_test();
    kis_hyper_compare();
    coverage_heatmap();
    rank_aa();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  Done.\n");
    printf("═══════════════════════════════════════════════════════════\n");
    return 0;
}
