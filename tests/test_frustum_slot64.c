/* test_frustum_slot64.c — frustum_slot64.h verification */
#include <stdio.h>
#include <string.h>
#include "../core/frustum_slot64.h"
#include "../core/frustum_trit.h"

int main(void)
{
    int pass = 0, fail = 0;

    /* T1: FrustumSlot64 size = 64B */
    if (sizeof(FrustumSlot64) == DIAMOND_BLOCK) {
        printf("[PASS] T1: FrustumSlot64 == 64B\n"); pass++;
    } else {
        printf("[FAIL] T1: FrustumSlot64 = %zu (expected 64)\n", sizeof(FrustumSlot64)); fail++;
    }

    /* T2: FrustumStore data size = 3456B */
    if (FRUSTUM_DATA_SZ == 3456u) {
        printf("[PASS] T2: FRUSTUM_DATA_SZ == 3456\n"); pass++;
    } else {
        printf("[FAIL] T2: FRUSTUM_DATA_SZ = %u\n", FRUSTUM_DATA_SZ); fail++;
    }

    /* T3: FrustumStore init zeroes all slots */
    FrustumStore fs;
    frustum_store_init(&fs);
    int all_zero = 1;
    for (uint8_t i = 0u; i < GEAR_MESH; i++) {
        for (uint8_t l = 0u; l < LEVEL_COUNT; l++) {
            if (fs.slots[i].core[l] != 0u) all_zero = 0;
        }
        if (fs.slots[i].reserved_mask != 0u) all_zero = 0;
        if (fs.slots[i].write_count != 0u) all_zero = 0;
        if (fs.slots[i].slope_lo != 0u) all_zero = 0;
    }
    if (fs.total_writes != 0u || fs.total_silenced != 0u) all_zero = 0;
    if (all_zero) {
        printf("[PASS] T3: frustum_store_init zeroes all\n"); pass++;
    } else {
        printf("[FAIL] T3: frustum_store_init non-zero fields\n"); fail++;
    }

    /* T4: frustum_slot_write merges core and tracks coset */
    FrustumSlot64 slot = {0};
    frustum_slot_write(&slot, 0, 0x11111111u, 2);
    frustum_slot_write(&slot, 1, 0x22222222u, 5);
    frustum_slot_write(&slot, 2, 0x33333333u, 2);  /* same coset */
    if (slot.core[0] == 0x11111111u &&
        slot.core[1] == 0x22222222u &&
        slot.core[2] == 0x33333333u &&
        slot.write_count == 3u &&
        (slot.reserved_mask & (1u << 2)) &&
        (slot.reserved_mask & (1u << 5)) &&
        !(slot.reserved_mask & (1u << 7))) {
        printf("[PASS] T4: frustum_slot_write merges core, tracks cosets\n"); pass++;
    } else {
        printf("[FAIL] T4: frustum_slot_write mismatch\n"); fail++;
    }

    /* T5: frustum_store_slot by trit */
    FrustumStore fs2;
    frustum_store_init(&fs2);
    TritAddr t = { .trit = 17, .coset = 2, .face = 5, .level = 1, .letter = 17, .slope = 0 };
    FrustumSlot64 *s = frustum_store_slot(&fs2, &t);
    uint8_t expected_idx = 2 * 6 + 5;  /* coset * FACE_COUNT + face = 17 */
    if (s == &fs2.slots[expected_idx]) {
        printf("[PASS] T5: frustum_store_slot by trit\n"); pass++;
    } else {
        printf("[FAIL] T5: frustum_store_slot wrong pointer\n"); fail++;
    }

    /* T6: frustum_store_coset_summary */
    FrustumStore fs3;
    frustum_store_init(&fs3);
    frustum_slot_write(&fs3.slots[0], 0, 1, 0);  /* coset 0, face 0 */
    frustum_slot_write(&fs3.slots[6], 0, 1, 1);  /* coset 1, face 0 */
    frustum_slot_write(&fs3.slots[12], 0, 1, 2); /* coset 2, face 0 */
    uint8_t coset_sum[COSET_COUNT] = {0};
    frustum_store_coset_summary(&fs3, coset_sum);
    if (coset_sum[0] == 0x01 &&  /* face 0 */
        coset_sum[1] == 0x01 &&  /* face 0 */
        coset_sum[2] == 0x01 &&  /* face 0 */
        coset_sum[3] == 0x00) {  /* unused */
        printf("[PASS] T6: frustum_store_coset_summary\n"); pass++;
    } else {
        printf("[FAIL] T6: coset summary mismatch\n"); fail++;
    }

    /* T7: frustum_store_merkle */
    FrustumStore fs4;
    frustum_store_init(&fs4);
    fs4.slots[0].core[0] = 0xAAAAAAAAu;
    fs4.slots[0].core[1] = 0x55555555u;
    fs4.slots[1].core[0] = 0xFFFFFFFFu;
    uint32_t merkle = frustum_store_merkle(&fs4);
    uint32_t expected = 0xAAAAAAAAu ^ 0x55555555u ^ 0xFFFFFFFFu;
    if (merkle == expected) {
        printf("[PASS] T7: frustum_store_merkle\n"); pass++;
    } else {
        printf("[FAIL] T7: merkle = %08x (expected %08x)\n", merkle, expected); fail++;
    }

    printf("\n=== frustum_slot64: %d PASS, %d FAIL ===\n", pass, fail);
    return fail ? 1 : 0;
}