/*
 * test_twin_rebalance.c — Test Twin Rebalance: 128×162 ↔ 144×144
 * ═══════════════════════════════════════════════════════════════════════════
 * BUILD: gcc -Wall -Wextra -Wno-unused-parameter -Icore -Icore/infra -no-pie \
 *        tests/test_twin_rebalance.c -o build/test_twin_rebalance.exe
 * ═══════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_dram_tile.h"
#include "../core/infra/geo_twin_rebalance.h"
#include "geo_fractal_addr.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── T0: Constants consistency ── */
static void test_constants(void)
{
    printf("T0: Constants — 128×162 = 144×144 = 20736\n");
    CHECK(0, "128 × 162 == 20736", TW_COMPUTE_SIDE * TW_GEOM_SIDE == TW_TOTAL);
    CHECK(0, "144 × 144 == 20736", TW_NATURAL_SIDE * TW_NATURAL_SIDE == TW_TOTAL);
    CHECK(0, "2^8 × 3^4 == 20736", (1u<<8) * (81u) == TW_TOTAL);
    CHECK(0, "DRAM_ANCHORS == 162", DRAM_ANCHORS == TW_GEOM_SIDE);
    CHECK(0, "DRAM_CELLS_PER == 128", DRAM_CELLS_PER == TW_COMPUTE_SIDE);
    CHECK(0, "DRAM_FULL == 20736", DRAM_FULL == TW_TOTAL);
}

/* ── T1: Flat ↔ Hardware roundtrip (full 20736) ── */
static void test_flat_hard_roundtrip(void)
{
    printf("T1: Flat ↔ Hardware roundtrip (20736 addresses)\n");
    int errors = 0;
    for (uint32_t flat = 0; flat < TW_TOTAL; flat++) {
        TW_HardAddr h = tw_flat_to_hard(flat);
        uint32_t back = tw_hard_to_flat(h);
        if (back != flat) errors++;
    }
    CHECK(1, "all 20736 roundtrip", errors == 0);
}

/* ── T2: Flat ↔ Natural roundtrip (full 20736) ── */
static void test_flat_nat_roundtrip(void)
{
    printf("T2: Flat ↔ Natural roundtrip (20736 addresses)\n");
    int errors = 0;
    for (uint32_t flat = 0; flat < TW_TOTAL; flat++) {
        TW_NatAddr n = tw_flat_to_nat(flat);
        uint32_t back = tw_nat_to_flat(n);
        if (back != flat) errors++;
    }
    CHECK(2, "all 20736 roundtrip", errors == 0);
}

/* ── T3: Hard ↔ Nat → Hard roundtrip (full 20736) ── */
static void test_hard_nat_roundtrip(void)
{
    printf("T3: Hard ↔ Natural ↔ Hard roundtrip\n");
    int errors = 0;
    for (uint32_t a = 0; a < TW_GEOM_SIDE; a++) {
        for (uint32_t l = 0; l < TW_COMPUTE_SIDE; l++) {
            TW_HardAddr h = {a, l};
            TW_NatAddr n = tw_hard_to_nat(h);
            TW_HardAddr back = tw_nat_to_hard(n);
            if (back.anchor != a || back.local != l) errors++;
        }
    }
    CHECK(3, "all 20736 Hard→Nat→Hard roundtrip", errors == 0);
}

/* ── T4: Bijection verify ── */
static void test_bijection(void)
{
    printf("T4: Bijection verification\n");
    CHECK(4, "tw_verify_bijection == 0", tw_verify_bijection() == 0);
    CHECK(4, "tw_verify_bounds == 0", tw_verify_bounds() == 0);
}

/* ── T5: Boundaries ── */
static void test_boundaries(void)
{
    printf("T5: Boundary addresses\n");

    /* Flat 0 → anchor 0, local 0 → row 0, col 0 */
    TW_HardAddr h0 = tw_flat_to_hard(0);
    TW_NatAddr n0 = tw_flat_to_nat(0);
    CHECK(5, "flat=0 → anchor=0, local=0", h0.anchor == 0 && h0.local == 0);
    CHECK(5, "flat=0 → row=0, col=0", n0.row == 0 && n0.col == 0);

    /* Flat 20735 → anchor 161, local 127 → row 143, col 143 */
    TW_HardAddr hmax = tw_flat_to_hard(TW_TOTAL - 1);
    TW_NatAddr nmax = tw_flat_to_nat(TW_TOTAL - 1);
    CHECK(5, "flat=20735 → anchor=161, local=127",
          hmax.anchor == 161 && hmax.local == 127);
    CHECK(5, "flat=20735 → row=143, col=143",
          nmax.row == 143 && nmax.col == 143);

    /* Anchor 1, local 0 → flat 128 → row 0, col 128 */
    TW_NatAddr n128 = tw_hard_to_nat((TW_HardAddr){1, 0});
    CHECK(5, "anchor=1,local=0 → row=0, col=128", n128.row == 0 && n128.col == 128);

    /* Row 1, col 0 → flat 144 → anchor 1, local 16 */
    TW_HardAddr h144 = tw_nat_to_hard((TW_NatAddr){1, 0});
    CHECK(5, "row=1,col=0 → anchor=1, local=16", h144.anchor == 1 && h144.local == 16);
}

/* ── T6: Data redistribution (no overlap) ── */
static void test_data_redistribution(void)
{
    printf("T6: Data redistribution lossless\n");
    uint8_t src[TW_TOTAL];
    uint8_t natural[TW_TOTAL];
    uint8_t back[TW_TOTAL];

    /* Fill source with unique pattern */
    for (uint32_t i = 0; i < TW_TOTAL; i++) src[i] = (uint8_t)(i & 0xFF);

    /* DRamTile → Natural → DRamTile */
    tw_rebalance_to_natural(src, natural);
    tw_rebalance_to_hardware(natural, back);

    int errors = 0;
    for (uint32_t i = 0; i < TW_TOTAL; i++) {
        if (src[i] != back[i]) errors++;
    }
    CHECK(6, "roundtrip byte-identical", errors == 0);
}

/* ── T7: Semantic identity — twin is reinterpretation, not copy ── */
static void test_data_layout_differs(void)
{
    printf("T7: Semantic identity — twin = reinterpretation\n");
    uint8_t src[TW_TOTAL];
    uint8_t natural[TW_TOTAL];

    for (uint32_t i = 0; i < TW_TOTAL; i++) src[i] = (uint8_t)(i & 0xFF);
    tw_rebalance_to_natural(src, natural);

    /* KEY INSIGHT: dst[flat] = src[flat] — same byte at same flat offset.
     * The layouts use the SAME flat indexing, so the copy is identity.
     * The "twin" is SEMANTIC: how you INDEX (anchor×128 vs row×144),
     * not how you STORE the bytes. */
    int identical = 1;
    for (uint32_t i = 0; i < TW_TOTAL; i++) {
        if (src[i] != natural[i]) { identical = 0; break; }
    }
    CHECK(7, "twin rebalance = identity (semantic, not physical)", identical);

    /* BUT: the SEMANTIC interpretation differs.
     * src[0..127] = anchor 0 (128 bytes)
     * natural[0..143] = row 0 (144 bytes)
     * These OVERLAP but have different semantics. */
    TW_HardAddr h_at_128 = tw_flat_to_hard(128);
    CHECK(7, "flat=128 is anchor=1 (new anchor starts)", h_at_128.anchor == 1);
    TW_NatAddr n_at_128 = tw_flat_to_nat(128);
    CHECK(7, "flat=128 is col=128 in row=0 (same row continues)", n_at_128.row == 0 && n_at_128.col == 128);
}

/* ── T8: Cross-view queries ── */
static void test_cross_view(void)
{
    printf("T8: Cross-view position queries\n");

    /* DRamTile (anchor=5, local=64) → where in field? */
    TW_NatAddr field = tw_where_in_field(5, 64);
    uint32_t flat = tw_addr(5, 64);  /* = 5*128+64 = 704 */
    TW_NatAddr expected = tw_flat_to_nat(flat);  /* = 704/144=4, 704%144=128 */
    CHECK(8, "cross-view hard→field", field.row == expected.row && field.col == expected.col);

    /* Field (row=4, col=128) → where in DRamTile? */
    TW_HardAddr dram = tw_where_in_dram(4, 128);
    uint32_t flat2 = tw_nat_to_flat((TW_NatAddr){4, 128});  /* = 4*144+128 = 704 */
    TW_HardAddr expected2 = tw_flat_to_hard(flat2);  /* = 704/128=5, 704%128=64 */
    CHECK(8, "cross-view field→dram", dram.anchor == expected2.anchor && dram.local == expected2.local);
}

/* ── T9: Composition with DRamTile ── */
static void test_compose_dram(void)
{
    printf("T9: Composition with DRamTile address\n");
    /* dram_addr(10, 3, 5, 1) → flat → field position */
    uint32_t flat = dram_addr(10, 3, 5, 1);
    TW_NatAddr field = tw_dram_to_field(10, 3, 5, 1);

    /* Verify: field should match flat_to_nat(flat) */
    TW_NatAddr expected = tw_flat_to_nat(flat);
    CHECK(9, "dram→field matches flat→nat",
          field.row == expected.row && field.col == expected.col);
}

/* ── T10: Zero-copy reinterpret ── */
static void test_zero_copy(void)
{
    printf("T10: Zero-copy reinterpret\n");
    uint8_t buf[TW_TOTAL];
    for (uint32_t i = 0; i < TW_TOTAL; i++) buf[i] = (uint8_t)(i * 3 + 7);

    /* Reinterpret as 144×144 field — same memory */
    uint8_t (*field)[TW_NATURAL_SIDE] = tw_interpret_as_natural(buf);

    /* Read through field view: field[row][col] should equal buf[row*144+col] */
    int errors = 0;
    for (uint32_t r = 0; r < TW_NATURAL_SIDE; r++) {
        for (uint32_t c = 0; c < TW_NATURAL_SIDE; c++) {
            uint32_t flat = r * TW_NATURAL_SIDE + c;
            if (field[r][c] != buf[flat]) errors++;
        }
    }
    CHECK(10, "zero-copy field view matches", errors == 0);

    /* Modify through field view, check through buf */
    field[0][0] = 0xFF;
    CHECK(10, "zero-copy write visible in buf", buf[0] == 0xFF);
}

/* ── T11: Verify full system ── */
static void test_full_verify(void)
{
    printf("T11: Full verification functions\n");
    CHECK(11, "tw_verify_bijection", tw_verify_bijection() == 0);
    CHECK(11, "tw_verify_bounds", tw_verify_bounds() == 0);

    uint8_t data[TW_TOTAL];
    for (uint32_t i = 0; i < TW_TOTAL; i++) data[i] = (uint8_t)i;
    CHECK(11, "tw_verify_data_roundtrip", tw_verify_data_roundtrip(data) == 0);
}

/* ── T12: Compose with fractal ── */
static void test_compose_fractal(void)
{
    printf("T12: Composition with fractal addressing\n");
    /* fractal_to_flat(2, 5, 3) → flat → DRamTile position */
    TW_HardAddr dram = tw_fractal_to_dram(2, 5, 3);
    uint32_t flat = fractal_to_flat(2, 5, 3);
    TW_HardAddr expected = tw_flat_to_hard(flat);
    CHECK(12, "fractal→dram matches flat→hard",
          dram.anchor == expected.anchor && dram.local == expected.local);
}

int main(void)
{
    printf("═══════════════════════════════════════════════════════\n");
    printf("  TWIN REBALANCE TEST SUITE\n");
    printf("  128×162 ↔ 144×144 = 20736\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    test_constants();
    test_flat_hard_roundtrip();
    test_flat_nat_roundtrip();
    test_hard_nat_roundtrip();
    test_bijection();
    test_boundaries();
    test_data_redistribution();
    test_data_layout_differs();
    test_cross_view();
    test_compose_dram();
    test_zero_copy();
    test_full_verify();
    test_compose_fractal();

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════════\n");
    return fail;
}
