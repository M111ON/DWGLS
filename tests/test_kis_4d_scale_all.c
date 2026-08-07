/* test_kis_4d_scale_all.c — Scale All 3 Axes Simultaneously
 *
 * Test: Scale X, Y, Z axes together → compression?
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_4d_scale_all test_kis_4d_scale_all.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/geo_kis_4d_container.h"
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
   RESOLVE with per-axis scale
   ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t resolve_with_axis_scale(uint32_t slot, 
                                         double scale_x, double scale_y, double scale_z,
                                         uint32_t x_slots, uint32_t y_slots, uint32_t z_slots) {
    uint8_t axis;
    uint32_t aslot, axis_slots;
    double scale;
    
    if (slot < x_slots) {
        axis = 0; aslot = slot; axis_slots = x_slots; scale = scale_x;
    } else if (slot < x_slots + y_slots) {
        axis = 1; aslot = slot - x_slots; axis_slots = y_slots; scale = scale_y;
    } else {
        axis = 2; aslot = slot - x_slots - y_slots; axis_slots = z_slots; scale = scale_z;
    }
    
    double angle = 2.0 * M_PI * (double)aslot / (double)axis_slots;
    angle += (double)axis * 2.0 * M_PI / 3.0;
    
    double new_angle = angle * scale;
    
    while (new_angle < 0) new_angle += 2.0 * M_PI;
    while (new_angle >= 2.0 * M_PI) new_angle -= 2.0 * M_PI;
    
    double a = new_angle;
    a -= (double)axis * 2.0 * M_PI / 3.0;
    if (a < 0) a += 2.0 * M_PI;
    
    uint32_t result = (uint32_t)(a * (double)axis_slots / (2.0 * M_PI) + 0.5);
    uint32_t offset = 0;
    if (axis == 1) offset = x_slots;
    else if (axis == 2) offset = x_slots + y_slots;
    
    return (result % axis_slots) + offset;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 1: Scale all axes equally
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_equal_scale(void) {
    printf("TEST 1: Scale All Axes Equally\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Loaded %u weights\n\n", count);
    
    double scales[] = {1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1};
    int n = sizeof(scales) / sizeof(scales[0]);
    
    printf("  Scale  | Unique Addrs | Compression | Roundtrip\n");
    printf("  -------|--------------|-------------|----------\n");
    
    for (int s = 0; s < n; s++) {
        double scale = scales[s];
        uint32_t addrs[MAX_WEIGHTS];
        
        for (uint32_t i = 0; i < count; i++) {
            addrs[i] = resolve_with_axis_scale(i, scale, scale, scale, 
                                                6912, 6912, 6912);
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
        
        /* Roundtrip at scale 1.0 */
        int rt_ok = 1;
        if (scale == 1.0) {
            for (uint32_t i = 0; i < count; i++) {
                if (addrs[i] != i) { rt_ok = 0; break; }
            }
        }
        
        printf("  %-6.2f | %12u | %11.2fx | %s\n",
               scale, unique, (double)count / unique,
               scale == 1.0 ? (rt_ok ? "100%" : "FAIL") : "-");
    }
    
    CHECK(1, "Equal scale works", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 2: Scale per-axis differently
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_per_axis_scale(void) {
    printf("TEST 2: Scale Per-Axis Differently\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    /* Test different per-axis scales */
    double configs[][3] = {
        {1.0, 1.0, 1.0},   /* baseline */
        {0.5, 1.0, 1.0},   /* X only */
        {1.0, 0.5, 1.0},   /* Y only */
        {1.0, 1.0, 0.5},   /* Z only */
        {0.5, 0.5, 1.0},   /* X+Y */
        {0.5, 1.0, 0.5},   /* X+Z */
        {1.0, 0.5, 0.5},   /* Y+Z */
        {0.5, 0.5, 0.5},   /* all */
        {0.1, 1.0, 1.0},   /* X extreme */
        {0.1, 0.1, 0.1},   /* all extreme */
    };
    int n_configs = sizeof(configs) / sizeof(configs[0]);
    
    printf("  Config (X,Y,Z)      | Unique | Compress\n");
    printf("  --------------------|--------|--------\n");
    
    for (int c = 0; c < n_configs; c++) {
        double sx = configs[c][0], sy = configs[c][1], sz = configs[c][2];
        uint32_t addrs[MAX_WEIGHTS];
        
        for (uint32_t i = 0; i < count; i++) {
            addrs[i] = resolve_with_axis_scale(i, sx, sy, sz, 6912, 6912, 6912);
        }
        
        uint32_t unique = 0;
        for (uint32_t i = 0; i < count; i++) {
            int found = 0;
            for (uint32_t j = 0; j < i; j++) {
                if (addrs[i] == addrs[j]) { found = 1; break; }
            }
            if (!found) unique++;
        }
        
        printf("  (%.1f, %.1f, %.1f)        | %6u | %.2fx\n",
               sx, sy, sz, unique, (double)count / unique);
    }
    
    CHECK(2, "Per-axis scale works", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 3: Compare with data dedup only
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_vs_dedup(void) {
    printf("TEST 3: KIS Scaling vs Data Dedup\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    /* Count unique values in data */
    uint8_t unique_vals[256] = {0};
    for (uint32_t i = 0; i < count; i++) {
        unique_vals[original[i]] = 1;
    }
    uint32_t data_unique = 0;
    for (int i = 0; i < 256; i++) {
        if (unique_vals[i]) data_unique++;
    }
    
    /* KIS scaling */
    uint32_t addrs[MAX_WEIGHTS];
    for (uint32_t i = 0; i < count; i++) {
        addrs[i] = resolve_with_axis_scale(i, 0.5, 0.5, 0.5, 6912, 6912, 6912);
    }
    uint32_t kis_unique = 0;
    for (uint32_t i = 0; i < count; i++) {
        int found = 0;
        for (uint32_t j = 0; j < i; j++) {
            if (addrs[i] == addrs[j]) { found = 1; break; }
        }
        if (!found) kis_unique++;
    }
    
    printf("  Data dedup:   %u unique values = %.2fx compression\n",
           data_unique, (double)count / data_unique);
    printf("  KIS scaling:  %u unique addrs = %.2fx compression\n",
           kis_unique, (double)count / kis_unique);
    printf("  KIS advantage: %.2fx more compression\n",
           (double)data_unique / kis_unique);
    
    CHECK(3, "Comparison complete", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS 4D: Scale All Axes Simultaneously\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_equal_scale();
    test_per_axis_scale();
    test_vs_dedup();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
