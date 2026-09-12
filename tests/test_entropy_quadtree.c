/* test_entropy_quadtree.c — Test Entropy-Driven QuadTree
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -Icore -Icore/infra \
 *        -o build/test-entropy_quadtree tests/test_entropy_quadtree.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_entropy_quadtree.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── T0: Node creation with uniform data (low entropy) ── */
static void test_uniform_node(void)
{
    printf("T0: Node — uniform data (low entropy)\n");
    uint8_t data[144];
    memset(data, 0x42, 144);
    EQNode n = eq_make_node(2, 0, 0, data, 20736);
    CHECK(0, "h==2", n.h == 2);
    CHECK(0, "addr == fractal_to_flat(2,0,0)", n.addr == fractal_to_flat(2,0,0));
    CHECK(0, "size == 144", n.size == 144);
    CHECK(0, "entropy < threshold (uniform)", n.entropy < EQ_THRESHOLD_LOW);
    CHECK(0, "split == NONE (grouped)", n.split == EQ_SPLIT_NONE);
    CHECK(0, "myelinated == 0", n.myelinated == 0);
}

/* ── T1: Node with high-entropy data ── */
static void test_high_entropy_node(void)
{
    printf("T1: Node — random data (high entropy)\n");
    uint8_t data[20736];
    /* Use pattern with full 256-range in 144-byte window for high entropy */
    for (int i = 0; i < 20736; i++) data[i] = (uint8_t)(i * 13 + 7);
    EQNode n = eq_make_node(2, 0, 0, data, 20736);
    CHECK(1, "entropy > 0", n.entropy > 0);
    CHECK(1, "split != NONE", n.split != EQ_SPLIT_NONE);
    CHECK(1, "myelinated (high entropy in 144-byte window)", n.myelinated == 1);
}

/* ── T2: Split decisions at each level ── */
static void test_split_decisions(void)
{
    printf("T2: Split decisions per level\n");
    CHECK(2, "h=0 → NONE (leaf)", eq_split_decision(255, 0) == EQ_SPLIT_NONE);
    CHECK(2, "h=4 → BOTH (root)", eq_split_decision(0, 4) == EQ_SPLIT_BOTH);
    CHECK(2, "h=2, entropy=32 → NONE",
          eq_split_decision(32, 2) == EQ_SPLIT_NONE);
    CHECK(2, "h=2, entropy=220 → BOTH",
          eq_split_decision(220, 2) == EQ_SPLIT_BOTH);
    /* Medium entropy: X or Y */
    uint8_t med = eq_split_decision(128, 2);
    CHECK(2, "h=2, entropy=128 → X or Y",
          med == EQ_SPLIT_X || med == EQ_SPLIT_Y);
}

/* ── T3: Entropy computation — deterministic ── */
static void test_entropy_deterministic(void)
{
    printf("T3: Entropy computation deterministic\n");
    uint8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;
    uint8_t e1 = eq_compute_entropy(data, 256);
    uint8_t e2 = eq_compute_entropy(data, 256);
    CHECK(3, "same input → same entropy", e1 == e2);

    uint8_t uniform[256];
    memset(uniform, 0x55, 256);
    uint8_t e3 = eq_compute_entropy(uniform, 256);
    CHECK(3, "uniform < random", e3 < e1);
}

/* ── T4: Full tree traversal — BFS on 20736 ── */
static void test_full_tree(void)
{
    printf("T4: Full tree traversal (synthetic data)\n");
    uint8_t data[20736];
    for (int i = 0; i < 20736; i++) data[i] = (uint8_t)(i * 7 + 3);

    EQStats st;
    eq_stats_init(&st);

    /* Start from root h=4, split recursively */
    uint32_t stack_h[8192];
    uint32_t stack_x[8192], stack_y[8192];
    int sp = 0;
    stack_h[sp] = 4; stack_x[sp] = 0; stack_y[sp] = 0; sp++;

    while (sp > 0) {
        sp--;
        uint32_t h = stack_h[sp], x = stack_x[sp], y = stack_y[sp];
        EQNode n = eq_make_node(h, x, y, data, 20736);
        if (n.split == EQ_SPLIT_NONE || h == 0) {
            /* Leaf: count its addresses */
            st.total_nodes++;
            st.total_addrs += n.size;
            if (n.myelinated) st.myelinated++;
            st.split_none++;
            if (h < st.max_depth || st.total_nodes == 1)
                st.max_depth = h;
            continue;
        }
        /* Internal: split stats only (don't count addresses) */
        st.total_nodes++;
        if (n.myelinated) st.myelinated++;
        switch (n.split) {
            case EQ_SPLIT_X:     st.split_x++;     break;
            case EQ_SPLIT_Y:     st.split_y++;     break;
            case EQ_SPLIT_BOTH:  st.split_both++;  break;
            default: break;
        }
        /* Push children */
        uint32_t gw = fractal_grid_w(h - 1);
        uint32_t gh = fractal_grid_h(h - 1);
        FractalAddr tl = fractal_child_top_left(x, y, h);
        uint32_t step_x = gw / fractal_grid_w(h);
        uint32_t step_y = gh / fractal_grid_h(h);
        for (uint32_t dx = 0; dx < step_x && sp < 8192; dx++) {
            for (uint32_t dy = 0; dy < step_y && sp < 8192; dy++) {
                stack_h[sp] = h - 1;
                stack_x[sp] = tl.x + dx;
                stack_y[sp] = tl.y + dy;
                sp++;
            }
        }
    }

    CHECK(4, "total_nodes > 0", st.total_nodes > 0);
    CHECK(4, "total_addrs == 20736", st.total_addrs == 20736);
    CHECK(4, "has grouped (split_none > 0)", st.split_none > 0);
    CHECK(4, "has myelinated", st.myelinated > 0);
    eq_stats_print(&st);
}

/* ── T5: Node at different positions ── */
static void test_various_positions(void)
{
    printf("T5: Nodes at various positions\n");
    uint8_t data[20736];
    for (int i = 0; i < 20736; i++) data[i] = (uint8_t)i;

    int ok = 1;
    for (uint32_t x = 0; x < 12 && ok; x++) {
        for (uint32_t y = 0; y < 12 && ok; y++) {
            EQNode n = eq_make_node(2, x, y, data, 20736);
            if (n.addr != fractal_to_flat(2, x, y)) { ok = 0; break; }
            if (n.size != 144) { ok = 0; break; }
        }
    }
    CHECK(5, "144 positions all have correct addr/size", ok);
}

/* ── T6: NULL data → entropy 0 ── */
static void test_null_data(void)
{
    printf("T6: NULL data → entropy 0\n");
    EQNode n = eq_make_node(2, 0, 0, NULL, 0);
    CHECK(6, "entropy == 0", n.entropy == 0);
    CHECK(6, "split == NONE", n.split == EQ_SPLIT_NONE);
}

int main(void)
{
    printf("═══════════════════════════════════════════════\n");
    printf("  Entropy QuadTree Test\n");
    printf("═══════════════════════════════════════════════\n\n");

    test_uniform_node();
    test_high_entropy_node();
    test_split_decisions();
    test_entropy_deterministic();
    test_full_tree();
    test_various_positions();
    test_null_data();

    printf("\n═══════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════\n");
    return fail;
}
