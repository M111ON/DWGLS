/* kis_4d_explore.c — Try different 4D → 20736 mappings */
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define SNAP 20736

/* ═══════════════════════════════════════════════════════════════
   APPROACH A: 3D + W as scale factor
   X, Y, Z = spatial (0..N-1)
   W = scale factor (how many slots per position)
   Total = X × Y × Z × W
   ═══════════════════════════════════════════════════════════════ */
void approach_a(void) {
    printf("=== APPROACH A: 3D + W as scale ===\n");
    
    /* Try different spatial sizes */
    int sizes[] = {2, 3, 4, 5, 6, 10, 12};
    int n_sizes = sizeof(sizes)/sizeof(int);
    
    for (int i = 0; i < n_sizes; i++) {
        int s = sizes[i];
        int total_3d = s * s * s;  /* X × Y × Z */
        int w = SNAP / total_3d;   /* W = scale factor */
        int actual = total_3d * w;
        
        printf("  %d³ × W=%d = %d slots", s, w, actual);
        if (actual == SNAP) printf(" ← PERFECT FIT");
        else if (actual < SNAP) printf(" (under)");
        else printf(" (over)");
        printf("\n");
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   APPROACH B: 4D hypercube (12⁴)
   X, Y, Z, W = each 0..11
   12 × 12 × 12 × 12 = 20736
   ═══════════════════════════════════════════════════════════════ */
void approach_b(void) {
    printf("=== APPROACH B: 4D hypercube (12⁴) ===\n");
    
    int d = 12;
    int total = d * d * d * d;
    
    printf("  %d × %d × %d × %d = %d", d, d, d, d, total);
    if (total == SNAP) printf(" ← PERFECT FIT");
    printf("\n\n");
    
    /* Show coordinate mapping */
    printf("  Coordinate mapping:\n");
    for (int i = 0; i < 5; i++) {
        int x = i % d;
        int y = (i / d) % d;
        int z = (i / (d*d)) % d;
        int w = (i / (d*d*d)) % d;
        int slot = w*d*d*d + z*d*d + y*d + x;
        printf("    (%d,%d,%d,%d) → slot %d\n", x, y, z, w, slot);
    }
    printf("    ...\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   APPROACH C: 6ico × scale (144 × 144)
   6ico = 144 vertices
   Scale = 144 (W dimension)
   144 × 144 = 20736
   ═══════════════════════════════════════════════════════════════ */
void approach_c(void) {
    printf("=== APPROACH C: 6ico × scale (144²) ===\n");
    
    int ico = 144;
    int scale = 144;
    int total = ico * scale;
    
    printf("  %d × %d = %d", ico, scale, total);
    if (total == SNAP) printf(" ← PERFECT FIT");
    printf("\n\n");
    
    /* Show mapping */
    printf("  Mapping:\n");
    printf("    6ico vertex (0..143) = spatial position\n");
    printf("    Scale (0..143) = W dimension (temporal position)\n");
    printf("    slot = vertex × 144 + scale\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   APPROACH D: Timeline as W
   20736 timeline positions
   W = position in timeline (0..20735)
   X, Y, Z = spatial within each position
   ═══════════════════════════════════════════════════════════════ */
void approach_d(void) {
    printf("=== APPROACH D: Timeline as W ===\n");
    
    printf("  W = timeline position (0..20735)\n");
    printf("  X, Y, Z = spatial within each position\n\n");
    
    /* How many spatial positions per timeline? */
    int spatial_sizes[] = {1, 2, 4, 8, 10, 16, 32};
    int n = sizeof(spatial_sizes)/sizeof(int);
    
    for (int i = 0; i < n; i++) {
        int s = spatial_sizes[i];
        int spatial = s * s * s;
        int timeline = SNAP / spatial;
        printf("    %d³ spatial × %d timeline = %d", s, timeline, spatial * timeline);
        if (spatial * timeline == SNAP) printf(" ← FIT");
        printf("\n");
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS Dimension — 4D Mapping Exploration\n");
    printf("SNAP = %d\n\n", SNAP);
    
    approach_a();
    approach_b();
    approach_c();
    approach_d();
    
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("OBSERVATIONS:\n");
    printf("  A: 3D + W scale — flexible but W is calculated, not independent\n");
    printf("  B: 12⁴ — perfect fit, each dimension independent (0..11)\n");
    printf("  C: 144² — 6ico × scale, elegant (protagonist × temporal)\n");
    printf("  D: Timeline as W — W is position, not scale\n\n");
    printf("NEXT: Try each approach with bird's eye view test\n");
    
    return 0;
}
