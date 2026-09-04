/* test_geo_octant.c — Phase 1 verification: octant identity + zero-sum */
#include <stdio.h>
#include <assert.h>
#include "../core/geo_octant.h"

static int test_roundtrip(void) {
    printf("  roundtrip flat→(tess,cube,slot)→flat ... ");
    for (uint32_t f = 0; f < OCT_FULL; f++) {
        uint32_t t = oct_tess_of(f);
        uint32_t c = oct_cube_of(f);
        uint32_t s = oct_slot_of(f);
        assert(oct_flat_of(t, c, s) == f);
    }
    printf("PASS (%u)\n", OCT_FULL);
    return 1;
}

static int test_zero_sum(void) {
    printf("  zero-sum i+j+k ∈ {0,1} ... ");
    uint32_t valid = 0, invalid = 0;
    for (uint32_t c = 0; c < OCT_CUBES; c++) {
        uint32_t s = oct_zero_sum(c);
        if (s <= 1u) valid++; else invalid++;
    }
    assert(valid == 4u);   /* 000, 001, 010, 100 */
    assert(invalid == 4u); /* 011, 101, 110, 111 */
    printf("PASS (4 valid + 4 invalid = 8)\n");
    return 1;
}

static int test_tetra_mapping(void) {
    printf("  tetra mapping (invalid → nearest valid) ... ");
    /* valid cubes map to themselves */
    assert(oct_tetra_of(0) == 0);  /* 000 → 000 */
    assert(oct_tetra_of(1) == 1);  /* 001 → 001 */
    assert(oct_tetra_of(2) == 2);  /* 010 → 010 */
    assert(oct_tetra_of(4) == 4);  /* 100 → 100 */
    /* invalid cubes map to valid */
    assert(oct_is_valid(oct_tetra_of(3)));  /* 011 → ? */
    assert(oct_is_valid(oct_tetra_of(5)));  /* 101 → ? */
    assert(oct_is_valid(oct_tetra_of(6)));  /* 110 → ? */
    assert(oct_is_valid(oct_tetra_of(7)));  /* 111 → ? */
    printf("PASS\n");
    return 1;
}

static int test_antipode(void) {
    printf("  antipode round-trip ... ");
    for (uint32_t c = 0; c < OCT_CUBES; c++) {
        assert(oct_antipode_cube(oct_antipode_cube(c)) == c);
    }
    /* full flat round-trip */
    for (uint32_t f = 0; f < OCT_FULL; f++) {
        uint32_t a = oct_antipode_flat(f);
        assert(oct_antipode_flat(a) == f);
        /* same tess, same slot, different cube */
        assert(oct_tess_of(a) == oct_tess_of(f));
        assert(oct_slot_of(a) == oct_slot_of(f));
        assert(oct_cube_of(a) != oct_cube_of(f));
    }
    printf("PASS\n");
    return 1;
}

static int test_12x12(void) {
    printf("  12×12 decomposition ... ");
    for (uint32_t slot = 0; slot < OCT_CELLS; slot++) {
        uint32_t s12, s0;
        oct_slot_to_12x12(slot, &s12, &s0);
        assert(s12 * 12 + s0 == slot);
        assert(s12 < 12 && s0 < 12);

        uint32_t sq, tri;
        oct_12_to_sq_tri(s12, &sq, &tri);
        assert(sq * 3 + tri == s12);
        assert(sq < 4 && tri < 3);
    }
    printf("PASS (144 slots)\n");
    return 1;
}

static int test_full_decompose(void) {
    printf("  full decompose flat→(tess,cube,sq12,tri12,sq0,tri0) ... ");
    for (uint32_t f = 0; f < OCT_FULL; f++) {
        uint32_t t, c, sq12, tri12, sq0, tri0;
        oct_full_decompose(f, &t, &c, &sq12, &tri12, &sq0, &tri0);
        /* reconstruct slot from components */
        uint32_t slot = (sq12 * 3 + tri12) * 12 + (sq0 * 3 + tri0);
        uint32_t rt = oct_flat_of(t, c, slot);
        assert(rt == f);
    }
    printf("PASS (%u)\n", OCT_FULL);
    return 1;
}

static int test_verify(void) {
    printf("  geo_octant_verify() ... ");
    assert(geo_octant_verify());
    printf("PASS\n");
    return 1;
}

int main(void) {
    printf("=== test_geo_octant (Phase 1: octant identity + zero-sum) ===\n");
    int ok = 1;
    ok &= test_roundtrip();
    ok &= test_zero_sum();
    ok &= test_tetra_mapping();
    ok &= test_antipode();
    ok &= test_12x12();
    ok &= test_full_decompose();
    ok &= test_verify();
    printf("=== %s ===\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
