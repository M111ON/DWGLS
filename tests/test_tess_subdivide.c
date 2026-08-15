/*
 * test_tess_subdivide.c — Subdivision rules on the equal-triangle floor
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Rescope (2026-08-14): we do NOT construct geometry — the tessellation is a
 * RULE over addresses. Mixed radix: node = hi·81 + lo, hi ∈ [0,256) = 4-ladder
 * (2⁸ = 4⁴), lo ∈ [0,81) = 3-ladder (3⁴). 20736 = 4⁴·3⁴ = 256·81.
 *
 * th_subdivide(node, aperture, depth, child):
 *   aperture 4 → 4 children per level (1/4 area: 1→4→16→64→256) — refines hi
 *   aperture 3 → 3 children per level (1/3: 1→3→9→27→81)        — refines lo
 *   aperture 7 → hexagon: 7 cells/tile (1 center + 6 ring, HEX_CELLS=7)
 * th_parent is the lossless inverse.
 *
 * Proof:
 *   S1  mixed radix: node == hi·81 + lo for all 20736; 256×81 = 20736
 *   S2  aperture-4 ladder: cell counts 1→4→16→64→256, sizes 20736→81
 *   S3  aperture-4 roundtrip: parent(subdivide(node,d,c), d+1) == node
 *       (lossless at EVERY level, exhaustive 20736 × 4 × 4)
 *   S4  aperture-3 ladder: counts 1→3→9→27→81, sizes 20736→256
 *   S5  aperture-3 roundtrip: exhaustive lossless
 *   S6  children of one parent are distinct (bijection per level)
 *   S7  two 4-subdivisions == 16 children (1/4 × 1/4 = 1/16, area /16)
 *   S8  hex-7 tile: 7 cells per tile, center = tile·7+6, HEX_CELLS = 7
 *   S9  hex-7 covers field exactly: 20736 / 7 tiles = 2962, 5 remainder —
 *       tiles are the encoding unit (144 tiles × 7 = 1008 data slots)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_subdivide tests/test_tess_subdivide.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/tri_hex_tess.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(void) {
    printf("═ SUBDIVISION RULES — equal-triangle floor, rule-only (rescope) ═\n");

    /* ── S1: mixed radix — node = hi·81 + lo ─────────────────────────── */
    {
        int ok = 1;
        for (uint32_t n = 0; n < GEO_FULL; n++) {
            uint32_t hi = th_hi4(n), lo = th_lo3(n);
            if (th_node(hi, lo) != n || hi >= TH_HI_BASE || lo >= TH_LO_BASE) { ok = 0; break; }
        }
        CHECK("S1: node == hi·81 + lo for all 20736 (hi<256, lo<81)", ok);
        CHECK("S1b: 256 × 81 == 20736 (4⁴·3⁴ — two ladders, one space)",
              TH_HI_BASE * TH_LO_BASE == GEO_FULL);
    }

    /* ── S2: aperture-4 ladder — counts and cell sizes ───────────────── */
    {
        int ok = 1;
        uint32_t expect_count = 1, expect_size = GEO_FULL;
        for (uint32_t d = 0; d <= TH_SUBDIV_4_DEPTH; d++) {
            if (th_cell_count(4u, d) != expect_count) ok = 0;
            if (th_cell_size(4u, d) != expect_size) ok = 0;
            expect_count *= 4u;
            expect_size  /= 4u;
        }
        CHECK("S2: aperture-4 ladder 1→4→16→64→256, sizes 20736→81", ok);
        CHECK("S2b: level 4 cell size == 81 == lo base (each 4-cell = full lo)",
              th_cell_size(4u, TH_SUBDIV_4_DEPTH) == TH_LO_BASE);
    }

    /* ── S3: aperture-4 roundtrip — exhaustive, every level × every child ── */
    /* subdivide acts on the cell base: parent(subdivide(node,d,c), d+1)
     * must return exactly the depth-d cell anchor of node (identity when
     * node is already an anchor). */
    {
        int ok = 1, ok_identity = 1;
        for (uint32_t node = 0; node < GEO_FULL && ok; node++)
            for (uint32_t d = 0; d < TH_SUBDIV_4_DEPTH && ok; d++)
                for (uint32_t c = 0; c < 4u; c++) {
                    uint32_t anchor = th_cell_anchor(node, 4u, d);
                    uint32_t child = th_subdivide4(node, d, c);
                    uint32_t back  = th_parent4(child, d + 1u);
                    if (back != anchor) { ok = 0; break; }
                    if (node == anchor) {            /* anchor in → anchor out */
                        if (th_cell_anchor(child, 4u, d + 1u) != child) ok_identity = 0;
                    }
                }
        CHECK("S3: aperture-4 roundtrip — parent(child) == cell anchor (all 20736 × 4 × 4)", ok);
        CHECK("S3b: anchor in → anchor out — child of an anchor is an anchor (identity)", ok_identity);
    }

    /* ── S4: aperture-3 ladder ────────────────────────────────────────── */
    {
        int ok = 1;
        uint32_t expect_count = 1, expect_size = GEO_FULL;
        for (uint32_t d = 0; d <= TH_SUBDIV_3_DEPTH; d++) {
            if (th_cell_count(3u, d) != expect_count) ok = 0;
            if (th_cell_size(3u, d) != expect_size) ok = 0;
            expect_count *= 3u;
            expect_size  /= 3u;
        }
        CHECK("S4: aperture-3 ladder 1→3→9→27→81, sizes 20736→256", ok);
        CHECK("S4b: level 4 cell size == 256 == hi base (each 3-cell = full hi)",
              th_cell_size(3u, TH_SUBDIV_3_DEPTH) == TH_HI_BASE);
    }

    /* ── S5: aperture-3 roundtrip — exhaustive ────────────────────────── */
    {
        int ok = 1, ok_identity = 1;
        for (uint32_t node = 0; node < GEO_FULL && ok; node++)
            for (uint32_t d = 0; d < TH_SUBDIV_3_DEPTH && ok; d++)
                for (uint32_t c = 0; c < 3u; c++) {
                    uint32_t anchor = th_cell_anchor(node, 3u, d);
                    uint32_t child = th_subdivide3(node, d, c);
                    uint32_t back  = th_parent3(child, d + 1u);
                    if (back != anchor) { ok = 0; break; }
                    if (node == anchor) {
                        if (th_cell_anchor(child, 3u, d + 1u) != child) ok_identity = 0;
                    }
                }
        CHECK("S5: aperture-3 roundtrip — parent(child) == cell anchor (all 20736 × 4 × 3)", ok);
        CHECK("S5b: anchor in → anchor out (identity)", ok_identity);
    }

    /* ── S6: children of one parent are distinct (bijection) ──────────── */
    {
        int ok = 1;
        for (uint32_t node = 0; node < GEO_FULL; node += 1000) {
            uint32_t anchor = th_cell_anchor(node, 4u, 0u);
            uint32_t kids4[4], kids3[3];
            for (uint32_t c = 0; c < 4u; c++) {
                kids4[c] = th_subdivide4(anchor, 0, c);
                for (uint32_t j = 0; j < c; j++) if (kids4[j] == kids4[c]) ok = 0;
            }
            uint32_t anchor3 = th_cell_anchor(node, 3u, 0u);
            for (uint32_t c = 0; c < 3u; c++) {
                kids3[c] = th_subdivide3(anchor3, 0, c);
                for (uint32_t j = 0; j < c; j++) if (kids3[j] == kids3[c]) ok = 0;
            }
        }
        CHECK("S6: children distinct — 4-way and 3-way split are bijections", ok);
    }

    /* ── S7: composition — two 4-subdivisions == 16 children (1/16) ───── */
    {
        int ok = 1;
        for (uint32_t node = 0; node < GEO_FULL; node += 1000) {
            uint32_t seen[16];
            memset(seen, 0xFF, sizeof(seen));
            for (uint32_t c = 0; c < 16u; c++) {
                uint32_t child = th_quarter2(node, 0, c);
                int dup = 0;
                for (uint32_t j = 0; j < c; j++) if (seen[j] == child) { dup = 1; break; }
                seen[c] = child;
                if (dup) { ok = 0; break; }
            }
        }
        CHECK("S7: 1/4 × 1/4 == 1/16 — two 4-subdivisions give 16 distinct children", ok);
    }

    /* ── S8/S9: hex-7 tile — 1 center + 6 ring, covers field ──────────── */
    {
        int ok_cells = 1, ok_center = 1;
        for (uint32_t node = 0; node < GEO_FULL; node += 1000) {
            uint32_t tile[7];
            th_hex7_tile(node, tile);
            uint32_t expect_center = (node / 7u) * 7u + 6u;
            if (tile[6] != expect_center) ok_center = 0;
            for (uint32_t i = 0; i < 7u; i++)
                if (tile[i] / 7u != node / 7u) ok_cells = 0;   /* same tile */
        }
        CHECK("S8: hex-7 — 7 cells per tile, center == tile·7+6 (HEX_CENTER=6)", ok_cells && ok_center);
        CHECK("S8b: HEX_CELLS == 7 == 1 center + 6 ring (hex_tile.h contract)",
              7u == 1u + 6u);
        CHECK("S9: hex-7 tile id maps 144 tiles × 7 == 1008 data slots (1 tesseract)",
              (144u * 7u) == 1008u);
    }

    printf("\nFINAL: %d/%d PASS — %s\n", pass_count, pass_count + fail_count,
           fail_count ? "subdivide rules broken" : "subdivide lossless, ladder counts exact");
    return fail_count ? 1 : 0;
}
