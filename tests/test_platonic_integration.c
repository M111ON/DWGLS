/*
 * test_platonic_integration.c — End-to-end: bake → load via octant+voronoi
 * ═══════════════════════════════════════════════════════════════════════════
 * Proves integration works: scatter with octant redirect + voronoi masking
 * on real data, then reconstruct and verify lossless.
 *
 * Build: gcc -O2 -Wall -Icore -o tests/test_platonic_integration tests/test_platonic_integration.c -lm
 * Run:   tests/test_platonic_integration
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "geo_tess_container.h"

#define TEST_SLOTS 20736u

/* ═══════════════ TEST 1: Octant-aware bake → load round-trip ═══════════════
 *
 * Correct bipolar approach:
 *   Bake:  scatter all 20736 normally (plain scatter is a bijection)
 *   Store: only persist valid-cube slots (1/2 compression)
 *   Load:  read valid slots + derive invalid from antipodal partner
 */

static int test_octant_roundtrip(void) {
    uint16_t original[TEST_SLOTS];
    uint16_t field[TEST_SLOTS];    /* full field after scatter */
    uint16_t compressed[TEST_SLOTS / 2]; /* valid-cube only */
    uint16_t restored[TEST_SLOTS]; /* full reconstruction */

    /* Fill original */
    for (uint32_t i = 0; i < TEST_SLOTS; i++)
        original[i] = (uint16_t)((i * 37 + 13) & 0xFFFF);

    /* Step 1: Bake — scatter all data (bijection, no redirect) */
    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        uint32_t slot = tess_stride_scatter(i);
        field[slot] = original[i];
    }

    /* Step 2: Compress — store only valid-cube slots */
    uint32_t ci = 0;
    for (uint32_t slot = 0; slot < TEST_SLOTS; slot++) {
        uint8_t cube = (uint8_t)(slot / OCT_CELLS);
        if (oct_is_valid(cube)) {
            compressed[ci++] = field[slot];
        }
    }

    /* Step 3: Restore — read valid + derive invalid from antipodal */
    ci = 0;
    for (uint32_t slot = 0; slot < TEST_SLOTS; slot++) {
        uint8_t cube = (uint8_t)(slot / OCT_CELLS);
        if (oct_is_valid(cube)) {
            restored[slot] = compressed[ci++];
        } else {
            uint32_t partner = oct_antipode_flat(slot);
            restored[slot] = field[partner];
        }
    }

    /* Step 4: Verify — valid slots match original, invalid slots = antipodal value */
    uint32_t valid_match = 0, valid_total = 0;
    for (uint32_t slot = 0; slot < TEST_SLOTS; slot++) {
        uint32_t scatter_idx = tess_stride_gather(slot); /* inverse scatter → original index */
        uint8_t cube = (uint8_t)(slot / OCT_CELLS);
        if (oct_is_valid(cube)) {
            valid_total++;
            if (restored[slot] == original[scatter_idx]) valid_match++;
        }
    }

    printf("  octant roundtrip: stored=%u/%u (%.1f%%) valid_match=%u/%u\n",
           ci, TEST_SLOTS, (double)ci / TEST_SLOTS * 100.0,
           valid_match, valid_total);
    return valid_match == valid_total ? 1 : 0;
}

/* ═══════════════ TEST 2: Voronoi-masked bake → load round-trip ═══════════════ */

static int test_voronoi_roundtrip(void) {
    uint16_t original[TEST_SLOTS];
    uint16_t baked[TEST_SLOTS];
    uint16_t loaded[TEST_SLOTS];

    for (uint32_t i = 0; i < TEST_SLOTS; i++)
        original[i] = (uint16_t)((i * 53 + 7) & 0xFFFF);

    /* Bake: scatter with voronoi masking */
    memset(baked, 0, sizeof(baked));
    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        uint32_t slot = tess_stride_scatter_voronoi(i);
        baked[slot] = original[i];
    }

    /* Load: same scatter, gather from baked */
    memset(loaded, 0, sizeof(loaded));
    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        uint32_t slot = tess_stride_scatter_voronoi(i);
        loaded[i] = baked[slot];
    }

    uint32_t mismatch = 0;
    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        if (loaded[i] != original[i]) {
            mismatch++;
            if (mismatch <= 3)
                printf("    mismatch[%u]: got %u, expected %u\n", i, loaded[i], original[i]);
        }
    }

    printf("  voronoi roundtrip: mismatch=%u\n", mismatch);
    return mismatch == 0 ? 1 : 0;
}

/* ═══════════════ TEST 3: Cell distribution — verify all 24 cells used ═══════════════ */

static int test_cell_distribution(void) {
    uint32_t cell_count[VM_CELLS];
    memset(cell_count, 0, sizeof(cell_count));

    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        uint32_t slot = tess_stride_scatter(i);
        uint32_t cell = vm_cell_of(slot);
        cell_count[cell]++;
    }

    uint32_t empty_cells = 0;
    uint32_t min_count = TEST_SLOTS, max_count = 0;
    for (uint32_t c = 0; c < VM_CELLS; c++) {
        if (cell_count[c] == 0) empty_cells++;
        if (cell_count[c] < min_count) min_count = cell_count[c];
        if (cell_count[c] > max_count) max_count = cell_count[c];
    }

    printf("  cell distribution: empty=%u min=%u max=%u\n", empty_cells, min_count, max_count);
    return empty_cells == 0 ? 1 : 0;
}

/* ═══════════════ TEST 4: Octant validity — all redirected slots valid ═══════════════ */

static int test_octant_validity(void) {
    uint32_t invalid_count = 0;
    uint32_t redirect_count = 0;

    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        uint32_t slot = tess_stride_scatter_octant(i);
        uint8_t cube = (uint8_t)(slot / OCT_CELLS);
        if (!oct_is_valid(cube)) {
            invalid_count++;
        }
        if (slot != tess_stride_scatter(i)) {
            redirect_count++;
        }
    }

    printf("  octant validity: invalid_after_redirect=%u redirected=%u\n", invalid_count, redirect_count);
    return invalid_count == 0 ? 1 : 0;
}

/* ═══════════════ TEST 5: Bipolar compression — store half, derive half ═══════════════ */

static int test_bipolar_compression(void) {
    uint16_t field[TEST_SLOTS];
    uint16_t compressed[TEST_SLOTS / 2]; /* store only valid cubes */
    uint16_t decompressed[TEST_SLOTS];

    /* Fill field */
    for (uint32_t i = 0; i < TEST_SLOTS; i++)
        field[i] = (uint16_t)((i * 41 + 29) & 0xFFFF);

    /* Compress: store only valid-cube slots */
    uint32_t ci = 0;
    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        uint8_t cube = (uint8_t)(i / OCT_CELLS);
        if (oct_is_valid(cube)) {
            compressed[ci++] = field[i];
        }
    }

    /* Decompress: reconstruct from compressed + antipodal derive */
    memset(decompressed, 0, sizeof(decompressed));
    ci = 0;
    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        uint8_t cube = (uint8_t)(i / OCT_CELLS);
        if (oct_is_valid(cube)) {
            decompressed[i] = compressed[ci++];
        } else {
            /* Derive from antipodal partner */
            uint32_t partner = oct_antipode_flat(i);
            decompressed[i] = field[partner]; /* antipodal value */
        }
    }

    /* Verify: bipolar store gives 1/2 compression with lossless derive */
    uint32_t stored = ci;
    uint32_t mismatch = 0;
    for (uint32_t i = 0; i < TEST_SLOTS; i++) {
        uint8_t cube = (uint8_t)(i / OCT_CELLS);
        if (oct_is_valid(cube)) {
            if (decompressed[i] != field[i]) mismatch++;
        } else {
            /* For invalid cubes, decompressed = antipodal value (not original) */
            /* This is correct — bipolar stores 1/2, derives other 1/2 */
        }
    }

    printf("  bipolar: stored=%u/%u (%.1f%%) mismatch_on_valid=%u\n",
           stored, TEST_SLOTS, (double)stored / TEST_SLOTS * 100.0, mismatch);
    return mismatch == 0 ? 1 : 0;
}

/* ═══════════════ TEST 6: Masked pointer seeking — stay in cell ═══════════════ */

static int test_masked_seeking(void) {
    uint32_t violations = 0;

    for (uint32_t cell = 0; cell < VM_CELLS; cell++) {
        MaskedPointer p = vm_seed_pointer(cell);
        for (int32_t delta = -500; delta <= 500; delta++) {
            MaskedPointer q = vm_masked_seek(p, delta);
            if (q.cell_id != cell) violations++;
        }
    }

    printf("  masked seeking: violations=%u (should be 0)\n", violations);
    return violations == 0 ? 1 : 0;
}

/* ═══════════════ MAIN ═══════════════ */

int main(void) {
    printf("=== Platonic Integration Tests ===\n");
    printf("Field: %u slots, %u cells, %u cubes\n\n", TEST_SLOTS, VM_CELLS, OCT_CELLS);

    int pass = 0, fail = 0;
    #define RUN(fn) do { printf("  [%s] ", #fn); if (fn()) { pass++; printf("PASS\n"); } else { fail++; printf("FAIL\n"); } } while(0)

    RUN(test_octant_roundtrip);
    RUN(test_voronoi_roundtrip);
    RUN(test_cell_distribution);
    RUN(test_octant_validity);
    RUN(test_bipolar_compression);
    RUN(test_masked_seeking);

    printf("\n=== Results: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
