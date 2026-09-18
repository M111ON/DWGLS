// test_digit9.c — doubling/squaring chain preserves digit-sum 27 -> root 9.
// 144^2=20736, 288^2=82944, 576^2=331776; digit sums 18/27, roots all 9.
// 27 = 3^3 (base-3 signature). Oracle: arithmetic.
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

static long dsum(long x) { long s = 0; while (x) { s += x % 10; x /= 10; } return s; }
static long drood(long x) { while (x >= 10) x = dsum(x); return x; }

int main(void) {
    printf("digit-9 chain:\n");
    CHECK(0, 12L * 12 == 144);
    CHECK(1, 144L * 144 == 20736);
    CHECK(2, 288L * 288 == 82944);
    CHECK(3, 576L * 576 == 331776);
    CHECK(4, dsum(20736) == 18 && dsum(82944) == 27 && dsum(331776) == 27);
    CHECK(5, drood(144) == 9 && drood(20736) == 9 && drood(288) == 9);
    CHECK(6, drood(576) == 9 && drood(82944) == 9 && drood(331776) == 9);
    CHECK(7, 27 == 3 * 3 * 3); // 27 = 3^3 base-3 signature
    CHECK(8, 82944 == 4 * 20736 && 331776 == 4 * 82944); // x4 per doubling
    CHECK(9, dsum(144) == 9); // 144 itself already root 9
    printf(fails ? "FAIL %d\n" : "DIGIT9 10/10 OK\n", fails);
    return fails;
}
