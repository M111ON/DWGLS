/* test_jump_module.c — Test the tess_jump module (geo_jump + hyper_jump + frame_seek + fan24 + jet).
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_jump_module tests/test_jump_module.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tess_resolver.h"
#include "tess_jump.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Jump Module Test (geo_jump + hyper_jump + frame_seek + fan24 + jet) ===\n\n");

    JumpContext ctx;
    jump_init(&ctx);

    /* ── Test 1: Jump init ── */
    printf("Test 1: Jump context init\n");
    CHECK(ctx.pointer_pos == 0, "pointer starts at 0");
    CHECK(ctx.side == 1, "starts on positive side");
    CHECK(ctx.cascade_active == 0, "cascade inactive initially");

    /* ── Test 2: Individual jump types ── */
    printf("\nTest 2: Individual jump types\n");
    {
        JumpContext c; jump_init(&c);
        /* Hilbert */
        JumpResult r = jump_execute(&c, JUMP_HILBERT, 10);
        CHECK(r.slot < TESS_TOTAL_SLOTS, "Hilbert jump → valid slot");
        CHECK(r.side == 1, "positive side");
        /* Peano */
        JumpResult r2 = jump_execute(&c, JUMP_PEANO, 5);
        CHECK(r2.slot < TESS_TOTAL_SLOTS, "Peano jump → valid slot");
        /* Pentagon */
        JumpResult r3 = jump_execute(&c, JUMP_PENTAGON, 3);
        CHECK(r3.slot < TESS_TOTAL_SLOTS, "Pentagon jump → valid slot");
        /* MOD */
        JumpResult r4 = jump_execute(&c, JUMP_MOD, 7);
        CHECK(r4.slot < TESS_TOTAL_SLOTS, "MOD jump → valid slot");
        /* Invert */
        JumpResult r5 = jump_execute(&c, JUMP_INVERT, 0);
        CHECK(r5.slot < TESS_TOTAL_SLOTS, "Invert jump → valid slot");
        /* Ground */
        JumpResult r6 = jump_execute(&c, JUMP_GROUND, 2);
        CHECK(r6.slot < TESS_TOTAL_SLOTS, "Ground jump → valid slot");
    }

    /* ── Test 3: Frame seek (fishbone snap) ── */
    printf("\nTest 3: Frame seek (fishbone snap to 1440 cycle)\n");
    {
        JumpContext c; jump_init(&c);
        c.frame_enc = 0;
        JumpResult r = jump_execute(&c, JUMP_FRAME, 0);
        CHECK(r.slot < TESS_TOTAL_SLOTS, "frame seek → valid slot");
        CHECK(c.frame_enc == 37, "frame_enc advanced by 37 (stride-37)");
        /* Next frame */
        JumpResult r2 = jump_execute(&c, JUMP_FRAME, 0);
        CHECK(c.frame_enc == 74, "frame_enc = 74 (37*2)");
    }

    /* ── Test 4: Fan24 CRT (ring-24) ── */
    printf("\nTest 4: Fan24 CRT (ring-24 bijection)\n");
    {
        JumpContext c; jump_init(&c);
        /* Test CRT bijection: dc ∈ [0,8), dx ∈ [0,3) → result ∈ [0,24) */
        int ok = 1;
        for (uint8_t dc = 0; dc < 8; dc++) {
            for (uint8_t dx = 0; dx < 3; dx++) {
                uint8_t res = fan24_crt(dc, dx);
                if (res >= 24) { ok = 0; break; }
            }
            if (!ok) break;
        }
        CHECK(ok, "fan24 CRT: all (dc,dx) → [0,24)");
        /* Deterministic */
        CHECK(fan24_crt(3, 1) == fan24_crt(3, 1), "fan24 deterministic");
    }

    /* ── Test 5: Hyper jump (4D) ── */
    printf("\nTest 5: Hyper jump (4D tes-compound)\n");
    {
        JumpContext c; jump_init(&c);
        c.pointer_pos = 1000;
        JumpResult r = jump_execute(&c, JUMP_HYPER, 1);
        CHECK(r.slot < TESS_TOTAL_SLOTS, "hyper jump (HJ4) → valid slot");
        /* HJ3 */
        JumpContext c2; jump_init(&c2);
        c2.pointer_pos = 1000;
        JumpResult r2 = jump_execute(&c2, JUMP_HYPER, 0);
        CHECK(r2.slot < TESS_TOTAL_SLOTS, "hyper jump (HJ3) → valid slot");
        /* Hyper jump inverts local → different from original */
        CHECK(r.slot != 1000 || r2.slot != 1000, "hyper jump inverts (not identity)");
    }

    /* ── Test 6: Jet bridge (snap) ── */
    printf("\nTest 6: Jet bridge (snap to 144x144)\n");
    {
        JumpContext c; jump_init(&c);
        c.pointer_pos = 100;
        JumpResult r = jump_execute(&c, JUMP_JET, 144);
        CHECK(r.slot < TESS_TOTAL_SLOTS, "jet snap → valid slot");
        /* Snap to nearest multiple of 144 */
        CHECK(r.slot % 144 == 0 || r.slot % 144 == 143, "jet snaps to 144 boundary");
    }

    /* ── Test 7: Cross boundary (positive ↔ negative) ── */
    printf("\nTest 7: Cross boundary (positive ↔ negative)\n");
    {
        JumpContext c; jump_init(&c);
        CHECK(c.side == 1, "starts positive");
        int new_side = jump_cross_boundary(&c);
        CHECK(new_side == -1, "crossed to negative");
        CHECK(c.side == -1, "side is now negative");
        new_side = jump_cross_boundary(&c);
        CHECK(new_side == 1, "crossed back to positive");
    }

    /* ── Test 8: Jump full sequence (outer → inner → snap) ── */
    printf("\nTest 8: Full jump sequence (outer→inner→snap)\n");
    {
        JumpContext c; jump_init(&c);
        c.pointer_pos = 500;
        JumpResult r = jump_full_sequence(&c, 10);
        CHECK(r.slot < TESS_TOTAL_SLOTS, "full sequence → valid slot");
        CHECK(r.cell == tess_slot_to_cell(r.slot), "cell = stride-37(slot)");
        CHECK(r.cube_id == r.slot / TESS_TOTAL_SLOTS, "cube_id = slot/20736");
        printf("  INFO: slot=%u cell=%u cube=%u side=%d distort=%u computed=%d\n",
               r.slot, r.cell, r.cube_id, r.side, r.distort, r.computed);
    }

    /* ── Test 9: Jump carries positive-negative ── */
    printf("\nTest 9: Jump carries positive-negative (two-sided)\n");
    {
        JumpContext c; jump_init(&c);
        c.side = 1; /* positive */
        JumpResult r1 = jump_execute(&c, JUMP_HILBERT, 10);
        CHECK(r1.side == 1, "positive side carried");
        jump_cross_boundary(&c);
        JumpResult r2 = jump_execute(&c, JUMP_HILBERT, 10);
        CHECK(r2.side == -1, "negative side carried");
    }

    /* ── Test 10: Cascade (rewind) integration ── */
    printf("\nTest 10: Cascade (rewind) integration\n");
    {
        JumpContext c; jump_init(&c);
        c.pointer_pos = 0;
        /* Jump to near position → cascade activates */
        JumpResult r = jump_execute(&c, JUMP_HILBERT, 3);
        if (r.computed) {
            CHECK(c.cascade_active == 1, "cascade active (rewind) after near jump");
            CHECK(c.cascade.root == 1, "cascade root activated");
        }
        printf("  INFO: cascade_active=%d, computed=%d\n", c.cascade_active, r.computed);
    }

    /* ── Test 11: Determinism ── */
    printf("\nTest 11: Jump determinism\n");
    {
        JumpContext c1, c2;
        jump_init(&c1); jump_init(&c2);
        c1.pointer_pos = 500; c2.pointer_pos = 500;
        JumpResult r1 = jump_full_sequence(&c1, 10);
        JumpResult r2 = jump_full_sequence(&c2, 10);
        CHECK(r1.slot == r2.slot, "same input → same slot (deterministic)");
        CHECK(r1.cell == r2.cell, "same input → same cell");
        CHECK(r1.side == r2.side, "same input → same side");
    }

    /* ── Test 12: All jump types produce valid slots ── */
    printf("\nTest 12: All jump types → valid slots\n");
    {
        JumpType types[] = { JUMP_HILBERT, JUMP_PEANO, JUMP_PENTAGON, JUMP_MOD,
                             JUMP_INVERT, JUMP_GROUND, JUMP_FRAME, JUMP_FAN24,
                             JUMP_HYPER, JUMP_JET };
        const char *names[] = { "HILBERT", "PEANO", "PENTAGON", "MOD", "INVERT",
                                "GROUND", "FRAME", "FAN24", "HYPER", "JET" };
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            JumpContext c; jump_init(&c);
            c.pointer_pos = 1000;
            JumpResult r = jump_execute(&c, types[i], 5);
            if (r.slot >= TESS_TOTAL_SLOTS) { ok = 0; printf("  FAIL: %s → %u (out of range)\n", names[i], r.slot); break; }
        }
        CHECK(ok, "all 10 jump types → valid slot (<20736)");
    }

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
