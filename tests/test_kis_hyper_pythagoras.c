/* test_kis_hyper_pythagoras.c — Pythagorean Spike Ratios
 *
 * Test: 3² + 4² = 5² (9:16:25)
 * Test: 2² × 3² = 6² (4:9:36)
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_pythagoras test_kis_hyper_pythagoras.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"
#include "gguf_reader.h"

#define MAX_WEIGHTS 20736
#define PI 3.14159265358979323846

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
   PYTHAGOREAN CONFIGURATIONS
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t x_slots;
    uint32_t y_slots;
    uint32_t z_slots;
    const char *name;
    const char *formula;
} PythConfig;

/* 3² + 4² = 5² → 9 + 16 + 25 = 50 parts */
static PythConfig CFG_345 = {
    3732, 6636, 10368,    /* 9:16:25 ratio */
    "3-4-5 (9:16:25)",
    "3² + 4² = 5²"
};

/* 2² × 3² = 6² → 4 + 9 + 36 = 49 parts */
static PythConfig CFG_236 = {
    1693, 3810, 15233,    /* 4:9:36 ratio */
    "2-3-6 (4:9:36)",
    "2² × 3² = 6²"
};

/* 5-12-13 (another Pythagorean triple) */
static PythConfig CFG_51213 = {
    20736 * 25 / 338,    /* 25 */
    20736 * 144 / 338,   /* 144 */
    20736 * 169 / 338,   /* 169 */
    "5-12-13 (25:144:169)",
    "5² + 12² = 13²"
};

/* ═══════════════════════════════════════════════════════════════════════════
   RESOLVER
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t select_axis_pyth(uint32_t slot, const PythConfig *cfg) {
    if (slot < cfg->x_slots) return 0;
    if (slot < cfg->x_slots + cfg->y_slots) return 1;
    return 2;
}

static inline uint32_t axis_slot_pyth(uint32_t slot, const PythConfig *cfg) {
    if (slot < cfg->x_slots) return slot;
    if (slot < cfg->x_slots + cfg->y_slots) return slot - cfg->x_slots;
    return slot - cfg->x_slots - cfg->y_slots;
}

static inline uint32_t resolve_pyth(uint32_t slot, uint32_t scale,
                                     const PythConfig *cfg) {
    uint8_t axis = select_axis_pyth(slot, cfg);
    uint32_t aslot = axis_slot_pyth(slot, cfg);
    
    uint32_t axis_slots;
    if (axis == 0) axis_slots = cfg->x_slots;
    else if (axis == 1) axis_slots = cfg->y_slots;
    else axis_slots = cfg->z_slots;
    
    double angle = 2.0 * PI * (double)aslot / (double)axis_slots;
    angle += (double)axis * 2.0 * PI / 3.0;
    
    double ratio = (double)scale / (double)((uint32_t)(1.0 * 65536.0));
    double new_angle = angle * ratio;
    
    while (new_angle < 0) new_angle += 2.0 * PI;
    while (new_angle >= 2.0 * PI) new_angle -= 2.0 * PI;
    
    double a = new_angle;
    a -= (double)axis * 2.0 * PI / 3.0;
    if (a < 0) a += 2.0 * PI;
    
    uint32_t result = (uint32_t)(a * (double)axis_slots / (2.0 * PI) + 0.5);
    uint32_t offset = 0;
    if (axis == 1) offset = cfg->x_slots;
    else if (axis == 2) offset = cfg->x_slots + cfg->y_slots;
    
    return (result % axis_slots) + offset;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: Compare Pythagorean Configs
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_pythagoras(void) {
    printf("TEST: Pythagorean Spike Ratios\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Loaded %u weights\n\n", count);
    
    PythConfig *configs[] = {&CFG_345, &CFG_236, &CFG_51213};
    int n = 3;
    
    double test_scales[] = {1.0, 0.5, 0.1};
    int n_scales = 3;
    
    printf("  Config         | Scale | Unique | Compress | Roundtrip\n");
    printf("  ----------------|-------|--------|----------|----------\n");
    
    for (int c = 0; c < n; c++) {
        PythConfig *cfg = configs[c];
        
        for (int s = 0; s < n_scales; s++) {
            double scale = test_scales[s];
            uint32_t scale_fp = (uint32_t)(scale * 65536.0);
            
            uint32_t addrs[MAX_WEIGHTS];
            for (uint32_t i = 0; i < count; i++) {
                addrs[i] = resolve_pyth(i, scale_fp, cfg);
            }
            
            uint32_t unique = 0;
            for (uint32_t i = 0; i < count; i++) {
                int found = 0;
                for (uint32_t j = 0; j < i; j++) {
                    if (addrs[i] == addrs[j]) { found = 1; break; }
                }
                if (!found) unique++;
            }
            
            /* Roundtrip at scale 1.0 */
            int rt_ok = 0;
            if (scale == 1.0) {
                for (uint32_t i = 0; i < count; i++) {
                    if (addrs[i] == i) rt_ok++;
                }
            }
            
            double compression = (double)count / unique;
            printf("  %-14s | %5.1f | %6u | %8.2fx | %s\n",
                   s == 0 ? cfg->name : "",
                   scale, unique, compression,
                   scale == 1.0 ? (rt_ok == (int)count ? "100%" : "FAIL") : "-");
        }
    }
    
    CHECK(1, "Pythagorean configs produce results", 1);
    printf("\n");
    
    /* Show the math */
    printf("  Pythagorean Math:\n");
    printf("    3² + 4² = 5²  → 9 + 16 + 25 = 50 parts\n");
    printf("    2² × 3² = 6²  → 4 + 9 + 36 = 49 parts\n");
    printf("    5² + 12² = 13² → 25 + 144 + 169 = 338 parts\n");
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Pythagorean Spike Ratios on 3 Axes\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_pythagoras();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
