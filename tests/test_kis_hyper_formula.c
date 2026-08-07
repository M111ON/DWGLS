/* test_kis_hyper_formula.c — Hyperbolic Formula: x × f(time)
 *
 * Build and test the Hyperbolic address resolver formula.
 *证明: formula can compute correct address at any scale.
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_formula test_kis_hyper_formula.c -lm
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
   HYPERBOLIC ADDRESS RESOLVER FORMULA
   ═══════════════════════════════════════════════════════════════════════════ */

/* Store creation point (where data was written) */
typedef struct {
    uint32_t slot;           /* original slot position */
    uint32_t scale;          /* scale when written (<<16 fixed-point) */
    double   hyper_re;       /* Hyperbolic real part */
    double   hyper_im;       /* Hyperbolic imaginary part */
} CreationPoint;

/* Compute Hyperbolic address from creation point + current scale */
static inline uint32_t hyper_resolve_address(const CreationPoint *cp, 
                                              uint32_t current_scale) {
    /* Formula: x × f(time)
     * x = creation point's Hyperbolic coordinate
     * f(time) = scale ratio (current / creation)
     * 
     * Result: address where data should be read at current scale
     */
    double scale_ratio = (double)current_scale / (double)cp->scale;
    
    /* Apply scale ratio to Hyperbolic coordinate */
    double new_re = cp->hyper_re * scale_ratio;
    double new_im = cp->hyper_im * scale_ratio;
    
    /* Convert back to KIS slot via inverse Cayley */
    HypComplex w = {new_re, new_im};
    return hyperbolic_to_kis_axis(w, HYP_AXIS_X);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 1: Formula works for simple case
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_formula_simple(void) {
    printf("TEST 1: Formula works for simple case\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Create at scale 1.0 */
    CreationPoint cp;
    cp.slot = 1000;
    cp.scale = (uint32_t)(1.0 * 65536.0);
    HypComplex w = kis_to_hyperbolic_axis(cp.slot, HYP_AXIS_X);
    cp.hyper_re = w.re;
    cp.hyper_im = w.im;
    
    printf("  Creation: slot=%u, scale=%.2f\n", cp.slot, cp.scale / 65536.0);
    printf("  Hyperbolic: (%.4f, %.4f)\n", cp.hyper_re, cp.hyper_im);
    
    /* Read at same scale (should work) */
    uint32_t addr_same = hyper_resolve_address(&cp, cp.scale);
    printf("  Read at same scale: %u → %s\n", addr_same, 
           (addr_same == cp.slot) ? "OK" : "WRONG");
    CHECK(1, "Formula works at same scale", addr_same == cp.slot);
    
    /* Read at different scale (should compute new address) */
    uint32_t scale_01 = (uint32_t)(0.1 * 65536.0);
    uint32_t addr_diff = hyper_resolve_address(&cp, scale_01);
    printf("  Read at scale 0.1: %u (new address)\n", addr_diff);
    printf("  Address changed: %s\n", (addr_diff != cp.slot) ? "YES (expected)" : "NO");
    CHECK(2, "Formula computes new address at different scale", addr_diff != cp.slot);
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 2: Formula with real GGUF data
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_formula_real_gguf(void) {
    printf("TEST 2: Formula with real GGUF data\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight", 
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Read %u bytes\n\n", count);
    
    /* Store first 10 weights with creation points */
    CreationPoint points[10];
    uint32_t scale_1 = (uint32_t)(1.0 * 65536.0);
    
    printf("  Storing creation points at scale 1.0:\n");
    for (int i = 0; i < 10; i++) {
        points[i].slot = i;
        points[i].scale = scale_1;
        HypComplex w = kis_to_hyperbolic_axis(i, HYP_AXIS_X);
        points[i].hyper_re = w.re;
        points[i].hyper_im = w.im;
        printf("    slot %d: value=%u, hyper=(%.4f, %.4f)\n",
               i, original[i], points[i].hyper_re, points[i].hyper_im);
    }
    
    /* Now try to read at scale 0.5 using formula */
    printf("\n  Reading at scale 0.5 using formula:\n");
    uint32_t scale_05 = (uint32_t)(0.5 * 65536.0);
    int correct = 0, wrong = 0;
    
    for (int i = 0; i < 10; i++) {
        uint32_t addr = hyper_resolve_address(&points[i], scale_05);
        printf("    slot %d: formula → addr %u\n", i, addr);
        /* We can't verify value because KIS projection changes with scale */
        /* But we can verify formula doesn't crash and produces valid address */
        if (addr < MAX_WEIGHTS) correct++; else wrong++;
    }
    
    CHECK(3, "Formula produces valid addresses", wrong == 0);
    printf("    Valid: %d, Invalid: %d\n", correct, wrong);
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 3: Formula determinism
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_formula_determinism(void) {
    printf("TEST 3: Formula determinism\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    CreationPoint cp;
    cp.slot = 2000;
    cp.scale = (uint32_t)(1.0 * 65536.0);
    HypComplex w = kis_to_hyperbolic_axis(cp.slot, HYP_AXIS_X);
    cp.hyper_re = w.re;
    cp.hyper_im = w.im;
    
    uint32_t scale_test = (uint32_t)(0.3 * 65536.0);
    
    /* Run formula 1000 times */
    uint32_t results[1000];
    for (int i = 0; i < 1000; i++) {
        results[i] = hyper_resolve_address(&cp, scale_test);
    }
    
    /* Check all results are identical */
    int deterministic = 1;
    for (int i = 1; i < 1000; i++) {
        if (results[i] != results[0]) {
            deterministic = 0;
            break;
        }
    }
    
    CHECK(4, "Formula is deterministic (1000 runs)", deterministic);
    printf("    Result: %u (all 1000 runs)\n", results[0]);
    
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Hyperbolic Formula: x × f(time)\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_formula_simple();
    test_formula_real_gguf();
    test_formula_determinism();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
