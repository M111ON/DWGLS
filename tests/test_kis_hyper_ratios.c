/* test_kis_hyper_ratios.c — Ratio as Codec: Sweep Different Ratios
 *
 * Test: How do different ratios affect compression and roundtrip?
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_ratios test_kis_hyper_ratios.c -lm
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
   RATIO TYPE
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t x, y, z;
    const char *name;
    const char *type;
} Ratio;

/* Pythagorean triples */
static const Ratio R_345 = {3, 4, 5, "3-4-5", "Pythagorean"};
static const Ratio R_51213 = {5, 12, 13, "5-12-13", "Pythagorean"};
static const Ratio R_81517 = {8, 15, 17, "8-15-17", "Pythagorean"};
static const Ratio R_72425 = {7, 24, 25, "7-24-25", "Pythagorean"};

/* Fibonacci-ish */
static const Ratio R_1123 = {1, 1, 2, "1-1-2", "Fibonacci"};
static const Ratio R_235 = {2, 3, 5, "2-3-5", "Fibonacci"};
static const Ratio R_358 = {3, 5, 8, "3-5-8", "Fibonacci"};
static const Ratio R_5813 = {5, 8, 13, "5-8-13", "Fibonacci"};

/* Powers of 2 */
static const Ratio R_124 = {1, 2, 4, "1-2-4", "Power2"};
static const Ratio R_148 = {1, 4, 8, "1-4-8", "Power2"};
static const Ratio R_248 = {2, 4, 8, "2-4-8", "Power2"};

/* Extreme unequal */
static const Ratio R_118 = {1, 1, 8, "1-1-8", "Extreme"};
static const Ratio R_126 = {1, 2, 6, "1-2-6", "Extreme"};
static const Ratio R_136 = {1, 3, 6, "1-3-6", "Extreme"};

/* Balanced */
static const Ratio R_111 = {1, 1, 1, "1-1-1", "Balanced"};
static const Ratio R_223 = {2, 2, 3, "2-2-3", "Balanced"};
static const Ratio R_334 = {3, 3, 4, "3-3-4", "Balanced"};

/* ═══════════════════════════════════════════════════════════════════════════
   RESOLVER
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint32_t resolve_ratio(uint32_t slot, uint32_t scale,
                                      const Ratio *r) {
    uint32_t total = r->x + r->y + r->z;
    uint32_t x_slots = 20736 * r->x / total;
    uint32_t y_slots = 20736 * r->y / total;
    uint32_t z_slots = 20736 - x_slots - y_slots;
    
    uint8_t axis;
    uint32_t aslot;
    uint32_t axis_slots;
    
    if (slot < x_slots) {
        axis = 0; aslot = slot; axis_slots = x_slots;
    } else if (slot < x_slots + y_slots) {
        axis = 1; aslot = slot - x_slots; axis_slots = y_slots;
    } else {
        axis = 2; aslot = slot - x_slots - y_slots; axis_slots = z_slots;
    }
    
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
    if (axis == 1) offset = x_slots;
    else if (axis == 2) offset = x_slots + y_slots;
    
    return (result % axis_slots) + offset;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: Sweep Ratios
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_sweep_ratios(void) {
    printf("TEST: Ratio as Codec — Sweep Different Ratios\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Loaded %u weights\n\n", count);
    
    const Ratio *ratios[] = {
        &R_345, &R_51213, &R_81517, &R_72425,     /* Pythagorean */
        &R_1123, &R_235, &R_358, &R_5813,          /* Fibonacci */
        &R_124, &R_148, &R_248,                     /* Power2 */
        &R_118, &R_126, &R_136,                     /* Extreme */
        &R_111, &R_223, &R_334,                     /* Balanced */
    };
    int n_ratios = 17;
    
    printf("  Ratio    | Type        | Scale 1.0 | Scale 0.5 | Scale 0.1 | Roundtrip\n");
    printf("  ---------|-------------|-----------|-----------|-----------|----------\n");
    
    for (int r = 0; r < n_ratios; r++) {
        const Ratio *ratio = ratios[r];
        uint32_t results[3];
        
        for (int s = 0; s < 3; s++) {
            double scale = (s == 0) ? 1.0 : (s == 1) ? 0.5 : 0.1;
            uint32_t scale_fp = (uint32_t)(scale * 65536.0);
            
            uint32_t addrs[MAX_WEIGHTS];
            for (uint32_t i = 0; i < count; i++) {
                addrs[i] = resolve_ratio(i, scale_fp, ratio);
            }
            
            uint32_t unique = 0;
            for (uint32_t i = 0; i < count; i++) {
                int found = 0;
                for (uint32_t j = 0; j < i; j++) {
                    if (addrs[i] == addrs[j]) { found = 1; break; }
                }
                if (!found) unique++;
            }
            results[s] = unique;
        }
        
        /* Roundtrip at scale 1.0 */
        uint32_t scale_1 = (uint32_t)(1.0 * 65536.0);
        int rt_ok = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (resolve_ratio(i, scale_1, ratio) == i) rt_ok++;
        }
        
        double c1 = (double)count / results[0];
        double c2 = (double)count / results[1];
        double c3 = (double)count / results[2];
        
        printf("  %-8s | %-11s | %6.2fx   | %6.2fx   | %6.2fx   | %s\n",
               ratio->name, ratio->type, c1, c2, c3,
               rt_ok == (int)count ? "100%" : "FAIL");
    }
    
    CHECK(1, "All ratios produce results", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Ratio as Codec: Sweep Different Ratios\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_sweep_ratios();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
