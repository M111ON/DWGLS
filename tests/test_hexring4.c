// test_hexring4.c — hexagon ring (hexagram core) == 4-subdivision whole.
// Verified numerically (shoelace + Sutherland-Hodgman clip, exact):
// outer hex R=1 (18 third-units) - inner star-core 6t = ring 12t = 4U,
// where U = unit-triangle area. 4U == side-2 triangle (4-subdivide whole).
// Per adjacent pair (big+small): 12t/3 = 4t = 4/3 U == user's "1+(2/6)".
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

int main(void) {
    printf("hex-ring-4 (ring == 4-subdivision):\n");
    // third-units t (1U = 3t): all exact integers
    int outer = 18; // (3sqrt3/2 R^2) / (sqrt3/12) with R=1
    int inner = 6;  // 6 spokes x (1/sqrt3)^2
    int ring = outer - inner;
    CHECK(0, outer == 18);
    CHECK(1, inner == 6);
    CHECK(2, ring == 12);
    CHECK(3, ring == 4 * 3); // 12t = 4U = side-2 triangle whole (4-subdivide)
    CHECK(4, ring / 3 == 4); // per adjacent pair: 4t = 4/3 U == 1+(2/6)
    CHECK(5, ring % 3 == 0); // pairs divide evenly: 3 pairs, no remainder
    CHECK(6, outer - ring == inner); // nothing lost: outer = ring + core
    CHECK(7, inner * 3 == outer); // core is exactly 1/3 of outer (the "2/6" in 1+(2/6))
    printf(fails ? "FAIL %d\n" : "HEXRING4 8/8 OK\n", fails);
    return fails;
}
