/* test_dim_bridge.c — Variable<->Value swap bijection proof.
 * BUILD: gcc -O2 -Wall -I. -Icore tests/test_dim_bridge.c -o test_dim_bridge.exe
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "core/geo_dim_bridge.h"

int main(void) {
    int pass = 0, fail = 0;

    /* 1. all 20736 roundtrip */
    int all_ok = 1;
    for (uint32_t x = 0; x < DIM_FIELD; x++) {
        if (!dim_bridge_roundtrip_ok(x)) { all_ok = 0; if (fail < 5) printf("FAIL rt %u\n", x); fail++; }
    }
    if (all_ok) { pass++; printf("roundtrip: %u/%u OK\n", DIM_FIELD, DIM_FIELD); }
    else { printf("roundtrip FAIL\n"); }

    /* 2. swap is a permutation (distinct outputs) */
    char *seen = (char *)calloc(DIM_FIELD, 1);
    int distinct = 1, cnt = 0;
    for (uint32_t x = 0; x < DIM_FIELD; x++) {
        uint32_t y = dim_swap_var_to_val(x);
        if (seen[y]) { distinct = 0; }
        seen[y] = 1; cnt++;
    }
    if (distinct && cnt == DIM_FIELD) { pass++; printf("permutation: distinct %d OK\n", cnt); }
    else { fail++; printf("FAIL permutation distinct=%d\n", cnt); }
    free(seen);

    /* 3. swap involution on sample */
    uint32_t samples[] = {0, 1, 100, 20735, 12345};
    int inv_ok = 1;
    for (int i = 0; i < 5; i++) {
        uint32_t v = samples[i];
        uint32_t y = dim_swap_var_to_val(v);
        uint32_t z = dim_swap_val_to_var(y);
        if (z != v) { inv_ok = 0; printf("FAIL involution %u -> %u -> %u\n", v, y, z); }
    }
    if (inv_ok) { pass++; printf("involution samples OK\n"); } else { fail++; }

    printf("dim_bridge: %d pass, %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
