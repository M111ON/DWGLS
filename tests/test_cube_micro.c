/* test_cube_micro.c — Micro-proof: 1 cube, zero-copy, stride-37, quadtree cascade,
 * Voronoi mask, constant distort (hyperbolic two-sided).
 *
 * Tests:
 *   1. Stride-37 roundtrip: scatter → resolve → byte-identical
 *   2. Zero-copy: resolver returns pointer into base, no memcpy
 *   3. Cascade: leaf activate → parent OR propagation (rewind)
 *   4. Cascade: leaf deactivate → recompute (domino shrink)
 *   5. Voronoi mask: apply → unfolded, clear → folded
 *   6. Constant distort: near pointer → ~0, far → large, deterministic
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_cube_micro tests/test_cube_micro.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tess_resolver.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Micro-Proof: 1 Cube (20736 cells, stride-37) ===\n\n");

    /* ── Setup: synthetic cube (20736 cells, 4 bytes each = 82944 bytes) ── */
    uint32_t csz = 4; /* F32 */
    uint8_t *base = (uint8_t *)calloc(TESS_TOTAL_SLOTS * csz, 1);
    /* Fill with known pattern: cell[i] = i (as uint32) */
    for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) {
        uint32_t val = i;
        memcpy(base + i * csz, &val, csz);
    }

    TessCubeView cube = { .base = base, .cell_size = csz, .n_cells = TESS_TOTAL_SLOTS, .capo_id = 0 };

    /* ── Test 1: Stride-37 roundtrip ── */
    printf("Test 1: Stride-37 scatter roundtrip\n");
    {
        /* Scatter: slot s → cell (s*37)%20736 */
        /* Resolve: cell → slot (inverse) */
        int ok = 1;
        for (uint32_t s = 0; s < TESS_TOTAL_SLOTS; s++) {
            uint32_t cell = tess_slot_to_cell(s);
            uint32_t s2 = tess_cell_to_slot(cell);
            if (s2 != s) { ok = 0; break; }
        }
        CHECK(ok, "stride-37 bijection: slot→cell→slot identity for all 20736");
    }

    /* ── Test 2: Zero-copy resolution ── */
    printf("\nTest 2: Zero-copy pointer arithmetic\n");
    {
        const uint8_t *p0 = tess_resolve_ptr(&cube, 0);
        const uint8_t *p1 = tess_resolve_ptr(&cube, 1);
        /* Pointer must be within base, no allocation */
        CHECK(p0 >= base && p0 < base + TESS_TOTAL_SLOTS * csz, "resolve(0) within base");
        CHECK(p1 >= base && p1 < base + TESS_TOTAL_SLOTS * csz, "resolve(1) within base");
        CHECK(p0 != p1, "resolve(0) != resolve(1) (different slots)");
        /* Verify no memcpy: pointer arithmetic only */
        uint32_t cell0 = tess_slot_to_cell(0);
        CHECK(p0 == base + cell0 * csz, "resolve = base + cell*csz (arithmetic only)");
    }

    /* ── Test 3: Cascade activate (rewind) ── */
    printf("\nTest 3: Quadtree cascade activate (rewind)\n");
    {
        TessCascade c = {0};
        tess_cascade_activate(&c, 5); /* activate leaf 5 */
        CHECK(c.leaf[5] == 1, "leaf[5] activated");
        CHECK(c.l16[5/4] == 1, "l16[1] propagated (OR)");
        CHECK(c.l4[0] == 1, "l4[0] propagated (OR)");
        CHECK(c.root == 1, "root activated (OR)");
        CHECK(c.active == 1, "cascade active");
    }

    /* ── Test 4: Cascade deactivate (domino shrink) ── */
    printf("\nTest 4: Quadtree cascade deactivate (domino shrink)\n");
    {
        TessCascade c = {0};
        tess_cascade_activate(&c, 5);
        tess_cascade_activate(&c, 10);
        CHECK(c.root == 1, "root active with 2 leaves");
        tess_cascade_deactivate(&c, 5);
        CHECK(c.leaf[5] == 0, "leaf[5] deactivated");
        CHECK(c.root == 1, "root still active (leaf[10] remains)");
        tess_cascade_deactivate(&c, 10);
        CHECK(c.root == 0, "root deactivated (domino shrink complete)");
        CHECK(c.active == 0, "cascade inactive");
    }

    /* ── Test 5: Voronoi mask (multi-pointer read) ── */
    printf("\nTest 5: Voronoi mask (temporary unfold)\n");
    {
        TessVoronoiMask m = {0};
        CHECK(tess_voronoi_is_unfolded(&m, 100) == 0, "slot 100 initially folded");
        tess_voronoi_apply(&m, 100);
        CHECK(tess_voronoi_is_unfolded(&m, 100) == 1, "slot 100 unfolded after apply");
        CHECK(m.applied == 1, "mask applied");
        tess_voronoi_clear(&m, 100);
        CHECK(tess_voronoi_is_unfolded(&m, 100) == 0, "slot 100 re-folded after clear");
        CHECK(m.applied == 0, "mask cleared");
    }

    /* ── Test 6: Constant distort (hyperbolic two-sided) ── */
    printf("\nTest 6: Constant distort (hyperbolic two-sided)\n");
    {
        uint32_t ptr = 1000;
        uint32_t d_near = tess_distort(ptr, ptr, TESS_TOTAL_SLOTS);       /* distance 0 */
        uint32_t d_far = tess_distort(ptr + 5000, ptr, TESS_TOTAL_SLOTS); /* distance 5000 */
        CHECK(d_near == 0, "distort at pointer position = 0 (exact)");
        CHECK(d_far > d_near, "distort grows with distance");
        /* Deterministic: same input → same output */
        CHECK(tess_distort(ptr + 5000, ptr, TESS_TOTAL_SLOTS) == d_far, "distort is deterministic");
        /* Two-sided: symmetric */
        CHECK(tess_distort(ptr, ptr + 5000, TESS_TOTAL_SLOTS) == tess_distort(ptr + 5000, ptr, TESS_TOTAL_SLOTS),
              "distort is symmetric (two-sided hyperbolic)");
        printf("  INFO: d_near=%u d_far=%u (constant, not random)\n", d_near, d_far);
    }

    /* ── Test 7: Data roundtrip (scatter → resolve → compare) ── */
    printf("\nTest 7: Data roundtrip (scatter → resolve)\n");
    {
        /* Build scattered cube: cell[slot_to_cell(s)] = original[s] */
        uint8_t *scattered = (uint8_t *)calloc(TESS_TOTAL_SLOTS * csz, 1);
        for (uint32_t s = 0; s < TESS_TOTAL_SLOTS; s++) {
            uint32_t cell = tess_slot_to_cell(s);
            memcpy(scattered + cell * csz, base + s * csz, csz);
        }
        /* Resolve: read back via pointer arithmetic */
        TessCubeView sc = { .base = scattered, .cell_size = csz, .n_cells = TESS_TOTAL_SLOTS, .capo_id = 0 };
        int ok = 1;
        for (uint32_t s = 0; s < TESS_TOTAL_SLOTS; s++) {
            const uint8_t *p = tess_resolve_ptr(&sc, s);
            uint32_t val;
            memcpy(&val, p, csz);
            if (val != s) { ok = 0; break; }
        }
        CHECK(ok, "roundtrip: resolve(s) == original[s] for all 20736");
        free(scattered);
    }

    free(base);

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
