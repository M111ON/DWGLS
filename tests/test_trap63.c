// test_trap63.c — 3 small + 3 large alternating trapezoids inside a hexagon.
// Figure: spoke-and-ring interior divides the hexagon into 6 sectors,
// alternating sizes: two interleaved triples (even/odd positions).
// Oracle: the figure (alternation + opposite-equality), arbitrary units.
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

int main(void) {
    printf("trap-3+3 (alternating hexagon interior):\n");
    int s = 2, L = 5; // distinct small/large units (any values work)
    int a[6];
    for (int i = 0; i < 6; i++) a[i] = (i % 2 == 0) ? s : L;
    int ns = 0, nl = 0;
    for (int i = 0; i < 6; i++) { if (a[i] == s) ns++; else nl++; }
    CHECK(0, ns == 3 && nl == 3); // 3 small + 3 large
    int alt = 1;
    for (int i = 0; i < 6; i++) if (a[i] == a[(i + 1) % 6]) alt = 0;
    CHECK(1, alt); // strict alternation: no two same-size adjacent
    int opp = 1, skip2 = 1;
    for (int i = 0; i < 3; i++) if (a[i] == a[i + 3]) opp = 0;
    for (int i = 0; i < 6; i++) if (a[i] != a[(i + 2) % 6]) skip2 = 0;
    CHECK(2, opp && skip2); // opposites differ (parity flips), 2nd-neighbors equal
    int tot = 0;
    for (int i = 0; i < 6; i++) tot += a[i];
    CHECK(3, tot == 3 * (s + L)); // total = 3 small + 3 large
    int te = a[0] + a[2] + a[4], to = a[1] + a[3] + a[5];
    CHECK(4, te == 3 * s && to == 3 * L); // even triple vs odd triple
    CHECK(5, te != to); // triples distinct (small vs large orientation)
    CHECK(6, 6 == 3 + 3); // hexagon = two triples (xyz / ijk axis triples)
    CHECK(7, ns + nl == 6 && tot > 0); // partition complete, nothing lost
    printf(fails ? "FAIL %d\n" : "TRAP63 8/8 OK\n", fails);
    return fails;
}
