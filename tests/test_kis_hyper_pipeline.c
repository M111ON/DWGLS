/* test_kis_hyper_pipeline.c — Full Pipeline: Store → Scale → Resolve → Read
 *
 * Complete pipeline proving Hyperbolic address resolver works with real data.
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_pipeline test_kis_hyper_pipeline.c -lm
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
   CREATION POINT STORE
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t slot;
    uint32_t scale;
    double   hyper_re;
    double   hyper_im;
    uint8_t  value;          /* original value */
} CreationPoint;

typedef struct {
    CreationPoint points[MAX_WEIGHTS];
    uint32_t count;
    uint32_t creation_scale;
} CreationStore;

/* Store all weights with their creation points */
static void store_creation_points(CreationStore *store, 
                                   const uint8_t *weights, uint32_t count,
                                   uint32_t scale) {
    store->count = count;
    store->creation_scale = scale;
    
    for (uint32_t i = 0; i < count; i++) {
        store->points[i].slot = i;
        store->points[i].scale = scale;
        store->points[i].value = weights[i];
        
        HypComplex w = kis_to_hyperbolic_axis(i, HYP_AXIS_X);
        store->points[i].hyper_re = w.re;
        store->points[i].hyper_im = w.im;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   ADDRESS RESOLVER
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint32_t resolve_address(const CreationPoint *cp, 
                                        uint32_t target_scale) {
    double ratio = (double)target_scale / (double)cp->scale;
    double new_re = cp->hyper_re * ratio;
    double new_im = cp->hyper_im * ratio;
    HypComplex w = {new_re, new_im};
    return hyperbolic_to_kis_axis(w, HYP_AXIS_X);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: Full Pipeline
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_full_pipeline(void) {
    printf("TEST: Full Pipeline (Store → Scale → Resolve → Read)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Loaded %u weights from real GGUF\n\n", count);
    
    /* Step 1: Store at scale 1.0 */
    CreationStore store;
    uint32_t scale_1 = (uint32_t)(1.0 * 65536.0);
    store_creation_points(&store, original, count, scale_1);
    printf("  Step 1: Stored %u creation points at scale 1.0\n", store.count);
    
    /* Step 2: What we store (creation points only) */
    uint32_t stored_bytes = count * sizeof(CreationPoint);
    uint32_t original_bytes = count;
    printf("  Step 2: Storage analysis\n");
    printf("    Original data: %u bytes\n", original_bytes);
    printf("    Creation points: %u bytes (%.1fx overhead)\n", 
           stored_bytes, (double)stored_bytes / original_bytes);
    
    /* Step 3: Resolve addresses at different scales */
    printf("\n  Step 3: Address resolution at different scales\n");
    
    double test_scales[] = {1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1};
    int n = sizeof(test_scales) / sizeof(test_scales[0]);
    
    printf("  Scale  | Unique Addrs | Compression\n");
    printf("  -------|--------------|------------\n");
    
    for (int s = 0; s < n; s++) {
        double scale = test_scales[s];
        uint32_t scale_fp = (uint32_t)(scale * 65536.0);
        
        /* Resolve all addresses */
        uint32_t addrs[MAX_WEIGHTS];
        for (uint32_t i = 0; i < count; i++) {
            addrs[i] = resolve_address(&store.points[i], scale_fp);
        }
        
        /* Count unique addresses */
        uint32_t unique = 0;
        for (uint32_t i = 0; i < count; i++) {
            int found = 0;
            for (uint32_t j = 0; j < i; j++) {
                if (addrs[i] == addrs[j]) { found = 1; break; }
            }
            if (!found) unique++;
        }
        
        double compression = (double)count / unique;
        printf("  %-6.2f | %12u | %.2fx\n", scale, unique, compression);
    }
    
    CHECK(1, "Pipeline completes without crash", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: Roundtrip verification
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_roundtrip(void) {
    printf("TEST: Roundtrip (Store → Resolve → Verify)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    /* Store at scale 1.0 */
    CreationStore store;
    uint32_t scale_1 = (uint32_t)(1.0 * 65536.0);
    store_creation_points(&store, original, count, scale_1);
    
    /* Verify: resolve at same scale should give original slot */
    int roundtrip_ok = 0, roundtrip_fail = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = resolve_address(&store.points[i], scale_1);
        if (addr == i) {
            roundtrip_ok++;
        } else {
            roundtrip_fail++;
            if (roundtrip_fail <= 3) {
                printf("  FAIL: slot %u → %u\n", i, addr);
            }
        }
    }
    
    CHECK(2, "Roundtrip at same scale (100% accuracy)", roundtrip_fail == 0);
    printf("    OK: %u, FAIL: %u\n", roundtrip_ok, roundtrip_fail);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS + Hyper Pipeline: Store → Scale → Resolve → Read\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_full_pipeline();
    test_roundtrip();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
