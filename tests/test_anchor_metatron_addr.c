/* test_anchor_metatron_addr.c — A1/A4/A6 live-consumer gate.
 *
 * The A5 test (test_anchor_metatron.c) counts the geometry (13 cells, 96
 * pieces). THIS test proves the same facts DRIVE live addresses via
 * core/anchor_metatron.h:
 *
 *   A1  every anchor has a Metatron address in [0, 20736) that DEPENDS on the
 *       seed (distinct seeds -> spread of addresses, not one constant);
 *   A4  the 96x216 grid tiles the field EXACTLY: (piece,slot)<->flat is a
 *       bijection over [0,20736), the 48+24+24 split holds, and every
 *       anchor's grid placement round-trips;
 *   A6  the hex address round-trips (type + centre value) for every anchor,
 *       and varies with the seed.
 *
 * Expectations come from geometry (spec), never from the functions under
 * test. Deterministic xorshift seeds (periodic ramps alias the 48-step walk,
 * see test_anchor_metatron.c).
 *
 * Build (MSYS2 gcc): gcc -O2 -std=c11 -I. -Iinclude -Icore \
 *     tests/test_anchor_metatron_addr.c -o build/test_anchor_metatron_addr.exe -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "anchor_metatron.h"   /* A1/A4/A6: am_* */
#include "anchor_weight.h"

#define N_INSTANCES 5000

static void seed_for(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b[i] = (uint8_t)s; }
}

int main(void) {
    long n = 0;
    long a1_range = 0, a1_distinct_vs_first = 0, a4_place_ok = 0, a6_rt = 0;
    uint32_t first_addr = 0; int first_set = 0;
    /* A6 spread: count distinct hex addresses (a constant address would be
     * a collapse like the #7346/#7348 class). */
    static uint8_t hex_seen[65536];
    long hex_distinct = 0;
    memset(hex_seen, 0, sizeof(hex_seen));
    /* A4 grid coverage: mark every flat address reached by a piece walk. */
    static uint8_t seen[AM_FIELD];
    memset(seen, 0, sizeof(seen));
    long covered = 0;

    for (uint32_t i = 0; i < N_INSTANCES; i++) {
        uint8_t seed[32];
        seed_for(seed, sizeof seed, 0xADD00000u + i * 2654435761u);
        aw_anchor_t a;
        aw_anchor_build(seed, sizeof seed, (uint8_t)(i & 1u), &a);
        n++;

        am_addr_t r = am_anchor_address(&a);

        /* A1: address in range and seed-dependent */
        if (r.metatron_addr < AM_FIELD) a1_range++;
        if (!first_set) { first_addr = r.metatron_addr; first_set = 1; }
        else if (r.metatron_addr != first_addr) a1_distinct_vs_first++;

        /* A4: the anchor's grid placement round-trips to its flat address */
        int piece = 0; uint32_t slot = 0;
        am_grid_place(&a, &piece, &slot);
        uint32_t flat = am_grid_flat(piece, slot);
        if (flat != 0xFFFFFFFFu && flat == r.metatron_addr &&
            am_grid_piece_of(flat) == piece && am_grid_slot_of(flat) == slot)
            a4_place_ok++;

        /* A6: hex address round-trips (type + centre) and spreads over seeds */
        if (am_hex_roundtrip(&a, NULL, NULL)) a6_rt++;
        if (!hex_seen[r.hex_addr]) { hex_seen[r.hex_addr] = 1; hex_distinct++; }
    }

    /* A4 bijection: walk every (piece,slot), assert flat covers [0,20736) once */
    for (int p = 0; p < (int)AM_GRID_PIECES; p++) {
        for (uint32_t s = 0; s < AM_GRID_SLOTS; s++) {
            uint32_t f = am_grid_flat(p, s);
            if (f < AM_FIELD && !seen[f]) { seen[f] = 1; covered++; }
        }
    }
    long full = 0;
    for (uint32_t f = 0; f < AM_FIELD; f++) full += seen[f];
    int a4_bijection = (covered == (long)AM_FIELD) && (full == (long)AM_FIELD);

    /* A4 split: 48 + 24 + 24 = 96 and 96*216 == 20736 */
    int a4_split = (AM_GRID_CUBE_EDGE + AM_GRID_TETRA_FACE + AM_GRID_CUBE_FACE
                    == AM_GRID_PIECES) &&
                   (AM_GRID_PIECES * AM_GRID_SLOTS == AM_FIELD);
    /* group boundaries land where the split says */
    int a4_groups = (am_grid_group(0) == 0) &&
                    (am_grid_group(AM_GRID_CUBE_EDGE - 1) == 0) &&
                    (am_grid_group(AM_GRID_CUBE_EDGE) == 1) &&
                    (am_grid_group(AM_GRID_CUBE_EDGE + AM_GRID_TETRA_FACE) == 2) &&
                    (am_grid_group(AM_GRID_PIECES - 1) == 2) &&
                    (am_grid_group(AM_GRID_PIECES) == -1);

    int c_a1 = (a1_range == n) && (a1_distinct_vs_first > (n * 9 / 10));
    int c_a4 = a4_bijection && a4_split && a4_groups && (a4_place_ok == n);
    /* A6 gate: the hex address is a 14-bit packed field (type 2b + diff 4b +
     * centre 8b, geo_hex_layer.h), so its ceiling is ~4*256 and the 52-letter
     * alphabet saturates the centre byte. A real address must (a) round-trip
     * its stable fields for every anchor and (b) exercise the centre byte
     * (>=256 distinct) — a constant/collapsed address would fail both. The
     * raw spread is printed so the coarse ceiling is visible, not hidden. */
    int c_a6 = (a6_rt == n) && (hex_distinct >= 256);

    printf("=== test_anchor_metatron_addr (N=%d) — A1/A4/A6 live consumer ===\n", N_INSTANCES);
    printf("A1 metatron addr in [0,20736)  : %ld/%ld range, %ld distinct vs first : %s\n",
           a1_range, n, a1_distinct_vs_first, c_a1 ? "PASS" : "FAIL");
    printf("A4 grid 96x216 bijection       : covered %ld/%u, split+groups ok, place %ld/%ld : %s\n",
           covered, AM_FIELD, a4_place_ok, n,
           c_a4 ? "PASS" : "FAIL");
    printf("A6 hex address roundtrip+spread: %ld/%ld rt, %ld distinct addrs : %s\n",
           a6_rt, n, hex_distinct, c_a6 ? "PASS" : "FAIL");

    int all = c_a1 && c_a4 && c_a6;
    printf("RESULT: %s\n", all ? "ALL PASS" : "FAIL");
    return all ? 0 : 1;
}
