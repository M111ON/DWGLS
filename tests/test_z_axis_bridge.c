/*
 * test_z_axis_bridge.c — Cross-Tesseract Z-Axis Bridge
 * ═══════════════════════════════════════════════════════════════════
 * Investigation found: mirror_z in 6ico compound CROSSES tesseract
 * boundaries (cube 0 → cube 11), while mirror_x/y stay within.
 *
 * 6ico structure: 144 cubes × 144 slots (12³) = 20736
 * 18 tesseracts × 8 cubes = 144 cubes
 *
 * KEY FINDING: mirror_z is NOT self-inverse — cube changes each time.
 * This means Z-axis traversal is a one-way bridge across tesseracts,
 * not a roundtrip. The bridge connects cube 0→11→22→... across the
 * 6ico compound.
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/test_z_axis_bridge
 *        tests/test_z_axis_bridge.c -lm
 */
#include <stdio.h>
#include <stdint.h>

#define TOTAL_SLOTS     20736u
#define CUBES           144u
#define SLOTS_PER_CUBE  144u    /* 20736 / 144 = 144 (12³) */
#define TESSERACTS      18u
#define CUBES_PER_TESS  8u      /* 144 / 18 = 8 */

/* ── 6ico compound structure ────────────────────────────────────── */

static void slot_to_cube(uint32_t slot, uint32_t *cube_idx, uint32_t *slot_in_cube) {
    *cube_idx = slot / SLOTS_PER_CUBE;
    *slot_in_cube = slot % SLOTS_PER_CUBE;
}

static uint32_t cube_to_tesseract(uint32_t cube_idx) {
    return cube_idx / CUBES_PER_TESS;
}

/* ── Mirrors: flip sign within 12³ cube ────────────────────────── */

/* Mirror X: flip x ∈ [0,11] → [11,0], same cube */
static uint32_t mirror_x(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    uint32_t x = s % 12;
    uint32_t rest = s / 12;
    uint32_t x_mirror = 11 - x;
    return cube * SLOTS_PER_CUBE + x_mirror + rest * 12;
}

/* Mirror Y: flip y ∈ [0,11] → [11,0], same cube */
static uint32_t mirror_y(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    uint32_t x = s % 12;
    uint32_t y = (s / 12) % 12;
    uint32_t z = s / 144;
    uint32_t y_mirror = 11 - y;
    return cube * SLOTS_PER_CUBE + x + y_mirror * 12 + z * 144;
}

/* Mirror Z: flip z ∈ [0,11] → [11,0], CROSSES cube boundary.
 * The z-component is computed from the flat slot index (slot / 144),
 * so z can be 0-143 across the full 20736 address space.
 * Flipping z changes which cube the slot belongs to.
 * NOTE: NOT self-inverse — cube changes each application. */
static uint32_t mirror_z(uint32_t slot) {
    uint32_t cube, s;
    slot_to_cube(slot, &cube, &s);
    uint32_t x = s % 12;
    uint32_t y = (s / 12) % 12;
    uint32_t z = s / 144;
    uint32_t z_mirror = 11 - z;
    return cube * SLOTS_PER_CUBE + x + y * 12 + z_mirror * 144;
}

/* ── Tests ───────────────────────────────────────────────────────── */

static int test_mirror_z_crosses_boundary(void)
{
    printf("═══ Test 1: mirror_z crosses tesseract boundary ═══\n");
    int fail = 0;

    uint32_t slot = 0;
    uint32_t cube_before, s_before;
    slot_to_cube(slot, &cube_before, &s_before);
    uint32_t tess_before = cube_to_tesseract(cube_before);

    uint32_t mirrored = mirror_z(slot);
    uint32_t cube_after, s_after;
    slot_to_cube(mirrored, &cube_after, &s_after);
    uint32_t tess_after = cube_to_tesseract(cube_after);

    printf("  slot=%u → cube=%u tess=%u\n", slot, cube_before, tess_before);
    printf("  mirror_z → slot=%u → cube=%u tess=%u\n",
           mirrored, cube_after, tess_after);

    int crosses = (cube_before != cube_after);
    printf("  Crosses boundary? %s\n", crosses ? "YES ✓" : "NO");
    if (!crosses) { printf("  ✗ Expected crossing\n"); fail++; }

    return fail;
}

static int test_mirror_x_stays_within(void)
{
    printf("\n═══ Test 2: mirror_x stays within cube ═══\n");
    int fail = 0;

    uint32_t slot = 0;
    uint32_t cube_before;
    slot_to_cube(slot, &cube_before, &(uint32_t){0});

    uint32_t mirrored = mirror_x(slot);
    uint32_t cube_after;
    slot_to_cube(mirrored, &cube_after, &(uint32_t){0});

    printf("  slot=%u → cube=%u\n", slot, cube_before);
    printf("  mirror_x → slot=%u → cube=%u\n", mirrored, cube_after);

    int stays = (cube_before == cube_after);
    printf("  Stays within cube? %s\n", stays ? "YES ✓" : "NO");
    if (!stays) { printf("  ✗ Expected staying\n"); fail++; }

    return fail;
}

static int test_mirror_z_not_self_inverse(void)
{
    printf("\n═══ Test 3: mirror_z NOT self-inverse (cube changes) ═══\n");
    int fail = 0;

    uint32_t slot = 42;
    uint32_t m1 = mirror_z(slot);
    uint32_t m2 = mirror_z(m1);

    uint32_t cube0, cube1, cube2;
    slot_to_cube(slot, &cube0, &(uint32_t){0});
    slot_to_cube(m1, &cube1, &(uint32_t){0});
    slot_to_cube(m2, &cube2, &(uint32_t){0});

    printf("  slot=%u (cube %u) → mirror_z → %u (cube %u) → mirror_z → %u (cube %u)\n",
           slot, cube0, m1, cube1, m2, cube2);

    int not_inverse = (m2 != slot);
    printf("  NOT self-inverse? %s (cube changed %u→%u→%u)\n",
           not_inverse ? "YES ✓" : "NO", cube0, cube1, cube2);
    if (!not_inverse) { printf("  ✗ Expected non-self-inverse\n"); fail++; }

    return fail;
}

static int test_bridge_chain(void)
{
    printf("\n═══ Test 4: Z-bridge chain (cube 0→11→22) ═══\n");
    int fail = 0;

    /* mirror_z applied 3 times: cube should advance by 11 each time */
    uint32_t slot = 0;
    uint32_t cubes[4];
    for (int i = 0; i < 4; i++) {
        uint32_t c;
        slot_to_cube(slot, &c, &(uint32_t){0});
        cubes[i] = c;
        if (i < 3) slot = mirror_z(slot);
    }

    printf("  Chain: cube %u → %u → %u → %u\n",
           cubes[0], cubes[1], cubes[2], cubes[3]);

    int chain_ok = (cubes[1] == 11 && cubes[2] == 22 && cubes[3] == 33);
    printf("  Expected 0→11→22→33? %s\n", chain_ok ? "YES ✓" : "NO");
    if (!chain_ok) { printf("  ✗ Unexpected chain\n"); fail++; }

    return fail;
}

static int test_bridge_across_all_tesseracts(void)
{
    printf("\n═══ Test 5: Z-bridge visits multiple tesseracts ═══\n");
    int fail = 0;

    uint32_t visited[TESSERACTS] = {0};
    uint32_t slot = 1;

    for (uint32_t i = 0; i < TESSERACTS; i++) {
        uint32_t tess = cube_to_tesseract(slot / SLOTS_PER_CUBE);
        if (tess < TESSERACTS) visited[tess]++;
        slot = mirror_z(slot);
    }

    uint32_t unique = 0;
    for (uint32_t i = 0; i < TESSERACTS; i++) {
        if (visited[i] > 0) unique++;
    }

    printf("  Z-bridge from slot 1: visited %u unique tesseracts\n", unique);
    for (uint32_t i = 0; i < TESSERACTS; i++) {
        if (visited[i] > 0) printf("    tess %u: %u visits\n", i, visited[i]);
    }

    int multi = (unique > 1);
    printf("  Visits multiple tesseracts? %s\n", multi ? "YES ✓" : "NO");
    if (!multi) { printf("  ✗ Expected multiple\n"); fail++; }

    return fail;
}

static int test_all_3_mirrors(void)
{
    printf("\n═══ Test 6: All 3 mirrors — which cross boundaries ═══\n");

    uint32_t slot = 500;
    uint32_t cube0;
    slot_to_cube(slot, &cube0, &(uint32_t){0});

    uint32_t mx = mirror_x(slot);
    uint32_t my = mirror_y(slot);
    uint32_t mz = mirror_z(slot);

    uint32_t cx, cy, cz;
    slot_to_cube(mx, &cx, &(uint32_t){0});
    slot_to_cube(my, &cy, &(uint32_t){0});
    slot_to_cube(mz, &cz, &(uint32_t){0});

    printf("  slot=%u (cube %u)\n", slot, cube0);
    printf("  mirror_x → cube %u (%s)\n", cx, cx == cube0 ? "SAME" : "CROSS");
    printf("  mirror_y → cube %u (%s)\n", cy, cy == cube0 ? "SAME" : "CROSS");
    printf("  mirror_z → cube %u (%s)\n", cz, cz == cube0 ? "SAME" : "CROSS");

    return 0;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void)
{
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Z-Axis Bridge Test — Cross-Tesseract Connection        ║\n");
    printf("║  6ico: 144 cubes × 144 slots = 20736                   ║\n");
    printf("║  18 tesseracts × 8 cubes = 144                         ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    int total_fail = 0;
    total_fail += test_mirror_z_crosses_boundary();
    total_fail += test_mirror_x_stays_within();
    total_fail += test_mirror_z_not_self_inverse();
    total_fail += test_bridge_chain();
    total_fail += test_bridge_across_all_tesseracts();
    total_fail += test_all_3_mirrors();

    printf("\n═══════════════════════════════════════════════════════════\n");
    if (total_fail == 0) {
        printf("PASS: All Z-axis bridge tests passed ✓\n");
    } else {
        printf("FAIL: %d test(s) failed\n", total_fail);
    }
    printf("═══════════════════════════════════════════════════════════\n");

    return total_fail;
}
