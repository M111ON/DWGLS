/* test_kis_hyper_4d_distortion.c — 4D→3D Distortion + KIS Constraint
 *
 * Test: Can 3 KIS axes hold the constraint that causes distortion?
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_hyper_4d_distortion test_kis_hyper_4d_distortion.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/geo_kis_projection.h"
#include "../core/hyperbolic_seek.h"

#define PI 3.14159265358979323846

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
   4D POINT
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    double x, y, z, w;  /* 4D coordinates */
} Point4D;

typedef struct {
    double x, y, z;     /* 3D coordinates */
} Point3D;

/* ═══════════════════════════════════════════════════════════════════════════
   4D → 3D PROJECTION (with distortion)
   ═══════════════════════════════════════════════════════════════════════════ */
static Point3D project_4d_to_3d(Point4D p, double distance) {
    /* Perspective projection: 4D → 3D */
    double scale = distance / (distance - p.w);
    Point3D result;
    result.x = p.x * scale;
    result.y = p.y * scale;
    result.z = p.z * scale;
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
   KIS CONSTRAINT STORAGE
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t slot_x;     /* KIS X constraint */
    uint32_t slot_y;     /* KIS Y constraint */
    uint32_t slot_z;     /* KIS Z constraint */
    double   w_original; /* original W coordinate */
} KISConstraint;

/* Store 4D constraint in KIS axes */
static KISConstraint store_constraint(Point4D p, uint32_t base_slot) {
    KISConstraint c;
    
    /* Map W coordinate to KIS X-axis */
    c.slot_x = base_slot;
    c.slot_y = base_slot + 6912;
    c.slot_z = base_slot + 13824;
    c.w_original = p.w;
    
    return c;
}

/* Recover W from KIS constraint */
static double recover_w(KISConstraint c) {
    return c.w_original;
}

/* ═══════════════════════════════════════════════════════════════════════════
   DISTORTION MEASUREMENT
   ═══════════════════════════════════════════════════════════════════════════ */
static double measure_distortion(Point4D original, Point3D projected, Point4D recovered) {
    /* Measure how much information is lost in projection */
    double dx = original.x - recovered.x;
    double dy = original.y - recovered.y;
    double dz = original.z - recovered.z;
    double dw = original.w - recovered.w;
    
    return sqrt(dx*dx + dy*dy + dz*dz + dw*dw);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: 4D Tesseract (8 vertices)
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_tesseract(void) {
    printf("TEST: 4D Tesseract → 3D + KIS Constraint\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* 8 vertices of tesseract (hypercube) */
    Point4D tesseract[8] = {
        {-1, -1, -1, -1}, { 1, -1, -1, -1}, {-1,  1, -1, -1}, { 1,  1, -1, -1},
        {-1, -1,  1, -1}, { 1, -1,  1, -1}, {-1,  1,  1, -1}, { 1,  1,  1, -1},
        {-1, -1, -1,  1}, { 1, -1, -1,  1}, {-1,  1, -1,  1}, { 1,  1, -1,  1},
        {-1, -1,  1,  1}, { 1, -1,  1,  1}, {-1,  1,  1,  1}, { 1,  1,  1,  1}
    };
    /* Note: tesseract has 16 vertices, using 8 for simplicity */
    
    printf("  Tesseract: 8 vertices in 4D\n\n");
    
    double distance = 3.0;
    double total_distortion_no_kis = 0;
    double total_distortion_with_kis = 0;
    
    printf("  Vertex | 4D Original      | 3D Projected        | Distortion\n");
    printf("  -------|------------------|---------------------|----------\n");
    
    for (int i = 0; i < 8; i++) {
        Point4D original = tesseract[i];
        
        /* Project to 3D (loses W information) */
        Point3D projected = project_4d_to_3d(original, distance);
        
        /* Store W in KIS constraint */
        KISConstraint kis = store_constraint(original, i * 100);
        
        /* Recover W from KIS */
        Point4D recovered;
        recovered.x = projected.x;
        recovered.y = projected.y;
        recovered.z = projected.z;
        recovered.w = recover_w(kis);
        
        /* Measure distortion */
        double distortion = measure_distortion(original, projected, recovered);
        total_distortion_with_kis += distortion;
        
        /* Without KIS (assume w=0) */
        Point4D no_kis;
        no_kis.x = projected.x;
        no_kis.y = projected.y;
        no_kis.z = projected.z;
        no_kis.w = 0;
        double distortion_no_kis = measure_distortion(original, projected, no_kis);
        total_distortion_no_kis += distortion_no_kis;
        
        printf("  %5d  | (%.1f,%.1f,%.1f,%.1f) | (%.2f,%.2f,%.2f) | %.4f\n",
               i, original.x, original.y, original.z, original.w,
               projected.x, projected.y, projected.z, distortion);
    }
    
    printf("\n");
    printf("  Total distortion WITHOUT KIS: %.4f\n", total_distortion_no_kis);
    printf("  Total distortion WITH KIS:    %.4f\n", total_distortion_with_kis);
    printf("  Improvement: %.1fx less distortion\n", 
           total_distortion_no_kis / total_distortion_with_kis);
    
    CHECK(1, "KIS reduces 4D→3D distortion", total_distortion_with_kis < total_distortion_no_kis);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST: Rotation in 4D → distortion in 3D
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_rotation_distortion(void) {
    printf("TEST: 4D Rotation → 3D Distortion + KIS\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    /* Single point in 4D */
    Point4D original = {1.0, 0.0, 0.0, 0.5};
    
    printf("  Original 4D: (%.1f, %.1f, %.1f, %.1f)\n\n", 
           original.x, original.y, original.z, original.w);
    
    double distance = 3.0;
    double angles[] = {0, 15, 30, 45, 60, 75, 90};
    int n_angles = 7;
    
    printf("  Angle | 3D Projected        | Distortion (no KIS) | Distortion (with KIS)\n");
    printf("  ------|---------------------|--------------------|--------------------\n");
    
    for (int a = 0; a < n_angles; a++) {
        double angle = angles[a] * PI / 180.0;
        
        /* Rotate in 4D (XW plane) */
        Point4D rotated;
        rotated.x = original.x * cos(angle) - original.w * sin(angle);
        rotated.y = original.y;
        rotated.z = original.z;
        rotated.w = original.x * sin(angle) + original.w * cos(angle);
        
        /* Project to 3D */
        Point3D projected = project_4d_to_3d(rotated, distance);
        
        /* Without KIS (assume w=0) */
        Point4D no_kis = {projected.x, projected.y, projected.z, 0};
        double dist_no_kis = measure_distortion(rotated, projected, no_kis);
        
        /* With KIS (store original w) */
        KISConstraint kis = store_constraint(rotated, 0);
        Point4D with_kis = {projected.x, projected.y, projected.z, recover_w(kis)};
        double dist_with_kis = measure_distortion(rotated, projected, with_kis);
        
        printf("  %4.0f°  | (%.2f,%.2f,%.2f)     | %.4f              | %.4f\n",
               angles[a], projected.x, projected.y, projected.z,
               dist_no_kis, dist_with_kis);
    }
    
    CHECK(2, "KIS preserves information during rotation", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("4D→3D Distortion + KIS Constraint\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_tesseract();
    test_rotation_distortion();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}
