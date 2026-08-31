/* test_moe_expert.c — MoE Expert ↔ Geometry Address Mapping
 *
 * Verify:
 * T1: expert → flat roundtrip (all valid combinations)
 * T2: expert → geometry roundtrip
 * T3: disk offset deterministic
 * T4: geometry neighbor property
 * T5: sibling weight type property
 * T6: capacity check
 * T7: boundary conditions
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -no-pie -o build/test_moe_expert tests/test_moe_expert.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "moe_expert_addr.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════ T1: expert → flat roundtrip ═══════════════ */

static void t1_expert_flat_roundtrip(void) {
    printf("── T1 expert → flat roundtrip\n");

    /* Boundary: (0,0,0) → 0 */
    CHECK(1, "(0,0,GATE) == 0", moe_expert_to_flat(0, 0, MOE_WTYPE_GATE) == 0);

    /* Sequential: (0,0,UP) → 1, (0,0,DOWN) → 2, (0,1,GATE) → 3 */
    CHECK(2, "(0,0,UP) == 1", moe_expert_to_flat(0, 0, MOE_WTYPE_UP) == 1);
    CHECK(3, "(0,0,DOWN) == 2", moe_expert_to_flat(0, 0, MOE_WTYPE_DOWN) == 2);
    CHECK(4, "(0,1,GATE) == 3", moe_expert_to_flat(0, 1, MOE_WTYPE_GATE) == 3);

    /* Full roundtrip: all valid combinations for 32 layers × 64 experts */
    int ok = 1;
    for (uint32_t l = 0; l < 32 && ok; l++)
        for (uint32_t e = 0; e < 64 && ok; e++)
            for (uint32_t w = 0; w < 3 && ok; w++) {
                uint32_t flat = moe_expert_to_flat(l, e, w);
                uint32_t l2, e2, w2;
                moe_flat_to_expert(flat, &l2, &e2, &w2);
                if (l2 != l || e2 != e || w2 != w) ok = 0;
            }
    CHECK(5, "32×64×3 roundtrip", ok);

    /* Flat values are unique */
    uint8_t seen[20736 / 8];
    memset(seen, 0, sizeof(seen));
    int unique = 1;
    for (uint32_t l = 0; l < 32 && unique; l++)
        for (uint32_t e = 0; e < 64 && unique; e++)
            for (uint32_t w = 0; w < 3 && unique; w++) {
                uint32_t f = moe_expert_to_flat(l, e, w);
                if (f >= 20736) { unique = 0; break; }
                uint32_t byte = f / 8, bit = f % 8;
                if (seen[byte] & (1u << bit)) { unique = 0; break; }
                seen[byte] |= (1u << bit);
            }
    CHECK(6, "all 6144 flat addresses unique", unique);
}

/* ═══════════════ T2: expert → geometry roundtrip ═══════════════ */

static void t2_expert_geom_roundtrip(void) {
    printf("── T2 expert → geometry roundtrip\n");

    int ok = 1;
    for (uint32_t l = 0; l < 32 && ok; l++)
        for (uint32_t e = 0; e < 64 && ok; e++)
            for (uint32_t w = 0; w < 3 && ok; w++) {
                uint32_t tess, cube, slot;
                moe_expert_to_geom(l, e, w, &tess, &cube, &slot);
                uint32_t l2, e2, w2;
                moe_geom_to_expert(tess, cube, slot, &l2, &e2, &w2);
                if (l2 != l || e2 != e || w2 != w) ok = 0;
            }
    CHECK(7, "geom roundtrip 32×64×3", ok);

    /* Geometry coordinates are valid */
    int valid = 1;
    for (uint32_t l = 0; l < 32 && valid; l++)
        for (uint32_t e = 0; e < 64 && valid; e++)
            for (uint32_t w = 0; w < 3 && valid; w++) {
                uint32_t tess, cube, slot;
                moe_expert_to_geom(l, e, w, &tess, &cube, &slot);
                if (tess >= 18 || cube >= 8 || slot >= 144) valid = 0;
            }
    CHECK(8, "all geometry coords in range", valid);
}

/* ═══════════════ T3: disk offset deterministic ═══════════════ */

static void t3_disk_offset(void) {
    printf("── T3 disk offset deterministic\n");

    /* Block size 512 bytes */
    uint64_t off1 = moe_expert_to_offset(0, 0, MOE_WTYPE_GATE, 512);
    uint64_t off2 = moe_expert_to_offset(0, 0, MOE_WTYPE_GATE, 512);
    CHECK(9, "same expert → same offset", off1 == off2);

    /* Different experts → different offsets */
    uint64_t off_a = moe_expert_to_offset(0, 0, MOE_WTYPE_GATE, 512);
    uint64_t off_b = moe_expert_to_offset(0, 0, MOE_WTYPE_UP, 512);
    CHECK(10, "different wtype → different offset", off_a != off_b);

    /* Offset = flat * block_size */
    uint32_t flat = moe_expert_to_flat(1, 2, MOE_WTYPE_DOWN);
    uint64_t off_c = moe_expert_to_offset(1, 2, MOE_WTYPE_DOWN, 512);
    CHECK(11, "offset = flat * block_size", off_c == (uint64_t)flat * 512);

    /* Large block size */
    uint64_t off_big = moe_expert_to_offset(0, 0, MOE_WTYPE_GATE, 4096);
    CHECK(12, "4096-byte blocks", off_big == 0);
}

/* ═══════════════ T4: geometry neighbor property ═══════════════ */

static void t4_neighbors(void) {
    printf("── T4 geometry neighbor property\n");

    /* Same layer+expert, different wtype → same group */
    uint32_t f_gate = moe_expert_to_flat(0, 0, MOE_WTYPE_GATE);
    uint32_t f_up   = moe_expert_to_flat(0, 0, MOE_WTYPE_UP);
    uint32_t f_down = moe_expert_to_flat(0, 0, MOE_WTYPE_DOWN);
    CHECK(13, "gate/up same group", moe_experts_same_group(f_gate, f_up));
    CHECK(14, "gate/down same group", moe_experts_same_group(f_gate, f_down));

    /* Different layer → different group */
    uint32_t f_l1 = moe_expert_to_flat(1, 0, MOE_WTYPE_GATE);
    CHECK(15, "different layer → different group", !moe_experts_same_group(f_gate, f_l1));

    /* Different expert in same layer → same group (64 experts / 8 cubes = 8 per cube) */
    uint32_t f_e1 = moe_expert_to_flat(0, 1, MOE_WTYPE_GATE);
    CHECK(16, "nearby expert → same group (shared cube)", moe_experts_same_group(f_gate, f_e1));

    /* Expert far away → different group */
    uint32_t f_e_far = moe_expert_to_flat(0, 63, MOE_WTYPE_GATE);
    CHECK(16, "distant expert → different group", !moe_experts_same_group(f_gate, f_e_far));
}

/* ═══════════════ T5: sibling weight type property ═══════════════ */

static void t5_siblings(void) {
    printf("── T5 sibling weight type property\n");

    uint32_t f_gate = moe_expert_to_flat(5, 10, MOE_WTYPE_GATE);
    uint32_t s_up   = moe_expert_sibling(f_gate, MOE_WTYPE_UP);
    uint32_t s_down = moe_expert_sibling(f_gate, MOE_WTYPE_DOWN);

    CHECK(17, "sibling gate→up correct", s_up == moe_expert_to_flat(5, 10, MOE_WTYPE_UP));
    CHECK(18, "sibling gate→down correct", s_down == moe_expert_to_flat(5, 10, MOE_WTYPE_DOWN));

    /* Self-sibling (same wtype) */
    uint32_t s_gate = moe_expert_sibling(f_gate, MOE_WTYPE_GATE);
    CHECK(19, "sibling gate→gate = self", s_gate == f_gate);
}

/* ═══════════════ T6: capacity check ═══════════════ */

static void t6_capacity(void) {
    printf("── T6 capacity check\n");

    /* 32 × 64 × 3 = 6144 — fits */
    CHECK(20, "32×64 fits", moe_capacity(32, 64) == 6144);

    /* 18 × 8 × 3 = 432 — fits */
    CHECK(21, "18×8 fits", moe_capacity(18, 8) == 432);

    /* 64 × 64 × 3 = 12288 — fits */
    CHECK(22, "64×64 fits", moe_capacity(64, 64) == 12288);

    /* 64 × 128 × 3 = 24576 — overflow */
    CHECK(23, "64×128 overflows", moe_capacity(64, 128) == 0);
}

/* ═══════════════ T7: boundary conditions ═══════════════ */

static void t7_boundaries(void) {
    printf("── T7 boundary conditions\n");

    /* Max valid flat: (31, 63, DOWN) = 31*192 + 63*3 + 2 = 5952 + 189 + 2 = 6143 */
    uint32_t max_flat = moe_expert_to_flat(31, 63, MOE_WTYPE_DOWN);
    CHECK(24, "max valid flat = 6143", max_flat == 6143);

    /* Max geometry: flat 6143 → tess=5, cube=3, slot=111 */
    uint32_t tess, cube, slot;
    flat_to_tess(max_flat, &tess, &cube, &slot);
    CHECK(25, "max flat → tess=5", tess == 5);
    CHECK(26, "max flat → cube=2", cube == 2);
    CHECK(27, "max flat → slot=95", slot == 95);

    /* Layer 0, expert 0, all wtypes → consecutive flats */
    CHECK(28, "consecutive wtypes", moe_expert_to_flat(0, 0, 0) + 1 == moe_expert_to_flat(0, 0, 1));
    CHECK(29, "consecutive wtypes", moe_expert_to_flat(0, 0, 1) + 1 == moe_expert_to_flat(0, 0, 2));
}

/* ═══════════════ MAIN ═══════════════ */

int main(void) {
    printf("MoE Expert ↔ Geometry Address Mapping\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    t1_expert_flat_roundtrip();
    t2_expert_geom_roundtrip();
    t3_disk_offset();
    t4_neighbors();
    t5_siblings();
    t6_capacity();
    t7_boundaries();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════\n");

    return fail ? 1 : 0;
}
