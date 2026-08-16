/* test_ghost_envelope.c — Block Envelope (§11.6 decided) + Auto Lift
 * ═══════════════════════════════════════════════════════════════════════════
 * Closes §11.6: MAX_EXPANSION_DEPTH = envelope_depth(gate) from the ROI
 * curve (same model as test_tess_leverage — the numbers ARE the contract).
 *
 *   T1  marginal ROI curve matches leverage test (17.5/8.5/4.0/1.75/0.625)
 *   T2  hard ceiling — beyond k=7 fp grows back → ROI < 0
 *   T3  envelope_depth: gate 1.0 → 5 | 2.0 → 4 | 0.5 → 6 | tiny → 7 (ceiling)
 *   T4  scale depth — ย่อฟรี (contraction = 0), ขยายจ่าย (expansion = distance)
 *   T5  needs_lift boundary: depth 5 ok / 6 lift (GATE=1); 4 ok / 5 lift (GATE=2)
 *   T6  ghost_lift_auto: within envelope → PLACE (nothing frozen)
 *   T7  ghost_lift_auto: beyond envelope → LIFT (frozen + route, lossless read)
 *   T8  knob moves the cliff — same depth-5 request: GATE=1 → PLACE, GATE=2 → LIFT
 *   T9  contraction never lifts (even far: from 10 → to 2)
 *   T10 deep request (from 3 → to 140) always lifts — still ONE log entry
 *   T11 error cases → GHOST_AUTO_ERR
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-ghost_envelope tests/test_ghost_envelope.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/geo_ghost_lift.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static void fill_pattern(uint8_t *buf, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 31 + seed * 7) & 0xFF);
}

/* ═══════════════════════════════════════════════════════════════
   T1 + T2 — marginal ROI curve (contract from test_tess_leverage)
   ═══════════════════════════════════════════════════════════════ */
static void test_roi_curve(void) {
    double r1 = ght_roi_step(1);
    double r2 = ght_roi_step(2);
    double r3 = ght_roi_step(3);
    double r4 = ght_roi_step(4);
    double r5 = ght_roi_step(5);
    printf("     roi_step: 1:%.2f 2:%.2f 3:%.2f 4:%.2f 5:%.2f 6:%.4f 7:%.1f\n",
           r1, r2, r3, r4, r5, ght_roi_step(6), ght_roi_step(7));

    CHECK(1, "roi_step(1) == 17.5", fabs(r1 - 17.5) < 1e-9);
    CHECK(1, "roi_step(2) == 8.5",  fabs(r2 - 8.5)  < 1e-9);
    CHECK(1, "roi_step(3) == 4.0",  fabs(r3 - 4.0)  < 1e-9);
    CHECK(1, "roi_step(4) == 1.75", fabs(r4 - 1.75) < 1e-9);
    CHECK(1, "roi_step(5) == 0.625",fabs(r5 - 0.625)< 1e-9);
    CHECK(1, "curve strictly decreasing (leverage แย่ลงตาม depth)",
          r1 > r2 && r2 > r3 && r3 > r4 && r4 > r5);
    CHECK(2, "hard ceiling: k≥7 fp grows back → ROI < 0",
          ght_roi_step(7) < 0.0 && ght_fp(8) > ght_fp(7));
    CHECK(2, "absolute fp minimum at k=7 (65)",
          ght_fp(7) == 65 && ght_fp(6) == 66 && ght_fp(8) == 68);
}

/* ═══════════════════════════════════════════════════════════════
   T3 — envelope_depth = MAX_EXPANSION_DEPTH (knob = gate)
   ═══════════════════════════════════════════════════════════════ */
static void test_envelope_depth(void) {
    printf("     envelope_depth: gate 1.0 → %u | 2.0 → %u | 0.5 → %u | tiny → %u\n",
           ght_envelope_depth(1.0), ght_envelope_depth(2.0),
           ght_envelope_depth(0.5), ght_envelope_depth(0.001));

    CHECK(3, "GATE=1.0 → depth 5 (k 4-5 เหมาะสมที่สุด)", ght_envelope_depth(1.0) == 5);
    CHECK(3, "GATE=2.0 → depth 4 (conservative — ห้าม depth 5)", ght_envelope_depth(2.0) == 4);
    CHECK(3, "GATE=0.5 → depth 6 (aggressive)", ght_envelope_depth(0.5) == 6);
    CHECK(3, "tiny gate → 7 (hard ceiling — ไม่เกิน 7 ได้)", ght_envelope_depth(0.001) == 7);
    CHECK(3, "gate 0 → 7 (ยังถูก ceiling จำกัด)", ght_envelope_depth(0.0) == 7);
}

/* ═══════════════════════════════════════════════════════════════
   T4 — scale depth: ย่อฟรี ขยายจ่าย
   ═══════════════════════════════════════════════════════════════ */
static void test_scale_depth(void) {
    CHECK(4, "same scale → depth 0", ght_scale_depth(5, 5) == 0);
    CHECK(4, "expansion 5→11 → depth 6", ght_scale_depth(5, 11) == 6);
    CHECK(4, "expansion 3→140 → depth 137", ght_scale_depth(3, 140) == 137);
    CHECK(4, "contraction 10→2 → depth 0 (ย่อฟรี)", ght_scale_depth(10, 2) == 0);
    CHECK(4, "contraction 140→3 → depth 0 (ฟรีแม้ไกล)", ght_scale_depth(140, 3) == 0);
}

/* ═══════════════════════════════════════════════════════════════
   T5 — needs_lift boundary
   ═══════════════════════════════════════════════════════════════ */
static void test_needs_lift(void) {
    /* GATE=1.0 → envelope 5 */
    CHECK(5, "depth 5 within envelope (no lift)", !ght_needs_lift(1.0, 5, 10));
    CHECK(5, "depth 6 exceeds envelope (lift)",   ght_needs_lift(1.0, 5, 11));
    /* GATE=2.0 → envelope 4 */
    CHECK(5, "GATE=2: depth 4 ok",  !ght_needs_lift(2.0, 5, 9));
    CHECK(5, "GATE=2: depth 5 → lift (cliff ขยับลง 1)", ght_needs_lift(2.0, 5, 10));
    /* contraction never lifts */
    CHECK(5, "contraction never lifts", !ght_needs_lift(1.0, 10, 2));
}

/* ═══════════════════════════════════════════════════════════════
   T6/T7/T8 — ghost_lift_auto end-to-end + knob moves the cliff
   ═══════════════════════════════════════════════════════════════ */
static void test_auto(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[32];
    fill_pattern(d, sizeof(d), 31);

    /* within envelope → PLACE — nothing frozen, no route */
    int r = ghost_lift_auto(&log, &rs, 1.0, 100, 5, 10, d, sizeof(d)); /* depth 5 */
    CHECK(6, "depth 5 @GATE=1 → PLACE", r == GHOST_AUTO_PLACE);
    CHECK(6, "PLACE → nothing frozen", rs.count == 0 && log.count == 0);

    /* beyond envelope → LIFT — frozen + route recorded */
    r = ghost_lift_auto(&log, &rs, 1.0, 101, 5, 11, d, sizeof(d)); /* depth 6 */
    CHECK(7, "depth 6 @GATE=1 → LIFT", r == GHOST_AUTO_LIFT);
    CHECK(7, "LIFT → frozen + route", rs.count == 1 && log.count == 1);
    uint32_t sz = 0;
    const void *got = ghost_read(&log, &rs, 101, 5, 11, &sz);
    CHECK(7, "lifted data readable lossless",
          got && sz == sizeof(d) && memcmp(got, d, sizeof(d)) == 0);

    /* knob moves the cliff: same depth-5 request, strict gate → LIFT */
    r = ghost_lift_auto(&log, &rs, 2.0, 102, 5, 10, d, sizeof(d)); /* depth 5 */
    CHECK(8, "same depth-5 @GATE=2 → LIFT (knob ขยับ cliff ลง 1)", r == GHOST_AUTO_LIFT);
    CHECK(8, "route recorded under strict gate", log.count == 2);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T9/T10 — contraction never lifts; deep request always lifts
   ═══════════════════════════════════════════════════════════════ */
static void test_edge_cases(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[16];
    fill_pattern(d, sizeof(d), 37);

    /* contraction even at the strictest gate → PLACE */
    int r = ghost_lift_auto(&log, &rs, 0.0, 200, 10, 2, d, sizeof(d));
    CHECK(9, "contraction (10→2) @GATE=0 → PLACE (ย่อฟรี)", r == GHOST_AUTO_PLACE);
    CHECK(9, "nothing frozen by contraction", rs.count == 0);

    /* deep expansion → LIFT at any gate, ONE telescope entry */
    r = ghost_lift_auto(&log, &rs, 1.0, 201, 3, 140, d, sizeof(d));
    CHECK(10, "from 3 → to 140 (depth 137) → LIFT", r == GHOST_AUTO_LIFT);
    CHECK(10, "still ONE log entry (telescope — ∝ events)", log.count == 1);
    CHECK(10, "deep ghost readable",
          ghost_read(&log, &rs, 201, 3, 140, NULL) != NULL);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   T11 — error cases
   ═══════════════════════════════════════════════════════════════ */
static void test_errors(void) {
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 64);

    uint8_t d[8];
    fill_pattern(d, sizeof(d), 41);

    CHECK(11, "NULL data → ERR", ghost_lift_auto(&log, &rs, 1.0, 300, 5, 20, NULL, 8) == GHOST_AUTO_ERR);
    CHECK(11, "size 0 → ERR", ghost_lift_auto(&log, &rs, 1.0, 300, 5, 20, d, 0) == GHOST_AUTO_ERR);
    CHECK(11, "NULL rs → ERR", ghost_lift_auto(&log, NULL, 1.0, 300, 5, 20, d, 8) == GHOST_AUTO_ERR);
    CHECK(11, "nothing frozen on error", rs.count == 0 && log.count == 0);

    rs_free(&rs);
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Block Envelope (§11.6 — decided) + Auto Lift\n");
    printf("════════════════════════════════════════════════════════\n\n");

    test_roi_curve();
    test_envelope_depth();
    test_scale_depth();
    test_needs_lift();
    test_auto();
    test_edge_cases();
    test_errors();

    printf("\n════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("════════════════════════════════════════════════════════\n");

    return fail == 0 ? 0 : 1;
}
