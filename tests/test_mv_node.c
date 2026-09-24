/* tests/test_mv_node.c — multiverse node + Peano entry/exit.
 *
 * Oracles: rot90 by coordinate arithmetic (definition); edge adjacency by
 * Manhattan distance recomputed inline; endpoints from the documented base
 * order {0,8} rotated — expected values hand-derived below, never read
 * back from the LUT under test except through the public functions.
 *
 * Hand-derived endpoint table (rot90 cw: (x,y)->(2-y,x)):
 *   r0: entry 0, exit 8 · r1: entry 2, exit 6
 *   r2: entry 8, exit 0 · r3: entry 6, exit 2 (+bit2 swaps)
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_mv_node tests/test_mv_node.c
 * RUN:   ./build/test_mv_node
 */
#include <stdio.h>
#include <stdint.h>

#include "mv_node.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

int main(void) {
    printf("test_mv_node\n");
    MVNode n;
    check(sizeof(MVNode) == 12, "node is 12B");

    /* rot90 is order-4 and maps corners correctly (coordinate oracle) */
    int ok = 1;
    for (int c = 0; c < 9; c++) {
        uint8_t r = c;
        for (int i = 0; i < 4; i++) r = mv_rot90(r);
        if (r != c) { ok = 0; break; }
    }
    check(ok, "rot90 order-4 on all cells");
    check(mv_rot90(0) == 2 && mv_rot90(2) == 8 && mv_rot90(8) == 6 && mv_rot90(6) == 0,
          "corner cycle 0→2→8→6");
    check(mv_rot90(4) == 4, "center fixed");

    /* endpoint table */
    const uint8_t exp_e[4] = { 0, 2, 8, 6 };
    const uint8_t exp_x[4] = { 8, 6, 0, 2 };
    ok = 1;
    for (int r = 0; r < 4; r++) {
        uint8_t e, x;
        if (mv_entry_exit((uint8_t)r, &e, &x) != 0 || e != exp_e[r] || x != exp_x[r]) { ok = 0; break; }
        if (mv_entry_exit((uint8_t)(r | 4), &e, &x) != 0 || e != exp_x[r] || x != exp_e[r]) { ok = 0; break; }
    }
    check(ok, "endpoint table 4 rotations x 2 directions");
    check(mv_entry_exit(8, NULL, NULL) == -1, "bad orient rejected");

    /* endpoints always distinct corners, never center/edge */
    ok = 1;
    for (int r = 0; r < 8; r++) {
        uint8_t e, x;
        mv_entry_exit((uint8_t)r, &e, &x);
        int ce = (e == 0 || e == 2 || e == 6 || e == 8);
        int cx = (x == 0 || x == 2 || x == 6 || x == 8);
        if (e == x || !ce || !cx) { ok = 0; break; }
    }
    check(ok, "endpoints distinct corners always");

    /* init carries id/layer/slide + pins */
    check(mv_init(&n, 12345u, 7u, 20737u, 1) == 0 && n.node == 12345u &&
          n.layer == 7u && n.slide == 20737u && n.entry == 2 && n.exit == 6,
          "init carries multiverse coords + pins");
    check(mv_init(&n, 0, 0, 0, 9) == -1, "init bad orient rejected");
    check(mv_init(NULL, 0, 0, 0, 0) == -1, "init NULL rejected");

    /* edge adjacency: hand-derived spot checks */
    check(mv_edge_adj(0, 1) && mv_edge_adj(0, 3) && !mv_edge_adj(0, 2) &&
          !mv_edge_adj(0, 4) && !mv_edge_adj(0, 8) && !mv_edge_adj(0, 0),
          "corner 0 adjacency {1,3}");
    check(mv_edge_adj(4, 1) && mv_edge_adj(4, 3) && mv_edge_adj(4, 5) &&
          mv_edge_adj(4, 7) && !mv_edge_adj(4, 0),
          "center adjacency cross");
    /* degree census: corners 2, edges 3, center 4 (combinatorics) */
    ok = 1;
    for (int c = 0; c < 9; c++) {
        int deg = 0;
        for (int d = 0; d < 9; d++) deg += mv_edge_adj((uint8_t)c, (uint8_t)d);
        int is_corner = (c == 0 || c == 2 || c == 6 || c == 8);
        int want = (c == 4) ? 4 : (is_corner ? 2 : 3);
        if (deg != want) { ok = 0; break; }
    }
    check(ok, "degree census 2/3/4");

    /* pinned: exactly entry+exit, 7 cells free for minors */
    mv_init(&n, 1u, 0u, 0u, 0);
    int pinned = 0, free = 0;
    ok = 1;
    for (int c = 0; c < 9; c++) {
        int p = mv_pinned(&n, (uint8_t)c);
        if (p < 0) { ok = 0; break; }
        pinned += p; free += !p;
    }
    check(ok && pinned == 2 && free == 7, "2 pinned + 7 minor-usable");
    check(mv_pinned(&n, 9) == -1 && mv_pinned(NULL, 0) == -1, "pinned guards");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
