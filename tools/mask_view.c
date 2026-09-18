/* mask_view.c — Voronoi mask growth visualization (hex style, cf. user pattern)
 * ─────────────────────────────────────────────────────────────────────────
 * Walks a REAL MaskedPointer via vm_masked_seek_overflow (gravity bends path,
 * cell boundary wraps) with fixed deltas. Coverage = visited (cell, cube)
 * pairs: each Voronoi cell = 6 cubes (VM_CUBES_PER), one sub-hex each.
 * 4 stages (a..d) = coverage after N steps. Red perimeter = boundary edges
 * between covered and uncovered/outside cells (computed, not drawn by hand).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/mask_view tools/mask_view.c -lm
 * RUN:   ./build/mask_view  →  build/mask_view.svg
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "core/geo_voronoi_mask.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── 24-cell axial layout (ring0:1, ring1:6, ring2:12, ring3:5) ── */
static int CQ[24], CR[24];
static void layout_cells(void) {
    static const int DQ[6] = {1, 1, 0, -1, -1, 0};
    static const int DR[6] = {0, -1, -1, 0, 1, 1};
    int n = 0;
    CQ[n] = 0; CR[n] = 0; n++;                       /* ring 0 */
    for (int s = 0; s < 6; s++) {                    /* ring 1 */
        CQ[n] = DQ[s]; CR[n] = DR[s]; n++;
    }
    int q = DQ[4] * 2, r = DR[4] * 2;                /* ring 2 */
    for (int s = 0; s < 6; s++)
        for (int i = 0; i < 2; i++) {
            CQ[n] = q; CR[n] = r; n++;
            q += DQ[s]; r += DR[s];
        }
    q = DQ[4] * 3; r = DR[4] * 3;                    /* ring 3 (partial, 5) */
    for (int s = 0; s < 6 && n < 24; s++) {
        CQ[n] = q; CR[n] = r; n++;
        q += DQ[s]; r += DR[s];
    }
}
static int cell_at(int q, int r) {
    for (int i = 0; i < 24; i++)
        if (CQ[i] == q && CR[i] == r) return i;
    return -1;
}

/* pointy-top hex: center + corner/edge geometry. dir→edge map for
   dirs E,NE,NW,W,SW,SE = edges 5,0,1,2,3,4 (midpoint-angle match). */
static const int DE[6] = {5, 0, 1, 2, 3, 4};
static const int DQ6[6] = {1, 1, 0, -1, -1, 0};
static const int DR6[6] = {0, -1, -1, 0, 1, 1};
static double hex_cx(int q, int r, double s) { return s * sqrt(3.0) * (q + r / 2.0); }
static double hex_cy(int q, int r, double s) { return s * 1.5 * r; }

#define NSTEPS 60
static const int32_t DELTAS[NSTEPS] = {
    120, -40, 200, 90, -150, 60, 300, -100, 170, 45,
    -210, 130, 80, -60, 250, 110, -180, 70, 190, -90,
    140, -120, 220, 55, -75, 160, -200, 95, 135, -65,
    210, -140, 75, 185, -110, 125, -85, 240, 65, -155,
    175, -95, 105, -135, 260, 85, -165, 115, -70, 195,
    145, -125, 90, 205, -80, 155, -145, 100, 230, -80
};
static const int STAGE_AT[4] = {8, 20, 35, 60};

/* coverage[stage][cell][cube6] */
static uint8_t cov[4][24][6];

static void run_walk(void) {
    static uint8_t step_hit[NSTEPS][24][6];
    MaskedPointer p;
    p.axis = 0; p.cell_id = 0; p.local = 0; p.position = 0;
    Spotlight spot = vm_spotlight_make(0, VM_SPOTLIGHT_RADIUS);
    for (int i = 0; i < NSTEPS; i++) {
        p = vm_masked_seek_overflow(p, DELTAS[i], &spot);
        uint32_t cube = (p.local % VM_SLOTS_PER) / (VM_SLOTS_PER / 6u);
        step_hit[i][p.cell_id % VM_CELLS][cube % 6] = 1;
    }
    /* stage s = union of steps [0, STAGE_AT[s]) */
    for (int s = 0; s < 4; s++)
        for (int i = 0; i < STAGE_AT[s]; i++)
            for (int c = 0; c < 24; c++)
                for (int b = 0; b < 6; b++)
                    cov[s][c][b] |= step_hit[i][c][b];
}

int main(void) {
    layout_cells();
    run_walk();
    FILE *f = fopen("build/mask_view.svg", "w");
    if (!f) { printf("cannot open output\n"); return 1; }
    const double S = 20.0, PW = 430.0, PH = 400.0;
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\">\n",
            PW * 4, PH);
    const char *tag[4] = {"(a)", "(b)", "(c)", "(d)"};
    for (int s = 0; s < 4; s++) {
        double ox = PW * s + PW / 2, oy = PH / 2;
        /* cells + visited sub-cubes */
        for (int c = 0; c < 24; c++) {
            double cx = ox + hex_cx(CQ[c], CR[c], S);
            double cy = oy + hex_cy(CQ[c], CR[c], S);
            fprintf(f, "<polygon points=\"");
            for (int k = 0; k < 6; k++) {
                double a = (30.0 + 60.0 * k) * M_PI / 180.0;
                fprintf(f, "%.1f,%.1f ", cx + S * cos(a), cy + S * sin(a));
            }
            fprintf(f, "\" fill=\"white\" stroke=\"black\" stroke-width=\"1.5\"/>\n");
            /* 6 sub-cube dots: gray if visited */
            for (int b = 0; b < 6; b++) {
                double a = (60.0 * b) * M_PI / 180.0;
                double dx = cx + (S * 0.52) * cos(a), dy = cy + (S * 0.52) * sin(a);
                const char *fill = cov[s][c][b] ? "#999999" : "white";
                fprintf(f, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"%.1f\" fill=\"%s\" stroke=\"black\" stroke-width=\"1\"/>\n",
                        dx, dy, S * 0.30, fill);
            }
        }
        /* red perimeter: covered cell edge adjacent to uncovered/outside */
        for (int c = 0; c < 24; c++) {
            int covered = 0;
            for (int b = 0; b < 6; b++) covered |= cov[s][c][b];
            if (!covered) continue;
            double cx = ox + hex_cx(CQ[c], CR[c], S);
            double cy = oy + hex_cy(CQ[c], CR[c], S);
            for (int d = 0; d < 6; d++) {
                int nb = cell_at(CQ[c] + DQ6[d], CR[c] + DR6[d]);
                int nb_cov = 0;
                if (nb >= 0)
                    for (int b = 0; b < 6; b++) nb_cov |= cov[s][nb][b];
                if (nb_cov) continue;
                int e = DE[d];
                double a1 = (30.0 + 60.0 * e) * M_PI / 180.0;
                double a2 = (30.0 + 60.0 * ((e + 1) % 6)) * M_PI / 180.0;
                fprintf(f, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"red\" stroke-width=\"2.5\"/>\n",
                        cx + S * cos(a1), cy + S * sin(a1),
                        cx + S * cos(a2), cy + S * sin(a2));
            }
        }
        fprintf(f, "<text x=\"%.0f\" y=\"%.0f\" text-anchor=\"middle\" font-size=\"20\">%s N=%d</text>\n",
                ox, PH - 12, tag[s], STAGE_AT[s]);
    }
    fprintf(f, "</svg>\n");
    fclose(f);
    int polys = 0, reds = 0, grays = 0;
    for (int s = 0; s < 4; s++) {
        int pairs = 0, cells = 0;
        for (int c = 0; c < 24; c++) {
            int cc = 0;
            for (int b = 0; b < 6; b++) { pairs += cov[s][c][b]; cc |= cov[s][c][b]; }
            cells += cc;
        }
        printf("  stage %s (N=%d): %d pairs over %d cells\n", tag[s], STAGE_AT[s], pairs, cells);
        grays += pairs;
    }
    printf("mask_view.svg written (cells=96 polygons, visited sub-cubes total=%d)\n", grays);
    (void)polys; (void)reds;
    return 0;
}
