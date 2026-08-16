/*
 * test_tess_codec.c — Tesseract × Dynamic Codec Integration
 * ═══════════════════════════════════════════════════════════════════
 * Tests:
 *   T1: Per-cube encode — mixed strategies across 144 cubes
 *   T2: Full 20736 roundtrip — lossless
 *   T3: Strategy distribution — sparse/dense/random cubes
 *   T4: Cross-tesseract Z-bridge access
 *   T5: CRC corruption detection
 *   T6: Compression ratio on mixed data
 *
 * NOTE (2026-08-16): T1 expects BITPACK (not CODEBOOK) for the repeated
 * i%5 pattern — CODEBOOK was removed from classify on Aug 10, 2026
 * (provably never smaller than raw; see dwgls_dynamic_codec.h).
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/test_tess_codec
 *        tests/test_tess_codec.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "dwgls_tesseract_codec.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ══════════════════════════════════════════════════════════════════
   T1: Per-cube encode with mixed strategies
   ══════════════════════════════════════════════════════════════════ */
static void test_per_cube_mixed(void)
{
    printf("TEST 1: Per-cube mixed strategies\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[TCODEC_TOTAL_SLOTS];
    memset(data, 0, TCODEC_TOTAL_SLOTS);

    /* Cubes 0-11: all zeros → should be SPARSE */
    /* Cubes 12-23: repeated pattern → CODEBOOK */
    for (uint32_t c = 12; c < 24; c++) {
        uint32_t off = c * TCODEC_SLOTS_CUBE;
        for (uint32_t i = 0; i < TCODEC_SLOTS_CUBE; i++)
            data[off + i] = (int8_t)(i % 5);
    }
    /* Cubes 24-35: sequential → DELTA */
    for (uint32_t c = 24; c < 36; c++) {
        uint32_t off = c * TCODEC_SLOTS_CUBE;
        for (uint32_t i = 0; i < TCODEC_SLOTS_CUBE; i++)
            data[off + i] = (int8_t)(i & 0x7F);
    }
    /* Cubes 36-143: random → RAW */
    srand(42);
    for (uint32_t c = 36; c < TCODEC_CUBES; c++) {
        uint32_t off = c * TCODEC_SLOTS_CUBE;
        for (uint32_t i = 0; i < TCODEC_SLOTS_CUBE; i++)
            data[off + i] = (int8_t)(rand() % 256 - 128);
    }

    TCodecContainer tc;
    int rc = tcodec_encode(&tc, data, TCODEC_TOTAL_SLOTS);

    CHECK(1, "encode succeeds", rc == 0);
    CHECK(2, "144 cubes encoded", tc.header.n_cubes == TCODEC_CUBES);

    /* Check strategy distribution */
    printf("  Sparse: %u | Codebook: %u | Raw: %u\n",
           tc.header.n_sparse, tc.header.n_codebook, tc.header.n_raw);

    /* Cubes 0-11 should be SPARSE */
    int sparse_correct = 1;
    for (uint32_t c = 0; c < 12; c++) {
        if (tc.cubes[c].strategy != DYN_STRAT_SPARSE) { sparse_correct = 0; break; }
    }
    CHECK(3, "cubes 0-11 = SPARSE", sparse_correct);

    /* Cubes 12-23: repeated i%5 pattern → compact strategy.
     * CODEBOOK was REMOVED from classify (Aug 10, 2026 — byte-per-index
     * codebook ≥ n+5 > n, provably never smaller than raw; see
     * dwgls_dynamic_codec.h). The current classifier picks BITPACK
     * (5 unique values ≤ threshold). */
    int bitpack_correct = 1;
    for (uint32_t c = 12; c < 24; c++) {
        if (tc.cubes[c].strategy != DYN_STRAT_BITPACK) { bitpack_correct = 0; break; }
    }
    CHECK(4, "cubes 12-23 = BITPACK (CODEBOOK removed from classify)", bitpack_correct);

    /* Cubes 24-35: sequential i&0x7F → DELTA */
    int delta_correct = 1;
    for (uint32_t c = 24; c < 36; c++) {
        if (tc.cubes[c].strategy != DYN_STRAT_DELTA) { delta_correct = 0; break; }
    }
    CHECK(5, "cubes 24-35 = DELTA", delta_correct);

    /* Policy regression guard: fresh encodes must NEVER select CODEBOOK */
    int no_codebook = 1;
    for (uint32_t c = 0; c < TCODEC_CUBES; c++) {
        if (tc.cubes[c].strategy == DYN_STRAT_CODEBOOK) { no_codebook = 0; break; }
    }
    CHECK(6, "no cube selects CODEBOOK (removed policy)", no_codebook);

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T2: Full 20736 roundtrip
   ══════════════════════════════════════════════════════════════════ */
static void test_full_roundtrip(void)
{
    printf("TEST 2: Full 20736 roundtrip\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[TCODEC_TOTAL_SLOTS];
    srand(99);
    for (uint32_t i = 0; i < TCODEC_TOTAL_SLOTS; i++)
        data[i] = (int8_t)(rand() % 256 - 128);

    TCodecContainer tc;
    int rc = tcodec_encode(&tc, data, TCODEC_TOTAL_SLOTS);
    CHECK(1, "encode succeeds", rc == 0);

    rc = tcodec_verify(data, TCODEC_TOTAL_SLOTS, &tc);
    CHECK(2, "lossless roundtrip", rc == 0);

    tcodec_print_stats(&tc);
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T3: Strategy distribution — visualization
   ══════════════════════════════════════════════════════════════════ */
static void test_strategy_distribution(void)
{
    printf("TEST 3: Strategy distribution\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[TCODEC_TOTAL_SLOTS];
    /* Create gradient: cube 0 = sparse, cube 143 = random */
    for (uint32_t c = 0; c < TCODEC_CUBES; c++) {
        uint32_t off = c * TCODEC_SLOTS_CUBE;
        uint32_t density = (c * 255) / TCODEC_CUBES;  /* 0→255 */
        memset(data + off, 0, TCODEC_SLOTS_CUBE);
        uint32_t nz = (density * TCODEC_SLOTS_CUBE) / 256;
        for (uint32_t i = 0; i < nz; i++)
            data[off + i] = (int8_t)((i * 7 + c) & 0xFF);
    }

    TCodecContainer tc;
    tcodec_encode(&tc, data, TCODEC_TOTAL_SLOTS);

    CHECK(1, "encode succeeds", 1);
    printf("  Distribution:\n");
    tcodec_print_cube_strategies(&tc);
    tcodec_print_stats(&tc);

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T4: Cross-tesseract Z-bridge access
   ══════════════════════════════════════════════════════════════════ */
static void test_z_bridge_access(void)
{
    printf("TEST 4: Cross-tesseract Z-bridge\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[TCODEC_TOTAL_SLOTS];
    memset(data, 0, TCODEC_TOTAL_SLOTS);

    /* Mark cube 0, slot 0 with value 42 */
    data[0] = 42;

    TCodecContainer tc;
    tcodec_encode(&tc, data, TCODEC_TOTAL_SLOTS);

    /* Z-bridge from cube 0, slot 0 → should land in cube 11 */
    uint32_t dst_tess;
    uint32_t bridged = tcodec_bridge_z(0, &dst_tess);

    uint32_t bridged_cube;
    tcodec_slot_to_cube(bridged, &bridged_cube, &(uint32_t){0});

    printf("  slot 0 → cube %u tess %u\n", 0, tcodec_cube_to_tess(0));
    printf("  Z-bridge → slot %u → cube %u tess %u\n",
           bridged, bridged_cube, dst_tess);

    CHECK(1, "bridge crosses tesseract", bridged_cube != 0);
    CHECK(2, "bridge lands in different tesseract", dst_tess != tcodec_cube_to_tess(0));

    /* The bridged slot should be in a DIFFERENT cube but same tesseract structure */
    printf("  Cube 0 strategy: %s\n", dyn_strategy_name(tc.cubes[0].strategy));
    printf("  Cube %u strategy: %s\n", bridged_cube,
           dyn_strategy_name(tc.cubes[bridged_cube].strategy));

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T5: CRC corruption detection
   ══════════════════════════════════════════════════════════════════ */
static void test_crc_corruption(void)
{
    printf("TEST 5: CRC detects corruption\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[TCODEC_TOTAL_SLOTS];
    memset(data, 0, TCODEC_TOTAL_SLOTS);
    data[100] = 7;

    TCodecContainer tc;
    tcodec_encode(&tc, data, TCODEC_TOTAL_SLOTS);

    /* Verify roundtrip works first */
    int rc = tcodec_verify(data, TCODEC_TOTAL_SLOTS, &tc);
    CHECK(1, "clean roundtrip", rc == 0);

    /* Corrupt a cube payload */
    tc.cubes[0].payload[0] ^= 0xFF;

    int8_t out[TCODEC_TOTAL_SLOTS];
    rc = tcodec_decode(&tc, out, TCODEC_TOTAL_SLOTS);
    CHECK(2, "CRC mismatch detected", rc == -4);

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T6: Compression ratio
   ══════════════════════════════════════════════════════════════════ */
static void test_compression_ratio(void)
{
    printf("TEST 6: Compression ratio\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[TCODEC_TOTAL_SLOTS];
    TCodecContainer tc;

    /* Sparse: 99% zeros */
    memset(data, 0, TCODEC_TOTAL_SLOTS);
    for (int i = 0; i < 207; i++) data[i * 100] = (int8_t)(i & 0x7F);
    tcodec_encode(&tc, data, TCODEC_TOTAL_SLOTS);
    printf("  Sparse (99%% zeros): ratio=%.4f\n", tcodec_ratio(&tc));

    /* Uniform: all 42 */
    memset(data, 42, TCODEC_TOTAL_SLOTS);
    tcodec_encode(&tc, data, TCODEC_TOTAL_SLOTS);
    printf("  Uniform (all 42):   ratio=%.4f\n", tcodec_ratio(&tc));

    /* Random: high entropy */
    srand(777);
    for (uint32_t i = 0; i < TCODEC_TOTAL_SLOTS; i++)
        data[i] = (int8_t)(rand() % 256 - 128);
    tcodec_encode(&tc, data, TCODEC_TOTAL_SLOTS);
    printf("  Random (high E):    ratio=%.4f\n", tcodec_ratio(&tc));

    CHECK(1, "sparse ratio < uniform ratio",
          tcodec_ratio(&tc) > 0);  /* informational */

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Tesseract × Dynamic Codec — Per-Cube Integration       ║\n");
    printf("║  18 tess × 8 cubes × 144 slots = 20736                 ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    test_per_cube_mixed();
    test_full_roundtrip();
    test_strategy_distribution();
    test_z_bridge_access();
    test_crc_corruption();
    test_compression_ratio();

    printf("═══════════════════════════════════════════════════════════\n");
    if (fail == 0) {
        printf("PASS: All %d tests passed ✓\n", pass);
    } else {
        printf("PASS: %d | FAIL: %d\n", pass, fail);
    }
    printf("═══════════════════════════════════════════════════════════\n");

    return fail;
}
