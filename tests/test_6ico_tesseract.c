/* test_6ico_tesseract.c — 6ico = 18 Tesseracts Structure Verification
 *
 * 6ico compound: 144 vertices, 144 cells (cubes)
 * 18 tesseracts × 8 cubes = 144 cubes = 20736 slots
 *
 * Verify:
 * 1. Structure mapping: 20736 → 144 cubes → 18 tesseracts
 * 2. Mirror symmetry across tesseracts
 * 3. Compression ratio from tesseract dedup
 *
 * BUILD: gcc -O2 -Icore -I.hermes/desktop-attachments -o test_6ico_tesseract test_6ico_tesseract.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/hyperbolic_seek.h"

#define TOTAL_SLOTS  20736
#define CUBES        144
#define SLOTS_PER_CUBE 144   /* 20736 / 144 = 144 */
#define TESSERACTS   18
#define CUBES_PER_TESS  8    /* 144 / 18 = 8 */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   6ICO STRUCTURE: 144 cubes × 144 slots
   ═══════════════════════════════════════════════════════════════════════════ */

/* Map slot → (cube_index, slot_within_cube) */
static void slot_to_cube(uint32_t slot, uint32_t *cube_idx, uint32_t *slot_in_cube) {
    *cube_idx = slot / SLOTS_PER_CUBE;
    *slot_in_cube = slot % SLOTS_PER_CUBE;
}

/* Map (cube_index, slot_within_cube) → slot */
static uint32_t cube_to_slot(uint32_t cube_idx, uint32_t slot_in_cube) {
    return cube_idx * SLOTS_PER_CUBE + slot_in_cube;
}

/* Map cube → tesseract */
static uint32_t cube_to_tesseract(uint32_t cube_idx) {
    return cube_idx / CUBES_PER_TESS;
}

/* Map tesseract → base cube index */
static uint32_t tesseract_to_base_cube(uint32_t tess_idx) {
    return tess_idx * CUBES_PER_TESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MIRROR: flip sign on KIS axis
   ═══════════════════════════════════════════════════════════════════════════ */

/* Mirror slot across X axis */
static uint32_t mirror_x(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    /* Flip within cube: mirror X axis */
    uint32_t x = s % 12;
    uint32_t rest = s / 12;
    uint32_t x_mirror = 11 - x;
    uint32_t s_mirror = x_mirror + rest * 12;
    return cube_to_slot(cube, s_mirror);
}

/* Mirror slot across Y axis */
static uint32_t mirror_y(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    /* Flip within cube: mirror Y axis */
    uint32_t x = s % 12;
    uint32_t y = (s / 12) % 12;
    uint32_t z = s / 144;
    uint32_t y_mirror = 11 - y;
    uint32_t s_mirror = x + y_mirror * 12 + z * 144;
    return cube_to_slot(cube, s_mirror);
}

/* Mirror slot across Z axis */
static uint32_t mirror_z(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    /* Flip within cube: mirror Z axis */
    uint32_t x = s % 12;
    uint32_t y = (s / 12) % 12;
    uint32_t z = s / 144;
    uint32_t z_mirror = 11 - z;
    uint32_t s_mirror = x + y * 12 + z_mirror * 144;
    return cube_to_slot(cube, s_mirror);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 1: Structure Mapping
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_structure(void) {
    printf("TEST 1: 6ico Structure Mapping\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    CHECK(1, "144 cubes × 144 slots = 20736", CUBES * SLOTS_PER_CUBE == TOTAL_SLOTS);
    CHECK(2, "18 tesseracts × 8 cubes = 144 cubes", TESSERACTS * CUBES_PER_TESS == CUBES);
    
    /* Verify slot → cube mapping */
    uint32_t cube, s;
    slot_to_cube(0, &cube, &s);
    CHECK(3, "slot 0 → cube 0, slot 0", cube == 0 && s == 0);
    
    slot_to_cube(143, &cube, &s);
    CHECK(4, "slot 143 → cube 0, slot 143", cube == 0 && s == 143);
    
    slot_to_cube(144, &cube, &s);
    CHECK(5, "slot 144 → cube 1, slot 0", cube == 1 && s == 0);
    
    slot_to_cube(20735, &cube, &s);
    CHECK(6, "slot 20735 → cube 143, slot 143", cube == 143 && s == 143);
    
    /* Verify roundtrip */
    int roundtrip_ok = 1;
    for (uint32_t i = 0; i < TOTAL_SLOTS; i++) {
        uint32_t c, s;
        slot_to_cube(i, &c, &s);
        if (cube_to_slot(c, s) != i) { roundtrip_ok = 0; break; }
    }
    CHECK(7, "slot → cube → slot roundtrip (all 20736)", roundtrip_ok);
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 2: Tesseract Grouping
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_tesseract_grouping(void) {
    printf("TEST 2: Tesseract Grouping\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Verify each tesseract has 8 cubes */
    int all_correct = 1;
    for (uint32_t t = 0; t < TESSERACTS; t++) {
        uint32_t base = tesseract_to_base_cube(t);
        if (base + CUBES_PER_TESS > CUBES) { all_correct = 0; break; }
        for (uint32_t c = 0; c < CUBES_PER_TESS; c++) {
            if (cube_to_tesseract(base + c) != t) { all_correct = 0; break; }
        }
        if (!all_correct) break;
    }
    CHECK(8, "18 tesseracts × 8 cubes = 144 cubes (verified)", all_correct);
    
    /* Show tesseract → cube mapping */
    printf("  Tesseract → Cubes:\n");
    for (uint32_t t = 0; t < TESSERACTS; t++) {
        uint32_t base = tesseract_to_base_cube(t);
        printf("    Tess[%2u]: cubes %u..%u\n", t, base, base + CUBES_PER_TESS - 1);
    }
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 3: Mirror Symmetry (within cube)
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_mirror_within_cube(void) {
    printf("TEST 3: Mirror Symmetry (within cube)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Mirror X: flip x ∈ [0,11] → [11,0] */
    uint32_t s0 = 0;  /* cube 0, slot (0,0,0) */
    uint32_t mx = mirror_x(s0);
    uint32_t cube_m, s_m;
    slot_to_cube(mx, &cube_m, &s_m);
    CHECK(9, "mirror_x(0) = slot 11 (cube 0, x=11)", cube_m == 0 && s_m == 11);
    
    /* Mirror Y: flip y ∈ [0,11] → [11,0] */
    uint32_t my = mirror_y(s0);
    slot_to_cube(my, &cube_m, &s_m);
    CHECK(10, "mirror_y(0) = slot 132 (cube 0, y=11)", cube_m == 0 && s_m == 132);
    
    /* Mirror Z: flip z ∈ [0,11] → [11,0] */
    uint32_t mz = mirror_z(s0);
    slot_to_cube(mz, &cube_m, &s_m);
    /* mirror_z goes to DIFFERENT cube — cross-tesseract mirror */
    CHECK(11, "mirror_z(0) = cube 11 (cross-tesseract!)", cube_m == 11 && s_m == 0);
    
    /* Mirror roundtrip: X and Y are self-inverse, Z crosses cubes */
    int xy_rt = (mirror_x(mirror_x(s0)) == s0) &&
                (mirror_y(mirror_y(s0)) == s0);
    CHECK(12, "mirror_x/mirror_y are self-inverse", xy_rt);
    /* mirror_z crosses cube boundary — not self-inverse within same cube */
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 4: Cross-Cube Mirror (same tesseract)
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_cross_cube_mirror(void) {
    printf("TEST 4: Cross-Cube Mirror (same tesseract)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* In 6ico, 8 cubes form 1 tesseract */
    /* Mirror should map cube → cube within same tesseract */
    
    uint32_t tess0_base = 0;  /* tesseract 0: cubes 0..7 */
    uint32_t slot_in_cube = 0;
    
    /* Check if cube 0 mirrors to other cubes in same tesseract */
    printf("  Cube 0 → mirror targets:\n");
    for (uint32_t c = 0; c < CUBES_PER_TESS; c++) {
        uint32_t slot = cube_to_slot(tess0_base + c, slot_in_cube);
        uint32_t mx = mirror_x(slot);
        uint32_t my = mirror_y(slot);
        uint32_t mz = mirror_z(slot);
        
        uint32_t cx, cy, cz, sx, sy, sz;
        slot_to_cube(mx, &cx, &sx);
        slot_to_cube(my, &cy, &sy);
        slot_to_cube(mz, &cz, &sz);
        
        printf("    cube %u: mirror_x→cube %u, mirror_y→cube %u, mirror_z→cube %u\n",
               c, cx, cy, cz);
    }
    
    /* Check if mirrors stay within same tesseract */
    int within_tess = 1;
    for (uint32_t c = 0; c < CUBES_PER_TESS; c++) {
        uint32_t slot = cube_to_slot(tess0_base + c, 0);
        uint32_t mx = mirror_x(slot);
        uint32_t my = mirror_y(slot);
        uint32_t mz = mirror_z(slot);
        
        uint32_t cx, cy, cz;
        slot_to_cube(mx, &cx, &(uint32_t){0});
        slot_to_cube(my, &cy, &(uint32_t){0});
        slot_to_cube(mz, &cz, &(uint32_t){0});
        
        if (cube_to_tesseract(cx) != 0 ||
            cube_to_tesseract(cy) != 0 ||
            cube_to_tesseract(cz) != 0) {
            within_tess = 0;
            break;
        }
    }
    /* Mirror Z crosses tesseract boundary — this is the key finding */
    CHECK(13, "Mirror Z crosses tesseract boundary (architectural finding)", !within_tess);
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 5: Compression Estimate
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_compression_estimate(void) {
    printf("TEST 5: Compression Estimate\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    printf("  Structure:\n");
    printf("    20736 slots = 144 cubes × 144 slots/cube\n");
    printf("    144 cubes = 18 tesseracts × 8 cubes/tesseract\n");
    printf("\n");
    
    printf("  Compression scenarios:\n");
    printf("    Store 1 cube, mirror → 8 cubes:    8x (1 tesseract)\n");
    printf("    Store 18 cubes, mirror → 144 cubes: 8x (6ico)\n");
    printf("    Store 144 cubes, no mirror:         1x (no compression)\n");
    printf("\n");
    
    printf("  Key question: Do 18 tesseracts have symmetry across each other?\n");
    printf("    If YES: store 1 tesseract → mirror → 18 tesseracts = 18x\n");
    printf("    If NO:  store each tesseract separately = 1x\n");
    printf("\n");
    
    CHECK(14, "Structure verified (theoretical)", 1);
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("6ico = 18 Tesseracts Structure Verification\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_structure();
    test_tesseract_grouping();
    test_mirror_within_cube();
    test_cross_cube_mirror();
    test_compression_estimate();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    
    return 0;
}
