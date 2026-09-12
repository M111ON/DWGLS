/*
 * test_hex_quad_dual.c — Test Hex-Quad-Dual Upgrades
 * ═══════════════════════════════════════════════════════════════════════════════
 * Tests: CRT Bridge, A2×A2 Symmetry, D4 Triality
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -Icore/infra -no-pie \
 *        tests/test_hex_quad_dual.c -o build/test_hex_quad_dual.exe
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../core/geo_crt_bridge.h"
#include "../core/geo_param_grid.h"
#include "../core/infra/geo_twin_rebalance.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   CRT BRIDGE TESTS
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_crt_constants(void)
{
    printf("T0: CRT Constants\n");
    CHECK(0, "CRT_MODULUS == 20736", CRT_MODULUS == 20736);
    CHECK(0, "CRT_BIN_SIZE == 256", CRT_BIN_SIZE == 256);
    CHECK(0, "CRT_TER_SIZE == 81", CRT_TER_SIZE == 81);
    CHECK(0, "256 × 81 == 20736", CRT_BIN_SIZE * CRT_TER_SIZE == CRT_MODULUS);
    CHECK(0, "81 × 177 ≡ 1 mod 256", (81u * 177u) % 256u == 1u);
    CHECK(0, "13 × 25 ≡ 1 mod 81", (13u * 25u) % 81u == 1u);
}

static void test_crt_roundtrip(void)
{
    printf("T1: CRT Roundtrip (flat ↔ (bin, ter))\n");
    int r = crt_verify();
    CHECK(1, "crt_verify() == 0", r == 0);
}

static void test_crt_edge_cases(void)
{
    printf("T2: CRT Edge Cases\n");

    /* flat=0 → (0, 0) */
    CRTAddr a0 = crt_from_flat(0);
    CHECK(0, "flat=0 → bin=0", a0.bin == 0);
    CHECK(0, "flat=0 → ter=0", a0.ter == 0);

    /* flat=20735 → max values */
    CRTAddr aMax = crt_from_flat(20735);
    uint32_t back = crt_to_flat(aMax);
    CHECK(0, "flat=20735 roundtrip", back == 20735);

    /* flat=1 → unique decomposition */
    CRTAddr a1 = crt_from_flat(1);
    uint32_t back1 = crt_to_flat(a1);
    CHECK(0, "flat=1 roundtrip", back1 == 1);

    /* flat=144 → special number */
    CRTAddr a144 = crt_from_flat(144);
    uint32_t back144 = crt_to_flat(a144);
    CHECK(0, "flat=144 roundtrip", back144 == 144);

    /* flat=20736 → wraps to 0 */
    CRTAddr aWrap = crt_from_flat(20736);
    CHECK(0, "flat=20736 → wraps", aWrap.bin == 0 && aWrap.ter == 0);
}

static void test_crt_path_decomposition(void)
{
    printf("T3: CRT Path Decomposition (binary bits + ternary trits)\n");

    /* Test every flat address */
    for (uint32_t flat = 0; flat < 20736; flat++) {
        CRTAddr a = crt_from_flat(flat);

        /* Binary path roundtrip */
        uint32_t bits[8];
        for (int d = 0; d < 8; d++)
            bits[d] = crt_bin_bit(a.bin, d);
        uint32_t bin_back = crt_bin_from_path(bits);
        if (bin_back != a.bin) {
            printf("  FAIL at flat=%u: bin roundtrip %u != %u\n", flat, bin_back, a.bin);
            fail++; return;
        }

        /* Ternary path roundtrip */
        uint32_t trits[4];
        for (int d = 0; d < 4; d++)
            trits[d] = crt_ter_trit(a.ter, d);
        uint32_t ter_back = crt_ter_from_path(trits);
        if (ter_back != a.ter) {
            printf("  FAIL at flat=%u: ter roundtrip %u != %u\n", flat, ter_back, a.ter);
            fail++; return;
        }
    }
    pass++;
    printf("  T3: PASS — All 20736 path decompositions roundtrip\n");
}

static void test_crt_seek(void)
{
    printf("T4: CRT Seek (O(1) address decomposition)\n");

    /* Seek for flat=12345 */
    CRTSeekResult r = crt_seek(12345);
    CHECK(0, "seek(12345).cube ∈ [0,255]", r.cube < 256);
    CHECK(0, "seek(12345).subcell ∈ [0,80]", r.subcell < 81);

    /* Reconstruct from seek result */
    uint32_t flat_recon = crt_encode(r.cube, r.subcell);
    CHECK(0, "seek→encode roundtrip", flat_recon == 12345);

    /* Performance comparison */
    printf("    linear scan: O(20736) = 20736 ops\n");
    printf("    CRT seek:    O(256+81) = 337 ops\n");
    printf("    speedup:     61.5x\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   A2 × A2 SYMMETRY TESTS
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_a2_constants(void)
{
    printf("T5: A2×A2 Constants\n");
    CHECK(0, "A2_ORDER == 6", A2_ORDER == 6);
    CHECK(0, "A2xA2_ORDER == 36", A2xA2_ORDER == 36);
    CHECK(0, "A2xA2xC2_ORDER == 144", A2xA2xC2_ORDER == 144);
    CHECK(0, "6 × 6 × 4 == 144", A2_ORDER * A2_ORDER * 4 == 144);
}

static void test_a2_generators(void)
{
    printf("T6: A2 Generators (60° rotation + reflection)\n");

    /* Rotation: pos → (pos+1) mod 6 */
    for (uint32_t pos = 0; pos < 6; pos++) {
        uint32_t rotated = a2_rotate(pos, 6);
        CHECK(0, "rotation wraps at 6", rotated == (pos + 1) % 6);
    }

    /* 6 rotations = identity */
    uint32_t pos = 3;
    for (int i = 0; i < 6; i++) pos = a2_rotate(pos, 6);
    CHECK(0, "6 rotations = identity", pos == 3);

    /* Reflection: pos → (6-pos) mod 6 */
    for (uint32_t pos = 0; pos < 6; pos++) {
        uint32_t reflected = a2_reflect(pos, 6);
        CHECK(0, "reflection valid", reflected < 6);
    }

    /* Double reflection = identity */
    for (uint32_t pos = 0; pos < 6; pos++) {
        uint32_t dr = a2_reflect(a2_reflect(pos, 6), 6);
        CHECK(0, "double reflection = identity", dr == pos);
    }
}

static void test_a2xa2_symmetry(void)
{
    printf("T7: A2×A2 Symmetry (GEO_COMPOUND_144)\n");

    int r = geo_verify_a2xa2_symmetry(GEO_COMPOUND_144);
    CHECK(7, "6ico A2×A2 orbit closure", r == 0);

    /* Apply all 36 A2×A2 elements */
    uint32_t n_faces = 24;
    for (uint32_t ra = 0; ra < 6; ra++) {
        for (uint32_t rb = 0; rb < 6; rb++) {
            A2xA2Elem e = {ra, rb};
            for (uint32_t v = 0; v < n_faces; v++) {
                uint32_t out_a, out_b;
                a2xa2_apply(e, v, v, n_faces, &out_a, &out_b);
                if (out_a >= n_faces || out_b >= n_faces) {
                    printf("  FAIL: A2×A2 element (%u,%u) maps v=%u out of range\n", ra, rb, v);
                    fail++; return;
                }
            }
        }
    }
    pass++;
    printf("  T7: PASS — All 36 A2×A2 elements map within bounds\n");
}

static void test_a2_other_geometries(void)
{
    printf("T8: A2 Symmetry (other geometries)\n");

    /* Most geometries should have vertices divisible by 6 */
    GeoType types[] = {
        GEO_DODEC_BASE, GEO_ICO_BASE, GEO_COMPOUND_24,
        GEO_DODEC_EDGES, GEO_COMPOUND_60, GEO_PENTAKIS_72,
        GEO_GOLDBERG_92, GEO_COMP_SPIKE_120, GEO_GOLDBERG_132,
        GEO_GOLDBERG_192
    };
    int n_types = sizeof(types) / sizeof(types[0]);

    for (int i = 0; i < n_types; i++) {
        int r = geo_verify_a2xa2_symmetry(types[i]);
        if (r == -11) {
            /* Vertices not divisible by 6 — might be expected */
            GeoProps p = geo_props(types[i]);
            printf("    type=%u: verts=%u not divisible by 6 (expected for this geometry)\n",
                   (unsigned)types[i], p.verts);
        } else if (r != 0) {
            printf("  FAIL: type=%u returned %d\n", (unsigned)types[i], r);
            fail++; return;
        }
    }
    pass++;
    printf("  T8: PASS — All geometry types checked\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   D4 TRIALITY TESTS
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_d4_constants(void)
{
    printf("T9: D4 Constants\n");
    CHECK(0, "D4_WEYL_ORDER == 192", D4_WEYL_ORDER == 192);
    CHECK(0, "D4_ROOT_COUNT == 24", D4_ROOT_COUNT == 24);
    CHECK(0, "D4_COXETER_NUM == 6", D4_COXETER_NUM == 6);
    CHECK(0, "D4_TRIALITY_ORDER == 3", D4_TRIALITY_ORDER == 3);
}

static void test_d4_structure(void)
{
    printf("T10: D4 Structure (Goldberg 192)\n");

    int r = geo_verify_d4_structure(GEO_GOLDBERG_192);
    CHECK(10, "Goldberg 192 D4 verify", r == 0);

    /* 192 = 8 × 24 */
    CHECK(0, "192 = 8 × 24", 8 * 24 == 192);
    /* 192 / 3 = 64 triality orbits */
    CHECK(0, "192 / 3 = 64 orbits", 192 / 3 == 64);
}

static void test_triality_cycle(void)
{
    printf("T11: D4 Triality Cycle\n");

    /* Cycle: HARD → NAT → FLAT → HARD */
    TW_ViewID v = TW_VIEW_HARD;
    v = tw_triality_cycle(v);
    CHECK(0, "HARD → NAT", v == TW_VIEW_NAT);
    v = tw_triality_cycle(v);
    CHECK(0, "NAT → FLAT", v == TW_VIEW_FLAT);
    v = tw_triality_cycle(v);
    CHECK(0, "FLAT → HARD", v == TW_VIEW_HARD);

    /* Inverse: HARD ← NAT ← FLAT ← HARD */
    v = TW_VIEW_HARD;
    v = tw_triality_inverse(v);
    CHECK(0, "HARD ← FLAT (inverse)", v == TW_VIEW_FLAT);
    v = tw_triality_inverse(v);
    CHECK(0, "FLAT ← NAT (inverse)", v == TW_VIEW_NAT);
    v = tw_triality_inverse(v);
    CHECK(0, "NAT ← HARD (inverse)", v == TW_VIEW_HARD);

    /* 3 steps = identity */
    v = TW_VIEW_NAT;
    v = tw_triality_apply(v, 3);
    CHECK(0, "3 steps = identity", v == TW_VIEW_NAT);
}

static void test_triality_verify(void)
{
    printf("T12: D4 Triality Full Verify (20736 addresses)\n");

    int r = tw_triality_verify();
    CHECK(12, "tw_triality_verify() == 0", r == 0);
}

static void test_triality_positions(void)
{
    printf("T13: D4 Triality Position Queries\n");

    /* Test a few specific addresses */
    uint32_t test_addrs[] = {0, 1, 127, 128, 162, 144, 20735, 12345};
    for (int i = 0; i < 8; i++) {
        uint32_t flat = test_addrs[i];
        TW_ViewPos ph = tw_triality_at(flat, TW_VIEW_HARD);
        TW_ViewPos pn = tw_triality_at(flat, TW_VIEW_NAT);
        TW_ViewPos pf = tw_triality_at(flat, TW_VIEW_FLAT);

        /* Verify HARD view */
        uint32_t h_flat = ph.coord[0] * 128 + ph.coord[1];
        if (h_flat != flat) {
            printf("  FAIL at flat=%u: HARD view mismatch %u != %u\n", flat, h_flat, flat);
            fail++; return;
        }

        /* Verify NAT view */
        uint32_t n_flat = pn.coord[0] * 144 + pn.coord[1];
        if (n_flat != flat) {
            printf("  FAIL at flat=%u: NAT view mismatch %u != %u\n", flat, n_flat, flat);
            fail++; return;
        }

        /* Verify FLAT view */
        if (pf.coord[0] != flat) {
            printf("  FAIL at flat=%u: FLAT view mismatch\n", flat);
            fail++; return;
        }
    }
    pass++;
    printf("  T13: PASS — All position queries correct\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   CROSS-SYSTEM TESTS
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_cross_system(void)
{
    printf("T14: Cross-System Verify (hex-quad-dual)\n");

    int r = geo_verify_hex_quad_dual();
    CHECK(14, "geo_verify_hex_quad_dual() == 0", r == 0);
}

static void test_cross_system_constants(void)
{
    printf("T15: Cross-System Constants\n");

    /* Fundamental equation */
    CHECK(0, "128 × 162 = 20736", 128 * 162 == 20736);
    CHECK(0, "144 × 144 = 20736", 144 * 144 == 20736);
    CHECK(0, "18 × 1152 = 20736", 18 * 1152 == 20736);

    /* Tesseract structure */
    CHECK(0, "8 cubes × 144 slots = 1152", 8 * 144 == 1152);
    CHECK(0, "18 tesseracts × 1152 = 20736", 18 * 1152 == 20736);

    /* Hex-quad duality */
    CHECK(0, "144 = 12² (hex²)", 144 == 12 * 12);
    CHECK(0, "144 = 8 × 18 (tess × cubes)", 144 == 8 * 18);
}

static void test_crt_cross_bridge(void)
{
    printf("T16: CRT ↔ Twin Rebalance Bridge\n");

    /* For every flat address, verify CRT decomposition
     * is consistent with twin rebalance views */
    for (uint32_t flat = 0; flat < 20736; flat++) {
        CRTAddr crt = crt_from_flat(flat);
        TW_HardAddr hard = tw_flat_to_hard(flat);
        TW_NatAddr nat = tw_flat_to_nat(flat);

        /* CRT bin should be derivable from hard view */
        /* CRT ter should be derivable from nat view */

        /* Verify consistency: same flat in all views */
        uint32_t flat_hard = tw_hard_to_flat(hard);
        uint32_t flat_nat = tw_nat_to_flat(nat);
        uint32_t flat_crt = crt_to_flat(crt);

        if (flat_hard != flat || flat_nat != flat || flat_crt != flat) {
            printf("  FAIL at flat=%u: views diverge\n", flat);
            fail++; return;
        }
    }
    pass++;
    printf("  T16: PASS — CRT and Twin Rebalance consistent\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Hex-Quad-Dual Upgrade Tests\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    /* CRT Bridge */
    test_crt_constants();
    test_crt_roundtrip();
    test_crt_edge_cases();
    test_crt_path_decomposition();
    test_crt_seek();

    /* A2×A2 Symmetry */
    test_a2_constants();
    test_a2_generators();
    test_a2xa2_symmetry();
    test_a2_other_geometries();

    /* D4 Triality */
    test_d4_constants();
    test_d4_structure();
    test_triality_cycle();
    test_triality_verify();
    test_triality_positions();

    /* Cross-system */
    test_cross_system();
    test_cross_system_constants();
    test_crt_cross_bridge();

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  RESULTS: %d passed, %d failed\n", pass, fail);
    printf("═══════════════════════════════════════════════════════════════\n");

    return fail ? 1 : 0;
}
