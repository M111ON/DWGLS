/* test_peano_overlay.c — experiment (owner 2026-10-10):
 * "ยิง 64x9 ทับกับ peano จะมีอะไรเกิดขึ้น" — chain-overlay the nine 64-bit
 * cell patterns in traversal order and check what happens.
 *
 * Setup: 9 cell patterns from aw_cell_pattern_w (fixed seed, full width).
 * Overlay op is deliberately NON-commutative (FNV-1a-style: acc = acc*P ^ w),
 * because XOR/sum are commutative — traversal order CANNOT matter through
 * them (the sum control below proves it: identical across orders, exactly).
 * Orders: row-major, Peano-9 S (bottom-left start, first approximant), col-major.
 *
 * Peano-9 order (flat row-major cell indices, row 0 = top):
 *   bottom row L->R, middle R->L, top L->R  = {6,7,8,5,4,3,0,1,2}.
 * Convention fixed here; the test asserts it is a Hamiltonian path (each
 * step moves exactly one cell orthogonally) and covers 0..8 once.
 *
 * What is measured over 64 fixed seeds: determinism, order divergence,
 * letter flips ((agg*37)%52 per order), mean hamming distance, and the
 * sum-control (must be order-blind, exactly).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "anchor_weight.h"

#define NSEEDS 64

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; }
    else { g_fail++; printf("  FAIL: %s\n", name); }
}

/* traversal orders over the 9 cells (flat row-major indices) */
static const int ORD_ROW[9] = {0,1,2,3,4,5,6,7,8};
static const int ORD_PEANO[9] = {6,7,8,5,4,3,0,1,2};
static const int ORD_COL[9] = {0,3,6,1,4,7,2,5,8};

static void fill(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b[i] = (uint8_t)s; }
}

/* non-commutative chain: traversal geometry reaches the aggregate */
static uint64_t chain_overlay(uint64_t w9[9], const int *ord) {
    uint64_t acc = 1469598103934665603ULL;
    for (int i = 0; i < 9; i++) {
        acc ^= w9[ord[i]];
        acc *= 1099511628211ULL;
    }
    acc ^= acc >> 29; acc *= 0xBF58476D1CE4E5B9ULL; acc ^= acc >> 32;
    return acc;
}

/* commutative control: geometry-blind by construction */
static uint64_t sum_overlay(uint64_t w9[9], const int *ord) {
    uint64_t acc = 0;
    for (int i = 0; i < 9; i++) acc += w9[ord[i]];
    return acc;
}

static int hamming(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    int n = 0;
    while (x) { n += (int)(x & 1u); x >>= 1; }
    return n;
}

static void t_peano_is_hamiltonian(void) {
    int seen[9] = {0};
    int ok = 1;
    for (int i = 0; i < 9; i++) {
        if (ORD_PEANO[i] < 0 || ORD_PEANO[i] > 8 || seen[ORD_PEANO[i]]) { ok = 0; break; }
        seen[ORD_PEANO[i]] = 1;
        if (i > 0) {
            int pr = ORD_PEANO[i-1] / 3, pc = ORD_PEANO[i-1] % 3;
            int cr = ORD_PEANO[i] / 3, cc = ORD_PEANO[i] % 3;
            int dr = pr > cr ? pr - cr : cr - pr;
            int dc = pc > cc ? pc - cc : cc - pc;
            if (dr + dc != 1) { ok = 0; break; }
        }
    }
    check(ok, "Peano-9 {6,7,8,5,4,3,0,1,2} is a Hamiltonian path over 3x3");
}

static void t_chain_deterministic(void) {
    uint8_t s[32];
    fill(s, sizeof s, 0xB1EAu);
    uint64_t w9[9];
    for (int c = 0; c < 9; c++) w9[c] = aw_cell_pattern_w(s, sizeof s, c, 64);
    int ok = (chain_overlay(w9, ORD_PEANO) == chain_overlay(w9, ORD_PEANO)) &&
             (chain_overlay(w9, ORD_ROW) == chain_overlay(w9, ORD_ROW));
    check(ok, "chain overlay deterministic per order");
}

static void t_sum_is_order_blind(void) {
    int ok = 1;
    for (uint32_t sd = 1; sd <= NSEEDS && ok; sd++) {
        uint8_t s[32];
        fill(s, sizeof s, sd * 0x9E37u + 11u);
        uint64_t w9[9];
        for (int c = 0; c < 9; c++) w9[c] = aw_cell_pattern_w(s, sizeof s, c, 64);
        if (sum_overlay(w9, ORD_ROW) != sum_overlay(w9, ORD_PEANO) ||
            sum_overlay(w9, ORD_ROW) != sum_overlay(w9, ORD_COL)) ok = 0;
    }
    check(ok, "sum aggregate identical across orders (control: commutative = blind)");
}

static uint64_t bitrev64(uint64_t x) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) { r <<= 1; r |= (x & 1u); x >>= 1; }
    return r;
}

/* nibble-reverse a 16-bit value (bit j <-> bit 15-j) */
static uint16_t nibrev16(uint16_t x) {
    uint16_t r = 0;
    for (int i = 0; i < 16; i++) { r <<= 1; r |= (uint16_t)(x & 1u); x >>= 1; }
    return r;
}

/* inversion check (owner: "16:9 ถ้าข้างใน inverted"):
 * reverse all 64 bits inside each cell, then ask what survives.
 * Theorem (exact): fold16(bitrev(p)) == nibble_rev(fold16(p)) — reversal
 * permutes whole nibbles, and OR inside a nibble is order-blind. Hence
 * popcount is preserved per cell AND on the fold: the [##] weight HOLDS
 * while the letter (sum aggregate) MOVES. Measured below. */
static void t_inversion(void) {
    int fold_holds = 1, w_holds = 1;
    int letter_flips = 0;
    for (uint32_t sd = 1; sd <= NSEEDS; sd++) {
        uint8_t s[32];
        fill(s, sizeof s, sd * 0x9E37u + 11u);
        uint64_t w9[9], v9[9];
        for (int c = 0; c < 9; c++) {
            w9[c] = aw_cell_pattern_w(s, sizeof s, c, 64);
            v9[c] = bitrev64(w9[c]);
        }
        for (int c = 0; c < 9; c++) {
            uint16_t f_full = 0, f_inv = 0;
            for (int i = 0; i < 16; i++) {
                if (((w9[c] >> (i*4)) & 0xFu) != 0) f_full |= (uint16_t)(1u << i);
                if (((v9[c] >> (i*4)) & 0xFu) != 0) f_inv |= (uint16_t)(1u << i);
            }
            if (f_inv != nibrev16(f_full)) fold_holds = 0;
            int pw = 0, pv = 0;
            { uint16_t t = f_full; while (t) { pw += t & 1u; t >>= 1; } }
            { uint16_t t = f_inv; while (t) { pv += t & 1u; t >>= 1; } }
            if (pw != pv) w_holds = 0;
        }
        uint32_t agg_w = 0, agg_v = 0;
        for (int c = 0; c < 9; c++) {
            uint16_t fw = 0, fv = 0;
            for (int i = 0; i < 16; i++) {
                if (((w9[c] >> (i*4)) & 0xFu) != 0) fw |= (uint16_t)(1u << i);
                if (((v9[c] >> (i*4)) & 0xFu) != 0) fv |= (uint16_t)(1u << i);
            }
            agg_w += fw; agg_v += fv;
        }
        if (((agg_w * 37u) % 52u) != ((agg_v * 37u) % 52u)) letter_flips++;
    }
    printf("  [measure] inversion: fold-theorem holds=%d weight-holds=%d letter-flips %d/%d\n",
           fold_holds, w_holds, letter_flips, NSEEDS);
    check(fold_holds, "fold16(bitrev) == nibble_rev(fold16) on all 9x64 cells");
    check(w_holds, "weight popcount survives bit-reversal (the [##] holds)");
}

static void t_orders_diverge(void) {
    int div_rp = 0, div_rc = 0, div_pc = 0;
    long ham_rp = 0, ham_rc = 0, ham_pc = 0;
    int flip_rp = 0;
    for (uint32_t sd = 1; sd <= NSEEDS; sd++) {
        uint8_t s[32];
        fill(s, sizeof s, sd * 0x9E37u + 11u);
        uint64_t w9[9];
        for (int c = 0; c < 9; c++) w9[c] = aw_cell_pattern_w(s, sizeof s, c, 64);
        uint64_t r = chain_overlay(w9, ORD_ROW);
        uint64_t p = chain_overlay(w9, ORD_PEANO);
        uint64_t c = chain_overlay(w9, ORD_COL);
        if (r != p) div_rp++;
        if (r != c) div_rc++;
        if (p != c) div_pc++;
        ham_rp += hamming(r, p); ham_rc += hamming(r, c); ham_pc += hamming(p, c);
        /* letter impact through the same (x*37)%52 selector the anchor uses */
        if (((r * 37u) % 52u) != ((p * 37u) % 52u)) flip_rp++;
    }
    printf("  [measure] diverge row/peano %d/%d  row/col %d/%d  peano/col %d/%d\n",
           div_rp, NSEEDS, div_rc, NSEEDS, div_pc, NSEEDS);
    printf("  [measure] mean hamming r/p %.1f  r/c %.1f  p/c %.1f (of 64 bits)\n",
           (double)ham_rp / NSEEDS, (double)ham_rc / NSEEDS, (double)ham_pc / NSEEDS);
    printf("  [measure] letter flips row->peano %d/%d\n", flip_rp, NSEEDS);
    check(div_rp == NSEEDS && div_rc == NSEEDS && div_pc == NSEEDS,
          "traversal order changes the chained aggregate (all seeds)");
    check(flip_rp >= NSEEDS - 4, "letter flips on most seeds (order reaches the letter)");
}

int main(void) {
    printf("test_peano_overlay — 64x9 chained in traversal order\n\n");
    t_peano_is_hamiltonian();
    t_chain_deterministic();
    t_sum_is_order_blind();
    t_orders_diverge();
    t_inversion();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
