/* test_hyper_jump.c — hyper_jump tower cycles vs hand-computed oracles.
 * Oracles derived from the jump definition (tower+1 mod N, mirror local),
 * NOT from calling the function under test in a loop.
 */
#include <stdio.h>
#include "geo_hyper_jump.h"

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { pass++; } else { fail++; printf("FAIL %s\n", name); } \
} while (0)

int main(void) {
    /* hj3: pos 0 (t0,l0) -> t1,l47 -> t2,l0 -> t0,l47. Oracle: 47. */
    uint32_t p = 0;
    p = hj3_jump(p); CHECK(p == 1u * 48u + 47u, "hj3 step1 = 95");
    p = hj3_jump(p); CHECK(p == 2u * 48u + 0u, "hj3 step2 = 96");
    p = hj3_jump(p); CHECK(p == 47u, "hj3 step3 = 47");

    /* hj3 full sweep: 3 jumps from any pos -> same tower, mirrored local. */
    for (uint32_t s = 0; s < HJ_TOTAL; s += 17u) {
        uint32_t q = hj3_jump(hj3_jump(hj3_jump(s)));
        CHECK(hj3_tower(q) == hj3_tower(s) && hj3_local(q) == 47u - hj3_local(s),
              "hj3^3 tower same local mirrored");
    }

    /* hj4: 4 jumps = identity (tower cycles 4, mirror applied even times). */
    for (uint32_t s = 0; s < HJ_TOTAL; s += 13u) {
        uint32_t q = hj4_jump(hj4_jump(hj4_jump(hj4_jump(s))));
        CHECK(q == s, "hj4^4 identity");
    }
    /* hj4 first step oracle: pos 5 (t0,l5) -> t1, l30 -> 36+30 = 66. */
    CHECK(hj4_jump(5u) == 66u, "hj4 step1 = 66");

    /* antipode involution on both spans. */
    for (uint32_t s = 0; s < 48u; s += 7u)
        CHECK(hj_antipode(hj_antipode(s, 48u), 48u) == s, "antipode48 involution");
    for (uint32_t s = 0; s < 36u; s += 5u)
        CHECK(hj_antipode(hj_antipode(s, 36u), 36u) == s, "antipode36 involution");
    CHECK(hj_antipode(0u, 48u) == 47u, "antipode48 pole pair");
    CHECK(hj_antipode(0u, 36u) == 35u, "antipode36 pole pair");

    /* tower/local split oracles. */
    CHECK(hj3_tower(95u) == 1u && hj3_local(95u) == 47u, "split95");
    CHECK(hj4_tower(66u) == 1u && hj4_local(66u) == 30u, "split66");

    /* metatron identity constants. */
    CHECK(HJ_METATRON_VIS + HJ_METATRON_RESID == HJ_METATRON_FULL, "48+16=64");
    CHECK(HJ_TES48 * HJ_TOWERS3 == HJ_TOTAL, "48x3=144");
    CHECK(HJ_TES36 * HJ_TOWERS4 == HJ_TOTAL, "36x4=144");

    printf("hyper_jump: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
