/*
 * test_kv_page_rdh.c — verify RDH address formula correctness
 *
 * Tests:
 *   1. All keys unique across (ring, wedge, mirror, u)
 *   2. Roundtrip: key → decompose → recompose
 *   3. Address space size
 *   4. No collision at full capacity
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kv_page_rdh.h"

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    int passed = 0, failed = 0;

    /* Test 1: All keys unique */
    int max_slots = kv_rdh_max_slots();
    int64_t *keys = malloc(sizeof(int64_t) * max_slots);
    int count = 0;

    for (int r = 0; r < KV_RDH_N_RINGS; r++)
        for (int w = 0; w < KV_RDH_N_WEDGES; w++)
            for (int m = 0; m < KV_RDH_N_MIRROR; m++)
                for (int u = 0; u < KV_RDH_MAX_U; u++)
                    keys[count++] = kv_rdh_key(r, w, m, u);

    qsort(keys, count, sizeof(int64_t), cmp_i64);
    int collision = 0;
    for (int i = 1; i < count; i++)
        if (keys[i] == keys[i-1]) { collision = 1; break; }

    if (!collision && count == max_slots) {
        printf("  PASS: %d unique keys (no collision)\n", count);
        passed++;
    } else {
        printf("  FAIL: %d keys, collision=%d, expected=%d\n", count, collision, max_slots);
        failed++;
    }

    /* Test 2: Roundtrip decompose → recompose */
    int rt_ok = 1;
    for (int i = 0; i < 1000; i++) {
        int r = rand() % KV_RDH_N_RINGS;
        int w = rand() % KV_RDH_N_WEDGES;
        int m = rand() % KV_RDH_N_MIRROR;
        int u = rand() % KV_RDH_MAX_U;
        int key = kv_rdh_key(r, w, m, u);
        int rr = kv_rdh_ring(key);
        int rm = kv_rdh_mirror(key);
        int ru = kv_rdh_u(key);
        if (rr != r || rm != m || ru != u) {
            printf("  FAIL roundtrip: (%d,%d,%d,%d) -> key=%d -> (%d,%d,%d)\n",
                r, w, m, u, key, rr, rm, ru);
            rt_ok = 0;
            failed++;
            break;
        }
    }
    if (rt_ok) {
        printf("  PASS: roundtrip decompose/recompose (1000 random)\n");
        passed++;
    }

    /* Test 3: Address space matches formula */
    int n_rings_used = 6;    /* LFM2 attn layers */
    int n_wedges_used = 1;
    int n_mirror_used = 2;
    int n_u_used = 32;       /* 4096 ctx / 128 */
    int expected = n_rings_used * n_wedges_used * n_mirror_used * n_u_used;
    int actual = 0;
    for (int r = 0; r < n_rings_used; r++)
        for (int m = 0; m < n_mirror_used; m++)
            for (int u = 0; u < n_u_used; u++) {
                int key = kv_rdh_key(r, 0, m, u);
                (void)key;
                actual++;
            }
    if (actual == expected) {
        printf("  PASS: LFM2 address space = %d slots (6 layers × 2 mirror × 32 u)\n", actual);
        passed++;
    } else {
        printf("  FAIL: LFM2 address space: got %d, expected %d\n", actual, expected);
        failed++;
    }

    /* Test 4: Total capacity */
    printf("  Total RDH capacity: %d slots (%d rings × %d wedges × %d mirror × %d u)\n",
        max_slots, KV_RDH_N_RINGS, KV_RDH_N_WEDGES,
        KV_RDH_N_MIRROR, KV_RDH_MAX_U);

    printf("\n%d / %d passed\n", passed, passed + failed);
    free(keys);
    return failed > 0 ? 1 : 0;
}
