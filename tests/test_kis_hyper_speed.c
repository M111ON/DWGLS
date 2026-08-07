/* test_kis_hyper_speed.c — Benchmark Hyperbolic Address Resolution Speed
 *
 * Measure: how fast can we compute addresses?
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_speed test_kis_hyper_speed.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"

static inline uint8_t select_axis(uint32_t slot) {
    if (slot < 6912) return 0;
    if (slot < 13824) return 1;
    return 2;
}

static inline uint32_t resolve_addr(uint32_t slot, uint32_t scale) {
    uint8_t axis = select_axis(slot);
    uint32_t aslot = slot % 6912;
    double ratio = (double)scale / (double)((uint32_t)(1.0 * 65536.0));
    
    /* Get Hyperbolic coordinate */
    uint32_t axis_slot = aslot % 6912;
    double angle = 2.0 * 3.14159265358979323846 * (double)axis_slot / 6912.0;
    angle += (double)axis * 2.0 * 3.14159265358979323846 / 3.0;
    
    double new_re = cos(angle) * ratio;
    double new_im = sin(angle) * ratio;
    
    /* Inverse Cayley */
    double denom = (1.0 + new_re) * (1.0 + new_re) + new_im * new_im;
    if (denom < 1e-15) return slot;
    
    double z_re = ((1.0 - new_re) * (1.0 + new_re) + new_im * new_im) / denom;
    double z_im = (-2.0 * new_im) / denom;
    
    double a = atan2(z_im, z_re);
    if (a < 0) a += 2.0 * 3.14159265358979323846;
    a -= (double)axis * 2.0 * 3.14159265358979323846 / 3.0;
    if (a < 0) a += 2.0 * 3.14159265358979323846;
    
    uint32_t result_slot = (uint32_t)(a * 6912.0 / (2.0 * 3.14159265358979323846) + 0.5);
    return (result_slot % 6912) + axis * 6912;
}

int main(void) {
    printf("Hyperbolic Address Resolution Speed Benchmark\n");
    printf("═══════════════════════════════════════════════════════════\n\n");
    
    uint32_t N = 1000000;
    uint32_t scale = (uint32_t)(0.5 * 65536.0);
    
    /* Warm up */
    for (uint32_t i = 0; i < 1000; i++) {
        volatile uint32_t r = resolve_addr(i, scale);
        (void)r;
    }
    
    /* Benchmark */
    clock_t start = clock();
    uint32_t dummy = 0;
    for (uint32_t i = 0; i < N; i++) {
        dummy += resolve_addr(i % 20736, scale);
    }
    clock_t end = clock();
    
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double ops_per_sec = N / elapsed;
    double ns_per_op = elapsed / N * 1e9;
    
    printf("  Resolved %u addresses in %.3f seconds\n", N, elapsed);
    printf("  Speed: %.0f ops/sec (%.1f ns/op)\n", ops_per_sec, ns_per_op);
    printf("  Dummy: %u (prevent optimization)\n\n", dummy);
    
    /* Compare with frame_seek */
    printf("  Comparison:\n");
    printf("    frame_seek: ~5 ns/op (simple integer)\n");
    printf("    hyper resolve: %.1f ns/op (float math)\n", ns_per_op);
    printf("    ratio: %.1fx slower than frame_seek\n", ns_per_op / 5.0);
    
    printf("\n═══════════════════════════════════════════════════════════\n");
    return 0;
}
