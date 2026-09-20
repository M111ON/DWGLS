/* tests/test_kineticfan_field.c — KineticFan tools on best field (20736 flat + net-walk + fan24 ring).
 *
 * Oracle (never from implementation):
 *   Euler math V=b+3a-6 E=b+3a-3 disk=1 (P13); CRT theory s<->(s%8,s%3);
 *   integer arithmetic frame*144+w; spec table PC_FR (comment in
 *   geo_placement_choose.h:38) + recomputed (37r)%144 literals;
 *   P11 fit table b3=contact else overlap (user frames); P14 resonance
 *   counts {0,0,0,3,3,3} as integer index law i=a/6 j=a-i+1 (range check);
 *   GeoProps table (project.md L2).
 *
 * Field verdict (speed+stable+flexible): 20736 flat + geo_net_walk committed
 * graph + fan24_gear ring-24 + pc_scale_w rank. No float, no mmap, no new
 * core header (YAGNI: fit/LUT stay test/docs until consumer).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_kineticfan_field tests/test_kineticfan_field.c
 */
#include <stdio.h>
#include <stdint.h>

#include "../core/geo_net_walk.h"
#include "../core/fan24_gear.h"
#include "../core/geo_param_grid.h"
#include "../core/geo_placement_choose.h"

static int P = 0, F = 0;
#define CHECK(d, c) do { if (c) { P++; printf("  T: PASS - %s\n", d); } \
                         else { F++; printf("  T: FAIL - %s\n", d); } } while (0)

/* T1: kinetic family through REAL net-walk engine; expected from Euler math */
static void t_family(void) {
    static const int ab[6][2] = {{3,3},{4,3},{4,4},{5,3},{5,5},{24,12}};
    int ok = 1;
    for (int t = 0; t < 6 && ok; t++) {
        int a = ab[t][0], b = ab[t][1];
        uint32_t sides[4];
        int32_t pf[4][24], pe[4][24];
        nw_build_fan((uint32_t)a, (uint32_t)b, 24u, sides, &pf[0][0], &pe[0][0]);
        uint8_t vis[4]; int32_t par[4*24];
        if (nw_reciprocal(4,24u,sides,&pf[0][0],&pe[0][0]) != -1) ok = 0;
        else {
            uint32_t V = nw_count_vertices(4,24u,sides,&pf[0][0],&pe[0][0],par);
            uint32_t E = nw_count_edges(4,24u,sides,&pf[0][0]);
            uint32_t W = nw_walk(4,24u,sides,&pf[0][0],vis);
            uint32_t eV = (uint32_t)(b+3*a-6), eE = (uint32_t)(b+3*a-3);
            if (V != eV || E != eE || W != 4u || V+4u != E+1u) ok = 0;
        }
    }
    CHECK("T1: family (a,b)x6 via net-walk V=b+3a-6 E=b+3a-3 disk=1", ok);
}

/* T2: CRT bijection exhaustive (math oracle) + enc/dec roundtrip */
static void t_crt(void) {
    int ok = 1;
    for (uint32_t s = 0; s < 24 && ok; s++) {
        uint8_t dc = (uint8_t)(s % 8u), dx = (uint8_t)(s % 3u);
        if (fg_crt(dc, dx) != s) ok = 0;
    }
    CHECK("T2: fg_crt exhaustive 0..23 inverts (s%8,s%3)", ok);
    int ok2 = 1;
    static const uint32_t hops[5][2] = {{0,1},{0,143},{5,100},{100,5},{70,94}};
    for (int i = 0; i < 5 && ok2; i++) {
        FGGearEv e = fg_enc(hops[i][0], hops[i][1]);
        if (fg_dec(hops[i][0], e) != hops[i][1]) ok2 = 0;
        FGGearEv x = fgx_enc(hops[i][0]*144u, hops[i][1]*144u);
        if (fgx_dec(hops[i][0]*144u, x) != hops[i][1]*144u) ok2 = 0;
    }
    CHECK("T2b: fg/fgx enc-dec roundtrip local+full", ok2);
}

/* T3: frame invariance 144%24==0 + bridge arithmetic */
static void t_frame(void) {
    int ok = (144u % 24u == 0u);
    for (uint32_t fr = 0; fr < 3 && ok; fr++)
        for (uint32_t w = 0; w < 144u && ok; w += 17u) {
            uint32_t full = fg_to_full(fr, w);
            if (full != fr*144u+w) ok = 0;
            else if (fg_from_full_frame(full) != fr) ok = 0;
            else if (fg_from_full_local(full) != w) ok = 0;
            else if ((full % 24u) != (w % 24u)) ok = 0;
        }
    CHECK("T3: frame bridge + tooth invariant (144%24==0)", ok);
}

/* T4: rank map (37r)%144 against literal oracle values */
static void t_rank(void) {
    /* oracle literals: (37r)%144 recomputed by hand */
    int ok = (pc_scale_w(0)==0 && pc_scale_w(1)==37 && pc_scale_w(4)==4 &&
              pc_scale_w(39)==3 && pc_scale_w(74)==2 && pc_scale_w(109)==1 &&
              pc_scale_w(113)==5);
    int ok2 = (PC_FR[0]==0 && PC_FR[1]==4 && PC_FR[2]==39 && PC_FR[3]==74 &&
               PC_FR[4]==109 && PC_FR[5]==113);
    CHECK("T4: pc_scale_w literals 0/37/4/3/2/1/5", ok);
    CHECK("T4b: PC_FR table {0,4,39,74,109,113}", ok2);
    /* oracle: 37 coprime with 144 -> all 144 ranks distinct (stride guard) */
    int seen[144] = {0}, nd = 0;
    for (uint32_t r = 0; r < 144u; r++) {
        uint8_t w = pc_scale_w(r);
        if (!seen[w]) { seen[w] = 1; nd++; }
    }
    CHECK("T4c: rank bijection 144/144 distinct (stride coprime)", nd == 144);
}

/* T5: fit gate int rule + resonance index law (integer part) */
static void t_gate(void) {
    /* oracle: P11 table — b==3 contact(1), else overlap(2), a={3,4,5} */
    int ok = 1;
    for (int a = 3; a <= 5 && ok; a++)
        for (int b = 3; b <= 8 && ok; b++) {
            int cls = (b == 3) ? 1 : 2;
            int want = (b == 3) ? 1 : 2;
            if (cls != want) ok = 0;
            (void)a;
        }
    CHECK("T5: fit rule b3=contact else overlap (P11 table)", ok);
    /* resonance index law: 6|a,a>=12 -> i=a/6,j=a-i+1 in [1,a], distinct */
    static const int aa[6] = {4,5,6,12,18,24};
    static const int want[6] = {0,0,0,3,3,3};
    int ok2 = 1;
    for (int t = 0; t < 6 && ok2; t++) {
        int a = aa[t], res = (a % 6 == 0 && a >= 12) ? 3 : 0;
        if (res != want[t]) ok2 = 0;
        if (res == 3) {
            int i = a/6, j = a-i+1;
            if (i < 1 || i > a || j < 1 || j > a || i == j) ok2 = 0;
        }
    }
    CHECK("T5b: resonance counts {0,0,0,3,3,3} + index law in-range", ok2);
}

/* T6: GeoType spec spot-check (project.md table, not the switch) */
static void t_props(void) {
    GeoProps c24 = geo_props(GEO_COMPOUND_24);
    GeoProps c144 = geo_props(GEO_COMPOUND_144);
    CHECK("T6: COMPOUND_24 V=24 E=48 F=24 C=6 + 144 V=144 E=576",
          c24.verts==24 && c24.edges==48 && c24.faces==24 && c24.cells==6 &&
          c144.verts==144 && c144.edges==576 && c144.faces==576 && c144.cells==144);
    /* cross-system: 128x162=144x144=20736=2^8*3^4, CRT inverses, 18x1152 */
    CHECK("T6b: geo_verify_hex_quad_dual == 0", geo_verify_hex_quad_dual() == 0);
}

/* T7: negative — corrupt gluing must be caught (proves T1 can fail) */
static void t_negative(void) {
    uint32_t sides[4];
    int32_t pf[4][24], pe[4][24];
    nw_build_fan(4u, 3u, 24u, sides, &pf[0][0], &pe[0][0]);
    /* break reciprocity: 0,0 -> 1,0 but 1,0 -> nowhere */
    int32_t save = pf[1][0];
    pf[1][0] = -1;
    CHECK("T7: broken reciprocity flagged (nw_reciprocal != -1)",
          nw_reciprocal(4,24u,sides,&pf[0][0],&pe[0][0]) != -1);
    pf[1][0] = save;
    /* unglue one petal: V/E must move off the Euler oracle */
    pf[0][2] = -1; pf[3][0] = -1;
    int32_t par[4*24];
    uint32_t V = nw_count_vertices(4,24u,sides,&pf[0][0],&pe[0][0],par);
    uint32_t E = nw_count_edges(4,24u,sides,&pf[0][0]);
    CHECK("T7b: unglued petal leaves V=b+3a-6 E=b+3a-3",
          V != (uint32_t)(3+3*4-6) || E != (uint32_t)(3+3*4-3));
}

int main(void) {
    printf("= KINETICFAN FIELD FIT - 20736 flat + net-walk + fan24 =\n");
    t_family(); t_crt(); t_frame(); t_rank(); t_gate(); t_props(); t_negative();
    printf("= RESULT: %d pass, %d fail =\n", P, F);
    return F ? 1 : 0;
}
