/* tests/test_clim_shift.c — climate change at scale + multi-consumer agreement.
 *
 * Proves: sliding the 20736 window (capo shift) keeps payload addressing
 * consistent across a full field and across independent consumers.
 *
 * Oracles (definition, not impl): u64 arithmetic, ClimRec cksum recompute.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_clim_shift tests/test_clim_shift.c
 * RUN:   ./build/test_clim_shift
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "clim_record.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

int main(void) {
    printf("test_clim_shift\n");

    /* scale: full 20736-slot field slides and roundtrips */
    const uint64_t K = 123456789ull;   /* arbitrary large offset */
    ClimRec a, b;
    check(clim_init(&a, 7) == 0, "consumer A init");
    check(clim_init(&b, 7) == 0, "consumer B init");
    clim_slide(&a, K);
    clim_slide(&b, K);
    check(clim_verify(&a) == 0 && clim_verify(&b) == 0, "both verify after slide");
    check(a.offset == b.offset && a.offset == K, "multi-consumer offset agrees");

    /* every physical slot maps to the same logical position on both consumers */
    int all_match = 1;
    for (uint32_t s = 0; s < 20736u; s++) {
        uint64_t la = clim_apply(&a, s);
        uint64_t lb = clim_apply(&b, s);
        if (la != lb) { all_match = 0; break; }
        /* invert must recover the original slot */
        uint32_t inv = 0;
        if (clim_invert(&a, la, &inv) != 0 || inv != s) { all_match = 0; break; }
    }
    check(all_match, "all 20736 slots apply+invert identical on A and B");

    /* window moves: old start falls out, new window inverts */
    uint32_t inv = 0;
    check(clim_invert(&a, K - 1, NULL) == -1, "pre-window position rejected");
    check(clim_invert(&a, K, &inv) == 0 && inv == 0, "new window start inverts");
    check(clim_invert(&a, K + 20735, &inv) == 0 && inv == 20735, "new window end inverts");
    check(clim_invert(&a, K + 20736, NULL) == -1, "past new window rejected");

    /* successive slides accumulate (climate keeps moving) */
    uint64_t before = a.offset;
    clim_slide(&a, 1u);
    clim_slide(&b, 1u);
    check(a.offset == before + 1 && b.offset == a.offset, "successive slide keeps consumers in sync");

    /* cksum reseals on every slide */
    check(clim_verify(&a) == 0 && clim_verify(&b) == 0, "reseal valid after successive slides");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}