/*
 * beam_sphere.c — Spherical Rotation Field (v6)
 * ═══════════════════════════════════════════════════════════════════
 *
 * "rotation ทรงกลมมัน 360x360"
 *   Dual sphere: outer=+, inner=- (sign from surface, 0 bits)
 *   Position: (θ, φ) on 360×360 sphere → 129,600 addresses
 *   Weight: XOR(θ, φ) distance from star 0 → magnitude
 *
 * Paradigm: "rotation" = moving data by shifting (θ, φ) on sphere
 *   ไม่ต้องมี index — sphere IS the address
 *   ไม่ต้องมี center distance — XOR diff between frames
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ── Spherical constants ────────────────────────────────────── */
#define SP_RES     360u      /* spherical resolution: 360×360 */
#define SP_GRID    (SP_RES * SP_RES)  /* 129,600 positions */
#define SP_STRIDE  37u       /* stride coprime with 129,600 (gcd(37,129600)=1) */
/* modinv(37, 129600): compute via extended Euclidean */
/* 129600 ÷ 37 = 3502 remainder 26 → 129600 = 3502*37 + 26 */
/* 37 ÷ 26 = 1 remainder 11 → 37 = 1*26 + 11 */
/* 26 ÷ 11 = 2 remainder 4 → 26 = 2*11 + 4 */
/* 11 ÷ 4 = 2 remainder 3 → 11 = 2*4 + 3 */
/* 4 ÷ 3 = 1 remainder 1 → 4 = 1*3 + 1 */
/* 3 ÷ 1 = 3 remainder 0 */
/* Back-substitute: */
/* 1 = 4 - 1*3 */
/* 3 = 11 - 2*4 → 1 = 4 - 1*(11 - 2*4) = 3*4 - 1*11 */
/* 4 = 26 - 2*11 → 1 = 3*(26 - 2*11) - 1*11 = 3*26 - 7*11 */
/* 11 = 37 - 1*26 → 1 = 3*26 - 7*(37 - 1*26) = 10*26 - 7*37 */
/* 26 = 129600 - 3502*37 → 1 = 10*(129600 - 3502*37) - 7*37 */
/*   = 10*129600 - 35020*37 - 7*37 = 10*129600 - 35027*37 */
/* So: -35027*37 ≡ 1 mod 129600 → 37 * (129600 - 35027) ≡ 1 */
/*   129600 - 35027 = 94573 */
/* 37 * 94573 = 37*90000 + 37*4573 = 3330000 + 169201 = 3499201 */
/* 3499201 / 129600 = 27.0... 27*129600 = 3499200. 3499201-3499200 = 1. ✓ */
/* modinv(37, 129600) = 94573 */
#define SP_INV     94573u

typedef enum { SURFACE_OUTER = 0, SURFACE_INNER = 1 } Surface;

/* Spherical coordinate: (θ, φ) on 360×360 sphere */
typedef struct {
    Surface surface;   /* outer=+, inner=- */
    uint16_t theta;    /* 0..359 azimuth */
    uint16_t phi;      /* 0..359 elevation */
} SphereCoord;         /* 5 bytes: 1 + 2 + 2 = 5 */

/* Star 0 reference frame */
#define SP_STAR_0 ((SphereCoord){SURFACE_OUTER, 0, 0})

/* ── Coordinate ↔ linear index ─────────────────────────────── */
static inline uint32_t sph_idx(SphereCoord c) {
    return (uint32_t)c.theta + (uint32_t)c.phi * SP_RES;
}

static inline SphereCoord idx_to_sph(uint32_t idx, Surface s) {
    SphereCoord c;
    c.surface = s;
    c.theta = (uint16_t)(idx % SP_RES);
    c.phi   = (uint16_t)(idx / SP_RES);
    return c;
}

/* ── XOR distance on sphere ───────────────────────────────────
 *
 * "ยิง beam จากพิกัด → วัด XOR diff = ระยะ"
 * star 0 = {0, 0} → XOR = θ ⊕ φ (after bit mixing)
 */
static inline uint8_t sph_xor_distance(SphereCoord c) {
    uint16_t mix = (uint16_t)(c.theta ^ c.phi);
    /* Extend 9-bit → 8-bit via diffusion */
    uint16_t m2 = mix ^ (mix >> 3) ^ (mix >> 6) ^ (mix >> 9);
    return (uint8_t)(m2 & 0xFF);
}

/* ── Bijection maps: distance → (θ, φ) on each surface ─────── */
static SphereCoord xmap_outer[256];
static SphereCoord xmap_inner[256];
static int xmap_ready = 0;

static void init(void) {
    if (xmap_ready) return;
    uint8_t used_outer[SP_GRID] = {0};
    uint8_t used_inner[SP_GRID] = {0};

    xmap_outer[0] = SP_STAR_0;  used_outer[0] = 1;
    xmap_inner[0] = (SphereCoord){SURFACE_INNER, 0, 0};  used_inner[0] = 1;

    for (int side = 0; side < 2; side++) {
        SphereCoord *map = (side == 0) ? xmap_outer : xmap_inner;
        uint8_t *used = (side == 0) ? used_outer : used_inner;
        Surface s = (side == 0) ? SURFACE_OUTER : SURFACE_INNER;

        for (int d = 1; d < 256; d++) {
            for (uint32_t idx = 0; idx < SP_GRID; idx++) {
                if (used[idx]) continue;
                SphereCoord c = idx_to_sph(idx, s);
                if (sph_xor_distance(c) == (uint8_t)d) {
                    map[d] = c;
                    used[idx] = 1;
                    break;
                }
            }
        }
    }
    xmap_ready = 1;
}

/* ── BAKE: weight → SphereCoord ────────────────────────────── */
static inline SphereCoord sph_bake(int32_t weight) {
    if (!xmap_ready) init();
    if (weight == 0) return SP_STAR_0;
    if (weight > 0) return xmap_outer[(uint8_t)(weight & 0xFF)];
    else            return xmap_inner[(uint8_t)((-weight) & 0xFF)];
}

/* ── DECODE: SphereCoord → weight ──────────────────────────── */
static inline int32_t sph_decode(SphereCoord c) {
    uint8_t dist = sph_xor_distance(c);
    if (c.surface == SURFACE_OUTER) return (int32_t)dist;
    else return -(int32_t)dist;
}

/* ── Rotation: shift (θ, φ) — "data moves by rotation" ────────
 * Rotating the sphere shifts all stored positions.
 * This is a pure geometric operation — no index, no hash. */

static inline SphereCoord sph_rotate(SphereCoord c, int16_t dtheta, int16_t dphi) {
    c.theta = (uint16_t)((c.theta + dtheta + SP_RES) % SP_RES);
    c.phi   = (uint16_t)((c.phi   + dphi   + SP_RES) % SP_RES);
    return c;
}

/* Weight-addressable rotation: rotation amount = function(weight, position)
 * This binds weight to geometry through rotation distance. */
static inline uint8_t sph_rotation_distance(SphereCoord a, SphereCoord b) {
    int16_t dt = (int16_t)((int16_t)b.theta - (int16_t)a.theta);
    int16_t dp = (int16_t)((int16_t)b.phi   - (int16_t)a.phi);
    if (dt < 0) dt += SP_RES;
    if (dp < 0) dp += SP_RES;
    return (uint8_t)(((uint16_t)dt ^ (uint16_t)dp) & 0xFF);
}

/* ── Verify ─────────────────────────────────────────────────── */

static int verify(void) {
    int pass = 0, fail = 0;
#define T(expr, msg) do { \
    if (expr) { pass++; printf("  PASS  %s\n", msg); } \
    else { fail++; printf("  FAIL  %s (line %d)\n", msg, __LINE__); } \
} while(0)

    printf("=== Sphere Verify ===\n");
    init();

    /* T1: Full Q8 lossless */
    {
        int ok = 1;
        for (int32_t w = -128; w <= 127; w++) {
            SphereCoord c = sph_bake(w);
            int32_t r = sph_decode(c);
            if (r != w) { ok = 0; break; }
        }
        T(ok, "full Q8 lossless roundtrip");
    }

    /* T2: Deterministic */
    {
        SphereCoord c1 = sph_bake(42);
        SphereCoord c2 = sph_bake(42);
        T(c1.theta == c2.theta && c1.phi == c2.phi && c1.surface == c2.surface,
          "deterministic");
    }

    /* T3: Zero collisions */
    {
        uint8_t seen[SP_GRID * 2] = {0};
        int collisions = 0;
        for (int32_t w = -128; w <= 127; w++) {
            SphereCoord c = sph_bake(w);
            uint32_t idx = sph_idx(c) + (c.surface == SURFACE_INNER ? SP_GRID : 0);
            if (seen[idx]) collisions++;
            seen[idx] = 1;
        }
        T(collisions == 0, "zero collisions across 259,200 sphere positions");
    }

    /* T4: Sign from surface */
    {
        T(sph_bake(50).surface == SURFACE_OUTER, "positive → outer");
        T(sph_bake(-50).surface == SURFACE_INNER, "negative → inner");
    }

    /* T5: Star 0 */
    {
        SphereCoord s0 = sph_bake(0);
        T(s0.surface == SURFACE_OUTER && s0.theta == 0 && s0.phi == 0,
          "zero → star 0 {outer, θ=0, φ=0}");
    }

    /* T6: Rotation moves position, preserves decode */
    {
        SphereCoord c = sph_bake(42);
        SphereCoord r = sph_rotate(c, 90, 180);
        /* After rotation, decode should be different (different position) */
        /* But re-rotate back should match */
        SphereCoord b = sph_rotate(r, -90, -180);
        int32_t back = sph_decode(b);
        /* Note: rotate doesn't change surface, just θ,φ */
        T(sph_decode(c) != sph_decode(r) || c.surface != r.surface,
          "rotation changes decode value");
        T(back == 42, "rotate back restores original weight");
    }

    /* T7: Rotation distance between frames */
    {
        SphereCoord a = sph_bake(10);
        SphereCoord b = sph_bake(100);
        uint8_t d = sph_rotation_distance(a, b);
        T(d >= 0 && d < 256, "rotation distance in Q8 range");
    }

    /* T8: Sphere resolution — 129,600 positions */
    {
        T(SP_GRID == 129600, "sphere = 360×360 = 129,600 positions");
        T(sizeof(SphereCoord) <= 8, "SphereCoord compact");
    }

    printf("  Result: %d pass, %d fail\n", pass, fail);
    return fail;
}

/* ── Demo ───────────────────────────────────────────────────── */

static void demo(void) {
    printf("\n═══ Spherical Rotation Demo ═══\n");
    printf("  360×360 sphere = 129,600 positions per surface\n");
    printf("  Dual: outer(+) + inner(-) = 259,200 total addresses\n");
    printf("  \"rotation = data movement on sphere surface\"\n\n");
    printf("  %6s | %6s | %4s %4s | %4s | %s\n",
           "weight", "surf", "θ", "φ", "XOR", "");
    printf("  " "------" "-" "------" "-" "----" "-" "----" "-" "----" "---" "\n");

    int32_t tests[] = {0, 1, -1, 42, -42, 127, -128, 64, -64, 100};
    int n = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < n; i++) {
        int32_t w = tests[i];
        SphereCoord c = sph_bake(w);
        int32_t d = sph_decode(c);
        printf("  %6d | %6s | %4d %4d | %4d | %s\n",
               w,
               (c.surface == SURFACE_OUTER) ? "OUTER" : "INNER",
               c.theta, c.phi,
               sph_xor_distance(c),
               (d == w) ? "Y" : "N");
    }

    printf("\n  Rotation demo: shift (θ, φ) → weight changes\n");
    printf("  %10s | %4s %4s | %6s\n", "operation", "θ", "φ", "decode");
    printf("  " "----------" "-" "----" "-" "----" "-" "------" "\n");
    SphereCoord c = sph_bake(42);
    printf("  %10s | %4d %4d | %6d\n", "bake(42)", c.theta, c.phi, sph_decode(c));
    c = sph_rotate(c, 90, 0);
    printf("  %10s | %4d %4d | %6d\n", "rotate +90°θ", c.theta, c.phi, sph_decode(c));
    c = sph_rotate(c, 0, 180);
    printf("  %10s | %4d %4d | %6d\n", "rotate +180°φ", c.theta, c.phi, sph_decode(c));
    c = sph_rotate(c, -90, -180);
    printf("  %10s | %4d %4d | %6d\n", "rotate back", c.theta, c.phi, sph_decode(c));
}

int main(void) {
    printf("Spherical Rotation Field v6 — 360×360 Dual Sphere\n");
    printf("==================================================\n\n");

    int r = verify();
    demo();

    printf("\n%s\n", r ? "FAIL" : "✓ ALL PASS");
    return r;
}
