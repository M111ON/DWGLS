/* test_shifts.c — Ladder/Orthogonal/Positional lossless roundtrip proof.
 * BUILD: gcc -O2 -Wall -I. -Icore tests/test_shifts.c -o test_shifts.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/geo_shifts.h"

int main(void) {
    int pass = 0, fail = 0;
    uint32_t n = 64;
    int32_t *v = (int32_t *)malloc(n * sizeof(int32_t));
    int32_t *c = (int32_t *)malloc(n * sizeof(int32_t));
    int32_t *A = (int32_t *)malloc(n * sizeof(int32_t));
    int32_t *B = (int32_t *)malloc(n * sizeof(int32_t));
    int32_t *v2 = (int32_t *)malloc(n * sizeof(int32_t));
    int32_t *c2 = (int32_t *)malloc(n * sizeof(int32_t));

    for (uint32_t i = 0; i < n; i++) { v[i] = (int32_t)(i * 3 + 7); c[i] = (int32_t)(i % 5); }

    /* Ladder: expand then contract */
    {
        int32_t *big = (int32_t *)malloc(n * LADDER_FACTOR * sizeof(int32_t));
        ladder_expand(v, big, n);
        int32_t *out = (int32_t *)malloc(n * sizeof(int32_t));
        ladder_contract(big, out, n);
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) if (out[i] != v[i]) { ok = 0; break; }
        if (ok) { pass++; printf("ladder: PASS\n"); } else { fail++; printf("ladder: FAIL\n"); }
        free(big); free(out);
    }

    /* Orthogonal: split then merge (v,c same parity needed; use even v,c) */
    {
        for (uint32_t i = 0; i < n; i++) { v[i] = (int32_t)(i * 2); c[i] = (int32_t)((i % 4) * 2); }
        ortho_split(v, c, A, B, n);
        ortho_merge(A, B, v2, c2, n);
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) if (v2[i] != v[i] || c2[i] != c[i]) { ok = 0; break; }
        if (ok) { pass++; printf("ortho: PASS\n"); } else { fail++; printf("ortho: FAIL\n"); }
    }

    /* Positional Add: encode then decode */
    {
        for (uint32_t i = 0; i < n; i++) v[i] = (int32_t)(i * 7 + 13);
        posadd_encode(v, A, n, 5);
        posadd_decode(A, v2, n, 5);
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) if (v2[i] != v[i]) { ok = 0; break; }
        if (ok) { pass++; printf("posadd: PASS\n"); } else { fail++; printf("posadd: FAIL\n"); }
    }

    printf("shifts: %d pass, %d fail\n", pass, fail);
    free(v); free(c); free(A); free(B); free(v2); free(c2);
    return fail ? 1 : 0;
}
