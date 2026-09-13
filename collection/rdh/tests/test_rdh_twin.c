/*
 * test_rdh_twin.c — twin capture: two nibble-fuses, two addresses
 * Hand oracles (same style as test_rdh_capture.c):
 *   zeros[48] on 64x81: both walks all-dir-0 -> (48,0) -> key 48. twin=(48,48)
 *   ones[48] (0x01): low NE x48 -> (48,48): 48*81+48 = 3936; high dir-0 -> 48
 *   hi1[48]  (0x10): low dir-0 -> 48; high NE x48 -> 3936. Mirror of ones.
 * Build: gcc -I../../collection/rdh test_rdh_twin.c -o test_rdh_twin
 * Run:   test_rdh_twin
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rdh_capture.h"

#define TEST(label, cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL [%d] %s\n", __LINE__, label); fails++; } \
    else { passes++; } \
} while(0)

int main(void)
{
    int passes = 0, fails = 0;

    printf("=== RDH Twin Test (64x81) ===\n\n");

    RDHConfig cfg = RDH_TWIN_64x81;
    TEST("capacity == 5184", rdh_capacity(&cfg) == 5184);

    /* ── T1: zeros agree on both fuses ── */
    {
        uint8_t zeros[48] = {0};
        int64_t k1, k2;
        rdh_capture_twin(zeros, 48, &cfg, &k1, &k2);
        TEST("zeros twin == (48,48)", k1 == 48 && k2 == 48);
    }

    /* ── T2: ones split (low moves, high stays) ── */
    {
        uint8_t ones[48];
        memset(ones, 1, 48);
        int64_t k1, k2;
        rdh_capture_twin(ones, 48, &cfg, &k1, &k2);
        TEST("ones twin == (3936,48)", k1 == 3936 && k2 == 48);
    }

    /* ── T3: high-plane mirror (high moves, low stays) ── */
    {
        uint8_t hi1[48];
        memset(hi1, 0x10, 48);
        int64_t k1, k2;
        rdh_capture_twin(hi1, 48, &cfg, &k1, &k2);
        TEST("0x10 twin == (48,3936)", k1 == 48 && k2 == 3936);
    }

    /* ── T4: determinism + range ── */
    {
        uint8_t buf[48];
        for (int i = 0; i < 48; i++) buf[i] = (uint8_t)(i * 7 + 3);
        int64_t a1, a2, b1, b2;
        rdh_capture_twin(buf, 48, &cfg, &a1, &a2);
        rdh_capture_twin(buf, 48, &cfg, &b1, &b2);
        TEST("deterministic pair", a1 == b1 && a2 == b2);
        TEST("both in [0,5184)", a1 >= 0 && a1 < 5184 && a2 >= 0 && a2 < 5184);
        /* original entry point == low fuse (behavior unchanged) */
        TEST("rdh_capture == k1", rdh_capture(buf, 48, &cfg) == a1);
    }

    /* ── T5: nibble-plane isolation on longer data ── */
    {
        uint8_t base[96], mod[96];
        for (int i = 0; i < 96; i++) base[i] = (uint8_t)(i * 5 + 11);
        memcpy(mod, base, 96);
        mod[17] ^= 0xF0;   /* high nibble only */
        int64_t k1b, k2b, k1m, k2m;
        rdh_capture_twin(base, 96, &cfg, &k1b, &k2b);
        rdh_capture_twin(mod, 96, &cfg, &k1m, &k2m);
        TEST("high-nibble tamper moves k2 only", k1b == k1m && k2b != k2m);
        mod[17] ^= 0xF0; mod[17] ^= 0x0F;   /* now low nibble only */
        rdh_capture_twin(mod, 96, &cfg, &k1m, &k2m);
        TEST("low-nibble tamper moves k1 only", k1b != k1m && k2b == k2m);
    }

    printf("\n=== Results: %d/%d passed ===\n", passes, passes + fails);
    return fails > 0 ? 1 : 0;
}
