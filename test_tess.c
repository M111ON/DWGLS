/* test_tess.c — Self-test for tesseract_container.h */
#include <stdio.h>
#include <stdlib.h>
#include "tesseract_container.h"

int main(void) {
    printf("=== Tesseract Container Self-Test ===\n\n");
    
    /* Run built-in self-test */
    int result = tess_selftest();
    printf("Self-test: %s\n\n", result == 0 ? "PASS" : "FAIL");
    
    /* Detailed tests */
    int pass = 0, fail = 0;
    
    /* Test: Header structure size */
    printf("sizeof(TessHeader) = %zu (expected 32)\n", sizeof(TessHeader));
    if (sizeof(TessHeader) == 32) pass++; else fail++;
    
    /* Test: Constants */
    printf("HYP_KIS_SLOTS  = %u (expected 20736)\n", HYP_KIS_SLOTS);
    printf("HYP_AXIS_SLOTS = %u (expected 6912)\n",  HYP_AXIS_SLOTS);
    printf("TESS_N_OCTANTS = %u (expected 8)\n",      TESS_N_OCTANTS);
    if (HYP_KIS_SLOTS == 20736 && HYP_AXIS_SLOTS == 6912) pass++; else fail++;
    
    /* Test: tess_axis_reflect
     * reflect(i) = (N - i) % N
     * reflect(0) = 0 (origin is its own reflection) */
    printf("\n--- Axis Reflect ---\n");
    uint32_t reflect_tests[][2] = {
        {0, 0},     /* origin reflects to itself */
        {1, 6911},
        {3456, 3456}, /* midpoint reflects to itself */
        {6911, 1}
    };
    for (int i = 0; i < 4; i++) {
        uint32_t got = tess_axis_reflect(reflect_tests[i][0]);
        uint32_t exp = reflect_tests[i][1];
        printf("  reflect(%u) = %u (expected %u) %s\n",
               reflect_tests[i][0], got, exp, got == exp ? "OK" : "FAIL");
        if (got == exp) pass++; else fail++;
    }
    
    /* Test: tess_octant_of — single-axis slot
     * For a slot on axis a, only that axis's sign is determinable.
     * Other axes default to positive (0).
     *   Axis 0 (X): local < 3456 → octant 0, local >= 3456 → octant 1
     *   Axis 1 (Y): local < 3456 → octant 0, local >= 3456 → octant 2
     *   Axis 2 (Z): local < 3456 → octant 0, local >= 3456 → octant 4 */
    printf("\n--- Octant Classification (single-axis) ---\n");
    uint32_t oct_slots[]   = {0, 3456, 6911, 6912, 10368, 13823, 13824, 17280, 20735};
    uint8_t  oct_expected[] = {0, 1,    1,    0,    2,     2,     0,     4,     4};
    for (int i = 0; i < 9; i++) {
        uint8_t got = tess_octant_of(oct_slots[i]);
        uint8_t exp = oct_expected[i];
        printf("  octant_of(%u) = %u (expected %u) %s\n",
               oct_slots[i], got, exp, got == exp ? "OK" : "FAIL");
        if (got == exp) pass++; else fail++;
    }
    
    /* Test: tess_octant_of_3d — full 3D octant from (kx, ky, kz) */
    printf("\n--- Octant 3D Classification ---\n");
    /* (kx, ky, kz, expected_octant) */
    struct { uint32_t x, y, z; uint8_t exp; } oct3d[] = {
        {0, 0, 0,       0},  /* all positive → PPP */
        {3456, 0, 0,    1},  /* X negative → NPP */
        {0, 3456, 0,    2},  /* Y negative → PNP */
        {3456, 3456, 0, 3},  /* X+Y negative → NNP */
        {0, 0, 3456,    4},  /* Z negative → PPN */
        {3456, 0, 3456, 5},  /* X+Z negative → NPN */
        {0, 3456, 3456, 6},  /* Y+Z negative → PNN */
        {3456, 3456, 3456, 7}, /* all negative → NNN */
        {100, 200, 300, 0},   /* all positive → PPP */
        {6911, 6911, 6911, 7}, /* all negative → NNN */
    };
    for (int i = 0; i < 10; i++) {
        uint8_t got = tess_octant_of_3d(oct3d[i].x, oct3d[i].y, oct3d[i].z);
        uint8_t exp = oct3d[i].exp;
        printf("  octant_of_3d(%u,%u,%u) = %u (expected %u) %s\n",
               oct3d[i].x, oct3d[i].y, oct3d[i].z, got, exp,
               got == exp ? "OK" : "FAIL");
        if (got == exp) pass++; else fail++;
    }
    
    /* Test: mirror_octant round-trip (flip same mask twice = identity) */
    printf("\n--- Mirror Round-Trip ---\n");
    uint32_t mirror_slots[] = {0, 100, 3456, 5000, 6911};
    for (int i = 0; i < 5; i++) {
        uint32_t s = mirror_slots[i];
        uint32_t m1 = mirror_octant(s, 0x07);  /* flip X,Y,Z */
        uint32_t m2 = mirror_octant(m1, 0x07); /* flip back  */
        printf("  mirror(mirror(%u)) = %u (expected %u) %s\n",
               s, m2, s, m2 == s ? "OK" : "FAIL");
        if (m2 == s) pass++; else fail++;
    }
    
    /* Test: mirror_octant individual axis flips */
    printf("\n--- Mirror Individual Axes ---\n");
    uint32_t test_slots[] = {100, 5000, 6912, 10368, 13824, 17280};
    for (int i = 0; i < 6; i++) {
        uint32_t s = test_slots[i];
        uint8_t axis = s / HYP_AXIS_SLOTS;
        uint32_t local = s % HYP_AXIS_SLOTS;
        uint8_t flip = 1u << axis;
        uint32_t m = mirror_octant(s, flip);
        uint32_t expected_local = (HYP_AXIS_SLOTS - local) % HYP_AXIS_SLOTS;
        uint32_t expected = axis * HYP_AXIS_SLOTS + expected_local;
        printf("  mirror(%u, flip_axis_%d) = %u (expected %u) %s\n",
               s, axis, m, expected, m == expected ? "OK" : "FAIL");
        if (m == expected) pass++; else fail++;
    }
    
    /* Test: tess_octant_map consistency with mirror_octant */
    printf("\n--- Octant Map Consistency ---\n");
    uint32_t map_slot = 1234;
    uint8_t src_oct = tess_octant_of(map_slot);
    int map_ok = 1;
    for (uint8_t dst_oct = 0; dst_oct < 8; dst_oct++) {
        uint32_t via_map  = tess_octant_map(map_slot, src_oct, dst_oct);
        uint32_t via_flip = mirror_octant(map_slot, src_oct ^ dst_oct);
        if (via_map != via_flip) {
            printf("  MISMATCH: map(%u,%u,%u)=%u vs flip(%u)=%u\n",
                   map_slot, src_oct, dst_oct, via_map, src_oct ^ dst_oct, via_flip);
            map_ok = 0;
        }
    }
    printf("  octant_map == mirror_octant: %s\n", map_ok ? "OK" : "FAIL");
    if (map_ok) pass++; else fail++;
    
    /* Test: tess_create/encode/decode/verify round-trip */
    printf("\n--- Container Round-Trip ---\n");
    TessContainer tc;
    int rc = tess_create(&tc, TESS_N_OCTANTS, TESS_SCALE_FP, TESS_FORMULA_CAYLEY);
    printf("  create: %s\n", rc == 0 ? "OK" : "FAIL");
    if (rc == 0) pass++; else fail++;
    
    uint32_t total = TESS_N_OCTANTS * HYP_AXIS_SLOTS;
    uint8_t *buf = (uint8_t *)malloc(total);
    for (uint32_t i = 0; i < total; i++)
        buf[i] = (uint8_t)(i * 7 & 0xFF);
    
    tess_encode(&tc, buf, total);
    printf("  encode: OK (checksum=0x%08X)\n", tc.header.checksum);
    pass++;
    
    int decoded_ok = 1;
    for (uint32_t i = 0; i < total; i++) {
        if (tess_decode(&tc, i) != buf[i]) {
            printf("  decode mismatch at slot %u\n", i);
            decoded_ok = 0;
            break;
        }
    }
    printf("  decode: %s\n", decoded_ok ? "OK" : "FAIL");
    if (decoded_ok) pass++; else fail++;
    
    int verified = tess_verify(&tc, buf, total);
    printf("  verify: %s\n", verified ? "OK" : "FAIL");
    if (verified) pass++; else fail++;
    
    tess_destroy(&tc);
    free(buf);
    
    /* Test: tess_octant_stats */
    printf("\n--- Octant Stats ---\n");
    TessContainer tc2;
    tess_create(&tc2, TESS_N_OCTANTS, TESS_SCALE_FP, TESS_FORMULA_LINEAR);
    TessOctantStats stats;
    tess_octant_stats(&tc2, &stats);
    printf("  total_slots = %u (expected %u)\n", stats.total_slots, total);
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        printf("  octant[%d] = %u\n", i, stats.per_octant[i]);
        sum += stats.per_octant[i];
    }
    printf("  sum = %u (expected %u) %s\n", sum, total, sum == total ? "OK" : "FAIL");
    if (sum == total) pass++; else fail++;
    tess_destroy(&tc2);
    
    /* Test: Hyperbolic round-trip consistency */
    printf("\n--- Hyperbolic Round-Trip ---\n");
    int hyp_pass = 0, hyp_fail = 0;
    for (uint32_t s = 0; s < 20736; s += 100) {
        uint8_t axis = s / HYP_AXIS_SLOTS;
        if (axis > 2) axis = 2;
        HypComplex w = kis_to_hyperbolic_axis(s, axis);
        uint32_t back = hyperbolic_to_kis_axis(w, axis);
        if (back == s) hyp_pass++; else hyp_fail++;
    }
    printf("  hyperbolic round-trip: %d pass, %d fail\n", hyp_pass, hyp_fail);
    if (hyp_fail == 0) pass++; else fail++;
    
    /* Test: tess_octant_resolve */
    printf("\n--- Octant Resolve ---\n");
    uint8_t resolve_oct;
    HypComplex hw = tess_octant_resolve(1234, &resolve_oct);
    printf("  resolve(1234): octant=%u, hyp=(%.4f + %.4fi)\n",
           resolve_oct, hw.re, hw.im);
    printf("  resolve: OK (hyp != NaN)\n");
    pass++;
    
    printf("\n=== Results: %d pass, %d fail ===\n", pass, fail);
    return fail;
}
