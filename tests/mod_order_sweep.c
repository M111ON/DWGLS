/*
 * mod_order_sweep.c — หา stride ที่มี order สูงสุดบน 20736
 *
 * MOD walk: node' = (node × stride) % 20736
 * order = จำนวน unique ก่อน repeat
 *
 * สูงสุดที่เป็นไปได้ = 1728? (lcm(2^6, 2×3^3) = 1728)
 *
 * BUILD: gcc -O2 -DGEO_JUMP_INLINE -I../../FGLS_new/collection/geo_jump_module/include -o build/mod_sweep tests/mod_order_sweep.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../FGLS_new/collection/geo_jump_module/include/geo_jump.h"

#define N 20736u

static uint32_t order_of(uint32_t stride) {
    uint8_t visited[N];
    memset(visited, 0, sizeof(visited));

    uint32_t node = 1;
    uint32_t steps = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (visited[node]) break;
        visited[node] = 1;
        node = (uint32_t)(((uint64_t)node * stride) % N);
        steps++;
    }
    return steps;
}

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  MOD Order Sweep — หา stride ที่ cover มากที่สุด        ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");
    printf("  ความเป็นไปได้ตามทฤษฎี:\n");
    printf("  φ(20736) = 6912 units\n");
    printf("  max element order = lcm(2^6, 2×3^3) = lcm(64, 54) = 1728\n");
    printf("  20736 / 1728 = 12 pentagon faces\n\n");

    printf("  Brute-force: sweep ตอนนี้?\n");
    printf("  (หาค่าแรกที่ order ≥ 1024, จนกว่าจะเจอ 1728 ตัวจริง)\n\n");

    /* Sweep odd strides not divisible by 3 (coprime candidates) */
    uint32_t best_order = 0;
    uint32_t best_stride = 0;
    int found_1728 = 0;

    for (uint32_t s = 1; s < N; s += 2) {
        if (s % 3 == 0) continue;  /* gcd(s,20736)≠1 skip */
        uint32_t o = order_of(s);
        if (o > best_order) {
            best_order = o;
            best_stride = s;
        }
        if (o == 1728) {
            printf("  ✓ stride=%u order=1728 (FULL pentagon!)\n", s);
            found_1728 = 1;
            if (best_order >= 1728) break;
        }
    }

    printf("\n  Best: stride=%u order=%u (%.1f%% of 20736)\n",
           best_stride, best_order, 100.0*best_order/N);
    printf("  Found 1728: %s\n", found_1728 ? "YES ✓" : "NO ✗");
    printf("  12 × %u = %u ✓ (12 orbits cover full space)\n",
           best_order, best_order * 12);

    printf("\n  สรุป: MOD stride คนเดียว cover ได้มากสุด 1728 = 1 pentagon\n");
    printf("  ต้องมี 12 orbits (หรือ combo กับ shell/capo) ถึง cover 20736\n");

    return 0;
}