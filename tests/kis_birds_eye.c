/* kis_birds_eye.c — Bird's eye view from all 4 axes on KIS field
 *
 * Demonstrates that 4D data looks different depending on which axis
 * you fix (look "down" from). Each axis produces a unique 2D slice.
 *
 * Uses kis_layer.h for KIS field constants.
 * 4D mapping: slot = vertex * 144 + scale
 *   vertex = (x * 12 + y) % 144  (spatial, 0..143)
 *   scale  = w % 144              (temporal/W, 0..143)
 *
 * Compile: gcc -O2 -Wall -Icore -o tests/kis_birds_eye.exe tests/kis_birds_eye.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "kis_layer.h"

/* ═══════════════════════════════════════════════════════════════
   4D COORDINATE SYSTEM
   Container dimensions: CX × CY × CZ × CW
   Indices (x,y,z,w) → flat index → weight value
   ═══════════════════════════════════════════════════════════════ */
#define CX 4u
#define CY 4u
#define CZ 4u
#define CW 4u
#define TOTAL (CX * CY * CZ * CW)  /* 256 */

/* Map 4D (x,y,z,w) to KIS slot (0..20735) */
static uint32_t to_kis_slot(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    uint32_t vertex = (x * 12u + y) % 144u;
    uint32_t scale  = w % 144u;
    return vertex * 144u + scale;
}

/* Map 4D (x,y,z,w) to flat container index (0..255) */
static uint32_t to_flat(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    return (x % CX) * (CY * CZ * CW)
         + (y % CY) * (CZ * CW)
         + (z % CZ) * CW
         + (w % CW);
}

/* ═══════════════════════════════════════════════════════════════
   CONTAINER
   Filled with a pattern that encodes all 4 coordinates so
   you can tell which axis each value "belongs to".
   ═══════════════════════════════════════════════════════════════ */
static float container[TOTAL];

static void fill_container(void) {
    for (uint32_t x = 0; x < CX; x++)
    for (uint32_t y = 0; y < CY; y++)
    for (uint32_t z = 0; z < CZ; z++)
    for (uint32_t w = 0; w < CW; w++) {
        uint32_t idx = to_flat(x, y, z, w);
        /* Encode each axis as a power of 10 digit:
           w*1000 + z*100 + y*10 + x  →  reads as (w,z,y,x) digits */
        container[idx] = (float)(w * 1000u + z * 100u + y * 10u + x);
    }
}

/* ═══════════════════════════════════════════════════════════════
   KIS FIELD — stores pointer to container at computed slot
   ═══════════════════════════════════════════════════════════════ */
static uint64_t kis_field[SLOT_SZ / 8];  /* one slot = 64 bytes = 8 uint64s */
static uint32_t placed_slot;

static void place_on_field(void) {
    placed_slot = to_kis_slot(0, 0, 0, 0);
    /* Store container pointer in first 8 bytes of the slot */
    uintptr_t ptr = (uintptr_t)container;
    memcpy(kis_field, &ptr, sizeof(ptr));
}

static float read_weight(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    (void)placed_slot;
    uint32_t idx = to_flat(x, y, z, w);
    return container[idx];
}

/* ═══════════════════════════════════════════════════════════════
   BIRD'S EYE VIEWS
   Fix one axis at a constant, scan the other two to produce
   a 2D table. Each view produces a DIFFERENT table because
   different axes are on the rows/cols.
   ═══════════════════════════════════════════════════════════════ */

/* Helper: format a weight as a compact string for display.
   The encoding w*1000 + z*100 + y*10 + x means the digits
   read (w)(z)(y)(x) — so we can extract individual coords. */
static void format_val(float val, char *buf, int wide) {
    int v = (int)val;
    int x = v % 10;
    int y = (v / 10) % 10;
    int z = (v / 100) % 10;
    int w = (v / 1000) % 10;
    if (wide)
        snprintf(buf, 16, "%d%d%d%d", w, z, y, x);
    else
        snprintf(buf, 16, "%d%d%d%d", w, z, y, x);
}

/* ─── VIEW 1: From X-axis (look down X, scan Y vs Z) ─────────
   Fix X=const, fix W=const, scan Y (cols) × Z (rows)
   ═══════════════════════════════════════════════════════════════ */
static void view_from_x(uint32_t fix_x, uint32_t fix_w) {
    printf("┌─ VIEW FROM X-AXIS (X=%u fixed, W=%u fixed) ─────────────────┐\n", fix_x, fix_w);
    printf("│ Scanning: Y (columns) × Z (rows)                           │\n");
    printf("│ Each cell shows: wzYx  (encoded weight)                    │\n");
    printf("│                                                             │\n");
    printf("│       ");
    for (uint32_t y = 0; y < CY; y++)
        printf("  Y=%-4u", y);
    printf("       │\n");
    printf("│      ");
    for (uint32_t y = 0; y < CY; y++)
        printf(" ──────");
    printf("──    │\n");

    for (uint32_t z = 0; z < CZ; z++) {
        printf("│ Z=%u │", z);
        for (uint32_t y = 0; y < CY; y++) {
            float val = read_weight(fix_x, y, z, fix_w);
            char buf[16];
            format_val(val, buf, 1);
            printf(" %s ", buf);
        }
        printf(" │\n");
    }
    printf("└─────────────────────────────────────────────────────────────┘\n\n");
}

/* ─── VIEW 2: From Y-axis (look down Y, scan X vs Z) ─────────
   Fix Y=const, fix W=const, scan X (cols) × Z (rows)
   ═══════════════════════════════════════════════════════════════ */
static void view_from_y(uint32_t fix_y, uint32_t fix_w) {
    printf("┌─ VIEW FROM Y-AXIS (Y=%u fixed, W=%u fixed) ─────────────────┐\n", fix_y, fix_w);
    printf("│ Scanning: X (columns) × Z (rows)                           │\n");
    printf("│ Each cell shows: wzYx  (encoded weight)                    │\n");
    printf("│                                                             │\n");
    printf("│       ");
    for (uint32_t x = 0; x < CX; x++)
        printf("  X=%-4u", x);
    printf("       │\n");
    printf("│      ");
    for (uint32_t x = 0; x < CX; x++)
        printf(" ──────");
    printf("──    │\n");

    for (uint32_t z = 0; z < CZ; z++) {
        printf("│ Z=%u │", z);
        for (uint32_t x = 0; x < CX; x++) {
            float val = read_weight(x, fix_y, z, fix_w);
            char buf[16];
            format_val(val, buf, 1);
            printf(" %s ", buf);
        }
        printf(" │\n");
    }
    printf("└─────────────────────────────────────────────────────────────┘\n\n");
}

/* ─── VIEW 3: From Z-axis (look down Z, scan X vs Y) ─────────
   Fix Z=const, fix W=const, scan X (cols) × Y (rows)
   ═══════════════════════════════════════════════════════════════ */
static void view_from_z(uint32_t fix_z, uint32_t fix_w) {
    printf("┌─ VIEW FROM Z-AXIS (Z=%u fixed, W=%u fixed) ─────────────────┐\n", fix_z, fix_w);
    printf("│ Scanning: X (columns) × Y (rows)                           │\n");
    printf("│ Each cell shows: wzYx  (encoded weight)                    │\n");
    printf("│                                                             │\n");
    printf("│       ");
    for (uint32_t x = 0; x < CX; x++)
        printf("  X=%-4u", x);
    printf("       │\n");
    printf("│      ");
    for (uint32_t x = 0; x < CX; x++)
        printf(" ──────");
    printf("──    │\n");

    for (uint32_t y = 0; y < CY; y++) {
        printf("│ Y=%u │", y);
        for (uint32_t x = 0; x < CX; x++) {
            float val = read_weight(x, y, fix_z, fix_w);
            char buf[16];
            format_val(val, buf, 1);
            printf(" %s ", buf);
        }
        printf(" │\n");
    }
    printf("└─────────────────────────────────────────────────────────────┘\n\n");
}

/* ─── VIEW 4: From W-axis (look down W, scan X vs Y) ─────────
   Fix W=const, fix Z=const, scan X (cols) × Y (rows)
   ═══════════════════════════════════════════════════════════════ */
static void view_from_w(uint32_t fix_w, uint32_t fix_z) {
    printf("┌─ VIEW FROM W-AXIS (W=%u fixed, Z=%u fixed) ─────────────────┐\n", fix_w, fix_z);
    printf("│ Scanning: X (columns) × Y (rows)                           │\n");
    printf("│ Each cell shows: wzYx  (encoded weight)                    │\n");
    printf("│                                                             │\n");
    printf("│       ");
    for (uint32_t x = 0; x < CX; x++)
        printf("  X=%-4u", x);
    printf("       │\n");
    printf("│      ");
    for (uint32_t x = 0; x < CX; x++)
        printf(" ──────");
    printf("──    │\n");

    for (uint32_t y = 0; y < CY; y++) {
        printf("│ Y=%u │", y);
        for (uint32_t x = 0; x < CX; x++) {
            float val = read_weight(x, y, fix_z, fix_w);
            char buf[16];
            format_val(val, buf, 1);
            printf(" %s ", buf);
        }
        printf(" │\n");
    }
    printf("└─────────────────────────────────────────────────────────────┘\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   CROSS-AXIS COMPARISON
   Show the same 4 positions viewed from 2 different axes
   to prove the data "looks different" depending on viewpoint.
   ═══════════════════════════════════════════════════════════════ */
static void cross_axis_demo(void) {
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  CROSS-AXIS COMPARISON — Same data, different perspectives\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    /* Pick 4 positions that have distinct coordinates */
    uint32_t coords[][4] = {
        {0, 0, 0, 0},   /* origin */
        {1, 2, 3, 3},   /* deep interior */
        {3, 0, 1, 2},   /* edge */
        {2, 3, 1, 0},   /* another corner */
    };
    const char *labels[] = {"origin", "interior", "edge", "other corner"};

    printf("  Selected positions:\n");
    for (int i = 0; i < 4; i++) {
        uint32_t x = coords[i][0], y = coords[i][1];
        uint32_t z = coords[i][2], w = coords[i][3];
        float val = read_weight(x, y, z, w);
        uint32_t slot = to_kis_slot(x, y, z, w);
        char buf[16];
        format_val(val, buf, 1);
        printf("    [%s]  (%u,%u,%u,%u)  val=%s  KIS_slot=%u\n",
               labels[i], x, y, z, w, buf, slot);
    }
    printf("\n");

    /* View 1: X-axis perspective — group by (y,z) pairs */
    printf("  X-AXIS PERSPECTIVE (W=0 fixed, scan Y×Z):\n");
    printf("  Shows which X-values appear for each (Y,Z) pair.\n");
    printf("    Row Z, Col Y → cell = (X varies across sub-positions)\n");
    for (uint32_t z = 0; z < CZ; z++) {
        printf("    Z=%u: ", z);
        for (uint32_t y = 0; y < CY; y++) {
            /* Show what X values look like from this (y,z) */
            printf("(");
            for (uint32_t x = 0; x < CX; x++) {
                float val = read_weight(x, y, z, 0);
                int v = (int)val;
                printf("%d", v % 10);  /* last digit = x */
                if (x < CX - 1) printf(",");
            }
            printf(") ");
        }
        printf("\n");
    }
    printf("    → From X-axis: you see how X encodes position. Each (Y,Z) slice\n");
    printf("      shows X=0,1,2,3 — the X axis is 'in your face'.\n\n");

    /* View 2: W-axis perspective — group by (x,y) pairs */
    printf("  W-AXIS PERSPECTIVE (Z=0 fixed, scan X×Y):\n");
    printf("  Shows which W-values appear for each (X,Y) pair.\n");
    printf("    Row Y, Col X → cell = (W varies)\n");
    for (uint32_t y = 0; y < CY; y++) {
        printf("    Y=%u: ", y);
        for (uint32_t x = 0; x < CX; x++) {
            printf("(");
            for (uint32_t w = 0; w < CW; w++) {
                float val = read_weight(x, y, 0, w);
                int v = (int)val;
                printf("%d", v / 1000);  /* first digit = w */
                if (w < CW - 1) printf(",");
            }
            printf(") ");
        }
        printf("\n");
    }
    printf("    → From W-axis: you see how W encodes position. Each (X,Y) slice\n");
    printf("      shows W=0,1,2,3 — the W axis is 'in your face'.\n\n");

    /* View 3: Z-axis perspective — group by (x,y) pairs but varying Z */
    printf("  Z-AXIS PERSPECTIVE (W=0 fixed, scan X×Y):\n");
    printf("  Shows which Z-values appear for each (X,Y) pair.\n");
    printf("    Row Y, Col X → cell = (Z varies)\n");
    for (uint32_t y = 0; y < CY; y++) {
        printf("    Y=%u: ", y);
        for (uint32_t x = 0; x < CX; x++) {
            printf("(");
            for (uint32_t z = 0; z < CZ; z++) {
                float val = read_weight(x, y, z, 0);
                int v = (int)val;
                printf("%d", (v / 100) % 10);  /* hundreds digit = z */
                if (z < CZ - 1) printf(",");
            }
            printf(") ");
        }
        printf("\n");
    }
    printf("    → From Z-axis: the same (X,Y) pairs now show Z variation.\n");
    printf("      Same grid positions, DIFFERENT numbers — axis matters!\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   KIS SLOT VERIFICATION
   Show how 4D coords map to KIS layer slots
   ═══════════════════════════════════════════════════════════════ */
static void slot_mapping_demo(void) {
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  KIS SLOT MAPPING — 4D coordinates → slot addresses\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    printf("  slot = vertex × 144 + scale\n");
    printf("  vertex = (x * 12 + y) %% 144\n");
    printf("  scale  = w %% 144\n\n");

    printf("  %-6s  %-12s  %-8s  %-8s  %-10s  %-6s\n",
           "(x,y)", "(z,w)", "vertex", "scale", "slot", "layer");
    printf("  %-6s  %-12s  %-8s  %-8s  %-10s  %-6s\n",
           "──────", "────────────", "──────", "─────", "──────────", "─────");

    /* Show mapping for several representative positions */
    uint32_t test_coords[][4] = {
        {0,0,0,0}, {1,0,0,0}, {0,1,0,0}, {1,1,0,0},
        {0,0,1,0}, {0,0,0,1}, {3,3,3,3},
        {2,3,1,2}, {0,0,0,3},
    };
    int n = sizeof(test_coords) / sizeof(test_coords[0]);

    for (int i = 0; i < n; i++) {
        uint32_t x = test_coords[i][0], y = test_coords[i][1];
        uint32_t z = test_coords[i][2], w = test_coords[i][3];
        uint32_t vertex = (x * 12u + y) % 144u;
        uint32_t scale  = w % 144u;
        uint32_t slot   = to_kis_slot(x, y, z, w);

        /* Find which KIS layer this slot falls in */
        uint32_t layer = 0;
        uint64_t cumulative = 0;
        for (uint32_t L = 0; L < 20; L++) {
            uint32_t nslots = KIS_SLOTS(L);
            if (cumulative + nslots > slot) {
                layer = L;
                break;
            }
            cumulative += nslots;
            if (L == 19) layer = 19;
        }

        printf("  (%u,%u)  (%u,%u)        %-8u  %-8u  %-10u  %u (%s)\n",
               x, y, z, w, vertex, scale, slot, layer,
               (layer % 2 == 0) ? "icosa" : "dodeca");
    }
    printf("\n");
    printf("  Note: Z is NOT encoded in the KIS slot — only X,Y (vertex) and W (scale).\n");
    printf("  Z lives inside the container's own indexing.\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   AXIS SYMMETRY TEST
   Show that rotating which axis you view from changes the
   apparent "structure" of the data.
   ═══════════════════════════════════════════════════════════════ */
static void axis_symmetry_test(void) {
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  AXIS SYMMETRY — Same cube, rotated perspectives\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    /* Fix z=1, w=1 → look at the 4×4 X-Y slice
       Then rotate: show same data as if X were the "up" axis */
    printf("  SLICE at Z=1, W=1 — standard X×Y view:\n");
    printf("  (last 2 digits of value = Y×10 + X)\n\n");

    printf("        X=0   X=1   X=2   X=3\n");
    printf("       ───── ───── ───── ─────\n");
    for (uint32_t y = 0; y < CY; y++) {
        printf("  Y=%u │", y);
        for (uint32_t x = 0; x < CX; x++) {
            float val = read_weight(x, y, 1, 1);
            int v = (int)val;
            printf(" %3d ", v % 100);  /* last 2 digits = y*10+x */
        }
        printf("\n");
    }
    printf("\n");

    /* Now rotate: fix x=1, w=1 → look at Z×Y slice */
    printf("  SAME CUBE, rotated — fix X=1, W=1 → Z×Y view:\n");
    printf("  (digits = W×1000 + Z×100 + Y×10 + X, last 2 = Y×10+X)\n\n");

    printf("        Y=0   Y=1   Y=2   Y=3\n");
    printf("       ───── ───── ───── ─────\n");
    for (uint32_t z = 0; z < CZ; z++) {
        printf("  Z=%u │", z);
        for (uint32_t y = 0; y < CY; y++) {
            float val = read_weight(1, y, z, 1);
            int v = (int)val;
            printf(" %3d ", v % 100);
        }
        printf("\n");
    }
    printf("\n");
    printf("  → The 2D numbers are different because different axes map to\n");
    printf("    different digits. Rotation = axis permutation = different view.\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║   KIS BIRD'S EYE VIEW — All 4 Axes Simultaneously          ║\n");
    printf("║   Container: %ux%ux%ux%u = %u weights                    ║\n",
           CX, CY, CZ, CW, TOTAL);
    printf("║   KIS field: %u slots (144²)                          ║\n", 144*144);
    printf("║   Slot formula: vertex×144 + scale                          ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");

    /* Step 1: Fill container with coordinate-encoded data */
    fill_container();
    printf("Container filled. Each weight encodes its 4D position:\n");
    printf("  value = W×1000 + Z×100 + Y×10 + X\n");
    printf("  Example: value 3210 means (X=0, Y=1, Z=2, W=3)\n\n");

    /* Step 2: Place on KIS field */
    place_on_field();
    printf("Container placed at KIS slot %u (vertex=%u, scale=%u)\n\n",
           placed_slot, placed_slot / 144, placed_slot % 144);

    /* Step 3: Slot mapping */
    slot_mapping_demo();

    /* Step 4: All 4 bird's eye views */
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  ALL 4 BIRD'S EYE VIEWS — Same container, 4 perspectives\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    /* View from X-axis: fix X=2, W=1 */
    view_from_x(2, 1);

    /* View from Y-axis: fix Y=2, W=1 */
    view_from_y(2, 1);

    /* View from Z-axis: fix Z=1, W=1 */
    view_from_z(1, 1);

    /* View from W-axis: fix W=2, Z=0 — different Z and W from the Z-view */
    view_from_w(2, 0);

    /* Step 5: Cross-axis comparison */
    cross_axis_demo();

    /* Step 6: Axis symmetry */
    axis_symmetry_test();

    /* Summary */
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");
    printf("  1. The SAME container (256 weights) produces 4 DISTINCT\n");
    printf("     2D views depending on which axis you fix.\n\n");
    printf("  2. From X-axis: X varies across cells → you see X-structure\n");
    printf("     From Y-axis: Y varies across cells → you see Y-structure\n");
    printf("     From Z-axis: Z varies across cells → you see Z-structure\n");
    printf("     From W-axis: W varies across cells → you see W-structure\n\n");
    printf("  3. Each view captures a different 'slice' of the 4D object.\n");
    printf("     No single 2D view contains all the information.\n\n");
    printf("  4. KIS slots only encode X,Y (vertex) and W (scale).\n");
    printf("     Z is implicit — it lives in the container's flat index.\n\n");
    printf("  5. Bird's eye = fixing one axis to observe the remaining\n");
    printf("     structure. All 4 views together reconstruct the full 4D data.\n");

    return 0;
}
