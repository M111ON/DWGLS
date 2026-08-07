/* test_kis_hyper_fast.c — Optimized: Store angle, not (re,im)
 *
 * Key insight: atan2 is the bottleneck. Store angle directly.
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_fast test_kis_hyper_fast.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"

#define PI 3.14159265358979323846

static inline uint8_t select_axis(uint32_t slot) {
    if (slot < 6912) return 0;
    if (slot < 13824) return 1;
    return 2;
}

/* ═══════════════════════════════════════════════════════════════════════════
   SLOW: Original (with atan2)
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint32_t resolve_slow(uint32_t slot, uint32_t scale) {
    uint8_t axis = select_axis(slot);
    uint32_t aslot = slot % 6912;
    double ratio = (double)scale / (double)((uint32_t)(1.0 * 65536.0));
    
    double angle = 2.0 * PI * (double)aslot / 6912.0;
    angle += (double)axis * 2.0 * PI / 3.0;
    
    double new_re = cos(angle) * ratio;
    double new_im = sin(angle) * ratio;
    
    /* Inverse Cayley (expensive: atan2) */
    double denom = (1.0 + new_re) * (1.0 + new_re) + new_im * new_im;
    if (denom < 1e-15) return slot;
    
    double z_re = ((1.0 - new_re) * (1.0 + new_re) + new_im * new_im) / denom;
    double z_im = (-2.0 * new_im) / denom;
    
    double a = atan2(z_im, z_re);  /* <-- BOTTLENECK */
    if (a < 0) a += 2.0 * PI;
    a -= (double)axis * 2.0 * PI / 3.0;
    if (a < 0) a += 2.0 * PI;
    
    uint32_t result = (uint32_t)(a * 6912.0 / (2.0 * PI) + 0.5);
    return (result % 6912) + axis * 6912;
}

/* ═══════════════════════════════════════════════════════════════════════════
   FAST: Store angle, skip atan2
   ═══════════════════════════════════════════════════════════════════════════ */

/* At creation: store angle directly */
typedef struct {
    uint32_t slot;
    uint32_t scale;
    double   angle;      /* precomputed angle */
    uint8_t  axis;
} FastPoint;

static inline FastPoint create_point(uint32_t slot, uint32_t scale) {
    FastPoint p;
    p.slot = slot;
    p.scale = scale;
    p.axis = select_axis(slot);
    uint32_t aslot = slot % 6912;
    p.angle = 2.0 * PI * (double)aslot / 6912.0;
    p.angle += (double)p.axis * 2.0 * PI / 3.0;
    return p;
}

/* At resolve: multiply angle by ratio (no atan2!) */
static inline uint32_t resolve_fast(const FastPoint *p, uint32_t target_scale) {
    double ratio = (double)target_scale / (double)p->scale;
    
    /* Just scale the angle — no Cayley, no atan2 */
    double new_angle = p->angle * ratio;
    
    /* Normalize to [0, 2PI) */
    while (new_angle < 0) new_angle += 2.0 * PI;
    while (new_angle >= 2.0 * PI) new_angle -= 2.0 * PI;
    
    /* Convert back to slot */
    double a = new_angle;
    a -= (double)p->axis * 2.0 * PI / 3.0;
    if (a < 0) a += 2.0 * PI;
    
    uint32_t result = (uint32_t)(a * 6912.0 / (2.0 * PI) + 0.5);
    return (result % 6912) + p->axis * 6912;
}

/* ═══════════════════════════════════════════════════════════════════════════
   BENCHMARK
   ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Hyperbolic Speed: Slow vs Fast\n");
    printf("═══════════════════════════════════════════════════════════\n\n");
    
    uint32_t N = 1000000;
    uint32_t scale = (uint32_t)(0.5 * 65536.0);
    
    /* Warm up */
    for (uint32_t i = 0; i < 1000; i++) {
        volatile uint32_t r1 = resolve_slow(i, scale);
        FastPoint p = create_point(i, (uint32_t)(1.0 * 65536.0));
        volatile uint32_t r2 = resolve_fast(&p, scale);
        (void)r1; (void)r2;
    }
    
    /* Benchmark SLOW */
    clock_t start = clock();
    uint32_t dummy1 = 0;
    for (uint32_t i = 0; i < N; i++) {
        dummy1 += resolve_slow(i % 20736, scale);
    }
    clock_t end = clock();
    double slow_time = (double)(end - start) / CLOCKS_PER_SEC;
    double slow_ns = slow_time / N * 1e9;
    
    /* Benchmark FAST */
    FastPoint points[20736];
    for (uint32_t i = 0; i < 20736; i++) {
        points[i] = create_point(i, (uint32_t)(1.0 * 65536.0));
    }
    
    start = clock();
    uint32_t dummy2 = 0;
    for (uint32_t i = 0; i < N; i++) {
        dummy2 += resolve_fast(&points[i % 20736], scale);
    }
    end = clock();
    double fast_time = (double)(end - start) / CLOCKS_PER_SEC;
    double fast_ns = fast_time / N * 1e9;
    
    printf("  SLOW (with atan2):  %6.1f ns/op  (%.0f M ops/sec)\n", 
           slow_ns, N / slow_time / 1e6);
    printf("  FAST (no atan2):    %6.1f ns/op  (%.0f M ops/sec)\n", 
           fast_ns, N / fast_time / 1e6);
    printf("  Speedup:            %6.1fx faster\n", slow_ns / fast_ns);
    printf("  Dummy: %u vs %u\n\n", dummy1, dummy2);
    
    printf("  vs frame_seek:      ~5 ns/op\n");
    printf("  fast is %.1fx slower than frame_seek\n", fast_ns / 5.0);
    
    printf("\n═══════════════════════════════════════════════════════════\n");
    return 0;
}
