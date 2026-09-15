/* test_geo_jump_roles.c — PROOF: projections (walls) vs movers.
 * HILBERT/PEANO/GROUND/PENTAGON must be idempotent (jump^2 = jump): they
 * canonicalize a node onto its region wall — structure stays still.
 * MOD/INVERT must NOT be idempotent: they move data.
 * BUILD: gcc -O2 -Wall -DGEO_JUMP_INLINE -Icore -I../FGLS_new/collection/geo_jump_module/include -o /tmp/gjc_roles tests/test_geo_jump_roles.c
 */
#include <stdio.h>
#include <stdint.h>
#include "../../FGLS_new/collection/geo_jump_module/include/geo_jump.h"

#define N 20736u

static uint32_t idem(GeoJumpType t, uint32_t p) {
    uint32_t n = 0;
    for (uint32_t x = 0; x < N; x++) {
        uint32_t j1 = geo_jump(x, t, p);
        if (geo_jump(j1, t, p) == j1) n++;
    }
    return n;
}

static uint32_t orbit(GeoJumpType t, uint32_t p, uint32_t start) {
    uint32_t node = geo_jump(start, t, p), k = 1;
    while (node != start && k <= N) { node = geo_jump(node, t, p); k++; }
    return (node == start) ? k : 0;
}

int main(void) {
    printf("WALLS (idempotent count / %u):\n", N);
    printf("  HILBERT(1):  %u\n", idem(JUMP_HILBERT, 1));
    printf("  PEANO(1):    %u\n", idem(JUMP_PEANO, 1));
    printf("  GROUND(1):   %u\n", idem(JUMP_GROUND, 1));
    printf("  PENTAGON(0): %u\n", idem(JUMP_PENTAGON, 0));
    printf("  PENTAGON(5): %u\n", idem(JUMP_PENTAGON, 5));
    printf("MOVERS (orbit length from node 1):\n");
    printf("  MOD-37:  %u\n", orbit(JUMP_MOD, 37, 1));
    printf("  MOD-5:   %u\n", orbit(JUMP_MOD, 5, 1));
    printf("  INVERT-0:%u\n", orbit(JUMP_INVERT, 0, 1));
    printf("  CAPO(+1):%u\n", orbit(JUMP_CAPO, 0, 1));
    return 0;
}
