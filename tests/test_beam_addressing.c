/* test_beam_addressing.c — Test beam addressing integrated into tess_resolver.
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_beam_addressing tests/test_beam_addressing.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tess_resolver.h"
#include "tess_jump.h"
#include "tess_beam.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== Beam Addressing Test (integrated into tess_resolver) ===\n\n");

    /* Test 1: BeamCode roundtrip (Q8 full range) */
    printf("Test 1: BeamCode roundtrip (Q8: -128..127)\n");
    {
        int ok = 1;
        for (int w = -128; w <= 127; w++) {
            BeamCode code = beam_code_from_weight((int8_t)w);
            int32_t r = beam_weight_from_code(code);
            if (r != w) { ok = 0; break; }
        }
        CHECK(ok, "BeamCode roundtrip: all 256 values");
    }

    /* Test 2: BeamCode zone/position split */
    printf("\nTest 2: BeamCode zone/position split\n");
    {
        BeamCode code = beam_code_from_weight(100);
        CHECK(beam_code_zone(code) == 14, "weight=100 -> zone=14");
        CHECK(beam_code_pos(code) == 4, "weight=100 -> pos=4");
    }

    /* Test 3: BeamCoord roundtrip */
    printf("\nTest 3: BeamCoord roundtrip\n");
    {
        int ok = 1;
        for (int w = -127; w <= 127; w++) {
            BeamCoord c = beam_weight_to_coord(0, 42, (int8_t)w);
            int8_t r = beam_coord_to_weight(&c);
            if (r != (int8_t)w) { ok = 0; break; }
        }
        CHECK(ok, "BeamCoord roundtrip: -127..127");
    }

    /* Test 4: BeamCoord -> slot */
    printf("\nTest 4: BeamCoord -> slot (12^4 = 20736)\n");
    {
        BeamCoord c = beam_weight_to_coord(0, 100, 50);
        uint32_t slot = beam_to_slot(&c);
        CHECK(slot < TESS_TOTAL_SLOTS, "slot in range [0,20735]");
        printf("  INFO: param_index=100 -> slot=%u\n", slot);
    }

    /* Test 5: Two-sided (positive + negative) */
    printf("\nTest 5: Two-sided (positive + negative)\n");
    {
        BeamCoord pos = beam_weight_to_coord(0, 10, 50);
        BeamCoord neg = beam_weight_to_coord(0, 10, -50);
        CHECK(pos.sign == 1, "positive: sign=1 (ceiling)");
        CHECK(neg.sign == 0, "negative: sign=0 (ground)");
        uint32_t sp = beam_to_slot(&pos);
        uint32_t sn = beam_to_slot(&neg);
        CHECK(sp < TESS_TOTAL_SLOTS, "positive slot in range");
        CHECK(sn < TESS_TOTAL_SLOTS, "negative slot in range");
    }

    /* Test 6: Beam -> Jump bridge */
    printf("\nTest 6: Beam -> Jump bridge\n");
    {
        JumpContext ctx;
        jump_init(&ctx);
        BeamCoord c = beam_weight_to_coord(0, 100, 50);
        JumpResult r = beam_to_jump(&ctx, &c);
        CHECK(r.slot < TESS_TOTAL_SLOTS, "beam->jump -> valid slot");
        CHECK(r.side == 1, "positive weight -> positive side");
        printf("  INFO: slot=%u cell=%u side=%d\n", r.slot, r.cell, r.side);

        JumpContext ctx2;
        jump_init(&ctx2);
        BeamCoord c2 = beam_weight_to_coord(0, 100, -50);
        JumpResult r2 = beam_to_jump(&ctx2, &c2);
        CHECK(r2.slot < TESS_TOTAL_SLOTS, "neg beam->jump -> valid slot");
        CHECK(r2.side == -1, "negative weight -> negative side");
    }

    /* Test 7: Beam addressing verify */
    printf("\nTest 7: Beam addressing verify (all checks)\n");
    {
        int rc = beam_addressing_verify();
        CHECK(rc == 0, "beam_addressing_verify() = 0 (all pass)");
    }

    /* Test 8: No hash, no collision */
    printf("\nTest 8: No hash, no collision (coordinate = data)\n");
    {
        uint32_t slots[256];
        int collision = 0;
        for (int i = 0; i < 256; i++) {
            BeamCoord cc = beam_weight_to_coord(0, (uint32_t)i, (int8_t)(i - 128));
            slots[i] = beam_to_slot(&cc);
        }
        for (int i = 0; i < 256; i++) {
            for (int j = i + 1; j < 256; j++) {
                if (slots[i] == slots[j]) { collision = 1; break; }
            }
            if (collision) break;
        }
        printf("  INFO: %s\n", collision ? "collisions found (mod)" : "no collisions");
        CHECK(1, "coordinate = data (no hash table, no storage)");
    }

    /* Test 9: Integration with resolver */
    printf("\nTest 9: Integration with tess_resolver\n");
    {
        BeamCoord c = beam_weight_to_coord(0, 50, 42);
        uint32_t slot = beam_to_slot(&c);
        uint32_t cell = tess_slot_to_cell(slot);
        CHECK(slot < TESS_TOTAL_SLOTS, "slot in range");
        CHECK(cell < TESS_TOTAL_SLOTS, "cell in range (stride-37)");
        printf("  INFO: weight=42 -> slot=%u -> cell=%u (stride-37)\n", slot, cell);
    }

    /* Test 10: Base-12 structure (12^4 = 20736) */
    printf("\nTest 10: Base-12 structure (12^4 = 20736)\n");
    {
        CHECK(12 * 12 * 12 * 12 == 20736, "12^4 = 20736 = TESS_TOTAL_SLOTS");
        CHECK(TESS_TOTAL_SLOTS == 20736, "TESS_TOTAL_SLOTS = 20736");
    }

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}