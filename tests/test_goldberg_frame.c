/*
 * test_goldberg_frame.c — Goldberg(4,0) frame over the 20736 field
 *
 * Oracle: integer identities + exhaustive partition + independent
 * cross-check vs geo_dram_tile.h (separate header, never includes impl).
 * Neighbor topology is named debt (not tested — not claimed).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_goldberg_frame tests/test_goldberg_frame.c
 */
#include <stdio.h>
#include <stdint.h>
#include "../core/geo_goldberg_frame.h"
#include "../core/geo_dram_tile.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(void) {
    printf("═ GOLDBERG(4,0) FRAME — 162 faces x 128 slots ═\n");

    /* ── G1: verified identities (any wrong constant breaks these) ── */
    CHECK("G1: 12+150==162 faces", GP16_PENT + GP16_HEX == GP16_FACES);
    CHECK("G1b: 162*128==20736 total",
          (uint32_t)GP16_FACES * GP16_SLOTS == GP16_TOTAL && GP16_TOTAL == 20736u);
    CHECK("G1c: Euler 320-480+162==2", GP16_VERTS - GP16_EDGES + GP16_FACES == 2u);
    CHECK("G1d: edge incidences (12*5+150*6)/2==480",
          (GP16_PENT * 5u + GP16_HEX * 6u) / 2u == GP16_EDGES);

    /* ── G2: partition roundtrip, exhaustive + uniform 128/face ───── */
    {
        static uint16_t per_face[162];
        int ok = 1;
        for (uint32_t flat = 0; flat < 20736u; flat++) {
            uint32_t f = gp16_face(flat), l = gp16_local(flat);
            if (f >= 162u || l >= 128u || gp16_flat(f, l) != flat) { ok = 0; break; }
            per_face[f]++;
        }
        int uniform = 1;
        for (uint32_t f = 0; f < 162u; f++)
            if (per_face[f] != 128u) { uniform = 0; break; }
        CHECK("G2: face<->flat roundtrip all 20736", ok);
        CHECK("G2b: uniform scale — every face exactly 128 slots (#814)", uniform);
    }

    /* ── G3: pentagon predicate hits exactly the canonical 12 ─────── */
    {
        int n = 0, ok = 1;
        for (uint32_t f = 0; f < 162u; f++) {
            int is5 = gp16_is_pentagon(f);
            if (is5) n++;
            if ((f < 12u) != (is5 != 0)) ok = 0;
        }
        CHECK("G3: exactly 12 pentagon faces (canonical 0..11)", ok && n == 12);
    }

    /* ── G4: cross-check vs DRAM layout (independent header) ──────── */
    CHECK("G4: 162x128 == DRAM_ANCHORS x DRAM_CELLS_PER == DRAM_FULL",
          DRAM_ANCHORS == GP16_FACES && DRAM_CELLS_PER == GP16_SLOTS &&
          DRAM_FULL == GP16_TOTAL);

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
