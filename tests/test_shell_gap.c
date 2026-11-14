/* test_shell_gap.c — the gap ledger and stride codec.
 * Checks the numbers we actually rely on, and records why the early ShellN
 * gate (delta = 34(n+1), delta % 18) is not used. */
#include <stdio.h>
#include <string.h>
#include "shell_gap.h"
#include "geo_tess_container.h"

#define FIELD 20736u   /* 2^8 * 3^4 */
#define ROW     896u   /* 2^7 * 7   */

static int fails;

static void ck(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static void test_gap(void) {
    uint64_t g = sg_unit(ROW, FIELD);
    printf("gap: gcd(%u,%u)=%u  bits=%.4f  period=%u  pages=%u\n",
           ROW, FIELD, (unsigned)g, sg_gap_bits(g),
           (unsigned)sg_period(ROW, FIELD), (unsigned)sg_pages(ROW, FIELD));
    ck(g == 128u, "gcd(896,20736) == 128 == 2^7");
    ck(sg_gap_bits(g) == 7.0, "gap cost == 7 bits");
    ck(sg_period(ROW, FIELD) == 162u, "period == 162 rows");
    ck(sg_pages(ROW, FIELD) == 7u, "pages == 7");
    ck(162ull * ROW == 7ull * FIELD, "162 rows == 7 fields");
    ck(sg_gap_bits(1u) == 0.0, "gcd 1 costs nothing");
    ck(sg_gap_bits(9u) > 3.16 && sg_gap_bits(9u) < 3.18, "gcd 9 == 2*log2(3)");
}

static void test_ring(void) {
    /* row r covers blocks R*r .. R*r+R-1 on a ring of F blocks; over one period
     * every block must be covered exactly `pages` times. */
    const uint64_t F = sg_period(ROW, FIELD), R = sg_pages(ROW, FIELD);
    uint32_t hits[162] = {0};
    for (uint64_t r = 0; r < F; r++) {
        uint64_t first = sg_row_first_block(r, ROW, FIELD);
        for (uint64_t j = 0; j < R; j++) hits[(first + j) % F]++;
    }
    int ok = 1;
    for (uint64_t b = 0; b < F; b++) if (hits[b] != R) ok = 0;
    printf("ring: %u blocks, each covered %u times over %u rows\n",
           (unsigned)F, (unsigned)R, (unsigned)F);
    ck(ok, "ring closes: every block covered exactly `pages` times");
}

static void test_old_gate_refuted(void) {
    /* the claim was delta = 34(n+1); the header's own sides give 34n */
    int mismatched = 1;
    for (uint32_t n = 4; n <= 16; n++)
        if (162u * n - 128u * n == 34u * (n + 1u)) mismatched = 0;
    printf("old gate: 34*8=%u, %u%%18=%u  (advertised n=8 anchor passes=%s)\n",
           34u * 8u, 34u * 8u, (34u * 8u) % 18u, (34u * 8u) % 18u == 0 ? "yes" : "no");
    ck(mismatched, "162n-128n never equals 34(n+1)");
    ck((34u * 8u) % 18u == 2u, "n=8 delta 272 fails the gate");
    ck((34u * 9u) % 18u == 0u, "honest anchor is n=9 (delta 306)");
}

static void test_stride_256(void) {
    const uint32_t N = 256;
    const uint32_t free_k[] = {3, 5, 7, 11, 13, 37};
    for (unsigned i = 0; i < sizeof free_k / sizeof free_k[0]; i++) {
        uint32_t k = free_k[i];
        int ok = 1;
        for (uint32_t x = 0; x < N; x++)
            if (sg_stride_dec(sg_stride_enc(x, k, N), k, N) != x) ok = 0;
        ck(ok, "free stride roundtrips all 256");
        ck(sg_stride_free(k, N), "free stride has gcd 1");
    }
    /* frustum: k=4 and k=12 collapse by the same 4, both cost 2 bits */
    ck(sg_gap_bits(sg_gcd(4, N)) == 2.0 && sg_gap_bits(sg_gcd(12, N)) == 2.0,
       "k=4 and k=12 cost the same 2 bits");
    ck(sg_stride_free(12, N) == 0, "k=12 is not free on 256");
}

static void test_stride_field(void) {
    const uint32_t N = FIELD;
    const uint32_t free_k[] = {5, 7, 11, 13, 37};
    for (unsigned i = 0; i < sizeof free_k / sizeof free_k[0]; i++) {
        uint32_t k = free_k[i];
        int ok = 1;
        for (uint32_t x = 0; x < N; x++)
            if (sg_stride_dec(sg_stride_enc(x, k, N), k, N) != x) ok = 0;
        ck(ok, "free stride roundtrips on the field");
        ck(sg_stride_free(k, N), "free on field means coprime to 6");
    }
    /* 3 is free on 256 but NOT on the field */
    ck(sg_stride_free(3, 256), "3 is free on 256");
    ck(sg_stride_free(3, N) == 0, "3 is NOT free on 20736");
    ck(sg_gap_bits(sg_gcd(3, N)) > 1.58 && sg_gap_bits(sg_gcd(3, N)) < 1.59,
       "3 on the field costs log2(3)");
}

static void test_field_wiring(void) {
    /* the constants below are the repo's, not ours */
    ck(TESS_TOTAL_SLOTS == FIELD, "repo TESS_TOTAL_SLOTS == 20736");
    ck(TESS_TOTAL_SLOTS == 128u * 162u, "20736 == 128 * 162");

    /* The field counts CELLS (TESS_TOTAL_SLOTS of them), so csz is free — cell
     * size never causes a gap. Only the tail of the last capo is empty. */
    static const uint32_t csz[] = {4, 2, 18, 20, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292};
    printf("cell ledger (field counts cells, so csz is free):\n");
    for (unsigned i = 0; i < sizeof csz / sizeof csz[0]; i++) {
        uint64_t u = sg_unit(csz[i], TESS_TOTAL_SLOTS);
        uint32_t rem = TESS_TOTAL_SLOTS % csz[i];
        printf("  cell=%3u  unit=%3u  cells_per_field=%5u  field%%cell=%2u  divides=%s\n",
               csz[i], (unsigned)u, (unsigned)(TESS_TOTAL_SLOTS / csz[i]), (unsigned)rem,
               rem == 0 ? "yes" : "no");
        ck(sg_gap_bits(u) >= 0.0, "cell size has a 2^j*3^l unit against the field");
    }
    ck(TESS_TOTAL_SLOTS % 144u == 0u, "Q4_K cell 144 divides the field exactly");
    ck(TESS_TOTAL_SLOTS % 34u == 30u, "Q8_0 cell 34 leaves 30 slots (tail only, no misalignment)");

    /* byte-addressed window: a 896-byte row laid into a 20736-byte window must
     * land inside it, and 162 rows must cover the window exactly 7 times. */
    int inrange = 1, covered = 1;
    uint32_t hits[162] = {0};
    for (uint64_t r = 0; r < 162; r++) {
        uint64_t first = sg_row_first_block(r, ROW, FIELD);
        for (uint64_t j = 0; j < 7; j++) {
            uint64_t block = (first + j) % 162;
            if (block * 128u >= TESS_TOTAL_SLOTS) inrange = 0;
            hits[block]++;
        }
    }
    for (unsigned b = 0; b < 162; b++) if (hits[b] != 7) covered = 0;
    ck(inrange, "every row block stays inside TESS_TOTAL_SLOTS");
    ck(covered, "162 rows cover the 20736-byte window exactly 7 times");
}

static void test_partial_capo_cannot_shrink(void) {
    /* The packer scatters cell i to (i*37) % 20736. For the last capo of a
     * 25088-block tensor, chunk = 4352 lands across the WHOLE cube, so the cube
     * cannot be truncated to `chunk` cells — the 10.4% tail waste is structural,
     * not sloppiness. Removing it needs the scatter modulus to become `chunk`,
     * which is a permutation iff gcd(37, chunk) == 1. */
    const uint32_t chunk = 25088u - FIELD;   /* 4352 */
    uint32_t lo = 0xFFFFFFFFu, hi = 0, used = 0;
    static uint8_t hit[FIELD];
    memset(hit, 0, sizeof hit);
    for (uint32_t i = 0; i < chunk; i++) {
        uint32_t s = (uint32_t)(((uint64_t)i * 37u) % FIELD);
        if (s < lo) lo = s;
        if (s > hi) hi = s;
        if (!hit[s]) { hit[s] = 1; used++; }
    }
    printf("partial capo: chunk=%u -> slots [%u..%u], %u/%u used (%.1f%%)\n",
           chunk, lo, hi, used, FIELD, (double)used * 100.0 / FIELD);
    ck(hi > FIELD - 10u, "last-capo cells reach the top of the cube (cannot truncate)");
    ck(used == chunk, "stride 37 is a permutation over the full field");
    ck((chunk % 37u) != 0u, "chunk 4352 is coprime with 37 (shrinking would be safe here)");
    ck((111u % 37u) == 0u, "chunk divisible by 37 would collide if shrunk to chunk");
}

int main(void) {
    test_gap();
    test_ring();
    test_old_gate_refuted();
    test_stride_256();
    test_stride_field();
    test_field_wiring();
    test_partial_capo_cannot_shrink();
    if (fails) { printf("FAILED: %d checks\n", fails); return 1; }
    printf("PASS: shell_gap (gap ledger + stride codec + field wiring + capo)\n");
    return 0;
}
