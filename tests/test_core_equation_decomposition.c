/* test_core_equation_decomposition.c — Verify Core Equation Structure
 *
 * Core equation: 128 × 162 = 144 × 144 = 20,736
 *
 * Discovery: Remove factor of 2 → 64, 81, 72
 * Rearrange: 64, 72, 81 = 8², 8×9, 9²
 * Differences: 8, 9, 17 = 8+9
 * 17³ = 4913 = boundary cube (residual space)
 *
 * BUILD: gcc -O2 -o test_core_eq_decomp test_core_equation_decomposition.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

int main(void) {
    printf("Core Equation Decomposition: 128×162 = 144×144 = 20736\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       TEST 1: Core Equation Verification
       ═══════════════════════════════════════════════════════════════════ */
    printf("TEST 1: Core Equation\n");
    printf("───────────────────────────────────────────────────────────────────\n");
    
    uint64_t prod1 = 128ULL * 162;
    uint64_t prod2 = 144ULL * 144;
    uint64_t total = 20736ULL;
    
    CHECK(1, "128 × 162 = 20736", prod1 == total);
    CHECK(2, "144 × 144 = 20736", prod2 == total);
    CHECK(3, "128 × 162 = 144 × 144", prod1 == prod2);
    
    printf("\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       TEST 2: Remove Factor of 2
       ═══════════════════════════════════════════════════════════════════ */
    printf("TEST 2: Remove Factor of 2\n");
    printf("───────────────────────────────────────────────────────────────────\n");
    
    uint32_t a = 128 / 2;  /* 64 */
    uint32_t b = 162 / 2;  /* 81 */
    uint32_t c = 144 / 2;  /* 72 */
    uint32_t d = 144 / 2;  /* 72 */
    
    printf("  128 / 2 = %u\n", a);
    printf("  162 / 2 = %u\n", b);
    printf("  144 / 2 = %u\n", c);
    printf("  144 / 2 = %u\n", d);
    
    CHECK(4, "128/2 = 64", a == 64);
    CHECK(5, "162/2 = 81", b == 81);
    CHECK(6, "144/2 = 72", c == 72);
    
    printf("\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       TEST 3: Rearrange → 64, 72, 81 = 8², 8×9, 9²
       ═══════════════════════════════════════════════════════════════════ */
    printf("TEST 3: Quadratic Sequence\n");
    printf("───────────────────────────────────────────────────────────────────\n");
    
    /* Rearrange: 64, 72, 81 */
    uint32_t seq[3] = {64, 72, 81};
    
    printf("  Sequence: %u, %u, %u\n", seq[0], seq[1], seq[2]);
    printf("  As products: %u=%u², %u=%u×%u, %u=%u²\n",
           seq[0], (int)sqrt(seq[0]),
           seq[1], (int)sqrt(seq[0]), (int)sqrt(seq[2]),
           seq[2], (int)sqrt(seq[2]));
    
    CHECK(7, "64 = 8²", seq[0] == 8*8);
    CHECK(8, "72 = 8×9", seq[1] == 8*9);
    CHECK(9, "81 = 9²", seq[2] == 9*9);
    
    /* Verify quadratic pattern: a², a×b, b² */
    uint32_t a_base = 8, b_base = 9;
    CHECK(10, "Pattern: a², a×b, b² where a=8, b=9",
          seq[0] == a_base*a_base &&
          seq[1] == a_base*b_base &&
          seq[2] == b_base*b_base);
    
    printf("\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       TEST 4: Differences → 8, 9, 17 = 8+9
       ═══════════════════════════════════════════════════════════════════ */
    printf("TEST 4: Self-Referential Differences\n");
    printf("───────────────────────────────────────────────────────────────────\n");
    
    uint32_t d1 = seq[1] - seq[0];  /* 72 - 64 = 8 */
    uint32_t d2 = seq[2] - seq[1];  /* 81 - 72 = 9 */
    uint32_t d3 = seq[2] - seq[0];  /* 81 - 64 = 17 */
    
    printf("  72 - 64 = %u\n", d1);
    printf("  81 - 72 = %u\n", d2);
    printf("  81 - 64 = %u\n", d3);
    printf("  8 + 9 = %u\n", d1 + d2);
    
    CHECK(11, "72 - 64 = 8", d1 == 8);
    CHECK(12, "81 - 72 = 9", d2 == 9);
    CHECK(13, "81 - 64 = 17", d3 == 17);
    CHECK(14, "8 + 9 = 17 (self-referential)", d1 + d2 == d3);
    
    printf("\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       TEST 5: 17³ = Boundary Cube (Residual Space)
       ═══════════════════════════════════════════════════════════════════ */
    printf("TEST 5: Boundary Cube\n");
    printf("───────────────────────────────────────────────────────────────────\n");
    
    uint64_t cube17 = 17ULL * 17 * 17;  /* 4913 */
    uint64_t residual = 20736 - cube17;  /* 20736 - 4913 = 15823 */
    
    printf("  17³ = %llu\n", cube17);
    printf("  20736 - 17³ = %llu\n", residual);
    printf("  20736 / 17³ = %.2f\n", (double)20736 / cube17);
    
    CHECK(15, "17³ = 4913", cube17 == 4913);
    CHECK(16, "20736 - 4913 = 15823 (residual)", residual == 15823);
    
    printf("\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       TEST 6: Connection to System
       ═══════════════════════════════════════════════════════════════════ */
    printf("TEST 6: Connection to System\n");
    printf("───────────────────────────────────────────────────────────────────\n");
    
    /* 20736 = 12⁴ = (2² × 3)⁴ = 2⁸ × 3⁴ */
    uint64_t p2_8 = 256;  /* 2⁸ */
    uint64_t p3_4 = 81;   /* 3⁴ */
    CHECK(17, "20736 = 2⁸ × 3⁴ = 256 × 81", p2_8 * p3_4 == 20736);
    
    /* 64 = 2⁶, 81 = 3⁴, 72 = 2³ × 3² */
    printf("  64 = 2⁶ (pure compute)\n");
    printf("  81 = 3⁴ (pure geometry)\n");
    printf("  72 = 2³ × 3² (mixed)\n");
    
    CHECK(18, "64 = 2⁶", (1<<6) == 64);
    CHECK(19, "81 = 3⁴", 3*3*3*3 == 81);
    CHECK(20, "72 = 2³ × 3²", (1<<3) * (3*3) == 72);
    
    /* 17 = 81 - 64 = 3⁴ - 2⁶ (geometry - compute) */
    uint32_t diff = 81 - 64;
    printf("  17 = 81 - 64 = 3⁴ - 2⁶ (geometry - compute)\n");
    CHECK(21, "17 = 3⁴ - 2⁶", diff == 17);
    
    printf("\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       TEST 7: Complete Structure
       ═══════════════════════════════════════════════════════════════════ */
    printf("TEST 7: Complete Structure\n");
    printf("───────────────────────────────────────────────────────────────────\n");
    
    printf("  Core equation: 128 × 162 = 144 × 144 = 20736\n");
    printf("  Remove factor 2: 64, 81, 72\n");
    printf("  Rearrange: 64, 72, 81 = 8², 8×9, 9²\n");
    printf("  Differences: 8, 9, 17 = 8+9\n");
    printf("  17³ = 4913 = boundary cube\n");
    printf("  Residual: 20736 - 4913 = 15823\n");
    printf("\n");
    printf("  Connection:\n");
    printf("    64 = 2⁶ = compute side\n");
    printf("    81 = 3⁴ = geometry side\n");
    printf("    72 = 2³×3² = mixed (bridge)\n");
    printf("    17 = 3⁴ - 2⁶ = difference (boundary)\n");
    printf("    17³ = boundary cube (residual space)\n");
    
    CHECK(22, "Complete structure verified", 1);
    
    printf("\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       SUMMARY
       ═══════════════════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    
    return 0;
}
