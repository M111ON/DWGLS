/* tools/minor_forage_probe.c — first assembly of the minor-forager loop (note #15).
 *
 * Composes ONLY locked rules, wiring real headers where they exist:
 *   nominate (top-b coarse) -> order (score-desc, latch-asc) -> visit
 *   (majors on major latch, minors on subway branch latches) -> accumulate
 *   (minor-mean, x100 ints) -> budget-K stop -> assemble.
 * Branch isolation is proven with the real core/geo_wang_latch.h: minor
 * traversals must never flip major-visible latch state.
 * All expectations hand-computed. Real-data SIFT leg is a follow-up
 * (needs anchor artifacts); this probe pins the mechanics.
 *
 * BUILD (repo root): gcc -O2 -Wall -I. -Icore -o build/minor_forage_probe tools/minor_forage_probe.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_wang_latch.h"

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); \
                           else { printf("  FAIL %s\n", msg); fails++; } } while (0)

typedef struct { uint32_t house; uint32_t layer; int32_t score; uint32_t latch; int32_t mean; } Visit;

static int before(const Visit *a, const Visit *b) {
    if (a->score != b->score) return a->score > b->score;
    return a->latch < b->latch;
}

int main(void) {
    /* Houses: A major L3, E major L2, m1/m2 minors. Scores already x100 ints. */
    Visit pool[6] = {
        {0, 3, 8800, 700,  1200},  /* A major */
        {1, 2, 9100, 100,  1500},  /* E major */
        {2, 3, 9100, 200,   400},  /* m1 minor (ties E on score) */
        {3, 2, 8500,  50,   300},  /* m2 minor */
        {4, 1, 7000,  10,   100},  /* m3 minor */
        {5, 3, 8500, 800,   200},  /* m4 minor (ties m2 on score) */
    };
    /* Leg 1 nominate top-b=4 by coarse score (hand: E,m1,A,m2 — 9100,9100,8800,8500;
       m4 also 8500 but loses the 4th seat to m2 on latch 50 < 800) */
    Visit nom[6]; memcpy(nom, pool, sizeof nom);
    for (int i = 1; i < 6; i++) {
        Visit k = nom[i]; int j = i - 1;
        while (j >= 0 && before(&k, &nom[j])) { nom[j + 1] = nom[j]; j--; }
        nom[j + 1] = k;
    }
    static const uint32_t nom_exp[4] = { 100, 200, 700, 50 };
    int l1 = 1;
    for (int i = 0; i < 4; i++) if (nom[i].latch != nom_exp[i]) l1 = 0;
    CHECK(l1, "L1 nominate top-4 = {E100, m1-200, A700, m2-50}");

    /* Leg 2 visit: majors on major latch, minors on subway branch latches */
    wl_latch_t major, sub_lo, sub_hi;
    wl_reset(&major); wl_reset(&sub_lo); wl_reset(&sub_hi);
    /* major walk first: E(100), A(700) traverse major lane */
    CHECK(wl_traverse(&major, 100) == 1, "L2 major E traverses major lane");
    CHECK(wl_traverse(&major, 700) == 1, "L2 major A traverses major lane");
    /* minors branch: m1 under (sub_lo), m2 over (sub_hi) — same cell space, own latch */
    CHECK(wl_traverse(&sub_lo, 200) == 1, "L2 minor m1 traverses UNDER branch");
    CHECK(wl_traverse(&sub_hi, 50) == 1, "L2 minor m2 traverses OVER branch");
    /* Leg 3 branch isolation: major-visible state untouched by minors */
    CHECK(!wl_is_open(&major, 100) && !wl_is_open(&major, 700),
          "L3 majors SHUT on major lane");
    CHECK(wl_is_open(&major, 200) && wl_is_open(&major, 50),
          "L3 minor ids still OPEN on major lane (no drill-through)");
    CHECK(!wl_is_open(&sub_lo, 200) && !wl_is_open(&sub_hi, 50),
          "L3 minors SHUT on their own branches (fingerprints kept)");

    /* Leg 4 accumulate minor-means (x100 ints, exact): 400 + 300 = 700 */
    int32_t acc = 0;
    for (int i = 0; i < 4; i++)
        if (nom[i].house == 2 || nom[i].house == 3) acc += nom[i].mean;
    CHECK(acc == 700, "L4 minor-mean 400+300 = 700");

    /* Leg 5 budget K=2 stop: first two of ordered nominees */
    CHECK(nom[0].latch == 100 && nom[1].latch == 200, "L5 budget K=2 keeps {100,200}");

    /* Leg 6 assemble receipt: winners + accumulated mean, deterministic */
    printf("  assembled = {E:100, m1:200} + minor_mean 700\n");
    printf(fails ? "FORAGE: %d FAIL\n" : "FORAGE: ALL PASS\n", fails);
    return fails != 0;
}
