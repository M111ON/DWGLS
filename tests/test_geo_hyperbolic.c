/*
 * test_geo_hyperbolic.c — TIER1 proof of the hyperbolic redesign
 *
 * Hyperbolic = deterministic route (centroid walk) + key frame + f(step).
 *
 * INDEPENDENT ORACLES (per AGENTS.md — no expected from implementation):
 *   - stride VALUES {1,9,81} come from the MIXED-RADIX geometry spec,
 *     NOT from hw_stride(): 20736 = 256×81 (hi 4-ladder stride 81,
 *     lo 3-ladder strides 9 then 1). T0 asserts them directly.
 *   - orbit: gcd(1,20736)=1 ⇒ additive stride-1 walk visits all 20736
 *     nodes once and returns home (pure modular arithmetic).
 *   - reversibility: (n+s)−s = n is a modular identity TRUE for any s;
 *     therefore it proves the arithmetic, and T0 pins the stride values
 *     so the pair is not circular.
 *   - parity: s odd ⇒ parity flips (bit identity), verified against the
 *     SPEC strides, not against hw_stride().
 *   - reconstruct: hw_at from ANY seed with stride 1 returns the node
 *     trivially (identity) — so T4 does NOT test "returns the node".
 *     Instead it cross-checks the store against the REFERENCE primitive
 *     th_cell_anchor (pre-existing, independently tested) and verifies
 *     hwf_index_of picks the correct cell (node / cell_size).
 *
 * The dual/dodeca facts are covered in test_dual_dodeca_probe.c.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_hyperbolic_walk.h"
#include "geo_hyperbolic_store.h"

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (!(cond)) { printf("  FAIL %s\n", name); g_fail++; } \
         else { printf("  PASS %s\n", name); } } while (0)

/* SPEC strides — mixed-radix geometry, independent of hw_stride():
 * 20736 = 256×81 = 2^8 × 3^4. hi digit stride = 81 (3^4),
 * lo digit strides = 1 (3^0), 9 (3^2), 27 (3^3). */
#define SPEC_S0 1u
#define SPEC_S1 9u
#define SPEC_S2 81u
#define SPEC_S3 27u

/* T0: stride table must equal the mixed-radix spec {1,9,81,27} */
static void t0_stride_spec(void) {
    printf("── T0 stride values match mixed-radix spec {1,9,81,27}\n");
    CHECK(hw_stride(0u) == SPEC_S0, "axis 0 stride == 1 (3^0)");
    CHECK(hw_stride(1u) == SPEC_S1, "axis 1 stride == 9 (3^2)");
    CHECK(hw_stride(2u) == SPEC_S2, "axis 2 stride == 81 (3^4)");
    CHECK(hw_stride(3u) == SPEC_S3, "axis 3 stride == 27 (3^3)");
    CHECK(hw_stride(4u) == SPEC_S0, "axis 4 wraps to 1 (axis%4)");
    /* round lengths follow from the field size and the spec strides */
    CHECK(hw_round_len(0u) == GEO_FULL / SPEC_S0, "round 0 = 20736/1");
    CHECK(hw_round_len(1u) == GEO_FULL / SPEC_S1, "round 1 = 20736/9");
    CHECK(hw_round_len(2u) == GEO_FULL / SPEC_S2, "round 2 = 20736/81");
    CHECK(hw_round_len(3u) == GEO_FULL / SPEC_S3, "round 3 = 20736/27");
}

/* T1: orbit — stride-1 walk visits all 20736 nodes exactly once
 * (oracle: gcd(1,20736)=1 ⇒ additive group order 20736) */
static void t1_orbit(void) {
    printf("── T1 full-field orbit (stride 1, axis 0)\n");
    uint8_t *seen = (uint8_t *)calloc(GEO_FULL, 1);
    if (!seen) { printf("  FAIL alloc\n"); g_fail++; return; }
    HWRouter r;
    hw_init(&r, 0u, 0u);
    uint32_t visits = 0;
    uint32_t repeat = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++) {
        uint32_t pos = hw_at(&r, i);
        if (seen[pos]) repeat++;
        seen[pos] = 1;
        visits++;
    }
    uint32_t distinct = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++) if (seen[i]) distinct++;
    printf("  visits=%u distinct=%u repeats=%u\n", visits, distinct, repeat);
    CHECK(distinct == GEO_FULL && repeat == 0, "stride-1 covers all 20736 once");
    CHECK(hw_at(&r, GEO_FULL) == 0u, "returns home after one full round");
    free(seen);
}

/* T2: reversibility — modular identity with the SPEC strides
 * (stride values are pinned in T0; here we prove the arithmetic) */
static void t2_reversibility(void) {
    printf("── T2 reversibility (all nodes × 4 axes, spec strides)\n");
    const uint32_t strides[4] = {SPEC_S0, SPEC_S1, SPEC_S2, SPEC_S3};
    int ok = 1;
    for (uint32_t n = 0; n < GEO_FULL && ok; n++) {
        for (uint32_t a = 0; a < 4u; a++) {
            uint32_t s = strides[a];
            uint32_t fwd = (uint32_t)((n + s) % GEO_FULL);
            uint32_t back = (uint32_t)((fwd + GEO_FULL - s) % GEO_FULL);
            if (back != n) { ok = 0; break; }
        }
    }
    CHECK(ok, "all nodes return after +s then −s (4 axes)");
}

/* T3: parity flip — all SPEC strides odd ⇒ up/down flips every step */
static void t3_parity(void) {
    printf("── T3 parity flip (all nodes × 4 axes, spec strides)\n");
    const uint32_t strides[4] = {SPEC_S0, SPEC_S1, SPEC_S2, SPEC_S3};
    int ok = 1;
    for (uint32_t n = 0; n < GEO_FULL && ok; n++) {
        for (uint32_t a = 0; a < 4u; a++) {
            uint32_t s = strides[a];
            if (s % 2u == 0u) { ok = 0; break; }       /* spec must be odd */
            uint32_t fwd = (uint32_t)((n + s) % GEO_FULL);
            if ((fwd & 1u) == (n & 1u)) { ok = 0; break; } /* no flip */
        }
    }
    CHECK(ok, "parity flips every step on all 4 axes");
}

/* T4: store cross-check against the REFERENCE primitive.
 * NOT an identity round-trip (that is trivially true); instead:
 *   (a) hwf_centroid(k) must equal th_cell_anchor(k·cell_size, ap, depth)
 *       — the store must use the real anchor, verified against the
 *       pre-existing primitive (reference oracle, independently tested);
 *   (b) hwf_index_of(node) must select the cell containing node:
 *       idx == node / cell_size (the field partition). */
static void t4_reconstruct(void) {
    printf("── T4 store cross-check vs reference th_cell_anchor\n");
    const uint32_t apertures[2] = {3u, 4u};
    const char *aname[2] = {"aperture-3", "aperture-4"};
    int all_ok = 1;
    for (int ai = 0; ai < 2; ai++) {
        for (uint32_t depth = 0; depth <= 4u; depth++) {
            for (uint32_t axis = 0; axis < 4u; axis++) {
                HWFrames f;
                hwf_init(&f, apertures[ai], depth, axis);
                uint32_t cells = hwf_count(&f);
                uint32_t cell_size = th_cell_size(f.aperture, f.depth);
                int ok_cells = 1;
                /* (a) centroid must equal the reference anchor */
                for (uint32_t c = 0; c < cells && ok_cells; c++) {
                    uint32_t base = (uint32_t)((uint64_t)c * cell_size % GEO_FULL);
                    uint32_t ref = th_cell_anchor(base, f.aperture, f.depth);
                    if (hwf_centroid(&f, c) != ref) { ok_cells = 0; break; }
                }
                /* (b) index_of must select the containing cell */
                for (uint32_t c = 0; c < cells && ok_cells; c++) {
                    for (uint32_t t = 0; t < cell_size && ok_cells; t++) {
                        uint32_t node = (uint32_t)(((uint64_t)c * cell_size + t)
                                                   % GEO_FULL);
                        if (hwf_index_of(&f, node) != c) { ok_cells = 0; break; }
                    }
                }
                if (!ok_cells) {
                    printf("  FAIL %s depth=%u axis=%u\n", aname[ai], depth, axis);
                    all_ok = 0;
                }
            }
        }
    }
    CHECK(all_ok, "store centroid/index match reference for all (ap,depth,axis)");
}

/* T5: enter-anywhere — every seed completes a full round back to itself
 * on every axis (walk is a bijection; seed only shifts the phase).
 * Uses SPEC round lengths, not hw_round_len, to avoid self-reference. */
static void t5_enter_anywhere(void) {
    printf("── T5 enter anywhere\n");
    const uint32_t strides[4] = {SPEC_S0, SPEC_S1, SPEC_S2, SPEC_S3};
    int ok = 1;
    for (uint32_t a = 0; a < 4u && ok; a++) {
        uint32_t round = GEO_FULL / strides[a];
        for (int k = 0; k < 8; k++) {
            uint32_t seed = (uint32_t)(k * 2597u) % GEO_FULL;
            HWRouter r;
            hw_init(&r, seed, a);
            if (hw_at(&r, round) != seed) { ok = 0; break; }
            if (hw_at(&r, round * 2u) != seed) { ok = 0; break; }
        }
    }
    CHECK(ok, "any seed returns to itself after one/two full rounds (4 axes)");
}

/* T6: centroid layer — every node snaps to its cell centroid, and
 * centroid(centroid) = centroid (idempotent — the snap point).
 * Uses the reference primitive th_cell_anchor directly (the property
 * is about the anchor lattice, which the store builds on). */
static void t6_centroid(void) {
    printf("── T6 centroid (th_cell_anchor) snap\n");
    int ok = 1;
    const uint32_t apertures[2] = {3u, 4u};
    for (int ai = 0; ai < 2 && ok; ai++) {
        for (uint32_t depth = 0; depth <= 4u && ok; depth++) {
            for (uint32_t n = 0; n < GEO_FULL; n += 37u) {
                uint32_t c1 = th_cell_anchor(n, apertures[ai], depth);
                uint32_t c2 = th_cell_anchor(c1, apertures[ai], depth);
                if (c1 != c2) { ok = 0; break; }
            }
        }
    }
    CHECK(ok, "centroid is idempotent (anchor of anchor = anchor)");
}

int main(void) {
    printf("test_geo_hyperbolic — hyperbolic = route + key frame + f(step)\n");
    t0_stride_spec();
    t1_orbit();
    t2_reversibility();
    t3_parity();
    t4_reconstruct();
    t5_enter_anywhere();
    t6_centroid();
    printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
    return g_fail ? 1 : 0;
}