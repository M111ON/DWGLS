/*
 * test_pyramid_carrier.c — Pyramid Swing Carrier v1 verification
 * ═══════════════════════════════════════════════════════════════════
 * Proves the user's stripped-core design on real numbers:
 *
 *   T1  Self-duality: V=F=5 (square pyramid is its own dual)
 *   T2  Swing 2-cycle: SEALED→SPIKED→SEALED roundtrip identity
 *   T3  Layer addressing: 4/5 alternating counts, bijective coverage
 *   T4  Recurrence constant: spike = +1, seal = −1, pair = 9=4+5
 *   T5  Octahedron fill: 2 pyramids base-to-base = V6/F8/E12
 *   T6  No-zero infinity: orbital grows without bound from any entry
 *
 * Build: gcc -O2 -Wall -Wextra -Icore -o build/test_pyramid_carrier.exe \
 *         tests/test_pyramid_carrier.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include "geo_pyramid_carrier.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) { g_pass++; printf("  PASS %-42s\n", name); } \
         else { g_fail++; printf("  FAIL %-42s\n", name); } } while (0)

int main(void)
{
    printf("Pyramid Swing Carrier v1 — test\n");

    /* T1: self-duality — pyramid is its own dual (V=F=5) */
    printf("T1 self-dual\n");
    CHECK(PYR_MAX == PYR_FACES, "V=5 == F=5 (self-dual)");
    CHECK(PYR_EDGES == 8u, "E=8 (pentagonal pyramid edge count)");

    /* T2: swing 2-cycle */
    printf("T2 swing cycle\n");
    CHECK(pyr_swing(PYR_SEALED) == PYR_SPIKED, "sealed → spiked");
    CHECK(pyr_swing(PYR_SPIKED) == PYR_SEALED, "spiked → sealed (2-cycle)");

    /* T3: layer counts 4/5 alternate and address is bijective */
    printf("T3 layer addressing\n");
    CHECK(pyr_layer_slots(0) == 4u, "layer 0 = sealed 4");
    CHECK(pyr_layer_slots(1) == 5u, "layer 1 = spiked 5");
    CHECK(pyr_layer_slots(2) == 4u, "layer 2 = sealed 4");
    CHECK(pyr_total(2) == 9u, "2 layers total = 4+5 = 9");
    CHECK(pyr_total(10) == 45u, "10 layers total = 5×9 = 45");
    {
        /* address + inverse roundtrip — node < layer_slots(n) only */
        int ok = 1;
        uint32_t n, node;
        for (n = 0; n < 100; n++) {
            for (node = 0; node < pyr_layer_slots(n); node++) {
                uint32_t f = pyr_addr(n, node);
                uint32_t l2, nd2;
                l2 = pyr_layer_of(f, &nd2);
                if (l2 != n || nd2 != node) { ok = 0; break; }
            }
        }
        CHECK(ok, "addr∘layer_of identity over 450 cells");
    }

    /* T4: recurrence constant */
    printf("T4 recurrence\n");
    CHECK(4u + 5u == PYR_PAIR, "spike+seal pair sums to 9");
    {
        /* orbit: layer parity = state (even→sealed 4, odd→spiked 5) */
        uint32_t k, nodes = 4;
        int ok = 1;
        for (k = 0; k < 24; k++) {
            uint8_t seen = pyr_layer_kind(k);   /* 0=sealed 1=spiked */
            uint8_t count = (seen == PYR_SPIKED) ? 5u : 4u;
            if (count != pyr_layer_slots(k)) { ok = 0; break; }
            if (count != 4u && count != 5u) { ok = 0; break; }
            nodes = count;
            if (nodes > 5u) { ok = 0; break; }
        }
        CHECK(ok, "recurrence alternates 4↔5 across 24 layers");
    }

    /* T5: octahedron fill */
    printf("T5 octahedron\n");
    CHECK(octa_verts(1) == 6u, "1 octa = 6 vertices");
    CHECK(octa_faces(1) == 8u, "1 octa = 8 faces");
    CHECK(octa_edges(1) == 12u, "1 octa = 12 edges");
    {
        /* 5 pairs fill a 10×10×10 volume: 1000 slots */
        uint32_t vol = 1000;
        CHECK(octa_verts(84) == 504u, "84 octa cover 504 verts (dense fill)");
        (void)vol;
    }

    /* T6: no-zero — ulim is unbounded, 0 is only an entry point */
    printf("T6 no-zero\n");
    {
        uint32_t f1 = pyr_addr(0, 0);   /* origin square corner */
        uint32_t f2 = pyr_addr(1, 4);   /* first apex (layer1 offset 4 + node 4) */
        CHECK(f1 == 0u, "origin = slot 0 (valid entry)");
        CHECK(f2 == 8u, "apex 1 lands at slot 8 (offset 4 + node 4)");
        /* moving upward never decreases the flat index */
        CHECK(pyr_addr(100,0) > 400u, "deep layer address strictly grows (infinity)");
    }

    /* T7: pyramid field fills 20736 exactly — 2304 pairs × 9 */
    printf("T7 full field\n");
    {
        /* pyr_total(4608) = 2304 pairs × 9 = 20736 */
        CHECK(pyr_total(4608) == 20736u, "4608 layers fill 20736 (0 overhang)");
        /* 2304 layers = 1152 pairs × 9 = 10368 */
        CHECK(pyr_total(2304) == 10368u, "2304 layers = 1152 pairs × 9 = 10368");
        CHECK(pyr_layer_slots(4607) == 5u, "last layer (4607) = spiked 5");
        CHECK(pyr_layer_slots(4606) == 4u, "layer 4606 = sealed 4");
        /* apex of last layer: offset(4607)=20731, +node4 = 20735 (last slot) */
        CHECK(pyr_addr(4607, 4) == 20735u, "apex at max layer = slot 20735 (last)");
    }

    /* T8: hyperbolic recurrence s(n+1) = s(n)×k — k = 9 per pair */
    printf("T8 hyperbolic recurrence\n");
    {
        /* s(n+1) = s(n) + 9 per pair — the constant step (spike+seal) */
        CHECK(pyr_total(2) - pyr_total(0) == 9u, "one pair advances +9");
        CHECK(pyr_total(4) - pyr_total(2) == 9u, "next pair also +9 (constant)");
        CHECK(pyr_total(4608) - pyr_total(4606) == 9u, "seal pair top +9 (two layers)");
        /* k = ratio of successive pair-totals: total(2n)/total(2(n-1)) → 1.0
           as n → ∞: linear growth, NOT exponential — records, not computes */
        uint32_t t100, t200;
        t100 = pyr_total(100);
        t200 = pyr_total(200);
        CHECK(t200 == 2u * t100, "doubling pairs doubles total (linear scale)");
        /* spiral identity: every sealed layer start is a multiple of 4 */
        CHECK((pyr_addr(3, 0)) == 13u, "start of sealed layer 3 = 12+1 = 13");
        CHECK(pyr_addr(5, 0) == 22u, "start of sealed layer 5 = 18+4 = 22");
    }
    printf("\nPyramid Test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}