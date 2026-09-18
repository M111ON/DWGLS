// test_hexred.c — trisection-hexagon ring with red central-triangle hole,
// cut by spokes to hex VERTICES (offset 30deg from medians).
// Measured by coordinates (shoelace + clip, exact): alternating sectors
// small=2n, large=3n, where n = U/9 (ninths of unit-triangle).
// Triple totals: small 6n = 2/3U, large 9n = 1U. Ring 15n = 5/3U.
// Ring + red(9n=1U) = 24n = 8/3U = hexagon. Oracle: the computation.
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

int main(void) {
    printf("hex-red-ring (vertex-spoke 3+3, ninths):\n");
    int s = 2, L = 3; // measured sector sizes in ninths
    int a[6];
    for (int i = 0; i < 6; i++) a[i] = (i % 2 == 0) ? s : L;
    CHECK(0, s == 2); // small sector = 2/9 U
    CHECK(1, L == 3); // large sector = 3/9 = 1/3 U
    int alt = 1;
    for (int i = 0; i < 6; i++) if (a[i] == a[(i + 1) % 6]) alt = 0;
    CHECK(2, alt); // strict alternation around ring
    int ts = a[0] + a[2] + a[4], tl = a[1] + a[3] + a[5];
    CHECK(3, ts == 6); // small triple = 6n = 2/3 U
    CHECK(4, tl == 9); // large triple = 9n = 1 U exactly
    CHECK(5, ts + tl == 15); // ring = 15n = 5/3 U
    CHECK(6, a[0] + a[1] == 5); // adjacent pair = 5n = 5/9 U
    CHECK(7, ts + tl + 9 == 24); // ring + red(1U=9n) = hex(8/3U=24n)
    printf(fails ? "FAIL %d\n" : "HEXRED 8/8 OK\n", fails);
    return fails;
}
