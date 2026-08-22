/*
 * test_iso_fold.c — 1 tesseract = 9 anchors fold proof
 * ══════════════════════════════════════════════════════════════════
 * Independent oracles:
 *   A. Hilbert inverse vs FORWARD function (exhaustive over 64 points —
 *      oracle is dram_hilbert_8x8 itself, not my inverse).
 *   B. Roundtrip: unfold(fold(tes,cell,slot)) == flat g for ALL 20736,
 *      with full bitset coverage (no hole, no collision).
 *   C. Alignment: tesseract k folds into anchors exactly [9k, 9k+9) —
 *      derived by hand from 1152/128 = 9; checked at both boundaries.
 *   D. Hand case from the rot90 session: slot 89 → anchor 0, layer 1,
 *      hilbert pos 25; the (x,y) for hpos=25 found by EXHAUSTIVE forward
 *      search (oracle independent of iso_hilbert_inv), then unfolded.
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_iso_fold tests/test_iso_fold.c
 */
#include <stdio.h>
#include <string.h>
#include "../core/iso_fold.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

int main(void) {
    printf("=== iso_fold — 1 tesseract (1152) = 9 anchors (9x128) ===\n");

    /* A. inverse Hilbert against the forward map */
    {
        int ok = 1;
        for (uint32_t y = 0; y < DRAM_GRID_Y && ok; y++) {
            for (uint32_t x = 0; x < DRAM_GRID_X && ok; x++) {
                uint32_t d = dram_hilbert_8x8(x, y);
                uint32_t bx, by;
                iso_hilbert_inv(d, &bx, &by);
                if (bx != x || by != y) ok = 0;
            }
        }
        CHECK(ok, "A: iso_hilbert_inv inverts dram_hilbert_8x8 on all 64 points");
    }

    /* B. full-space roundtrip + coverage */
    {
        static uint8_t seen[DRAM_FULL / 8 + 1];
        memset(seen, 0, sizeof(seen));
        int ok = 1;
        for (uint32_t tes = 0; tes < ISO_N_TES && ok; tes++) {
            for (uint32_t c = 0; c < ISO_TES_CELLS && ok; c++) {
                for (uint32_t s = 0; s < ISO_TES_SLOTS && ok; s++) {
                    uint32_t g = tes * ISO_TES_SIZE + c * ISO_TES_SLOTS + s;
                    IsoFold f = iso_fold(tes, c, s);
                    uint32_t u = iso_unfold(&f);
                    if (u != g) ok = 0;
                    if (seen[g >> 3] & (1u << (g & 7))) ok = 0;
                    seen[g >> 3] |= (uint8_t)(1u << (g & 7));
                }
            }
        }
        CHECK(ok, "B: fold/unfold roundtrip lossless on all 20736, no collision");
    }

    /* C. tesseract k lives in anchors [9k, 9k+9) */
    {
        int ok = 1;
        for (uint32_t tes = 0; tes < ISO_N_TES && ok; tes++) {
            IsoFold first = iso_fold(tes, 0, 0);
            IsoFold last  = iso_fold(tes, ISO_TES_CELLS - 1, ISO_TES_SLOTS - 1);
            if (first.anchor != tes * ISO_ANCHORS_PER_TES) ok = 0;
            if (last.anchor != (tes + 1) * ISO_ANCHORS_PER_TES - 1) ok = 0;
        }
        CHECK(ok, "C: tes k spans anchors exactly [9k, 9k+8]");
    }

    /* D. hand case continuing the rot90 session: cell0 slot89 */
    {
        IsoFold f = iso_fold(0, 0, 89);
        CHECK(f.anchor == 0 && f.layer == 1, "D: slot 89 -> anchor 0, layer 1");

        /* find (x,y) with forward-hilbert == 25 — exhaustive search oracle */
        uint32_t hx = 99, hy = 99;
        for (uint32_t y = 0; y < DRAM_GRID_Y; y++)
            for (uint32_t x = 0; x < DRAM_GRID_X; x++)
                if (dram_hilbert_8x8(x, y) == 25) { hx = x; hy = y; }
        CHECK(hx != 99 && f.x == hx && f.y == hy,
              "D: fold's (x,y) matches exhaustive forward search for hpos");

        uint32_t back = dram_addr(f.anchor, f.x, f.y, f.layer);
        CHECK(back == 89, "D: dram_addr round-trips slot 89");
    }

    printf("%s (%d failure%s)\n",
           failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
