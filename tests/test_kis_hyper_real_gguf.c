/* test_kis_hyper_real_gguf.c — KIS + Hyper Delta on Real GGUF Weights
 *
 * Test with real GGUF model weights to see if delta is smaller than original.
 *
 * BUILD: gcc -O2 -I../core -I../../FGLS_new/runner -o test_kis_hyper_real_gguf test_kis_hyper_real_gguf.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"
#include "gguf_reader.h"

#define MAX_WEIGHTS 20736  /* KIS address space */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   Read Q8_0 weights from GGUF tensor
   ═══════════════════════════════════════════════════════════════════════════ */
static int read_q8_weights(const char *gguf_path, const char *tensor_name,
                            uint8_t *weights, uint32_t max_weights, uint32_t *count) {
    GgufReader reader;
    if (gguf_open(gguf_path, &reader) != 0) {
        printf("  ERROR: Cannot open %s\n", gguf_path);
        return -1;
    }
    
    /* Find tensor */
    int found = -1;
    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        if (reader.names[i] && strcmp(reader.names[i], tensor_name) == 0) {
            found = i;
            break;
        }
    }
    
    if (found < 0) {
        printf("  ERROR: Tensor '%s' not found\n", tensor_name);
        printf("  Available tensors:\n");
        for (uint32_t i = 0; i < reader.n_tensors && i < 10; i++) {
            printf("    %s\n", reader.names[i] ? reader.names[i] : "(null)");
        }
        gguf_close(&reader);
        return -1;
    }
    
    /* Read tensor data */
    uint32_t size = reader.sizes[found];
    uint64_t offset = reader.offsets[found];
    
    if (size > max_weights) size = max_weights;
    
    FILE *f = fopen(gguf_path, "rb");
    if (!f) { gguf_close(&reader); return -1; }
    
    _fseeki64(f, reader.data_offset + offset, SEEK_SET);
    *count = (uint32_t)fread(weights, 1, size, f);
    fclose(f);
    
    gguf_close(&reader);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Test: KIS + Hyper Delta on Real GGUF
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_real_gguf_delta(const char *gguf_path, const char *tensor_name) {
    printf("TEST: KIS + Hyper Delta on Real GGUF\n");
    printf("  Model: %s\n", gguf_path);
    printf("  Tensor: %s\n", tensor_name);
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Read real weights */
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    
    if (read_q8_weights(gguf_path, tensor_name, original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP: Cannot read tensor\n");
        return;
    }
    
    printf("  Read %u bytes from tensor\n", count);
    if (count == 0) { printf("  SKIP: Empty tensor\n"); return; }
    
    /* KIS projection at LARGE scale (coarse) */
    uint32_t scale_large = (uint32_t)(1.0 * 65536.0);
    uint32_t kis_coarse[MAX_WEIGHTS];
    for (uint32_t i = 0; i < count; i++) {
        kis_coarse[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale_large);
    }
    
    /* KIS projection at SMALL scale (PLATEAU) */
    uint32_t scale_small = (uint32_t)(0.00001 * 65536.0);
    uint32_t kis_small[MAX_WEIGHTS];
    for (uint32_t i = 0; i < count; i++) {
        kis_small[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale_small);
    }
    
    /* Calculate delta */
    uint8_t delta[MAX_WEIGHTS];
    for (uint32_t i = 0; i < count; i++) {
        int diff = (int)original[i] - (int)(kis_small[i] & 0xFF);
        delta[i] = (uint8_t)(diff & 0xFF);
    }
    
    /* Store delta in Hyperbolic */
    uint8_t hyper_delta[MAX_WEIGHTS];
    for (uint32_t slot = 0; slot < count && slot < 6912; slot++) {
        HypComplex w = kis_to_hyperbolic_axis(slot, HYP_AXIS_X);
        hyper_delta[slot] = (uint8_t)((int)(w.re * 100) & 0xFF);
    }
    
    /* Verify roundtrip */
    int match = 1;
    int mismatches = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t kis_val = kis_coarse[i] & 0xFF;
        uint8_t d = delta[i];
        uint8_t recovered = (uint8_t)((kis_val + d) & 0xFF);
        if (recovered != original[i]) {
            match = 0;
            mismatches++;
        }
    }
    
    CHECK(1, "KIS + delta = original (real GGUF)", match);
    printf("    Mismatches: %d / %u\n", mismatches, count);
    
    /* Measure delta size vs original */
    int delta_nonzero = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (delta[i] != 0) delta_nonzero++;
    }
    
    printf("  Delta analysis:\n");
    printf("    Original size: %u bytes\n", count);
    printf("    Delta non-zero: %d bytes (%.1f%%)\n", 
           delta_nonzero, (delta_nonzero * 100.0 / count));
    printf("    Delta zero: %d bytes (%.1f%%)\n",
           count - delta_nonzero, ((count - delta_nonzero) * 100.0 / count));
    
    /* Sample */
    printf("\n  Sample (first 10):\n");
    printf("  i  | orig | kis_coarse | kis_small | delta | recovered\n");
    printf("  ---|------|------------|-----------|-------|----------\n");
    for (int i = 0; i < 10 && i < (int)count; i++) {
        uint32_t kis_c = kis_coarse[i] & 0xFF;
        uint32_t kis_s = kis_small[i] & 0xFF;
        uint8_t d = delta[i];
        uint8_t rec = (uint8_t)((kis_c + d) & 0xFF);
        printf("  %2d | %4u | %10u | %9u | %5u | %9u\n",
               i, original[i], kis_c, kis_s, d, rec);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS + Hyper Delta on Real GGUF Weights\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    /* Test with available GGUF models */
    const char *models[] = {
        "I:/model/qwen25_q8.gguf",
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Kokoro_no_espeak_Q8.gguf"
    };
    const char *tensors[] = {
        "token_embd.weight",
        "token_embd.weight",
        "token_embd.weight"
    };
    
    int n = sizeof(models) / sizeof(models[0]);
    for (int i = 0; i < n; i++) {
        test_real_gguf_delta(models[i], tensors[i]);
    }
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    
    return fail == 0 ? 0 : 1;
}
