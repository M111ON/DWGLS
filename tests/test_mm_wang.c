/* tests/test_mm_wang.c — Wang-tile gating for minor crossings.
 *
 * Oracles: colors recomputed inline from the documented formula
 * (node*5 + layer*11 + (slide%144)*7 + edge*3) % 8 — expectations below
 * hand-computed; edge directions from coordinate deltas; gate behavior
 * from the spec (color-match default, override wins, shut blocks cross).
 *
 * Hand-computed colors (node 10, layer 3, slide 100):
 *   base = 10*5 + 3*11 + (100%144)*7 = 50+33+700 = 783; 783%8 = 7
 *   N: (783+0)%8=7 · E: (783+3)%8=2 · S: (783+6)%8=5 · W: (783+9)%8=0
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_mm_wang tests/test_mm_wang.c
 * RUN:   ./build/test_mm_wang
 */
#include <stdio.h>
#include <stdint.h>

#include "mm_wang.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

int main(void) {
    printf("test_mm_wang\n");
    mmw_clear();

    /* colors: hand-computed above */
    check(mmw_color(10u, 3u, 100u, 0) == 7, "N color 7");
    check(mmw_color(10u, 3u, 100u, 1) == 2, "E color 2");
    check(mmw_color(10u, 3u, 100u, 2) == 5, "S color 5");
    check(mmw_color(10u, 3u, 100u, 3) == 0, "W color 0");
    check(mmw_color(0, 0, 0, 4) == 0xFF, "bad edge guard");
    /* determinism + slide wraps mod 144 */
    check(mmw_color(10u, 3u, 100u, 0) == mmw_color(10u, 3u, 244u, 0),
          "slide mod-144 stable");

    /* cross edge directions (coordinate oracle) */
    check(mmw_cross_edge(1, 0) == 3, "1→0 is W");
    check(mmw_cross_edge(0, 1) == 1, "0→1 is E");
    check(mmw_cross_edge(0, 3) == 2, "0→3 is S");
    check(mmw_cross_edge(3, 0) == 0, "3→0 is N");
    check(mmw_cross_edge(1, 1) == -1 && mmw_cross_edge(0, 8) == -1,
          "non-adjacent rejected");
    check(mmw_opp(0) == 2 && mmw_opp(1) == 3 && mmw_opp(2) == 0 && mmw_opp(3) == 1,
          "opposite edges");

    /* default gate: match opens, mismatch shuts.
     * A(10,3,100) W=0; need B edge with color 0: B(11,3,100):
     * base = 55+33+700 = 788, 788%8 = 4 → E:(788+3)%8 = 791%8 = 7.
     * A.W(0) vs B.E(7): shut. Self-cross A.W vs A.W: open. */
    MVNode A, B;
    mv_init(&A, 10u, 3u, 100u, 0);
    mv_init(&B, 11u, 3u, 100u, 1);
    check(mmw_default_open(&A, 3, &A, 3) == 1, "self color match opens");
    check(mmw_default_open(&A, 3, &B, 1) == 0, "0 vs 7 mismatch shuts");
    check(mmw_default_open(NULL, 0, &B, 0) == 0, "default NULL guard");

    /* override opens on demand, close re-shuts */
    check(mmw_open(&A, 3, &B, 1) == 0, "shut before override");
    mmw_override(10u, 3, 1);
    check(mmw_open(&A, 3, &B, 1) == 1, "on-demand open");
    mmw_override(10u, 3, 0);
    check(mmw_open(&A, 3, &B, 1) == 0, "re-shut works");
    mmw_clear();
    check(mmw_open(&A, 3, &B, 1) == 0, "clear restores default");

    /* gated cross end-to-end: A.1→B.0 is a valid minor cross (proven in
     * test_mm_route); gate on W/E pair above is shut by default. */
    check(mm_minor_cross(&A, 1, &B, 0) == 1, "base cross valid (sanity)");
    check(mm_minor_cross_gated(&A, 1, &B, 0) == 0, "gated cross shut by default");
    mmw_override(10u, 3, 1);
    check(mm_minor_cross_gated(&A, 1, &B, 0) == 1, "gated cross opens on demand");
    mmw_clear();
    /* pinned cells stay refused even with gates wide open */
    mmw_override(10u, 0, 1);
    mmw_override(11u, 0, 1);
    check(mm_minor_cross_gated(&A, 0, &B, 0) == 0, "pinned refused despite open gates");
    mmw_clear();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
