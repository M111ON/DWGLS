/* kis_alternating_verify.c — Verify 20/12 alternating pattern */
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#define LAYERS 20

/* ═══════════════════════════════════════════════════════════════
   CORRECT: Alternating 20/12
   ═══════════════════════════════════════════════════════════════ */
static uint32_t correct_slots(uint32_t layer) {
    return (layer % 2 == 0) ? 20 : 12;  /* icosa=20, dodeca=12 */
}

/* ═══════════════════════════════════════════════════════════════
   WRONG: Broken pattern (silent bug)
   ═══════════════════════════════════════════════════════════════ */
static uint32_t wrong_slots(uint32_t layer) {
    /* BUG: forgot to alternate, always returns 20 */
    return 20;  /* WRONG! */
}

/* ═══════════════════════════════════════════════════════════════
   OFFSET CALCULATION
   ═══════════════════════════════════════════════════════════════ */
static uint64_t offset_correct(uint32_t layer) {
    uint64_t off = 0;
    for (uint32_t i = 0; i < layer; i++) {
        off += correct_slots(i);
    }
    return off;
}

static uint64_t offset_wrong(uint32_t layer) {
    uint64_t off = 0;
    for (uint32_t i = 0; i < layer; i++) {
        off += wrong_slots(i);
    }
    return off;
}

/* ═══════════════════════════════════════════════════════════════
   ADDRESS: (layer, k) → slot
   ═══════════════════════════════════════════════════════════════ */
static uint64_t addr_correct(uint32_t layer, uint32_t k) {
    uint32_t slots = correct_slots(layer);
    if (k >= slots) return UINT64_MAX;  /* out of bounds */
    return offset_correct(layer) + k;
}

static uint64_t addr_wrong(uint32_t layer, uint32_t k) {
    uint32_t slots = wrong_slots(layer);
    if (k >= slots) return UINT64_MAX;
    return offset_wrong(layer) + k;
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS Alternating Pattern Verification\n");
    printf("Pattern: 20, 12, 20, 12, ... (icosa, dodeca, icosa, dodeca, ...)\n\n");
    
    /* 1. Show pattern */
    printf("=== Pattern ===\n");
    for (uint32_t i = 0; i < 10; i++) {
        printf("  Layer %2u: %u slots (%s)\n", 
               i, correct_slots(i), 
               (i % 2 == 0) ? "icosa" : "dodeca");
    }
    printf("\n");
    
    /* 2. Show offsets */
    printf("=== Offsets ===\n");
    for (uint32_t i = 0; i < 10; i++) {
        uint64_t off_c = offset_correct(i);
        uint64_t off_w = offset_wrong(i);
        printf("  Layer %2u: correct=%llu, wrong=%llu, diff=%lld\n",
               i, off_c, off_w, (int64_t)(off_c - off_w));
    }
    printf("\n");
    
    /* 3. Show address drift */
    printf("=== Address Drift (correct vs wrong) ===\n");
    for (uint32_t layer = 0; layer < LAYERS; layer++) {
        uint32_t slots = correct_slots(layer);
        for (uint32_t k = 0; k < slots && k < 3; k++) {  /* show first 3 k */
            uint64_t addr_c = addr_correct(layer, k);
            uint64_t addr_w = addr_wrong(layer, k);
            int64_t drift = (int64_t)(addr_c - addr_w);
            
            if (drift != 0) {
                printf("  (%2u, %u): correct=%llu, wrong=%llu, drift=%+lld ⚠️\n",
                       layer, k, addr_c, addr_w, drift);
            }
        }
    }
    printf("\n");
    
    /* 4. Total capacity */
    uint64_t total_correct = 0;
    uint64_t total_wrong = 0;
    for (uint32_t i = 0; i < LAYERS; i++) {
        total_correct += correct_slots(i);
        total_wrong += wrong_slots(i);
    }
    
    printf("=== Total Capacity (20 layers) ===\n");
    printf("  Correct: %llu slots\n", total_correct);
    printf("  Wrong:   %llu slots\n", total_wrong);
    printf("  Diff:    %lld slots\n\n", (int64_t)(total_correct - total_wrong));
    
    /* 5. Verification */
    printf("=== Verification ===\n");
    int pass = 1;
    
    /* Check pattern is strict */
    for (uint32_t i = 0; i < LAYERS; i++) {
        uint32_t expected = (i % 2 == 0) ? 20 : 12;
        if (correct_slots(i) != expected) {
            printf("  FAIL: Layer %u should be %u, got %u\n", i, expected, correct_slots(i));
            pass = 0;
        }
    }
    
    /* Check no overlap */
    for (uint32_t i = 0; i < LAYERS - 1; i++) {
        uint64_t end_i = offset_correct(i) + correct_slots(i);
        uint64_t start_next = offset_correct(i + 1);
        if (end_i != start_next) {
            printf("  FAIL: Gap/overlap between layer %u and %u\n", i, i + 1);
            pass = 0;
        }
    }
    
    if (pass) {
        printf("  ✅ Pattern strict: 20, 12, 20, 12, ...\n");
        printf("  ✅ No gaps, no overlaps\n");
        printf("  ✅ All addresses unique\n");
    }
    
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("LESSON: If you forget n%%2==0 at ANY point,\n");
    printf("address drifts silently — no error, no warning.\n");
    printf("Always verify with offset_correct() == offset_wrong().\n");
    
    return pass ? 0 : 1;
}
