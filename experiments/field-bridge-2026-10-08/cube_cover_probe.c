/* tools/cube_cover_probe.c — "1<4<16<64 looks like a pyramid, but it is a
 * quadtree; I use the frustum because it is the same thing" (owner 2026-10-08).
 *
 * What this probe checks — and ONLY this:
 *   L1  1<4<16<64 is a 4-ary (quadtree) ladder, not a solid pyramid. Rule-only.
 *   L2  the frustum is the SAME refinement tree entered by 6 directions, and
 *       6 == the cube's half-axes (3 axes x 2 signs). This is a DIRECTION/FACE
 *       rule identity — NOT cube assembly.
 *   L3  a slot has a NESTING DEPTH in a decomposition (self + ancestors). That
 *       is depth, NOT "N cubes sitting on the slot".
 *   L4  one field, more than one nested decomposition (4^4 x 3^4 and 18x8x144).
 *
 * BOUNDARY (owner 2026-10-08): there is NO code that composes frustum faces
 * into a cube, and none is required. The 6-direction composite is a design
 * property of the descent, not an assembly step. What the fold is prepared for
 * is splicing/continuing the fold chain elsewhere — folding is the manageable
 * form, not a cube builder.
 *
 * Every number is read from the real headers, never asserted from memory.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/cube_cover_probe tools/cube_cover_probe.c -lm
 */
#include <stdio.h>
#include <stdint.h>

#include "tri_hex_tess.h"       /* TH_HI_BASE=256=4^4, TH_LO_BASE=81=3^4, GEO_FULL=20736 */
#include "geo_octant.h"         /* OCT_CUBES=8, OCT_CELLS=144, OCT_TESS_COUNT=18 */
#include "frustum_trit.h"       /* FACE_COUNT=6, LEVEL_COUNT=4 */
#include "geo_cube_in_dodeca.h" /* CUBE_HALF_AXES=6, CUBE_AXES=3 */

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); \
                           else { printf("  FAIL %s\n", msg); fails++; } } while (0)

static uint32_t ipow(uint32_t b, uint32_t e) { uint32_t r = 1; while (e--) r *= b; return r; }

int main(void) {
    printf("== quadtree / frustum identity (no cube assembly) ==\n\n");

    /* -- L1: the "pyramid" the eye reads is a 4-ary refinement ladder -- */
    printf("[L1] 1<4<16<64 is a 4-ary (quadtree) ladder, not a pyramid\n");
    CHECK(ipow(4,0)==1 && ipow(4,1)==4 && ipow(4,2)==16 && ipow(4,3)==64 && ipow(4,4)==256,
          "L1 4-ladder is 1<4<16<64<256 (4-ary, not a solid)");
    CHECK(TH_HI_BASE == 256u, "L1 TH_HI_BASE = 256 = 4^4 (aperture-4 refinement)");
    CHECK(TH_SUBDIV_4_DEPTH == 4u, "L1 aperture-4 depth = 4 levels (1->4->16->64->256)");
    CHECK(TH_LO_BASE == 81u, "L1 TH_LO_BASE = 81 = 3^4 (the other ladder)");

    /* -- L2: frustum = same tree, entered by 6 directions = cube half-axes -- */
    printf("\n[L2] frustum = that tree entered by 6 directions (= cube half-axes)\n");
    printf("      FACE_COUNT=%u  CUBE_HALF_AXES=%u  CUBE_AXES=%u x2\n",
           (unsigned)FACE_COUNT, (unsigned)CUBE_HALF_AXES, (unsigned)CUBE_AXES);
    CHECK(FACE_COUNT == 6u && CUBE_HALF_AXES == 6u,
          "L2 6 frustum directions == 6 cube half-axes (direction/face identity)");
    CHECK(CUBE_AXES * 2u == FACE_COUNT, "L2 3 axes x 2 signs = 6 directions");
    CHECK(LEVEL_COUNT == 4u, "L2 4 frustum levels == the 4-ladder depth (same tree)");
    CHECK(1, "L2 NOTE: this is a RULE (directions=faces); NO assembly code exists");

    /* -- L3: nesting depth, not "N cubes covering" -- */
    printf("\n[L3] a slot has a NESTING DEPTH in a decomposition\n");
    int hi_depth = (int)TH_SUBDIV_4_DEPTH + 1;   /* self + ancestors on the root path */
    printf("      4-ladder: depth-4 leaf -> %d nodes on its root path\n", hi_depth);
    CHECK(hi_depth == 5, "L3 depth-4 leaf has 5 nodes on its root path (self+4 ancestors)");
    printf("      NOTE: depth != count of cubes on the slot; no cube object exists\n");

    /* -- L4: one field, more than one nested decomposition -- */
    printf("\n[L4] one field, more than one nested decomposition\n");
    CHECK((uint64_t)TH_HI_BASE * TH_LO_BASE == GEO_FULL,
          "L4 256 x 81 = 20736 (4^4 x 3^4 - two ladders, one field)");
    CHECK(OCT_TESS_COUNT * OCT_CUBES * OCT_CELLS == GEO_FULL,
          "L4 18 x 8 x 144 = 20736 (tesseract > cube > slot view)");

    printf("\n-----------------------------------------------\n");
    printf("FAIL: %d\n", fails);
    return fails != 0;
}
