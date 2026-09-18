// test_dualworld.c — xyz-cube world vs ijk-triangle world chain (owner derivation).
// xyz: squares, 6 dirs = 3 pairs (+-), base-2 (16/32/2^5).
// ijk: triangles, 4 dirs unique, base-3 (27).
// Combined: 1152 (tesseract), 864, pentagon angles {36,54,72,108}, 12 -> 144.
// NOTE: "360-108=144" as written is 252 (WRONG); corrected to 360-2*108=144,
// matching the text's own doubling/halving pattern. Oracle: arithmetic.
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

int main(void) {
    printf("dual-world chain (xyz x ijk):\n");
    CHECK(0, 3 * 6 == 18 && 18 * 2 == 36 && 6 * 6 == 36); // 18tes, doubling
    CHECK(1, 3 * 3 * 3 == 27); // 3 axes base-3
    CHECK(2, 4 * 4 == 16 && 16 * 2 == 32 && 8 * 4 == 32); // tesseract verts, doubling
    CHECK(3, 32 == 2 * 2 * 2 * 2 * 2); // 2^5: why base-2 works
    CHECK(4, 6 * 6 * 8 * 4 == 1152); // one tesseract from both worlds
    CHECK(5, 27 * 32 == 864); // base3 x base2 parts
    CHECK(6, 864 / 8 == 108); // pentagon interior angle
    // owner intent: triangle world 180-72=108 (square world's 360 = 4x90 separate)
    CHECK(7, 180 - 72 == 108);
    CHECK(8, 4 * 90 == 360); // square world total belongs here, not in the chain
    CHECK(9, 108 - 72 == 36 && 108 + 36 == 144); // loop closes: 108->36->144
    CHECK(10, 144 / 2 == 72 && 108 / 2 == 54); // 72 regenerates: fixed point
    // pentagon angle family {36,54,72,108}: golden-triangle vertex/base, half/full interior
    CHECK(11, 36 + 72 + 72 == 180 && 2 * 54 == 108);
    CHECK(12, 32 - 27 == 5); // base2 - base3 = pentagon number
    CHECK(13, 6 + 6 == 12 && 8 + 4 == 12 && 12 * 12 == 144); // both roads -> 12 -> 144
    // structural: cube 6 faces in 3 parallel pairs; tetra 4 faces, none parallel
    CHECK(14, 6 / 2 == 3 && 4 - 0 == 4); // +-3 pairs vs 4 unique dirs
    printf(fails ? "FAIL %d\n" : "DUALWORLD 15/15 OK\n", fails);
    return fails;
}
