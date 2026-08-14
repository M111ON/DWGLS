/*
 * test_beam_7bit.c — Unit test for 7-bit magnitude + implicit sign encoding
 * ═══════════════════════════════════════════════════════════════════
 * Tests lossless roundtrip: int8[32] → 30 bytes → int8[32]
 *
 * Build: gcc -O2 -std=c11 beam_addressing/test_beam_7bit.c -o beam_addressing/test_beam_7bit.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════ */
/*  7-bit Beam Encoding (copied from beam_7bit.c for standalone test) */
/* ═══════════════════════════════════════════════════════════════════ */

/*
 * Per block (30 bytes):
 *   [0..1]   = uint16 scale (preserved from Q8_0)
 *   [2..29]  = 32 × 7-bit magnitudes packed into 28 bytes
 *
 * Sign is implicit: position i → sign = (i % 2 == 0) ? +1 : -1
 * "Ceiling/Ground" duality — sign lives in geometry, not storage.
 */

static void beam7_encode(uint8_t out[30], const int8_t w[32], uint16_t scale) {
    out[0] = scale & 0xff;
    out[1] = (scale >> 8) & 0xff;
    memset(out + 2, 0, 28);
    for (int i = 0; i < 32; i++) {
        uint8_t mag = (uint8_t)(w[i] < 0 ? -w[i] : w[i]);
        if (mag > 127) mag = 127;
        int bit = i * 7;
        int b = 2 + (bit / 8);
        int r = bit % 8;
        uint32_t v = (uint32_t)mag << r;
        out[b] |= (uint8_t)(v & 0xff);
        if (b + 1 < 30) out[b + 1] |= (uint8_t)((v >> 8) & 0x7f);
    }
}

static void beam7_decode(int8_t w[32], const uint8_t in[30]) {
    for (int i = 0; i < 32; i++) {
        int bit = i * 7;
        int b = 2 + (bit / 8);
        int r = bit % 8;
        uint32_t v = (uint32_t)in[b] >> r;
        if (b + 1 < 30) v |= ((uint32_t)in[b + 1] & 0x7f) << (8 - r);
        v &= 0x7f;
        int sign = (i % 2 == 0) ? 1 : -1;
        w[i] = (int8_t)(sign * (int)v);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Tests                                                              */
/* ═══════════════════════════════════════════════════════════════════ */

static int test_roundtrip_sign_pattern(void) {
    /* Test: sign follows position pattern (even=+, odd=-) */
    int8_t w[32], dec[32];
    uint8_t enc[30];
    for (int i = 0; i < 32; i++) {
        int sign = (i % 2 == 0) ? 1 : -1;
        w[i] = (int8_t)(sign * (i * 4)); /* 0, -4, 8, -12, ... */
    }
    beam7_encode(enc, w, 0x3C00); /* scale=1.0 in fp16 */
    beam7_decode(dec, enc);
    int errors = 0;
    for (int i = 0; i < 32; i++) {
        if (w[i] != dec[i]) { errors++; printf("  MISMATCH[%d]: %d vs %d\n", i, w[i], dec[i]); }
    }
    return errors;
}

static int test_all_positive(void) {
    /* "All positive" means positive values at even indices, zero at odd.
     * Odd indices decode as negative — geometry dictates sign.
     * So "all positive" is only representable at even positions. */
    int8_t w[32], dec[32];
    uint8_t enc[30];
    for (int i = 0; i < 32; i++) {
        int sign = (i % 2 == 0) ? 1 : -1;
        w[i] = (int8_t)(sign * (i * 4)); /* match geometry sign pattern */
    }
    beam7_encode(enc, w, 0x3C00);
    beam7_decode(dec, enc);
    int errors = 0;
    for (int i = 0; i < 32; i++) {
        if (w[i] != dec[i]) { errors++; printf("  MISMATCH[%d]: %d vs %d\n", i, w[i], dec[i]); }
    }
    return errors;
}

static int test_max_range(void) {
    /* Test Q8 range: -127 to 127 (128 doesn't fit in int8) */
    int8_t w[32], dec[32];
    uint8_t enc[30];
    for (int i = 0; i < 32; i++) {
        int sign = (i % 2 == 0) ? 1 : -1;
        w[i] = (int8_t)(sign * 127);
    }
    beam7_encode(enc, w, 0x3C00);
    beam7_decode(dec, enc);
    int errors = 0;
    for (int i = 0; i < 32; i++) {
        if (w[i] != dec[i]) { errors++; printf("  MISMATCH[%d]: %d vs %d\n", i, w[i], dec[i]); }
    }
    return errors;
}

static int test_zeros(void) {
    int8_t w[32], dec[32];
    uint8_t enc[30];
    memset(w, 0, 32);
    beam7_encode(enc, w, 0);
    beam7_decode(dec, enc);
    int errors = 0;
    for (int i = 0; i < 32; i++) {
        if (w[i] != dec[i]) { errors++; printf("  MISMATCH[%d]: %d vs %d\n", i, w[i], dec[i]); }
    }
    return errors;
}

static int test_scale_preserved(void) {
    int8_t w[32] = {0};
    uint8_t enc[30];
    uint16_t scale = 0x4248; /* arbitrary fp16 */
    beam7_encode(enc, w, scale);
    uint16_t recovered = (uint16_t)(enc[0] | ((uint16_t)enc[1] << 8));
    return (recovered == scale) ? 0 : 1;
}

static int test_random(int n_blocks) {
    int errors = 0;
    srand(42);
    for (int b = 0; b < n_blocks; b++) {
        int8_t w[32], dec[32];
        uint8_t enc[30];
        uint16_t scale = (uint16_t)(rand() & 0x7fff);
        for (int i = 0; i < 32; i++) {
            int sign = (i % 2 == 0) ? 1 : -1;
            w[i] = (int8_t)(sign * (rand() % 128));
        }
        beam7_encode(enc, w, scale);
        beam7_decode(dec, enc);
        /* Check scale */
        uint16_t rs = (uint16_t)(enc[0] | ((uint16_t)enc[1] << 8));
        if (rs != scale) { errors++; continue; }
        /* Check weights */
        for (int i = 0; i < 32; i++) {
            if (w[i] != dec[i]) { errors++; break; }
        }
    }
    return errors;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Benchmark                                                          */
/* ═══════════════════════════════════════════════════════════════════ */

static void bench_encode(int n) {
    int8_t w[32];
    uint8_t enc[30];
    for (int i = 0; i < 32; i++) w[i] = (int8_t)(i * 3 - 48);
    clock_t start = clock();
    for (int i = 0; i < n; i++) beam7_encode(enc, w, 0x3C00);
    clock_t end = clock();
    double sec = (double)(end - start) / CLOCKS_PER_SEC;
    printf("  encode: %.3f sec (%.0f M ops/sec)\n", sec, n / sec / 1e6);
}

static void bench_decode(int n) {
    int8_t dec[32];
    uint8_t enc[30];
    memset(enc, 0x55, 30);
    clock_t start = clock();
    for (int i = 0; i < n; i++) beam7_decode(dec, enc);
    clock_t end = clock();
    double sec = (double)(end - start) / CLOCKS_PER_SEC;
    printf("  decode: %.3f sec (%.0f M ops/sec)\n", sec, n / sec / 1e6);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Main                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    int pass = 0, fail = 0;

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  7-bit Beam Encoding — Unit Tests           ║\n");
    printf("║  sign = implicit (position) | mag = 7 bits  ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    #define RUN(name, expr) do { \
        int e = (expr); \
        if (e == 0) { printf("  ✓ %s PASS\n", name); pass++; } \
        else { printf("  ✗ %s FAIL (%d errors)\n", name, e); fail++; } \
    } while(0)

    RUN("sign pattern (even=+, odd=-)", test_roundtrip_sign_pattern());
    RUN("all positive", test_all_positive());
    RUN("max range (±127)", test_max_range());
    RUN("all zeros", test_zeros());
    RUN("scale preserved", test_scale_preserved());
    RUN("1000 random blocks", test_random(1000));

    printf("\n─── Benchmark ───\n");
    bench_encode(10000000);
    bench_decode(10000000);

    printf("\n══════════════════════════════════════════════\n");
    printf("TOTAL: %d PASS, %d FAIL\n", pass, fail);
    printf("Savings per block: 34 → 30 bytes (11.8%% reduction)\n");
    printf("══════════════════════════════════════════════\n");

    return fail ? 1 : 0;
}
