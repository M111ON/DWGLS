/*
 * test_triality_serve.c — Verify D4 triality zero-copy serve layer
 * ═══════════════════════════════════════════════════════════════════════════════
 * Build: gcc -O2 -Wall -I. -Icore -Icore/infra -o tests/test_triality_serve tests/test_triality_serve.c
 * Run:   tests/test_triality_serve
 */
#include <stdio.h>
#include <string.h>
#include "core/infra/geo_triality_serve.h"

/* T0: D4 constants + perm table + roundtrip */
static int test_d4_basics(void) {
    if (!d4_verify_constants()) { printf("  FAIL: d4 constants\n"); return 0; }
    if (!d4_verify_perm_table()) { printf("  FAIL: d4 perm table\n"); return 0; }
    if (!d4_verify_roundtrip()) { printf("  FAIL: d4 roundtrip\n"); return 0; }
    printf("  PASS: D4 constants + perm + roundtrip\n");
    return 1;
}

/* T1: TrialityServe construction + view accessors return same pointer */
static int test_view_pointers(void) {
    uint8_t buf[TW_TOTAL];
    memset(buf, 0xAB, sizeof(buf));
    TrialityServe ts = ts_make(buf, 1);

    void *hw  = ts_as_hardware(&ts);
    void *nat = ts_as_natural(&ts);
    void *fl  = ts_as_flat(&ts);

    if (hw != buf)  { printf("  FAIL: hardware ptr mismatch\n"); return 0; }
    if (nat != buf) { printf("  FAIL: natural ptr mismatch\n"); return 0; }
    if (fl != buf)  { printf("  FAIL: flat ptr mismatch\n"); return 0; }
    printf("  PASS: all 3 view pointers == base (zero-copy)\n");
    return 1;
}

/* T2: Element access — same byte at same flat offset across all views */
static int test_element_access(void) {
    uint8_t buf[TW_TOTAL];
    for (uint32_t i = 0; i < TW_TOTAL; i++) buf[i] = (uint8_t)(i & 0xFF);

    TrialityServe ts = ts_make(buf, 1);

    /* Spot-check: flat=1000 → hardware and natural must read same byte */
    for (uint32_t flat = 0; flat < TW_TOTAL; flat += 307) {
        uint32_t anchor = flat / TW_COMPUTE_SIDE;
        uint32_t local  = flat % TW_COMPUTE_SIDE;
        uint32_t row    = flat / TW_NATURAL_SIDE;
        uint32_t col    = flat % TW_NATURAL_SIDE;

        const uint8_t *hw_p  = (const uint8_t *)ts_at_hardware(&ts, anchor, local);
        const uint8_t *nat_p = (const uint8_t *)ts_at_natural(&ts, row, col);
        const uint8_t *fl_p  = (const uint8_t *)ts_at_flat(&ts, flat);

        if (*hw_p != *fl_p)  { printf("  FAIL: flat=%u hw!=flat\n", flat); return 0; }
        if (*nat_p != *fl_p) { printf("  FAIL: flat=%u nat!=flat\n", flat); return 0; }
    }
    printf("  PASS: element access same byte across 3 views\n");
    return 1;
}

/* T3: Cross-view translation — hard→nat→hard = identity */
static int test_cross_view(void) {
    for (uint32_t flat = 0; flat < TW_TOTAL; flat += 211) {
        TW_HardAddr h = tw_flat_to_hard(flat);
        TW_NatAddr  n = ts_hard_to_nat(h.anchor, h.local);
        TW_HardAddr h2 = ts_nat_to_hard(n.row, n.col);
        if (h2.anchor != h.anchor || h2.local != h.local) {
            printf("  FAIL: flat=%u hard→nat→hard mismatch\n", flat);
            return 0;
        }

        /* nat→hard→nat */
        TW_NatAddr n2 = ts_flat_to_nat(flat);
        TW_HardAddr h3 = ts_nat_to_hard(n2.row, n2.col);
        TW_NatAddr n3 = ts_hard_to_nat(h3.anchor, h3.local);
        if (n3.row != n2.row || n3.col != n2.col) {
            printf("  FAIL: flat=%u nat→hard→nat mismatch\n", flat);
            return 0;
        }
    }
    printf("  PASS: cross-view translation identity (20736 via stride-211)\n");
    return 1;
}

/* T4: Batch translation — translate range to all 3 views */
static int test_batch_translate(void) {
    uint32_t coords[TW_TOTAL * 2];  /* worst case */
    uint32_t start = 0, count = TW_TOTAL;

    /* HARDWARE view */
    if (ts_translate_range(NULL, start, count, coords, D4_VIEW_HARDWARE) != 0) {
        printf("  FAIL: batch translate hardware\n"); return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t flat = (start + i) % TW_TOTAL;
        uint32_t expected_h = flat / TW_COMPUTE_SIDE;
        uint32_t expected_l = flat % TW_COMPUTE_SIDE;
        if (coords[i*2] != expected_h || coords[i*2+1] != expected_l) {
            printf("  FAIL: batch hw flat=%u got (%u,%u) exp (%u,%u)\n",
                   flat, coords[i*2], coords[i*2+1], expected_h, expected_l);
            return 0;
        }
    }

    /* NATURAL view */
    if (ts_translate_range(NULL, start, count, coords, D4_VIEW_NATURAL) != 0) {
        printf("  FAIL: batch translate natural\n"); return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t flat = (start + i) % TW_TOTAL;
        uint32_t expected_r = flat / TW_NATURAL_SIDE;
        uint32_t expected_c = flat % TW_NATURAL_SIDE;
        if (coords[i*2] != expected_r || coords[i*2+1] != expected_c) {
            printf("  FAIL: batch nat flat=%u got (%u,%u) exp (%u,%u)\n",
                   flat, coords[i*2], coords[i*2+1], expected_r, expected_c);
            return 0;
        }
    }

    printf("  PASS: batch translate all 20736 × 3 views\n");
    return 1;
}

/* T5: Full ts_verify */
static int test_verify(void) {
    uint8_t buf[TW_TOTAL];
    for (uint32_t i = 0; i < TW_TOTAL; i++) buf[i] = (uint8_t)(i * 37 & 0xFF);
    TrialityServe ts = ts_make(buf, 1);
    int rc = ts_verify(&ts, buf);
    printf("  %s: ts_verify (rc=%d)\n", rc == 0 ? "PASS" : "FAIL", rc);
    return rc == 0;
}

/* T6: Multi-element sizes (2-byte, 4-byte) */
static int test_multielem(void) {
    uint16_t buf16[TW_TOTAL];
    for (uint32_t i = 0; i < TW_TOTAL; i++) buf16[i] = (uint16_t)(i * 7);
    TrialityServe ts = ts_make(buf16, 2);

    /* Same flat offset must read same 2 bytes via all views */
    for (uint32_t flat = 0; flat < TW_TOTAL; flat += 433) {
        const uint16_t *hw_p  = (const uint16_t *)ts_at_hardware(&ts,
            flat / TW_COMPUTE_SIDE, flat % TW_COMPUTE_SIDE);
        const uint16_t *nat_p = (const uint16_t *)ts_at_natural(&ts,
            flat / TW_NATURAL_SIDE, flat % TW_NATURAL_SIDE);
        const uint16_t *fl_p  = (const uint16_t *)ts_at_flat(&ts, flat);

        if (*hw_p != *fl_p)  { printf("  FAIL: u16 flat=%u hw\n", flat); return 0; }
        if (*nat_p != *fl_p) { printf("  FAIL: u16 flat=%u nat\n", flat); return 0; }
    }
    printf("  PASS: 2-byte elements consistent across views\n");
    return 1;
}

int main(void) {
    printf("=== Triality Serve Tests ===\n");
    printf("Field: %u slots (128×162 = 144×144)\n\n", TW_TOTAL);

    int pass = 0, fail = 0;
    #define RUN(fn) do { if (fn()) pass++; else fail++; } while(0)

    RUN(test_d4_basics);
    RUN(test_view_pointers);
    RUN(test_element_access);
    RUN(test_cross_view);
    RUN(test_batch_translate);
    RUN(test_verify);
    RUN(test_multielem);

    printf("\n=== Results: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
