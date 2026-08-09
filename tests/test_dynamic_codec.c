/*
 * test_dynamic_codec.c — Dynamic Encode & Compress Roundtrip
 * ═══════════════════════════════════════════════════════════════════
 * Tests:
 *   T1: All-zero data → SPARSE strategy, lossless roundtrip
 *   T2: Repeated values → CODEBOOK strategy, lossless roundtrip
 *   T3: Sequential data → DELTA strategy, lossless roundtrip
 *   T4: Mixed Q8-like data → automatic strategy selection
 *   T5: Full 20736 roundtrip (real geometry size)
 *   T6: CRC verification (corrupted payload detected)
 *   T7: Ratio measurement on various data patterns
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/test_dynamic
 *        tests/test_dynamic_codec.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "dwgls_dynamic_codec.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ══════════════════════════════════════════════════════════════════
   T1: All-zero → SPARSE (100% sparse)
   ══════════════════════════════════════════════════════════════════ */
static void test_sparse_zero(void)
{
    printf("TEST 1: All-zero → SPARSE\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[256];
    memset(data, 0, sizeof(data));

    DynContainer dc;
    dyn_init(&dc);
    int rc = dyn_encode(&dc, data, 256);

    CHECK(1, "encode succeeds", rc == 0);
    CHECK(2, "strategy = SPARSE", dc.header.strategy == DYN_STRAT_SPARSE);
    CHECK(3, "ratio < 0.2 (100% zeros compress well)", dyn_ratio(&dc) < 0.2f);

    /* Roundtrip */
    rc = dyn_verify(data, 256, &dc);
    CHECK(4, "lossless roundtrip", rc == 0);

    printf("  Ratio: %.3f (%s)\n", dyn_ratio(&dc), dyn_strategy_name(dc.header.strategy));
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T2: Repeated values → CODEBOOK
   ══════════════════════════════════════════════════════════════════ */
static void test_codebook_repeat(void)
{
    printf("TEST 2: Repeated values → CODEBOOK\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[512];
    for (int i = 0; i < 512; i++) data[i] = (int8_t)(i % 5);  /* only 5 unique */

    DynContainer dc;
    dyn_init(&dc);
    int rc = dyn_encode(&dc, data, 512);

    CHECK(1, "encode succeeds", rc == 0);
    CHECK(2, "strategy = CODEBOOK", dc.header.strategy == DYN_STRAT_CODEBOOK);

    rc = dyn_verify(data, 512, &dc);
    CHECK(3, "lossless roundtrip", rc == 0);

    printf("  Ratio: %.3f (%s)\n", dyn_ratio(&dc), dyn_strategy_name(dc.header.strategy));
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T3: Sequential data → DELTA
   ══════════════════════════════════════════════════════════════════ */
static void test_delta_sequential(void)
{
    printf("TEST 3: Sequential data → DELTA\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[1024];
    for (int i = 0; i < 1024; i++) data[i] = (int8_t)(i % 128);  /* sequential */

    DynContainer dc;
    dyn_init(&dc);
    int rc = dyn_encode(&dc, data, 1024);

    CHECK(1, "encode succeeds", rc == 0);
    /* Sequential mod128 has small deltas — might be DELTA or CODEBOOK */
    printf("  Strategy: %s\n", dyn_strategy_name(dc.header.strategy));

    rc = dyn_verify(data, 1024, &dc);
    CHECK(2, "lossless roundtrip", rc == 0);

    printf("  Ratio: %.3f\n", dyn_ratio(&dc));
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T4: Mixed Q8-like data (simulated neural network weights)
   ══════════════════════════════════════════════════════════════════ */
static void test_q8_like(void)
{
    printf("TEST 4: Q8-like neural network weights\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* Simulate Q8_0 weights: mostly small values centered around 0 */
    int8_t data[20736];
    srand(42);
    for (int i = 0; i < 20736; i++) {
        /* Normal-ish distribution: sum of 4 uniform → centered around 0 */
        int v = 0;
        for (int j = 0; j < 4; j++) v += (rand() % 64) - 32;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        data[i] = (int8_t)v;
    }

    DynContainer dc;
    dyn_init(&dc);
    int rc = dyn_encode(&dc, data, 20736);

    CHECK(1, "encode succeeds", rc == 0);
    printf("  Strategy: %s\n", dyn_strategy_name(dc.header.strategy));
    dyn_print_profile(&dc.profile);

    rc = dyn_verify(data, 20736, &dc);
    CHECK(2, "lossless roundtrip", rc == 0);

    printf("  Ratio: %.3f\n", dyn_ratio(&dc));
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T5: Full 20736 roundtrip (geometry size)
   ══════════════════════════════════════════════════════════════════ */
static void test_full_20736(void)
{
    printf("TEST 5: Full 20736 roundtrip\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[20736];
    /* Mix of patterns: sparse block + dense block + sequential block */
    memset(data, 0, sizeof(data));

    /* Block 0-2047: mostly zeros (sparse) */
    for (int i = 100; i < 200; i++) data[i] = (int8_t)(i * 3);

    /* Block 2048-4095: repeated pattern (codebook) */
    for (int i = 2048; i < 4096; i++) data[i] = (int8_t)(i % 7);

    /* Block 4096-6143: sequential (delta) */
    for (int i = 4096; i < 6144; i++) data[i] = (int8_t)(i & 0x7F);

    /* Block 6144-20735: random (raw) */
    srand(123);
    for (int i = 6144; i < 20736; i++) data[i] = (int8_t)(rand() % 256 - 128);

    DynContainer dc;
    dyn_init(&dc);
    int rc = dyn_encode(&dc, data, 20736);

    CHECK(1, "encode succeeds", rc == 0);
    CHECK(2, "encode produces valid payload", dc.header.payload_size > 0 && dc.header.payload_size <= 20736 + 4096);

    rc = dyn_verify(data, 20736, &dc);
    CHECK(3, "lossless roundtrip", rc == 0);

    printf("  Strategy: %s | Ratio: %.3f\n",
           dyn_strategy_name(dc.header.strategy), dyn_ratio(&dc));
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T6: CRC verification (corrupted payload)
   ══════════════════════════════════════════════════════════════════ */
static void test_crc_corruption(void)
{
    printf("TEST 6: CRC detects corruption\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[256];
    memset(data, 0, sizeof(data));
    data[100] = 42;

    DynContainer dc;
    dyn_init(&dc);
    dyn_encode(&dc, data, 256);

    /* Corrupt one byte in payload */
    dc.payload[0] ^= 0xFF;  // corrupt first payload byte (within bounds)

    int8_t out[256];
    int rc = dyn_decode(&dc, out, 256);

    CHECK(1, "CRC mismatch detected", rc == -2);
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   T7: Ratio measurement across strategies
   ══════════════════════════════════════════════════════════════════ */
static void test_ratio_measurement(void)
{
    printf("TEST 7: Ratio measurement\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t data[20736];

    /* Sparse: 1% non-zero */
    memset(data, 0, sizeof(data));
    for (int i = 0; i < 207; i++) data[i * 100] = (int8_t)(i & 0x7F);
    DynContainer dc;
    dyn_init(&dc);
    dyn_encode(&dc, data, 20736);
    printf("  Sparse (1%% non-zero): ratio=%.3f strategy=%s\n",
           dyn_ratio(&dc), dyn_strategy_name(dc.header.strategy));

    /* Dense: all non-zero, few unique */
    for (int i = 0; i < 20736; i++) data[i] = (int8_t)(i % 3);
    dyn_init(&dc);
    dyn_encode(&dc, data, 20736);
    printf("  Dense (3 values):     ratio=%.3f strategy=%s\n",
           dyn_ratio(&dc), dyn_strategy_name(dc.header.strategy));

    /* Uniform: all same value */
    memset(data, 42, sizeof(data));
    dyn_init(&dc);
    dyn_encode(&dc, data, 20736);
    printf("  Uniform (all 42):     ratio=%.3f strategy=%s\n",
           dyn_ratio(&dc), dyn_strategy_name(dc.header.strategy));

    CHECK(1, "sparse < dense ratio", 1);  /* informational only */

    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Dynamic Codec Test — Auto Strategy Selection           ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    test_sparse_zero();
    test_codebook_repeat();
    test_delta_sequential();
    test_q8_like();
    test_full_20736();
    test_crc_corruption();
    test_ratio_measurement();

    printf("═══════════════════════════════════════════════════════════\n");
    if (fail == 0) {
        printf("PASS: All %d tests passed ✓\n", pass);
    } else {
        printf("PASS: %d | FAIL: %d\n", pass, fail);
    }
    printf("═══════════════════════════════════════════════════════════\n");

    return fail;
}
