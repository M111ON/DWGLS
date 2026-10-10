/* tests/test_anchor_weight.c — self-check for include/anchor_weight.h.
 *
 * Oracles (independent of the header):
 *   pattern: determinism (rebuild -> memcmp) and cell divergence
 *            (the 9 patterns must not be all-identical, centre included).
 *   overlay: cell pattern == RDH(+off) XOR RDH(-off) recomposed from the
 *            halves, and the overlay is non-trivial (differs from each half).
 *   fold:    64->16->4->1 recomputed inline from the raw pattern (9 cells).
 *   letter:  the 6 directions must be pairwise distinct (spec: "6 ทิศต้องต่าง"),
 *            their case-folded pairs (%26) distinct too, and the letter pool
 *            walk must be a bijection over 52 for dir = 0..51 (stride 37
 *            coprime to 52).
 *   weight:  popcount of the 16-fold, range check 0..16 (9 cells);
 *            weight[AW_CENTER] is the centre letter's [##].
 *   pair:    A/a share pair id (letter % 26: 0 and 26 -> pair 0, 51 -> 25);
 *            case bit does not enter the pattern.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Iinclude -o build/test_anchor_weight tests/test_anchor_weight.c -lm
 * RUN:   ./build/test_anchor_weight
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "anchor_weight.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

/* independent fold oracle */
static uint16_t oracle_fold16(uint64_t p) {
    uint16_t o = 0;
    for (int i = 0; i < 16; i++) if (((p >> (i*4)) & 0xF) != 0) o |= (uint16_t)(1u << i);
    return o;
}
static uint8_t oracle_fold4(uint16_t p) {
    uint8_t o = 0;
    for (int i = 0; i < 4; i++) if (((p >> (i*4)) & 0xF) != 0) o |= (uint8_t)(1u << i);
    return o;
}
static int oracle_pop16(uint16_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static void fill(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b[i] = (uint8_t)s; }
}

static void t_build_deterministic(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0xC0FFEEu);
    aw_anchor_t a, b;
    aw_anchor_build(s, sizeof s, 1, &a);
    aw_anchor_build(s, sizeof s, 1, &b);
    check(memcmp(&a, &b, sizeof a) == 0, "build is deterministic (same bytes -> same anchor)");
}

static void t_pattern_slots_diverge(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0xBEEFu);
    aw_anchor_t a;
    aw_anchor_build(s, sizeof s, 0, &a);
    int all_same = 1;
    for (int i = 1; i < (int)AW_CELLS; i++)
        if (a.pattern[i] != a.pattern[0]) { all_same = 0; break; }
    check(!all_same, "9 cells produce different 64-bit patterns");
    int any_nonzero = 0;
    for (int i = 0; i < (int)AW_CELLS; i++) if (a.pattern[i]) any_nonzero = 1;
    check(any_nonzero, "patterns are non-trivial (not all zero)");
}

static void t_pm_overlay(void) {
    /* composition oracle: cell == plus-half XOR minus-half, recomposed here
     * from the halves (not via aw_rdh_pattern's own XOR line). */
    uint8_t s[64];
    fill(s, sizeof s, 0xA5A5u);
    int ok = 1, nontrivial = 0;
    for (int c = 0; c < (int)AW_CELLS; c++) {
        uint64_t plus = aw_rdh_half(s, sizeof s, c, +1);
        uint64_t minus = aw_rdh_half(s, sizeof s, c, -1);
        aw_anchor_t a;
        aw_anchor_build(s, sizeof s, 0, &a);
        if (a.pattern[c] != (plus ^ minus)) { ok = 0; break; }
        if (a.pattern[c] != plus && a.pattern[c] != minus) nontrivial = 1;
    }
    check(ok, "cell pattern = RDH(+off) XOR RDH(-off)");
    check(nontrivial, "overlay is non-trivial (differs from each half)");
}

static void t_fold_correct(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0x1234u);
    aw_anchor_t a;
    aw_anchor_build(s, sizeof s, 0, &a);
    int ok = 1;
    for (int i = 0; i < (int)AW_CELLS; i++) {
        if (a.fold16[i] != oracle_fold16(a.pattern[i])) { ok = 0; break; }
        if (a.fold4[i]  != oracle_fold4(a.fold16[i]))   { ok = 0; break; }
        if (a.fold1[i]  != (a.fold4[i] ? 1u : 0u))      { ok = 0; break; }
    }
    check(ok, "64->16->4->1 fold matches independent oracle (9 cells)");
}

static void t_six_directions_distinct(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0xABCDu);
    aw_anchor_t a;
    aw_anchor_build(s, sizeof s, 0, &a);
    int distinct = 1;
    for (int i = 0; i < 6 && distinct; i++)
        for (int j = i + 1; j < 6; j++)
            if (a.dir_letter[i] == a.dir_letter[j]) { distinct = 0; break; }
    check(distinct, "the 6 centre directions are pairwise distinct");
}

static void t_six_pairs_distinct(void) {
    /* the stride-37 walk must also keep the 6 case-folded pairs distinct:
     * 37k mod 52 never hits {0,26} for k=1..5, so no two of 6 dirs share a
     * pair — verified here on real builds across seeds, not just arithmetic. */
    int ok = 1;
    for (uint32_t seed = 1; seed <= 8 && ok; seed++) {
        uint8_t s[64];
        fill(s, sizeof s, 0xF00u + seed);
        aw_anchor_t a;
        aw_anchor_build(s, sizeof s, 0, &a);
        for (int i = 0; i < 6 && ok; i++)
            for (int j = i + 1; j < 6; j++)
                if ((a.dir_letter[i] % AW_PAIRS) == (a.dir_letter[j] % AW_PAIRS))
                    ok = 0;
    }
    check(ok, "the 6 directions' pairs (%26) are pairwise distinct (8 seeds)");
}

static void t_letter_pool_bijection(void) {
    /* walk 52 steps by AW_STRIDE: must cover all 52 letters exactly once */
    char seen[AW_LETTERS];
    memset(seen, 0, sizeof seen);
    int ok = 1;
    for (uint32_t i = 0; i < AW_LETTERS; i++) {
        uint32_t l = (i * AW_STRIDE) % AW_LETTERS;
        if (l >= AW_LETTERS || seen[l]) { ok = 0; break; }
        seen[l] = 1;
    }
    for (uint32_t i = 0; i < AW_LETTERS && ok; i++) if (!seen[i]) ok = 0;
    check(ok, "stride-37 walk over 52 letters is a bijection (no repeat pool)");
}

static void t_weight_range(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0x5555u);
    aw_anchor_t a;
    aw_anchor_build(s, sizeof s, 0, &a);
    int ok = 1;
    for (int i = 0; i < (int)AW_CELLS; i++) {
        if (a.weight[i] != (uint8_t)oracle_pop16(a.fold16[i])) { ok = 0; break; }
        if (a.weight[i] > 16) { ok = 0; break; }
    }
    check(ok, "weight = popcount(fold16), range 0..16 (9 cells)");
    check(a.weight[AW_CENTER] == (uint8_t)oracle_pop16(a.fold16[AW_CENTER]),
          "centre letter carries its own [##] (same popcount rule)");
}

static void t_case_pair(void) {
    /* the RULE (not self-agreement): pair = letter % 26, so A(0) and a(26)
     * are pair 0, z(51) is pair 25. Case lives in the letter, identity in
     * the pair; weights ride per side. */
    aw_anchor_t t;
    memset(&t, 0, sizeof t);
    t.letter = 0;
    int pA = aw_pair_id(&t);
    t.letter = 26;
    int pa = aw_pair_id(&t);
    t.letter = 51;
    int pz = aw_pair_id(&t);
    t.letter = 1;
    int pB = aw_pair_id(&t);
    check(pA == 0 && pa == 0, "A(0) and a(26) share pair 0");
    check(pz == 25, "z(51) is pair 25");
    check(pA != pB, "A and B are different pairs");
    /* the branch: same seed, both cases -> same pair id, case bit recorded */
    uint8_t s[64];
    fill(s, sizeof s, 0x9999u);
    aw_anchor_t up, lo;
    aw_anchor_build(s, sizeof s, 1, &up);
    aw_anchor_build(s, sizeof s, 0, &lo);
    check(aw_pair_id(&up) == aw_pair_id(&lo),
          "same seed both cases -> same pair id");
    check(up.is_upper == 1 && lo.is_upper == 0, "case bit recorded");
}

static void t_weight_bias(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0x2468u);
    aw_anchor_t a;
    aw_anchor_build(s, sizeof s, 0, &a);
    check(aw_weight_bias(&a, 0) == (int)a.weight[0] - 8, "bias = weight - 8");
    check(aw_weight_bias(&a, 8) == (int)a.weight[AW_CENTER] - 8, "bias covers centre slot");
    check(aw_weight_bias(&a, -1) == 0 && aw_weight_bias(&a, 99) == 0, "bias out-of-range = 0");
    check(aw_weight_bias(NULL, 0) == 0, "bias NULL anchor = 0");
}

static void t_width_prefix(void) {
    /* the rung promise: narrow bits are a PREFIX of the full 64 (group
     * salts depend only on g), so small spaces never open full width. */
    uint8_t s[64];
    fill(s, sizeof s, 0xB16Bu);
    aw_anchor_t full, n16, n4, n1;
    int ok = 1;
    if (aw_anchor_build_w(s, sizeof s, 0, 64, &full) != 0) ok = 0;
    if (aw_anchor_build_w(s, sizeof s, 0, 16, &n16) != 0) ok = 0;
    if (aw_anchor_build_w(s, sizeof s, 0, 4, &n4) != 0) ok = 0;
    if (aw_anchor_build_w(s, sizeof s, 0, 1, &n1) != 0) ok = 0;
    for (int c = 0; c < (int)AW_CELLS && ok; c++) {
        if (n16.pattern[c] != (full.pattern[c] & 0xFFFFu)) ok = 0;
        if (n4.pattern[c] != (full.pattern[c] & 0xFu)) ok = 0;
        if (n1.pattern[c] != (full.pattern[c] & 0x1u)) ok = 0;
    }
    check(ok, "narrow captures are prefixes of full 64 (16/4/1)");
}

static void t_width_rungs(void) {
    /* each rung: deterministic, width recorded, weight bounded by rung,
     * letter in range, fold chain consistent. */
    static const int rungs[4] = {64, 16, 4, 1};
    static const int wmax[4] = {16, 16, 4, 1};
    int ok = 1;
    for (int ri = 0; ri < 4 && ok; ri++) {
        uint8_t s[64];
        fill(s, sizeof s, 0x100u + (uint32_t)ri);
        aw_anchor_t a, b;
        if (aw_anchor_build_w(s, sizeof s, 1, rungs[ri], &a) != 0) { ok = 0; break; }
        if (aw_anchor_build_w(s, sizeof s, 1, rungs[ri], &b) != 0) { ok = 0; break; }
        if (memcmp(&a, &b, sizeof a) != 0) { ok = 0; break; }
        if (a.width != rungs[ri]) { ok = 0; break; }
        if (a.letter >= AW_LETTERS) { ok = 0; break; }
        for (int c = 0; c < (int)AW_CELLS && ok; c++) {
            if (a.pattern[c] != (a.pattern[c] & aw_width_mask(rungs[ri]))) ok = 0;
            if (a.weight[c] != (uint8_t)oracle_pop16(a.fold16[c])) ok = 0;
            if (a.weight[c] > wmax[ri]) ok = 0;
            if (a.fold4[c] != oracle_fold4(a.fold16[c])) ok = 0;
            if (a.fold1[c] != (a.fold4[c] ? 1u : 0u)) ok = 0;
        }
    }
    check(ok, "all rungs deterministic, bounded, chain-consistent");
    /* pairs still distinct at narrow width (construction, any base) */
    {
        uint8_t s[64];
        fill(s, sizeof s, 0x77u);
        aw_anchor_t a;
        aw_anchor_build_w(s, sizeof s, 0, 16, &a);
        int distinct = 1;
        for (int i = 0; i < 6 && distinct; i++)
            for (int j = i + 1; j < 6; j++)
                if ((a.dir_letter[i] % AW_PAIRS) == (a.dir_letter[j] % AW_PAIRS))
                    distinct = 0;
        if (!distinct) ok = 0;
    }
    check(ok, "pairs distinct at width 16 too");
    aw_anchor_t a;
    uint8_t s[64];
    fill(s, sizeof s, 1u);
    int bad = 0;
    bad += (aw_anchor_build_w(s, sizeof s, 0, 32, &a) == -1);
    bad += (aw_anchor_build_w(s, sizeof s, 0, 8, &a) == -1);
    bad += (aw_anchor_build_w(s, sizeof s, 0, 0, &a) == -1);
    bad += (aw_anchor_build_w(s, sizeof s, 0, -1, &a) == -1);
    bad += (aw_anchor_build_w(s, sizeof s, 0, 64, NULL) == -1);
    check(bad == 5, "5/5 bad widths/args rejected");
}

int main(void) {
    printf("test_anchor_weight — RDH pattern wired into anchor\n\n");
    t_build_deterministic();
    t_pattern_slots_diverge();
    t_pm_overlay();
    t_fold_correct();
    t_six_directions_distinct();
    t_six_pairs_distinct();
    t_letter_pool_bijection();
    t_weight_range();
    t_case_pair();
    t_weight_bias();
    t_width_prefix();
    t_width_rungs();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
