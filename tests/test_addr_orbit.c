/* test_addr_orbit.c — item-3 gate for the address-system draft (note #15).
 *
 * Pins the locked math BEFORE any header implements it. Every expectation
 * below is hand-computed from the spec, never from a function under test.
 * Complements tests/test_wang_latch.c (which drives the latch header);
 * this file tests NO header — pure integer oracles a future header must satisfy.
 *
 * BUILD: gcc -O2 -Wall -o build/test-addr-orbit tests/test_addr_orbit.c
 */
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); \
                           else { printf("  FAIL %s\n", msg); fails++; } } while (0)

/* spec encode: addr = d0 + 12*d1 + 144*d2 + 1728*d3, di in [0,12) */
static uint32_t enc(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    return d0 + 12u * d1 + 144u * d2 + 1728u * d3;
}

typedef struct { int32_t score; uint32_t latch; } Cand;

/* FINAL-locked order: score desc, then latch id asc (deterministic, #803) */
static int cand_before(const Cand *a, const Cand *b) {
    if (a->score != b->score) return a->score > b->score;
    return a->latch < b->latch;
}

int main(void) {
    /* T1 cross pairs: 24 is the multiplier, never a sum (note #15 fix) */
    CHECK(6 * 4 == 24, "T1 6x4 = 24 (Wang x gen)");
    CHECK(3 * 8 == 24, "T1 3x8 = 24 (layers x octant)");
    CHECK(2 * 12 == 24, "T1 2x12 = 24 (orient x 12)");

    /* T2 144 identities */
    CHECK(12 * 12 == 144, "T2 12^2 = 144");
    CHECK(24 * 6 == 144, "T2 24x6 = 144");
    CHECK(16 * 9 == 144, "T2 4^2 x 3^2 = 144");

    /* T3 20736 identities (all hand-verified, machine re-checks) */
    CHECK(12 * 12 * 12 * 12 == 20736, "T3 12^4 = 20736");
    CHECK(144 * 144 == 20736, "T3 144^2 = 20736");
    CHECK(24 * 24 * 36 == 20736, "T3 24^2 x 36 = 20736");
    CHECK(256 * 81 == 20736, "T3 4^4 x 81 = 20736");
    CHECK((72 + 72) * 144 == 20736, "T3 (72+72) x 144 = 20736 (parens load-bearing)");

    /* T4 base-2/base-3 coexistence: 2^a x 3^b per generation */
    CHECK(4 * 3 == 12, "T4 gen1: 2^2 x 3 = 12");
    CHECK(16 * 9 == 144, "T4 gen2: 2^4 x 3^2 = 144");
    CHECK(64 * 27 == 1728, "T4 gen3: 2^6 x 3^3 = 1728");
    CHECK(256 * 81 == 20736, "T4 gen4: 2^8 x 3^4 = 20736");

    /* T5 address encode boundaries + hand spot vector */
    CHECK(enc(0, 0, 0, 0) == 0, "T5 origin = 0");
    CHECK(enc(11, 11, 11, 11) == 20735, "T5 max = 20735 ([0,20736))");
    CHECK(enc(5, 4, 3, 2) == 5 + 48 + 432 + 3456, "T5 spot (5,4,3,2) = 3941");
    CHECK(enc(5, 4, 3, 2) == 3941, "T5 spot value 3941");

    /* T6 digit split: face = d>>1 in [0,6), orient = d&1 — full table, hand-written */
    static const uint32_t face_exp[12] = {0,0,1,1,2,2,3,3,4,4,5,5};
    static const uint32_t ori_exp[12]  = {0,1,0,1,0,1,0,1,0,1,0,1};
    int t6 = 1;
    for (uint32_t d = 0; d < 12; d++)
        if ((d >> 1) != face_exp[d] || (d & 1u) != ori_exp[d]) t6 = 0;
    CHECK(t6, "T6 face/orient table for d in [0,12)");

    /* T7 full-field roundtrip: every addr decomposes and recomposes (20736 iters) */
    int t7 = 1;
    for (uint32_t a = 0; a < 20736; a++) {
        uint32_t d0 = a % 12, d1 = (a / 12) % 12, d2 = (a / 144) % 12, d3 = a / 1728;
        if (d0 > 11 || d1 > 11 || d2 > 11 || d3 > 11 || enc(d0, d1, d2, d3) != a) { t7 = 0; break; }
    }
    CHECK(t7, "T7 20736/20736 addrs roundtrip");

    /* T8 B-latch id space (spec-level; header behavior lives in test_wang_latch) */
    CHECK(143u * 72u + 71u == 10367u, "T8 max latch id 10367");
    CHECK(144u * 72u == 10368u, "T8 144 cells x 72 choices = 10368");
    CHECK(10368u - 2u == 10366u, "T8 minor pool 10366 (reserved {0,10367})");
    CHECK(10368u * 2u == 20736u, "T8 half-field 10368x2 = 20736");

    /* T9 selector order on hand vectors (x100 int scores, ties by latch asc) */
    Cand in[5] = { {85,700}, {90,100}, {85,200}, {90,50}, {70,10} };
    static const uint32_t order_exp[5] = { 50, 100, 200, 700, 10 }; /* latch ids out */
    /* insertion sort by cand_before */
    for (int i = 1; i < 5; i++) {
        Cand k = in[i]; int j = i - 1;
        while (j >= 0 && cand_before(&k, &in[j])) { in[j + 1] = in[j]; j--; }
        in[j + 1] = k;
    }
    int t9 = 1;
    for (int i = 0; i < 5; i++)
        if (in[i].latch != order_exp[i]) t9 = 0;
    CHECK(t9, "T9 order (90,50),(90,100),(85,200),(85,700),(70,10)");

    /* T10 fixed-K budget: top-4 of the sorted 5 = first four latches */
    int t10 = (in[0].latch == 50 && in[1].latch == 100 &&
               in[2].latch == 200 && in[3].latch == 700);
    CHECK(t10, "T10 budget K=4 keeps {50,100,200,700}");

    /* T11 angle lock, integer microdegrees only (no float in the gate) */
    CHECK(60 * 4 == 240, "T11 60x4 = 240");
    CHECK(112133377 + 127866623 == 240000000, "T11 arc pair = 240.0deg in udeg");

    printf(fails ? "ADDR-ORBIT: %d FAIL\n" : "ADDR-ORBIT: ALL PASS\n", fails);
    return fails != 0;
}
