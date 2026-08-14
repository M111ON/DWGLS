/*
 * beam_codec_test.c — Direct encode/decode test (no fp16)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define Q8_BLOCK_SZ 32
#define K_BITS 5
#define K_OFFSET 16

static void sort_floats(float *arr, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (arr[i] > arr[j]) { float t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
}

/* Pack bits */
static void pack_bits(uint8_t *out, const int *vals, int n, int bits) {
    int bit_pos = 0;
    for (int i = 0; i < n; i++) {
        int val = vals[i] & ((1 << bits) - 1);
        for (int b = 0; b < bits; b++) {
            int byte_idx = bit_pos / 8;
            int bit_idx = bit_pos % 8;
            if (val & (1 << b))
                out[byte_idx] |= (1 << bit_idx);
            bit_pos++;
        }
    }
}

static void unpack_bits(int *out, const uint8_t *data, int n, int bits, int shift) {
    int bit_pos = 0;
    for (int i = 0; i < n; i++) {
        int val = 0;
        for (int b = 0; b < bits; b++) {
            int byte_idx = bit_pos / 8;
            int bit_idx = bit_pos % 8;
            if (data[byte_idx] & (1 << bit_idx))
                val |= (1 << b);
            bit_pos++;
        }
        out[i] = val - shift;
    }
}

int main(void)
{
    printf("=== Direct Encode/Decode Test ===\n\n");

    /* Generate weights */
    float weights[32];
    srand(42);
    for (int i = 0; i < 32; i++)
        weights[i] = (float)((rand() % 256) - 128) * 0.01f;

    /* Sort */
    float sorted[32];
    for (int i = 0; i < 32; i++) sorted[i] = weights[i];
    sort_floats(sorted, 32);

    /* Compute mean, R */
    float sum = 0;
    for (int i = 0; i < 32; i++) sum += sorted[i];
    float mean = sum / 32;

    float sum_sq = 0;
    for (int i = 0; i < 32; i++) {
        float d = sorted[i] - mean;
        sum_sq += d * d;
    }
    float R = sqrtf(sum_sq / 32);

    printf("mean=%.6f  R=%.6f\n\n", mean, R);

    /* Encode: compute k and delta for each sorted weight */
    int k_vals[32];
    float deltas[32];
    float max_delta = 0;

    for (int i = 0; i < 32; i++) {
        float k = roundf((sorted[i] - mean) / R);
        k_vals[i] = (int)k;
        float ideal = mean + k * R;
        deltas[i] = sorted[i] - ideal;
        if (fabsf(deltas[i]) > max_delta)
            max_delta = fabsf(deltas[i]);
    }

    printf("max_delta=%.6f\n\n", max_delta);

    /* Show first 5 */
    printf("  i   sorted      k   k_stored  ideal       delta\n");
    for (int i = 0; i < 5; i++) {
        float ideal = mean + k_vals[i] * R;
        printf("  %2d  %8.4f  %3d  %3d     %8.4f  %8.6f\n",
               i, sorted[i], k_vals[i], k_vals[i] + K_OFFSET, ideal, deltas[i]);
    }
    printf("\n");

    /* Pack k values (5 bits each) */
    uint8_t k_packed[32 * K_BITS / 8 + 1];
    memset(k_packed, 0, sizeof(k_packed));
    int k_shifted[32];
    for (int i = 0; i < 32; i++) k_shifted[i] = k_vals[i] + K_OFFSET;
    pack_bits(k_packed, k_shifted, 32, K_BITS);

    /* Show packed bytes */
    printf("  k_shifted (first 5): ");
    for (int i = 0; i < 5; i++) printf("%d ", k_shifted[i]);
    printf("\n");

    /* Pack deltas (8 bits each) */
    uint8_t d_packed[32];
    memset(d_packed, 0, sizeof(d_packed));
    int d_quantized[32];
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;
    for (int i = 0; i < 32; i++) {
        int q = (int)roundf(deltas[i] / delta_scale * 127);
        if (q < -128) q = -128;
        if (q > 127) q = 127;
        d_quantized[i] = q + 128;  /* shift to 0..255 */
    }
    pack_bits(d_packed, d_quantized, 32, 8);

    /* Decode */
    int k_decoded[32];
    unpack_bits(k_decoded, k_packed, 32, K_BITS, K_OFFSET);

    int d_decoded_raw[32];
    unpack_bits(d_decoded_raw, d_packed, 32, 8, 128);  /* shift back */

    printf("  k_decoded (first 5): ");
    for (int i = 0; i < 5; i++) printf("%d ", k_decoded[i]);
    printf("\n");
    printf("  k_vals    (first 5): ");
    for (int i = 0; i < 5; i++) printf("%d ", k_vals[i]);
    printf("\n");
    printf("  d_decoded (first 5): ");
    for (int i = 0; i < 5; i++) printf("%d ", d_decoded_raw[i]);
    printf("\n");
    printf("  d_quant   (first 5): ");
    for (int i = 0; i < 5; i++) printf("%d ", d_quantized[i] - 128);
    printf("\n\n");

    float decoded[32];
    for (int i = 0; i < 32; i++) {
        float dq = (float)d_decoded_raw[i] * delta_scale / 127;
        decoded[i] = mean + (float)k_decoded[i] * R + dq;
    }

    /* Compare */
    printf("  i   original    decoded     err\n");
    int pass = 0;
    float total_err = 0;
    for (int i = 0; i < 32; i++) {
        float err = fabsf(sorted[i] - decoded[i]);
        total_err += err;
        if (err < 0.001f) pass++;
        if (i < 5)
            printf("  %2d  %8.4f  %8.4f  %8.6f%s\n",
                   i, sorted[i], decoded[i], err, err < 0.001f ? " ✓" : "");
    }

    printf("\n  PASS: %d/32  total_err=%.6f\n\n", pass, total_err);

    /* Storage */
    int k_bytes = (32 * K_BITS + 7) / 8;
    int total = 6 + k_bytes + 32;  /* mean(2) + R(2) + max_delta(2) + k + deltas */
    printf("  Storage: %d bytes/block\n", total);
    printf("  vs Q8_0: 34 bytes/block\n");
    printf("  Ratio: %.4fx\n", (double)total / 34.0);

    return 0;
}
