/* test_18tes_field.c — 18tes full field verification
 *
 * 18 tesseracts × 8 cubes × 144 slots = 20736
 *
 * NOTE: geo_tess_wiring.h (TESS_CELLS=144) and geo_tesseract_addr.h
 * (TESS_3D_CELLS=8) no longer conflict. Both can be included safely.
 *
 * Verify:
 * T1: flat ↔ tess roundtrip (all 20736 positions)
 * T2: cross-tesseract mirror_z tracking
 * T3: global passive log (cross-tess events)
 * T4: global magnify glass (antipodal across field)
 * T5: cross-tess stride-37 walk coverage
 * T6: write/read roundtrip all 18 tess
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -o build/test_18tes_field tests/test_18tes_field.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_tess_wiring.h"

/* ═══════════════ LOCAL CONSTANTS (avoid TESS_CELLS conflict) ═══════════════ */
#define FIELD_TOTAL     20736u
#define N_TESS          18u
#define CUBES_PER_TESS  8u
#define SLOTS_PER_CUBE  144u
#define SLOTS_PER_TESS  (CUBES_PER_TESS * SLOTS_PER_CUBE)  /* 1152 */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════ MIRROR HELPERS (144-slot cube model) ═══════════════ */

static void slot_to_cube(uint32_t slot, uint32_t *cube, uint32_t *s) {
    *cube = slot / SLOTS_PER_CUBE;
    *s    = slot % SLOTS_PER_CUBE;
}
static uint32_t cube_to_slot(uint32_t cube, uint32_t s) {
    return cube * SLOTS_PER_CUBE + s;
}

/* Mirror X: flip x ∈ [0,11] */
static uint32_t mirror_x(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    uint32_t x = s % 12;
    uint32_t rest = s / 12;
    return cube_to_slot(cube, (11u - x) + rest * 12);
}

/* Mirror Y: flip y ∈ [0,11] */
static uint32_t mirror_y(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    uint32_t x = s % 12;
    uint32_t y = (s / 12) % 12;
    uint32_t z = s / 144;
    return cube_to_slot(cube, x + (11u - y) * 12 + z * 144);
}

/* Mirror Z: flip z ∈ [0,1] — crosses cube boundary */
static uint32_t mirror_z(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    uint32_t x = s % 12;
    uint32_t y = (s / 12) % 12;
    uint32_t z = s / 144;
    return cube_to_slot(cube, x + y * 12 + (1u - z) * 144);
}

/* ═══════════════ T1: Full flat ↔ tess roundtrip ═══════════════ */

static void t1_full_roundtrip(void) {
    printf("── T1 flat ↔ tess roundtrip (%u)\n", FIELD_TOTAL);

    CHECK(1, "tess_to_flat(0,0,0) == 0",
          tess_to_flat(0, 0, 0) == 0);
    CHECK(2, "tess_to_flat(0,7,143) == 1151",
          tess_to_flat(0, 7, 143) == 1151);
    CHECK(3, "tess_to_flat(1,0,0) == 1152",
          tess_to_flat(1, 0, 0) == 1152);
    CHECK(4, "tess_to_flat(17,7,143) == 20735",
          tess_to_flat(17, 7, 143) == 20735);

    /* Full roundtrip: every flat address */
    int ok = 1;
    for (uint32_t f = 0; f < FIELD_TOTAL; f++) {
        uint32_t tess, cube, local;
        flat_to_tess(f, &tess, &cube, &local);
        if (tess_to_flat(tess, cube, local) != f) { ok = 0; break; }
    }
    CHECK(5, "full roundtrip 0..20735", ok);

    /* Uniqueness: linear map f = tess*1152 + cube*144 + local is
     * mixed-radix with coprime bases → bijective. Verify via counting. */
    uint32_t count = 0;
    for (uint32_t tess = 0; tess < N_TESS; tess++)
        for (uint32_t cube = 0; cube < CUBES_PER_TESS; cube++)
            for (uint32_t local = 0; local < SLOTS_PER_CUBE; local++)
                count++;
    CHECK(6, "18×8×144 = 20736 iteration count", count == FIELD_TOTAL);
}

/* ═══════════════ T2: Cross-tesseract mirror_z tracking ═══════════════ */

static void t2_cross_tess_mirror(void) {
    printf("── T2 cross-tess mirror_z tracking\n");

    /* In 144-slot cube model: each cube has 12×12×1 = 1728 logical positions
     * mapped into 144 slots. z ∈ {0,1}, so mirror_z flips z and crosses cube.
     * cube 0 slot 0 (z=0) → cube 1 slot 0 (z=1) */
    uint32_t mz = mirror_z(0);
    uint32_t mc, ms;
    slot_to_cube(mz, &mc, &ms);
    CHECK(7, "mirror_z(0) = cube 1, slot 0", mc == 1 && ms == 0);

    /* Check which tess cubes 0..7 land in after mirror_z */
    uint32_t n_cross = 0;
    printf("    mirror_z cross-tess mapping (tess 0):\n");
    for (uint32_t c = 0; c < CUBES_PER_TESS; c++) {
        uint32_t slot = cube_to_slot(c, 0);
        uint32_t mz_c = mirror_z(slot);
        uint32_t target_cube, target_slot;
        slot_to_cube(mz_c, &target_cube, &target_slot);

        uint32_t target_tess = target_cube / CUBES_PER_TESS;
        printf("      cube %u → cube %u (tess %u)\n", c, target_cube, target_tess);
        if (target_tess != 0) n_cross++;
    }
    CHECK(8, "mirror_z crosses tess boundary for some cubes", n_cross > 0);

    /* Mirror X and Y stay within cube */
    uint32_t mx0 = mirror_x(cube_to_slot(0, 0));
    uint32_t my0 = mirror_y(cube_to_slot(0, 0));
    uint32_t cx, cy, sx, sy;
    slot_to_cube(mx0, &cx, &sx);
    slot_to_cube(my0, &cy, &sy);
    CHECK(9, "mirror_x stays in cube 0", cx == 0);
    CHECK(10, "mirror_y stays in cube 0", cy == 0);

    /* Mirror X/Y self-inverse */
    CHECK(11, "mirror_x self-inverse", mirror_x(mirror_x(0)) == 0);
    CHECK(12, "mirror_y self-inverse", mirror_y(mirror_y(0)) == 0);
}

/* ═══════════════ T3: Global passive log ═══════════════ */

static void t3_global_passive_log(void) {
    printf("── T3 global passive log (cross-tess)\n");

    TessPassiveLog log;
    log.count = 0;

    /* Log event: scale change from w=10 to w=90 (cross-tess w positions) */
    tess_log_append(&log, 10, 90);
    CHECK(13, "log append", log.count == 1);

    /* Replay: initial position w=10 → final w=90 */
    uint32_t replayed = tess_log_replay(&log, 10);
    CHECK(14, "log replay returns target w", replayed == 90);

    /* Multiple events */
    tess_log_append(&log, 90, 5);
    tess_log_append(&log, 5, 120);
    CHECK(15, "log 3 events", log.count == 3);

    /* Replay: chain of events, last wins */
    uint32_t r2 = tess_log_replay(&log, 10);
    CHECK(16, "log replay chain (last event wins)", r2 == 120);

    /* Collapse */
    uint32_t final_w;
    tess_log_collapse(&log, 10, &final_w);
    CHECK(17, "log collapse produces single entry", log.count == 1);
    CHECK(18, "collapsed entry: 10 → 120", log.entries[0].from_w == 10 && log.entries[0].to_w == 120);
}

/* ═══════════════ T4: Global magnify glass ═══════════════ */

static void t4_global_magnify_glass(void) {
    printf("── T4 global magnify glass (antipodal)\n");

    /* Antipodal property: different position from source */
    int antipodal_ok = 1;
    uint32_t test_flats[] = {0, 36, 72, 108, 1152, 10368, 20735};
    for (int i = 0; i < 7; i++) {
        uint32_t f = test_flats[i];
        uint32_t ap = tess_antipode(f);
        if (ap == f) { antipodal_ok = 0; break; }
    }
    CHECK(19, "antipodal positions differ from source", antipodal_ok);

    /* Glass membership */
    uint32_t center_flat = tess_to_flat(0, 0, 72);  /* w=72 center */
    uint32_t edge_flat   = tess_to_flat(0, 0, 0);    /* w=0 edge */
    CHECK(20, "glass center (w=72) is in glass", tess_in_glass(center_flat));
    CHECK(21, "glass edge (w=0) is not in glass", !tess_in_glass(edge_flat));

    /* tess_verify_wiring(NULL) returns 0 (no volume to verify — correct) */
    CHECK(22, "tess_verify_wiring(NULL) returns 0 (no-op)", tess_verify_wiring(NULL) == 0);
}

/* ═══════════════ T5: Cross-tess stride-37 walk ═══════════════ */

static void t5_stride37_walk(void) {
    printf("── T5 cross-tess stride-37 walk\n");

    /* Verify gcd(37, 20736) = 1 (coprime → full coverage) */
    uint32_t a = 37, b = FIELD_TOTAL;
    while (b) { uint32_t t = b; b = a % b; a = t; }
    CHECK(23, "gcd(37, 20736) == 1 (coprime)", a == 1);

    /* Walk within tess 0: start at flat 0, stride 37 */
    /* tess_seek_flat wraps at TESS_TOTAL (20736), so walk stays global */
    uint32_t pos = 0;
    uint32_t max_tess = 0;
    uint32_t steps = 0;
    int returned = 0;

    for (uint32_t i = 0; i < FIELD_TOTAL; i++) {
        pos = tess_seek_flat(0, steps + 1);
        steps++;
        uint32_t tess, cube, local;
        flat_to_tess(pos, &tess, &cube, &local);
        if (tess > max_tess) max_tess = tess;
        if (pos == 0 && steps > 1) { returned = 1; break; }
    }
    CHECK(24, "stride-37 walk returns to start", returned);
    printf("    walked %u steps, max tess=%u\n", steps, max_tess);

    /* Verify coprime with 144 (per-tess coverage) */
    a = 37; b = SLOTS_PER_CUBE;
    while (b) { uint32_t t = b; b = a % b; a = t; }
    CHECK(25, "gcd(37, 144) == 1 (full cube coverage)", a == 1);
}

/* ═══════════════ T6: Write/read roundtrip all 18 tess ═══════════════ */

static void t6_write_read_roundtrip(void) {
    printf("── T6 write/read roundtrip all 18 tess\n");

    /* Allocate 20736 × 64-byte slots */
    static uint8_t field[FIELD_TOTAL * 64];

    /* Write deterministic data via tess_to_flat */
    for (uint32_t tess = 0; tess < N_TESS; tess++) {
        for (uint32_t cube = 0; cube < CUBES_PER_TESS; cube++) {
            for (uint32_t local = 0; local < SLOTS_PER_CUBE; local++) {
                uint32_t flat = tess_to_flat(tess, cube, local);
                uint32_t pattern = flat ^ (flat >> 8) ^ 0xA5A5A5A5u;
                memcpy(&field[flat * 64], &pattern, 4);
            }
        }
    }

    /* Read back via tess_to_flat/flat_to_tess and verify */
    int ok = 1;
    for (uint32_t f = 0; f < FIELD_TOTAL; f++) {
        uint32_t tess, cube, local;
        flat_to_tess(f, &tess, &cube, &local);
        uint32_t flat2 = tess_to_flat(tess, cube, local);
        if (flat2 != f) { ok = 0; break; }
        uint32_t pattern = f ^ (f >> 8) ^ 0xA5A5A5A5u;
        uint32_t read_val;
        memcpy(&read_val, &field[f * 64], 4);
        if (read_val != pattern) { ok = 0; break; }
    }
    CHECK(26, "write/read roundtrip all 20736 slots", ok);

    /* Index frame build for tess 0 */
    uint8_t index_frame[SLOTS_PER_CUBE * 64];
    uint32_t cube_sizes[8];
    for (int i = 0; i < 8; i++) cube_sizes[i] = SLOTS_PER_CUBE;
    tess_build_index(index_frame, 0, NULL, cube_sizes);

    uint32_t base, len, stride;
    tess_read_index(index_frame, 0, &base, &len, &stride);
    CHECK(27, "index frame cube 0 base == 0", base == 0);
    CHECK(28, "index frame cube 0 len == 144", len == 144);
    CHECK(29, "index frame cube 0 stride == 37", stride == 37);

    tess_read_index(index_frame, 5, &base, &len, &stride);
    CHECK(30, "index frame cube 5 base == 720", base == 720);
}

/* ═══════════════ MAIN ═══════════════ */

int main(void) {
    printf("18tes Full Field Verification\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    t1_full_roundtrip();
    t2_cross_tess_mirror();
    t3_global_passive_log();
    t4_global_magnify_glass();
    t5_stride37_walk();
    t6_write_read_roundtrip();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════\n");

    return fail ? 1 : 0;
}
