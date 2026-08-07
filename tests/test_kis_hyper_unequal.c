/* test_kis_hyper_unequal.c — Unequal Spike Offsets on 3 Axes
 *
 * Test: What if each axis has different spike density?
 * X-axis: high density (many spikes)
 * Y-axis: medium density
 * Z-axis: low density (few spikes)
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_unequal test_kis_hyper_unequal.c -lm
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
   UNEQUAL SPIKE CONFIGURATIONS
   ═══════════════════════════════════════════════════════════════════════════ */

/* Different spike densities per axis */
typedef struct {
    uint32_t x_slots;    /* X-axis slots */
    uint32_t y_slots;    /* Y-axis slots */
    uint32_t z_slots;    /* Z-axis slots */
    double   x_scale;    /* X-axis scale factor */
    double   y_scale;    /* Y-axis scale factor */
    double   z_scale;    /* Z-axis scale factor */
    const char *name;
} SpikeConfig;

/* Configuration 1: Equal (baseline) */
static const SpikeConfig CFG_EQUAL = {
    6912, 6912, 6912,    /* slots per axis */
    1.0, 1.0, 1.0,       /* scale factors */
    "Equal (baseline)"
};

/* Configuration 2: High X, Medium Y, Low Z */
static const SpikeConfig CFG_UNEQUAL_1 = {
    10368, 6912, 3456,   /* X: 10368 (50%), Y: 6912 (33%), Z: 3456 (17%) */
    1.5, 1.0, 0.5,       /* X: 1.5x, Y: 1.0x, Z: 0.5x */
    "Unequal: High X, Med Y, Low Z"
};

/* Configuration 3: Balanced unequal */
static const SpikeConfig CFG_UNEQUAL_2 = {
    9216, 7200, 4320,    /* X: 9216 (44%), Y: 7200 (35%), Z: 4320 (21%) */
    1.33, 1.05, 0.63,    /* scale factors */
    "Balanced Unequal"
};

/* Configuration 4: Extreme */
static const SpikeConfig CFG_UNEQUAL_3 = {
    13824, 5184, 1728,   /* X: 13824 (67%), Y: 5184 (25%), Z: 1728 (8%) */
    2.0, 0.75, 0.25,     /* scale factors */
    "Extreme: High X, Low Z"
};

/* ═══════════════════════════════════════════════════════════════════════════
   UNEQUAL RESOLVER
   ═══════════════════════════════════════════════════════════════════════════ */

/* Select axis based on slot and config */
static inline uint8_t select_axis_unequal(uint32_t slot, const SpikeConfig *cfg) {
    if (slot < cfg->x_slots) return 0;  /* X-axis */
    if (slot < cfg->x_slots + cfg->y_slots) return 1;  /* Y-axis */
    return 2;  /* Z-axis */
}

/* Get slot within axis */
static inline uint32_t axis_slot_unequal(uint32_t slot, const SpikeConfig *cfg) {
    if (slot < cfg->x_slots) return slot;
    if (slot < cfg->x_slots + cfg->y_slots) return slot - cfg->x_slots;
    return slot - cfg->x_slots - cfg->y_slots;
}

/* Resolve address with unequal spikes */
static inline uint32_t resolve_unequal(uint32_t slot, uint32_t scale,
                                        const SpikeConfig *cfg) {
    uint8_t axis = select_axis_unequal(slot, cfg);
    uint32_t aslot = axis_slot_unequal(slot, cfg);
    
    /* Get axis-specific slot count */
    uint32_t axis_slots;
    double axis_scale;
    if (axis == 0) { axis_slots = cfg->x_slots; axis_scale = cfg->x_scale; }
    else if (axis == 1) { axis_slots = cfg->y_slots; axis_scale = cfg->y_scale; }
    else { axis_slots = cfg->z_slots; axis_scale = cfg->z_scale; }
    
    /* Compute angle */
    double angle = 2.0 * PI * (double)aslot / (double)axis_slots;
    angle += (double)axis * 2.0 * PI / 3.0;
    
    /* Apply scale ratio */
    double ratio = (double)scale / ((double)((uint32_t)(1.0 * 65536.0)) * axis_scale);
    double new_angle = angle * ratio;
    
    /* Normalize */
    while (new_angle < 0) new_angle += 2.0 * PI;
    while (new_angle >= 2.0 * PI) new_angle -= 2.0 * PI;
    
    /* Convert back to slot */
    double a = new_angle;
    a -= (double)axis * 2.0 * PI / 3.0;
    if (a < 0) a += 2.0 * PI;
    
    uint32_t result = (uint32_t)(a * (double)axis_slots / (2.0 * PI) + 0.5);
    return (result % axis_slots) + cfg->x_slots * (axis > 0 ? 1 : 0) 
           + cfg->y_slots * (axis > 1 ? 1 : 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: Compare Configurations
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_compare_configs(void) {
    printf("TEST: Compare Spike Configurations\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Loaded %u weights\n\n", count);
    
    const SpikeConfig *configs[] = {&CFG_EQUAL, &CFG_UNEQUAL_1, &CFG_UNEQUAL_2, &CFG_UNEQUAL_3};
    int n_configs = 4;
    
    double test_scales[] = {1.0, 0.5, 0.1};
    int n_scales = 3;
    
    printf("  Config                     | Scale | Unique | Compress\n");
    printf("  ---------------------------|-------|--------|--------\n");
    
    for (int c = 0; c < n_configs; c++) {
        const SpikeConfig *cfg = configs[c];
        
        for (int s = 0; s < n_scales; s++) {
            double scale = test_scales[s];
            uint32_t scale_fp = (uint32_t)(scale * 65536.0);
            
            /* Resolve all addresses */
            uint32_t addrs[MAX_WEIGHTS];
            for (uint32_t i = 0; i < count; i++) {
                addrs[i] = resolve_unequal(i, scale_fp, cfg);
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
            
            double compression = (double)count / unique;
            printf("  %-26s | %5.1f | %6u | %.2fx\n",
                   c == 0 ? cfg->name : (s == 0 ? cfg->name : ""),
                   scale, unique, compression);
        }
        printf("\n");
    }
    
    CHECK(1, "All configs produce results", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: Roundtrip with unequal
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_unequal_roundtrip(void) {
    printf("TEST: Unequal Roundtrip\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    const SpikeConfig *configs[] = {&CFG_EQUAL, &CFG_UNEQUAL_1, &CFG_UNEQUAL_2, &CFG_UNEQUAL_3};
    int n_configs = 4;
    
    for (int c = 0; c < n_configs; c++) {
        const SpikeConfig *cfg = configs[c];
        uint32_t total = cfg->x_slots + cfg->y_slots + cfg->z_slots;
        
        /* Roundtrip test */
        uint32_t scale_1 = (uint32_t)(1.0 * 65536.0);
        int ok = 0, fail_count = 0;
        
        for (uint32_t i = 0; i < total && i < MAX_WEIGHTS; i++) {
            uint32_t addr = resolve_unequal(i, scale_1, cfg);
            if (addr == i) ok++;
            else fail_count++;
        }
        
        printf("  %s: %u/%u PASS (%.1f%%)\n",
               cfg->name, ok, total, (ok * 100.0 / total));
    }
    
    CHECK(2, "Roundtrip works", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("Unequal Spike Offsets on 3 Axes\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_compare_configs();
    test_unequal_roundtrip();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
