/* test_entropy_tensor.c
 * Round-trip test on a synthetic tensor (array of State blocks) + file I/O.
 * Proves bit-exact losslessness and produces a real .bin artifact.
 */
#include "entropy_routing.c"

#include <stdio.h>
#include <stdlib.h>

#define N 1024
#define DX 3000
#define DY 4000

static uint32_t lcg = 12345u;
static int32_t rnd(void) {
    lcg = lcg * 1664525u + 1013904223u;
    return (int32_t)(lcg >> 1);
}

static int state_array_equal(const State *a, const State *b, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (!state_equal(&a[i], &b[i])) return 0;
    }
    return 1;
}

int main(void) {
    State *orig = (State*)malloc(N * sizeof(State));
    State *work = (State*)malloc(N * sizeof(State));
    int i, ok = 1;

    /* Fill with deterministic pseudo-random tensor data */
    for (i = 0; i < N; i++) {
        orig[i].quad[0] = rnd() % 1000;
        orig[i].quad[1] = rnd() % 1000;
        orig[i].quad[2] = rnd() % 1000;
        orig[i].quad[3] = rnd() % 1000;
        orig[i].void_entropy = rnd() % 1000;
    }

    /* Write original to .bin artifact */
    {
        FILE *f = fopen("tensor_orig.bin", "wb");
        if (!f) { printf("fopen orig failed\n"); return 1; }
        fwrite(orig, sizeof(State), N, f);
        fclose(f);
    }

    /* Test 1: shift_diagonal round-trip over whole tensor */
    for (i = 0; i < N; i++) work[i] = orig[i];
    for (i = 0; i < N; i++) shift_diagonal(&work[i], DX, DY);
    for (i = 0; i < N; i++) shift_diagonal(&work[i], -DX, -DY);
    {
        int pass = state_array_equal(work, orig, N);
        printf("tensor shift_diagonal round-trip (%d blocks): %s\n", N, pass ? "PASS" : "FAIL");
        if (!pass) ok = 0;
    }

    /* Test 2: shift_perpendicular round-trip over whole tensor */
    for (i = 0; i < N; i++) work[i] = orig[i];
    for (i = 0; i < N; i++) shift_perpendicular(&work[i], DX, DY);
    for (i = 0; i < N; i++) shift_perpendicular(&work[i], -DX, -DY);
    {
        int pass = state_array_equal(work, orig, N);
        printf("tensor shift_perpendicular round-trip (%d blocks): %s\n", N, pass ? "PASS" : "FAIL");
        if (!pass) ok = 0;
    }

    /* Test 3: reflect_quadrant self-inverse over whole tensor */
    for (i = 0; i < N; i++) work[i] = orig[i];
    for (i = 0; i < N; i++) reflect_quadrant(&work[i], 0, 3);
    for (i = 0; i < N; i++) reflect_quadrant(&work[i], 0, 3);
    {
        int pass = state_array_equal(work, orig, N);
        printf("tensor reflect_quadrant self-inverse (%d blocks): %s\n", N, pass ? "PASS" : "FAIL");
        if (!pass) ok = 0;
    }

    /* Write round-tripped result and byte-compare with original */
    for (i = 0; i < N; i++) work[i] = orig[i];
    for (i = 0; i < N; i++) shift_diagonal(&work[i], DX, DY);
    for (i = 0; i < N; i++) shift_diagonal(&work[i], -DX, -DY);
    {
        FILE *f = fopen("tensor_roundtrip.bin", "wb");
        if (!f) { printf("fopen rt failed\n"); return 1; }
        fwrite(work, sizeof(State), N, f);
        fclose(f);
    }
    {
        /* byte compare via fread both into arrays */
        State *ra = (State*)malloc(N * sizeof(State));
        State *rb = (State*)malloc(N * sizeof(State));
        int same = 0;
        FILE *fa = fopen("tensor_orig.bin", "rb");
        FILE *fb = fopen("tensor_roundtrip.bin", "rb");
        if (fa && fb) {
            size_t na = fread(ra, sizeof(State), N, fa);
            size_t nb = fread(rb, sizeof(State), N, fb);
            if (na == N && nb == N && state_array_equal(ra, rb, N)) same = 1;
            fclose(fa); fclose(fb);
        }
        free(ra); free(rb);
        printf("file byte-compare orig vs roundtrip: %s\n", same ? "IDENTICAL" : "DIFFER");
        if (!same) ok = 0;
    }

    printf("\nOVERALL: %s\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    free(orig); free(work);
    return ok ? 0 : 1;
}
