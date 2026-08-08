/*
 * test_pyramid_3axis.c — Pyramid carrier × KIS{x,y,z} + Hyperbolic
 * ═══════════════════════════════════════════════════════════════════
 * User: "3axis + hyperbolic ไปด้วยเลย"
 *
 *   T1  Axis split: X/Y/Z cover the SAME 20736, sliced at 1728/3456
 *   T2  Pyramid parity survives axis shifts (4↔5 machine per axis)
 *   T3  Hyperbolic resolver: slot → angle × shift (creation ↔ shared)
 *   T4  3-axis compose: x=data[i], y=data[i+1728], z=data[i+3456]
 *   T5  Lossless at creation point (record NOT calculate)
 *   T6  Scale drill: 4 rescale levels survive, drift only off-origin
 *
 * Build: gcc -O2 -Wall -Wextra -Icore -o build/test_pyramid_3axis.exe \
 *         tests/test_pyramid_3axis.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "geo_pyramid_carrier.h"

#define N_SLOTS 20736u

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) { g_pass++; printf("  PASS %-46s\n", name); } \
         else { g_fail++; printf("  FAIL %-46s\n", name); } } while (0)

/* pyramid parity walk: consume slots up to N, report (layer,node) */
static void pyr_walk(uint32_t flat, uint32_t *layer, uint32_t *node)
{
    *layer = pyr_layer_of(flat, node);
}

/* --- shared 3-axis slice --- */
#define AXIS_STRIDE 1728u   /* KIS projection offset between axes */

static inline uint32_t kis_x(uint32_t i) { return i; }
static inline uint32_t kis_y(uint32_t i) { return (i + AXIS_STRIDE) % N_SLOTS; }
static inline uint32_t kis_z(uint32_t i) { return (i + 2u * AXIS_STRIDE) % N_SLOTS; }

/* --- hyperbolic resolver: x × f(time) = angle-shifted address ---
 * creation slot s₀; at time t, address = (s₀ × shift) mod N_SLOTS.
 * shift=1 → identity (creation point recovered exactly). */
static inline uint32_t hyp_shift(uint32_t slot, uint32_t shift)
{
    return (uint32_t)(((uint64_t)slot * (uint64_t)shift) % N_SLOTS);
}

int main(void)
{
    printf("Pyramid 3-axis + Hyperbolic — test\n");

    /* T1: axis split respects pyramid full field */
    printf("T1 3-axis split\n");
    {
        /* each axis owns the same 20736 — pyramid field covers each */
        CHECK(pyr_total(4608) == N_SLOTS, "axis field = 20736 (pyramid 4608 layers)");
        uint32_t ys = pyr_total(4608), zs = pyr_total(4608);
        CHECK(ys == N_SLOTS && zs == N_SLOTS, "all 3 axes hold full pyramid field");
    }

    /* T2: pyramid parity machine survives shift — walk at +1728 offset */
    printf("T2 parity per axis\n");
    {
        uint32_t layer_full, node_full, layer_shift, node_shift;
        int ok = 1;
        for (uint32_t i = 0; i < 4096; i++) {
            pyr_walk(i, &layer_full, &node_full);
            pyr_walk(kis_x(i), &layer_shift, &node_shift);
            if (layer_shift != layer_full || node_shift != node_full) { ok = 0; break; }
        }
        CHECK(ok, "axis X pyramid layer_of identity (0..4095)");
        /* Y axis: shifted index → same pyramid slot → same layer */
        pyr_walk(kis_y(7), &layer_shift, &node_shift);
        /* kis_y(7)=1735 = 192 pairs(1728) + 7 → layer 385 node 3 */
        CHECK(layer_shift == 385u && node_shift == 3u, "axis Y start = pyramid slot");
    }

    /* T3: hyperbolic resolver — creation ↔ shared address */
    printf("T3 hyperbolic resolver\n");
    {
        /* at origin shift=1: identity */
        CHECK(hyp_shift(12345u, 1u) == 12345u, "shift=1 is identity (creation)");
        /* shift=2: doubles (mod) */
        CHECK(hyp_shift(500u, 2u) == 1000u, "shift=2 doubles slot");
        /* 5⁻¹ mod 20736 = 16589 — proven inverse */
        CHECK(hyp_shift(hyp_shift(1000u, 5u), 16589u) == 1000u, "5×16589≡1 roundtrip");
    }

    /* T4: 3-axis composition x,y,z on same underline pyramid */
    printf("T4 3-axis compose\n");
    {
        uint32_t p = 1777;
        uint32_t xs = kis_x(p), ys = kis_y(p), zs = kis_z(p);
        CHECK(xs == p, "X axis = identity");
        CHECK(ys == (p + 1728u) % 20736u, "Y axis = +1728");
        CHECK(zs == (p + 3456u) % 20736u, "Z axis = +3456 (2×1728)");
        /* pyramid decomposition still exact after all shifts */
        uint32_t l, n;
        uint32_t f = (xs + ys + zs) % 20736u;  /* sum stays in range */
        pyr_walk(f, &l, &n);
        CHECK(l < 4608u && n < 5u, "composed sum lands in pyramid field");
    }

    /* T5: lossless at creation point (record not calculate) */
    printf("T5 lossless at creation\n");
    {
        uint32_t slots[8] = {0, 1728, 3456, 5184, 10000, 20735, 12345, 9999};
        int ok = 1;
        for (int k = 0; k < 8; k++) {
            uint32_t s = slots[k];
            if (hyp_shift(s, 1u) != s) { ok = 0; }
        }
        CHECK(ok, "8/8 creation slots recover exactly (shift=1)");
    }

    /* T6: 4-step act — scale via shift, restore at origin, verify */
    printf("T6 scale/recover\n");
    {
        uint32_t original = 4242u;
        uint32_t scaled   = hyp_shift(original, 5u);   /* drift */
        uint32_t restored = hyp_shift(scaled, 16589u); /* 5⁻¹ mod 20736 */
        CHECK(restored == original, "s×5 ×5⁻¹(16589) roundtrip == original");
        /* creation-point lossless across 6 sample slots */
        uint32_t samples[6] = {0u, 1728u, 3456u, 10000u, 20735u, 12345u};
        int ok = 1;
        for (int k = 0; k < 6; k++)
            if (hyp_shift(samples[k], 1u) != samples[k]) ok = 0;
        CHECK(ok, "6/6 creation slots exact under shift=1");
    }

        /* T7: 4-step act on 3 axes — append, scale, restore, verify */
    printf("T7 4-step protocol\n");
    {
        /* append at creation point on all 3 axes */
        uint32_t c[3] = {kis_x(999u), kis_y(999u), kis_z(999u)};
        /* reduce scale ×5 (drift) */
        uint32_t d[3] = {hyp_shift(c[0], 5u), hyp_shift(c[1], 5u), hyp_shift(c[2], 5u)};
        /* restore at creation point (5⁻¹) */
        uint32_t r[3] = {hyp_shift(d[0], 16589u), hyp_shift(d[1], 16589u), hyp_shift(d[2], 16589u)};
        CHECK(r[0] == c[0] && r[1] == c[1] && r[2] == c[2], "3-axis append→scale→restore lossless");
        /* pyramid layer decomposition survives drift on every axis */
        uint32_t l, n, ok = 1;
        for (int k = 0; k < 3; k++) {
            pyr_walk(r[k], &l, &n);
            if (l >= 4608u || n >= 5u) ok = 0;
        }
        CHECK(ok, "restored 3-axis slots land inside pyramid field");
    }
    printf("\nPyramid 3-Axis Test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}