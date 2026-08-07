/* test_kis_hyper_threshold.c — Find optimal threshold for KIS ↔ Hyper
 *
 * Test different scale values to find where delta is smallest.
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_threshold test_kis_hyper_threshold.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"
#include "gguf_reader.h"

#define MAX_WEIGHTS 20736

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static int read_q8_weights(const char *path, const char *name,
                            uint8_t *w, uint32_t max, uint32_t *count) {
    GgufReader r;
    if (gguf_open(path, &r) != 0) return -1;
    int f = -1;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        if (r.names[i] && strcmp(r.names[i], name) == 0) { f = i; break; }
    }
    if (f < 0) { gguf_close(&r); return -1; }
    uint32_t sz = r.sizes[f] > max ? max : r.sizes[f];
    FILE *fp = fopen(path, "rb");
    _fseeki64(fp, r.data_offset + r.offsets[f], SEEK_SET);
    *count = (uint32_t)fread(w, 1, sz, fp);
    fclose(fp);
    gguf_close(&r);
    return 0;
}

static void test_threshold_sweep(const char *gguf_path, const char *tensor_name) {
    printf("TEST: Threshold Sweep — find optimal KIS scale\n");
    printf("  Model: %s, Tensor: %s\n", gguf_path, tensor_name);
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights(gguf_path, tensor_name, original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Read %u bytes\n\n", count);
    
    /* Test different scales */
    double scales[] = {
        1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 
        0.4, 0.3, 0.2, 0.1, 0.05, 0.01
    };
    int n = sizeof(scales) / sizeof(scales[0]);
    
    printf("  Scale   | Delta non-zero | Delta %  | KIS captures %%\n");
    printf("  --------|----------------|----------|----------------\n");
    
    for (int s = 0; s < n; s++) {
        double scale = scales[s];
        uint32_t scale_fp = (uint32_t)(scale * 65536.0);
        
        int delta_nonzero = 0;
        int kis_captures = 0;
        
        for (uint32_t i = 0; i < count; i++) {
            uint32_t proj = kis_project_4d_to_3d(i, 0, 0, 0, scale_fp);
            uint8_t kis_val = (uint8_t)(proj & 0xFF);
            uint8_t diff = (uint8_t)((int)original[i] - (int)kis_val);
            
            if (diff != 0) delta_nonzero++;
            if (kis_val == original[i]) kis_captures++;
        }
        
        printf("  %-7.2f | %14d | %6.1f%% | %12.1f%%\n",
               scale, delta_nonzero, 
               (delta_nonzero * 100.0 / count),
               (kis_captures * 100.0 / count));
    }
    printf("\n");
}

int main(void) {
    printf("KIS ↔ Hyper Threshold Finding\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_threshold_sweep("I:/model/qwen25_q8.gguf", "token_embd.weight");
    test_threshold_sweep("I:/model/SmolLM2-360M-Instruct.Q8_0.gguf", "token_embd.weight");
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
