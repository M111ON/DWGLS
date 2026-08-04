/* kis_container_place.c — Place container on KIS field, explore operations */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define KIS_SNAP 20736
#define ICO_VERTS 144

/* ═══════════════════════════════════════════════════════════════
   SIMPLE CONTAINER (FGLS-like)
   Just data + metadata — no compression
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t magic;      /* 0x46474C53 = "FGLS" */
    uint32_t n_weights;  /* number of weights */
    uint32_t n_dims;     /* number of dimensions */
    uint32_t dims[4];    /* dimension sizes (X, Y, Z, W) */
    float    weights[];  /* actual data (flexible array) */
} Container;

static Container* container_create(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    uint32_t n = x * y * z * w;
    Container *c = (Container*)malloc(sizeof(Container) + n * sizeof(float));
    if (!c) return NULL;
    
    c->magic = 0x46474C53;  /* "FGLS" */
    c->n_weights = n;
    c->n_dims = 4;
    c->dims[0] = x;
    c->dims[1] = y;
    c->dims[2] = z;
    c->dims[3] = w;
    
    /* Fill with test data */
    for (uint32_t i = 0; i < n; i++) {
        c->weights[i] = (float)i;
    }
    
    return c;
}

static void container_free(Container *c) {
    free(c);
}

/* ═══════════════════════════════════════════════════════════════
   KIS FIELD — 20736 slots
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t slots[KIS_SNAP];  /* address of container at each slot */
    uint32_t n_used;           /* how many slots used */
} KISField;

static void kis_init(KISField *f) {
    memset(f->slots, 0, sizeof(f->slots));
    f->n_used = 0;
}

/* Place container at slot (lookup only — no computation) */
static int kis_place(KISField *f, uint32_t slot, Container *c) {
    if (slot >= KIS_SNAP) return -1;
    f->slots[slot] = (uint32_t)(uintptr_t)c;
    f->n_used++;
    return 0;
}

/* Retrieve container from slot (lookup only) */
static Container* kis_get(KISField *f, uint32_t slot) {
    if (slot >= KIS_SNAP) return NULL;
    return (Container*)(uintptr_t)f->slots[slot];
}

/* ═══════════════════════════════════════════════════════════════
   4D COORDINATE MAPPING
   Approach C: 6ico × scale (144²)
   ═══════════════════════════════════════════════════════════════ */

/* Map 4D coordinate (x,y,z,w) → slot on KIS field */
static uint32_t kis_slot_4d(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    /* 6ico vertex = spatial position (0..143) */
    uint32_t vertex = (x * 12 + y) % ICO_VERTS;
    
    /* W = scale/temporal position (0..143) */
    uint32_t scale = w % ICO_VERTS;
    
    /* Combine: vertex × 144 + scale */
    return vertex * ICO_VERTS + scale;
}

/* Inverse: slot → 4D coordinate */
static void kis_unslot_4d(uint32_t slot, uint32_t *x, uint32_t *y, uint32_t *z, uint32_t *w) {
    uint32_t vertex = slot / ICO_VERTS;
    uint32_t scale = slot % ICO_VERTS;
    
    *x = vertex / 12;
    *y = vertex % 12;
    *z = 0;  /* simplified */
    *w = scale;
}

/* ═══════════════════════════════════════════════════════════════
   OPERATIONS ON CONTAINER
   ═══════════════════════════════════════════════════════════════ */

/* 1. READ — get weight at 4D position */
static float op_read(KISField *f, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    uint32_t slot = kis_slot_4d(x, y, z, w);
    Container *c = kis_get(f, slot);
    if (!c) return 0.0f;
    
    /* Map 4D to container index */
    uint32_t idx = (x % c->dims[0]) * c->dims[1] * c->dims[2] * c->dims[3]
                 + (y % c->dims[1]) * c->dims[2] * c->dims[3]
                 + (z % c->dims[2]) * c->dims[3]
                 + (w % c->dims[3]);
    
    return c->weights[idx % c->n_weights];
}

/* 2. WRITE — set weight at 4D position */
static void op_write(KISField *f, uint32_t x, uint32_t y, uint32_t z, uint32_t w, float val) {
    uint32_t slot = kis_slot_4d(x, y, z, w);
    Container *c = kis_get(f, slot);
    if (!c) return;
    
    uint32_t idx = (x % c->dims[0]) * c->dims[1] * c->dims[2] * c->dims[3]
                 + (y % c->dims[1]) * c->dims[2] * c->dims[3]
                 + (z % c->dims[2]) * c->dims[3]
                 + (w % c->dims[3]);
    
    c->weights[idx % c->n_weights] = val;
}

/* 3. BIRD'S EYE — view from one axis (X, Y, Z, or W) */
static void op_birds_eye(KISField *f, int axis, uint32_t fix_val) {
    const char *axes[] = {"X", "Y", "Z", "W"};
    printf("Bird's eye view from %s=%u:\n", axes[axis], fix_val);
    
    for (int i = 0; i < 5; i++) {
        uint32_t x = (axis == 0) ? fix_val : i;
        uint32_t y = (axis == 1) ? fix_val : i;
        uint32_t z = (axis == 2) ? fix_val : i;
        uint32_t w = (axis == 3) ? fix_val : i;
        
        float val = op_read(f, x, y, z, w);
        printf("  (%u,%u,%u,%u) = %.1f\n", x, y, z, w, val);
    }
    printf("\n");
}

/* 4. SCALE — change W (temporal position) */
static void op_scale(KISField *f, uint32_t old_w, uint32_t new_w) {
    printf("Scale: W=%u → W=%u\n", old_w, new_w);
    
    /* Read from old W */
    float vals[4];
    for (int i = 0; i < 4; i++) {
        vals[i] = op_read(f, i, 0, 0, old_w);
    }
    
    /* Write to new W */
    for (int i = 0; i < 4; i++) {
        op_write(f, i, 0, 0, new_w, vals[i]);
    }
    
    printf("  Copied 4 values from W=%u to W=%u\n", old_w, new_w);
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS Container Placement — Explore Operations\n\n");
    
    /* Create KIS field */
    KISField field;
    kis_init(&field);
    
    /* Create container (2×2×2×2 = 16 weights) */
    Container *c = container_create(2, 2, 2, 2);
    if (!c) { printf("Failed to create container\n"); return 1; }
    
    printf("Container created:\n");
    printf("  magic: 0x%08X\n", c->magic);
    printf("  weights: %u\n", c->n_weights);
    printf("  dims: %u×%u×%u×%u\n\n", c->dims[0], c->dims[1], c->dims[2], c->dims[3]);
    
    /* Place on KIS field */
    uint32_t slot = kis_slot_4d(0, 0, 0, 0);  /* slot 0 */
    kis_place(&field, slot, c);
    printf("Placed container at slot %u\n\n", slot);
    
    /* Operation 1: READ */
    printf("=== Operation 1: READ ===\n");
    for (int i = 0; i < 4; i++) {
        float val = op_read(&field, i, 0, 0, 0);
        printf("  read(%d,0,0,0) = %.1f\n", i, val);
    }
    printf("\n");
    
    /* Operation 2: WRITE */
    printf("=== Operation 2: WRITE ===\n");
    op_write(&field, 0, 0, 0, 0, 999.0f);
    printf("  write(0,0,0,0) = 999.0\n");
    printf("  read(0,0,0,0) = %.1f\n\n", op_read(&field, 0, 0, 0, 0));
    
    /* Operation 3: BIRD'S EYE */
    printf("=== Operation 3: BIRD'S EYE ===\n");
    op_birds_eye(&field, 3, 0);  /* View from W=0 */
    printf("\n");
    
    /* Operation 4: SCALE */
    printf("=== Operation 4: SCALE ===\n");
    op_scale(&field, 0, 1);
    printf("  read(0,0,0,1) = %.1f (should be 999.0)\n\n", op_read(&field, 0, 0, 0, 1));
    
    /* Summary */
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("SUMMARY — Operations on Container in KIS Field:\n");
    printf("  1. READ   — get weight at 4D position (lookup only)\n");
    printf("  2. WRITE  — set weight at 4D position\n");
    printf("  3. BIRD'S EYE — view from one axis\n");
    printf("  4. SCALE  — change W (temporal position)\n\n");
    printf("All operations are O(1) — pure lookup, no computation\n");
    
    /* Cleanup */
    container_free(c);
    
    return 0;
}
