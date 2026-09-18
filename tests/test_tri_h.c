// test_tri_h.c — field scale hangs on triangle height H (single knob).
// Side s -> (2H)^2 = 3s^2 exact integers. s=2: (2H)^2=12 (base!),
// area 4U = 4-subdivision whole. Oracle: Pythagoras on half-triangle.
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

int main(void) {
    printf("tri-H (scale knob):\n");
    CHECK(0, 3 * 2 * 2 == 12); // s=2: (2H)^2 = 12
    CHECK(1, 3 * 4 * 4 == 48); // s=4: (2H)^2 = 48 = 12*4 (area scales x4)
    CHECK(2, 3 * 1 * 1 == 3); // s=1: (2H)^2 = 3
    CHECK(3, 2 * 2 == 4); // s=2 area = 4U = 4-subdivision whole
    CHECK(4, 4 * 4 == 16); // s=4 area = 16U = 4x (quadratic scaling)
    CHECK(5, 48 / 12 == 4); // doubling side quadruples everything
    CHECK(6, 3 * 6 * 6 == 108); // s=6: (2H)^2 = 108 (pentagon interior)
    CHECK(7, 3 * 12 * 12 == 432 && 432 == 3 * 144); // s=12: (2H)^2 = 3x144
    // H=12: s^2 = 4*144/3 = 192 (area 192U = goldberg-192), (2H)^2 = 576 (6ico E+F)
    CHECK(8, 4 * 12 * 12 / 3 == 192);
    CHECK(9, 2 * 12 * (2 * 12) == 576);
    printf(fails ? "FAIL %d\n" : "TRIH 10/10 OK\n", fails);
    return fails;
}
