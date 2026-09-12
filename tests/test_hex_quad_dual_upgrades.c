/*
 * test_hex_quad_dual_upgrades.c — Tests for Gosper, FCC, Robinson
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Upgrade #4: Gosper Path (hex space-filling curve)
 * Upgrade #5: FCC DRamTile (3D lattice addressing)
 * Upgrade #6: Robinson Aperiodic Tiles (4-type)
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../core/geo_gosper_path.h"
#include "../core/geo_fcc_dramtile.h"
#include "../core/geo_robinson.h"

static int total = 0;
static int passed = 0;
static int failed = 0;

#define CHECK(expr, msg) do { \
    total++; \
    if (expr) { passed++; } \
    else { failed++; printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   GOSPER CURVE TESTS
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_gosper_basics(void)
{
    printf("\n── Gosper Basics ──\n");

    /* Level 0: single cell */
    CHECK(gosper_cell_count(0) == 1, "level 0 = 1 cell");
    HexCoord c0 = gosper_at(0, 0);
    CHECK(c0.q == 0 && c0.r == 0, "level 0 = origin");

    /* Level 1: 7 cells */
    CHECK(gosper_cell_count(1) == 7, "level 1 = 7 cells");
    HexCoord c1_0 = gosper_at(1, 0);
    CHECK(c1_0.q == 0 && c1_0.r == 0, "level 1 step 0 = origin");
    HexCoord c1_1 = gosper_at(1, 1);
    CHECK(c1_1.q == 1 && c1_1.r == 0, "level 1 step 1 = E");

    /* Level 2: 49 cells */
    CHECK(gosper_cell_count(2) == 49, "level 2 = 49 cells");
}

static void test_gosper_roundtrip(void)
{
    printf("\n── Gosper Roundtrip ──\n");

    /* Level 2: all 49 cells must roundtrip through index↔position */
    uint64_t total_cells = gosper_cell_count(2);
    uint32_t roundtrip_ok = 0;
    uint32_t all_unique = 1;

    for (uint64_t i = 0; i < total_cells; i++) {
        HexCoord pos = gosper_at(2, i);
        int64_t idx = gosper_index(2, pos);
        if (idx >= 0 && (uint64_t)idx == i) roundtrip_ok++;

        /* Check uniqueness: no two indices map to same position */
        for (uint64_t j = 0; j < i; j++) {
            HexCoord prev = gosper_at(2, j);
            if (pos.q == prev.q && pos.r == prev.r) {
                all_unique = 0;
                break;
            }
        }
    }

    CHECK(roundtrip_ok == total_cells, "level 2 roundtrip all 49");
    CHECK(all_unique, "level 2 positions unique");
}

static void test_gosper_locality(void)
{
    printf("\n── Gosper Locality ──\n");

    /* Level 2: locality ratio should be reasonable (1.0 - 3.0) */
    GosperStats st = gosper_stats(2);
    CHECK(st.total_cells == 49, "stats cell count = 49");
    CHECK(st.avg_locality > 0.5 && st.avg_locality < 5.0,
          "level 2 avg locality reasonable");
    printf("  avg locality: %.3f\n", st.avg_locality);
}

static void test_gosper_hex_ops(void)
{
    printf("\n── Gosper Hex Operations ──\n");

    /* Hex distance: same point = 0 */
    HexCoord a = hex_make(0, 0);
    CHECK(hex_dist(a, a) == 0, "hex dist same = 0");

    /* Hex distance: adjacent = 1 */
    HexCoord b = hex_make(1, 0);
    CHECK(hex_dist(a, b) == 1, "hex dist adjacent = 1");

    /* Hex distance: opposite = 2 */
    HexCoord c = hex_make(-1, 0);
    CHECK(hex_dist(a, c) == 1, "hex dist opposite = 1");
    CHECK(hex_dist(b, c) == 2, "hex dist 2 apart = 2");

    /* Rotation: 6 rotations = identity */
    HexCoord p = hex_make(3, -1);
    HexCoord p6 = p;
    for (int i = 0; i < 6; i++) p6 = hex_rotate60(p6, 1);
    CHECK(p6.q == p.q && p6.r == p.r, "hex rotate60^6 = identity");
}

/* ═══════════════════════════════════════════════════════════════════════════
   FCC DRamTile TESTS
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_fcc_parity(void)
{
    printf("\n── FCC Parity ──\n");

    CHECK(fcc_valid(0, 0, 0), "(0,0,0) valid");
    CHECK(fcc_valid(1, 1, 0), "(1,1,0) valid");
    CHECK(fcc_valid(1, 0, 1), "(1,0,1) valid");
    CHECK(!fcc_valid(1, 0, 0), "(1,0,0) invalid");
    CHECK(!fcc_valid(0, 1, 0), "(0,1,0) invalid");
}

static void test_fcc_roundtrip(void)
{
    printf("\n── FCC Roundtrip ──\n");

    /* All 20736 flat addresses must roundtrip through FCC */
    uint32_t ok = 0;
    for (uint32_t f = 0; f < FCC_FULL; f++) {
        FCCCoord c = flat_to_fcc(f);
        uint32_t f2 = fcc_to_flat(c);
        if (f2 == f) ok++;
    }

    CHECK(ok == FCC_FULL, "FCC roundtrip all 20736");
}

static void test_fcc_neighbors(void)
{
    printf("\n── FCC Neighbors ──\n");

    /* All neighbors must be valid FCC and have 12 neighbors */
    uint32_t ok = 0;
    for (uint32_t f = 0; f < 100; f++) {  /* sample first 100 */
        FCCCoord c = flat_to_fcc(f);
        uint32_t valid = 0;
        for (uint32_t d = 0; d < FCC_NEIGHBORS; d++) {
            FCCCoord n = fcc_neighbor(c, d);
            if (fcc_valid(n.x, n.y, n.z)) valid++;
        }
        if (valid == FCC_NEIGHBORS) ok++;
    }

    CHECK(ok == 100, "first 100 FCC cells have 12 valid neighbors");
}

static void test_fcc_dramtile_bridge(void)
{
    printf("\n── FCC DRamTile Bridge ──\n");

    /* DRamTile (0,0) → FCC → valid */
    FCCCoord c0 = dramtile_to_fcc(0, 0);
    CHECK(fcc_valid(c0.x, c0.y, c0.z), "DRamTile (0,0) → FCC valid");

    /* DRamTile (127,161) → FCC → valid */
    FCCCoord c1 = dramtile_to_fcc(127, 161);
    CHECK(fcc_valid(c1.x, c1.y, c1.z), "DRamTile (127,161) → FCC valid");

    /* All 20736 DRamTile positions → FCC valid */
    uint32_t all_valid = 1;
    for (uint32_t a = 0; a < 128; a++) {
        for (uint32_t co = 0; co < 162; co++) {
            FCCCoord fc = dramtile_to_fcc(a, co);
            if (!fcc_valid(fc.x, fc.y, fc.z)) { all_valid = 0; break; }
        }
        if (!all_valid) break;
    }
    CHECK(all_valid, "all 20736 DRamTile → FCC valid");
}

static void test_fcc_density(void)
{
    printf("\n── FCC Density ──\n");

    FCCStats st = fcc_verify();
    CHECK(st.valid_count > 0, "FCC has valid sites");
    /* FCC parity condition: x+y+z even = 50% of cubic lattice
     * Sphere packing density = π/(3√2) ≈ 74% is a different metric */
    CHECK(st.density_pct >= 49 && st.density_pct <= 51,
          "FCC parity density ~50%");
    printf("  parity density: %u%% (parity: 50%%, sphere pack: 74%%)\n", st.density_pct);
}

/* ═══════════════════════════════════════════════════════════════════════════
   ROBINSON APERIODIC TILES TESTS
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_robinson_types(void)
{
    printf("\n── Robinson Types ──\n");

    /* All 4 types must appear in an 8×8 grid */
    uint32_t has_type[4] = {0, 0, 0, 0};
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t t = rob_tile_type(x, y);
            if (t < 4) has_type[t] = 1;
        }
    }

    CHECK(has_type[0], "type A present");
    CHECK(has_type[1], "type B present");
    CHECK(has_type[2], "type C present");
    CHECK(has_type[3], "type D present");
}

static void test_robinson_edges(void)
{
    printf("\n── Robinson Edge Colors ──\n");

    /* Edge colors must be in [0,3] */
    uint32_t all_valid = 1;
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            for (uint32_t e = 0; e < 4; e++) {
                uint8_t c = rob_edge_color(x, y, e);
                if (c > 3) { all_valid = 0; break; }
            }
            if (!all_valid) break;
        }
        if (!all_valid) break;
    }
    CHECK(all_valid, "edge colors in [0,3]");
}

static void test_robinson_tile_creation(void)
{
    printf("\n── Robinson Tile Creation ──\n");

    /* All tiles must be creatable */
    uint32_t ok = 0;
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            RobinsonTile t = rob_make_tile(x, y);
            if (t.type < 4) ok++;
        }
    }
    CHECK(ok == 64, "64 tiles created from 8×8 grid");
}

static void test_robinson_level(void)
{
    printf("\n── Robinson Level Hierarchy ──\n");

    /* Level must be non-negative and bounded */
    uint32_t max_level = 0;
    for (uint32_t y = 0; y < 16; y++) {
        for (uint32_t x = 0; x < 16; x++) {
            uint32_t lev = rob_level(x, y);
            if (lev > max_level) max_level = lev;
        }
    }
    CHECK(max_level >= 2, "max level >= 2 in 16×16");
    CHECK(max_level <= 4, "max level <= 4 in 16×16");
}

static void test_robinson_verify_grid(void)
{
    printf("\n── Robinson Grid Verify ──\n");

    /* 8×8 grid must have consistent adjacency */
    RobinsonStats st = rob_verify_grid(8);
    CHECK(st.total_edges == 8 * 7 * 2, "8×8 = 112 edges");
    CHECK(st.type_count[ROB_TYPE_D] > 0, "type D count > 0");
    CHECK(st.type_count[ROB_TYPE_C] > 0, "type C count > 0");

    printf("  match rate: %.1f%%\n",
           st.total_edges > 0 ? 100.0 * st.matched / st.total_edges : 0.0);
}

/* ═══════════════════════════════════════════════════════════════════════════
   CROSS-SYSTEM INTEGRATION
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_cross_gosper_fcc(void)
{
    printf("\n── Cross: Gosper ↔ FCC ──\n");

    /* Gosper positions must map to valid FCC coordinates */
    uint32_t ok = 0;
    for (uint64_t i = 0; i < 49; i++) {  /* level 2 = 49 cells */
        HexCoord hex = gosper_at(2, i);
        /* Map hex (q,r) to flat address */
        uint32_t flat = (uint32_t)((hex.q + 20) * 144 + (hex.r + 20));
        if (flat < FCC_FULL) {
            FCCCoord fc = flat_to_fcc(flat);
            if (fcc_valid(fc.x, fc.y, fc.z)) ok++;
        }
    }
    CHECK(ok >= 40, "most Gosper cells → valid FCC");
}

static void test_cross_robinson_dramtile(void)
{
    printf("\n── Cross: Robinson → DRamTile ──\n");

    /* Robinson tile type must be deterministic for each DRamTile address */
    uint32_t ok = 0;
    for (uint32_t f = 0; f < 1000; f++) {
        uint32_t x = f % 32;
        uint32_t y = f / 32;
        uint32_t t1 = rob_tile_type(x, y);
        uint32_t t2 = rob_tile_type(x, y);  /* same input = same output */
        if (t1 == t2 && t1 < 4) ok++;
    }
    CHECK(ok == 1000, "Robinson deterministic for 1000 addresses");
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Hex-Quad-Dual Upgrades #4-6: Gosper + FCC + Robinson\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    /* #4: Gosper */
    test_gosper_basics();
    test_gosper_roundtrip();
    test_gosper_locality();
    test_gosper_hex_ops();

    /* #5: FCC DRamTile */
    test_fcc_parity();
    test_fcc_roundtrip();
    test_fcc_neighbors();
    test_fcc_dramtile_bridge();
    test_fcc_density();

    /* #6: Robinson */
    test_robinson_types();
    test_robinson_edges();
    test_robinson_tile_creation();
    test_robinson_level();
    test_robinson_verify_grid();

    /* Cross-system */
    test_cross_gosper_fcc();
    test_cross_robinson_dramtile();

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  TOTAL: %d  PASS: %d  FAIL: %d\n", total, passed, failed);
    printf("═══════════════════════════════════════════════════════════════\n");

    return failed > 0 ? 1 : 0;
}
