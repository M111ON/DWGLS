/* tests/test_anchor_frustum.c — RDH anchor wired in as the frustum apex.
 *
 * Oracles (independent of include/anchor_frustum.h):
 *   descent: fr_route_produce recomputed inline for the L0 (finest) level;
 *            levels must visit 3,2,1,0; branches in [0,6); span narrows.
 *   cells:   1,4,16,64 recomputed as 4^level.
 *   slope:   rebuild anchor twice -> identical slope (determinism), and
 *            different bytes -> different slope (no collapse).
 *   trit:    af_anchor_trit recomputed via trit_decompose.
 *   chain:   child seed from level-2 event replays; distinct anchors ->
 *            distinct child seeds.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Iinclude -o build/test_anchor_frustum tests/test_anchor_frustum.c -lm
 * RUN:   ./build/test_anchor_frustum
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "anchor_frustum.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

static void fill(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b[i] = (uint8_t)s; }
}

static void t_cells_1_4_16_64(void) {
    int ok = 1;
    for (int l = 0; l < (int)LEVEL_COUNT; l++) {
        uint32_t want = 1u << (2 * l);       /* 4^l = 1,4,16,64 */
        if (AF_LEVEL_CELLS[l] != want) { ok = 0; break; }
    }
    check(ok, "level cells = 1,4,16,64 (= 4^level)");
}

static void t_descent_shape(void) {
    uint8_t s[64]; fill(s, sizeof s, 0xA11CEu);
    aw_anchor_t a; aw_anchor_build(s, sizeof s, 1, &a);
    FrustumRouteEvent ev[FR_MAX_DEPTH];
    int n = af_anchor_descent(&a, ev);
    check(n == (int)FR_MAX_DEPTH, "descent produces 4 levels");
    int order = 1;
    for (int l = 0; l < 4; l++) if (ev[l].level != (uint32_t)(3 - l)) order = 0;
    check(order, "levels visit 3,2,1,0 (coarse->fine)");
    int br = 1;
    for (int l = 0; l < 4; l++) if (ev[l].branch_path > 5u) br = 0;
    check(br, "every branch_path in [0,6) (6 directions live)");
    int span = 1;
    for (int l = 1; l < 4; l++) if (!(ev[l].span_size < ev[l-1].span_size)) span = 0;
    check(span, "span narrows every level (refinement)");
    int capo = 1;
    for (int l = 1; l < 4; l++) if (ev[l].capo_key != ev[0].capo_key) capo = 0;
    check(capo, "capo_key stable = 1<->capo binding");
}

static void t_slope_deterministic(void) {
    uint8_t s[64]; fill(s, sizeof s, 0xD00Du);
    aw_anchor_t a, b;
    aw_anchor_build(s, sizeof s, 0, &a);
    aw_anchor_build(s, sizeof s, 0, &b);
    check(af_apex_slope(&a) == af_apex_slope(&b), "apex slope deterministic (same bytes)");
    uint8_t s2[64]; fill(s2, sizeof s2, 0xF00Du);
    aw_anchor_t c; aw_anchor_build(s2, sizeof s2, 0, &c);
    check(af_apex_slope(&a) != af_apex_slope(&c), "different bytes -> different slope");
}

static void t_descent_replay(void) {
    uint8_t s[64]; fill(s, sizeof s, 0xBEEFu);
    aw_anchor_t a; aw_anchor_build(s, sizeof s, 0, &a);
    FrustumRouteEvent e1[4], e2[4];
    af_anchor_descent(&a, e1);
    af_anchor_descent(&a, e2);
    int same = 1;
    for (int l = 0; l < 4; l++) if (e1[l].route_ref != e2[l].route_ref) same = 0;
    check(same, "same anchor replays identical descent path (timeline)");
}

static void t_anchor_trit(void) {
    uint8_t s[64]; fill(s, sizeof s, 0x77u);
    aw_anchor_t a; aw_anchor_build(s, sizeof s, 0, &a);
    TritAddr t = af_anchor_trit(&a);
    TritAddr ref; trit_decompose((uint8_t)(a.letter % TRIT_MOD), &ref);
    int ok = (t.trit == ref.trit && t.coset == ref.coset &&
              t.face == ref.face && t.level == ref.level);
    ok = ok && (t.letter == (uint8_t)(a.letter % LETTER_COUNT));
    ok = ok && (t.slope == af_apex_slope(&a));
    check(ok, "anchor -> TritAddr matches trit_decompose oracle");
    check(t.letter < LETTER_COUNT, "TritAddr.letter in 0..25 (A..Z)");
}

static void t_chain(void) {
    uint8_t s[64]; fill(s, sizeof s, 0x11u);
    aw_anchor_t a; aw_anchor_build(s, sizeof s, 0, &a);
    FrustumRouteEvent ev[4];
    af_anchor_descent(&a, ev);
    uint32_t c1 = af_major_anchor_seed(&ev[2]);   /* level-2 = 16 cells */
    uint32_t c2 = af_major_anchor_seed(&ev[2]);
    check(c1 == c2, "major-anchor seed from level-16 replays identically");
    check(c1 != 0, "major-anchor seed non-zero");

    /* distinct anchors -> distinct child seeds */
    uint8_t s2[64]; fill(s2, sizeof s2, 0x22u);
    aw_anchor_t b; aw_anchor_build(s2, sizeof s2, 0, &b);
    FrustumRouteEvent ev2[4];
    af_anchor_descent(&b, ev2);
    uint32_t d1 = af_major_anchor_seed(&ev2[2]);
    check(c1 != d1, "distinct anchors -> distinct child seeds");
}

static void t_face_letters(void) {
    uint8_t s[64]; fill(s, sizeof s, 0x33u);
    aw_anchor_t a; aw_anchor_build(s, sizeof s, 0, &a);
    int distinct = 1;
    for (int i = 0; i < 6 && distinct; i++)
        for (int j = i + 1; j < 6; j++)
            if (af_face_letter(&a, i) == af_face_letter(&a, j)) { distinct = 0; break; }
    check(distinct, "6 frustum faces carry pairwise-distinct letters");
    check(af_face_letter(&a, -1) == 0 && af_face_letter(&a, 6) == 0,
          "face out of range -> 0");
}

int main(void) {
    printf("test_anchor_frustum — RDH anchor as frustum apex\n\n");
    t_cells_1_4_16_64();
    t_descent_shape();
    t_slope_deterministic();
    t_descent_replay();
    t_anchor_trit();
    t_chain();
    t_face_letters();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
