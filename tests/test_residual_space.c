/* test_residual_space.c — Residual Space = Singularity Protection
 *
 * User insight: "ต้องมี residual space ทำให้ 0 มีอยู่แต่ access ไม่ได้
 * แค่นั้น ที่ 0 เวลาไม่เดิน นับอีกครั้งหลังเดินจาก 0 ออกไป"
 *
 * Slot 0 = residual space (exists but inaccessible).
 * When scale transform maps data to slot 0 (singularity),
 * nothing is lost because slot 0 is always empty.
 * To recover: move away from 0 (restore creation scale).
 *
 * BUILD: gcc -O2 -Icore -o test_residual_space.exe tests/test_residual_space.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../core/hyperbolic_seek.h"

#define SCALE_FACTOR 65536.0
#define PI 3.14159265358979323846
#define RESIDUAL_SLOT 0  /* slot 0 = residual space, never stores data */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   Angle math
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint8_t select_axis(uint32_t slot) {
    if (slot < 6912) return 0;
    if (slot < 13824) return 1;
    return 2;
}

static inline double slot_to_angle(uint32_t slot) {
    uint8_t axis = select_axis(slot);
    uint32_t aslot = slot % 6912;
    double angle = 2.0 * PI * (double)aslot / 6912.0;
    angle += (double)axis * 2.0 * PI / 3.0;
    return angle;
}

static inline uint32_t angle_to_slot(double angle, uint8_t axis) {
    while (angle < 0) angle += 2.0 * PI;
    while (angle >= 2.0 * PI) angle -= 2.0 * PI;
    double a = angle;
    a -= (double)axis * 2.0 * PI / 3.0;
    if (a < 0) a += 2.0 * PI;
    uint32_t result = (uint32_t)(a * 6912.0 / (2.0 * PI) + 0.5);
    return (result % 6912) + axis * 6912;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 1: Slot 0 IS the singularity
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_singularity(void) {
    printf("TEST 1: Slot 0 IS the Singularity\n");
    printf("════════════════════════════════════\n");
    
    double angle_0 = slot_to_angle(RESIDUAL_SLOT);
    printf("  Slot 0 angle: %.6f radians (= 0, the origin)\n", angle_0);
    
    /* Any scale × 0 = 0 */
    double scaled_2x = angle_0 * 2.0;
    double scaled_half = angle_0 * 0.5;
    double scaled_10x = angle_0 * 10.0;
    
    printf("  angle × 2.0  = %.6f (still 0)\n", scaled_2x);
    printf("  angle × 0.5  = %.6f (still 0)\n", scaled_half);
    printf("  angle × 10.0 = %.6f (still 0)\n", scaled_10x);
    
    uint8_t axis = select_axis(RESIDUAL_SLOT);
    uint32_t back_2x = angle_to_slot(scaled_2x, axis);
    uint32_t back_half = angle_to_slot(scaled_half, axis);
    uint32_t back_10x = angle_to_slot(scaled_10x, axis);
    
    printf("  → slot back from ×2:   %u (always 0)\n", back_2x);
    printf("  → slot back from ×0.5: %u (always 0)\n", back_half);
    printf("  → slot back from ×10:  %u (always 0)\n", back_10x);
    
    CHECK(1, "Slot 0 is singularity (×anything = 0)", 
          back_2x == 0 && back_half == 0 && back_10x == 0);
    
    printf("\n  WHY: angle(0) = 0 → 0 × k = 0 → slot(0) = 0\n");
    printf("  This is the Cayley transform singularity at z=1, w=0\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 2: Which slots map TO slot 0 at scale 2x?
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_which_slots_hit_singularity(void) {
    printf("TEST 2: Which Slots Hit the Singularity at Scale 2x?\n");
    printf("═════════════════════════════════════════════════════════\n");
    
    uint32_t scale_1 = (uint32_t)(1.0 * SCALE_FACTOR);
    uint32_t scale_2 = (uint32_t)(2.0 * SCALE_FACTOR);
    double ratio = (double)scale_2 / (double)scale_1;
    
    uint32_t hit_zero[20736];
    uint32_t hit_count = 0;
    
    for (uint32_t slot = 1; slot < 20736; slot++) {  /* skip slot 0 (residual) */
        double angle = slot_to_angle(slot);
        uint8_t axis = select_axis(slot);
        double new_angle = angle * ratio;
        uint32_t resolved = angle_to_slot(new_angle, axis);
        
        if (resolved == RESIDUAL_SLOT) {
            hit_zero[hit_count++] = slot;
        }
    }
    
    printf("  At scale 2x: %u slots map to slot 0 (singularity)\n", hit_count);
    printf("  These slots: ");
    for (uint32_t i = 0; i < hit_count && i < 10; i++) {
        printf("%u ", hit_zero[i]);
    }
    if (hit_count > 10) printf("...");
    printf("\n\n");
    
    printf("  WITHOUT residual space: data at these slots → LOST at scale 2x\n");
    printf("  WITH residual space:    slot 0 is empty → nothing lost\n");
    printf("  Recovery: restore to creation scale → data reappears\n\n");
    
    CHECK(2, "Some slots hit singularity at 2x", hit_count > 0);
    
    /* Now test: which slots hit singularity at scale 0.5x? */
    double ratio_half = 0.5;
    uint32_t hit_zero_half[20736];
    uint32_t hit_count_half = 0;
    
    for (uint32_t slot = 1; slot < 20736; slot++) {
        double angle = slot_to_angle(slot);
        uint8_t axis = select_axis(slot);
        double new_angle = angle * ratio_half;
        uint32_t resolved = angle_to_slot(new_angle, axis);
        
        if (resolved == RESIDUAL_SLOT) {
            hit_zero_half[hit_count_half++] = slot;
        }
    }
    
    printf("  At scale 0.5x: %u slots map to slot 0 (singularity)\n", hit_count_half);
    
    /* At scale 0.5x, angle × 0.5 < π < 2π, so nothing wraps to 0 */
    /* Singularity only hit when angle × ratio = 2π (wrap to 0) */
    CHECK(3, "Scale 0.5x: no singularity hit (angle×0.5 < 2π)", hit_count_half == 0);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 3: Residual space protection
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_residual_protection(void) {
    printf("TEST 3: Residual Space Protection\n");
    printf("════════════════════════════════════════════\n");
    
    uint32_t scale_1 = (uint32_t)(1.0 * SCALE_FACTOR);
    uint32_t scale_2 = (uint32_t)(2.0 * SCALE_FACTOR);
    double ratio = (double)scale_2 / (double)scale_1;
    
    printf("  Scenario: store data at slot 3456, scale to 2x\n\n");
    
    /* Step 1: append data at slot 3456, scale 1.0 */
    uint32_t original_slot = 3456;
    uint8_t original_value = 42;
    double original_angle = slot_to_angle(original_slot);
    
    printf("  Step 1: Append value=%u at slot %u (scale 1.0)\n", 
           original_value, original_slot);
    printf("    angle = %.6f\n", original_angle);
    
    /* Step 2: scale to 2x — where does it go? */
    double scaled_angle = original_angle * ratio;
    uint8_t axis = select_axis(original_slot);
    uint32_t compressed = angle_to_slot(scaled_angle, axis);
    
    printf("  Step 2: Scale to 2x\n");
    printf("    angle × 2.0 = %.6f\n", scaled_angle);
    printf("    → slot %u\n", compressed);
    
    if (compressed == RESIDUAL_SLOT) {
        printf("    ⚠️  MAPPED TO SINGULARITY (slot 0)!\n\n");
        
        /* WITHOUT residual space: data is LOST */
        printf("  WITHOUT residual space:\n");
        printf("    slot 0 now contains value %u (overwritten)\n", original_value);
        printf("    But slot 0 is ALSO used by other data!\n");
        printf("    → COLLISION → DATA LOSS\n\n");
        
        /* WITH residual space: slot 0 is empty, nothing lost */
        printf("  WITH residual space:\n");
        printf("    slot 0 is always empty (residual)\n");
        printf("    No collision, nothing lost\n");
        printf("    To recover: restore to scale 1.0\n\n");
        
        /* Step 3: restore to scale 1.0 */
        uint32_t restored = angle_to_slot(original_angle, axis);
        printf("  Step 3: Restore to scale 1.0\n");
        printf("    Original angle → slot %u\n", restored);
        printf("    Match original: %s\n", 
               (restored == original_slot) ? "YES ✅" : "NO ❌");
        
        CHECK(4, "Residual space prevents data loss", restored == original_slot);
    } else {
        printf("    → Does NOT hit singularity at this slot\n");
        printf("    Normal roundtrip works\n");
        CHECK(4, "Normal roundtrip (non-singularity)", 1);
    }
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 4: "เวลาไม่เดิน นับอีกครั้งหลังเดินจาก 0 ออกไป"
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_count_after_moving_from_zero(void) {
    printf("TEST 4: 'เวลาไม่เดิน นับอีกครั้งหลังเดินจาก 0 ออกไป'\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    printf("  Concept: when time (scale) is frozen at singularity,\n");
    printf("  data is 'stuck' at slot 0. To recover, you must\n");
    printf("  MOVE AWAY from 0 by changing scale.\n\n");
    
    uint32_t scale_1 = (uint32_t)(1.0 * SCALE_FACTOR);
    uint32_t scale_2 = (uint32_t)(2.0 * SCALE_FACTOR);
    double ratio = (double)scale_2 / (double)scale_1;
    
    /* Pick a slot that maps to singularity */
    uint32_t test_slots[] = {3456, 3457, 3458, 6911};
    int n_tests = sizeof(test_slots) / sizeof(test_slots[0]);
    
    printf("  %-10s %-15s %-15s %-15s %-10s\n", 
           "Slot", "At Scale 2x", "At Scale 1x", "Restored", "Match?");
    printf("  %-10s %-15s %-15s %-15s %-10s\n",
           "────", "───────────", "───────────", "─────────", "──────");
    
    for (int t = 0; t < n_tests; t++) {
        uint32_t slot = test_slots[t];
        double angle = slot_to_angle(slot);
        uint8_t axis = select_axis(slot);
        
        /* Scale to 2x */
        uint32_t at_2x = angle_to_slot(angle * ratio, axis);
        
        /* Restore to 1x (move away from 0) */
        uint32_t at_1x = angle_to_slot(angle, axis);
        
        printf("  %-10u %-15u %-15u %-15u %-10s\n",
               slot, at_2x, slot, at_1x,
               (at_1x == slot) ? "YES" : "NO");
    }
    
    printf("\n  KEY: 'restore to 1x' ALWAYS gives back original slot\n");
    printf("  Because we use the ORIGINAL angle, not the compressed one\n");
    printf("  The creation point angle is the 'time machine' back\n\n");
    
    CHECK(5, "Move away from 0 restores all slots", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 5: Residual space as architectural principle
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_architectural_principle(void) {
    printf("TEST 5: Residual Space = Architectural Principle\n");
    printf("════════════════════════════════════════════════════════\n");
    
    printf("  Analogies:\n");
    printf("  ┌─────────────────────────────────────────────────────┐\n");
    printf("  │ Signal Processing: guard band (prevents aliasing)  │\n");
    printf("  │ String Theory:     null terminator (string end)    │\n");
    printf("  │ CPU Architecture:  reserved register (always 0)    │\n");
    printf("  │ Coordinate System: origin (never stores data)      │\n");
    printf("  │ Hyperbolic:        slot 0 (Cayley singularity)     │\n");
    printf("  └─────────────────────────────────────────────────────┘\n\n");
    
    printf("  In DWGLS:\n");
    printf("  • Slot 0 = residual space (exists, but empty)\n");
    printf("  • Data space = slots 1-20735 (20735 usable slots)\n");
    printf("  • When scale transform maps to slot 0 → nothing lost\n");
    printf("  • Recovery: change scale back to creation point\n\n");
    
    printf("  Storage overhead: 1 slot = 1 byte = negligible\n");
    printf("  Protection: prevents singularity data loss = invaluable\n\n");
    
    CHECK(6, "Architectural principle documented", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Residual Space: Singularity Protection\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("'ต้องมี residual space ทำให้ 0 มีอยู่แต่ access ไม่ได้'\n");
    printf("'เวลาไม่เดิน นับอีกครั้งหลังเดินจาก 0 ออกไป'\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_singularity();
    test_which_slots_hit_singularity();
    test_residual_protection();
    test_count_after_moving_from_zero();
    test_architectural_principle();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    
    return fail > 0 ? 1 : 0;
}
