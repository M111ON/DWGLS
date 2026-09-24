/* tests/test_mm_route.c — major/minor routing over multiverse nodes.
 *
 * Oracles: hand-derived 3x3 adjacency (Manhattan), endpoint table from
 * test_mv_node, layer/slide rules from the owner spec (|dlayer|<=1,
 * same slide). Every expectation below is written from the spec, never
 * read back from mm_route.h.
 *
 * Layout used: A orient0 (entry 0, exit 8), B orient1 (entry 2, exit 6),
 * C orient2 (entry 8, exit 0). Free cells: A={1,2,3,4,5,6,7},
 * B={0,1,3,4,5,7,8}, C={1,2,3,4,5,6,7}.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_mm_route tests/test_mm_route.c
 * RUN:   ./build/test_mm_route
 */
#include <stdio.h>
#include <stdint.h>

#include "mm_route.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

int main(void) {
    printf("test_mm_route\n");
    MVNode A, B, C;
    mv_init(&A, 10u, 3u, 100u, 0);
    mv_init(&B, 11u, 3u, 100u, 1);
    mv_init(&C, 12u, 3u, 100u, 2);

    /* major chain: layer+slide travel, ids advance, pins per orient */
    MVNode n1, n2;
    check(mm_major_step(&A, 11u, 1, &n1) == 0 && n1.node == 11u &&
          n1.layer == 3u && n1.slide == 100u && n1.entry == 2 && n1.exit == 6,
          "major step carries layer+slide, new pins");
    check(mm_major_step(&n1, 12u, 2, &n2) == 0 && n2.entry == 8 && n2.exit == 0,
          "major chain extends");
    check(mm_major_step(NULL, 0, 0, &n2) == -1 && mm_major_step(&A, 0, 0, NULL) == -1,
          "major step guards");
    check(mm_major_step(&A, 0, 9, &n2) == -1, "major step bad orient rejected");

    /* chain audit */
    MVNode chain[3] = { A, n1, n2 };
    check(mm_chain_audit(chain, 3) == 3, "chain audit 3/3");
    check(mm_chain_audit(NULL, 3) == -1 && mm_chain_audit(chain, 0) == -1,
          "audit guards");
    MVNode bad = A;
    bad.exit = bad.entry;
    check(mm_chain_audit(&bad, 1) == -1, "audit catches pin collision");

    /* minor-free mask: A pins {0,8} → bits 1..7 set = 0xFE */
    check(mm_minor_free(&A) == 0xFEu, "A free mask 0xFE");
    check(mm_minor_free(&B) == (0x1FFu & ~(1u << 2) & ~(1u << 6)), "B free mask");
    check(mm_minor_free(NULL) == 0, "free mask NULL");

    /* minor cross: A.1→B.1? cell1 edge-adj cell1? NO — same cell, not
     * edge-adjacent (dist 0). Valid example: A.1→B.0? A.1 free ✓, B.0
     * free ✓, edge-adj(1,0) ✓ (both are cell idx; cross uses same frame).
     * NOTE: cross compares cell indices across node frames — layout
     * decision: co-located frames (same 3x3 orientation). */
    check(mm_minor_cross(&A, 1, &B, 0) == 1, "minor cross A.1→B.0 valid");
    check(mm_minor_cross(&A, 0, &B, 0) == 0, "cross from pinned entry refused");
    check(mm_minor_cross(&A, 1, &B, 2) == 0, "cross to pinned entry refused");
    check(mm_minor_cross(&A, 1, &B, 8) == 0, "cross non-adjacent refused");
    check(mm_minor_cross(&A, 1, &B, 1) == 0, "cross same-cell refused (dist 0)");
    check(mm_minor_cross(NULL, 1, &B, 0) == 0, "cross NULL refused");

    /* layer bridge ±1 at same slide */
    MVNode B4 = B;
    B4.layer = 4;
    MVNode B5 = B;
    B5.layer = 5;
    MVNode Bs = B;
    Bs.slide = 101;
    check(mm_minor_cross(&A, 1, &B4, 0) == 1, "layer +1 bridge ok");
    check(mm_minor_cross(&A, 1, &B5, 0) == 0, "layer +2 refused");
    check(mm_minor_cross(&A, 1, &Bs, 0) == 0, "different slide refused");

    /* minor path: A.1→A.4→B.4? B.4 free ✓ adj(1,4)✓ adj(4,4)? dist 0 NO.
     * Use A.1→A.4→A.3→B.3? B.3 free ✓ adj(3,3) dist 0 NO.
     * Cross-node steps need distinct edge-adjacent cells: A.3→B.0
     * (adj ✓). Path: [A.1, A.4, A.3, B.0, B.1]: steps (1,4)✓ (4,3)✓
     * (3,0)✓ adj? 3:(0,1) 0:(0,0) → dy=1 ✓, (0,1)✓. */
    MVNode path_n[5] = { A, A, A, B, B };
    uint8_t path_c[5] = { 1, 4, 3, 0, 1 };
    check(mm_minor_path(path_n, path_c, 5) == 1, "minor path across nodes");
    uint8_t bad_c[5] = { 1, 4, 3, 0, 2 };
    check(mm_minor_path(path_n, bad_c, 5) == 0, "path into pinned cell refused");
    uint8_t jump_c[3] = { 1, 4, 8 };
    MVNode jump_n[3] = { A, A, A };
    check(mm_minor_path(jump_n, jump_c, 3) == 0, "path jump refused");
    check(mm_minor_path(NULL, path_c, 5) == 0 && mm_minor_path(path_n, path_c, 0) == 0,
          "path guards");

    /* major/minor separation: minor path through exit cell fails even
     * when geometrically adjacent */
    uint8_t via_exit[2] = { 7, 8 };
    MVNode via_n[2] = { A, A };
    check(mm_minor_path(via_n, via_exit, 2) == 0, "minor via exit-8 refused");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
