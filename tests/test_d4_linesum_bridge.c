/*
 * test_d4_linesum_bridge.c — Tests for D4 symmetry, line-sum, level bridge
 * ═══════════════════════════════════════════════════════════════════════════
 * BUILD: gcc -Wall -Wextra -Wno-unused-parameter -Icore -Icore/infra -no-pie \
 *        tests/test_d4_linesum_bridge.c -o build/test_d4_linesum_bridge.exe
 * ═══════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_fractal_addr.h"
#include "../core/infra/geo_level_bridge.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── T0: D4 identity ── */
static void test_d4_identity(void)
{
    printf("T0: D4 identity\n");
    FractalAddr r = fractal_d4_apply(5, 3, 12, 12, FRACTAL_D4_ID);
    CHECK(0, "x preserved", r.x == 5);
    CHECK(0, "y preserved", r.y == 3);
}

/* ── T1: D4 rot90 on 12×12 grid ── */
static void test_d4_rot90(void)
{
    printf("T1: D4 rot90 on 12×12\n");
    /* (0,0) → (0, 11) on 12×12 (sx=11) */
    FractalAddr r = fractal_d4_apply(0, 0, 12, 12, FRACTAL_D4_ROT90);
    CHECK(1, "x = y = 0", r.x == 0);
    CHECK(1, "y = sx-x = 11", r.y == 11);

    /* (11,0) → (0, 0) */
    r = fractal_d4_apply(11, 0, 12, 12, FRACTAL_D4_ROT90);
    CHECK(1, "corner maps to origin", r.x == 0 && r.y == 0);
}

/* ── T2: D4 self-inverse (all except rot90/270) ── */
static void test_d4_self_inverse(void)
{
    printf("T2: D4 self-inverse\n");
    for (int op = 0; op < 8; op++) {
        if (op == FRACTAL_D4_ROT90 || op == FRACTAL_D4_ROT270) continue;
        FractalAddr r = fractal_d4_apply(3, 7, 12, 12, (FractalD4Op)op);
        FractalAddr back = fractal_d4_apply(r.x, r.y, 12, 12, (FractalD4Op)op);
        CHECK(2, "double apply = identity", back.x == 3 && back.y == 7);
    }
}

/* ── T3: D4 rot90^4 = identity ── */
static void test_d4_rot90_cycle(void)
{
    printf("T3: D4 rot90^4 = identity\n");
    uint32_t x = 5, y = 3;
    for (int i = 0; i < 4; i++) {
        FractalAddr r = fractal_d4_apply(x, y, 12, 12, FRACTAL_D4_ROT90);
        x = r.x; y = r.y;
    }
    CHECK(3, "4 rotations return to start", x == 5 && y == 3);
}

/* ── T4: Line-sum of Lo Shu magic square ── */
static void test_linesum_magic(void)
{
    printf("T4: Line-sum — Lo Shu magic square\n");
    uint8_t magic[3][3] = {
        {2, 7, 6},
        {9, 5, 1},
        {4, 3, 8}
    };
    FractalLineSum ls = fractal_line_sum_3x3(magic);
    CHECK(4, "all rows = 15", ls.row[0]==15 && ls.row[1]==15 && ls.row[2]==15);
    CHECK(4, "all cols = 15", ls.col[0]==15 && ls.col[1]==15 && ls.col[2]==15);
    CHECK(4, "diag = 15", ls.diag == 15);
    CHECK(4, "anti = 15", ls.anti == 15);
    CHECK(4, "n15 = 8", fractal_n15_count(ls) == 8);
}

/* ── T5: Line-sum of uniform grid ── */
static void test_linesum_uniform(void)
{
    printf("T5: Line-sum — uniform grid\n");
    uint8_t grid[3][3] = {{5,5,5},{5,5,5},{5,5,5}};
    FractalLineSum ls = fractal_line_sum_3x3(grid);
    CHECK(5, "all rows = 15", ls.row[0]==15 && ls.row[1]==15 && ls.row[2]==15);
    CHECK(5, "n15 = 8 (all sum to 15)", fractal_n15_count(ls) == 8);
}

/* ── T6: Line-sum of all-ones ── */
static void test_linesum_ones(void)
{
    printf("T6: Line-sum — all-ones grid\n");
    uint8_t grid[3][3] = {{1,1,1},{1,1,1},{1,1,1}};
    FractalLineSum ls = fractal_line_sum_3x3(grid);
    CHECK(6, "rows = 3", ls.row[0] == 3);
    CHECK(6, "n15 = 0", fractal_n15_count(ls) == 0);
}

/* ── T7: Bridge chord invariant ── */
static void test_bridge_chord(void)
{
    printf("T7: Bridge chord invariant\n");
    CHECK(7, "2+7=9 ✓", lb_check_chord(2, 7) == 1);
    CHECK(7, "7+2=9 ✓", lb_check_chord(7, 2) == 1);
    CHECK(7, "3+5=8 ✗", lb_check_chord(3, 5) == 0);
    CHECK(7, "0+0=0 ✗", lb_check_chord(0, 0) == 0);
}

/* ── T8: Bridge create ── */
static void test_bridge_create(void)
{
    printf("T8: Bridge create\n");
    uint8_t coarse[3][3] = {{2,7,6},{9,5,1},{4,3,8}};
    uint8_t fine[3][3]   = {{1,8,3},{6,5,4},{7,2,9}};
    FractalAddr pc = {3, 11, 0};
    FractalAddr pf = {2, 0, 0};
    LB_Bridge b = lb_bridge_create(3, coarse, fine, pc, pf);
    CHECK(8, "level_top = 3", b.level_top == 3);
    CHECK(8, "level_bot = 2", b.level_bot == 2);
    CHECK(8, "state is PASS or BLOCK", b.state == LB_PASS || b.state == LB_BLOCK);
}

/* ── T9: Bridge set ── */
static void test_bridge_set(void)
{
    printf("T9: Bridge set\n");
    LB_BridgeSet set = lb_set_init();
    CHECK(9, "empty set", set.count == 0 && set.active == 0);

    LB_Bridge b1 = {0}; b1.state = LB_PASS; b1.weight = 100;
    int8_t idx = lb_set_add(&set, b1);
    CHECK(9, "added at index 0", idx == 0);
    CHECK(9, "count = 1", set.count == 1);
    CHECK(9, "active = 1", set.active == 1);

    LB_Bridge b2 = {0}; b2.state = LB_BLOCK; b2.weight = 0;
    lb_set_add(&set, b2);
    CHECK(9, "count = 2", set.count == 2);
    CHECK(9, "active still 1", set.active == 1);

    uint8_t counts[4];
    lb_set_stats(&set, counts);
    CHECK(9, "PASS=1, BLOCK=1", counts[0] == 1 && counts[1] == 1);

    uint32_t total = lb_set_total_weight(&set);
    CHECK(9, "total weight = 100", total == 100);
}

/* ── T10: Bridge signal propagation ── */
static void test_bridge_propagate(void)
{
    printf("T10: Bridge signal propagation\n");
    LB_Bridge b_pass = {0}; b_pass.state = LB_PASS; b_pass.weight = 200;
    LB_Bridge b_block = {0}; b_block.state = LB_BLOCK; b_block.weight = 0;

    CHECK(10, "up through PASS", lb_propagate_up(&b_pass, 255) > 0);
    CHECK(10, "up through BLOCK = 0", lb_propagate_up(&b_block, 255) == 0);
    CHECK(10, "down through PASS", lb_propagate_down(&b_pass, 255) > 0);
    CHECK(10, "down through BLOCK = 0", lb_propagate_down(&b_block, 255) == 0);
}

int main(void)
{
    printf("═══════════════════════════════════════════════════════\n");
    printf("  D4 + LINE-SUM + BRIDGE TEST SUITE\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    test_d4_identity();
    test_d4_rot90();
    test_d4_self_inverse();
    test_d4_rot90_cycle();
    test_linesum_magic();
    test_linesum_uniform();
    test_linesum_ones();
    test_bridge_chord();
    test_bridge_create();
    test_bridge_set();
    test_bridge_propagate();

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════════\n");
    return fail;
}
