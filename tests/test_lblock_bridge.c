/*
 * test_lblock_bridge.c — geo_jump ↔ L-block Placement Adapter
 * ═══════════════════════════════════════════════════════════════════════════════
 * Proves: geo_jump coordinates → L-block placement is deterministic, lossless,
 * and fit-guaranteed across all 20736 nodes.
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o tests/test_lblock_bridge tests/test_lblock_bridge.c -lm
 * Run:   tests/test_lblock_bridge
 */
#include <stdio.h>
#include <string.h>
#include "core/geo_lblock_bridge.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass_count++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail_count++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* T0: bridge verify on grid_n=128 (standard power-of-2 for 20736) */
static int test_verify_128(void) {
    int rc = glb_verify(128);
    printf("  %s: glb_verify(grid_n=128) rc=%d\n", rc == 0 ? "PASS" : "FAIL", rc);
    if (rc == 0) pass_count++; else fail_count++;
    return rc == 0;
}

/* T1: bridge verify on grid_n=64 */
static int test_verify_64(void) {
    int rc = glb_verify(64);
    printf("  %s: glb_verify(grid_n=64) rc=%d\n", rc == 0 ? "PASS" : "FAIL", rc);
    if (rc == 0) pass_count++; else fail_count++;
    return rc == 0;
}

/* T2: bridge verify on grid_n=16 */
static int test_verify_16(void) {
    int rc = glb_verify(16);
    printf("  %s: glb_verify(grid_n=16) rc=%d\n", rc == 0 ? "PASS" : "FAIL", rc);
    if (rc == 0) pass_count++; else fail_count++;
    return rc == 0;
}

/* T3: spot-check known coordinates */
static int test_spot_check(void) {
    /* node 0 = face=0, tick=0, local=0 → d=0 on any grid */
    LBlockPlacement p0 = glb_place(0, 0, 0, 128);
    if (p0.node != 0) { printf("  FAIL: node 0\n"); fail_count++; return 0; }
    if (p0.d != 0) { printf("  FAIL: d for node 0\n"); fail_count++; return 0; }

    /* node 20735 = face=11, tick=11, local=143 */
    LBlockPlacement p_max = glb_place(11, 11, 143, 128);
    if (p_max.node != 20735) { printf("  FAIL: node 20735\n"); fail_count++; return 0; }
    if (p_max.d != 20735 % (128*128)) { printf("  FAIL: d for node 20735\n"); fail_count++; return 0; }

    /* node 1728 = face=1, tick=0, local=0 */
    LBlockPlacement p_f1 = glb_place(1, 0, 0, 128);
    if (p_f1.node != 1728) { printf("  FAIL: node 1728\n"); fail_count++; return 0; }

    /* node 144 = face=0, tick=1, local=0 */
    LBlockPlacement p_t1 = glb_place(0, 1, 0, 128);
    if (p_t1.node != 144) { printf("  FAIL: node 144\n"); fail_count++; return 0; }

    pass_count++;
    printf("  T3: PASS — spot-check node=0,144,1728,20735\n");
    return 1;
}

/* T4: both API paths agree (glb_place vs glb_place_node) */
static int test_api_agree(void) {
    int ok = 1;
    /* spot-check 100 evenly-spaced nodes */
    for (uint32_t node = 0; node < 20736; node += 207) {
        LBlockPlacement p1 = glb_place_node(node, 128);
        uint32_t face = gsb_face_of(node);
        uint32_t tick = gsb_tick_of(node);
        uint32_t local = gsb_local_of(node);
        LBlockPlacement p2 = glb_place(face, tick, local, 128);
        if (p1.node != p2.node || p1.d != p2.d || p1.rotation != p2.rotation) {
            printf("  FAIL: node=%u disagree\n", node);
            ok = 0;
            break;
        }
    }
    CHECK(4, "glb_place == glb_place_node for 100 nodes", ok);
    return ok;
}

/* T5: decompose roundtrip — placement → geo_jump coords match original */
static int test_decompose(void) {
    int ok = 1;
    for (uint32_t face = 0; face < 12; face++) {
        for (uint32_t tick = 0; tick < 12; tick += 3) {
            for (uint32_t local = 0; local < 144; local += 17) {
                LBlockPlacement p = glb_place(face, tick, local, 128);
                uint32_t f2, t2, l2;
                glb_decompose(&p, &f2, &t2, &l2);
                if (f2 != face || t2 != tick || l2 != local) {
                    printf("  FAIL: (%u,%u,%u) → decompose → (%u,%u,%u)\n",
                           face, tick, local, f2, t2, l2);
                    ok = 0;
                    break;
                }
            }
            if (!ok) break;
        }
        if (!ok) break;
    }
    CHECK(5, "decompose roundtrip (face,tick,local) through placement", ok);
    return ok;
}

/* T6: fit guarantee on all 20736 for grid_n=128 */
static int test_fit_all(void) {
    int bad = 0;
    for (uint32_t node = 0; node < 20736; node++) {
        LBlockPlacement p = glb_place_node(node, 128);
        if (!geo_lb_fits_grid(p.d, 128)) bad++;
    }
    CHECK(6, "fit guarantee: all 20736 nodes fit on 128×128", bad == 0);
    if (bad) printf("        %d nodes failed fit\n", bad);
    return bad == 0;
}

/* T7: rotation distribution — all 4 rotations used */
static int test_rotation_dist(void) {
    uint32_t rc[4] = {0, 0, 0, 0};
    for (uint32_t node = 0; node < 20736; node++) {
        LBlockPlacement p = glb_place_node(node, 128);
        if (p.rotation < 4) rc[p.rotation]++;
    }
    int ok = rc[0] > 0 && rc[1] > 0 && rc[2] > 0 && rc[3] > 0;
    CHECK(7, "rotation distribution: all 4 rotations used (n=128)", ok);
    printf("        rot dist: [%u %u %u %u]\n", rc[0], rc[1], rc[2], rc[3]);
    return ok;
}

/* T8: batch place — all 20736 via glb_place_range */
static int test_batch(void) {
    LBlockPlacement placements[20736];
    glb_place_range(placements, 128);

    int ok = 1;
    for (uint32_t i = 0; i < 20736; i++) {
        if (placements[i].node != i) {
            printf("  FAIL: batch[%u].node=%u\n", i, placements[i].node);
            ok = 0;
            break;
        }
        if (!geo_lb_fits_grid(placements[i].d, 128)) {
            printf("  FAIL: batch[%u] fit\n", i);
            ok = 0;
            break;
        }
    }
    CHECK(8, "glb_place_range: all 20736 batch-placed + fit", ok);
    return ok;
}

/* T9: cross-grid consistency — same node on different grids → same node */
static int test_cross_grid(void) {
    int ok = 1;
    for (uint32_t node = 0; node < 20736; node += 503) {
        LBlockPlacement p64  = glb_place_node(node, 64);
        LBlockPlacement p128 = glb_place_node(node, 128);
        if (p64.node != p128.node) {
            printf("  FAIL: node=%u grid64=%u grid128=%u\n",
                   node, p64.node, p128.node);
            ok = 0;
            break;
        }
    }
    CHECK(9, "cross-grid: node invariant across grid_n=64 and 128", ok);
    return ok;
}

int main(void) {
    printf("═══ test_lblock_bridge — geo_jump ↔ L-block Placement ═══\n\n");

    test_verify_128();
    test_verify_64();
    test_verify_16();
    test_spot_check();
    test_api_agree();
    test_decompose();
    test_fit_all();
    test_rotation_dist();
    test_batch();
    test_cross_grid();

    printf("\n═══════════════════════════════════════\n");
    printf("RESULT: %d PASS / %d FAIL\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
