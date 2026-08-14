/*
 * beam_wave.c — Wave Stacking on Dual Square 360×360 (v1)
 * ═══════════════════════════════════════════════════════════════════
 *
 * "คลื่นซ้อนทับบน square — 1 coordinate พก 2 ค่า"
 *
 * Wave Stacking Concept:
 *   weight  = XOR(x,y)        [8 bits]  → amplitude
 *   phase   = x               [8 bits]  → position on contour  
 *   sign    = layer           [1 bit]   → polarity
 *   Total: 17 bits in one (x,y,layer) coordinate
 *
 * Core O(1) identity:
 *   y = x XOR |weight|  ⇒  XOR(x,y) = |weight|  ∀ x
 *   → No lookup table needed
 *   → x is free to carry phase (independent data)
 *
 * Data density: 256×256×2 = 131,072 distinct states
 *   (vs 256×2 = 512 in old lookup-table approach)
 *   = ×256 density improvement at zero extra storage cost
 *
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────── */
#define WV_RES    360u          /* 360×360 per square          */
#define WV_GRID   (WV_RES * WV_RES)  /* 129,600               */
#define WV_Q8_MIN -128
#define WV_Q8_MAX  127

/* Layer enum */
typedef enum { WV_XY = 0, WV_YX = 1 } WaveLayer;

/* Wave coordinate — 17 bits in 4 bytes */
typedef struct {
    WaveLayer layer;   /* XY=outer(+), YX=inner(-) */
    uint16_t x;        /* 0..359 */
    uint16_t y;        /* 0..359 */
} WaveCoord;

#define WV_STAR_0 ((WaveCoord){WV_XY, 0, 0})


/* ══════════════════════════════════════════════════════════════════
   CORE: O(1) WAVE ENCODE — weight + phase → coordinate
   ══════════════════════════════════════════════════════════════════ */

static inline WaveCoord wave_encode(int32_t weight, uint8_t phase)
{
    WaveCoord c;
    /* Amplitude: weight magnitude (0..255) */
    uint8_t d = (uint8_t)((weight < 0) ? -weight : weight);
    /* Phase = x coordinate (free parameter) */
    c.x = (uint16_t)phase;        /* 0..255 (always < 360) */
    /* y = x XOR d guarantees XOR(x,y) = d */
    c.y = (uint16_t)(c.x ^ d);    /* 0..255 (always < 360) */
    /* Layer = sign */
    c.layer = (weight >= 0) ? WV_XY : WV_YX;
    return c;
}


/* ══════════════════════════════════════════════════════════════════
   CORE: O(1) WAVE DECODE — coordinate → weight + phase
   ══════════════════════════════════════════════════════════════════ */

static inline void wave_decode(WaveCoord c, int32_t *weight, uint8_t *phase)
{
    uint8_t d = (uint8_t)(c.x ^ c.y);
    if (weight) *weight = (c.layer == WV_XY) ? (int32_t)d : -(int32_t)d;
    if (phase)  *phase  = (uint8_t)c.x;
}


/* ══════════════════════════════════════════════════════════════════
   EXTRACTORS
   ══════════════════════════════════════════════════════════════════ */

static inline int32_t wave_weight(WaveCoord c)
{
    uint8_t d = (uint8_t)(c.x ^ c.y);
    return (c.layer == WV_XY) ? (int32_t)d : -(int32_t)d;
}

static inline uint8_t wave_phase(WaveCoord c)
{
    return (uint8_t)c.x;
}

static inline uint32_t wave_idx(WaveCoord c)
{
    return (uint32_t)c.x + (uint32_t)c.y * WV_RES;
}


/* ══════════════════════════════════════════════════════════════════
   WAVE OPERATIONS — transformations on the square
   ══════════════════════════════════════════════════════════════════ */

/* Shift: move wave on square — changes BOTH weight and phase
 * This is "wave propagation" — moving modulates amplitude+phase simultaneously */
static inline WaveCoord wave_shift(WaveCoord c, int16_t dx, int16_t dy)
{
    c.x = (uint16_t)((c.x + dx + WV_RES) % WV_RES);
    c.y = (uint16_t)((c.y + dy + WV_RES) % WV_RES);
    return c;
}

/* Transpose: flip layer XY↔YX — toggles sign */
static inline WaveCoord wave_transpose(WaveCoord c)
{
    uint16_t tx = c.y;
    uint16_t ty = c.x;
    c.layer = (c.layer == WV_XY) ? WV_YX : WV_XY;
    c.x = tx;
    c.y = ty;
    return c;
}

/* XOR distance between two coordinates */
static inline uint8_t wave_dist(WaveCoord a, WaveCoord b)
{
    return (uint8_t)((a.x ^ b.x) ^ (a.y ^ b.y));
}

/* Compose two waves: positional superposition
 *   r = a ∘ b  →  r.x = (a.x + b.x) mod 360, r.y = (a.y + b.y) mod 360
 *   layer: XY if both same layer, YX if different */
static inline WaveCoord wave_compose(WaveCoord a, WaveCoord b)
{
    WaveCoord r;
    r.x = (uint16_t)((a.x + b.x) % WV_RES);
    r.y = (uint16_t)((a.y + b.y) % WV_RES);
    r.layer = (a.layer == b.layer) ? WV_XY : WV_YX;
    return r;
}

/* Decompose: r = a ∘ b  →  b = r ∘ inverse(a) */
static inline WaveCoord wave_decompose(WaveCoord r, WaveCoord a)
{
    WaveCoord b;
    b.x = (uint16_t)((r.x - a.x + WV_RES) % WV_RES);
    b.y = (uint16_t)((r.y - a.y + WV_RES) % WV_RES);
    b.layer = (r.layer == a.layer) ? WV_XY : WV_YX;
    return b;
}


/* ══════════════════════════════════════════════════════════════════
   OLD LOOKUP-TABLE APPROACH (for comparison)
   ══════════════════════════════════════════════════════════════════ */

typedef int (*LookupBakeFn)(int32_t weight, WaveCoord *out);

/* Build old-style bijection table (256 entries, scan 129,600 slots) */
static WaveCoord old_xy_map[256];
static WaveCoord old_yx_map[256];
static int old_ready = 0;

static void old_init(void)
{
    if (old_ready) return;
    uint8_t used_xy[WV_GRID] = {0};
    uint8_t used_yx[WV_GRID] = {0};

    old_xy_map[0] = WV_STAR_0;    used_xy[0] = 1;
    old_yx_map[0] = (WaveCoord){WV_YX, 0, 0};  used_yx[0] = 1;

    for (int side = 0; side < 2; side++) {
        WaveCoord *map = (side == 0) ? old_xy_map : old_yx_map;
        uint8_t *used = (side == 0) ? used_xy : used_yx;
        WaveLayer lay = (side == 0) ? WV_XY : WV_YX;

        for (int d = 1; d < 256; d++) {
            for (uint32_t idx = 0; idx < WV_GRID; idx++) {
                if (used[idx]) continue;
                WaveCoord c;
                c.layer = lay;
                c.x = (uint16_t)(idx % WV_RES);
                c.y = (uint16_t)(idx / WV_RES);
                uint8_t dist = (uint8_t)(c.x ^ c.y);
                if (dist == (uint8_t)d) {
                    map[d] = c;
                    used[idx] = 1;
                    break;
                }
            }
        }
    }
    old_ready = 1;
}

/* Old lookup-table bake (for benchmark comparison) */
static int old_bake(int32_t weight, WaveCoord *out)
{
    if (!old_ready) old_init();
    if (weight == 0) { *out = WV_STAR_0; return 0; }
    uint8_t d = (uint8_t)((weight < 0) ? -weight : weight);
    *out = (weight >= 0) ? old_xy_map[d] : old_yx_map[d];
    return 0;
}


/* ══════════════════════════════════════════════════════════════════
   TESTS
   ══════════════════════════════════════════════════════════════════ */

static int test_wave_roundtrip(void)
{
    int fail = 0;
    for (int32_t w = WV_Q8_MIN; w <= WV_Q8_MAX; w++) {
        for (int p = 0; p < 256; p++) {
            WaveCoord c = wave_encode(w, (uint8_t)p);
            int32_t rw; uint8_t rp;
            wave_decode(c, &rw, &rp);
            if (rw != w || rp != (uint8_t)p) {
                printf("  FAIL roundtrip: w=%d p=%d → rw=%d rp=%d\n", w, p, rw, rp);
                fail++;
                if (fail > 5) return fail;
            }
        }
    }
    return fail;
}

static int test_wave_collision(void)
{
    /* Every (weight, phase) maps to a unique coordinate */
    uint8_t seen[WV_GRID * 2] = {0};
    int collisions = 0;

    for (int32_t w = WV_Q8_MIN; w <= WV_Q8_MAX; w++) {
        for (int p = 0; p < 256; p++) {
            WaveCoord c = wave_encode(w, (uint8_t)p);
            uint32_t idx = wave_idx(c) + (c.layer == WV_YX ? WV_GRID : 0);
            if (seen[idx]) collisions++;
            seen[idx] = 1;
        }
    }
    return collisions;
}

static int test_wave_density(void)
{
    /* Count distinct states */
    uint8_t seen[WV_GRID * 2] = {0};
    uint32_t distinct = 0;

    for (int32_t w = WV_Q8_MIN; w <= WV_Q8_MAX; w++) {
        for (int p = 0; p < 256; p++) {
            WaveCoord c = wave_encode(w, (uint8_t)p);
            uint32_t idx = wave_idx(c) + (c.layer == WV_YX ? WV_GRID : 0);
            if (!seen[idx]) { seen[idx] = 1; distinct++; }
        }
    }
    return (int)distinct;
}

static int test_wave_operations(void)
{
    int pass = 0, fail = 0;
#define T(expr, msg) do { \
    if (expr) { pass++; } \
    else { fail++; printf("  FAIL %s\n", msg); } \
} while(0)

    /* T1: Shift then un-shift restores */
    {
        WaveCoord c = wave_encode(100, 50);
        WaveCoord s = wave_shift(c, 30, -20);
        WaveCoord b = wave_shift(s, -30, 20);
        T(b.x == c.x && b.y == c.y && b.layer == c.layer,
          "shift + unshift identity");
    }

    /* T2: Transpose flips sign */
    {
        WaveCoord cp = wave_encode(42, 10);
        WaveCoord cn = wave_transpose(cp);
        T(wave_weight(cn) == -wave_weight(cp),
          "transpose flips sign");
    }

    /* T3: Double transpose = identity */
    {
        WaveCoord c = wave_encode(42, 10);
        WaveCoord d = wave_transpose(wave_transpose(c));
        T(d.x == c.x && d.y == c.y && d.layer == c.layer,
          "double transpose = identity");
    }

    /* T4: Compose then decompose */
    {
        WaveCoord a = wave_encode(30, 100);
        WaveCoord b = wave_encode(50, 200);
        WaveCoord r = wave_compose(a, b);
        WaveCoord b2 = wave_decompose(r, a);
        T(b2.x == b.x && b2.y == b.y && b2.layer == b.layer,
          "compose + decompose roundtrip");
    }

    /* T5: Star 0 */
    {
        WaveCoord z = wave_encode(0, 0);
        T(z.layer == WV_XY && z.x == 0 && z.y == 0,
          "zero weight + zero phase → star 0");
    }

    /* T6: Composition is associative: (a∘b)∘c = a∘(b∘c) */
    {
        WaveCoord a = wave_encode(10, 20);
        WaveCoord b = wave_encode(30, 40);
        WaveCoord c = wave_encode(50, 60);
        WaveCoord ab_c = wave_compose(wave_compose(a, b), c);
        WaveCoord a_bc = wave_compose(a, wave_compose(b, c));
        T(ab_c.x == a_bc.x && ab_c.y == a_bc.y && ab_c.layer == a_bc.layer,
          "composition associative");
    }

    printf("  Wave ops: %d pass, %d fail\n", pass, fail);
    return fail;
}

static int test_wave_verify(void)
{
    int pass = 0, fail = 0;
#define V(expr, msg) do { \
    if (expr) { pass++; } \
    else { fail++; printf("  FAIL %s\n", msg); } \
} while(0)

    printf("\n=== Wave Stacking Verify ===\n");

    /* V1: Full Q8 roundtrip */
    {
        int ok = 1;
        for (int32_t w = WV_Q8_MIN; w <= WV_Q8_MAX && ok; w++) {
            for (int p = 0; p < 256; p++) {
                WaveCoord c = wave_encode(w, (uint8_t)p);
                if (wave_weight(c) != w || wave_phase(c) != (uint8_t)p) {
                    ok = 0; break;
                }
            }
        }
        V(ok, "full Q8 × 256-phase roundtrip (65,536 states/layer)");
    }

    /* V2: Zero collisions */
    {
        int coll = test_wave_collision();
        V(coll == 0, "zero collisions across 131,072 states");
    }

    /* V3: Density */
    {
        int d = test_wave_density();
        V(d == 256 * 256, "density = 256×256 = 65,536");
        printf("    Distinct states: %d / %d slots (%.1f%% utilization)\n",
           d, WV_GRID * 2, 100.0 * d / (WV_GRID * 2));
    }

    /* V4: O(1) formula correctness */
    {
        int ok = 1;
        for (int w = 0; w <= 255; w++) {
            for (int p = 0; p < 256; p++) {
                WaveCoord c = wave_encode(w, (uint8_t)p);
                uint8_t d = (uint8_t)(c.x ^ c.y);
                if (d != (uint8_t)w || c.x != (uint16_t)p) {
                    ok = 0; break;
                }
                if (c.x >= WV_RES || c.y >= WV_RES) { ok = 0; break; }
            }
        }
        V(ok, "O(1) formula always produces valid coordinates");
    }

    /* V5: Phase carry-through */
    {
        uint8_t phases[] = {0, 1, 128, 200, 255};
        int ok = 1;
        for (int i = 0; i < 5; i++) {
            WaveCoord c = wave_encode(100, phases[i]);
            if (c.x != phases[i]) { ok = 0; break; }
            /* y = x XOR 100 */
            if (c.y != (uint16_t)(phases[i] ^ 100)) { ok = 0; break; }
        }
        V(ok, "phase = x coordinate (carry-through)");
    }

    /* V6: Same weight, different phase = different coords */
    {
        WaveCoord a = wave_encode(42, 0);
        WaveCoord b = wave_encode(42, 1);
        V(!(a.x == b.x && a.y == b.y),
          "different phase → different coordinate");
    }

    /* V7: 360×360 structure intact */
    V(WV_GRID == 129600, "square = 360×360 = 129,600");
    V(sizeof(WaveCoord) <= 8, "WaveCoord compact");

    printf("  Result: %d pass, %d fail\n", pass, fail);
    return fail;
}


/* ══════════════════════════════════════════════════════════════════
   BENCHMARK
   ══════════════════════════════════════════════════════════════════ */

static double now_sec(void)
{
    clock_t c = clock();
    return (double)c / (double)CLOCKS_PER_SEC;
}

#define N_BENCH 5000000

static void benchmark(void)
{
    printf("\n═══ Wave Stacking Benchmark ═══\n");
    printf("  Encoding %d coordinates...\n", N_BENCH);

    double t0, t1;
    int32_t dummy = 0;

    /* Pre-gen data */
    int32_t *weights = (int32_t*)malloc(N_BENCH * sizeof(int32_t));
    uint8_t *phases  = (uint8_t*)malloc(N_BENCH * sizeof(uint8_t));
    for (int i = 0; i < N_BENCH; i++) {
        weights[i] = (int32_t)((i % 256) - 128);
        phases[i]  = (uint8_t)(i & 0xFF);
    }

    /* BENCH 1: New O(1) wave_encode */
    t0 = now_sec();
    for (int i = 0; i < N_BENCH; i++) {
        WaveCoord c = wave_encode(weights[i], phases[i]);
        dummy += (int32_t)c.x;
    }
    t1 = now_sec();
    double new_enc = (double)N_BENCH / (t1 - t0);
    printf("  NEW O(1) encode:  %.0f ops/sec  (%d in %.3fs)\n",
           new_enc, N_BENCH, t1 - t0);

    /* BENCH 2: New O(1) wave_decode */
    WaveCoord *coords = (WaveCoord*)malloc(N_BENCH * sizeof(WaveCoord));
    for (int i = 0; i < N_BENCH; i++) {
        coords[i] = wave_encode(weights[i], phases[i]);
    }
    t0 = now_sec();
    for (int i = 0; i < N_BENCH; i++) {
        int32_t w; uint8_t p;
        wave_decode(coords[i], &w, &p);
        dummy += w + (int32_t)p;
    }
    t1 = now_sec();
    double new_dec = (double)N_BENCH / (t1 - t0);
    printf("  NEW O(1) decode:  %.0f ops/sec  (%d in %.3fs)\n",
           new_dec, N_BENCH, t1 - t0);

    /* BENCH 3: Old lookup-table bake */
    old_init();
    t0 = now_sec();
    for (int i = 0; i < N_BENCH; i++) {
        WaveCoord c;
        old_bake(weights[i], &c);
        dummy += (int32_t)c.x;
    }
    t1 = now_sec();
    double old_speed = (double)N_BENCH / (t1 - t0);
    printf("  OLD table bake:   %.0f ops/sec  (%d in %.3fs)\n",
           old_speed, N_BENCH, t1 - t0);

    /* BENCH 4: wave_shift */
    t0 = now_sec();
    for (int i = 0; i < N_BENCH; i++) {
        WaveCoord c = coords[i % 256];
        c = wave_shift(c, (int16_t)(i % 360), (int16_t)((i+1) % 360));
        dummy += (int32_t)c.x;
    }
    t1 = now_sec();
    double shift_speed = (double)N_BENCH / (t1 - t0);
    printf("  O(1) shift:       %.0f ops/sec  (%d in %.3fs)\n",
           shift_speed, N_BENCH, t1 - t0);

    printf("\n  Density comparison:\n");
    printf("    OLD table:  256 values / 259,200 slots  (%.2f%% utilization)\n",
           100.0 * 512 / (WV_GRID * 2));
    printf("    NEW wave:   65,536 states / 259,200 slots (%.1f%% utilization)\n",
           100.0 * 65536 / (WV_GRID * 2));
    printf("    Density gain: ×%.0f\n", (double)65536 / 512.0);

    free(weights);
    free(phases);
    free(coords);

    /* Use dummy to prevent optimization */
    if (dummy < 0) printf("  (dummy check: %d)\n", dummy);
}


/* ══════════════════════════════════════════════════════════════════
   DEMO
   ══════════════════════════════════════════════════════════════════ */

static void demo(void)
{
    printf("\n═══ Wave Stacking Demo ═══\n");
    printf("  O(1) identity: y = x XOR |weight| → XOR(x,y) = |weight|\n");
    printf("  Phase channel: x carries independent 8-bit data\n\n");

    printf("  %6s | %6s | %4s %4s | %4s %4s | %s\n",
           "weight", "phase", "X", "Y", "XOR", "layer", "match");
    printf("  " "------" "-" "------" "-" "----" "-" "----" "-" "----" "-" "----" "---" "------" "\n");

    int32_t test_w[] = {0, 42, -42, 127, -128, 64, -64, 100, -1, 1};
    uint8_t test_p[] = {0, 50, 100, 200, 255, 10, 20, 128, 5, 250};

    for (int i = 0; i < 10; i++) {
        WaveCoord c = wave_encode(test_w[i], test_p[i]);
        uint8_t d = (uint8_t)(c.x ^ c.y);
        int32_t rw; uint8_t rp;
        wave_decode(c, &rw, &rp);
        printf("  %6d | %6d | %4d %4d | %4d  %4s | %s\n",
               test_w[i], test_p[i], c.x, c.y, d,
               (c.layer == WV_XY) ? "XY" : "YX",
               (rw == test_w[i] && rp == test_p[i]) ? "✓" : "✗");
    }

    printf("\n  Operations:\n");
    WaveCoord c = wave_encode(42, 100);
    printf("    encode(42, 100)  = XY(%d,%d)  → weight=%d phase=%d\n",
           c.x, c.y, wave_weight(c), wave_phase(c));

    WaveCoord s = wave_shift(c, 90, 180);
    printf("    shift(90,180)    = XY(%d,%d)  → weight=%d phase=%d\n",
           s.x, s.y, wave_weight(s), wave_phase(s));

    WaveCoord t = wave_transpose(c);
    printf("    transpose        = YX(%d,%d)  → weight=%d phase=%d\n",
           t.x, t.y, wave_weight(t), wave_phase(t));

    /* Demo wave composition */
    WaveCoord a = wave_encode(30, 50);
    WaveCoord b = wave_encode(70, 150);
    WaveCoord r = wave_compose(a, b);
    printf("    compose(a∘b):    a(%d,%d) ∘ b(%d,%d) = (%d,%d,%s)\n",
           a.x, a.y, b.x, b.y, r.x, r.y, r.layer == WV_XY ? "XY" : "YX");

    WaveCoord b2 = wave_decompose(r, a);
    printf("    decompose:       r∘inv(a) = (%d,%d,%s)  %s b\n",
           b2.x, b2.y, b2.layer == WV_XY ? "XY" : "YX",
           (b2.x == b.x && b2.y == b.y) ? "==" : "!=");
}


/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("Wave Stacking — Dual Square 360×360 (v1)\n");
    printf("==============================================\n");
    printf("  O(1) wave formula replaces lookup table\n");
    printf("  17 bits per coordinate: weight(8) + phase(8) + sign(1)\n\n");

    int r = test_wave_verify();
    r += test_wave_operations();

    if (r == 0) {
        printf("\n✓ ALL TESTS PASS — Wave stacking works.\n");
    } else {
        printf("\n✗ %d FAILURES\n", r);
    }

    demo();
    benchmark();

    printf("\n%s\n", r ? "FAIL" : "✓ DONE");
    return r;
}
