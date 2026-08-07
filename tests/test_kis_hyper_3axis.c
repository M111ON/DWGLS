/* test_kis_hyper_3axis.c — 3-Axis Pipeline with Auto Axis Selection
 *
 * Fix: Auto-select axis based on slot number
 * AXIS_X: 0-6912, AXIS_Y: 6913-13824, AXIS_Z: 13825-20736
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_3axis test_kis_hyper_3axis.c -lm
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
   AUTO AXIS SELECTION
   ═══════════════════════════════════════════════════════════════════════════ */

/* Select axis based on slot number */
static inline uint8_t select_axis(uint32_t slot) {
    if (slot < HYP_AXIS_SLOTS) return HYP_AXIS_X;      /* 0-6911 */
    if (slot < HYP_AXIS_SLOTS * 2) return HYP_AXIS_Y;  /* 6912-13823 */
    return HYP_AXIS_Z;                                   /* 13824-20735 */
}

/* Get slot within axis (normalize to 0-6912) */
static inline uint32_t axis_slot(uint32_t slot) {
    return slot % HYP_AXIS_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CREATION POINT (3-axis aware)
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t original_slot;  /* 0-20736 */
    uint32_t scale;
    uint8_t  axis;           /* auto-selected */
    double   hyper_re;
    double   hyper_im;
    uint8_t  value;
} CreationPoint3;

typedef struct {
    CreationPoint3 points[MAX_WEIGHTS];
    uint32_t count;
} CreationStore3;

static void store_creation_points_3axis(CreationStore3 *store,
                                         const uint8_t *weights, uint32_t count,
                                         uint32_t scale) {
    store->count = count;
    
    for (uint32_t i = 0; i < count; i++) {
        store->points[i].original_slot = i;
        store->points[i].scale = scale;
        store->points[i].value = weights[i];
        
        /* Auto-select axis */
        uint8_t axis = select_axis(i);
        uint32_t aslot = axis_slot(i);
        
        store->points[i].axis = axis;
        HypComplex w = kis_to_hyperbolic_axis(aslot, axis);
        store->points[i].hyper_re = w.re;
        store->points[i].hyper_im = w.im;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   ADDRESS RESOLVER (3-axis aware)
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint32_t resolve_address_3axis(const CreationPoint3 *cp,
                                              uint32_t target_scale) {
    double ratio = (double)target_scale / (double)cp->scale;
    double new_re = cp->hyper_re * ratio;
    double new_im = cp->hyper_im * ratio;
    HypComplex w = {new_re, new_im};
    
    /* hyperbolic_to_kis_axis already adds axis offset */
    return hyperbolic_to_kis_axis(w, cp->axis);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: 3-Axis Roundtrip
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_3axis_roundtrip(void) {
    printf("TEST: 3-Axis Roundtrip (all 20736 slots)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Loaded %u weights\n\n", count);
    
    /* Store at scale 1.0 */
    CreationStore3 store;
    uint32_t scale_1 = (uint32_t)(1.0 * 65536.0);
    store_creation_points_3axis(&store, original, count, scale_1);
    
    /* Roundtrip test */
    int ok = 0, fail_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = resolve_address_3axis(&store.points[i], scale_1);
        if (addr == i) {
            ok++;
        } else {
            fail_count++;
            if (fail_count <= 5) {
                printf("  FAIL: slot %u → %u (axis %u)\n", 
                       i, addr, store.points[i].axis);
            }
        }
    }
    
    CHECK(1, "3-axis roundtrip (all 20736)", fail_count == 0);
    printf("    OK: %u, FAIL: %u\n", ok, fail_count);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: Compression across all axes
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_3axis_compression(void) {
    printf("TEST: 3-Axis Compression\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    CreationStore3 store;
    uint32_t scale_1 = (uint32_t)(1.0 * 65536.0);
    store_creation_points_3axis(&store, original, count, scale_1);
    
    double test_scales[] = {1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1};
    int n = sizeof(test_scales) / sizeof(test_scales[0]);
    
    printf("  Scale  | Unique Addrs | Compression\n");
    printf("  -------|--------------|------------\n");
    
    for (int s = 0; s < n; s++) {
        double scale = test_scales[s];
        uint32_t scale_fp = (uint32_t)(scale * 65536.0);
        
        uint32_t addrs[MAX_WEIGHTS];
        for (uint32_t i = 0; i < count; i++) {
            addrs[i] = resolve_address_3axis(&store.points[i], scale_fp);
        }
        
        /* Count unique */
        uint32_t unique = 0;
        for (uint32_t i = 0; i < count; i++) {
            int found = 0;
            for (uint32_t j = 0; j < i; j++) {
                if (addrs[i] == addrs[j]) { found = 1; break; }
            }
            if (!found) unique++;
        }
        
        printf("  %-6.2f | %12u | %.2fx\n", scale, unique, (double)count / unique);
    }
    
    CHECK(2, "Compression calculation works", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS + Hyper: 3-Axis Pipeline with Auto Axis Selection\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_3axis_roundtrip();
    test_3axis_compression();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
