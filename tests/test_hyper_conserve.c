/* test_hyper_conserve.c — 4D conservation: jumps move tes units between
 * towers without loss. Oracles: permutation theory (distinct images,
 * inverse roundtrip, cycle order) + shell/antipode identity from the
 * weave graph. All expectations hand-derived, not re-runs of the impl.
 */
#include <stdio.h>
#include "geo_hyper_jump.h"
#include "geo_gidpith_weave.h"

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { pass++; } else { fail++; printf("FAIL %s\n", name); } \
} while (0)

/* inverse jumps: tower-1 (mod N) + mirror (involution). */
static inline uint32_t hj3_inv(uint32_t pos) {
    uint32_t t = hj3_tower(pos);
    uint32_t prev = (t + HJ_TOWERS3 - 1u) % HJ_TOWERS3;
    return prev * HJ_TES48 + (HJ_TES48 - 1u - hj3_local(pos));
}
static inline uint32_t hj4_inv(uint32_t pos) {
    uint32_t t = hj4_tower(pos);
    uint32_t prev = (t + HJ_TOWERS4 - 1u) % HJ_TOWERS4;
    return prev * HJ_TES36 + (HJ_TES36 - 1u - hj4_local(pos));
}

int main(void) {
    /* T1: hj3 is a permutation of 144 (distinct images = no loss). */
    uint8_t seen[HJ_TOTAL] = {0};
    for (uint32_t p = 0; p < HJ_TOTAL; p++) seen[hj3_jump(p)] = 1;
    int perm = 1;
    for (uint32_t p = 0; p < HJ_TOTAL; p++) if (!seen[p]) perm = 0;
    CHECK(perm, "hj3 permutation of 144");

    /* T2: inverse roundtrip both directions, all positions. */
    int rt = 1;
    for (uint32_t p = 0; p < HJ_TOTAL; p++)
        if (hj3_inv(hj3_jump(p)) != p || hj3_jump(hj3_inv(p)) != p) rt = 0;
    CHECK(rt, "hj3 inverse roundtrip x144");

    /* T3: cycle order 6 (tower +3 = id, mirror x6 = id; 3 jumps = mirror). */
    int cyc = 1;
    for (uint32_t p = 0; p < HJ_TOTAL; p++) {
        uint32_t q = p;
        for (int i = 0; i < 6; i++) q = hj3_jump(q);
        if (q != p) cyc = 0;
    }
    CHECK(cyc, "hj3^6 = identity");

    /* T4: same three facts for the 36x4 ladder. */
    uint8_t seen4[HJ_TOTAL] = {0};
    for (uint32_t p = 0; p < HJ_TOTAL; p++) seen4[hj4_jump(p)] = 1;
    int perm4 = 1, rt4 = 1, cyc4 = 1;
    for (uint32_t p = 0; p < HJ_TOTAL; p++) {
        if (!seen4[p]) perm4 = 0;
        if (hj4_inv(hj4_jump(p)) != p) rt4 = 0;
        uint32_t q = hj4_jump(hj4_jump(hj4_jump(hj4_jump(p))));
        if (q != p) cyc4 = 0;
    }
    CHECK(perm4, "hj4 permutation of 144");
    CHECK(rt4, "hj4 inverse roundtrip x144");
    CHECK(cyc4, "hj4^4 = identity");

    /* T5: shells partition 384 (visible across shells + hidden nowhere). */
    hj_build();
    uint8_t shell[HJ_VERTS];
    uint32_t ns = hj_shells(0, shell);
    uint32_t total = 0;
    for (uint32_t v = 0; v < HJ_VERTS; v++) total += (shell[v] != 0xFF);
    CHECK(total == HJ_VERTS, "shells cover 384");

    /* T6: antipodal shell identity — shell[v] + shell[anti[v]] == diameter.
     * Nothing hides: depth from apex plus depth from exit pole is constant. */
    uint32_t diam = ns - 1u;
    int ident = 1;
    for (uint32_t v = 0; v < HJ_VERTS; v++)
        if ((uint32_t)shell[v] + (uint32_t)shell[HJ_ANTIPODE[v]] != diam) ident = 0;
    CHECK(ident, "shell[v]+shell[anti[v]] == diameter x384");
    printf("  diameter: %u\n", diam);

    printf("hyper_conserve: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
