/* test_gidpith_gates.c — gate map vs source-extracted oracles.
 * Oracles come from data/gidpith_layers.json (parsed from Klitzing
 * gidpith.htm), NOT from the header under test. Structural asserts
 * (38 rows, entry/exit) are hand-derived from the same source.
 */
#include <stdio.h>
#include <string.h>
#include "geo_gidpith_gates.h"

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { pass++; } else { fail++; printf("FAIL %s\n", name); } \
} while (0)

int main(void) {
    /* entry pole: layer 1 carries all four firsts. */
    CHECK(strcmp(HJ_GATES[0].layer, "1") == 0, "row0 is layer 1");
    CHECK(HJ_GATES[0].first == HJ_GATE_ALL, "entry has all firsts");
    CHECK(HJ_GATES[0].opposite == 0, "entry has no opposite");

    /* exit pole: layer 24 carries opposite hip. */
    CHECK(strcmp(HJ_GATES[37].layer, "24") == 0, "row37 is layer 24");
    CHECK(HJ_GATES[37].opposite == HJ_GATE_HIP, "exit is opposite hip");

    /* per-cell opposites land where the source says (not layer-mirrored). */
    CHECK(hj_opposite_row(HJ_GATE_GIRCO) == 10u, "girco opposite at 8a=10");
    CHECK(strcmp(HJ_GATES[10].layer, "8a") == 0, "row10 is 8a");
    CHECK(hj_opposite_row(HJ_GATE_TOE) == 25u, "toe opposite at 15=25");
    CHECK(hj_opposite_row(HJ_GATE_OP) == 26u, "op opposite at 16a=26");
    CHECK(hj_opposite_row(HJ_GATE_HIP) == 37u, "hip opposite at 24=37");

    /* no other row carries firsts or opposites (source has exactly 5 marks). */
    unsigned marks = 0;
    for (unsigned i = 0; i < HJ_GATE_ROWS; i++)
        if (HJ_GATES[i].first || HJ_GATES[i].opposite) marks++;
    CHECK(marks == 5u, "exactly 5 annotated rows");

    /* layer order spot checks (source order). */
    CHECK(strcmp(HJ_GATES[19].layer, "12a") == 0, "row19 is 12a");
    CHECK(strcmp(HJ_GATES[22].layer, "13b") == 0, "row22 is 13b");
    CHECK(strcmp(HJ_GATES[25].layer, "15") == 0, "row25 is 15");

    /* tower -> gate binding (pole-to-pole lock). */
    CHECK(hj_tower_gate(0) == HJ_ROW_ENTRY, "tower0 -> entry");
    CHECK(hj_tower_gate(1) == HJ_ROW_MID_A, "tower1 -> transit");
    CHECK(hj_tower_gate(2) == HJ_ROW_EXIT, "tower2 -> exit");

    /* tower0 entry gate and tower2 exit gate are the antipodal poles. */
    CHECK(HJ_GATES[hj_tower_gate(0)].first == HJ_GATE_ALL, "entry pole full");
    CHECK(HJ_GATES[hj_tower_gate(2)].opposite == HJ_GATE_HIP, "exit pole hip");

    printf("gidpith_gates: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
