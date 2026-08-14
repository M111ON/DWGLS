/*
 * beam_dual.c — Dual Icosahedron Field (v5)
 * ═══════════════════════════════════════════════════════════════════
 *
 * outer (+) / inner (-) / star 0 reference
 * weight = XOR(coord, star_0) + implicit sign from surface
 * Bijection via precomputed XOR map (2304 > 256 → unique per value)
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define DF_FACES      12u
#define DF_SLOTS      12u
#define DF_ICO_IDX    16u
#define DF_GRID       (DF_FACES * DF_SLOTS * DF_ICO_IDX)

typedef enum { SURFACE_OUTER = 0, SURFACE_INNER = 1 } Surface;

typedef struct {
    Surface surface;
    uint8_t face;
    uint8_t slot;
    uint8_t ixo;
} DualCoord;

/* ── XOR distance from star 0 reference ───────────────────────
 * "frame แรกที่ทุกอย่าง 0, XOR /diff frame สุดท้าย = ระยะ"
 * star 0: (OUTER, 0, 0, 0) → XOR = 0 */
static inline uint8_t xor_distance(DualCoord c) {
    uint16_t mix = (uint16_t)(c.face << 0)
                 ^ (uint16_t)(c.slot << 4)
                 ^ (uint16_t)(c.ixo << 8);
    mix = mix ^ (mix >> 3) ^ (mix >> 6);
    return (uint8_t)(mix & 0xFF);
}

/* ── Precomputed bijection maps ─────────────────────────────── */

static DualCoord xmap_outer[256];  /* distance → coord on outer surface */
static DualCoord xmap_inner[256];  /* distance → coord on inner surface */
static int xmap_ready = 0;

static void init_xor_maps(void) {
    if (xmap_ready) return;

    uint8_t used_outer[DF_GRID] = {0};
    uint8_t used_inner[DF_GRID] = {0};

    /* Distance 0 = star 0 reference on outer surface */
    xmap_outer[0] = (DualCoord){SURFACE_OUTER, 0, 0, 0};
    used_outer[0] = 1;
    xmap_inner[0] = (DualCoord){SURFACE_INNER, 0, 0, 0};
    used_inner[0] = 1;

    /* For each distance 1..255, find first unclaimed coordinate */
    for (int side = 0; side < 2; side++) {
        DualCoord *map = (side == 0) ? xmap_outer : xmap_inner;
        uint8_t *used = (side == 0) ? used_outer : used_inner;
        Surface s = (side == 0) ? SURFACE_OUTER : SURFACE_INNER;

        for (int d = 1; d < 256; d++) {
            int found = 0;
            for (uint32_t idx = 0; idx < DF_GRID && !found; idx++) {
                if (used[idx]) continue;
                DualCoord c;
                c.surface = s;
                c.face = (uint8_t)(idx / (DF_SLOTS * DF_ICO_IDX));
                c.slot = (uint8_t)((idx / DF_ICO_IDX) % DF_SLOTS);
                c.ixo  = (uint8_t)(idx % DF_ICO_IDX);
                if (xor_distance(c) == (uint8_t)d) {
                    map[d] = c;
                    used[idx] = 1;
                    found = 1;
                }
            }
            if (!found) {
                /* Fallback: allow collision (shouldn't happen with 2304 slots) */
                for (uint32_t idx = 0; idx < DF_GRID; idx++) {
                    DualCoord c;
                    c.surface = s;
                    c.face = (uint8_t)(idx / (DF_SLOTS * DF_ICO_IDX));
                    c.slot = (uint8_t)((idx / DF_ICO_IDX) % DF_SLOTS);
                    c.ixo  = (uint8_t)(idx % DF_ICO_IDX);
                    if (xor_distance(c) == (uint8_t)d) {
                        map[d] = c;
                        break;
                    }
                }
            }
        }
    }
    xmap_ready = 1;
}

/* ── BAKE: weight → DualCoord ───────────────────────────────── */

static inline DualCoord dual_bake(int32_t weight) {
    if (!xmap_ready) init_xor_maps();

    if (weight == 0) return xmap_outer[0];  /* star 0 */

    if (weight > 0) {
        return xmap_outer[(uint8_t)(weight & 0xFF)];   /* 1..127 */
    } else {
        return xmap_inner[(uint8_t)((-weight) & 0xFF)]; /* 1..128 */
    }
}

/* ── DECODE: DualCoord → weight ───────────────────────────────
 * "face ด้านไหน active ก็บอก sign — inner=(-), outer=(+)" */

static inline int32_t dual_decode(DualCoord c) {
    uint8_t dist = xor_distance(c);
    if (c.surface == SURFACE_OUTER) return (int32_t)dist;     /* 0..255 → treat as 0..127 */
    else return -(int32_t)dist;                                 /* -256..-1 → clamp to -128..-1 */
}

/* ── Verify ─────────────────────────────────────────────────── */

static int dual_verify(void) {
    int pass = 0, fail = 0;
#define T(expr, msg) do { \
    if (expr) { pass++; printf("  PASS  %s\n", msg); } \
    else { fail++; printf("  FAIL  %s (line %d)\n", msg, __LINE__); } \
} while(0)

    printf("=== Verify v5 ===\n");
    init_xor_maps();

    /* [T1] Full Q8 lossless roundtrip */
    {
        int ok = 1;
        for (int32_t w = -128; w <= 127; w++) {
            DualCoord c = dual_bake(w);
            int32_t r = dual_decode(c);
            if (r != w) { ok = 0; printf("  BREAK: w=%d r=%d\n", w, r); break; }
        }
        T(ok, "full Q8 lossless roundtrip: bake→decode = weight");
    }

    /* [T2] Deterministic */
    {
        DualCoord c1 = dual_bake(42);
        DualCoord c2 = dual_bake(42);
        T(c1.surface == c2.surface && c1.face == c2.face &&
          c1.slot == c2.slot && c1.ixo == c2.ixo,
          "deterministic");
    }

    /* [T3] Zero collisions */
    {
        uint8_t seen[2 * DF_GRID] = {0};
        int collisions = 0;
        for (int32_t w = -128; w <= 127; w++) {
            DualCoord c = dual_bake(w);
            uint32_t idx = (uint32_t)(c.surface == SURFACE_INNER ? DF_GRID : 0)
                         + (uint32_t)c.face * (DF_SLOTS * DF_ICO_IDX)
                         + (uint32_t)c.slot * DF_ICO_IDX
                         + (uint32_t)c.ixo;
            if (seen[idx]) collisions++;
            seen[idx] = 1;
        }
        T(collisions == 0, "zero collisions: 256 unique coords");
    }

    /* [T4] Sign from surface */
    {
        T(dual_bake(50).surface == SURFACE_OUTER, "positive → outer");
        T(dual_bake(-50).surface == SURFACE_INNER, "negative → inner");
    }

    /* [T5] Star 0 */
    {
        DualCoord s0 = dual_bake(0);
        T(s0.surface == SURFACE_OUTER && s0.face == 0 &&
          s0.slot == 0 && s0.ixo == 0,
          "zero → star 0 {outer, 0, 0, 0}");
    }

    /* [T6] decode(outer dist=128) doesn't collide with decode(inner dist=128) */
    {
        /* outer max = 127, inner max = 128 */
        DualCoord co = dual_bake(127);
        DualCoord ci = dual_bake(-128);
        T(co.surface == SURFACE_OUTER, "127 on outer");
        T(ci.surface == SURFACE_INNER, "-128 on inner");
        /* They must be different coords (different surfaces are allowed same bits) */
        int same_pos = (co.face == ci.face) && (co.slot == ci.slot) && (co.ixo == ci.ixo);
        T(!same_pos || co.surface != ci.surface, "127 and -128 on diff surfaces");
    }

    printf("  Result: %d pass, %d fail\n", pass, fail);
    return fail;
}

/* ── Demo ───────────────────────────────────────────────────── */

static void demo(void) {
    printf("\n═══ Dual Icosahedron Demo v5 ═══\n");
    printf("  outer (+) = oil floats | inner (-) = sinks\n");
    printf("  star 0    = ref frame {outer, 0, 0, 0}\n");
    printf("  weight    = XOR(coord, star_0) + implicit sign from surface\n\n");
    printf("  %6s | %6s | %4s %4s %4s | %4s |\n",
           "weight", "surf", "face", "slot", "ixo", "XOR");
    printf("  " "------" "-" "------" "-" "----" "-" "----" "-" "----" "-" "----" "\n");

    int32_t tests[] = {0, 1, -1, 42, -42, 127, -128, 64, -64, 100};
    int n = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < n; i++) {
        int32_t w = tests[i];
        DualCoord c = dual_bake(w);
        int32_t d = dual_decode(c);
        uint8_t dist = xor_distance(c);
        printf("  %6d | %6s | %4d %4d %4d | %4d | %s\n",
               w,
               (c.surface == SURFACE_OUTER) ? "OUTER" : "INNER",
               c.face, c.slot, c.ixo, dist,
               (d == w) ? "Y" : "N");
    }
}

int main(void) {
    printf("Dual Icosahedron v5 — Pure XOR Bijection\n");
    printf("=========================================\n\n");

    int r = dual_verify();
    demo();

    printf("\n%s\n", r ? "FAIL" : "✓ ALL PASS");
    return r;
}
