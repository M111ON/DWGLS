/*
 * gp16_neighbors.h — Goldberg(4,0) face-adjacency table (closes DEBT in
 * geo_goldberg_frame.h: "neighbor topology ... needed for face-walking fold")
 * ═══════════════════════════════════════════════════════════════════════════
 * CONSTRUCTION (integer combinatorics, zero geometry — no vertex is ever
 * computed; only incidence is built):
 *   1. Icosahedron: 12 vertices, 20 faces (static LUT below).
 *   2. Frequency-4 subdivision per face: lattice (i,j,k), i+j+k=4 ->
 *      15 points/face (3 corners + 9 edge + 3 interior).
 *   3. Glue: corners = icosa ids; edge points canonical by distance from
 *      min(u,v) (both adjacent faces agree); face interiors are private.
 *      Total: 12 + 30*3 + 20*3 = 162 points.
 *   4. DUAL: Goldberg faces <-> subdivision points; face-adjacency <->
 *      point-adjacency along lattice edges. The 12 icosa vertices (degree
 *      5) are the pentagons; the other 150 (degree 6) are hexagons.
 *
 * NUMBERING: pentagons are faces 0..11 (matches gp16_is_pentagon by
 * construction: corners keep icosa ids). Hexagons 12..161 get meaning
 * here for the first time (construction order: 90 edge points in edge
 * first-seen order, then 60 face interiors in face order).
 *
 * API: gp16 neighbors built once into a static table (no malloc):
 *   gp16_degree(f)        -> 5 or 6
 *   gp16_neighbor(f, k)   -> k-th neighbor id (k < degree)
 *   gp16_are_neighbors(a,b)
 *   gp16_edge_count()     -> 480
 *
 * Header-only, int-only, zero dependencies.
 */
#ifndef GP16_NEIGHBORS_H
#define GP16_NEIGHBORS_H

#include <stdint.h>

#define GP16N_FACES 162u
#define GP16N_PENT  12u
#define GP16N_MAXDEG 6u

/* ── Icosahedron faces (vertex ids 0..11). Validated by test: 30 canonical
 * edges shared by exactly 2 faces each, every vertex in exactly 5 faces. */
static const uint8_t GP16N_ICO_FACES[20][3] = {
    {0,11,5}, {0,5,1}, {0,1,7}, {0,7,10}, {0,10,11},
    {1,5,9}, {5,11,4}, {11,10,2}, {10,7,6}, {7,1,8},
    {3,9,4}, {3,4,2}, {3,2,6}, {3,6,8}, {3,8,9},
    {4,9,5}, {2,4,11}, {6,2,10}, {8,6,7}, {9,8,1}
};

#define GP16N_SUBDIV 4u

/* global id classes */
#define GP16N_EDGE_BASE 12u                    /* + edge_id*3 + (t-1), t=1..3 */
#define GP16N_FACE_BASE (12u + 30u*3u)         /* + face*3 + interior_idx */

/* table storage (built once, no malloc) */
static uint8_t  gp16n_ndeg[GP16N_FACES];
static uint8_t  gp16n_nbr[GP16N_FACES][GP16N_MAXDEG];
static uint8_t  gp16n_built = 0;
static uint8_t  gp16n_edge_id[12][12];  /* canonical edge {u,v},u<v -> 0..29 */
static uint8_t  gp16n_nedges = 0;

/* lattice point (i,j,k) of face (A,B,C) -> global id */
static inline uint32_t gp16n_lattice_id(uint32_t A, uint32_t B, uint32_t C,
                                        uint32_t i, uint32_t j, uint32_t k,
                                        uint32_t face) {
    (void)B;
    /* corners */
    if (i == GP16N_SUBDIV) return A;
    if (j == GP16N_SUBDIV) return (uint32_t)GP16N_ICO_FACES[face][1];
    if (k == GP16N_SUBDIV) return C;
    /* edge points: one coord zero — canonical by distance from min(u,v) */
    if (k == 0u) {          /* edge AB */
        uint32_t lo = (A < B) ? A : B, hi = (A < B) ? B : A;
        uint32_t t = (A < B) ? j : i;   /* distance from lo (1..3) */
        uint32_t e = gp16n_edge_id[lo][hi];
        return GP16N_EDGE_BASE + (uint32_t)e * 3u + (t - 1u);
    }
    if (j == 0u) {          /* edge AC */
        uint32_t lo = (A < C) ? A : C, hi = (A < C) ? C : A;
        uint32_t t = (A < C) ? k : i;   /* distance from lo (1..3) */
        uint32_t e = gp16n_edge_id[lo][hi];
        return GP16N_EDGE_BASE + (uint32_t)e * 3u + (t - 1u);
    }
    if (i == 0u) {          /* edge BC */
        uint32_t b = GP16N_ICO_FACES[face][1];
        uint32_t lo = (b < C) ? b : C, hi = (b < C) ? C : b;
        uint32_t t = (b < C) ? k : j;   /* distance from lo */
        uint32_t e = gp16n_edge_id[lo][hi];
        return GP16N_EDGE_BASE + (uint32_t)e * 3u + (t - 1u);
    }
    /* face interior: (2,1,1)->0, (1,2,1)->1, (1,1,2)->2 */
    uint32_t idx = (i == 2u) ? 0u : ((j == 2u) ? 1u : 2u);
    return GP16N_FACE_BASE + face * 3u + idx;
}

static inline void gp16n_link(uint32_t a, uint32_t b) {
    if (a >= GP16N_FACES || b >= GP16N_FACES || a == b) return;
    for (uint32_t k = 0; k < gp16n_ndeg[a]; k++)
        if (gp16n_nbr[a][k] == (uint8_t)b) return;   /* seam dup: skip */
    if (gp16n_ndeg[a] < GP16N_MAXDEG) gp16n_nbr[a][gp16n_ndeg[a]++] = (uint8_t)b;
    for (uint32_t k = 0; k < gp16n_ndeg[b]; k++)
        if (gp16n_nbr[b][k] == (uint8_t)a) return;
    if (gp16n_ndeg[b] < GP16N_MAXDEG) gp16n_nbr[b][gp16n_ndeg[b]++] = (uint8_t)a;
}

static inline void gp16n_build(void) {
    if (gp16n_built) return;
    for (uint32_t a = 0; a < 12u; a++)
        for (uint32_t b = 0; b < 12u; b++) gp16n_edge_id[a][b] = 0xFFu;
    gp16n_nedges = 0;
    /* canonical edge ids in face first-seen order (deterministic) */
    for (uint32_t f = 0; f < 20u; f++) {
        uint32_t v[3] = { GP16N_ICO_FACES[f][0], GP16N_ICO_FACES[f][1],
                          GP16N_ICO_FACES[f][2] };
        for (uint32_t e = 0; e < 3u; e++) {
            uint32_t u = v[e], w = v[(e + 1u) % 3u];
            uint32_t lo = (u < w) ? u : w, hi = (u < w) ? w : u;
            if (gp16n_edge_id[lo][hi] == 0xFFu)
                gp16n_edge_id[lo][hi] = gp16n_nedges++;
        }
    }
    /* lattice links per face; 3 fixed directions -> each mesh edge once */
    for (uint32_t f = 0; f < 20u; f++) {
        uint32_t A = GP16N_ICO_FACES[f][0], B = GP16N_ICO_FACES[f][1],
                 C = GP16N_ICO_FACES[f][2];
        for (uint32_t i = 0; i <= GP16N_SUBDIV; i++) {
            for (uint32_t j = 0; j <= GP16N_SUBDIV - i; j++) {
                uint32_t k = GP16N_SUBDIV - i - j;
                uint32_t p = gp16n_lattice_id(A, B, C, i, j, k, f);
                if (j > 0u && i + 1u <= GP16N_SUBDIV)
                    gp16n_link(p, gp16n_lattice_id(A, B, C, i + 1u, j - 1u, k, f));
                if (k > 0u && i + 1u <= GP16N_SUBDIV)
                    gp16n_link(p, gp16n_lattice_id(A, B, C, i + 1u, j, k - 1u, f));
                if (k > 0u && j + 1u <= GP16N_SUBDIV - i)
                    gp16n_link(p, gp16n_lattice_id(A, B, C, i, j + 1u, k - 1u, f));
            }
        }
    }
    gp16n_built = 1;
}

static inline uint32_t gp16n_degree(uint32_t f) {
    gp16n_build();
    return (f < GP16N_FACES) ? gp16n_ndeg[f] : 0u;
}

static inline uint32_t gp16n_neighbor(uint32_t f, uint32_t k) {
    gp16n_build();
    if (f >= GP16N_FACES || k >= gp16n_ndeg[f]) return 0xFFFFFFFFu;
    return gp16n_nbr[f][k];
}

static inline int gp16n_are_neighbors(uint32_t a, uint32_t b) {
    gp16n_build();
    if (a >= GP16N_FACES || b >= GP16N_FACES) return 0;
    for (uint32_t k = 0; k < gp16n_ndeg[a]; k++)
        if (gp16n_nbr[a][k] == (uint8_t)b) return 1;
    return 0;
}

static inline uint32_t gp16n_edge_count(void) {
    gp16n_build();
    uint32_t ends = 0;
    for (uint32_t f = 0; f < GP16N_FACES; f++) ends += gp16n_ndeg[f];
    return ends / 2u;
}

#endif /* GP16_NEIGHBORS_H */
