/* test_hyper_seeker.c — viewport moves vs hand-derived oracles.
 * Pan = jump, home = entry reset, view = tower window. Expectations from
 * the jump definitions (tower+1, mirror), not from the seeker itself.
 */
#include <stdio.h>
#include "geo_hyper_seeker.h"

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { pass++; } else { fail++; printf("FAIL %s\n", name); } \
} while (0)

int main(void) {
    HyperSeeker s;

    /* init/home: entry pole. */
    hs_init(&s, HS_MODE48);
    CHECK(s.tower == 0 && s.local == 0, "init at entry");
    CHECK(s.gate == HJ_ROW_ENTRY, "init gate entry");
    CHECK(HJ_GATES[s.gate].first == HJ_GATE_ALL, "entry all-firsts");

    /* pan 1: tower0 -> tower1, local mirrored 47. */
    hs_pan(&s);
    CHECK(s.tower == 1 && s.local == 47u, "pan1 tower1 local47");
    CHECK(s.gate == HJ_ROW_MID_A, "pan1 gate transit");

    /* pan 2: tower1 -> tower2, local back to 0. */
    hs_pan(&s);
    CHECK(s.tower == 2 && s.local == 0, "pan2 tower2 local0");
    CHECK(s.gate == HJ_ROW_EXIT, "pan2 gate exit");
    CHECK(HJ_GATES[s.gate].opposite == HJ_GATE_HIP, "exit opposite hip");

    /* pan 3: back to tower0 mirrored; 6 pans = identity. */
    hs_pan(&s);
    CHECK(s.tower == 0 && s.local == 47u, "pan3 tower0 local47");
    hs_pan(&s); hs_pan(&s); hs_pan(&s);
    CHECK(s.tower == 0 && s.local == 0, "6 pans identity");

    /* home resets from anywhere. */
    hs_pan(&s); hs_pan(&s);
    hs_home(&s);
    CHECK(s.tower == 0 && s.local == 0 && s.gate == HJ_ROW_ENTRY, "home resets");

    /* views partition the minimap: 3 disjoint x48 = 144. */
    uint32_t seen[144] = {0};
    hs_home(&s);
    for (int i = 0; i < 3; i++) {
        uint32_t st, ct;
        hs_view(&s, &st, &ct);
        CHECK(ct == 48u && st == (uint32_t)i * 48u, "view window");
        for (uint32_t k = 0; k < ct; k++) seen[st + k]++;
        hs_pan(&s);
    }
    int cover = 1;
    for (int i = 0; i < 144; i++) if (seen[i] != 1) cover = 0;
    CHECK(cover, "3 views cover 144 exactly once");

    /* mode36: 4 towers x36, 4 pans identity. */
    hs_init(&s, HS_MODE36);
    CHECK(s.towers == 4u && s.span == 36u, "mode36 shape");
    hs_pan(&s);
    CHECK(s.tower == 1 && s.local == 35u, "36 pan1 tower1 local35");
    hs_pan(&s); hs_pan(&s); hs_pan(&s);
    CHECK(s.tower == 0 && s.local == 0, "4 pans identity");

    printf("hyper_seeker: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
