/*
 * beam_field_opt.c — Optimized Beam Field Codec
 * 
 * Key insight: tessellation grid is FIXED at decode time
 * → only store: triangle_id + quantized barycentric coords
 * → 3 bytes/weight = same as Q8_0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "gguf_reader.h"

/* ══════════════════════════════════════════════════════════════
   GRID (same as before, but stored for decode)
   ══════════════════════════════════════════════════════════════ */

typedef struct { double x, y; } FPos;

typedef struct {
    FPos v0, v1, v2;
    FPos centroid;
} Tri;

#define GRID_SIDE 10
#define NUM_TRIS (GRID_SIDE * GRID_SIDE * 2)

static Tri grid[NUM_TRIS];
static int grid_ready = 0;

static void grid_build(FPos origin, double size)
{
    if (grid_ready) return;
    double ax = size, bx = size*0.5, by = size*sqrt(3.0)/2.0;
    int n = 0;
    for (int r = 0; r < GRID_SIDE; r++) {
        for (int c = 0; c < GRID_SIDE; c++) {
            FPos p0 = { origin.x + c*ax + r*bx, origin.y + r*by };
            /* Up */
            grid[n].v0 = p0;
            grid[n].v1 = (FPos){p0.x+ax, p0.y};
            grid[n].v2 = (FPos){p0.x+bx, p0.y+by};
            grid[n].centroid.x = (grid[n].v0.x+grid[n].v1.x+grid[n].v2.x)/3.0;
            grid[n].centroid.y = (grid[n].v0.y+grid[n].v1.y+grid[n].v2.y)/3.0;
            n++;
            /* Down */
            FPos p3 = {p0.x+bx, p0.y+by};
            grid[n].v0 = p3;
            grid[n].v1 = (FPos){p3.x+bx, p3.y-by};
            grid[n].v2 = (FPos){p3.x+ax, p3.y};
            grid[n].centroid.x = (grid[n].v0.x+grid[n].v1.x+grid[n].v2.x)/3.0;
            grid[n].centroid.y = (grid[n].v0.y+grid[n].v1.y+grid[n].v2.y)/3.0;
            n++;
        }
    }
    grid_ready = 1;
}

/* ══════════════════════════════════════════════════════════════
   ENCODE: Weight → 3-byte field code
   ══════════════════════════════════════════════════════════════
 *
 *   [tri_id:8][bary_l0:8][bary_l1:8] = 24 bits = 3 bytes
 *
 *   barycentric l0, l1 ∈ [0,1] → Q8 量化 (0-255)
 */

typedef struct {
    uint8_t tri_id;
    uint8_t bary_l0;  /* Q8: l0 * 255 */
    uint8_t bary_l1;  /* Q8: l1 * 255 */
} FieldCode;

/* Beam: weight → field position */
static FPos beam_pos(int weight, double beam_length)
{
    FPos p;
    p.x = (double)weight * cos(M_PI * weight / 128.0) * beam_length;
    p.y = (double)weight * sin(M_PI * weight / 128.0) * beam_length;
    return p;
}

/* Barycentric coords */
static int barycentric(FPos p, Tri *t, double *l0, double *l1)
{
    double denom = (t->v1.y-t->v2.y)*(t->v0.x-t->v2.x) +
                   (t->v2.x-t->v1.x)*(t->v0.y-t->v2.y);
    if (fabs(denom) < 1e-10) return 0;
    *l0 = ((t->v1.y-t->v2.y)*(p.x-t->v2.x) + (t->v2.x-t->v1.x)*(p.y-t->v2.y)) / denom;
    *l1 = ((t->v2.y-t->v0.y)*(p.x-t->v2.x) + (t->v0.x-t->v2.x)*(p.y-t->v2.y)) / denom;
    return 1;
}

/* Find triangle + barycentric coords */
static FieldCode field_encode(int weight, double beam_length)
{
    FPos bp = beam_pos(weight, beam_length);
    
    /* Find triangle containing bp */
    int best_tri = 0;
    double best_dist = 1e18;
    double best_l0 = 0, best_l1 = 0;
    
    for (int i = 0; i < NUM_TRIS; i++) {
        double l0, l1;
        if (barycentric(bp, &grid[i], &l0, &l1)) {
            double l2 = 1.0 - l0 - l1;
            if (l0 >= 0 && l1 >= 0 && l2 >= 0) {
                /* Exact containment */
                FieldCode fc;
                fc.tri_id = (uint8_t)i;
                fc.bary_l0 = (uint8_t)(l0 * 255.0 + 0.5);
                fc.bary_l1 = (uint8_t)(l1 * 255.0 + 0.5);
                return fc;
            }
        }
        /* Fallback: nearest centroid */
        double dx = bp.x - grid[i].centroid.x;
        double dy = bp.y - grid[i].centroid.y;
        double d2 = dx*dx + dy*dy;
        if (d2 < best_dist) {
            best_dist = d2;
            best_tri = i;
            barycentric(bp, &grid[i], &best_l0, &best_l1);
        }
    }
    
    /* Clamp barycentric to [0,1] */
    if (best_l0 < 0) best_l0 = 0; if (best_l0 > 1) best_l0 = 1;
    if (best_l1 < 0) best_l1 = 0; if (best_l1 > 1) best_l1 = 1;
    
    FieldCode fc;
    fc.tri_id = (uint8_t)best_tri;
    fc.bary_l0 = (uint8_t)(best_l0 * 255.0 + 0.5);
    fc.bary_l1 = (uint8_t)(best_l1 * 255.0 + 0.5);
    return fc;
}

/* ══════════════════════════════════════════════════════════════
   DECODE: 3-byte field code → Weight
   ══════════════════════════════════════════════════════════════ */

static int field_decode(FieldCode *fc, double beam_length)
{
    Tri *t = &grid[fc->tri_id];
    double l0 = fc->bary_l0 / 255.0;
    double l1 = fc->bary_l1 / 255.0;
    double l2 = 1.0 - l0 - l1;
    
    /* Reconstruct position from barycentric */
    FPos pos;
    pos.x = l0*t->v0.x + l1*t->v1.x + l2*t->v2.x;
    pos.y = l0*t->v0.y + l1*t->v1.y + l2*t->v2.y;
    
    /* Find nearest weight */
    int best_w = 0;
    double best_err = 1e18;
    for (int w = -128; w <= 127; w++) {
        FPos bp = beam_pos(w, beam_length);
        double dx = bp.x - pos.x;
        double dy = bp.y - pos.y;
        double err = dx*dx + dy*dy;
        if (err < best_err) { best_err = err; best_w = w; }
    }
    return best_w;
}

/* ══════════════════════════════════════════════════════════════
   TEST
   ══════════════════════════════════════════════════════════════ */

static void test_roundtrip(void)
{
    printf("=== Q8 Range Roundtrip (-128..127) ===\n");
    grid_build((FPos){-5,-5}, 1.0);
    
    int pass = 0, fail = 0;
    int max_err = 0;
    
    for (int w = -128; w <= 127; w++) {
        FieldCode fc = field_encode(w, 1.0);
        int decoded = field_decode(&fc, 1.0);
        int err = abs(decoded - w);
        if (err > max_err) max_err = err;
        if (decoded == w) pass++; else fail++;
    }
    
    printf("  PASS: %d/256  FAIL: %d/256  max_err=%d\n\n", pass, fail, max_err);
}

static void test_real_model(const char *path)
{
    printf("=== Real Model (Qwen2.5-0.5B) ===\n");
    
    GGUF_File *gf = gguf_open(path);
    if (!gf) { fprintf(stderr, "Failed to open %s\n", path); return; }
    
    int idx = gguf_find_tensor(gf, "token_embd");
    if (idx < 0) idx = 0;
    
    GGUF_Tensor *tensor = &gf->tensors[idx];
    printf("  Tensor: %s (%llu bytes)\n", tensor->name, (unsigned long long)tensor->size_bytes);
    
    /* Read Q8_0 data */
    uint8_t *raw = malloc(tensor->size_bytes);
    fseek(gf->fp, gf->tensor_data_start + tensor->offset, SEEK_SET);
    fread(raw, 1, tensor->size_bytes, gf->fp);
    
    int n_blocks = (int)(tensor->size_bytes / 33);
    int n_test = (n_blocks > 1000) ? 1000 : n_blocks;
    
    /* Dequantize */
    float *weights = malloc(n_test * sizeof(float));
    for (int b = 0; b < n_test; b++) {
        uint16_t sh = *(uint16_t*)(raw + b*33);
        if ((sh & 0x7C00) == 0x7C00) { weights[b] = 0; continue; }
        uint32_t sign=(sh>>15)&1, exp=(sh>>10)&0x1F, mant=sh&0x3FF;
        uint32_t f;
        if (exp==0) f=(sign<<31)|(mant<<13);
        else if (exp==31) f=(sign<<31)|0x7F800000|(mant<<13);
        else { exp+=127-15; f=(sign<<31)|(exp<<23)|(mant<<13); }
        float scale; memcpy(&scale,&f,4);
        float d = scale/127.0f;
        float sum=0;
        for (int i=0;i<32;i++) sum += (float)((int8_t)raw[b*33+2+i])*d;
        weights[b] = sum/32.0f;
    }
    
    /* Roundtrip test */
    int pass=0, fail=0, max_err=0;
    double total_err=0;
    
    for (int i = 0; i < 100; i++) {
        int w_q = (int)(weights[i] * 127.0);
        if (w_q > 127) w_q = 127;
        if (w_q < -128) w_q = -128;
        
        FieldCode fc = field_encode(w_q, 1.0);
        int decoded = field_decode(&fc, 1.0);
        
        int err = abs(decoded - w_q);
        total_err += err;
        if (err > max_err) max_err = err;
        if (decoded == w_q) pass++; else fail++;
    }
    
    printf("  Roundtrip: %d/100 PASS  max_err=%d  total_err=%.0f\n", pass, fail, max_err, total_err);
    
    /* Size comparison */
    printf("\n  === Size ===\n");
    printf("  Q8_0:    %.2f MB (33 B/block = %.2f B/weight)\n",
           (double)tensor->size_bytes/1048576.0, 33.0/32.0);
    printf("  Field:   %.2f MB (3 B/weight)\n",
           (double)n_test*3/1048576.0);
    printf("  Ratio:   %.4fx (field/Q8_0) %s\n\n",
           3.0 / (33.0/32.0),
           3.0 < (33.0/32.0) ? "← SMALLER!" : "");
    
    free(raw); free(weights);
    gguf_close(gf);
}

int main(int argc, char **argv)
{
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  Beam Field Codec — Optimized (3 bytes/weight)    ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    test_roundtrip();
    
    if (argc >= 2)
        test_real_model(argv[1]);
    
    return 0;
}
