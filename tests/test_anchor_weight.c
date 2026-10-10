/* tests/test_anchor_weight.c — self-check for include/anchor_weight.h.
 *
 * Oracles (independent of the header):
 *   pattern: determinism (rebuild -> memcmp) and slot divergence
 *            (the 8 patterns must not be all-identical).
 *   fold:    64->16->4->1 recomputed inline from the raw pattern.
 *   letter:  the 6 directions must be pairwise distinct (spec: "6 ทิศต้องต่าง")
 *            and the letter pool walk must be a bijection over 52 for
 *            dir = 0..51 (stride 37 coprime to 52).
 *   weight:  popcount of the 16-fold, range check 0..16.
 *   pair:    A/a share pair id; weight may differ (case bit does not enter
 *            the pattern, so weights match — the branch is available).
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
    for (int i = 1; i < (int)AW_OUTER; i++)
        if (a.pattern[i] != a.pattern[0]) { all_same = 0; break; }
    check(!all_same, "8 outer slots produce different 64-bit patterns");
    int any_nonzero = 0;
    for (int i = 0; i < (int)AW_OUTER; i++) if (a.pattern[i]) any_nonzero = 1;
    check(any_nonzero, "patterns are non-trivial (not all zero)");
}

static void t_fold_correct(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0x1234u);
    aw_anchor_t a;
    aw_anchor_build(s, sizeof s, 0, &a);
    int ok = 1;
    for (int i = 0; i < (int)AW_OUTER; i++) {
        if (a.fold16[i] != oracle_fold16(a.pattern[i])) { ok = 0; break; }
        if (a.fold4[i]  != oracle_fold4(a.fold16[i]))   { ok = 0; break; }
        if (a.fold1[i]  != (a.fold4[i] ? 1u : 0u))      { ok = 0; break; }
    }
    check(ok, "64->16->4->1 fold matches independent oracle");
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
    for (int i = 0; i < (int)AW_OUTER; i++) {
        if (a.weight[i] != (uint8_t)oracle_pop16(a.fold16[i])) { ok = 0; break; }
        if (a.weight[i] > 16) { ok = 0; break; }
    }
    check(ok, "weight = popcount(fold16), range 0..16");
}

static void t_case_pair(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0x9999u);
    aw_anchor_t up, lo;
    aw_anchor_build(s, sizeof s, 1, &up);
    aw_anchor_build(s, sizeof s, 0, &lo);
    /* case bit does not enter the pattern: same pair id, weights available
     * to differ per-walk (tombstone branch). Here they coincide. */
    check(aw_pair_id(&up) == aw_pair_id(&lo),
          "A/a share pair id (case-opposite pair = same identity)");
    check(up.is_upper == 1 && lo.is_upper == 0, "case bit recorded");
}

static void t_weight_bias(void) {
    uint8_t s[64];
    fill(s, sizeof s, 0x2468u);
    aw_anchor_t a;
    aw_anchor_build(s, sizeof s, 0, &a);
    check(aw_weight_bias(&a, 0) == (int)a.weight[0] - 8, "bias = weight - 8");
    check(aw_weight_bias(&a, -1) == 0 && aw_weight_bias(&a, 99) == 0, "bias out-of-range = 0");
}

int main(void) {
    printf("test_anchor_weight — RDH pattern wired into anchor\n\n");
    t_build_deterministic();
    t_pattern_slots_diverge();
    t_fold_correct();
    t_six_directions_distinct();
    t_letter_pool_bijection();
    t_weight_range();
    t_case_pair();
    t_weight_bias();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
