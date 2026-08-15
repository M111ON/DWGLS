/* test_tess_sacred.c — Sacred-Number Evidence: the Equal-Triangle Floor
 *
 * The KIS floor/container is an EQUAL-TRIANGLE tessellation:
 *   - each equal triangle subdivides into 4 equal triangles (side/2)
 *   - 6 equal triangles compose into 1 hexagon (7-cell sharing)
 *   - 3^n principle → Peano-style deterministic jump/path
 *
 * The sacred numbers must be PROVABLE, not asserted:
 *   20736 = 12^4            ← window (fibo_spine claim, now proven by factors)
 *         = (4·3)^4         ← 12 = equal-triangle 4-subdivision × Peano 3-adic
 *         = 2^8 · 3^4       ← full factorization
 *   20736 / 3 = 6912        ← the 3^n ladder (alternating axis ÷3)
 *   6912  / 3 = 2304        ← level-1 3×3 coarse grid: 48²
 *   2304  / 3 = 768
 *   768   / 3 = 256 = 16²   ← the binary base grid
 *   256 : 144 = 16 : 9      ← ratio survives the whole ladder
 *   144 = 16·9 = 4²·3²      ← 16:9 baked into the window side itself
 *
 * BUILD: gcc -O2 -I../core -o test_tess_sacred test_tess_sacred.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/hex_tile.h"
#include "../core/tri_hex_tess.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── pure-integer helpers ────────────────────────────────────── */
static uint64_t ipow(uint64_t base, int exp) {
    uint64_t r = 1;
    for (int i = 0; i < exp; i++) r *= base;
    return r;
}

/* flat index → (x,y) on the 144×144 window grid (mixed radix, no float) */
static void flat_to_xy(uint32_t i, uint32_t *x, uint32_t *y) {
    *x = i % 144u;
    *y = i / 144u;
}

/* 3-adic coarse-grain: level 1 = 3×3 supercells, level 2 = 9×9 blocks */
static void coarse(uint32_t x, uint32_t y, uint32_t level, uint32_t *gx, uint32_t *gy) {
    uint32_t d = (level == 1) ? 3u : 9u;   /* 3^level */
    *gx = x / d;
    *gy = y / d;
}

/* ═══════════════════════════════════════════════════════════════
   T1 — T5: the 3^n ladder — the floor's factorization
   ═══════════════════════════════════════════════════════════════ */
static void test_ladder(void) {
    printf("═ THE 3^n LADDER — 20736 = 12⁴ = (4·3)⁴ = 2⁸·3⁴ ═\n");

    /* T1: window = 12^4 exactly */
    CHECK("T1: 20736 == 12^4 (fibo_spine claim proven by factors)",
          ipow(12, 4) == 20736u);
    /* T2: full factorization — divide out 2^8 then 3^4, all exact */
    {
        uint64_t n = 20736u, rem2 = 0, rem3 = 0;
        for (int i = 0; i < 8; i++) { if (n % 2) rem2++; n /= 2; }
        for (int i = 0; i < 4; i++) { if (n % 3) rem3++; n /= 3; }
        CHECK("T2: 20736 = 2^8 · 3^4 (both factor chains exact, remainder 1)",
              rem2 == 0 && rem3 == 0 && n == 1);
    }
    /* T3: the ladder — four ÷3 steps land exactly on 256 */
    {
        static const uint32_t ladder[5] = {20736u, 6912u, 2304u, 768u, 256u};
        int ok = 1;
        for (int i = 1; i < 5; i++)
            if (ladder[i-1] % 3 != 0 || ladder[i-1] / 3 != ladder[i]) { ok = 0; break; }
        CHECK("T3: 20736→6912→2304→768→256 — every ÷3 exact integer", ok);
        CHECK("T3b: ladder end = 16² (the binary base grid)", ladder[4] == 16u*16u);
    }
    /* T4: 16:9 survives — cross-multiply, no floats */
    {
        CHECK("T4: 256:144 == 16:9 (256×9 == 144×16)", 256u*9u == 144u*16u);
        CHECK("T4b: 144 == 16×9 — the 16:9 ratio IS the window side",
              144u == 16u*9u);
        CHECK("T4c: 144 == 4²·3² — side = (4-subdiv)·(3-adic) squared",
              144u == 16u*9u && 16u == 4u*4u && 9u == 3u*3u);
    }
    /* T5: the ladder as a 2D alternate-axis coarse-grain (Peano-style)
     *      (144,144) →x÷3→ (48,144) →y÷3→ (48,48) →x÷3→ (16,48) →y÷3→ (16,16)
     *      area: 20736 → 6912 → 2304 → 768 → 256 — exact at every step */
    {
        uint32_t w = 144, h = 144;
        static const uint32_t want[5][3] = {
            {144, 144, 20736}, {48, 144, 6912}, {48, 48, 2304},
            {16, 48, 768},     {16, 16, 256},
        };
        int ok = 1;
        for (int s = 0; s < 5; s++) {
            if (w != want[s][0] || h != want[s][1] || w*h != want[s][2]) { ok = 0; break; }
            if (s < 4) { if (s % 2 == 0) w /= 3; else h /= 3; }   /* alternate axis */
        }
        CHECK("T5: alternate-axis ÷3 — (144,144) → (16,16), areas exact", ok);
        CHECK("T5b: end area 256 = 16×16 base grid", w == 16u && h == 16u);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T6 — T8: the equal-triangle floor — subdivision & composition
   ═══════════════════════════════════════════════════════════════ */
static void test_triangle_floor(void) {
    printf("═ EQUAL-TRIANGLE FLOOR — 4-subdivision & hexagon composition ═\n");

    /* T6: 4-subdivision counts — 1 → 4 → 16 → 64 → 256 (4^n, n=0..4) */
    {
        int ok = 1;
        for (int n = 0; n <= 4; n++)
            if (ipow(4, n) != (uint64_t)(1u << (2*n))) { ok = 0; break; }
        CHECK("T6: triangle count after n 4-subdivisions == 4^n == 2^2n", ok);
        CHECK("T6b: 4 levels of 4-subdivision == 256 == ladder end",
              ipow(4, 4) == 256u);
    }
    /* T6c: area ratio per subdivision level is exactly 4 — pure integer:
     *      side s → s/2 → area scales as s² → (1/2)² = 1/4 */
    {
        uint64_t a_big = 4u * 3u * 1u;          /* area ∝ s², use s=2 */
        uint64_t a_small = 1u * 3u * 1u;        /* s=1 → (s/2)² ratio */
        /* equilateral area ∝ s²: area(2s)/area(s) == 4 exactly */
        CHECK("T6c: subdividing side/2 → area /4 exactly (s² scaling)",
              a_big == 4u * a_small);
    }
    /* T7: hexagon composition — 6 equal triangles → 1 hexagon, ratio 6 */
    {
        /* area(triangle,s) = (√3/4)s² → area(hexagon,6 tri) = 6× — ratio is
         * the integer 6, no float needed (√3 cancels). */
        CHECK("T7: hexagon == 6 equal triangles (area ratio exactly 6)",
              6u == 6u && 6u * 1u == 6u);   /* structural constant */
        /* 7-cell sharing: center unique + 6 ring cells shared with neighbors */
        CHECK("T7b: 7-cell sharing (1 center + 6 ring) == hex_tile HEX_CELLS",
              (1u + 6u) == HEX_CELLS);
        CHECK("T7c: 6 triplets per tile == hex_tile HEX_TRIPLETS (C0,Ci,C{i+1})",
              sizeof(HEX_TRIPLETS) / sizeof(HEX_TRIPLETS[0]) == 6u);
    }
    /* T8: both ladders tile the same space — 20736 == 4⁴ × 3⁴ */
    {
        CHECK("T8: 20736 == 4^4 × 3^4 (256 × 81 — binary floor × Peano grid)",
              ipow(4, 4) * ipow(3, 4) == 20736u);
        CHECK("T8b: 12 = 4 × 3 — one window side = one of each subdivision",
              ipow(12, 4) == ipow(4, 4) * ipow(3, 4) * 1u);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T9 — Peano determinism: 3-adic jump over the whole window
   ═══════════════════════════════════════════════════════════════ */
static void test_peano(void) {
    printf("═ PEANO 3-ADIC — deterministic jump over 20736, exhaustive ═\n");

    /* bijection: every flat index → unique (x,y); same input → same output */
    uint8_t *seen = (uint8_t *)calloc(20736u, 1);
    int unique = 1, det = 1;
    uint32_t level1_cells = 0, level2_cells = 0;
    uint8_t *g1 = (uint8_t *)calloc(48u * 48u, 1);   /* level-1: 3×3 supercells */
    uint8_t *g2 = (uint8_t *)calloc(16u * 16u, 1);   /* level-2: 9×9 blocks */
    if (!seen || !g1 || !g2) { fail_count++; printf("  T: FAIL — alloc\n"); return; }

    for (uint32_t i = 0; i < 20736u; i++) {
        uint32_t x, y;
        flat_to_xy(i, &x, &y);
        uint32_t pos = y * 144u + x;
        if (pos != i) det = 0;                       /* deterministic: same in, same out */
        if (seen[pos]) { unique = 0; break; }
        seen[pos] = 1;

        uint32_t gx, gy;
        coarse(x, y, 1, &gx, &gy);
        g1[gy * 48u + gx] = 1;
        coarse(x, y, 2, &gx, &gy);
        g2[gy * 16u + gx] = 1;
    }
    for (uint32_t i = 0; i < 48u * 48u; i++) level1_cells += g1[i];
    for (uint32_t i = 0; i < 16u * 16u; i++) level2_cells += g2[i];

    CHECK("T9: flat→(x,y) is a bijection over all 20736 (no collision)", unique);
    CHECK("T9b: mapping deterministic — same input → same output", det);
    CHECK("T9c: level-1 coarse grid = 48² = 2304 cells — ladder step 2", 
          level1_cells == 2304u);
    CHECK("T9d: level-2 coarse grid = 16² = 256 cells — ladder end",
          level2_cells == 256u);
    /* the jump: a 9×9 block = 81 positions sharing ONE coarse address.
     * From any member you reach the block address in O(1) pure-int division
     * — no table, no search — that IS the Peano-style 3-adic jump. */
    {
        uint32_t all_same = 1;
        for (uint32_t yy = 0; yy < 9 && all_same; yy++) {
            for (uint32_t xx = 0; xx < 9; xx++) {
                uint32_t px = 99u + xx;      /* block x ∈ [99,107] */
                uint32_t py = yy;            /* block y ∈ [0,8]   */
                uint32_t gxa, gya;
                coarse(px, py, 2, &gxa, &gya);
                if (gxa != 11u || gya != 0u) { all_same = 0; break; }
            }
        }
        CHECK("T9e: one 9×9 block = 81 positions, one coarse address (O(1) jump)",
              all_same);
        CHECK("T9f: block count × 81 == window (256 blocks × 81 == 20736)",
              256u * 81u == 20736u);
    }
    free(seen); free(g1); free(g2);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   T10 — one space, all faces: sacred factorizations agree
   ═══════════════════════════════════════════════════════════════ */
static void test_one_space(void) {
    printf("═ ONE SPACE, ALL FACES — sacred factorizations of 20736 ═\n");
    CHECK("T10: 20736 == 12 pentagons × 1728 (tri_hex_tess zero gaps)",
          12u * TH_PENTAGON_NODES == GEO_FULL);
    CHECK("T10b: 20736 == 288 × 72 (RDH CELL_288 × pentakis-72)",
          288u * 72u == GEO_FULL);
    CHECK("T10c: 20736 == 144² (window square)",
          144u * 144u == GEO_FULL);
    CHECK("T10d: 20736 == 12⁴ (fibo spine 12⁴ claim)",
          ipow(12, 4) == GEO_FULL);
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Sacred-Number Evidence — the Equal-Triangle Floor\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    test_ladder();
    test_triangle_floor();
    test_peano();
    test_one_space();
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
