/*
 * test_geo_inner_field.c — Inner-field digit-extension (spec 2026-09-13)
 *
 * Oracle policy: expected values come from the SET {0..20735} itself
 * (bitmap coverage) and from counts — never from the impl under test.
 * Composition path uses geo_tess_wiring.h (independent code).
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_geo_inner_field tests/test_geo_inner_field.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_inner_field.h"
#include "../core/geo_tess_wiring.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(void) {
    printf("═ INNER FIELD — digit-extension nesting (address-only) ═\n");

    /* ── T0: constants (spec arithmetic, independent literals) ─────── */
    CHECK("T0: 16*9==144, 256*81==20736", IF_Q_BRANCH * IF_L_CELL == 144u &&
          IF_HI_BASE * IF_LO_BASE == 20736u);
    CHECK("T0b: L1 total 20736*8*18==144^3==2985984",
          (uint32_t)20736u * 8u * 18u == 2985984u && IF_L1_TOTAL == 2985984u);
    CHECK("T0c: L2 slots 20736^3==12^12", IF_L2_SLOTS == 8916100448256ull &&
          IF_L2_SLOTS == (uint64_t)20736u * 20736u * 20736u);

    /* ── T1: slot split roundtrip, all 144 ─────────────────────────── */
    {
        int ok = 1;
        for (uint32_t s = 0; s < 144u; s++) {
            uint32_t q, l;
            if_slot_split(s, &q, &l);
            if (q >= 16u || l >= 9u || if_slot(q, l) != s) { ok = 0; break; }
        }
        CHECK("T1: slot<->(q,l) roundtrip all 144 (q<16,l<9)", ok);
    }

    /* ── T2: refine coverage — bitmap oracle, NOT impl ─────────────── */
    {
        static uint8_t seen[20736];
        memset(seen, 0, sizeof(seen));
        int ok = 1;
        for (uint32_t s = 0; s < 144u && ok; s++)
            for (uint32_t q2 = 0; q2 < 16u && ok; q2++)
                for (uint32_t l2 = 0; l2 < 9u && ok; l2++) {
                    uint32_t inner = if_inner_refine(s, q2, l2);
                    if (inner >= 20736u || seen[inner]) { ok = 0; break; }
                    seen[inner] = 1;
                }
        uint32_t n = 0;
        for (uint32_t i = 0; i < 20736u; i++) n += seen[i];
        CHECK("T2: 144*144 refines cover [0,20736) exactly once (20736/20736)",
              ok && n == 20736u);
    }

    /* ── T3: parent projection — coarse-side roundtrip + 144:1 ─────── */
    {
        int ok = 1, counts[144] = { 0 };
        for (uint32_t inner = 0; inner < 20736u; inner++) {
            uint32_t p = if_inner_parent(inner);
            if (p >= 144u) { ok = 0; break; }
            counts[p]++;
            /* coarse-side: re-refine parent's (q,l) with inner's fine
             * digits must reproduce inner (independent recompute) */
            uint32_t Q = inner / 81u, L = inner % 81u;
            uint32_t q = Q / 16u, l = L / 9u;
            uint32_t q2 = Q % 16u, l2 = L % 9u;
            if (if_inner_from(q, l, q2, l2) != inner) { ok = 0; break; }
        }
        int even = 1;
        for (uint32_t s = 0; s < 144u; s++) if (counts[s] != 144) { even = 0; break; }
        CHECK("T3: parent(refine)==prefix all + exactly 144 children/slot", ok && even);
    }

    /* ── T4: parent is LOSSY — witness pair, same parent ───────────── */
    {
        uint32_t a = if_inner_refine(0u, 0u, 0u);
        uint32_t b = if_inner_refine(0u, 1u, 0u);
        CHECK("T4: lossy witness — a!=b but parent(a)==parent(b)==0",
              a != b && if_inner_parent(a) == 0u && if_inner_parent(b) == 0u);
    }

    /* ── T5: composition through tess_to_flat/flat_to_tess ─────────── */
    {
        int ok = 1;
        for (uint32_t t = 0; t < 18u && ok; t += 7)
            for (uint32_t c = 0; c < 8u && ok; c++)
                for (uint32_t s = 0; s < 144u && ok; s++) {
                    uint32_t inner = if_inner_refine(s, (s * 7u) % 16u, (s * 5u) % 9u);
                    uint32_t p = if_inner_parent(inner);
                    uint32_t flat = tess_to_flat(t, c, p);
                    uint32_t rt, rc, rl;
                    flat_to_tess(flat, &rt, &rc, &rl);
                    if (rt != t || rc != c || rl != s || p != s) { ok = 0; break; }
                }
        CHECK("T5: parent∘refine==slot through tess_to_flat/flat_to_tess", ok);
    }

    /* ── T6: L2 pack/unpack — samples + boundary + lex order ───────── */
    {
        int ok = 1;
        uint32_t xs[] = { 0u, 1u, 143u, 144u, 6912u, 20735u };
        for (unsigned i = 0; i < 6 && ok; i++)
            for (unsigned j = 0; j < 6 && ok; j++)
                for (unsigned k = 0; k < 6 && ok; k++) {
                    uint32_t X, Y, Z;
                    if_l2_unpack(if_l2_pack(xs[i], xs[j], xs[k]), &X, &Y, &Z);
                    if (X != xs[i] || Y != xs[j] || Z != xs[k]) { ok = 0; break; }
                }
        uint32_t X, Y, Z;
        if_l2_unpack(IF_L2_SLOTS - 1u, &X, &Y, &Z);
        int bound = (X == 20735u && Y == 20735u && Z == 20735u) &&
                    if_l2_pack(20735u, 20735u, 20735u) == IF_L2_SLOTS - 1u;
        int lex = if_l2_pack(0u, 0u, 1u) < if_l2_pack(0u, 1u, 0u) &&
                  if_l2_pack(0u, 1u, 0u) < if_l2_pack(1u, 0u, 0u);
        CHECK("T6: L2 roundtrip 216 samples + max==20736^3-1 + lex order",
              ok && bound && lex);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
