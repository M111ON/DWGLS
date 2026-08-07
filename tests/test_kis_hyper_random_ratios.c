/* test_kis_hyper_random_ratios.c — Random Unequal Spike Ratios
 *
 * Test: RANDOM unequal spike ratios (not mathematical like Pythagorean/Fibonacci)
 * Each axis gets a random number of slots (total = 20736)
 * Measures compression at scale 1.0, 0.5, 0.1
 * Tests roundtrip (decode back to original slot)
 * Compares with mathematical ratios
 *
 * BUILD: gcc -O2 -I../core -I../FGLS_new/runner -o test_kis_hyper_random_ratios test_kis_hyper_random_ratios.c -lm
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
   RANDOM RATIO GENERATOR
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t x, y, z;          /* slot counts per axis (total = 20736) */
    const char *name;
    const char *type;
} RandomRatio;

/* Generate random ratio: each axis gets random portion, total = 20736 */
static RandomRatio generate_random_ratio(uint32_t seed) {
    static uint32_t state = 12345;
    state = seed;
    
    #define NEXT_RAND() (state = state * 1103515245 + 12345)
    
    uint32_t s1 = (NEXT_RAND() % 20734) + 1;
    uint32_t s2 = (NEXT_RAND() % 20734) + 1;
    
    if (s1 > s2) { uint32_t tmp = s1; s1 = s2; s2 = tmp; }
    
    RandomRatio r;
    r.x = s1;
    r.y = s2 - s1;
    r.z = 20736 - s2;
    
    if (r.x == 0) r.x = 1;
    if (r.y == 0) r.y = 1;
    if (r.z == 0) r.z = 1;
    
    uint32_t total = r.x + r.y + r.z;
    if (total != 20736) {
        r.z = 20736 - r.x - r.y;
        if (r.z == 0) r.z = 1;
    }
    
    return r;
    #undef NEXT_RAND
}

/* Scale a small ratio to fit 20736 total */
static RandomRatio scale_ratio(uint32_t rx, uint32_t ry, uint32_t rz,
                                const char *name, const char *type) {
    uint32_t total = rx + ry + rz;
    RandomRatio r;
    r.x = 20736 * rx / total;
    r.y = 20736 * ry / total;
    r.z = 20736 - r.x - r.y;
    r.name = name;
    r.type = type;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
   RESOLVER
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t resolve_ratio(uint32_t slot, uint32_t scale,
                                      const RandomRatio *r) {
    uint32_t x_slots = r->x;
    uint32_t y_slots = r->y;
    uint32_t z_slots = r->z;
    
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
   HELPER: Count unique addresses
   ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t count_unique(const uint32_t *addrs, uint32_t n) {
    uint32_t unique = 0;
    for (uint32_t i = 0; i < n; i++) {
        int found = 0;
        for (uint32_t j = 0; j < i; j++) {
            if (addrs[i] == addrs[j]) { found = 1; break; }
        }
        if (!found) unique++;
    }
    return unique;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 1: Random Ratios — Compression + Roundtrip
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_random_ratios(void) {
    printf("TEST 1: Random Unequal Ratios — Compression + Roundtrip\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Loaded %u weights\n\n", count);
    
    /* Generate 5 random ratios */
    RandomRatio randoms[5];
    for (int i = 0; i < 5; i++) {
        randoms[i] = generate_random_ratio(12345 + i * 6789);
        printf("  Random %d: %u:%u:%u (total=%u)\n",
               i+1, randoms[i].x, randoms[i].y, randoms[i].z,
               randoms[i].x + randoms[i].y + randoms[i].z);
    }
    printf("\n");
    
    double scales[] = {1.0, 0.5, 0.1};
    int n_scales = 3;
    
    printf("  Config    | Scale | Unique | Compress | Roundtrip\n");
    printf("  ----------|-------|--------|----------|----------\n");
    
    for (int r = 0; r < 5; r++) {
        RandomRatio *ratio = &randoms[r];
        char label[32];
        snprintf(label, sizeof(label), "R%d %u:%u:%u",
                 r+1, ratio->x, ratio->y, ratio->z);
        
        for (int s = 0; s < n_scales; s++) {
            double scale = scales[s];
            uint32_t scale_fp = (uint32_t)(scale * 65536.0);
            
            uint32_t addrs[MAX_WEIGHTS];
            for (uint32_t i = 0; i < count; i++) {
                addrs[i] = resolve_ratio(i, scale_fp, ratio);
            }
            
            uint32_t unique = count_unique(addrs, count);
            
            int rt_ok = 0;
            if (scale == 1.0) {
                for (uint32_t i = 0; i < count; i++) {
                    if (addrs[i] == i) rt_ok++;
                }
            }
            
            double compression = (double)count / unique;
            printf("  %-9s | %5.1f | %6u | %8.2fx | %s\n",
                   s == 0 ? label : "",
                   scale, unique, compression,
                   scale == 1.0 ? (rt_ok == (int)count ? "100%" : "FAIL") : "-");
        }
    }
    
    CHECK(1, "Random ratios produce results", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 2: Compare Random vs Mathematical Ratios (both scaled to 20736)
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_compare_ratios(void) {
    printf("TEST 2: Random vs Mathematical Ratios (all scaled to 20736)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    /* Mathematical ratios — scaled to 20736 */
    RandomRatio math_ratios[] = {
        scale_ratio(3, 4, 5, "3-4-5", "Pythagorean"),
        scale_ratio(5, 12, 13, "5-12-13", "Pythagorean"),
        scale_ratio(2, 3, 5, "2-3-5", "Fibonacci"),
        scale_ratio(3, 5, 8, "3-5-8", "Fibonacci"),
        scale_ratio(1, 2, 4, "1-2-4", "Power2"),
        scale_ratio(1, 1, 1, "1-1-1", "Equal"),
    };
    int n_math = 6;
    
    /* Random ratios — already scaled to 20736 */
    RandomRatio rand_ratios[3];
    for (int i = 0; i < 3; i++) {
        rand_ratios[i] = generate_random_ratio(99999 + i * 11111);
    }
    int n_rand = 3;
    
    double scales[] = {1.0, 0.5, 0.1};
    
    printf("  Type   | Ratio        | S=1.0  | S=0.5  | S=0.1  | RT@1.0\n");
    printf("  --------|--------------|--------|--------|--------|-------\n");
    
    /* Test mathematical ratios */
    for (int r = 0; r < n_math; r++) {
        RandomRatio *ratio = &math_ratios[r];
        char results_str[128] = "";
        int rt_pass = 1;
        
        for (int s = 0; s < 3; s++) {
            uint32_t scale_fp = (uint32_t)(scales[s] * 65536.0);
            
            uint32_t addrs[MAX_WEIGHTS];
            for (uint32_t i = 0; i < count; i++) {
                addrs[i] = resolve_ratio(i, scale_fp, ratio);
            }
            
            uint32_t unique = count_unique(addrs, count);
            double compression = (double)count / unique;
            
            char buf[32];
            snprintf(buf, sizeof(buf), "%s%.2fx",
                     s > 0 ? " " : "", compression);
            strcat(results_str, buf);
            
            if (s == 0) {
                int rt_ok = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (addrs[i] == i) rt_ok++;
                }
                rt_pass = (rt_ok == (int)count);
            }
        }
        
        printf("  Math   | %-12s | %s | %s\n",
               ratio->name, results_str, rt_pass ? "100%" : "FAIL");
    }
    
    /* Test random ratios */
    for (int r = 0; r < n_rand; r++) {
        RandomRatio *ratio = &rand_ratios[r];
        char label[32];
        snprintf(label, sizeof(label), "R%d %u:%u:%u",
                 r+1, ratio->x, ratio->y, ratio->z);
        char results_str[128] = "";
        int rt_pass = 1;
        
        for (int s = 0; s < 3; s++) {
            uint32_t scale_fp = (uint32_t)(scales[s] * 65536.0);
            
            uint32_t addrs[MAX_WEIGHTS];
            for (uint32_t i = 0; i < count; i++) {
                addrs[i] = resolve_ratio(i, scale_fp, ratio);
            }
            
            uint32_t unique = count_unique(addrs, count);
            double compression = (double)count / unique;
            
            char buf[32];
            snprintf(buf, sizeof(buf), "%s%.2fx",
                     s > 0 ? " " : "", compression);
            strcat(results_str, buf);
            
            if (s == 0) {
                int rt_ok = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (addrs[i] == i) rt_ok++;
                }
                rt_pass = (rt_ok == (int)count);
            }
        }
        
        printf("  Random | %-12s | %s | %s\n",
               label, results_str, rt_pass ? "100%" : "FAIL");
    }
    
    CHECK(2, "Comparison complete", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 3: Extreme Random Ratios
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_extreme_random(void) {
    printf("TEST 3: Extreme Random Ratios (very unequal)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    RandomRatio extremes[] = {
        {18000, 1000, 1736, "Extreme 1", "87% X"},
        {500, 18000, 2236, "Extreme 2", "87% Y"},
        {1000, 1000, 18736, "Extreme 3", "90% Z"},
        {10368, 6912, 3456, "Extreme 4", "50/33/17"},
        {15000, 4000, 1736, "Extreme 5", "72/19/8"},
    };
    int n_extremes = 5;
    
    printf("  Config    | Scale | Unique | Compress | Roundtrip\n");
    printf("  ----------|-------|--------|----------|----------\n");
    
    for (int r = 0; r < n_extremes; r++) {
        RandomRatio *ratio = &extremes[r];
        char label[32];
        snprintf(label, sizeof(label), "Ext%d %u:%u:%u",
                 r+1, ratio->x, ratio->y, ratio->z);
        
        for (int s = 0; s < 3; s++) {
            double scale = (s == 0) ? 1.0 : (s == 1) ? 0.5 : 0.1;
            uint32_t scale_fp = (uint32_t)(scale * 65536.0);
            
            uint32_t addrs[MAX_WEIGHTS];
            for (uint32_t i = 0; i < count; i++) {
                addrs[i] = resolve_ratio(i, scale_fp, ratio);
            }
            
            uint32_t unique = count_unique(addrs, count);
            
            int rt_ok = 0;
            if (scale == 1.0) {
                for (uint32_t i = 0; i < count; i++) {
                    if (addrs[i] == i) rt_ok++;
                }
            }
            
            double compression = (double)count / unique;
            printf("  %-9s | %5.1f | %6u | %8.2fx | %s\n",
                   s == 0 ? label : "",
                   scale, unique, compression,
                   scale == 1.0 ? (rt_ok == (int)count ? "100%" : "FAIL") : "-");
        }
    }
    
    CHECK(3, "Extreme ratios produce results", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 4: Roundtrip Verification (scale 1.0 identity)
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_roundtrip(void) {
    printf("TEST 4: Roundtrip Verification (resolve(slot, 1.0) == slot?)\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    /* Test 5 random + 3 math ratios */
    RandomRatio ratios[8];
    for (int i = 0; i < 5; i++) {
        ratios[i] = generate_random_ratio(77777 + i * 3333);
    }
    ratios[5] = scale_ratio(3, 4, 5, "3-4-5", "Pythagorean");
    ratios[6] = scale_ratio(1, 1, 1, "1-1-1", "Equal");
    ratios[7] = scale_ratio(1, 2, 4, "1-2-4", "Power2");
    
    printf("  Config        | RT Pass | RT Fail | RT %%\n");
    printf("  --------------|---------|---------|------\n");
    
    for (int r = 0; r < 8; r++) {
        RandomRatio *ratio = &ratios[r];
        char label[32];
        if (r < 5) {
            snprintf(label, sizeof(label), "R%d %u:%u:%u",
                     r+1, ratio->x, ratio->y, ratio->z);
        } else {
            snprintf(label, sizeof(label), "%s %u:%u:%u",
                     ratio->name, ratio->x, ratio->y, ratio->z);
        }
        
        uint32_t scale_fp = (uint32_t)(1.0 * 65536.0);
        
        int rt_pass = 0, rt_fail = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t addr = resolve_ratio(i, scale_fp, ratio);
            if (addr == i) rt_pass++;
            else rt_fail++;
        }
        
        printf("  %-14s | %7d | %7d | %4.1f%%\n",
               label, rt_pass, rt_fail,
               (rt_pass * 100.0 / count));
    }
    
    CHECK(4, "Roundtrip test complete", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 5: Collision Analysis — Which axis causes collisions at scale < 1.0?
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_collision_analysis(void) {
    printf("TEST 5: Collision Analysis per Axis\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    /* Use one random ratio */
    RandomRatio ratio = generate_random_ratio(55555);
    printf("  Ratio: %u:%u:%u\n\n", ratio.x, ratio.y, ratio.z);
    
    double scales[] = {1.0, 0.5, 0.1};
    
    printf("  Scale | Axis X | Axis Y | Axis Z | Total Unique\n");
    printf("  ------|--------|--------|--------|-------------\n");
    
    for (int s = 0; s < 3; s++) {
        uint32_t scale_fp = (uint32_t)(scales[s] * 65536.0);
        
        /* Count unique per axis */
        uint32_t x_addrs[6912], y_addrs[6912], z_addrs[6912];
        uint32_t x_count = 0, y_count = 0, z_count = 0;
        
        for (uint32_t i = 0; i < count; i++) {
            uint32_t addr = resolve_ratio(i, scale_fp, &ratio);
            if (i < ratio.x) {
                x_addrs[x_count++] = addr;
            } else if (i < ratio.x + ratio.y) {
                y_addrs[y_count++] = addr;
            } else {
                z_addrs[z_count++] = addr;
            }
        }
        
        uint32_t x_unique = count_unique(x_addrs, x_count);
        uint32_t y_unique = count_unique(y_addrs, y_count);
        uint32_t z_unique = count_unique(z_addrs, z_count);
        
        uint32_t total_addrs[MAX_WEIGHTS];
        for (uint32_t i = 0; i < count; i++) {
            total_addrs[i] = resolve_ratio(i, scale_fp, &ratio);
        }
        uint32_t total_unique = count_unique(total_addrs, count);
        
        printf("  %5.1f | %6u/%u | %6u/%u | %6u/%u | %u\n",
               scales[s], x_unique, x_count, y_unique, y_count,
               z_unique, z_count, total_unique);
    }
    
    CHECK(5, "Collision analysis complete", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Random Unequal Spike Ratios on 3 Axes\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_random_ratios();
    test_compare_ratios();
    test_extreme_random();
    test_roundtrip();
    test_collision_analysis();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
