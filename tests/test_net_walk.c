/*
 * test_net_walk.c — fold-net engine on known solids (ground truth, no eyeball)
 *
 * Tetra closed pairing valid by K4 argument (every face pair shares exactly
 * one edge); cube pairing valid by cube-graph adjacency (each face meets 4
 * others exactly once). V/E asserted against Euler-derived values — a wrong
 * table or wrong engine both go red.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_net_walk tests/test_net_walk.c
 */
#include <stdio.h>
#include <stdint.h>
#include "../core/geo_net_walk.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── tetra closed: oriented faces A=(2,3,4) B=(1,4,3) C=(1,2,4) D=(1,3,2)
 * shared edges opposite-directed: A1-B1,A2-C1,A0-D1,B0-C2,B2-D0,C0-D2 ── */
static const uint32_t T_SIDES[4] = { 3, 3, 3, 3 };
static const int32_t T_PF[12] = { 3,1,2, 2,0,3, 3,0,1, 1,0,2 };
static const int32_t T_PE[12] = { 1,1,1, 2,1,0, 2,2,0, 2,0,0 };

/* ── tetra open net (3 petals around center 0; 3 glued, 6 open) ── */
static const int32_t O_PF[12] = { 1,2,3, 0,-1,-1, 0,-1,-1, 0,-1,-1 };
static const int32_t O_PE[12] = { 0,0,0, 0,0,0, 1,0,0, 2,0,0 };

/* ── cube closed: outward-oriented faces (normals checked by cross product)
 * T=(5,6,7,8) B=(1,4,3,2) F=(1,2,6,5) Ba=(3,4,8,7) L=(1,5,8,4) R=(2,3,7,6)
 * pairs: B0-L3,B1-Ba0,B2-R0,B3-F0,T0-F2,T1-R2,T2-Ba2,T3-L1,F1-R3,F3-L0,Ba1-L2,Ba3-R1 ── */
static const uint32_t C_SIDES[6] = { 4, 4, 4, 4, 4, 4 };
static const int32_t C_PF[24] = {
    2,5,3,4,  4,3,5,2,  1,5,0,4,
    1,4,0,5,  2,0,3,1,  1,3,0,2
};
static const int32_t C_PE[24] = {
    2,2,2,1,  3,0,0,0,  3,3,0,0,
    1,2,2,1,  3,3,1,0,  2,3,1,1
};

/* ── dodeca closed: derived from outward-oriented faces (dodeca verts
 * (±1,±1,±1)+(0,±φ,±1/φ)+cyc, face normals = icosa verts, cyclic order by
 * angle around outward normal; 12 coplanar pentagons, 30 shared edges) ── */
static const uint32_t D_SIDES[12] = { 5,5,5,5,5,5,5,5,5,5,5,5 };
static const int32_t D_PF[60] = {
    6,9,2,8,4,  10,3,11,6,4,  8,0,9,7,5,  7,11,1,10,5,
    0,8,10,1,6,  3,10,8,2,7,  1,11,9,0,4,  2,9,11,3,5,
    2,5,10,4,0,  6,11,7,2,0,  4,8,5,3,1,  3,7,9,6,1
};
static const int32_t D_PE[60] = {
    3,4,1,4,0,  4,2,4,0,3,  0,2,3,0,3,  3,0,1,3,0,
    4,3,0,4,4,  4,2,1,4,4,  3,3,0,0,4,  3,2,1,0,4,
    0,2,1,1,3,  2,2,1,2,1,  2,2,1,3,0,  1,2,1,1,2
};

int main(void) {
    printf("═ NET WALK — committed graph only ═\n");
    uint8_t vis[12];
    int32_t par[60];

    /* ── tetra closed: V=4 E=6 F=4 Euler=2 ── */
    CHECK("W1: tetra gluing reciprocal", nw_reciprocal(4, 3, T_SIDES, T_PF, T_PE) == -1);
    CHECK("W2: tetra walk visits 4 (connectedness)", nw_walk(4, 3, T_SIDES, T_PF, vis) == 4);
    {
        uint32_t V = nw_count_vertices(4, 3, T_SIDES, T_PF, T_PE, par);
        uint32_t E = nw_count_edges(4, 3, T_SIDES, T_PF);
        CHECK("W3: tetra V=4 E=6 Euler=2", V == 4u && E == 6u && V - E + 4u == 2u);
    }

    /* ── tetra open net: disk Euler=1, 6 open halves ── */
    CHECK("W4: open net reciprocal", nw_reciprocal(4, 3, T_SIDES, O_PF, O_PE) == -1);
    {
        uint32_t V = nw_count_vertices(4, 3, T_SIDES, O_PF, O_PE, par);
        uint32_t E = nw_count_edges(4, 3, T_SIDES, O_PF);
        uint32_t O = nw_count_open(4, 3, T_SIDES, O_PF);
        CHECK("W5: open walk still visits 4 (solid edges connect)",
              nw_walk(4, 3, T_SIDES, O_PF, vis) == 4);
        CHECK("W6: open net V=6 E=9 Euler disk=1, 6 dashed halves",
              V == 6u && E == 9u && V - E + 4u == 1u && O == 6u);
    }

    /* ── cube closed: V=8 E=12 F=6 Euler=2 ── */
    CHECK("W7: cube gluing reciprocal", nw_reciprocal(6, 4, C_SIDES, C_PF, C_PE) == -1);
    {
        uint32_t V = nw_count_vertices(6, 4, C_SIDES, C_PF, C_PE, par);
        uint32_t E = nw_count_edges(6, 4, C_SIDES, C_PF);
        CHECK("W8: cube walk=6 V=8 E=12 Euler=2",
              nw_walk(6, 4, C_SIDES, C_PF, vis) == 6 && V == 8u && E == 12u &&
              V - E + 6u == 2u);
    }

    /* ── dodeca closed: V=20 E=30 F=12 Euler=2 (paper Fig.18 class) ── */
    CHECK("W9: dodeca gluing reciprocal", nw_reciprocal(12, 5, D_SIDES, D_PF, D_PE) == -1);
    {
        uint32_t V = nw_count_vertices(12, 5, D_SIDES, D_PF, D_PE, par);
        uint32_t E = nw_count_edges(12, 5, D_SIDES, D_PF);
        CHECK("W10: dodeca walk=12 V=20 E=30 Euler=2",
              nw_walk(12, 5, D_SIDES, D_PF, vis) == 12 && V == 20u && E == 30u &&
              V - E + 12u == 2u);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
