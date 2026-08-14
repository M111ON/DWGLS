/*
 * beam_field.c — Geometric Field Codec
 *
 * Design: "Weights land on field naturally, tessellation locks it"
 *   1. Create a field (coordinate space)
 *   2. Weights land on it by themselves
 *   3. Lock with tessellation (icosahedron triangle grid)
 *   4. Pattern emerges from the locked positions
 *
 * No shapes created — just a coordinate system that naturally
 * organizes weights.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════════════════════════════════════════════════════
   FIELD DEFINITION
   ══════════════════════════════════════════════════════════════
 *   The field is a coordinate space where weights can land.
 *   We don't create shapes — we define the space.
 */

typedef struct {
    double x, y;
} FieldPos;

/* ══════════════════════════════════════════════════════════════
   ICOSAHEDRON TRIANGLE GRID (the lock)
   ══════════════════════════════════════════════════════════════
 *   Tessellation: triangle grid on icosahedron
 *   Each triangle has 3 vertices and 1 centroid
 *   Centroid = average of 3 vertices
 */

typedef struct {
    FieldPos v0, v1, v2;   /* vertices */
    FieldPos centroid;      /* centroid = (v0+v1+v2)/3 */
    int id;
} Triangle;

/* Triangle grid parameters */
#define GRID_SIDE 10
#define NUM_TRIANGLES (GRID_SIDE * GRID_SIDE * 2)

static Triangle triangles[NUM_TRIANGLES];
static int tri_count = 0;

/* Build triangle grid from icosahedron
 * The grid is defined by:
 *   - Origin point
 *   - Direction 1 (60° from origin)
 *   - Direction 2 (variable)
 *   - Triangle size (derived from 3 points)
 */
static void build_grid(FieldPos origin, double size)
{
    tri_count = 0;
    
    /* Two basis vectors for equilateral triangle grid
     * a = (size, 0)
     * b = (size/2, size*sqrt(3)/2)
     */
    double ax = size;
    double ay = 0;
    double bx = size * 0.5;
    double by = size * sqrt(3.0) / 2.0;
    
    for (int row = 0; row < GRID_SIDE; row++) {
        for (int col = 0; col < GRID_SIDE; col++) {
            /* Upward triangle */
            FieldPos p0 = { origin.x + col*ax + row*bx,
                           origin.y + row*by };
            FieldPos p1 = { p0.x + ax, p0.y };
            FieldPos p2 = { p0.x + bx, p0.y + by };
            
            Triangle *t = &triangles[tri_count++];
            t->v0 = p0;
            t->v1 = p1;
            t->v2 = p2;
            t->centroid.x = (p0.x + p1.x + p2.x) / 3.0;
            t->centroid.y = (p0.y + p1.y + p2.y) / 3.0;
            t->id = tri_count;
            
            /* Downward triangle */
            FieldPos p3 = { p0.x + bx, p0.y + by };
            FieldPos p4 = { p3.x + bx, p3.y - by };
            FieldPos p5 = { p3.x + ax, p3.y };
            
            t = &triangles[tri_count++];
            t->v0 = p3;
            t->v1 = p4;
            t->v2 = p5;
            t->centroid.x = (p3.x + p4.x + p5.x) / 3.0;
            t->centroid.y = (p3.y + p4.y + p5.y) / 3.0;
            t->id = tri_count;
        }
    }
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT LANDING
   ══════════════════════════════════════════════════════════════
 *   Weight = beam radius
 *   Beam lands in field based on its length
 *   The position is determined by the tessellation
 */

/* Find which triangle contains the point */
static int find_triangle(FieldPos p)
{
    for (int i = 0; i < tri_count; i++) {
        Triangle *t = &triangles[i];
        
        /* Compute barycentric coordinates */
        double denom = (t->v1.y - t->v2.y) * (t->v0.x - t->v2.x) +
                       (t->v2.x - t->v1.x) * (t->v0.y - t->v2.y);
        if (fabs(denom) < 1e-10) continue;
        
        double lambda0 = ((t->v1.y - t->v2.y) * (p.x - t->v2.x) +
                          (t->v2.x - t->v1.x) * (p.y - t->v2.y)) / denom;
        double lambda1 = ((t->v2.y - t->v0.y) * (p.x - t->v2.x) +
                          (t->v0.x - t->v2.x) * (p.y - t->v2.y)) / denom;
        double lambda2 = 1.0 - lambda0 - lambda1;
        
        if (lambda0 >= 0 && lambda1 >= 0 && lambda2 >= 0) {
            return i;
        }
    }
    return -1; /* not found */
}

/* ══════════════════════════════════════════════════════════════
   ENCODE: Weight → Field Position
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    int triangle_id;        /* which triangle */
    double local_x, local_y; /* position within triangle */
} FieldDelta;

/* Encode: weight → field position */
static FieldDelta field_encode(int weight, double beam_length)
{
    /* Beam endpoint in field */
    FieldPos beam_end;
    beam_end.x = (double)weight * cos(M_PI * weight / 128.0) * beam_length;
    beam_end.y = (double)weight * sin(M_PI * weight / 128.0) * beam_length;
    
    /* Find which triangle */
    int tri = find_triangle(beam_end);
    if (tri < 0) {
        /* Outside grid — use nearest triangle */
        tri = 0;
        for (int i = 0; i < tri_count; i++) {
            Triangle *t = &triangles[i];
            double dx = beam_end.x - t->centroid.x;
            double dy = beam_end.y - t->centroid.y;
            double dx2 = beam_end.x - triangles[tri].centroid.x;
            double dy2 = beam_end.y - triangles[tri].centroid.y;
            if (dx*dx + dy*dy < dx2*dx2 + dy2*dy2) {
                tri = i;
            }
        }
    }
    
    /* Local position within triangle (barycentric → Cartesian) */
    Triangle *t = &triangles[tri];
    double dx = beam_end.x - t->centroid.x;
    double dy = beam_end.y - t->centroid.y;
    
    FieldDelta d;
    d.triangle_id = tri;
    d.local_x = dx;
    d.local_y = dy;
    return d;
}

/* ══════════════════════════════════════════════════════════════
   DECODE: Field Position → Weight
   ══════════════════════════════════════════════════════════════ */

/* Decode: field delta → weight */
static int field_decode(FieldDelta *d, double beam_length)
{
    Triangle *t = &triangles[d->triangle_id];
    
    /* Reconstruct absolute position */
    FieldPos pos;
    pos.x = t->centroid.x + d->local_x;
    pos.y = t->centroid.y + d->local_y;
    
    /* Invert beam landing: find weight that maps to this position */
    int best_w = 0;
    double best_err = 1e9;
    
    for (int w = -128; w <= 127; w++) {
        FieldPos beam_end;
        beam_end.x = (double)w * cos(M_PI * w / 128.0) * beam_length;
        beam_end.y = (double)w * sin(M_PI * w / 128.0) * beam_length;
        
        double dx = beam_end.x - pos.x;
        double dy = beam_end.y - pos.y;
        double err = dx*dx + dy*dy;
        
        if (err < best_err) {
            best_err = err;
            best_w = w;
        }
    }
    
    return best_w;
}

/* ══════════════════════════════════════════════════════════════
   DEMO
   ══════════════════════════════════════════════════════════════ */

static void demo_field_pattern(void)
{
    printf("=== Field Pattern (weight landing) ===\n");
    
    /* Build grid */
    FieldPos origin = {-5, -5};
    build_grid(origin, 1.0);
    
    /* Show where each weight lands */
    printf("Weight → Triangle → Local Position\n");
    for (int w = -8; w <= 8; w += 2) {
        FieldDelta d = field_encode(w, 1.0);
        printf("  w=%4d → tri=%3d  local=(%.2f, %.2f)\n",
               w, d.triangle_id, d.local_x, d.local_y);
    }
}

static void demo_encode_decode(void)
{
    printf("\n=== Encode/Decode Roundtrip ===\n");
    
    FieldPos origin = {-5, -5};
    build_grid(origin, 1.0);
    
    int pass = 0, fail = 0;
    for (int w = -128; w <= 127; w++) {
        FieldDelta d = field_encode(w, 1.0);
        int decoded = field_decode(&d, 1.0);
        
        if (decoded == w) pass++;
        else {
            fail++;
            if (fail <= 3) printf("  w=%d decoded=%d\n", w, decoded);
        }
    }
    printf("Result: %d/%d PASS\n", pass, 256);
}

int main(void)
{
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  Beam Field Codec — Weight Lands on Field         ║\n");
    printf("║  \"Weights land naturally, tessellation locks it\"  ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    demo_field_pattern();
    demo_encode_decode();
    
    return 0;
}
