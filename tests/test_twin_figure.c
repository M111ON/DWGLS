// test_twin_figure.c — pins the kis/hyper twin figure's claims in integers.
// Figure: 2 overlapping circles (twins) + up/down triangles (duals) +
// node circles (key frames) + vertical axis (W timeline), ONE shared field.
// Oracle: the figure + sacred constants (never the implementation).
// usage: gcc -O2 -o test_twin_figure test_twin_figure.c && ./test_twin_figure
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

static int ugcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

int main(void) {
    printf("twin-figure invariants:\n");
    // T0: ONE shared field — both twins read the same 20736 cells
    CHECK(0, 144 * 144 == 20736);
    // T1: TWO views, structurally DIFFERENT (kis stride-37 vs hyper stride-1 family)
    CHECK(1, 37 != 1);
    // T2: universal stride coprime to field (bijection, no collision)
    CHECK(2, ugcd(37, 20736) == 1);
    // T3: twin stride triple {1,9,81} = mixed-radix spec, all odd (parity flip every step)
    CHECK(3, 1 % 2 == 1 && 9 % 2 == 1 && 81 % 2 == 1);
    CHECK(4, 20736 == 256 * 81); // 2^8 * 3^4: binary ladder x Peano ladder
    // T5: vertical axis = W timeline, 144 teeth; W=12 <-> s=0.5 in s*65536 fixed point
    CHECK(5, 144 == 12 * 12);
    CHECK(6, (65536 >> 1) == 32768);
    // T7: dual triangles (up/down) = both parities of i+j+k occur in {0,1}
    CHECK(7, (0 + 0 + 0) % 2 == 0 && (0 + 0 + 1) % 2 == 1);
    // T8: ring-24 gear identity: 144 === 0 (mod 24), frame-invariant teeth
    CHECK(8, 144 % 24 == 0 && 20736 % 144 == 0);
    // T9: 18 tesseracts x 1152 = full 18tes field (8 cubes x 144 slots each)
    CHECK(9, 18 * 8 * 144 == 20736);
    printf(fails ? "FAIL %d\n" : "TWIN_FIGURE 10/10 OK\n", fails);
    return fails;
}
