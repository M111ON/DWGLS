/* test_fractal_addr.c — Test Fractal Coordinate Addressing
 * ═══════════════════════════════════════════════════════════════════════════════
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -Icore \
 *          -o build/test-fractal_addr tests/test_fractal_addr.c -lm
 * ═══════════════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_fractal_addr.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static void test_constants(void)
{
    printf("T0: Constants — 12⁴ = 20736\n");
    CHECK(0, "FRACTAL_FULL == 20736", FRACTAL_FULL == 20736);
    CHECK(0, "cell(0)==1 cell(1)==12 cell(2)==144",
          fractal_cell_size(0)==1 && fractal_cell_size(1)==12 &&
          fractal_cell_size(2)==144 && fractal_cell_size(3)==1728 &&
          fractal_cell_size(4)==20736);
    CHECK(0, "h=4: 1×1×20736=20736",
          fractal_grid_w(4)*fractal_grid_h(4)*fractal_cell_size(4)==20736);
    CHECK(0, "h=3: 12×1×1728=20736",
          fractal_grid_w(3)*fractal_grid_h(3)*fractal_cell_size(3)==20736);
    CHECK(0, "h=2: 12×12×144=20736",
          fractal_grid_w(2)*fractal_grid_h(2)*fractal_cell_size(2)==20736);
    CHECK(0, "h=1: 144×12×12=20736",
          fractal_grid_w(1)*fractal_grid_h(1)*fractal_cell_size(1)==20736);
    CHECK(0, "h=0: 144×144×1=20736",
          fractal_grid_w(0)*fractal_grid_h(0)*fractal_cell_size(0)==20736);
}

static void test_roundtrip(void)
{
    printf("T1: flat ↔ (h,x,y) roundtrip\n");
    int ok = 1;
    for (uint32_t h = 0; h <= 4 && ok; h++) {
        uint32_t gw = fractal_grid_w(h);
        uint32_t gh = fractal_grid_h(h);
        for (uint32_t x = 0; x < gw && ok; x++) {
            for (uint32_t y = 0; y < gh && ok; y++) {
                uint32_t flat = fractal_to_flat(h, x, y);
                if (flat >= FRACTAL_FULL) { ok = 0; break; }
                FractalAddr a = fractal_from_flat(flat, h);
                if (a.h != h || a.x != x || a.y != y) { ok = 0; break; }
            }
        }
    }
    CHECK(1, "all 5 levels roundtrip", ok);
}

static void test_h0_coverage(void)
{
    printf("T2: h=0 covers all 20736 addresses\n");
    uint8_t seen[20736 / 8 + 1];
    memset(seen, 0, sizeof(seen));
    int ok = 1;
    for (uint32_t x = 0; x < 144 && ok; x++) {
        for (uint32_t y = 0; y < 144 && ok; y++) {
            uint32_t flat = fractal_to_flat(0, x, y);
            if (flat >= 20736) { ok = 0; break; }
            if (seen[flat >> 3] & (1u << (flat & 7u))) { ok = 0; break; }
            seen[flat >> 3] |= (1u << (flat & 7u));
        }
    }
    CHECK(2, "144×144 = 20736, zero collision", ok);
}

static void test_h4_root(void)
{
    printf("T3: h=4 = root\n");
    CHECK(3, "grid 1×1", fractal_grid_w(4)==1 && fractal_grid_h(4)==1);
    CHECK(3, "cell_size 20736", fractal_cell_size(4)==20736);
    CHECK(3, "flat(4,0,0)==0", fractal_to_flat(4,0,0)==0);
}

static void test_h1_pipes(void)
{
    printf("T4: h=1 = 1728 pipes\n");
    CHECK(4, "grid 144×12, cell=12",
          fractal_grid_w(1)==144 && fractal_grid_h(1)==12 &&
          fractal_cell_size(1)==12);
}

static void test_h2_groups(void)
{
    printf("T5: h=2 = 144 groups\n");
    CHECK(5, "grid 12×12, cell=144",
          fractal_grid_w(2)==12 && fractal_grid_h(2)==12 &&
          fractal_cell_size(2)==144);
}

static void test_parent_child(void)
{
    printf("T6: Parent/child consistency\n");
    int ok = 1;
    for (uint32_t x = 0; x < fractal_grid_w(1) && ok; x++) {
        for (uint32_t y = 0; y < fractal_grid_h(1) && ok; y++) {
            FractalAddr p = fractal_parent(x, y, 1);
            if (p.h != 2) { ok = 0; break; }
        }
    }
    CHECK(6, "h=1 → parent h=2", ok);
}

static void test_cell_coverage(void)
{
    printf("T7: Cell coverage\n");
    int ok = 1;
    for (uint32_t h = 1; h <= 3 && ok; h++) {
        uint32_t csz = fractal_cell_size(h);
        uint32_t gw = fractal_grid_w(h);
        uint32_t gh = fractal_grid_h(h);
        for (uint32_t x = 0; x < gw && ok; x++) {
            for (uint32_t y = 0; y < gh && ok; y++) {
                uint32_t base = fractal_to_flat(h, x, y);
                for (uint32_t off = 0; off < csz && ok; off++) {
                    FractalAddr a = fractal_from_flat(base + off, h);
                    if (a.x != x || a.y != y) { ok = 0; break; }
                }
            }
        }
    }
    CHECK(7, "each cell contains exactly cell_size addrs", ok);
}

static void test_dram_bridge(void)
{
    printf("T8: DRamTile bridge\n");
    int ok = 1;
    for (uint32_t f = 0; f < 20736 && ok; f++) {
        if (fractal_to_dram(f) != f || dram_to_fractal(f) != f) ok = 0;
    }
    CHECK(8, "identity bridge", ok);
}

static void test_spine_bridge(void)
{
    printf("T9: FiboSpine bridge\n");
    int ok = 1;
    for (uint32_t f = 0; f < 20736 && ok; f++) {
        uint16_t pid; uint8_t tick;
        fractal_to_pipe_tick(f, &pid, &tick);
        if (pipe_tick_to_fractal(pid, tick) != f) ok = 0;
        if (pid >= 1728 || tick >= 12) ok = 0;
    }
    CHECK(9, "flat ↔ pipe_id,tick roundtrip", ok);
}

static void test_local_coords(void)
{
    printf("T10: Local coords within cell\n");
    int ok = 1;
    /* h=2: cell_size=144, grid 12×12, cell (x,y) starts at flat = x*144*12 + y*144 */
    for (uint32_t x = 0; x < 12 && ok; x++) {
        for (uint32_t y = 0; y < 12 && ok; y++) {
            uint32_t flat = fractal_to_flat(2, x, y);
            FractalAddr a = fractal_from_flat(flat, 2);
            if (a.x != x || a.y != y) { ok = 0; break; }
        }
    }
    CHECK(10, "h=2 local coords correct", ok);
}

static void test_entropy_split(void)
{
    printf("T11: Entropy split\n");
    CHECK(11, "h=0 never splits", fractal_should_split(255, 0) == 0);
    CHECK(11, "h=4 no split", fractal_should_split(0, 4) == 0);
    CHECK(11, "h=2 low entropy → group", fractal_should_split(50, 2) == 0);
    CHECK(11, "h=2 high entropy → split", fractal_should_split(200, 2) == 1);
}

static void test_full_verify(void)
{
    printf("T12: Full verify\n");
    CHECK(12, "fractal_verify()==0", fractal_verify() == 0);
}

/* ── T13: address ranges per level ── */
static void test_address_ranges(void)
{
    printf("T13: Address ranges\n");
    /* h=0: x ∈ [0,143], y ∈ [0,143], flat ∈ [0,20735] */
    uint32_t max_flat_0 = fractal_to_flat(0, 143, 143);
    CHECK(13, "h=0 max flat == 20735", max_flat_0 == 20735);

    /* h=2: cell (0,0) starts at 0 */
    CHECK(13, "h=2 flat(0,0)==0", fractal_to_flat(2, 0, 0) == 0);

    /* h=1: cell (0,0) starts at 0, cell (0,1) starts at 12 */
    CHECK(13, "h=1 flat(0,1)==12", fractal_to_flat(1, 0, 1) == 12);

    /* h=1: cell (1,0) starts at 12*12 = 144 */
    CHECK(13, "h=1 flat(1,0)==144", fractal_to_flat(1, 1, 0) == 144);
}

int main(void)
{
    printf("═══════════════════════════════════════════════\n");
    printf("  Fractal Coordinate Addressing Test\n");
    printf("  20736 = 12⁴ = tensor rank-4\n");
    printf("═══════════════════════════════════════════════\n\n");

    test_constants();
    test_roundtrip();
    test_h0_coverage();
    test_h4_root();
    test_h1_pipes();
    test_h2_groups();
    test_parent_child();
    test_cell_coverage();
    test_dram_bridge();
    test_spine_bridge();
    test_local_coords();
    test_entropy_split();
    test_full_verify();
    test_address_ranges();

    printf("\n═══════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════\n");
    return fail;
}
