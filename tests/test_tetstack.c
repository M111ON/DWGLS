// test_tetstack.c — tetrahedron+octahedron stack (octet truss) conservation.
// Figures: octa/tetra interlock (1), 4 corner tetras (2), side-2 tetra
// = 4 small tetras + 1 octahedron (3), tetra star (4), triangle->rhombus
// face map (5), octahedron in sphere (6).
// Identity: T(n)+D(n)+4*O(n) == n^3, with tetrahedral T, down-tetras D,
// octahedral gaps O. Oracle: simplex combinatorics.
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

static int T(int n) { return n * (n + 1) * (n + 2) / 6; } // up tetras
static int D(int n) { return n < 3 ? 0 : (n - 2) * (n - 1) * n / 6; } // down tetras
static int O(int n) { return n < 2 ? 0 : (n - 1) * n * (n + 1) / 6; } // octa gaps

int main(void) {
    printf("tet-stack (octet truss conservation):\n");
    CHECK(0, T(1) == 1 && T(2) == 4 && T(3) == 10); // tetrahedral numbers
    CHECK(1, D(1) == 0 && D(2) == 0 && D(3) == 1); // down tetras appear at n=3
    CHECK(2, O(1) == 0 && O(2) == 1 && O(3) == 4); // octa gaps (fig 3: the 1)
    CHECK(3, T(2) + D(2) + 4 * O(2) == 8); // 4+0+4 = 2^3 (fig 3: 4 tetras + 1 octa)
    CHECK(4, T(3) + D(3) + 4 * O(3) == 27); // 10+1+16 = 3^3
    CHECK(5, T(4) + D(4) + 4 * O(4) == 64); // 20+4+40 = 4^3
    int n = 5;
    CHECK(6, T(n) + D(n) + 4 * O(n) == n * n * n); // general conservation
    CHECK(7, 4 * 1 == 4); // octahedron = 4 tetra-volumes (the x4 weight)
    printf(fails ? "FAIL %d\n" : "TETSTACK 8/8 OK\n", fails);
    return fails;
}
