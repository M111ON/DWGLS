/* test_hj_gj_unify.c — hyper_jump 48x3 IS geo_jump JUMP_INVERT.
 * Oracle: independent re-derivation — both reduce to
 * next=(t+1)%3, mirror=47-local on 0..143. If the two implementations
 * ever diverge, this fails. hj4 (36x4) has no GJ counterpart by
 * construction (GJ block is 48).
 */
#include <stdio.h>
#include "geo_hyper_jump.h"
#define GEO_JUMP_INLINE   /* sid/geo_jump.h gates its bodies behind this */
#include "../sid/geo_jump.h"

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { pass++; } else { fail++; printf("FAIL %s\n", name); } \
} while (0)

int main(void) {
    /* T1: identity on all 144 positions (_jump_invert is the GJ primitive;
     * the geo_jump dispatcher is non-static, so call the primitive direct). */
    int same = 1;
    for (uint32_t p = 0; p < 144u; p++)
        if (hj3_jump(p) != _jump_invert(p, 0)) same = 0;
    CHECK(same, "hj3 == GJ _jump_invert(0) x144");

    /* T2: shared constants (the unification is structural, not coincidental). */
    CHECK(HJ_TES48 == GEO_BLOCK, "48 == GEO_BLOCK");
    CHECK(HJ_TOWERS3 == GEO_METATRON_FLOORS, "3 == METATRON_FLOORS");
    CHECK(HJ_TOTAL == GEO_TOWER, "144 == GEO_TOWER");

    /* T3: HJ tower index == GJ floor index (transposed view of one 144). */
    int trans = 1;
    for (uint32_t p = 0; p < 144u; p++)
        if (hj3_tower(p) != (p / GEO_BLOCK) % GEO_METATRON_FLOORS) trans = 0;
    CHECK(trans, "hj tower == gj floor");

    /* T4: hj4 genuinely new — no 36-block in GJ. */
    CHECK(HJ_TES36 != GEO_BLOCK, "36 has no GJ counterpart");

    printf("hj_gj_unify: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
