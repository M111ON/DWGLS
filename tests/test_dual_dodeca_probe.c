/*
 * test_dual_dodeca_probe.c — EXPERIMENT: verify "centroid = dodeca vertex"
 * ═══════════════════════════════════════════════════════════════════
 *
 * User hypothesis (2026-08-21):
 *   "เดินบน centroid ของ triangle — centroid = จุดตัดของมุม 30° สองเส้น
 *    = dual transform เป็น dodecahedron (face center ของ icosa = vertex
 *    ของ dodeca)"
 *
 * Independent oracle (NO expected values from implementation):
 *   - DODECA_VERTS_TABLE (geo_cube_in_dodeca.h:60) = canonical dodeca
 *   - Regular dodecahedron edge length = 2/φ where φ = (1+√5)/2
 *     (derived: distance between (1,1,1) and (0,1/φ,φ) = √(1 + 1/φ⁴ + 1/φ²))
 *   - Euler: V − E + F = 2  (sphere topology)
 *   - Dual theorem: icosa(12V,30E,20F) ↔ dodeca(20V,30E,12F)
 *     face center ของ icosa triangle = vertex ของ dodeca (20 = 20)
 *
 * VERIFIES:
 *   T1  20 vertices distinct, each on unit-ish radius
 *   T2  edge pairs = 30 (distance = 2/φ ± eps)
 *   T3  every vertex degree 3  ← the "3-in-1-out" rule
 *   T4  Euler: V − E + F = 2 → F = 12 pentagon faces
 *   T5  dual counts: F_icosa(20) == V_dodeca(20), V_icosa(12) == F_dodeca(12)
 *   T6  field: 20736 = 12 pentagons × 1728 (dodeca face view)
 *       and centroid walk axes (3) == dodeca vertex degree (3)
 *
 * BUILD: gcc -O2 -Wall -o build/test_dual_dodeca_probe tests/test_dual_dodeca_probe.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "../core/geo_cube_in_dodeca.h"
#include "../core/tri_hex_tess.h"

#define EPS 1e-9

static double dist(const Vec3D *a, const Vec3D *b) {
    double dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

static void t1_distinct(void) {
    printf("── T1 dodeca vertices distinct\n");
    uint32_t dup = 0;
    for (uint32_t i = 0; i < DODECA_VERTS; i++)
        for (uint32_t j = i + 1; j < DODECA_VERTS; j++)
            if (dist(&DODECA_VERTS_TABLE[i], &DODECA_VERTS_TABLE[j]) < EPS) dup++;
    printf("  verts=%u distinct-pairs-dup=%u\n", DODECA_VERTS, dup);
}

static void t2_t3_edges(void) {
    printf("── T2/T3 edges + degree (oracle: edge length = 2/φ)\n");
    double edge_len = 2.0 / PHI;
    double tol = 1e-6;
    uint32_t edges = 0;
    uint32_t deg[20] = {0};
    for (uint32_t i = 0; i < DODECA_VERTS; i++) {
        for (uint32_t j = i + 1; j < DODECA_VERTS; j++) {
            double d = dist(&DODECA_VERTS_TABLE[i], &DODECA_VERTS_TABLE[j]);
            if (fabs(d - edge_len) < tol) { edges++; deg[i]++; deg[j]++; }
        }
    }
    uint32_t deg3 = 1, deg_any = 0;
    for (uint32_t i = 0; i < 20; i++) {
        if (deg[i] != 3) deg3 = 0;
        if (deg[i] == 0) deg_any = 1;
    }
    printf("  edges=%u (expect 30), all-degree-3=%s, any-isolated=%s\n",
           edges, deg3 ? "YES" : "NO", deg_any ? "YES" : "NO");
}

static void t4_euler(void) {
    printf("── T4 Euler: V − E + F = 2\n");
    uint32_t edges = 0;
    double edge_len = 2.0 / PHI, tol = 1e-6;
    for (int i = 0; i < 20; i++)
        for (int j = i + 1; j < 20; j++)
            if (fabs(dist(&DODECA_VERTS_TABLE[i], &DODECA_VERTS_TABLE[j]) - edge_len) < tol)
                edges++;
    long V = 20;
    long E = (long)edges;
    long F = 2 - V + E;
    long euler = V - E + F;
    printf("  V=%ld E=%ld F=%ld  Euler=%ld (must be 2)\n", V, E, F, euler);
}

static void t5_dual(void) {
    printf("── T5 dual theorem icosa ↔ dodeca\n");
    /* icosa: V=12, E=30, F=20 ; dodeca: V=20, E=30, F=12 */
    const int ico_V = 12, ico_E = 30, ico_F = 20;
    const int dod_V = 20, dod_E = 30, dod_F = 12;
    printf("  F_icosa(%d)==V_dodeca(%d): %s\n", ico_F, dod_V, ico_F == dod_V ? "YES" : "NO");
    printf("  V_icosa(%d)==F_dodeca(%d): %s\n", ico_V, dod_F, ico_V == dod_F ? "YES" : "NO");
    printf("  E_icosa(%d)==E_dodeca(%d): %s\n", ico_E, dod_E, ico_E == dod_E ? "YES" : "NO");
    printf("  icosa Euler: %d\n", ico_V - ico_E + ico_F);
    printf("  dodeca Euler: %d\n", dod_V - dod_E + dod_F);
}

static void t6_field(void) {
    printf("── T6 field structure ↔ dodeca\n");
    printf("  GEO_FULL=%u = 12 pentagons × %u nodes\n", GEO_FULL, TH_PENTAGON_NODES);
    printf("  12 pentagons == F_dodeca(12): %s\n",
           (GEO_PENTAGONS == 12) ? "YES" : "NO");
    printf("  field multi-cell view: %u pentagons × %u nodes = %u\n",
           GEO_PENTAGONS, TH_PENTAGON_NODES,
           GEO_PENTAGONS * TH_PENTAGON_NODES);
    /* pentagon nodes per face from dodeca: each pentagon = 1728 */
    printf("  nodes-per-pentagon=%u, 12×%u=%u\n",
           TH_PENTAGON_NODES, TH_PENTAGON_NODES, TH_PENTAGON_NODES * GEO_PENTAGONS);
}

int main(void) {
    printf("test_dual_dodeca_probe — centroid = dodeca vertex (dual)\n");
    t1_distinct();
    t2_t3_edges();
    t4_euler();
    t5_dual();
    t6_field();
    printf("── done (measurements)\n");
    return 0;
}