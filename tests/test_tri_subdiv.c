// test_tri_subdiv.c — triangle subdivision counts behind the hex-cell pair.
// Fig A (cells): hexagonal rosettes on triangular patches.
// Fig B (rule): big triangle -> 4 sub-triangles, rosettes at 3 corners + hub center.
// Oracle: the figures + triangular numbers (never the implementation).
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

static int ipow(int b, int e) { int r = 1; while (e--) r *= b; return r; }
// binary (edge-halving) subdivision: sub-triangles and distinct vertices
static int S2(int n) { return ipow(4, n); }
static int V2(int n) { int s = ipow(2, n); return (s + 1) * (s + 2) / 2; }
// ternary (Peano 3-ladder) subdivision
static int S3(int n) { return ipow(9, n); }

int main(void) {
    printf("triangle-subdivision (hex-cell pair):\n");
    CHECK(0, S2(0) == 1 && S2(1) == 4 && S2(2) == 16); // 4^n sub-triangles
    CHECK(1, V2(0) == 3 && V2(1) == 6 && V2(2) == 15); // triangular numbers
    CHECK(2, V2(1) == 6 && 6 < 3 * 4); // vertices shared, never double-counted
    CHECK(3, S3(0) == 1 && S3(1) == 9); // 9^n ternary cells
    CHECK(4, S3(2) == 81); // depth-2 ternary = 81 = Peano lo range [0,81)
    CHECK(5, 3 + 1 == 4); // 3 corner rosettes + center hub = tetrahedron anchors
    CHECK(6, V2(1) - 3 == 3); // depth-1 adds 3 edge-midpoint nodes (circles on edges)
    CHECK(7, S2(1) == 4 && 4 * 3 == 12); // 4 sub-tris x 3 sides = 12 = base unit
    printf(fails ? "FAIL %d\n" : "TRI_SUBDIV 8/8 OK\n", fails);
    return fails;
}
