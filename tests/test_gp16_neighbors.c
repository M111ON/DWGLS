/*
 * test_gp16_neighbors.c — Goldberg(4,0) adjacency from integer combinatorics
 * ═══════════════════════════════════════════════════════════════════════════
 * Oracles are INDEPENDENT of the construction (icosahedron definition,
 * Goldberg degree sequence, Euler count) — a typo in the LUT or a glue
 * bug goes red instead of being frozen in.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_gp16_neighbors tests/test_gp16_neighbors.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include "gp16_neighbors.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(void) {
    printf("═ GP16 NEIGHBORS — face-walking topology ═\n");

    /* ── G1: icosa LUT is a real icosahedron (definition, not construction) ── */
    {
        uint32_t edge_use[12][12] = {{0}};
        uint32_t vert_faces[12] = {0};
        for (uint32_t f = 0; f < 20u; f++) {
            uint32_t v[3] = { GP16N_ICO_FACES[f][0], GP16N_ICO_FACES[f][1],
                              GP16N_ICO_FACES[f][2] };
            for (uint32_t e = 0; e < 3u; e++) {
                uint32_t u = v[e], w = v[(e + 1u) % 3u];
                uint32_t lo = (u < w) ? u : w, hi = (u < w) ? w : u;
                if (lo != hi) edge_use[lo][hi]++;
            }
            for (uint32_t e = 0; e < 3u; e++) vert_faces[v[e]]++;
        }
        uint32_t edges2 = 0, edges_tot = 0, v5 = 0;
        for (uint32_t a = 0; a < 12u; a++) {
            if (vert_faces[a] == 5u) v5++;
            for (uint32_t b = a + 1u; b < 12u; b++) {
                if (edge_use[a][b]) {
                    edges_tot++;
                    if (edge_use[a][b] == 2u) edges2++;
                }
            }
        }
        CHECK("G1: LUT is icosahedron (30 edges x2 faces, 12 verts x5)",
              edges_tot == 30u && edges2 == 30u && v5 == 12u);
    }

    /* ── G2: degree sequence 12x5 + 150x6 (pentagons first) ── */
    {
        uint32_t d5 = 0, d6 = 0, pent_ok = 1;
        for (uint32_t f = 0; f < 162u; f++) {
            uint32_t d = gp16n_degree(f);
            if (d == 5u) d5++;
            else if (d == 6u) d6++;
            if ((f < 12u && d != 5u) || (f >= 12u && d != 6u)) pent_ok = 0;
        }
        CHECK("G2: 12 pentagons (deg 5, faces 0..11) + 150 hexagons (deg 6)",
              d5 == 12u && d6 == 150u && pent_ok);
    }

    /* ── G3: 480 edges, symmetric ── */
    {
        uint32_t sym = 1;
        for (uint32_t f = 0; sym && f < 162u; f++)
            for (uint32_t k = 0; k < gp16n_degree(f); k++) {
                uint32_t g = gp16n_neighbor(f, k);
                if (g >= 162u || !gp16n_are_neighbors(g, f)) { sym = 0; break; }
            }
        CHECK("G3: 480 edges, adjacency symmetric",
              gp16n_edge_count() == 480u && sym);
    }

    /* ── G4: connected (BFS from 0 reaches all 162) ── */
    {
        uint8_t seen[162] = {0};
        uint32_t q[162];
        uint32_t qh = 0, qt = 0, n = 0;
        q[qt++] = 0; seen[0] = 1;
        while (qh < qt) {
            uint32_t f = q[qh++];
            n++;
            for (uint32_t k = 0; k < gp16n_degree(f); k++) {
                uint32_t g = gp16n_neighbor(f, k);
                if (!seen[g]) { seen[g] = 1; q[qt++] = g; }
            }
        }
        CHECK("G4: one component (BFS reaches 162/162)", n == 162u);
    }

    /* ── G5: no pentagon-pentagon edge (m=4 separates icosa vertices) ── */
    {
        uint32_t pp = 0;
        for (uint32_t a = 0; a < 12u; a++)
            for (uint32_t k = 0; k < gp16n_degree(a); k++)
                if (gp16n_neighbor(a, k) < 12u) pp++;
        CHECK("G5: pentagons never adjacent", pp == 0u);
    }

    /* ── G6: face-walking works (the debt's use case) ── */
    {
        uint32_t f = 0, ok = 1;
        for (int s = 0; s < 500; s++) {
            uint32_t d = gp16n_degree(f);
            if (d < 5u || d > 6u) { ok = 0; break; }
            f = gp16n_neighbor(f, (uint32_t)((s * 37u) % d));
            if (f >= 162u) { ok = 0; break; }
        }
        CHECK("G6: 500-step face walk stays on the solid", ok);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
