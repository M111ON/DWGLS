/*
 * test_dual_loop.c — Ico(20) <-> Dodec(12) alternation
 * ═══════════════════════════════════════════════════════════════════════════
 * Oracles: hand-computed XOR (python, hardcoded) + incidence definition +
 * the proven round-trip identity. A wrong merge/split goes red.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_dual_loop tests/test_dual_loop.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include "dual_loop.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(void) {
    printf("═ DUAL LOOP — 20 seeds <-> 12 homes ═\n");

    /* ── L1: incidence definition — every vertex touches exactly 5 faces ── */
    {
        uint32_t cnt[12] = {0};
        for (uint32_t f = 0; f < 20u; f++)
            for (uint32_t e = 0; e < 3u; e++) cnt[GP16N_ICO_FACES[f][e]]++;
        int ok = 1;
        for (uint32_t v = 0; v < 12u; v++)
            if (cnt[v] != 5u) { ok = 0; break; }
        CHECK("L1: 12 verts x 5 faces (merge 5->1 is exact)", ok);
    }

    /* ── L2: hand oracle — vertex 0 sees faces 0..4 ──
     * seeds 0x11..0x55: 1^2^3^4^5 = 1 -> homes[0] = 0x11111111 */
    {
        uint32_t seeds[20] = {0};
        seeds[0] = 0x11111111u; seeds[1] = 0x22222222u;
        seeds[2] = 0x33333333u; seeds[3] = 0x44444444u;
        seeds[4] = 0x55555555u;
        uint32_t homes[12];
        dual_seed_to_home(seeds, homes);
        CHECK("L2: homes[0] = 0x11111111 (hand XOR)", homes[0] == 0x11111111u);
    }

    /* ── L3: conservation — XOR(homes) == XOR(seeds) ── */
    {
        uint32_t seeds[20], homes[12];
        for (uint32_t i = 0; i < 20u; i++)
            seeds[i] = 0x9E3779B9u * (i + 1u) + 0x12345678u;
        dual_seed_to_home(seeds, homes);
        uint32_t xs = 0, xh = 0;
        for (uint32_t i = 0; i < 20u; i++) xs ^= seeds[i];
        for (uint32_t v = 0; v < 12u; v++) xh ^= homes[v];
        CHECK("L3: XOR conserved across merge", xs == xh);
    }

    /* ── L4: settle idempotent — merge(split(h)) == h ── */
    {
        uint32_t homes[12], seeds[20], back[12];
        for (uint32_t v = 0; v < 12u; v++)
            homes[v] = 0x85EBCA6Bu * (v * 3u + 1u) + 0x27D4EB2Fu;
        dual_home_to_seed(homes, seeds);
        dual_seed_to_home(seeds, back);
        int ok = 1;
        for (uint32_t v = 0; v < 12u; v++)
            if (back[v] != homes[v]) { ok = 0; break; }
        CHECK("L4: merge o split = identity (homes are fixed points)", ok);
    }

    /* ── L5: asymmetric — split o merge != id (seeds 1..20 witness) ── */
    {
        uint32_t seeds[20], homes[12], again[20];
        for (uint32_t i = 0; i < 20u; i++) seeds[i] = i + 1u;
        dual_seed_to_home(seeds, homes);
        dual_home_to_seed(homes, again);
        int same = 1;
        for (uint32_t i = 0; i < 20u; i++)
            if (again[i] != seeds[i]) { same = 0; break; }
        uint32_t xs = 0, xh = 0;
        for (uint32_t i = 0; i < 20u; i++) xs ^= seeds[i];
        for (uint32_t v = 0; v < 12u; v++) xh ^= homes[v];
        CHECK("L5: traveling wave mixes (no perpetual motion), XOR still kept",
              !same && xs == xh);
    }

    /* ── L6: NULL guards ── */
    {
        uint32_t s[20] = {0}, h[12] = {0};
        dual_seed_to_home(0, h);
        dual_seed_to_home(s, 0);
        dual_home_to_seed(0, s);
        dual_home_to_seed(h, 0);
        CHECK("L6: NULL tolerated", 1);
    }

    /* ── L7: 32-unit view — hand boundaries + exhaustive roundtrip ──
     * flat 20735: face = 31 (31x648 = 20088), local 647 = 7x81 + 80. */
    {
        int ends = (d32_face(0) == 0u && d32_wheel(0) == 0u &&
                    d32_ladder(0) == 0u && d32_face(20735u) == 31u &&
                    d32_wheel(20735u) == 7u && d32_ladder(20735u) == 80u &&
                    d32_flat(31u, 7u, 80u) == 20735u);
        int ok = ends;
        for (uint32_t f = 0; ok && f < 20736u; f++) {
            uint32_t fa = d32_face(f), w = d32_wheel(f), l = d32_ladder(f);
            if (fa >= 32u || w >= 8u || l >= 81u) { ok = 0; break; }
            if (d32_flat(fa, w, l) != f) { ok = 0; break; }
        }
        CHECK("L7: 32x(8x81) view exhaustive (20736/20736, bounds hold)", ok);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
