/*
 * beam_field_test.c — Test beam_field codec on real GGUF model
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
   FIELD CODEC
   ══════════════════════════════════════════════════════════════ */

typedef struct { double x, y; } FieldPos;

typedef struct {
    FieldPos v0, v1, v2;
    FieldPos centroid;
    int id;
} Triangle;

#define GRID_SIDE 10
#define NUM_TRIANGLES (GRID_SIDE * GRID_SIDE * 2)

static Triangle triangles[NUM_TRIANGLES];
static int tri_count = 0;

static void build_grid(FieldPos origin, double size)
{
    tri_count = 0;
    double ax = size, ay = 0;
    double bx = size * 0.5, by = size * sqrt(3.0) / 2.0;
    
    for (int row = 0; row < GRID_SIDE; row++) {
        for (int col = 0; col < GRID_SIDE; col++) {
            /* Upward */
            FieldPos p0 = { origin.x + col*ax + row*bx, origin.y + row*by };
            FieldPos p1 = { p0.x + ax, p0.y };
            FieldPos p2 = { p0.x + bx, p0.y + by };
            
            Triangle *t = &triangles[tri_count++];
            t->v0 = p0; t->v1 = p1; t->v2 = p2;
            t->centroid.x = (p0.x + p1.x + p2.x) / 3.0;
            t->centroid.y = (p0.y + p1.y + p2.y) / 3.0;
            t->id = tri_count;
            
            /* Downward */
            FieldPos p3 = { p0.x + bx, p0.y + by };
            FieldPos p4 = { p3.x + bx, p3.y - by };
            FieldPos p5 = { p3.x + ax, p3.y };
            
            t = &triangles[tri_count++];
            t->v0 = p3; t->v1 = p4; t->v2 = p5;
            t->centroid.x = (p3.x + p4.x + p5.x) / 3.0;
            t->centroid.y = (p3.y + p4.y + p5.y) / 3.0;
            t->id = tri_count;
        }
    }
}

static int find_triangle(FieldPos p)
{
    for (int i = 0; i < tri_count; i++) {
        Triangle *t = &triangles[i];
        double denom = (t->v1.y - t->v2.y) * (t->v0.x - t->v2.x) +
                       (t->v2.x - t->v1.x) * (t->v0.y - t->v2.y);
        if (fabs(denom) < 1e-10) continue;
        double l0 = ((t->v1.y - t->v2.y)*(p.x - t->v2.x) + (t->v2.x - t->v1.x)*(p.y - t->v2.y)) / denom;
        double l1 = ((t->v2.y - t->v0.y)*(p.x - t->v2.x) + (t->v0.x - t->v2.x)*(p.y - t->v2.y)) / denom;
        double l2 = 1.0 - l0 - l1;
        if (l0 >= 0 && l1 >= 0 && l2 >= 0) return i;
    }
    return -1;
}

typedef struct { int triangle_id; double local_x, local_y; } FieldDelta;

static FieldDelta field_encode(double weight, double beam_length)
{
    FieldPos beam_end;
    beam_end.x = weight * cos(M_PI * weight / 128.0) * beam_length;
    beam_end.y = weight * sin(M_PI * weight / 128.0) * beam_length;
    
    int tri = find_triangle(beam_end);
    if (tri < 0) {
        tri = 0;
        for (int i = 0; i < tri_count; i++) {
            double dx = beam_end.x - triangles[i].centroid.x;
            double dy = beam_end.y - triangles[i].centroid.y;
            double dx2 = beam_end.x - triangles[tri].centroid.x;
            double dy2 = beam_end.y - triangles[tri].centroid.y;
            if (dx*dx + dy*dy < dx2*dx2 + dy2*dy2) tri = i;
        }
    }
    
    FieldDelta d;
    d.triangle_id = tri;
    d.local_x = beam_end.x - triangles[tri].centroid.x;
    d.local_y = beam_end.y - triangles[tri].centroid.y;
    return d;
}

static int field_decode(FieldDelta *d, double beam_length)
{
    Triangle *t = &triangles[d->triangle_id];
    FieldPos pos;
    pos.x = t->centroid.x + d->local_x;
    pos.y = t->centroid.y + d->local_y;
    
    int best_w = 0;
    double best_err = 1e9;
    for (int w = -128; w <= 127; w++) {
        FieldPos beam_end;
        beam_end.x = (double)w * cos(M_PI * w / 128.0) * beam_length;
        beam_end.y = (double)w * sin(M_PI * w / 128.0) * beam_length;
        double dx = beam_end.x - pos.x;
        double dy = beam_end.y - pos.y;
        double err = dx*dx + dy*dy;
        if (err < best_err) { best_err = err; best_w = w; }
    }
    return best_w;
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: %s <model.gguf>\n", argv[0]); return 1; }
    
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  Beam Field Codec — Real Model Test               ║\n");
    printf("║  \"Weights land naturally, tessellation locks it\"  ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    /* Build grid */
    FieldPos origin = {-5, -5};
    build_grid(origin, 1.0);
    printf("Grid: %d triangles\n\n", tri_count);
    
    /* Open GGUF */
    GGUF_File *gf = gguf_open(argv[1]);
    if (!gf) { fprintf(stderr, "Failed to open %s\n", argv[1]); return 1; }
    
    /* Find Q8_0 tensor */
    int idx = gguf_find_tensor(gf, "token_embd");
    if (idx < 0 && gf->tensor_count > 0) idx = 0;
    if (idx < 0) { fprintf(stderr, "No tensor\n"); gguf_close(gf); return 1; }
    
    GGUF_Tensor *tensor = &gf->tensors[idx];
    printf("Tensor: %s (%llu bytes, type=%u)\n",
           tensor->name, (unsigned long long)tensor->size_bytes, tensor->type);
    
    /* Read raw data */
    uint8_t *raw = malloc(tensor->size_bytes);
    fseek(gf->fp, gf->tensor_data_start + tensor->offset, SEEK_SET);
    fread(raw, 1, tensor->size_bytes, gf->fp);
    
    /* Dequantize Q8_0 blocks */
    int block_sz = 33; /* Q8_0: 2B scale + 32B weights */
    int n_blocks = (int)(tensor->size_bytes / block_sz);
    int n_test = (n_blocks > 1000) ? 1000 : n_blocks;
    float *weights = malloc(n_test * sizeof(float));
    
    for (int b = 0; b < n_test; b++) {
        uint16_t sh = *(uint16_t*)(raw + b * block_sz);
        if ((sh & 0x7C00) == 0x7C00) { weights[b] = 0.0f; continue; }
        
        /* fp16 to fp32 */
        uint32_t sign = (sh >> 15) & 1;
        uint32_t exp = (sh >> 10) & 0x1F;
        uint32_t mant = sh & 0x3FF;
        uint32_t f;
        if (exp == 0) f = (sign << 31) | (mant << 13);
        else if (exp == 31) f = (sign << 31) | 0x7F800000 | (mant << 13);
        else { exp += 127 - 15; f = (sign << 31) | (exp << 23) | (mant << 13); }
        float scale; memcpy(&scale, &f, 4);
        
        float d = scale / 127.0f;
        float sum = 0;
        for (int i = 0; i < 32; i++)
            sum += (float)((int8_t)raw[b * block_sz + 2 + i]) * d;
        weights[b] = sum / 32.0f;
    }
    
    printf("Dequantized %d blocks\n\n", n_test);
    
    /* Roundtrip test */
    printf("=== Roundtrip Test (first 30) ===\n");
    printf("  weight    w_q     decoded   err\n");
    printf("  ------    ----    -------   ---\n");
    
    int pass = 0, fail = 0;
    double total_err = 0;
    int max_err = 0;
    
    for (int i = 0; i < 30; i++) {
        int w_q = (int)(weights[i] * 127.0);
        if (w_q > 127) w_q = 127;
        if (w_q < -128) w_q = -128;
        
        FieldDelta d = field_encode(w_q, 1.0);
        int decoded = field_decode(&d, 1.0);
        
        int err = abs(decoded - w_q);
        total_err += (double)err;
        if (err > max_err) max_err = err;
        
        if (decoded == w_q) pass++; else fail++;
        
        printf("  %7.4f  %5d  %5d  %d%s\n", weights[i], w_q, decoded, err, err == 0 ? " ✓" : "");
    }
    
    printf("\n  PASS: %d/30  FAIL: %d/30  total_err=%.1f  max_err=%d\n\n",
           pass, fail, total_err, max_err);
    
    /* Size */
    printf("=== Size ===\n");
    printf("  Q8_0:  %.2f MB (33 B/block)\n", (double)tensor->size_bytes / 1048576.0);
    printf("  Field: %.2f MB (18 B/weight = tri_id:2B + lx:8B + ly:8B)\n",
           (double)n_test * 18 / 1048576.0);
    printf("  Ratio: %.4fx (field/Q8_0) — field LARGER\n\n", 18.0 / 33.0);
    
    free(raw); free(weights);
    gguf_close(gf);
    return 0;
}
