/* test_system_full.c — Full system integration test.
 * Exercises ALL components together: balanced ternary, 3/4 branch, 111^3 walking,
 * stride-37 scatter, zero-copy resolve, quadtree cascade, Voronoi mask,
 * constant distort, inside-out, palindrome.
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_system_full tests/test_system_full.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tess_resolver.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Full System Integration Test ===\n\n");

    /* ── 1. Balanced ternary roundtrip (full range) ── */
    printf("1. Balanced ternary roundtrip...\n");
    {
        int digits[12];
        int ok = 1;
        for (int v = -500; v <= 500; v++) {
            if (bt_encode(v, digits, 12) != 0) { ok = 0; break; }
            if (bt_decode(digits, 12) != v) { ok = 0; break; }
        }
        CHECK(ok, "balanced ternary: -500..500 identity (12 digits)");
    }

    /* ── 2. 3/4 branch + balanced ternary integration ── */
    printf("2. 3/4 branch integration...\n");
    {
        CHECK(tetra_usable_count() == 3, "branch factor = 3");
        CHECK(!tetra_can_branch(3), "face 3 blocked");
        int digits[3];
        int ok = 1;
        for (int v = -13; v <= 13; v++) {
            if (bt_encode(v, digits, 3) != 0) { ok = 0; break; }
            uint32_t slot = bt_coord_to_slot(digits[0], digits[1], digits[2]);
            if (slot >= TESS_TOTAL_SLOTS) { ok = 0; break; }
        }
        CHECK(ok, "ternary coord → slot (all 27 positions valid)");
    }

    /* ── 3. 111^3 walking + rewind ── */
    printf("3. 111^3 walking + rewind...\n");
    {
        WalkPos p = {0,0,0};
        /* Walk a path */
        for (int i = 0; i < 50; i++) p = walk_step(p, 1, 1, 0);
        (void)p; /* saved position, not used in this test */
        /* Walk back */
        for (int i = 0; i < 50; i++) p = walk_step(p, -1, -1, 0);
        CHECK(p.x == 0 && p.y == 0 && p.z == 0, "walk 50+50 = origin (rewind)");
        /* Deterministic */
        WalkPos a = {0,0,0}, b = {0,0,0};
        for (int i = 0; i < 30; i++) { a = walk_step(a, 2, -1, 1); b = walk_step(b, 2, -1, 1); }
        CHECK(a.x==b.x && a.y==b.y && a.z==b.z, "walking deterministic");
    }

    /* ── 4. Stride-37 roundtrip + zero-copy ── */
    printf("4. Stride-37 + zero-copy...\n");
    {
        uint32_t csz = 4;
        uint8_t *base = calloc(TESS_TOTAL_SLOTS * csz, 1);
        for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) {
            uint32_t v = i; memcpy(base + i*csz, &v, csz);
        }
        /* Scatter */
        uint8_t *scat = calloc(TESS_TOTAL_SLOTS * csz, 1);
        for (uint32_t s = 0; s < TESS_TOTAL_SLOTS; s++) {
            uint32_t cell = tess_slot_to_cell(s);
            memcpy(scat + cell*csz, base + s*csz, csz);
        }
        /* Resolve (zero-copy) */
        TessCubeView cube = { .base = scat, .cell_size = csz, .n_cells = TESS_TOTAL_SLOTS, .capo_id = 0 };
        int ok = 1;
        for (uint32_t s = 0; s < TESS_TOTAL_SLOTS; s++) {
            const uint8_t *p = tess_resolve_ptr(&cube, s);
            if (p < scat || p >= scat + TESS_TOTAL_SLOTS*csz) { ok = 0; break; }
            uint32_t val; memcpy(&val, p, csz);
            if (val != s) { ok = 0; break; }
        }
        CHECK(ok, "stride-37 roundtrip + zero-copy (20736 cells)");
        free(base); free(scat);
    }

    /* ── 5. Cascade (rewind) ── */
    printf("5. Cascade (rewind + domino shrink)...\n");
    {
        TessCascade c = {0};
        tess_cascade_activate(&c, 5);
        tess_cascade_activate(&c, 10);
        CHECK(c.root == 1, "cascade active with 2 leaves");
        tess_cascade_deactivate(&c, 5);
        CHECK(c.root == 1, "cascade still active (leaf 10 remains)");
        tess_cascade_deactivate(&c, 10);
        CHECK(c.root == 0 && c.active == 0, "cascade inactive (domino shrink)");
    }

    /* ── 6. Voronoi mask (multi-pointer) ── */
    printf("6. Voronoi mask (multi-pointer)...\n");
    {
        TessVoronoiMask m = {0};
        tess_voronoi_apply(&m, 100);
        tess_voronoi_apply(&m, 200);
        CHECK(m.applied == 1, "mask applied");
        CHECK(tess_voronoi_is_unfolded(&m, 100) && tess_voronoi_is_unfolded(&m, 200), "both unfolded");
        tess_voronoi_clear(&m, 100);
        CHECK(tess_voronoi_is_unfolded(&m, 100) == 0, "100 re-folded");
        CHECK(tess_voronoi_is_unfolded(&m, 200) == 1, "200 still unfolded");
        tess_voronoi_clear(&m, 200);
        CHECK(m.applied == 0, "mask fully cleared");
    }

    /* ── 7. Constant distort (hyperbolic two-sided) ── */
    printf("7. Constant distort (hyperbolic)...\n");
    {
        uint32_t total = TESS_TOTAL_SLOTS;
        uint32_t ptr = 1000;
        CHECK(tess_distort(ptr, ptr, total) == 0, "near = 0 (exact)");
        CHECK(tess_distort(ptr + 5000, ptr, total) > 0, "far > 0 (distort)");
        CHECK(tess_distort(ptr + 5000, ptr, total) == tess_distort(ptr, ptr + 5000, total),
              "symmetric (two-sided)");
        CHECK(tess_distort(ptr + 5000, ptr, total) == tess_distort(ptr + 5000, ptr, total),
              "deterministic");
    }

    /* ── 8. Inside-out (pos24 + neg24 = 48) ── */
    printf("8. Inside-out (octahedral group)...\n");
    {
        CHECK(POS_COUNT + NEG_COUNT == OCTAHEDRAL_GROUP, "24+24=48");
        CHECK(OCTAHEDRAL_GROUP == 8 * 6, "48 = 8*6");
        CHECK(POS_COUNT - NEG_COUNT == 0, "closed loop (returns to origin)");
    }

    /* ── 9. Palindrome symmetry ── */
    printf("9. Palindrome symmetry...\n");
    {
        CHECK(111 * 111 == 12321, "111^2 = 12321");
        CHECK(111 * 111 * 111 == 1367631, "111^3 = 1367631");
    }

    /* ── 10. 24 Tetra identity ── */
    printf("10. 24 Tetra identity...\n");
    {
        CHECK(TETRA_PER_AXIS * TETRA_AXES == TETRA_TOTAL, "8*3 = 24 tetra");
        CHECK(TETRA_AXES == 3, "3 square planes = identity anchor");
    }

    /* ── 11. bt_coord_to_slot (balanced ternary → slot) ── */
    printf("11. Balanced ternary coord → slot...\n");
    {
        int ok = 1;
        for (int d0 = -1; d0 <= 1; d0++) {
            for (int d1 = -1; d1 <= 1; d1++) {
                for (int d2 = -1; d2 <= 1; d2++) {
                    uint32_t slot = bt_coord_to_slot(d0, d1, d2);
                    if (slot >= TESS_TOTAL_SLOTS) { ok = 0; break; }
                }
                if (!ok) break;
            }
            if (!ok) break;
        }
        CHECK(ok, "all 27 balanced ternary coords → valid slot");
    }

    /* ── 12. Full integration: ternary → slot → resolve → cascade ── */
    printf("12. Full integration (ternary → slot → resolve → cascade)...\n");
    {
        uint32_t csz = 4;
        uint8_t *base = calloc(TESS_TOTAL_SLOTS * csz, 1);
        for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) {
            uint32_t v = i; memcpy(base + i*csz, &v, csz);
        }
        uint8_t *scat = calloc(TESS_TOTAL_SLOTS * csz, 1);
        for (uint32_t s = 0; s < TESS_TOTAL_SLOTS; s++) {
            uint32_t cell = tess_slot_to_cell(s);
            memcpy(scat + cell*csz, base + s*csz, csz);
        }
        TessCubeView cube = { .base = scat, .cell_size = csz, .n_cells = TESS_TOTAL_SLOTS, .capo_id = 0 };

        /* Walk a path using 111^3 engine, resolve at each step */
        WalkPos p = {0,0,0};
        TessCascade cascade = {0};
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            p = walk_step(p, 1, 0, 0);
            /* Convert walk position to slot (simple: x mod 20736) */
            uint32_t slot = (uint32_t)((p.x + 111) * (TESS_TOTAL_SLOTS / 223));
            if (slot >= TESS_TOTAL_SLOTS) slot = TESS_TOTAL_SLOTS - 1;
            /* Resolve (zero-copy) */
            const uint8_t *ptr = tess_resolve_ptr(&cube, slot);
            if (ptr < scat || ptr >= scat + TESS_TOTAL_SLOTS*csz) { ok = 0; break; }
            /* Cascade activate (rewind) */
            tess_cascade_activate(&cascade, (int)(slot % 64));
        }
        CHECK(ok, "full integration: walk → resolve → cascade (10 steps)");
        CHECK(cascade.active == 1, "cascade active after walk");
        free(base); free(scat);
    }

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
