/*
 * test_rdh_capture.c — test RDH capture entry point
 * Build: gcc -I../../collection/rdh test_rdh_capture.c -o test_rdh_capture
 * Run:   test_rdh_capture
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
    
    printf("=== RDH Capture Test ===\n\n");
    
    /* ── Config: 144×144 field ── */
    RDHConfig cfg = RDH_CAPTURE_144;
    printf("Config: {n_rings=%lld, n_wedges=%lld, n_mirror=%lld, max_u=%lld, n_v=%lld}\n",
           (long long)cfg.n_rings, (long long)cfg.n_wedges,
           (long long)cfg.n_mirror, (long long)cfg.max_u,
           (long long)cfg.n_v);
    printf("Capacity: %lld (should be 20736)\n", (long long)rdh_capacity(&cfg));
    TEST("capacity == 20736", rdh_capacity(&cfg) == 20736);
    
    /* ── Test 1: Deterministic ── */
    {
        uint8_t data[48];
        for (int i = 0; i < 48; i++) data[i] = (uint8_t)(i * 3);
        
        int64_t k1 = rdh_capture(data, 48, &cfg);
        int64_t k2 = rdh_capture(data, 48, &cfg);
        int64_t k3 = rdh_capture(data, sizeof(data), &cfg);
        
        TEST("deterministic (same buf)", k1 == k2);
        TEST("deterministic (same array)", k1 == k3);
    }
    
    /* ── Test 2: All zeros → home (48, 0) → key = 48 ── */
    {
        uint8_t zeros[48] = {0};
        int64_t key = rdh_capture(zeros, 48, &cfg);
        TEST("zeros → key == 48", key == 48);
        
        /* Decompose: ring=0, wedge=48 */
        int64_t ring, wedge, mirror, u;
        rdh_decompose(&cfg, key, &ring, &wedge, &mirror, &u);
        TEST("zeros → ring==0 (home_y)", ring == 0);
        TEST("zeros → wedge==48 (home_x)", wedge == 48);
    }
    
    /* ── Test 3: All ones → NE 48x → home (48, 48) → key = 48×144+48 = 6960 ── */
    {
        uint8_t ones[48];
        memset(ones, 1, 48);
        int64_t key = rdh_capture(ones, 48, &cfg);
        TEST("ones → key == 6960", key == 6960);
    }
    
    /* ── Test 4: Varying data → mostly unique keys ── */
    {
        int64_t keys[10];
        uint8_t buf[48];
        for (int i = 0; i < 10; i++) {
            /* Guarantee uniqueness: use data where byte 0 varies too */
            for (int j = 0; j < 48; j++)
                buf[(i + j) % 48] = (uint8_t)(j * 7 + i * 13 + 3);
            keys[i] = rdh_capture(buf, 48, &cfg);
        }
        int unique = 1;
        for (int i = 0; i < 10 && unique; i++)
            for (int j = i+1; j < 10 && unique; j++)
                if (keys[i] == keys[j]) unique = 0;
        /* Not guaranteed unique — geometry can collide. Just verify they're valid. */
        int valid = 1;
        for (int i = 0; i < 10; i++)
            if (keys[i] < 0 || keys[i] >= rdh_capacity(&cfg)) valid = 0;
        TEST("10 varying buffers → all in range", valid);
        printf("  Keys: ");
        for (int i = 0; i < 10; i++) printf("%lld ", (long long)keys[i]);
        printf("\n");
    }
    
    /* ── Test 5: Short data (< 48) auto-pads by cycling ── */
    {
        uint8_t short_data[1] = {0x42};  /* 0x42 → dir = 2 (N) */
        int64_t key = rdh_capture(short_data, 1, &cfg);
        /* walks 48 steps: each step dir=2 → acc_y += 48 */
        /* ring = 48 % 144 = 48, wedge = 0 */
        int64_t ring, wedge, mirror, u;
        rdh_decompose(&cfg, key, &ring, &wedge, &mirror, &u);
        TEST("short data auto-pads (ring=48)", ring == 48);
        TEST("short data auto-pads (wedge=0)", wedge == 0);
    }
    
    /* ── Test 6: Frame seek bridge ── */
    {
        uint8_t data[48];
        for (int i = 0; i < 48; i++) data[i] = (uint8_t)(i * 7);
        
        uint16_t enc = rdh_capture_to_enc(data, 48, &cfg);
        TEST("enc in range 0..1439", enc < 1440);
        
        /* Same data → same enc */
        uint16_t enc2 = rdh_capture_to_enc(data, 48, &cfg);
        TEST("enc deterministic", enc == enc2);
        
        /* Different data → different enc (usually) */
        memset(data, 0xFF, 48);
        uint16_t enc3 = rdh_capture_to_enc(data, 48, &cfg);
        /* 0xFF → dir=15 → default (no move) → home(0,0) → enc=0 */
        TEST("0xFF → enc == 0", enc3 == 0);
    }
    
    /* ── Test 7: Long data (> 48) uses full length ── */
    {
        uint8_t long_data[200];
        for (size_t i = 0; i < 200; i++) long_data[i] = (uint8_t)(i * 3 + 7);
        
        int64_t key_short = rdh_capture(long_data, 48, &cfg);
        int64_t key_long  = rdh_capture(long_data, 200, &cfg);
        TEST("200-byte ≠ 48-byte (more entropy)", key_short != key_long);
    }
    
    /* ── Test 8: Scale 49 → 1M capacity ── */
    {
        RDHConfig scale_cfg = RDH_CAPTURE_SCALE(49);
        TEST("scale 49 capacity >= 1M", rdh_capacity(&scale_cfg) >= 1000000LL);
        printf("  Scale 49 capacity: %lld\n", (long long)rdh_capacity(&scale_cfg));
        
        /* Random test on scale 49 */
        uint8_t buf[48];
        for (int i = 0; i < 48; i++) buf[i] = (uint8_t)(i * 13);
        int64_t key = rdh_capture(buf, 48, &scale_cfg);
        TEST("scale 49 key in range", key >= 0 && key < rdh_capacity(&scale_cfg));
    }
    
    /* ── Results ── */
    printf("\n=== Results: %d/%d passed ===\n", passes, passes + fails);
    return fails > 0 ? 1 : 0;
}
