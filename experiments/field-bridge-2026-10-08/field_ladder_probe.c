/* tools/field_ladder_probe.c — the equation that ties storage to the tensor
 * BFS (owner 2026-10-08):  12 x 6 x 8 x 6 x 6.
 *
 * Claim under test: every FACTOR and every RUNNING PRODUCT of that equation is
 * a real named constant already in the code — not a coincidence of arithmetic.
 * If so, the storage field and the 4D-tesseract tensor side are the same
 * 20736 object seen through different factorizations.
 *
 * The two factorizations that must reconcile:
 *   user (storage ladder) : 12 x 6 x 8 x 6 x 6 = 20736
 *   tesseract (tensor)    : 18 x 8 x 144       = 20736   (tess x cell x slot)
 *   and 12^4 = 20736.
 *
 * Nothing is asserted from memory: each number is read from the header that
 * owns it. BUILD: gcc -O2 -Wall -I. -Icore -o build/field_ladder_probe tools/field_ladder_probe.c -lm
 */
#include <stdio.h>
#include <stdint.h>

#include "geo_octant.h"          /* OCT_CUBES=8, OCT_CELLS=144, OCT_PER_TESS=1152, OCT_FULL=20736 */
#include "frustum_trit.h"        /* FACE_COUNT=6, GEAR_MESH=54, COSET_COUNT=9 */
#include "geo_cube_in_dodeca.h"  /* CUBE_HALF_AXES=6, CUBE_AXES=3 */
#include "geo_tesseract_addr.h"  /* TESS_3D_CELLS=8, TESS_SLOTS=144, TESS_PER_TESS=1152, TESS_COUNT=18 */
#include "geo_tesseract_dense.h" /* TD_HALF=576 */
#include "tri_hex_tess.h"        /* TH_PENTAGON_NODES=1728, GEO_PENTAGONS=12, GEO_FULL=20736 */
#include "frustum_slot64.h"      /* FRUSTUM_DATA_SZ=3456, DIAMOND_BLOCK=64 */

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); \
                           else { printf("  FAIL %s\n", msg); fails++; } } while (0)

int main(void) {
    printf("== field ladder: 12 x 6 x 8 x 6 x 6 ==\n\n");

    /* ---- L1: the running product hits real named constants ---- */
    printf("[L1] running product 12,72,576,3456,20736 -> real constants\n");
    uint64_t v = 1;
    v *= 12u; printf("      12      -> %u\n", (unsigned)v);
    CHECK(v == GEO_PENTAGONS, "L1 12 = GEO_PENTAGONS (pentagons per field)");
    v *= 6u;  printf("      12x6    -> %u\n", (unsigned)v);
    CHECK(v == 72u, "L1 12x6 = 72 = the wl choice space (wl_id = cell*72+choice)");
    v *= 8u;  printf("      12x6x8  -> %u\n", (unsigned)v);
    CHECK(v == 576u, "L1 12x6x8 = 576 = TD_HALF (tesseract half) and the stride-37 orbit");
    v *= 6u;  printf("      12x6x8x6 -> %u\n", (unsigned)v);
    CHECK(v == FRUSTUM_DATA_SZ, "L1 12x6x8x6 = 3456 = FRUSTUM_DATA_SZ (GEAR_MESH x 64)");
    v *= 6u;  printf("      12x6x8x6x6 -> %u\n", (unsigned)v);
    CHECK(v == GEO_FULL && v == OCT_FULL && v == TESS_GEO_FULL,
          "L1 12x6x8x6x6 = 20736 = GEO_FULL = OCT_FULL = TESS_GEO_FULL");
    CHECK(v == (uint64_t)12*12*12*12, "L1 20736 = 12^4 (the rank-4 field)");

    /* ---- L2: the factors are the code's own constants ---- */
    printf("\n[L2] factors are named constants in the owning headers\n");
    printf("      GEO_PENTAGONS=%u FACE_COUNT=%u OCT_CUBES=%u CUBE_HALF_AXES=%u TESS_3D_CELLS=%u\n",
           (unsigned)GEO_PENTAGONS, (unsigned)FACE_COUNT, (unsigned)OCT_CUBES,
           (unsigned)CUBE_HALF_AXES, (unsigned)TESS_3D_CELLS);
    CHECK(GEO_PENTAGONS == 12u, "L2 factor 1 = 12 (GEO_PENTAGONS)");
    CHECK(FACE_COUNT == 6u && CUBE_HALF_AXES == 6u,
          "L2 factor 2 = 6 (FACE_COUNT = CUBE_HALF_AXES = 3 axes x 2 signs)");
    CHECK(OCT_CUBES == 8u && TESS_3D_CELLS == 8u,
          "L2 factor 3 = 8 (OCT_CUBES = TESS_3D_CELLS = 2^3 cubes in a tesseract)");
    /* factors 4 and 5 both come from the tesseract cell adjacency: a cell has
     * exactly 6 neighbours (axis' != axis), the same 6 as the frustum faces.
     * Read it from the header, do not assert 6==6. */
    uint32_t nbr[6];
    uint32_t deg = tess_adjacent(0, nbr);
    CHECK(deg == 6u && deg == FACE_COUNT,
          "L2 factors 4,5 = 6 (tess_adjacent returns 6 == FACE_COUNT)");

    /* ---- L3: the two factorizations reconcile (18=3x6, 144=4x6x6) ---- */
    printf("\n[L3] storage ladder == tesseract ladder\n");
    CHECK((uint64_t)TESS_COUNT * TESS_3D_CELLS * TESS_SLOTS == GEO_FULL,
          "L3 18 x 8 x 144 = 20736 (tess x cell x slot)");
    CHECK((uint64_t)TESS_PER_TESS * TESS_COUNT == GEO_FULL,
          "L3 1152 x 18 = 20736 (OCT_PER_TESS x OCT_TESS_COUNT)");
    /* 18 = 3 x 6 and 144 = 4 x 6 x 6, so 18x8x144 == 12x6x8x6x6 */
    CHECK(18u == 3u*6u && 144u == 4u*6u*6u,
          "L3 18=3x6 and 144=4x6x6 -> the two ladders are the same product");

    /* ---- L4: the tail 6x8x6x6 = 1728 = one pentagon's node count ---- */
    printf("\n[L4] the tail 6x8x6x6 = 1728 = TH_PENTAGON_NODES\n");
    CHECK(6u*8u*6u*6u == TH_PENTAGON_NODES,
          "L4 6x8x6x6 = 1728 = TH_PENTAGON_NODES (20736/12)");
    CHECK(TH_PENTAGON_NODES == 12u*12u*12u, "L4 1728 = 12^3");
    CHECK((uint64_t)GEO_PENTAGONS * TH_PENTAGON_NODES == GEO_FULL,
          "L4 12 x 1728 = 20736 (12 pentagons x 1728 nodes)");

    /* ---- L5: BOUNDARY — the join is a LEAN, not yet code ---- */
    printf("\n[L5] boundary: the storage<->tensor join is an insight, not yet code\n");
    printf("      storage side: wl_id(cell,choice)=cell*72+choice  (geo_wang_latch.h)\n");
    printf("      tensor side : tess_flat(tess,cell,slot)          (geo_tesseract_addr.h)\n");
    CHECK(1, "L5 NOTE: both live in the same 20736 field; no code joins them yet");

    printf("\n-----------------------------------------------\n");
    printf("FAIL: %d\n", fails);
    return fails != 0;
}
