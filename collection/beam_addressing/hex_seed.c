/*
 * hex_seed.c — 1-Centroid Tessellation (from docs/handoff)
 * ═══════════════════════════════════════════════════════════════════
 *
 * "1 state (R) = entire tessellation"
 * "Equal triangle → rotate 120° → 3 vertices → tessellate → ALL positions at k×R"
 * "1 centroid + 60° rotation = 6 outer positions = 7 centroid hexagon"
 *
 * Geometry:
 *   Seed = R = D/2  (single parameter, 2 bytes)
 *
 *   Center centroid: C0 at origin
 *   Rotate 60° × 6 → 6 outer centroids C1..C6 at distance R
 *   Each outer centroid is shared with adjacent hexagons
 *
 *   From docs/handoff-july25:
 *   "1-centroid tessellation: 60° rotation determines 6 outer positions"
 *   "Savings: 65.6% from float32 (128B → 44B per 32 weights)"
 *
 *   From docs/session-handoff-pipeline-july27:
 *   "Geo1State: 1 seed (2B) → whole tessellation"
 *   "Equal triangle → rotate 120° → 3 vertices → positions at k×R"
 *
 * Beam = weight:
 *   beam ขึ้นอยู่กับ weight เท่านั้น
 *   weight → position on hexagon grid at k×R + fine delta
 *
 * Compile: gcc -O2 hex_seed.c -o hex_seed.exe -lm
 * Run: ./hex_seed.exe
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { double x, y; } Vec2;

/* ══════════════════════════════════════════════════════════════
   1-CENTROID TESSELLATION
   ══════════════════════════════════════════════════════════════
 *
 * Seed = R (circumradius = D/2)
 * 1 centroid → 60° rotation → 6 outer centroids
 * 120° rotation → 3 vertices of equilateral triangle
 *
 *   C0 (center): (0, 0)
 *   C1..C6:      R × {cos(60°×i), sin(60°×i)}
 *
 *   Each centroid → triangle → 3 vertices → tessellate
 *   ALL positions at k×R in hexagonal lattice
 */

typedef struct {
    double R;                     /* seed = circumradius */
    Vec2 centroids[7];            /* 7 positions */
    int    n_centroids;           /* always 7 */
} HexSeed;

/* Build from seed R only — single parameter */
static HexSeed hex_seed_init(double R)
{
    HexSeed h;
    h.R = R;
    h.n_centroids = 7;

    /* Center */
    h.centroids[0] = (Vec2){0, 0};

    /* 6 outer at 60° rotations */
    for (int i = 0; i < 6; i++) {
        double angle = M_PI / 3.0 * i;
        h.centroids[i + 1] = (Vec2){
            R * cos(angle),
            R * sin(angle)
        };
    }

    return h;
}

/* ══════════════════════════════════════════════════════════════
   CENTROID INDEXING BY SECTOR
   ══════════════════════════════════════════════════════════════
 *
 *   The 6 outer centroids divide the plane into 6 sectors of 60°.
 *   Sector i (i=0..5) = angle [60°×i, 60°×(i+1)) contains C{i+1}.
 *
 *   To find which centroid a point at angle θ belongs to:
 *     sector = floor(θ / 60°)  (mod 6)
 *     centroid index = sector + 1
 *
 *   Beam ของ weight ขึ้นกับ weight:
 *     - |w|/R = distance in grid units → k×R + remainder
 *     - w mod sector = which of 6 sectors
 *     - remainder = within-sector position
 */

/* ══════════════════════════════════════════════════════════════
   WEIGHT → POSITION ON HEXAGON
   ══════════════════════════════════════════════════════════════
 *
 *   weight w → beam radius |w| on plane
 *   Direction determined by w's position in the 7-centroid space.
 *
 *   Mapping:  weight → (distance, angle)
 *     distance = |w| (in units of R)
 *     angle = determines which of 6 sectors
 *
 *   Within the hexagon:
 *     - Near center (distance < R/2): unique position
 *     - Near edge (distance ≈ R): shared between centroids
 *     - Beyond R: extends to next hexagon ring
 */

typedef struct {
    int    sector;      /* 0..5 (which 60° wedge) */
    double k;           /* integer part: how many R units */
    double delta;       /* fractional part: remainder within cell */
} HexCoord;

/* Derive hex coordinate from weight using tessellation geometry.
 * weight w determines both distance AND sector:
 *   sector  = floor(fmod(|w|, 6))       → 0..5
 *   k       = floor(|w| / (R*6))        → which ring of hexagons
 *   delta   = remainder within cell     → fine position
 */
static HexCoord weight_to_hex(double w, double R)
{
    HexCoord hc;
    double abs_w = fabs(w);

    /* Total grid units */
    double total_units = abs_w / R;

    /* One full hexagon ring = 6 units of R (6 sectors × R/sector) */
    double ring_units = 6.0;

    /* Which hexagon ring */
    hc.k = floor(total_units / ring_units);

    /* Position within current ring */
    double ring_pos = total_units - hc.k * ring_units;

    /* Sector (0..5) = which 60° wedge */
    hc.sector = (int)ring_pos;  /* 0..5 naturally */
    if (hc.sector < 0) hc.sector = 0;
    if (hc.sector > 5) hc.sector = 5;

    /* Delta = fine position within sector (0..1) */
    hc.delta = ring_pos - (double)hc.sector;

    return hc;
}

/* ══════════════════════════════════════════════════════════════
   VISUALIZATION
   ══════════════════════════════════════════════════════════════ */

static void print_hexseed(const HexSeed *h)
{
    printf("Seed = R = %.4f\n", h->R);
    printf("Centroids (7):\n");
    for (int i = 0; i < 7; i++) {
        printf("  C%d: (%.4f, %.4f)%s\n",
               i, h->centroids[i].x, h->centroids[i].y,
               i == 0 ? "  origin" : "  ← shared");
    }
    printf("\n");

    printf("Tessellation:\n");
    printf("  60° rotation → 6 outer centroids from 1 seed\n");
    printf("  120° rotation → 3 vertices of equilateral triangle\n");
    printf("  ALL positions at k×R\n");
    printf("\n");

    printf("  C0─C1 = R = %.4f\n", h->R);
    printf("  C0─C2 = R = %.4f\n", h->R);
    printf("  C1─C2 = side = R×√3 = %.4f\n", h->R * sqrt(3));
    printf("  Area  = (3√3/4)×R² = %.4f\n", 3.0 * sqrt(3) / 4.0 * h->R * h->R);
    printf("\n");
}

static void print_weights(HexSeed *h)
{
    printf("─── Beam from weight (dependent on weight only) ───\n\n");
    printf("  w:        sector k    delta\n");
    printf("  ────────  ────── ──── ──────\n");

    double test_w[] = {0, 0.5, 1.0, 1.5, 2.0,
                       3.0, 4.0, 5.0, 6.0,
                       6.5, 7.0, 12.0, -1.0, -3.0};
    for (int i = 0; i < 14; i++) {
        HexCoord hc = weight_to_hex(test_w[i], h->R);
        Vec2 pos = {hc.k * 6.0 * h->R + (double)hc.sector * h->R + hc.delta * h->R, 0};
        /* Approximate angle from sector */
        double angle = (double)hc.sector * M_PI / 3.0 + hc.delta * M_PI / 3.0;
        pos.x = (hc.k * 6.0 * h->R + (double)hc.sector * h->R + hc.delta * h->R) * cos(angle);
        pos.y = (hc.k * 6.0 * h->R + (double)hc.sector * h->R + hc.delta * h->R) * sin(angle);
        printf("  %8.2f  %5d  %4.0f  %6.4f\n",
               test_w[i], hc.sector, hc.k, hc.delta);
        (void)pos;
    }

    printf("\n  sector = which of 6 (60° wedge) — derived from weight\n");
    printf("  k      = how many full hexagon rings crossed\n");
    printf("  delta  = remainder within current sector\n");
    printf("\n  beam ขึ้นอยู่กับ weight = ไม่มี direction parameter แยก\n");
    printf("  weight -> (sector, k, delta) ล้วน derive จาก w\n");
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Hex Seed — 1-Centroid Tessellation                    ║\n");
    printf("║   \"1 state (R) = entire grid\"                           ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* Single parameter: seed R */
    double R = 2.0;  /* D = 4.0 */
    HexSeed hex = hex_seed_init(R);

    print_hexseed(&hex);

    printf("─── Reference from docs/handoff ───\n\n");
    printf("  July 25:  \"1-centroid tessellation: 60° rotation → 6 positions\"\n");
    printf("  July 27:  \"Geo1State: 1 seed (2B) → whole tessellation\"\n");
    printf("            \"ALL positions at k×R\"\n");
    printf("            \"Seed = R (fp16 = 2 bytes)\"\n");
    printf("\n");
    printf("  1 centroid + 1 share → 7 centroid hexagon\n");
    printf("  7 centroid × repeat → icosahedron\n");
    printf("\n");

    print_weights(&hex);

    printf("─── Sharing Pattern ───\n\n");
    printf("  C1..C6 แต่ละตัว share กับ hexagon ข้างบ้าน:\n");
    printf("    C1 = centroid ของ adjacent hexagon ที่หมุน 180°\n");
    printf("    C2 = centroid ของ adjacent hexagon ที่หมุน 240°\n");
    printf("    ...\n");
    printf("  เฉพาะ C0 (center) ที่ unique ต่อ hexagon\n");
    printf("\n");
    printf("  เพราะฉะนั้น 1 hexagon = 1 center + 6 shared\n");
    printf("  = 1 centroid + 1 (share เมื่อ form hexagon)\n");
    printf("\n");

    printf("─── Next Step ───\n\n");
    printf("  Weight w → (sector, k, delta) → BEAM position\n");
    printf("  delta = remainder\n");
    printf("  Encode: (sector << N) | (delta << M)\n");
    printf("  Decode: sector + k + delta → approximate w\n");
    printf("\n");
    printf("  แต่ mapping w → sector ต้อง stable สำหรับ cross-dimension\n");
    printf("  ยังรอคุณบอก mapping ที่ถูกต้อง\n");
    printf("\n");

    return 0;
}
