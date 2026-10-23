/* test_addr_orbit.c — item-3 gate for the address-system draft (note #15).
 *
 * Pins the locked math BEFORE any header implements it. Every expectation
 * below is hand-computed from the spec, never from a function under test.
 * Complements tests/test_wang_latch.c (which drives the latch header);
 * this file tests NO header — pure integer oracles a future header must satisfy.
 *
 * T12-T15 pin the SHIFT algebra (hourglass figure, 2026-10-23): the reflection
 * through the shared centre is d^1 (exact, composes); the naive arithmetic
 * shift (i<<1)%20736 is NOT a permutation; the 2^8 lane rotation is.
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

    /* T12 naive shift: (i<<1) % 20736 is NOT a permutation. 20736 is even, so
     * every odd slot is unreachable — the image is exactly half the field. */
    static uint8_t seen_shift[20736];
    long shift_img = 0;
    for (uint32_t i = 0; i < 20736; i++) {
        uint32_t j = (i << 1) % 20736u;
        if (!seen_shift[j]) { seen_shift[j] = 1; shift_img++; }
    }
    CHECK(shift_img == 10368, "T12 (i<<1)%20736 covers 10368/20736 (half lost)");

    /* T13 lane split 20736 = 2^8 x 81: rotating the 8-bit lane is exact and
     * its inverse round-trips every address (nothing to store). */
    static uint8_t seen_lane[20736];
    int t13 = 1;
    for (uint32_t i = 0; i < 20736 && t13; i++) {
        uint32_t u = i / 81u, v = i % 81u;            /* u in [0,256), v in [0,81) */
        uint32_t r = ((u << 1) | (u >> 7)) & 0xFFu;   /* rot8 left  */
        uint32_t j = r * 81u + v;
        uint32_t back = (((r >> 1) | (r << 7)) & 0xFFu) * 81u + v;
        if (j >= 20736u || seen_lane[j] || back != i) t13 = 0;
        else seen_lane[j] = 1;
    }
    CHECK(t13, "T13 2^8 lane rot8 = bijection on 20736 with exact inverse");

    int t13b = 1;
    for (uint32_t u = 0; u < 256; u++) {
        uint32_t r = u;
        for (int k = 0; k < 8; k++) r = ((r << 1) | (r >> 7)) & 0xFFu;
        if (r != u) { t13b = 0; break; }
    }
    CHECK(t13b, "T13 rot8^8 = identity (256/256 lane)");

    /* T14 hourglass reflection. Point reflection through the shared centre
     * swaps the two ends of a pair => d^1. 12 is even, so this stays in range
     * for every digit: involution, 6 pairs, 0 fixed points (the fixed point is
     * the centre itself, which is NOT a slot). The face rotation d^6 (face+3)
     * is the non-uniform one: 12 = 4x3 is not a power of two, so it leaves
     * [0,12) once face >= 4. */
    int t14 = 1;
    for (uint32_t d = 0; d < 12; d++) {
        uint32_t e = d ^ 1u;
        if (e >= 12u || (e ^ 1u) != d || e == d) t14 = 0;
    }
    CHECK(t14, "T14 d^1 involution in [0,12) for all 12 (6 pairs, 0 fixed)");

    int t14b = 0;
    for (uint32_t d = 0; d < 12; d++) if ((d ^ 6u) < 12u) t14b++;
    CHECK(t14b == 8, "T14 d^6 (face+3) in range 8/12 only (f<4)");

    /* T15 composition: k crossings of the centre collapse to one XOR — the
     * jump is O(1) and needs no table, no walk (contrast: stride-37 multiply). */
    int t15 = 1;
    for (uint32_t d = 0; d < 12; d++)
        for (int a = 0; a < 6; a++) {
            uint32_t x = d;
            for (int k = 0; k < a; k++) x ^= 1u;
            if (x != (d ^ (uint32_t)(a & 1))) t15 = 0;
        }
    CHECK(t15, "T15 k flips collapse to d^(k&1)");

    printf(fails ? "ADDR-ORBIT: %d FAIL\n" : "ADDR-ORBIT: ALL PASS\n", fails);
    return fails != 0;
}
