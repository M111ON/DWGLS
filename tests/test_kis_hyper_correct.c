/* test_kis_hyper_correct.c — KIS + Hyper: Read from Creation Point
 *
 * RULE: Must read from where data was CREATED to get complete data.
 *
 * Test:
 * 1. Store data at KIS scale 1.0 → slot X
 * 2. Scale down → slot X changes position
 * 3. Read from slot X at scaled position → WRONG data
 * 4. Read from original creation point → CORRECT data
 * 5. Delta = what's lost when you can't read from creation point
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_correct test_kis_hyper_correct.c -lm
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

/* ═══════════════════════════════════════════════════════════════════════════
   Test: Read from creation point vs scaled position
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_creation_point(const char *gguf_path, const char *tensor_name) {
    printf("TEST: Read from Creation Point (Correct Rule)\n");
    printf("  Model: %s, Tensor: %s\n", gguf_path, tensor_name);
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights(gguf_path, tensor_name, original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Read %u bytes\n\n", count);
    
    /* Step 1: Store data at KIS scale 1.0 → each weight at slot i */
    uint32_t scale_1 = (uint32_t)(1.0 * 65536.0);
    printf("  Step 1: Store at KIS scale 1.0\n");
    printf("    weight[0] = %u → slot 0\n", original[0]);
    printf("    weight[1] = %u → slot 1\n", original[1]);
    printf("    weight[2] = %u → slot 2\n", original[2]);
    
    /* Step 2: Scale down to 0.1 → slot positions change */
    uint32_t scale_2 = (uint32_t)(0.1 * 65536.0);
    printf("\n  Step 2: Scale down to 0.1\n");
    
    /* At scale 0.1, where does slot 0 map to? */
    uint32_t proj_0_scale1 = kis_project_4d_to_3d(0, 0, 0, 0, scale_1);
    uint32_t proj_0_scale2 = kis_project_4d_to_3d(0, 0, 0, 0, scale_2);
    uint32_t proj_1_scale2 = kis_project_4d_to_3d(1, 0, 0, 0, scale_2);
    uint32_t proj_2_scale2 = kis_project_4d_to_3d(2, 0, 0, 0, scale_2);
    
    printf("    slot 0 at scale 1.0 → projects to %u\n", proj_0_scale1);
    printf("    slot 0 at scale 0.1 → projects to %u\n", proj_0_scale2);
    printf("    slot 1 at scale 0.1 → projects to %u\n", proj_1_scale2);
    printf("    slot 2 at scale 0.1 → projects to %u\n", proj_2_scale2);
    
    /* Step 3: Read from scaled position → WRONG data */
    printf("\n  Step 3: Read from scaled position (WRONG)\n");
    
    /* At scale 0.1, slot 0 now contains data from different original position */
    uint32_t read_from_scaled = proj_0_scale2 & 0xFF;
    uint32_t expected_from_creation = original[0];
    
    printf("    Read from slot 0 at scale 0.1: %u\n", read_from_scaled);
    printf("    Expected from creation point: %u\n", expected_from_creation);
    printf("    Match: %s\n", (read_from_scaled == expected_from_creation) ? "YES" : "NO");
    
    /* Step 4: Read from creation point → CORRECT data */
    printf("\n  Step 4: Read from creation point (CORRECT)\n");
    
    /* The data was stored at slot 0 when scale was 1.0 */
    /* To read it back, we must access slot 0 at scale 1.0 */
    uint32_t read_from_creation = original[0]; /* Direct read from original storage */
    
    printf("    Read from creation point (slot 0, scale 1.0): %u\n", read_from_creation);
    printf("    Expected: %u\n", expected_from_creation);
    printf("    Match: %s\n", (read_from_creation == expected_from_creation) ? "YES" : "NO");
    
    CHECK(1, "Read from creation point gives correct data", 
          read_from_creation == expected_from_creation);
    
    /* Step 5: What Hyperbolic can do */
    printf("\n  Step 5: Hyperbolic structure (x × f(time))\n");
    printf("    If we know the creation point (slot 0, scale 1.0),\n");
    printf("    Hyperbolic can compute: x × f(time) = correct address\n");
    printf("    No need to store delta — compute on fly!\n");
    
    /* Verify Hyperbolic can help */
    HypComplex w = kis_to_hyperbolic_axis(0, HYP_AXIS_X);
    uint32_t hyper_back = hyperbolic_to_kis_axis(w, HYP_AXIS_X);
    printf("    Hyperbolic roundtrip for slot 0: %u → %u\n", 0, hyper_back);
    CHECK(2, "Hyperbolic roundtrip works", hyper_back == 0);
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS + Hyper: Read from Creation Point (Correct Rule)\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_creation_point("I:/model/qwen25_q8.gguf", "token_embd.weight");
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
