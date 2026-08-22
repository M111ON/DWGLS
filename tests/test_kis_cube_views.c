/*
 * test_kis_cube_views.c — 6 face-views of one KIS cube (S₃ on 12³)
 * ══════════════════════════════════════════════════════════════════
 * Independent oracles:
 *   A. BIJECTION — each of the 6 views swept over all 1728 slots with a
 *      count array; every target hit exactly once (oracle = counting).
 *   B. INVERSES — view∘inv(view) and inv(view)∘view = identity for all
 *      1728 × 6 (oracle: the group-inverse definition, checked by sweep).
 *   C. ORDER STRUCTURE — views 1..3 are involutions (order 2), views 4..5
 *      have order 3 (v∘v∘v = id) — hand-derived from S₃ before coding.
 *   D. HAND-COMPUTED — p=(1,2,3) slot 457:
 *        swap xy → (2,1,3)=446 · swap xz → (3,2,1)=171 · swap yz → (1,3,2)=325
 *        cycle x→y→z → (2,3,1)=182 · cycle x→z→y → (3,1,2)=303
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_kis_cube_views tests/test_kis_cube_views.c
 */
#include <stdio.h>
#include <string.h>
#include "../core/kis_cube_views.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

int main(void) {
    printf("=== kis_cube_views — 6 views of one cube (%u = FS_PIPES) ===\n",
           (unsigned)KIS_CUBE);

    /* A. bijection for every view */
    {
        int ok = 1;
        static int hit[KIS_CUBE];
        for (uint32_t v = 0; v < KIS_VIEWS && ok; v++) {
            memset(hit, 0, sizeof(hit));
            for (uint32_t s = 0; s < KIS_CUBE; s++) {
                uint32_t r = kis_view6_slot(v, s);
                if (r >= KIS_CUBE || hit[r]++) ok = 0;
            }
            for (uint32_t s = 0; s < KIS_CUBE; s++)
                if (hit[s] != 1) ok = 0;
        }
        CHECK(ok, "A: all 6 views bijective on all 1728 slots");
    }

    /* B. mutual inverses */
    {
        int ok = 1;
        for (uint32_t v = 0; v < KIS_VIEWS && ok; v++)
            for (uint32_t s = 0; s < KIS_CUBE && ok; s++)
                if (kis_view6_slot(v, kis_view6_inv(v, s)) != s ||
                    kis_view6_inv(v, kis_view6_slot(v, s)) != s) ok = 0;
        CHECK(ok, "B: view∘inv = inv∘view = id for all 6 views");
    }

    /* C. order structure from S₃ */
    {
        int ord2 = 1, ord3 = 1;
        for (uint32_t s = 0; s < KIS_CUBE && (ord2 || ord3); s++) {
            for (uint32_t v = 1; v <= 3; v++)
                if (kis_view6_slot(v, kis_view6_slot(v, s)) != s) ord2 = 0;
            for (uint32_t v = 4; v <= 5; v++) {
                uint32_t a = kis_view6_slot(v, s);
                uint32_t b = kis_view6_slot(v, a);
                if (kis_view6_slot(v, b) != s) ord3 = 0;
            }
        }
        CHECK(ord2, "C: swap views (1..3) have order 2");
        CHECK(ord3, "C: cycle views (4..5) have order 3");
    }

    /* D. hand-computed values */
    {
        /* p=(1,2,3): slot = 3*144 + 2*12 + 1 = 457 */
        uint32_t s457 = kis_cube_slot(1, 2, 3);
        CHECK(s457 == 457, "D: slot(1,2,3) == 457");
        CHECK(kis_view6_slot(1, 457) == kis_cube_slot(2, 1, 3) &&
              kis_cube_slot(2, 1, 3) == 446, "D: swap xy -> 446");
        CHECK(kis_view6_slot(2, 457) == 171, "D: swap xz -> (3,2,1) = 171");
        CHECK(kis_view6_slot(3, 457) == 325, "D: swap yz -> (1,3,2) = 325");
        CHECK(kis_view6_slot(4, 457) == 182, "D: cycle x->y->z -> (2,3,1) = 182");
        CHECK(kis_view6_slot(5, 457) == 303, "D: cycle x->z->y -> (3,1,2) = 303");
    }

    printf("%s (%d failure%s)\n",
           failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
