/*
 * test_tess_full_cycle.c — Full 20736-cycles: which walk covers the field
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Question: does any walk in the system cover all 20736 slots as ONE cycle?
 *
 * Candidates:
 *   tetra-12 (additive +12)   → gcd(12,20736)=12     → 12 orbits × 1728  ❌
 *   MOD-5 (multiplicative ×5) → order of 5 = 1728    → 4 max orbits     ❌
 *   CAPO-144 (additive +144)  → gcd(144,20736)=144   → 144 cycles ×144  ❌
 *   Z-walk (step (1,−1))      → 144-cycles (torus)   → 144 cycles ×144  ❌
 *   a_w view                  → permutation of Z₁₄₄  → 144-permutation  ❌
 *   additive +5               → gcd(5,20736)=1       → ONE 20736-cycle ✅
 *   additive +37              → gcd(37,20736)=1      → ONE 20736-cycle ✅
 *
 * THE ANSWER: the additive walk n → n+s mod 20736 is a full cycle iff
 * gcd(s, 20736) = 1, i.e. s is coprime to 2⁸·3⁴ — s odd and s not divisible
 * by 3 (s ≡ 1 or 5 mod 6). The system's OWN frame-seek stride 37 satisfies
 * this (37 ≡ 1 mod 12 too → the cycle visits the 12 tetra orbits in strict
 * rotation), and geo_jump's stride 5 does as well — additively. The
 * "37 ผิดโดเมน 20736" note from the geo_jump explorer applies to the
 * MULTIPLICATIVE MOD walk (order 576), not to the additive walk.
 *
 * Proof:
 *   T1  no candidate (tetra-12 / MOD-5 / CAPO-144 / Z-walk / a_w) is a full cycle
 *   T2  additive orbit size = 20736/gcd(s,20736) — walk-verified s=1..255
 *   T3  full cycle ⇔ gcd(s,20736)=1 ⇔ (s odd ∧ 3∤s) — verified s=1..1023
 *   T4  stride-37 IS a full cycle — 20736 steps, returns to seed; also full
 *       on the 1440 frame-seek domain (gcd(37,1440)=1)
 *   T5  stride-37 ≡ 1 mod 12 — the full cycle visits the 12 tetra orbits in
 *       strict rotation (node_k ≡ k mod 12)
 *   T6  additive +5 is a full cycle too (geo_jump's stride, additive form)
 *   T7  Hamiltonian on the torus — the stride-37 walk visits all 20736 cells
 *       of the 144×144 (w,pos) grid exactly once and closes (no repeats)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_full_cycle tests/test_tess_full_cycle.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_sync_bridge.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint32_t gcd_u(uint32_t a, uint32_t b) {
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

/* additive walk orbit size from seed 0 */
static uint32_t add_orbit_size(uint32_t s) {
    uint32_t n = 0, cnt = 0;
    do { n = (n + s) % GSB_FULL; cnt++; } while (n != 0u);
    return cnt;
}

int main(void) {
    printf("═ FULL 20736-CYCLES — which walk covers the field in one loop ═\n");
    printf("  additive full cycle ⇔ gcd(s, 20736) = 1 (s odd, 3 ∤ s)\n\n");

    /* ── T1: candidates are NOT full cycles ───────────────────────────── */
    {
        CHECK("T1a: tetra-12 (additive +12) = 12 orbits × 1728 — not a full cycle",
              add_orbit_size(12u) == 1728u && 1728u != GSB_FULL);
        /* MOD(5) multiplicative — order of 5 mod 20736 = 1728 from seed 1 */
        {
            uint32_t n = 1u, cnt = 0;
            do { n = (n * 5u) % GSB_FULL; cnt++; } while (n != 1u);
            CHECK("T1b: MOD-5 (multiplicative ×5) = 1728 (max orbit) — not a full cycle",
                  cnt == 1728u && cnt != GSB_FULL);
        }
        CHECK("T1c: CAPO-144 (additive +144) = 144 cycles × 144 — not a full cycle",
              add_orbit_size(144u) == 144u && 144u != GSB_FULL);
        /* Z-walk: step (1,−1) on (w,pos) — 144-cycle per z-line. z = w+p stays
         * CONSTANT along the walk, so track the position (w) instead. */
        {
            uint8_t seen[GSW_LOCAL];
            memset(seen, 0, sizeof(seen));
            uint32_t w = 0, p = 0, cnt = 0;
            do { w = (w + 1u) % GSW_LOCAL; p = (p + GSW_LOCAL - 1u) % GSW_LOCAL;
                 if (seen[w]) break;
                 seen[w] = 1; cnt++; } while (!(w == 0u && p == 0u));
            CHECK("T1d: Z-walk (step (1,−1)) = 144-cycle (one z-line) — not a full cycle",
                  cnt == 144u);
        }
        /* a_w view: permutation of Z₁₄₄ — a 144-permutation, not a 20736 walk */
        {
            GSWScale s;
            gsw_scale_init(&s);
            int ok = 1;
            for (uint32_t w = 0; w < GSW_LOCAL && ok; w++) {
                uint8_t seen[GSW_LOCAL];
                memset(seen, 0, sizeof(seen));
                for (uint32_t l = 0; l < GSW_LOCAL; l++) {
                    uint32_t p = gsw_view(&s, w, l);
                    if (seen[p]) { ok = 0; break; }
                    seen[p] = 1;
                }
            }
            CHECK("T1e: a_w view is a 144-permutation (local axis) — not a 20736-cycle walk",
                  ok);
        }
    }

    /* ── T2: additive orbit size = 20736/gcd(s,20736) — walk-verified ─── */
    {
        int ok = 1;
        for (uint32_t s = 1; s <= 255u && ok; s++)
            if (add_orbit_size(s) != GSB_FULL / gcd_u(s, GSB_FULL)) ok = 0;
        CHECK("T2: additive orbit size = 20736/gcd(s,20736) — walk-verified for all s ∈ [1,255]",
              ok);
    }

    /* ── T3: full cycle ⇔ gcd=1 ⇔ (s odd ∧ 3∤s) ──────────────────────── */
    {
        int ok = 1;
        for (uint32_t s = 1; s <= 1023u && ok; s++) {
            int full = (add_orbit_size(s) == GSB_FULL);
            int expect = (gcd_u(s, GSB_FULL) == 1u);
            int parity = ((s & 1u) != 0u && (s % 3u) != 0u);
            if (full != expect || expect != parity) ok = 0;
        }
        CHECK("T3: full cycle ⇔ gcd(s,20736)=1 ⇔ s odd ∧ 3∤s — verified for all s ∈ [1,1023]",
              ok);
    }

    /* ── T4: stride-37 IS a full cycle ────────────────────────────────── */
    {
        uint8_t seen[GSB_FULL];
        memset(seen, 0, sizeof(seen));
        uint32_t n = 0, cnt = 0;
        int uniq = 1;
        do {
            if (seen[n]) uniq = 0;
            seen[n] = 1;
            n = (n + 37u) % GSB_FULL;
            cnt++;
        } while (n != 0u);
        CHECK("T4a: additive +37 = ONE full 20736-cycle — every node exactly once, returns to seed",
              cnt == GSB_FULL && uniq);
        CHECK("T4b: stride-37 also full on the frame-seek domain — gcd(37,1440)=1",
              gcd_u(37u, 1440u) == 1u);
    }

    /* ── T5: 37 ≡ 1 mod 12 — the cycle visits the 12 tetra orbits in rotation ── */
    {
        int ok = 1;
        for (uint32_t k = 0; k < GSB_FULL && ok; k++) {
            uint32_t node = (k * 37u) % GSB_FULL;
            if (node % 12u != k % 12u) ok = 0;     /* node_k ≡ k mod 12 */
        }
        CHECK("T5: stride-37 ≡ 1 mod 12 — the full cycle steps the 12 tetra orbits in strict rotation",
              ok);
    }

    /* ── T6: additive +5 is a full cycle too ──────────────────────────── */
    {
        CHECK("T6: additive +5 (geo_jump's stride, additive form) = full 20736-cycle — gcd(5,20736)=1",
              add_orbit_size(5u) == GSB_FULL && gcd_u(5u, GSB_FULL) == 1u);
    }

    /* ── T7: Hamiltonian cycle on the 144×144 torus ───────────────────── */
    {
        uint8_t seen[GSB_FULL];
        memset(seen, 0, sizeof(seen));
        uint32_t n = 0, cnt = 0;
        int uniq = 1;
        do {
            uint32_t slot = gsw_scale_of_node(n) * GSW_LOCAL + gsw_pos_of_node(n);
            if (seen[slot]) uniq = 0;
            seen[slot] = 1;
            n = (n + 37u) % GSB_FULL;
            cnt++;
        } while (n != 0u);
        uint32_t filled = 0;
        for (uint32_t i = 0; i < GSB_FULL; i++) filled += seen[i];
        CHECK("T7: stride-37 is a Hamiltonian cycle on the (w,pos) torus — all 20736 cells exactly once, closed",
              uniq && filled == GSB_FULL && cnt == GSB_FULL);
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
