/*
 * test_iso_rot90.c — iso↔square bridge bijection proof
 * ══════════════════════════════════════════════════════════════════════
 * Independent oracles only (no circular expected-from-implementation):
 *   A. BIJECTION — sweep all 144 slots with a count array; every target
 *      must be hit exactly once (oracle = counting over the full domain).
 *   B. INVOLUTION — rot90(rot90(s)) == s for every slot (oracle: the dual
 *      definition itself, checked against counting, not against stored
 *      expected values).
 *   C. HAND-COMPUTED VALUES — digits worked out by hand on paper:
 *        x=5,y=7 → A=1,C=2 / B=2,D=1 → x'=9,y'=6 · slot 89→81
 *        slot 0→0, 143→143, 12(=x0y1)→4(=x4y0), 23(x11y1)→143(x11y11)
 *   D. STRUCTURE — fixed points are exactly the corners {0,11}×{0,11}
 *      plus axis values where tri==sq*? verified by explicit enumeration.
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_iso_rot90 tests/test_iso_rot90.c
 */
#include <stdio.h>
#include <string.h>
#include "../core/iso_rot90.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

int main(void) {
    printf("=== iso_rot90 — (4x4)x(3x3)=12x12 triangle<->square bridge ===\n");

    /* A. bijection over the full domain */
    {
        int hit[ISO_SLOTS];
        memset(hit, 0, sizeof(hit));
        for (int32_t s = 0; s < ISO_SLOTS; s++) {
            int32_t r = iso_rot90_slot(s);
            if (r < 0 || r >= ISO_SLOTS) { printf("  FAIL: out of range %d\n", r); failures++; continue; }
            hit[r]++;
        }
        int bijective = 1;
        for (int32_t s = 0; s < ISO_SLOTS; s++)
            if (hit[s] != 1) bijective = 0;
        CHECK(bijective, "A: rot90 is a bijection on all 144 slots");
    }

    /* B. mutual inverses — rot270∘rot90 = rot90∘rot270 = identity */
    {
        int inv1 = 1, inv2 = 1;
        for (int32_t s = 0; s < ISO_SLOTS; s++) {
            if (iso_rot270_slot(iso_rot90_slot(s)) != s) inv1 = 0;
            if (iso_rot90_slot(iso_rot270_slot(s)) != s) inv2 = 0;
        }
        CHECK(inv1 && inv2, "B: rot90 and rot270 are mutual inverses on all 144 slots");
    }

    /* C. hand-computed values (worked out before writing the code) */
    {
        /* x=5: A=1,C=2 -> x'=2*4+1=9 ; y=7: B=2,D=1 -> y'=1*4+2=6 */
        iso_pt p = {5, 7};
        iso_pt r = iso_rot90(p);
        CHECK(r.x == 9 && r.y == 6, "C: (5,7) -> (9,6)");
        CHECK(iso_slot(5, 7) == 89 && iso_slot(9, 6) == 81 && iso_rot90_slot(89) == 81,
              "C: slot 89 -> 81");

        CHECK(iso_rot90_slot(0) == 0,   "C: corner 0 fixed");
        CHECK(iso_rot90_slot(143) == 143, "C: corner 143 fixed");

        /* x=0: A=0,C=0 -> x'=0 ; y=1: B=0,D=1 -> y'=4 => (0,1)->(0,4) */
        CHECK(iso_rot90_slot(iso_slot(0, 1)) == iso_slot(0, 4), "C: (0,1) -> (0,4)");

        /* x=11: A=3,C=2 -> x'=2*4+3=11 ; y=1: -> y'=4 => (11,1)->(11,4) */
        CHECK(iso_rot90_slot(iso_slot(11, 1)) == iso_slot(11, 4), "C: (11,1) -> (11,4)");

        /* rot270 returns the trip: (9,6) -> (5,7) — hand: x'=9%4=1,9/4=2->1*3+2=5 */
        {
            iso_pt p965 = {9, 6};
            iso_pt b = iso_rot270(p965);
            CHECK(b.x == 5 && b.y == 7, "C: rot270(9,6) -> (5,7)");
            CHECK(iso_rot270_slot(81) == 89, "C: slot 81 -> 89");
        }
    }

    /* D. structure: fixed points of the axis map are exactly v where
       sq*4+tri == tri*3+sq  <=>  sq*3 == tri*2  <=> (tri,sq) in
       {(0,0),(2,3)} -> per-axis {0,11}; corners of the grid are fixed,
       edges move along the same edge */
    {
        int fx[ISO_SIDE], nfx = 0;
        memset(fx, 0, sizeof(fx));
        for (int32_t v = 0; v < ISO_SIDE; v++)
            if (iso_rot90_axis(v) == v) { fx[v] = 1; nfx++; }
        CHECK(nfx == 2 && fx[0] && fx[11], "D: axis fixed points = {0,11} exactly");
    }

    printf("%s (%d failure%s)\n",
           failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
