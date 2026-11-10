/* test_entropy_model.c
 * Round-trip Constrained Entropy Routing on a REAL model tensor.
 * Uses multilingual-e5-small (F32 tensor: token_embd_norm.weight).
 * Core lib stays integer-only; test reinterprets raw F32 bytes as int32.
 */
#include "entropy_routing.c"
#include "core/gguf_reader.h"

#include <stdio.h>
#include <stdlib.h>

#define MODEL "I:/model/multilingual-e5-small-q8_0-fixed.gguf"
#define TENSOR_IDX 3   /* token_embd_norm.weight, F32, 1536 bytes */

int main(void) {
    GgufReader r;
    if (gguf_open(MODEL, &r) != 0) { printf("open fail\n"); return 1; }
    printf("model: %s\n", MODEL);
    printf("tensor[%d]: %s (dtype=%u, %u bytes)\n",
           TENSOR_IDX, r.names[TENSOR_IDX], r.dtypes[TENSOR_IDX], r.sizes[TENSOR_IDX]);

    uint8_t *buf = (uint8_t*)malloc(r.sizes[TENSOR_IDX]);
    if (gguf_read_tensor(MODEL, &r, TENSOR_IDX, buf, r.sizes[TENSOR_IDX]) != 0) {
        printf("read tensor fail\n"); gguf_close(&r); return 1;
    }

    int n_ints = r.sizes[TENSOR_IDX] / 4;
    int n_states = n_ints / 5;   /* 5 int32 per State */
    if (n_states == 0) { printf("tensor too small\n"); return 1; }

    State *orig = (State*)malloc(n_states * sizeof(State));
    State *work = (State*)malloc(n_states * sizeof(State));
    int i;
    /* reinterpret raw bytes as int32 array -> State blocks */
    int32_t *raw = (int32_t*)buf;
    for (i = 0; i < n_states; i++) {
        orig[i].quad[0] = raw[i*5 + 0];
        orig[i].quad[1] = raw[i*5 + 1];
        orig[i].quad[2] = raw[i*5 + 2];
        orig[i].quad[3] = raw[i*5 + 3];
        orig[i].void_entropy = raw[i*5 + 4];
    }

    int ok = 1;

    /* shift_diagonal round-trip */
    for (i = 0; i < n_states; i++) work[i] = orig[i];
    for (i = 0; i < n_states; i++) shift_diagonal(&work[i], 3000, 4000);
    for (i = 0; i < n_states; i++) shift_diagonal(&work[i], -3000, -4000);
    {
        int pass = 1;
        for (i = 0; i < n_states; i++) if (!state_equal(&work[i], &orig[i])) { pass = 0; break; }
        printf("real-model shift_diagonal round-trip (%d states): %s\n", n_states, pass ? "PASS" : "FAIL");
        if (!pass) ok = 0;
    }

    /* shift_perpendicular round-trip */
    for (i = 0; i < n_states; i++) work[i] = orig[i];
    for (i = 0; i < n_states; i++) shift_perpendicular(&work[i], 3000, 4000);
    for (i = 0; i < n_states; i++) shift_perpendicular(&work[i], -3000, -4000);
    {
        int pass = 1;
        for (i = 0; i < n_states; i++) if (!state_equal(&work[i], &orig[i])) { pass = 0; break; }
        printf("real-model shift_perpendicular round-trip (%d states): %s\n", n_states, pass ? "PASS" : "FAIL");
        if (!pass) ok = 0;
    }

    /* reflect self-inverse */
    for (i = 0; i < n_states; i++) work[i] = orig[i];
    for (i = 0; i < n_states; i++) reflect_quadrant(&work[i], 0, 3);
    for (i = 0; i < n_states; i++) reflect_quadrant(&work[i], 0, 3);
    {
        int pass = 1;
        for (i = 0; i < n_states; i++) if (!state_equal(&work[i], &orig[i])) { pass = 0; break; }
        printf("real-model reflect_quadrant self-inverse (%d states): %s\n", n_states, pass ? "PASS" : "FAIL");
        if (!pass) ok = 0;
    }

    printf("\nOVERALL: %s\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    free(buf); free(orig); free(work);
    gguf_close(&r);
    return ok ? 0 : 1;
}
