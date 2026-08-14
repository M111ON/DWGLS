/*
 * beam_square.c — Dual Square 360×360 (v7)
 * ═══════════════════════════════════════════════════════════════════
 *
 * "เอา 360 มาลากเป็นเส้นตรง angular เป็น XY L ลูกนึง
 *   แล้วก็ YX L กลับด้านอีกลูกนึง — ได้ square 360×360"
 *
 *   XY square (outer+) : X=θ, Y=φ
 *   YX square (inner-) : X=φ, Y=θ  (transposed mirror)
 *
 *   Each square = 360×360 = 129,600 positions
 *   Dual total   = 259,200 positions
 *
 *   Weight = XOR(X, Y)  (XOR is commutative → same formula both squares)
 *   Sign   = which square (XY=outer=+, YX=inner=-)
 *   Star 0 = {XY, 0, 0}
 *
 * Flat geometry, no spherical wrapping, no center distance.
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define SQ_RES    360u      /* 360×360 per square */
#define SQ_GRID   (SQ_RES * SQ_RES)  /* 129,600 */

/* Dual square layers */
typedef enum { SQUARE_XY = 0, SQUARE_YX = 1 } SquareLayer;

/* Coordinate on one square */
typedef struct {
    SquareLayer layer;   /* XY=outer(+), YX=inner(-) */
    uint16_t x;          /* 0..359  [θ for XY, φ for YX] */
    uint16_t y;          /* 0..359  [φ for XY, θ for YX] */
} SquareCoord;

/* Star 0 reference */
#define SQ_STAR_0 ((SquareCoord){SQUARE_XY, 0, 0})

/* ── Index within square ────────────────────────────────────── */
static inline uint32_t sq_idx(SquareCoord c) {
    return (uint32_t)c.x + (uint32_t)c.y * SQ_RES;
}

static inline SquareCoord idx_to_sq(uint32_t idx, SquareLayer lay) {
    SquareCoord c;
    c.layer = lay;
    c.x = (uint16_t)(idx % SQ_RES);
    c.y = (uint16_t)(idx / SQ_RES);
    return c;
}

/* ── XOR distance on square ───────────────────────────────────
 *
 * "XOR ของพิกัดบน square = ระยะ = weight magnitude"
 * Flat geometry: 9-bit x ^ 9-bit y → mix down to 8-bit.
 */
static inline uint8_t sq_xor_distance(SquareCoord c) {
    uint16_t mix = (uint16_t)(c.x ^ c.y);
    /* Extend 9-bit XOR → full 8-bit range */
    uint16_t m2 = mix ^ (mix >> 3) ^ (mix >> 5) ^ (mix >> 7);
    return (uint8_t)(m2 & 0xFF);
}

/* ── Bijection maps ─────────────────────────────────────────── */
static SquareCoord xmap_xy[256];   /* XY square: distance → coord */
static SquareCoord xmap_yx[256];   /* YX square: distance → coord */
static int xmap_ready = 0;

static void init(void) {
    if (xmap_ready) return;
    uint8_t used_xy[SQ_GRID] = {0};
    uint8_t used_yx[SQ_GRID] = {0};

    xmap_xy[0] = SQ_STAR_0;  used_xy[0] = 1;
    xmap_yx[0] = (SquareCoord){SQUARE_YX, 0, 0};  used_yx[0] = 1;

    for (int side = 0; side < 2; side++) {
        SquareCoord *map = (side == 0) ? xmap_xy : xmap_yx;
        uint8_t *used = (side == 0) ? used_xy : used_yx;
        SquareLayer lay = (side == 0) ? SQUARE_XY : SQUARE_YX;

        for (int d = 1; d < 256; d++) {
            for (uint32_t idx = 0; idx < SQ_GRID; idx++) {
                if (used[idx]) continue;
                SquareCoord c = idx_to_sq(idx, lay);
                if (sq_xor_distance(c) == (uint8_t)d) {
                    map[d] = c;
                    used[idx] = 1;
                    break;
                }
            }
        }
    }
    xmap_ready = 1;
}

/* ── BAKE: weight → SquareCoord ─────────────────────────────── */
static inline SquareCoord sq_bake(int32_t weight) {
    if (!xmap_ready) init();
    if (weight == 0) return SQ_STAR_0;
    if (weight > 0)  return xmap_xy[(uint8_t)(weight & 0xFF)];
    else             return xmap_yx[(uint8_t)((-weight) & 0xFF)];
}

/* ── DECODE: SquareCoord → weight ───────────────────────────── */
static inline int32_t sq_decode(SquareCoord c) {
    uint8_t dist = sq_xor_distance(c);
    if (c.layer == SQUARE_XY) return (int32_t)dist;
    else                      return -(int32_t)dist;
}

/* ── Shift (translation) on square — "grid redirect" ──────────
 * Shift coordinate by (dx, dy) → moves to different address.
 * This is the L-Block redirect: shift geometry → redirect to grid. */
static inline SquareCoord sq_shift(SquareCoord c, int16_t dx, int16_t dy) {
    c.x = (uint16_t)((c.x + dx + SQ_RES) % SQ_RES);
    c.y = (uint16_t)((c.y + dy + SQ_RES) % SQ_RES);
    return c;
}

/* ── Transpose XY ↔ YX (switch layers) ────────────────────────
 * XY(x,y) → YX(y,x) — mirror across diagonal.
 * This is the dual operation: switch sign by transposing. */
static inline SquareCoord sq_transpose(SquareCoord c) {
    uint16_t tx = c.y;  /* swap x ↔ y */
    uint16_t ty = c.x;
    c.layer = (c.layer == SQUARE_XY) ? SQUARE_YX : SQUARE_XY;
    c.x = tx;
    c.y = ty;
    return c;
}

/* ── Verify ─────────────────────────────────────────────────── */

static int verify(void) {
    int pass = 0, fail = 0;
#define T(expr, msg) do { \
    if (expr) { pass++; printf("  PASS  %s\n", msg); } \
    else { fail++; printf("  FAIL  %s (line %d)\n", msg, __LINE__); } \
} while(0)

    printf("=== Dual Square Verify ===\n");
    init();

    /* T1: Full Q8 lossless roundtrip */
    {
        int ok = 1;
        for (int32_t w = -128; w <= 127; w++) {
            SquareCoord c = sq_bake(w);
            if (sq_decode(c) != w) { ok = 0; break; }
        }
        T(ok, "full Q8 lossless roundtrip");
    }

    /* T2: Deterministic */
    T(sq_bake(42).x == sq_bake(42).x, "deterministic");

    /* T3: Zero collisions across both squares */
    {
        uint8_t seen[SQ_GRID * 2] = {0};
        int collisions = 0;
        for (int32_t w = -128; w <= 127; w++) {
            SquareCoord c = sq_bake(w);
            uint32_t idx = sq_idx(c) + (c.layer == SQUARE_YX ? SQ_GRID : 0);
            if (seen[idx]) collisions++;
            seen[idx] = 1;
        }
        T(collisions == 0, "zero collisions across 259,200");
    }

    /* T4: Layer = sign */
    T(sq_bake(50).layer == SQUARE_XY,  "positive → XY (outer)");
    T(sq_bake(-50).layer == SQUARE_YX, "negative → YX (inner)");

    /* T5: Star 0 */
    {
        SquareCoord s0 = sq_bake(0);
        T(s0.layer == SQUARE_XY && s0.x == 0 && s0.y == 0, "zero → star 0");
    }

    /* T6: Transpose flips sign (XY ↔ YX) */
    {
        SquareCoord cp = sq_bake(42);        /* XY layer */
        SquareCoord cn = sq_transpose(cp);    /* → YX layer */
        T(cn.layer == SQUARE_YX, "transpose XY → YX");
        T(sq_decode(cn) == -sq_decode(cp), "transpose flips sign");
    }

    /* T7: Double transpose = identity */
    {
        SquareCoord c = sq_bake(42);
        SquareCoord d = sq_transpose(sq_transpose(c));
        T(d.layer == c.layer && d.x == c.x && d.y == c.y,
          "double transpose = identity");
    }

    /* T8: Shift then un-shift */
    {
        SquareCoord c = sq_bake(100);
        SquareCoord s = sq_shift(c, 50, -30);
        SquareCoord b = sq_shift(s, -50, 30);
        T(b.x == c.x && b.y == c.y, "shift + unshift restores coord");
    }

    /* T9: 360×360 = 129,600 */
    T(SQ_GRID == 129600, "square = 360×360 = 129,600");
    T(sizeof(SquareCoord) <= 8, "SquareCoord compact");

    printf("  Result: %d pass, %d fail\n", pass, fail);
    return fail;
}

/* ── Demo ───────────────────────────────────────────────────── */

static void demo(void) {
    printf("\n═══ Dual Square Demo ═══\n");
    printf("  XY square (outer +) :  X=θ, Y=φ   360×360\n");
    printf("  YX square (inner -) :  X=φ, Y=θ   (transposed)\n");
    printf("  \"flat geometry, no spherical wrapping\"\n\n");

    printf("  %6s | %6s | %4s %4s | %4s | %s\n",
           "weight", "square", "X", "Y", "XOR", "");
    printf("  " "------" "-" "------" "-" "----" "-" "----" "-" "----" "---" "\n");

    int32_t tests[] = {0, 1, -1, 42, -42, 127, -128, 64, -64, 100};
    int n = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < n; i++) {
        int32_t w = tests[i];
        SquareCoord c = sq_bake(w);
        printf("  %6d | %6s | %4d %4d | %4d | %s\n",
               w,
               (c.layer == SQUARE_XY) ? "XY" : "YX",
               c.x, c.y,
               sq_xor_distance(c),
               (sq_decode(c) == w) ? "Y" : "N");
    }

    printf("\n  Operations:\n");
    SquareCoord c = sq_bake(42);
    printf("    bake(42)      = XY(%d,%d) → %d\n", c.x, c.y, sq_decode(c));
    SquareCoord c2 = sq_shift(c, 90, 0);
    printf("    shift(90,0)   = XY(%d,%d) → %d\n", c2.x, c2.y, sq_decode(c2));
    SquareCoord c3 = sq_transpose(c);
    printf("    transpose     = YX(%d,%d) → %d\n", c3.x, c3.y, sq_decode(c3));
}

int main(void) {
    printf("Dual Square 360×360 — Geometric Field v7\n");
    printf("=========================================\n\n");

    init();
    int r = verify();
    demo();

    printf("\n%s\n", r ? "FAIL" : "✓ ALL PASS — Dual square works.");
    return r;
}
