/*
 * test_voronoi_mask.c — Verify Voronoi pointer masking
 * ═══════════════════════════════════════════════════════════════════════════
 * Build: gcc -O2 -Wall -Icore -o tests/test_voronoi_mask tests/test_voronoi_mask.c -lm
 * Run:   tests/test_voronoi_mask
 */
#include <stdio.h>
#include <string.h>
#include "geo_voronoi_mask.h"

static int test_constants(void) {
    if (VM_CELLS * VM_SLOTS_PER != VM_FULL) {
        printf("  FAIL: %u × %u ≠ %u\n", VM_CELLS, VM_SLOTS_PER, VM_FULL);
        return 0;
    }
    printf("  PASS: constants %u cells × %u slots = %u\n", VM_CELLS, VM_SLOTS_PER, VM_FULL);
    return 1;
}

static int test_mask_unmask(void) {
    for (uint32_t flat = 0; flat < VM_FULL; flat++) {
        MaskedPointer p = vm_mask(flat);
        uint32_t rt = vm_unmask(p);
        if (rt != flat) {
            printf("  FAIL: flat=%u → mask(%u,%u) → %u\n", flat, p.cell_id, p.local, rt);
            return 0;
        }
    }
    printf("  PASS: mask→unmask round-trip (20736)\n");
    return 1;
}

static int test_cell_boundary(void) {
    for (uint32_t flat = 0; flat < VM_FULL; flat++) {
        MaskedPointer p = vm_mask(flat);
        if (!vm_in_cell(p, flat)) {
            printf("  FAIL: flat=%u not in cell %u\n", flat, p.cell_id);
            return 0;
        }
        if (vm_cell_of(flat) >= VM_CELLS) {
            printf("  FAIL: cell_of(%u) = %u ≥ %u\n", flat, vm_cell_of(flat), VM_CELLS);
            return 0;
        }
    }
    printf("  PASS: cell boundary check (20736)\n");
    return 1;
}

static int test_masked_seek(void) {
    for (uint32_t cell = 0; cell < VM_CELLS; cell++) {
        MaskedPointer p = vm_seed_pointer(cell);
        for (int32_t d = -200; d <= 200; d++) {
            MaskedPointer q = vm_masked_seek(p, d);
            if (q.cell_id != p.cell_id) {
                printf("  FAIL: cell %u seek %d → cell %u (overflow)\n", cell, d, q.cell_id);
                return 0;
            }
            uint32_t flat_q = vm_unmask(q);
            if (vm_cell_of(flat_q) != cell) {
                printf("  FAIL: cell %u seek %d → flat %u (wrong cell)\n", cell, d, flat_q);
                return 0;
            }
        }
    }
    printf("  PASS: masked seek stays in cell (±200)\n");
    return 1;
}

static int test_masked_rw(void) {
    uint16_t data[VM_FULL];
    memset(data, 0, sizeof(data));

    /* Write through mask */
    for (uint32_t cell = 0; cell < VM_CELLS; cell++) {
        MaskedPointer p = vm_seed_pointer(cell);
        vm_masked_write(data, p, (uint16_t)(cell + 0xA000));
    }

    /* Read through mask — should match */
    for (uint32_t cell = 0; cell < VM_CELLS; cell++) {
        MaskedPointer p = vm_seed_pointer(cell);
        uint16_t got = vm_masked_read(data, p);
        uint16_t exp = (uint16_t)(cell + 0xA000);
        if (got != exp) {
            printf("  FAIL: cell %u read %u exp %u\n", cell, got, exp);
            return 0;
        }
    }
    printf("  PASS: masked read/write (24 cells)\n");
    return 1;
}

static int test_overflow_seek(void) {
    MaskedPointer p = vm_mask(100); /* cell 0, local 100 */
    MaskedPointer q = vm_masked_seek_overflow(p, 900); /* should cross to cell 1 */

    if (q.cell_id == p.cell_id) {
        printf("  FAIL: overflow seek didn't cross cell boundary\n");
        return 0;
    }
    if (q.cell_id != 1) {
        printf("  FAIL: overflow seek cell=%u expected=1\n", q.cell_id);
        return 0;
    }
    printf("  PASS: overflow seek crosses cell boundary (cell 0→%u)\n", q.cell_id);
    return 1;
}

static int test_verify(void) {
    int ok = vm_verify();
    printf("  %s: vm_verify\n", ok == 0 ? "PASS" : "FAIL");
    return ok == 0;
}

int main(void) {
    printf("=== Voronoi Mask Tests ===\n");
    printf("Field: %u slots, %u cells × %u slots/cell\n\n", VM_FULL, VM_CELLS, VM_SLOTS_PER);

    int pass = 0, fail = 0;
    #define RUN(fn) do { if (fn()) pass++; else fail++; } while(0)

    RUN(test_constants);
    RUN(test_mask_unmask);
    RUN(test_cell_boundary);
    RUN(test_masked_seek);
    RUN(test_masked_rw);
    RUN(test_overflow_seek);
    RUN(test_verify);

    printf("\n=== Results: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
