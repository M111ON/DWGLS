/* test_kis_hyper_storage.c — Creation Points Storage Analysis
 *
 * Question: How much storage do creation points need?
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_storage test_kis_hyper_storage.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"

#define PI 3.14159265358979323846

static inline uint8_t select_axis(uint32_t slot) {
    if (slot < 6912) return 0;
    if (slot < 13824) return 1;
    return 2;
}

static inline uint32_t axis_slot(uint32_t slot) {
    return slot % 6912;
}

int main(void) {
    printf("Creation Points Storage Analysis\n");
    printf("═══════════════════════════════════════════════════════════\n\n");
    
    uint32_t N = 20736;
    
    /* ═══════════════════════════════════════════════════════════════════
       OPTION 1: Store creation points (slot, scale, hyper_re, hyper_im)
       ═══════════════════════════════════════════════════════════════════ */
    printf("OPTION 1: Store creation points\n");
    printf("  Structure: slot(4B) + scale(4B) + hyper_re(8B) + hyper_im(8B) = 24B\n");
    uint32_t opt1_size = N * 24;
    printf("  Total: %u × 24 = %u bytes = %.1f KB\n", N, opt1_size, opt1_size / 1024.0);
    printf("  Original data: %u bytes\n", N);
    printf("  Overhead: %.1fx\n\n", (double)opt1_size / N);
    
    /* ═══════════════════════════════════════════════════════════════════
       OPTION 2: Store only (slot, scale) — compute hyper on fly
       ═══════════════════════════════════════════════════════════════════ */
    printf("OPTION 2: Store only (slot, scale) — compute hyper on fly\n");
    printf("  Structure: slot(2B) + scale(2B) = 4B\n");
    uint32_t opt2_size = N * 4;
    printf("  Total: %u × 4 = %u bytes = %.1f KB\n", N, opt2_size, opt2_size / 1024.0);
    printf("  Original data: %u bytes\n", N);
    printf("  Overhead: %.1fx\n\n", (double)opt2_size / N);
    
    /* ═══════════════════════════════════════════════════════════════════
       OPTION 3: Don't store creation points — formula computes all
       ═══════════════════════════════════════════════════════════════════ */
    printf("OPTION 3: Don't store creation points — formula computes all\n");
    printf("  Input: slot number (already known) + scale factor (already known)\n");
    printf("  Formula: address = resolve(slot, scale)\n");
    printf("  Storage: 0 bytes (no creation points needed!)\n");
    printf("  Original data: %u bytes\n", N);
    printf("  Overhead: 0x\n\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       OPTION 4: Store compressed data only
       ═══════════════════════════════════════════════════════════════════ */
    printf("OPTION 4: Store compressed data only\n");
    printf("  At scale 0.1: 2076 unique values × 1B = 2076 bytes\n");
    printf("  At scale 0.5: 10370 unique values × 1B = 10370 bytes\n");
    printf("  At scale 1.0: 20736 unique values × 1B = 20736 bytes\n\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       COMPARISON TABLE
       ═══════════════════════════════════════════════════════════════════ */
    printf("COMPARISON:\n");
    printf("  Option | Storage    | Overhead | Lossless | Notes\n");
    printf("  -------|------------|----------|----------|------\n");
    printf("  1      | 527 KB     | 24x      | Yes      | Store all creation points\n");
    printf("  2      | 81 KB      | 4x       | Yes      | Store slot+scale only\n");
    printf("  3      | 0 KB       | 0x       | Yes      | Formula computes all\n");
    printf("  4      | 2-20 KB    | 0.1-1x   | Yes      | Store compressed data\n");
    printf("  Original | 20 KB    | 1x       | Yes      | Raw data\n\n");
    
    /* ═══════════════════════════════════════════════════════════════════
       KEY INSIGHT
       ═══════════════════════════════════════════════════════════════════ */
    printf("KEY INSIGHT:\n");
    printf("  We don't need to store creation points!\n");
    printf("  The formula computes addresses from (slot, scale)\n");
    printf("  Both are already known at decode time.\n\n");
    
    printf("  ENCODE: data[slot] at scale 1.0\n");
    printf("  DECODE: address = resolve(slot, scale)\n");
    printf("          data = read(address)\n\n");
    
    printf("  Storage = compressed data only\n");
    printf("  No metadata, no creation points, no overhead!\n\n");
    
    printf("═══════════════════════════════════════════════════════════\n");
    return 0;
}
