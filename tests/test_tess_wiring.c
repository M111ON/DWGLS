/* ═══════════════════════════════════════════════════════════════════════════
 * test_tess_wiring.c — Test wiring between rescope and unified access
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geo_tess_wiring.h"

int main(void) {
    printf("Tesseract Wiring Test\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");
    
    int errors = 0;
    
    /* Test 1: Address mapping round-trip */
    printf("Test 1: Address mapping round-trip\n");
    {
        for (uint32_t tess = 0; tess < TESS_N_TESS; tess++) {
            for (uint32_t cube = 0; cube < TESS_CUBES; cube++) {
                for (uint32_t local = 0; local < TESS_CELLS; local++) {
                    uint32_t flat = tess_to_flat(tess, cube, local);
                    uint32_t t2, c2, l2;
                    flat_to_tess(flat, &t2, &c2, &l2);
                    
                    if (t2 != tess || c2 != cube || l2 != local) {
                        printf("  ❌ Round-trip failed: tess=%u cube=%u local=%u → flat=%u → tess=%u cube=%u local=%u\n",
                               tess, cube, local, flat, t2, c2, l2);
                        errors++;
                        break;
                    }
                }
            }
        }
        if (errors == 0) {
            printf("  ✅ All %u positions round-trip correctly\n", TESS_TOTAL);
        }
    }
    
    /* Test 2: Index frame build/read */
    printf("\nTest 2: Index frame build/read\n");
    {
        uint8_t index_frame[TESS_CELLS * 64];
        uint32_t cube_sizes[TESS_CUBES] = {144, 144, 144, 144, 144, 144, 144, 144};
        
        tess_build_index(index_frame, 0, NULL, cube_sizes);
        
        for (uint32_t c = 0; c < TESS_CUBES; c++) {
            uint32_t base, len, stride;
            tess_read_index(index_frame, c, &base, &len, &stride);
            
            /* tess_to_flat(0,c,0) = (0×1152 + c×144 + 0) % 20736 = c×144
             * written by hand — not derived from the function under test */
            uint32_t expected_base = c * 144u;
            if (base != expected_base || len != 144 || stride != 37) {
                printf("  ❌ Index read failed: cube=%u base=%u (expected %u) len=%u stride=%u\n",
                       c, base, expected_base, len, stride);
                errors++;
            }
        }
        if (errors == 0) {
            printf("  ✅ Index frame build/read correct\n");
        }
    }
    
    /* Test 3: Passive log */
    printf("\nTest 3: Passive log replay\n");
    {
        TessPassiveLog log;
        log.count = 0;
        
        /* append some scale changes */
        tess_log_append(&log, 0, 10);
        tess_log_append(&log, 10, 20);
        tess_log_append(&log, 20, 30);
        
        /* replay */
        uint32_t final_w = tess_log_replay(&log, 0);
        if (final_w != 30) {
            printf("  ❌ Log replay failed: expected 30, got %u\n", final_w);
            errors++;
        } else {
            printf("  ✅ Log replay correct (0 → 30)\n");
        }
        
        /* collapse */
        uint32_t collapsed_final;
        tess_log_collapse(&log, 0, &collapsed_final);
        if (log.count != 1 || log.entries[0].from_w != 0 || log.entries[0].to_w != 30) {
            printf("  ❌ Log collapse failed\n");
            errors++;
        } else {
            printf("  ✅ Log collapse correct (3 entries → 1)\n");
        }
    }
    
    /* Test 4: Magnify glass */
    printf("\nTest 4: Magnify glass\n");
    {
        /* center should be in glass */
        uint32_t center_flat = tess_to_flat(0, 0, TESS_GLASS_CENTER);
        if (!tess_in_glass(center_flat)) {
            printf("  ❌ Center not in glass\n");
            errors++;
        } else {
            printf("  ✅ Center in glass\n");
        }
        
        /* opposite side should not be in glass */
        uint32_t opp_flat = tess_antipode(center_flat);
        if (tess_in_glass(opp_flat)) {
            printf("  ❌ Antipode should not be in glass\n");
            errors++;
        } else {
            printf("  ✅ Antipode outside glass\n");
        }
    }
    
    /* Test 5: Unified access through wiring */
    printf("\nTest 5: Unified access through wiring\n");
    {
        GeoUnifiedVolume v;
        geo_unified_init(&v);
        
        /* write through wiring */
        uint8_t data[64];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 3 + 7);
        
        for (uint32_t tess = 0; tess < 3; tess++) {
            for (uint32_t cube = 0; cube < TESS_CUBES; cube++) {
                for (uint32_t local = 0; local < TESS_CELLS; local++) {
                    if (tess_unified_write(&v, tess, cube, local, data, 64) != 0) {
                        printf("  ❌ Write failed: tess=%u cube=%u local=%u\n", tess, cube, local);
                        errors++;
                        break;
                    }
                }
            }
        }
        
        /* read through wiring */
        uint8_t readbuf[64];
        for (uint32_t tess = 0; tess < 3; tess++) {
            for (uint32_t cube = 0; cube < TESS_CUBES; cube++) {
                for (uint32_t local = 0; local < TESS_CELLS; local++) {
                    if (tess_unified_read(&v, tess, cube, local, readbuf, 64) != 0) {
                        printf("  ❌ Read failed: tess=%u cube=%u local=%u\n", tess, cube, local);
                        errors++;
                        break;
                    }
                    if (memcmp(data, readbuf, 64) != 0) {
                        printf("  ❌ Data mismatch: tess=%u cube=%u local=%u\n", tess, cube, local);
                        errors++;
                        break;
                    }
                }
            }
        }
        
        if (errors == 0) {
            printf("  ✅ Unified access through wiring correct (3 tesseracts)\n");
        }
        
        geo_unified_free(&v);
    }
    
    /* Test 6: Verify wiring */
    printf("\nTest 6: Verify wiring\n");
    {
        GeoUnifiedVolume v;
        geo_unified_init(&v);
        
        /* fill all slots */
        for (uint32_t i = 0; i < TESS_TOTAL; i++) {
            uint8_t *ptr = (uint8_t *)v.slot_ptrs[i];
            if (ptr) {
                for (int j = 0; j < 64; j++) ptr[j] = (uint8_t)(i + j);
            }
        }
        
        if (tess_verify_wiring(&v)) {
            printf("  ✅ Wiring verified for all %u positions\n", TESS_TOTAL);
        } else {
            printf("  ❌ Wiring verification failed\n");
            errors++;
        }
        
        geo_unified_free(&v);
    }
    
    /* Summary */
    printf("\n═══════════════════════════════════════════════════════════════\n");
    if (errors == 0) {
        printf("✅ ALL TESTS PASSED\n");
    } else {
        printf("❌ %d TESTS FAILED\n", errors);
    }
    
    return errors;
}
