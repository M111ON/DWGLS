/*
 * test_wonder_cube.c — Paz Fig.3 (pivot 8) as net-walk test vector
 *
 * Proposition under test (§351): Fig.3's perspective layout (center square
 * + 4 trapezoid petals + top rectangle) is TOPOLOGICALLY the same net as
 * a T/cross layout — so it runs on pairing tables IDENTICAL to the cube
 * tables in test_net_walk.c (copied verbatim; layout differs, gluing same).
 * SameSum sets are paper constants (Paz 2022, Fig.3: pivot c=8):
 *   corner triplet (9,12,3) -> 24 = 3c
 *   associated triplet (7,4,13) -> 24 = 3c
 *   corner ring 6-tuple (1,13,7,1,12,14) -> 48 = 6c
 *   complementary 4-tuple (2,9,10,11) -> 32 = 4c
 * Oracle = the paper, never the implementation.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_wonder_cube tests/test_wonder_cube.c
 */
#include <stdio.h>
#include <stdint.h>
#include "../core/geo_net_walk.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* Fig.3 layout roles: center square = bottom, 4 petals = sides,
 * top rectangle = top. Pairing tables BYTE-IDENTICAL to test_net_walk
 * C_PF/C_PE (order T,B,F,Ba,L,R) — the proposition IS the identity:
 * perspective drawing changes nothing in the gluing. Role map: center=B,
 * petals=F,R,Ba,L in cyclic order, top rect=T. */
static const uint32_t W_SIDES[6] = { 4, 4, 4, 4, 4, 4 };
static const int32_t W_PF[24] = {
    2,5,3,4,  4,3,5,2,  1,5,0,4,
    1,4,0,5,  2,0,3,1,  1,3,0,2
};
static const int32_t W_PE[24] = {
    2,2,2,1,  3,0,0,0,  3,3,0,0,
    1,2,2,1,  3,3,1,0,  2,3,1,1
};

#define PIVOT 8u
static const uint32_t TRIP[3] = { 9u, 12u, 3u };
static const uint32_t ASSOC[3] = { 7u, 4u, 13u };
static const uint32_t RING[6] = { 1u, 13u, 7u, 1u, 12u, 14u };
static const uint32_t COMP[4] = { 2u, 9u, 10u, 11u };

static uint32_t sum_of(const uint32_t *a, uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += a[i];
    return s;
}

int main(void) {
    printf("═ WONDER CUBE Fig.3 — net + SameSum (paper oracle) ═\n");
    uint8_t vis[8];
    int32_t par[24];

    /* ── net half: perspective layout, same gluing ── */
    CHECK("C1: Fig.3 pairing reciprocal", nw_reciprocal(6, 4, W_SIDES, W_PF, W_PE) == -1);
    {
        uint32_t V = nw_count_vertices(6, 4, W_SIDES, W_PF, W_PE, par);
        uint32_t E = nw_count_edges(6, 4, W_SIDES, W_PF);
        CHECK("C2: walk=6 V=8 E=12 Euler=2 (closes like T-layout)",
              nw_walk(6, 4, W_SIDES, W_PF, vis) == 6 && V == 8u && E == 12u &&
              V - E + 6u == 2u);
    }

    /* ── SameSum half: paper constants ── */
    CHECK("S1: corner triplet = 3c", sum_of(TRIP, 3) == 3u * PIVOT);
    CHECK("S2: associated triplet = 3c", sum_of(ASSOC, 3) == 3u * PIVOT);
    CHECK("S3: corner ring 6-tuple = 6c", sum_of(RING, 6) == 6u * PIVOT);
    CHECK("S4: complementary 4-tuple = 4c", sum_of(COMP, 4) == 4u * PIVOT);
    CHECK("S5: all sets exact multiples of pivot (3,3,6,4)",
          sum_of(TRIP, 3) / PIVOT == 3u && sum_of(ASSOC, 3) / PIVOT == 3u &&
          sum_of(RING, 6) / PIVOT == 6u && sum_of(COMP, 4) / PIVOT == 4u &&
          sum_of(TRIP, 3) % PIVOT == 0u && sum_of(RING, 6) % PIVOT == 0u);

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
