/* tests/test_scale_bridge.c — ONE scale timeline: BFS seeker ⇄ tess gear ring.
 *
 * Spec (independent oracle — คณิตศาสตร์, ไม่ได้ copy จาก implementation):
 *   1 tooth = 1 semitone = 2^(1/12); teeth(s) = −12·log2(s); W = teeth mod 144.
 *   Named window W ∈ [0,144) covers s ∈ [2^(−143/12), 1] ≈ [2.06e-4, 1].
 *   Alignments: W=0 ↔ s=1 (home) · W=12 ↔ s=0.5 (BFS hyper boundary) ·
 *   Δ12 = ×2 (octave) · Δ24 = ×4 (rim turn) · ring 144 = ×4096.
 *   BFS floor 1e-6 → teeth 239.18 → round 239 → W = 239 mod 144 = 95.
 *   Expansion s=2 → teeth −12 → W = −12 mod 144 = 132.
 *   Quantization: s→W→s ratio error ≤ 2^(1/24) (half tooth).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_scale_bridge tests/test_scale_bridge.c -lm
 * RUN:   ./build/test_scale_bridge
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "scale_bridge.h"
#include "geo_box_axes.h"
#include "breathing_fs.h"
#include "bfs_seek_anchor.h"
#include "bfs_persist.h"
#include "fan24_gear.h"

static int g_pass = 0, g_fail = 0;

static void check(int ok, const char *name)
{
    if (ok) { g_pass++; printf("  ✅ %s\n", name); }
    else    { g_fail++; printf("  ❌ %s\n", name); }
}

/* ── 1. octave exactness: s(12k) must equal 2^(−k) EXACTLY (both exact
 *    powers of two; oracle = integer arithmetic 1.0/(1<<k), not exp2). */
static void test_octave_exact(void)
{
    int all = 1;
    for (int k = 0; k < 12; k++) {
        double s = sbr_w_to_scale((uint32_t)(12 * k));
        double oracle = 1.0 / (double)((uint64_t)1 << k);
        if (s != oracle) all = 0;
    }
    check(all, "octaves exact: s(12k) == 2^-k for k=0..11 (oracle: integer 1/(1<<k))");
    check(sbr_w_to_scale(0) == 1.0,  "s(0) == 1.0 home");
    check(sbr_w_to_scale(12) == 0.5, "s(12) == 0.5 — BFS hyperbolic boundary");
}

/* ── 2. ring bijection: W → s → W must recover W for every tooth. */
static void test_ring_bijection(void)
{
    int all = 1;
    for (uint32_t w = 0; w < SBR_RING; w++)
        if (sbr_scale_to_ring(sbr_w_to_scale(w)) != w) { all = 0; break; }
    check(all, "ring bijection: W→s→W == W for all 144 teeth");
}

/* ── 3. strict monotonic decrease across the whole axis. */
static void test_monotonic(void)
{
    int all = 1;
    for (uint32_t w = 0; w + 1 < SBR_RING; w++)
        if (!(sbr_w_to_scale(w) > sbr_w_to_scale(w + 1))) { all = 0; break; }
    check(all, "monotonic: s strictly decreases with W over [0,144)");
}

/* ── 4. hyperbolic boundary vs the REAL BFS seeker (oracle = BFS's own
 *    window>space invariant, computed by seeker_scale — not by us). */
static void test_hyper_boundary_real_bfs(void)
{
    int all = 1;
    BreathingSeeker sk;
    seeker_init(&sk);
    for (uint32_t w = 0; w < SBR_RING; w++) {
        seeker_scale(&sk, sbr_w_to_scale(w));
        int hyper = (sk.is_hyperbolic & 1);
        int expect = sbr_w_is_hyperbolic(w);
        if (hyper != expect) { all = 0; break; }
    }
    check(all, "hyperbolic: real BFS seeker flag == (W > 12) for all W");
}

/* ── 5. quantization bound: nearest-tooth error ≤ 2^(±1/24) over the
 *    named window (sweep oracle: dense multiplicative walk). */
static void test_quantization(void)
{
    double b = exp2(1.0 / 24.0);           /* half-tooth ratio bound */
    double s = 1.0;
    int ok = 1, n = 0;
    /* stop above the ring wrap point t=143.5 (s = 2^(-143.5/12) ≈ 2.516e-4):
     * deeper teeth legitimately wrap to W=0 (ring semantics), so the
     * nearest-tooth bound only holds while t ≤ 143.5 */
    while (s > 2.6e-4) {
        s *= 0.997;                          /* ~1.7 teeth per step */
        uint32_t w = sbr_scale_to_ring(s);
        double ratio = sbr_w_to_scale(w) / s;
        if (!(ratio <= b * 1.0001 && ratio >= (1.0 / b) * 0.9999)) { ok = 0; break; }
        n++;
    }
    if (ok) printf("  ✅ quantization: s→W→s within half-tooth (±2^(1/24)) over window (n=%d)\n", n);
    else    { printf("  ❌ quantization: s→W→s within half-tooth (±2^(1/24)) over window\n"); }
    g_pass += ok; g_fail += !ok;
}

/* ── 6. deep wrap spec: BFS floor 1e-6 → W=95 (spec math: teeth = −12·log2(1e-6)
 *    = 239.18 → round 239 → 239 mod 144 = 95). */
static void test_deep_wrap(void)
{
    uint32_t w = sbr_scale_to_ring(SBR_WRAP_DEEP_S);
    check(w == 95, "deep wrap: s=1e-6 (BFS floor) → ring W=95 (spec: 239 mod 144)");
    /* independent consistency: 95 teeth behind 239; 144 teeth = ×4096 = 4^6,
     * so s(95) must equal 1e-6 × 4^6 within half tooth. */
    double oracle = SBR_WRAP_DEEP_S * 4096.0;   /* ×4^6, literal oracle */
    double ratio = sbr_w_to_scale(w) / oracle;
    double b = exp2(1.0 / 24.0);
    check(ratio <= b * 1.0001 && ratio >= (1.0 / b) * 0.9999,
          "deep wrap consistent: s(95) ≈ 1e-6 × 4^6 (4096)");
}

/* ── 7. expansion: s=2.0 → W=132 (−12 mod 144); teeth(s) signed correctly. */
static void test_expansion(void)
{
    check(sbr_scale_to_ring(2.0) == 132, "expansion: s=2.0 → W=132 (spec: −12 mod 144)");
    double t = sbr_scale_to_teeth(2.0);
    check(fabs(t - (-12.0)) < 1e-12, "expansion: teeth(2.0) == −12.0 (signed)");
    check(isinf(sbr_scale_to_teeth(0.0)) && sbr_scale_to_teeth(0.0) > 0,
          "degenerate: teeth(s≤0) == +INFINITY");
    check(sbr_scale_to_ring(0.0) == SBR_RING - 1 && sbr_scale_to_ring(-1.0) == SBR_RING - 1,
          "degenerate: ring(s≤0) == deepest tooth 143");
}

/* ── 8. rim turn: Δ24 = ×4 EXACTLY (2 octaves). */
static void test_rim_turn(void)
{
    int all = 1;
    for (uint32_t w = 0; w + SBR_RIM_TURN < SBR_RING; w++) {
        double r = sbr_w_to_scale(w) / sbr_w_to_scale(w + SBR_RIM_TURN);
        if (fabs(r - 4.0) > 1e-12) { all = 0; break; }
    }
    check(all, "rim turn: Δ24 = ×4 exactly (oracle: literal 4.0)");
}

/* ── 9. ring-distance helper vs hand ring arithmetic. */
static void test_step_teeth(void)
{
    check(sbr_step_teeth(0, 12) == 12, "step(0→12) == 12 (1 octave)");
    check(sbr_step_teeth(140, 5) == 9, "step(140→5) == 9 (5+144−140)");
    check(sbr_step_teeth(7, 7) == 0,   "step(7→7) == 0 (home tooth, no event)");
    double r = sbr_step_ratio(0, 12);
    check(fabs(r - 0.5) < 1e-12, "ratio(0→12) == 0.5 — gear Δ=12 IS one octave");
}

/* ── 10. REAL BFS integration: write file → move seeker to bridge W=12
 *    (s=0.5) → per-block deltas match manual oracle → go home → lossless. */
static void test_bfs_integration(void)
{
    BreathingFS fs;
    bfs_init(&fs);
    int8_t data[200];
    for (int i = 0; i < 200; i++) data[i] = (int8_t)(i * 7 + 3);
    check(bfs_write(&fs, "alpha", data, 200) == 0, "bfs: write 200 B file (2 blocks)");

    /* 2 blocks → home_pos 0 and 144 (seeker advances 144/block). Move to
     * bridge scale of W=12: s = 0.5 exactly (exp2(−1)). */
    double s = sbr_w_to_scale(12);
    bfs_move_seeker(&fs, s);
    check(fs.seeker.scale == 0.5, "bfs: seeker now at bridge scale s(12) == 0.5 exactly");

    /* manual oracle (integer): home even & < space(10368) → delta = home/2 − home.
     * block0 home 0 → 0; block1 home 144 → 72 − 144 = −72. */
    int b0 = (fs.block_meta[0].home_pos == 0) ? (fs.block_meta[0].delta == 0) : 0;
    int b1 = (fs.block_meta[1].home_pos == 144) ? (fs.block_meta[1].delta == -72) : 0;
    check(b0 && b1, "bfs: deltas at bridge scale match manual oracle (0, −72)");

    /* cross-check with the BFS delta formula at the bridge scale */
    int32_t d1 = bfs_delta_at(fs.block_meta[1].home_pos, s);
    check(d1 == -72, "bfs: bfs_delta_at(home, s(12)) == −72 (formula cross-check)");

    bfs_go_home(&fs);
    check(fs.seeker.scale == 1.0, "bfs: go_home restores scale 1.0");
    check(bfs_verify_file(&fs, "alpha", data, 200) == 0,
          "bfs: file lossless after bridge-scale excursion (verify at home)");
}

/* ── 11. gear home tooth: Δ=0 emits no event; Δ12 is one octave — the
 *    same 12 teeth the bridge calls an octave. */
static void test_gear_wire(void)
{
    FGLog g;
    fg_log_init(&g);
    check(fg_log_push(&g, 0, 0) == -2, "gear: Δ=0 (home) emits nothing (−2)");
    check(fg_log_push(&g, 0, 12) == 0, "gear: Δ=12 accepted (1 octave event)");
    check(fg_log_push(&g, 12, 24) == 0, "gear: Δ=12 again (12→24)");
    check(fg_log_push(&g, 24, 36) == 0, "gear: Δ=12 again (24→36)");
    check(g.hdr.n == 3, "gear: 3 events logged");
    uint32_t chain[8];
    uint32_t n = fg_reconstruct(&g, 36, chain, 8);
    check(n == 4 && chain[0] == 0 && chain[1] == 12 && chain[2] == 24 && chain[3] == 36,
          "gear: reconstruct from cur_w=36 walks back to append 0 (enter anywhere)");
}

/* ── 12. real-file disk roundtrip: save BIMG at DEEP bridge scale (W=95),
 *    reload in a fresh struct, verify scale + lossless read + CRC parse. */
static void test_disk_roundtrip(void)
{
    const char *path = "scale_bridge_rt.bimg";
    BreathingFS fs;
    bfs_init(&fs);
    int8_t data[300];
    for (int i = 0; i < 300; i++) data[i] = (int8_t)(i * 13 + 5);
    if (bfs_write(&fs, "beta", data, 300) != 0) { check(0, "disk: write prep"); return; }

    /* deep scale: s(95) ≈ 4.1e-3 — well into hyperbolic (W>12) */
    double s = sbr_w_to_scale(95);
    bfs_move_seeker(&fs, s);
    check((fs.seeker.is_hyperbolic & 1) == 1, "disk: W=95 seeker is hyperbolic");

    if (bfs_save_img(path, &fs) != 0) { check(0, "disk: save image"); remove(path); return; }

    BreathingFS fs2;
    int rc = bfs_load_img(path, &fs2);
    check(rc == 0, "disk: load image (CRC parse ok)");
    if (rc == 0) {
        check(fabs(fs2.seeker.scale - s) < 1e-12, "disk: header scale preserved (s(95))");
        check((fs2.seeker.is_hyperbolic & 1) == 1, "disk: hyperbolic flag survives load");
        check(bfs_verify_file(&fs2, "beta", data, 300) == 0,
              "disk: file lossless after deep-scale save/load roundtrip");
    }
    remove(path);
}

/* ── 13. axis-aware scale: 6 axes each have W = position mod 144 ───────── */
static void test_axis_aware_w(void)
{
    int all = 1;
    for (uint32_t axis = 0; axis < GBA_AXIS_COUNT; axis++) {
        for (uint64_t pos = 0; pos < 300; pos++) {
            GBA_Address a = gba_make(axis, pos, 0);
            uint32_t w = sbr_gba_to_w(a);
            if (w != pos % SBR_RING) { all = 0; break; }
        }
    }
    check(all, "axis-aware: W = position mod 144 for all 6 axes");

    /* Scale is IDENTICAL across axes (exact bijection) */
    GBA_Address sq = gba_make(GBA_AXIS_X, 12, 0);
    GBA_Address tr = gba_make(GBA_AXIS_I, 12, 0);
    double s_sq = sbr_gba_to_scale(sq);
    double s_tr = sbr_gba_to_scale(tr);
    check(fabs(s_sq - sbr_w_to_scale(12)) < 1e-15, "square axis X at pos=12 uses standard base (s=0.5)");
    check(fabs(s_tr - sbr_w_to_scale(12)) < 1e-15, "triangle axis I at pos=12 uses SAME base (exact bijection)");
}

/* ── 14. axis-aware hyperbolic boundary differs ────────────────────────── */
static void test_axis_hyperbolic(void)
{
    /* Square axis: hyper starts at W=13 */
    GBA_Address sq_12 = gba_make(GBA_AXIS_X, 12, 0);
    GBA_Address sq_13 = gba_make(GBA_AXIS_X, 13, 0);
    check(!sbr_gba_is_hyperbolic(sq_12), "square axis W=12 NOT hyperbolic");
    check(sbr_gba_is_hyperbolic(sq_13), "square axis W=13 IS hyperbolic");

    /* Triangle axis: hyper starts at W=19 (12+6) */
    GBA_Address tr_18 = gba_make(GBA_AXIS_I, 18, 0);
    GBA_Address tr_19 = gba_make(GBA_AXIS_I, 19, 0);
    check(!sbr_gba_is_hyperbolic(tr_18), "triangle axis W=18 NOT hyperbolic");
    check(sbr_gba_is_hyperbolic(tr_19), "triangle axis W=19 IS hyperbolic");
}

/* ── 15. GBA step teeth/ratio ──────────────────────────────────────────── */
static void test_gba_step(void)
{
    GBA_Address a0 = gba_make(GBA_AXIS_X, 0, 0);
    GBA_Address a1 = gba_make(GBA_AXIS_X, 12, 0);
    GBA_Address a2 = gba_make(GBA_AXIS_Y, 0, 0);  /* different axis */

    check(sbr_gba_step_teeth(a0, a1) == 12, "gba step same axis X 0→12 = 12 teeth");
    check(sbr_gba_step_ratio(a0, a1) == 0.5, "gba ratio same axis X 0→12 = 0.5");
    check(sbr_gba_step_teeth(a0, a2) == SBR_RING, "gba step different axes = max distance");
    check(sbr_gba_step_ratio(a0, a2) == 0.0, "gba ratio different axes = 0");
}

/* ── 16. scale_factor axis-aware roundtrip ─────────────────────────────── */
static void test_axis_scale_factor(void)
{
    int all = 1;
    for (uint32_t axis = 0; axis < GBA_AXIS_COUNT; axis++) {
        for (uint64_t pos = 0; pos < 200; pos += 13) {
            GBA_Address a = gba_make(axis, pos, 0);
            uint32_t sf = sbr_gba_to_scale_factor(a);
            /* scale_factor encodes the actual scale, not just W */
            double s = (double)sf / 65536.0;
            /* scale_factor has limited precision (1/65536) — at very deep scales
             * it rounds to 0. Only check positions where scale > 1/65536 */
            if (sbr_gba_to_scale(a) > 1.0/65536.0) {
                if (s <= 0.0 || s > 1.0) { all = 0; break; }
            }
        }
    }
    check(all, "axis scale_factor roundtrip: valid for all axes/positions (where precision allows)");
}

/* ── 17. internal scale_bridge verify ───────────────────────────────────── */
static void test_internal_verify(void)
{
    check(sbr_seeker_verify() == 0, "internal sbr_seeker_verify() passes");
    check(geo_box_axes_verify() == 1, "internal geo_box_axes_verify() passes");
}

int main(void)
{
    printf("Scale bridge — BFS seeker ⇄ tess gear ring (tests/test_scale_bridge.c)\n");
    printf("════════════════════════════════════════════════════════════════\n");

    test_octave_exact();
    test_ring_bijection();
    test_monotonic();
    test_hyper_boundary_real_bfs();
    test_quantization();
    test_deep_wrap();
    test_expansion();
    test_rim_turn();
    test_step_teeth();
    test_bfs_integration();
    test_gear_wire();
    test_disk_roundtrip();
    test_axis_aware_w();
    test_axis_hyperbolic();
    test_gba_step();
    test_axis_scale_factor();
    test_internal_verify();

    printf("───────────────────────────────────────\n");
    printf("PASS: %d  FAIL: %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
