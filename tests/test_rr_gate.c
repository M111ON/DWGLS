/*
 * test_rr_gate.c — Rotation-Reversal Gate
 * ═══════════════════════════════════════════════════════════════════════════════
 * Proves: RR gate detects rotation reversals in Hilbert walk sequences.
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore \
 *        -o tests/test_rr_gate tests/test_rr_gate.c -lm
 * Run:   tests/test_rr_gate
 */
#include <stdio.h>
#include <string.h>
#include "core/geo_rr_gate.h"
#include "core/geo_lblock.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass_count++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail_count++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* T0: constants + rot_dist bijection + gate consistency */
static int test_basics(void) {
    if (rr_verify_constants() != 0) { printf("  FAIL: constants\n"); fail_count++; return 0; }
    if (rr_verify_rot_dist() != 0) { printf("  FAIL: rot_dist\n"); fail_count++; return 0; }
    if (rr_verify_gate() != 0)     { printf("  FAIL: gate\n"); fail_count++; return 0; }
    pass_count += 3;
    printf("  T0a: PASS — constants\n");
    printf("  T0b: PASS — rot_dist bijection (all 16 pairs)\n");
    printf("  T0c: PASS — gate consistency (all 64 triples)\n");
    return 1;
}

/* T1: rot_dist specific cases */
static int test_rot_dist(void) {
    /* same → 0 */
    if (rr_rot_dist(0, 0) != 0)  { printf("  FAIL: dist(0,0)\n"); fail_count++; return 0; }
    if (rr_rot_dist(3, 3) != 0)  { printf("  FAIL: dist(3,3)\n"); fail_count++; return 0; }
    /* forward +1 */
    if (rr_rot_dist(0, 1) != 1)  { printf("  FAIL: dist(0,1)\n"); fail_count++; return 0; }
    if (rr_rot_dist(2, 3) != 1)  { printf("  FAIL: dist(2,3)\n"); fail_count++; return 0; }
    if (rr_rot_dist(3, 0) != 1)  { printf("  FAIL: dist(3,0) wrap\n"); fail_count++; return 0; }
    /* backward -1 */
    if (rr_rot_dist(1, 0) != -1) { printf("  FAIL: dist(1,0)\n"); fail_count++; return 0; }
    if (rr_rot_dist(0, 3) != -1) { printf("  FAIL: dist(0,3) wrap\n"); fail_count++; return 0; }
    /* diametric ±2 */
    if (rr_rot_dist(0, 2) != 2 && rr_rot_dist(0, 2) != -2) {
        printf("  FAIL: dist(0,2)\n"); fail_count++; return 0; }
    pass_count++;
    printf("  T1: PASS — rot_dist specific cases\n");
    return 1;
}

/* T2: gate — forward sequence (0,1,2,3) */
static int test_forward_sequence(void) {
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        RRState s = rr_gate((uint8_t)i, (uint8_t)(i+1), (uint8_t)(i+2));
        if (s != RR_FORWARD) { ok = 0; break; }
    }
    CHECK(2, "forward sequence 0→1→2→3 → all FORWARD", ok);
    return ok;
}

/* T3: gate — reversed sequence (0,1,0) */
static int test_reversed(void) {
    RRState s = rr_gate(0, 1, 0);
    if (s != RR_REVERSED) {
        printf("  T3: FAIL — rr_gate(0,1,0) = %d (expected REVERSED=2)\n", s);
        fail_count++;
        return 0;
    }
    pass_count++;
    printf("  T3: PASS — rr_gate(0,1,0) → REVERSED\n");
    return 1;
}

/* T4: gate — hold (same rotation) */
static int test_hold(void) {
    RRState s = rr_gate(2, 2, 2);
    if (s != RR_HOLD) {
        printf("  T4: FAIL — rr_gate(2,2,2) = %d (expected HOLD=0)\n", s);
        fail_count++;
        return 0;
    }
    pass_count++;
    printf("  T4: PASS — rr_gate(2,2,2) → HOLD\n");
    return 1;
}

/* T5: gate — starting (0,0,1) */
static int test_starting(void) {
    RRState s = rr_gate(0, 0, 1);
    if (s != RR_FORWARD) {
        printf("  T5: FAIL — rr_gate(0,0,1) = %d (expected FORWARD=1)\n", s);
        fail_count++;
        return 0;
    }
    pass_count++;
    printf("  T5: PASS — rr_gate(0,0,1) → FORWARD\n");
    return 1;
}

/* T6: gate — stopping (1,2,2) */
static int test_stopping(void) {
    RRState s = rr_gate(1, 2, 2);
    if (s != RR_HOLD) {
        printf("  T6: FAIL — rr_gate(1,2,2) = %d (expected HOLD=0)\n", s);
        fail_count++;
        return 0;
    }
    pass_count++;
    printf("  T6: PASS — rr_gate(1,2,2) → HOLD\n");
    return 1;
}

/* T7: pair gate */
static int test_pair_gate(void) {
    if (rr_pair_gate(0, 0) != RR_HOLD)     { printf("  FAIL: pair(0,0)\n"); fail_count++; return 0; }
    if (rr_pair_gate(0, 1) != RR_FORWARD)  { printf("  FAIL: pair(0,1)\n"); fail_count++; return 0; }
    if (rr_pair_gate(1, 0) != RR_FORWARD)  { printf("  FAIL: pair(1,0)\n"); fail_count++; return 0; }
    if (rr_pair_gate(0, 2) != RR_REVERSED) { printf("  FAIL: pair(0,2)\n"); fail_count++; return 0; }
    if (rr_pair_gate(1, 3) != RR_REVERSED) { printf("  FAIL: pair(1,3)\n"); fail_count++; return 0; }
    pass_count++;
    printf("  T7: PASS — pair gate specific cases\n");
    return 1;
}

/* T8: count_reversals on real Hilbert walk rotations */
static int test_hilbert_reversals(void) {
    enum { GRID = 16, N = GRID * GRID };
    uint8_t rots[N];

    /* collect rotations from Hilbert walk on 16×16 */
    for (uint32_t d = 0; d < N; d++) {
        int32_t dx, dy;
        uint32_t rot;
        int32_t cells[4][2];
        (void)cells;
        geo_lb_from_hilbert(d, GRID, cells, &rot, &dx, &dy);
        rots[d] = (uint8_t)rot;
    }

    uint32_t revs = rr_count_reversals(rots, N);

    /* Hilbert curve on 16×16 has known structure — count should be > 0
       (curve reverses at fractal boundaries) and < N (not every step) */
    int ok = (revs > 0 && revs < N);
    CHECK(8, "Hilbert 16×16 has reversals (0 < count < N)", ok);
    printf("        reversals: %u / %d\n", revs, N);
    return ok;
}

/* T9: deterministic — same sequence → same gate results */
static int test_deterministic(void) {
    uint8_t seq[] = {0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 0, 1};
    uint32_t n = sizeof(seq) / sizeof(seq[0]);

    uint32_t revs1 = rr_count_reversals(seq, n);
    uint32_t revs2 = rr_count_reversals(seq, n);
    int ok = (revs1 == revs2);
    CHECK(9, "deterministic: same sequence → same reversal count", ok);
    printf("        count=%u\n", revs1);
    return ok;
}

/* T10: all rotation triples produce valid states */
static int test_exhaustive(void) {
    uint32_t counts[3] = {0, 0, 0};  /* HOLD, FORWARD, REVERSED */
    for (uint8_t a = 0; a < RR_ROTS; a++) {
        for (uint8_t b = 0; b < RR_ROTS; b++) {
            for (uint8_t c = 0; c < RR_ROTS; c++) {
                RRState s = rr_gate(a, b, c);
                counts[s]++;
            }
        }
    }
    int ok = (counts[RR_HOLD] > 0 && counts[RR_FORWARD] > 0 && counts[RR_REVERSED] > 0);
    CHECK(10, "exhaustive: all 3 states appear in 64 triples", ok);
    printf("        HOLD=%u FORWARD=%u REVERSED=%u\n",
           counts[RR_HOLD], counts[RR_FORWARD], counts[RR_REVERSED]);
    return ok;
}

int main(void) {
    printf("═══ test_rr_gate — Rotation-Reversal Gate ═══\n\n");

    test_basics();
    test_rot_dist();
    test_forward_sequence();
    test_reversed();
    test_hold();
    test_starting();
    test_stopping();
    test_pair_gate();
    test_hilbert_reversals();
    test_deterministic();
    test_exhaustive();

    printf("\n═══════════════════════════════════════\n");
    printf("RESULT: %d PASS / %d FAIL\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
