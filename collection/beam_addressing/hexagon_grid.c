/*
 * hexagon_grid.c — 7-Centroid Hexagon Grid (Seed of Life)
 * ═══════════════════════════════════════════════════════════════════
 *
 * "grid ที่มี 7 centroid ใช้ 1 centroid + 1 (share when form into hexagon)"
 *
 * Geometry:
 *   - 1 center centroid (unique)
 *   - 6 outer centroids at 60° (shared with adjacent hexagons)
 *   - 7 centroids total per hexagon
 *   - Each centroid = center of equilateral triangle cell
 *     (circumscribed circle diameter = D)
 *
 * Vertex sharing:
 *   - Each hexagon shares outer centroids with neighbors
 *   - The 6 outer centroids → 6 shared edges → 7 total per cell
 *   - "1 centroid + 1" = 1 triangle centroid + shared vertices
 *
 * All from single parameter D = circumcircle diameter.
 *
 * Compile: gcc -O2 hexagon_grid.c -o hexagon_grid.exe -lm
 * Run: ./hexagon_grid.exe
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════════════════════════════════════════════════════
   GEOMETRY CONSTANTS
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    double x, y;
} Vec2;

/* ══════════════════════════════════════════════════════════════
   7 CENTROIDS OF SEED OF LIFE HEXAGON
   ══════════════════════════════════════════════════════════════
 *
 *   Seed of Life: 7 circles, radius = R = D/2
 *   Centroids (circle centers):
 *
 *         C3 (120°)
 *          │
 *     C4 — C0 — C2 (60°)
 *          │
 *         C5 (240°)
 *
 *   C0: center (0, 0)
 *   C1: R at 0°    → (R, 0)
 *   C2: R at 60°   → (R/2, R√3/2)
 *   C3: R at 120°  → (-R/2, R√3/2)
 *   C4: R at 180°  → (-R, 0)
 *   C5: R at 240°  → (-R/2, -R√3/2)
 *   C6: R at 300°  → (R/2, -R√3/2)
 *
 *   7 centroids → form 6 equilateral triangles around center
 *
 *   Sharing: C1..C6 are each shared with 1 adjacent hexagon
 *   When tiling: each outer centroid belongs to 2 hexagons
 *   Center centroid C0 is unique to this hexagon.
 */

#define N_CENTROIDS 7

typedef struct {
    Vec2 centroids[N_CENTROIDS];  /* 7 centroids */
    double diameter;              /* circumcircle diameter D */
    double radius;                /* R = D/2 */
} HexagonGrid;

static HexagonGrid hexagon_init(double diameter)
{
    HexagonGrid h;
    h.diameter = diameter;
    h.radius   = diameter / 2.0;
    double R   = h.radius;

    /* Center */
    h.centroids[0] = (Vec2){0, 0};

    /* 6 outer at 60° intervals (Seed of Life) */
    for (int i = 0; i < 6; i++) {
        double angle = M_PI / 3.0 * i;  /* 0°, 60°, 120°, 180°, 240°, 300° */
        h.centroids[i + 1] = (Vec2){
            R * cos(angle),
            R * sin(angle)
        };
    }

    return h;
}

/* ══════════════════════════════════════════════════════════════
   TRIANGLE CELLS IN THE HEXAGON
   ══════════════════════════════════════════════════════════════
 *
 *   6 equilateral triangles, each sharing the center centroid.
 *
 *   Triangle i: (C0, Ci, C(i+1)) for i = 1..6
 *   (with C7 = C1 for wrap-around)
 *
 *   Each triangle: circumscribed circle diameter = D
 *                  circumcenter = centroid of triangle
 */

#define N_TRIANGLES 6

/* Get triangle vertices for triangle i (0..5)
 * Triangle i uses: C0 (center), Ci, C{i+1}
 * with wrap: i=5 uses C0, C6, C1
 */
static void hexagon_triangle(const HexagonGrid *h, int tri_idx,
                              Vec2 *v0, Vec2 *v1, Vec2 *v2)
{
    if (tri_idx < 0 || tri_idx >= 6) return;
    int i1 = tri_idx + 1;              /* Ci: 1..6 */
    int i2 = ((tri_idx + 1) % 6) + 1;  /* C{i+1}: 2..6,1 (wrap) */
    *v0 = h->centroids[0];             /* C0 = center */
    *v1 = h->centroids[i1];
    *v2 = h->centroids[i2];
}

/* ══════════════════════════════════════════════════════════════
   BEAM ON HEXAGON — weight as beam radius
   ══════════════════════════════════════════════════════════════
 *
 *   Weight w = beam radius from centroid in a direction.
 *
 *   The beam "drifts" the centroid by w along a direction:
 *     - Direction could be along one of the 6 triangle edges
 *     - Or perpendicular to the triangle face (in icosahedron)
 *
 *   The delta = displacement of centroid from its rest position
 *   Stored as: (hexagon_id, triangle_id, within_cell_position)
 *
 *   For a 7-centroid hexagon:
 *     - Each centroid can drift within its triangle cell
 *     - Drift direction is along one of the 3 edges
 *     - Or combine: beam is a VECTOR not scalar
 */

/* ══════════════════════════════════════════════════════════════
   ENCODE: weight w → (centroid, direction, delta)
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    int    centroid_idx;   /* 0..6 (which centroid) */
    int    direction;      /* 0..5 (which of 6 directions) */
    double delta;          /* drift along direction (normalized by R) */
} HexBeam;

/* Encode weight using the hexagon geometry */
static HexBeam hex_encode(const HexagonGrid *h, double weight, int dir)
{
    HexBeam beam;
    beam.direction = dir % 6;

    /* Start from center centroid (C0) */
    beam.centroid_idx = 0;

    /* The beam of radius |weight| along direction dir */
    /* Direction 0 = toward C1 (0°), 1 = toward C2 (60°), etc. */
    double angle = M_PI / 3.0 * beam.direction;

    /* The beam extends from center in this direction */
    /* Total travel = |weight| / R  (how many radii) */
    double travel = fabs(weight) / h->radius;

    /* Which triangle does the beam end in? */
    int triangle = beam.direction;  /* triangle i = direction i */

    /* Within-triangle fraction (0..1 where 1 = triangle far edge) */
    /* At travel=0: at center. travel=1: at outer centroid */
    if (travel <= 1.0) {
        /* Within first triangle */
        beam.centroid_idx = 0;  /* still in center triangle */
        beam.delta = travel;    /* 0..1 within cell */
    } else {
        /* Beyond one radius — drifts to outer centroid */
        /* For now: saturate */
        beam.centroid_idx = triangle + 1;  /* outer centroid */
        beam.delta = travel - 1.0;         /* leftover drift */
    }

    return beam;
}

/* ══════════════════════════════════════════════════════════════
   PRINT the hexagon state
   ══════════════════════════════════════════════════════════════ */

static void print_hexagon(const HexagonGrid *h)
{
    printf("Hexagon: D=%.4f, R=%.4f\n", h->diameter, h->radius);
    printf("Centroids (7):\n");
    for (int i = 0; i < 7; i++) {
        printf("  C%d: (%.4f, %.4f)%s\n",
               i, h->centroids[i].x, h->centroids[i].y,
               i == 0 ? "  ← center" : "  ← shared");
    }

    printf("\nTriangles (6):\n");
    for (int t = 0; t < 6; t++) {
        Vec2 v0, v1, v2;
        hexagon_triangle(h, t, &v0, &v1, &v2);
        int i1 = t + 1;
        int i2 = ((t + 1) % 6) + 1;
        printf("  T%d: C0→C%d→C%d  centroid=(%.4f, %.4f)\n",
               t, i1, i2,
               (v0.x + v1.x + v2.x) / 3.0,
               (v0.y + v1.y + v2.y) / 3.0);
    }
}

/* ══════════════════════════════════════════════════════════════
   SHOW: beam encode on hexagon
   ══════════════════════════════════════════════════════════════ */

static void demo_beam(const HexagonGrid *h, double weight, int dir)
{
    HexBeam b = hex_encode(h, weight, dir);
    double angles[] = {0, 60, 120, 180, 240, 300};

    printf("  w=%-7.4f dir=%d° | centroid=C%d | delta=%-7.4f | triangle=T%d\n",
           weight, (int)angles[dir % 6],
           b.centroid_idx, b.delta, dir % 6);
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Hexagon Grid — 7 Centroids (Seed of Life)            ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    double D = 4.0;
    HexagonGrid hex = hexagon_init(D);
    print_hexagon(&hex);

    printf("\n─── Structure ───\n");
    printf("  1 center centroid (unique per hexagon)\n");
    printf("  6 outer centroids (each shared with adjacent hexagon)\n");
    printf("  → 7 centroids total = Seed of Life pattern\n");
    printf("  → 6 equilateral triangles, each with circumcircle D=%.4f\n", D);
    printf("\n");

    printf("  Sharing pattern:\n");
    printf("    Each outer centroid C%d..C%d is shared:\n", 1, 6);
    printf("    C1 = C1_of_adjacent_hexagon_at_180°\n");
    printf("    C2 = C2_of_adjacent_hexagon_at_240°\n");
    printf("    (each outer = 1 unique center + 6 shared)\n");
    printf("\n");

    printf("  \"1 centroid + 1\":\n");
    printf("    Each triangle: 1 centroid (circumcenter)\n");
    printf("    + 3 vertices shared with adjacent triangles\n");
    printf("    + vertices form hexagon outer ring\n");
    printf("\n");

    /* Beam demo */
    printf("─── Beam (weight=radius) on Hexagon ───\n");
    printf("  Beam direction 0° = toward C1\n");
    printf("  Beam length |w| / R = travel (fraction of one cell)\n\n");

    printf("  Direction 0° (→C1):\n");
    demo_beam(&hex, 0.5, 0);
    demo_beam(&hex, 1.0, 0);
    demo_beam(&hex, 1.5, 0);
    demo_beam(&hex, 2.5, 0);

    printf("\n  All 6 directions (w=0.8):\n");
    for (int d = 0; d < 6; d++)
        demo_beam(&hex, 0.8, d);

    printf("\n─── Next step ───\n");
    printf("  Repeat hexagon → tile icosahedron faces\n");
    printf("  Beam on 3D axis → 1D angular → 2D hexagon\n");
    printf("  Weight w = radius → drift centroid → delta\n");
    printf("  Delta stored = position within hexagon grid\n");

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Hexagon ready — 7 centroids, 6 triangles             ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    return 0;
}
