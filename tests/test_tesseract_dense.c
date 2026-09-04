/*
 * test_tesseract_dense.c — Verify dense tesseract (1 tesseract, deterministic)
 * ═══════════════════════════════════════════════════════════════════════════
 * Build: gcc -O2 -Wall -Icore -o tests/test_tesseract_dense tests/test_tesseract_dense.c -lm
 * Run:   tests/test_tesseract_dense
 */
#include <stdio.h>
#include <string.h>
#include "geo_tesseract_dense.h"

static int test_init(void) {
    DenseTesseract t;
    td_init(&t);

    /* Check valid_mask: cubes 0,1,2,4 should be set */
    uint8_t expected = (1u<<0)|(1u<<1)|(1u<<2)|(1u<<4);
    if (t.valid_mask != expected) {
        printf("  FAIL: valid_mask=0x%02x expected=0x%02x\n", t.valid_mask, expected);
        return 0;
    }
    printf("  PASS: init valid_mask\n");
    return 1;
}

static int test_bipolar_roundtrip(void) {
    DenseTesseract t;
    td_init(&t);

    /* Write valid cubes */
    for (uint32_t c = 0; c < OCT_CUBES; c++) {
        if (!oct_is_valid(c)) continue;
        for (uint32_t s = 0; s < TD_SLOTS_PER; s++) {
            td_write(&t, c, s, (uint16_t)(c * 144 + s));
        }
    }

    /* Read valid cubes directly */
    for (uint32_t c = 0; c < OCT_CUBES; c++) {
        if (!oct_is_valid(c)) continue;
        for (uint32_t s = 0; s < TD_SLOTS_PER; s++) {
            uint16_t got = td_read(&t, c, s);
            uint16_t exp = (uint16_t)(c * 144 + s);
            if (got != exp) {
                printf("  FAIL: read valid cube=%u slot=%u got=%u exp=%u\n", c, s, got, exp);
                return 0;
            }
        }
    }
    printf("  PASS: valid cube read\n");

    /* Bipolar read for antipodal cubes should match valid partner */
    for (uint32_t c = 0; c < OCT_CUBES; c++) {
        if (oct_is_valid(c)) continue;
        uint32_t partner = oct_tetra_of(c);
        for (uint32_t s = 0; s < TD_SLOTS_PER; s++) {
            uint16_t got = td_bipolar_read(&t, c, s);
            uint16_t exp = (uint16_t)(partner * 144 + s);
            if (got != exp) {
                printf("  FAIL: bipolar read cube=%u slot=%u got=%u exp=%u partner=%u\n", c, s, got, exp, partner);
                return 0;
            }
        }
    }
    printf("  PASS: antipodal bipolar read\n");
    return 1;
}

static int test_bipolar_write(void) {
    DenseTesseract t;
    td_init(&t);

    /* Write to valid cube 0, should sync to antipodal 7 */
    td_bipolar_write(&t, 0, 42, 0xABCD);

    uint16_t r0 = td_read(&t, 0, 42);
    uint16_t r7 = td_read(&t, 7, 42);
    if (r0 != 0xABCD || r7 != 0xABCD) {
        printf("  FAIL: sync valid->antipodal r0=%u r7=%u\n", r0, r7);
        return 0;
    }
    printf("  PASS: bipolar write sync\n");

    /* Write to invalid cube 7, should redirect to valid partner (oct_tetra_of(7)=4) */
    td_bipolar_write(&t, 7, 99, 0x1234);
    uint32_t partner = oct_tetra_of(7);
    uint16_t r = td_read(&t, partner, 99);
    if (r != 0x1234) {
        printf("  FAIL: redirect invalid->valid partner=%u r=%u\n", partner, r);
        return 0;
    }
    printf("  PASS: bipolar write redirect (cube 7 → cube %u)\n", partner);
    return 1;
}

static int test_flat_addressing(void) {
    DenseTesseract t;
    td_init(&t);

    for (uint32_t flat = 0; flat < TD_TOTAL; flat++) {
        uint16_t val = (uint16_t)(flat ^ 0xCAFE);
        td_flat_write(&t, flat, val);
        uint16_t got = td_flat_read(&t, flat);
        if (got != val) {
            printf("  FAIL: flat round-trip flat=%u got=%u exp=%u\n", flat, got, val);
            return 0;
        }
    }
    printf("  PASS: flat addressing round-trip (1152 slots)\n");
    return 1;
}

static int test_verify(void) {
    DenseTesseract t;
    td_init(&t);
    int ok = td_verify(&t);
    printf("  %s: td_verify\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main(void) {
    printf("=== Dense Tesseract Tests ===\n");
    printf("1 tesseract = %u cubes × %u slots = %u total\n",
           OCT_CUBES, TD_SLOTS_PER, TD_TOTAL);
    printf("Valid cubes: %u (zero-sum ≤ 1)\n", TD_VALID_CUBES);
    printf("Storage: %u bytes (bipolar 1/2)\n\n", td_storage_size());

    int pass = 0, fail = 0;
    #define RUN(fn) do { if (fn()) pass++; else fail++; } while(0)

    RUN(test_init);
    RUN(test_bipolar_roundtrip);
    RUN(test_bipolar_write);
    RUN(test_flat_addressing);
    RUN(test_verify);

    printf("\n=== Results: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
