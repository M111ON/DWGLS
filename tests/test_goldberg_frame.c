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
#include "../core/geo_goldberg_nbr.h"
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

    /* ── G5: table degrees from the table itself ──────────────────── */
    {
        int ok = 1;
        for (uint32_t f = 0; f < 162u; f++) {
            uint32_t want = (f < 12u) ? 5u : 6u;
            if (gp_nbr_count(f) != want) { ok = 0; break; }
            if (f < 12u && gp_nbr_get(f, 5u) != 255u) { ok = 0; break; }
        }
        CHECK("G5: pent rows degree 5 (+255 pad), hex rows degree 6", ok);
    }

    /* ── G6: symmetry re-verified from table side (not generator) ─── */
    {
        int ok = 1;
        for (uint32_t f = 0; f < 162u && ok; f++)
            for (uint32_t k = 0; k < gp_nbr_count(f); k++) {
                uint32_t g = gp_nbr_get(f, k);
                int back = 0;
                for (uint32_t j = 0; j < gp_nbr_count(g); j++)
                    if (gp_nbr_get(g, j) == f) { back = 1; break; }
                if (!back || g == f) { ok = 0; break; }
            }
        CHECK("G6: adjacency symmetric, no self-loops (all 162)", ok);
    }

    /* ── G7: pentagon isolation — pent neighbors all hexagons ─────── */
    {
        int ok = 1;
        uint32_t inc = 0;
        for (uint32_t f = 0; f < 162u; f++) {
            uint32_t n = gp_nbr_count(f);
            inc += n;
            if (f < 12u)
                for (uint32_t k = 0; k < n; k++)
                    if (gp_nbr_get(f, k) < 12u) { ok = 0; break; }
        }
        CHECK("G7: no pent-pent edge + incidence 960 = 2*480",
              ok && inc == 960u && inc / 2u == GP16_EDGES);
    }

    /* ── G8: numbering shared with frame header (pentagon set match) ─ */
    {
        int ok = 1;
        for (uint32_t f = 0; f < 162u; f++)
            if (gp16_is_pentagon(f) != (f < 12u) ||
                (gp_nbr_count(f) == 5u) != (f < 12u)) { ok = 0; break; }
        CHECK("G8: frame predicate == table degrees (shared numbering)", ok);
    }

    /* ── G9: coverage walk over table adjacency visits all 162 ────── */
    {
        uint8_t seen[162] = { 0 };
        uint32_t stack[162];
        uint32_t top = 0, count = 0;
        stack[top++] = 0;
        seen[0] = 1;
        while (top > 0) {
            uint32_t f = stack[--top];
            count++;
            for (uint32_t k = 0; k < gp_nbr_count(f); k++) {
                uint32_t g = gp_nbr_get(f, k);
                if (!seen[g]) { seen[g] = 1; stack[top++] = g; }
            }
        }
        CHECK("G9: BFS coverage walk reaches all 162 faces (connected)", count == 162u);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
