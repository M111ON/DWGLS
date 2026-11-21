/* test_field_focus.c — Demonstrate field focus/out-of-focus behavior.
 *
 * Shows:
 *   1. Field with pointer at position A → near regions in-focus (exact), far regions out-of-focus (distort)
 *   2. Pointer moves to B → focus shifts, old regions shrink (domino)
 *   3. Pointer returns to A → regions re-activate (rewind)
 *   4. Out-of-focus regions are NOT computed (lazy evaluation)
 *   5. Constant distort = f(distance) — deterministic, not random
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_field_focus tests/test_field_focus.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tess_resolver.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

/* Field region state */
#define N_REGIONS 42
typedef struct {
    int active;       /* 1 = in-focus (exact), 0 = out-of-focus (distort) */
    int distort;      /* distort value (0 = exact, >0 = distort) */
    int computed;     /* 1 = was computed (lazy eval), 0 = never computed */
} RegionState;

typedef struct {
    int ptr_region;           /* pointer's current region */
    RegionState regions[N_REGIONS];
    int n_active;             /* count of in-focus regions */
    int n_computed;           /* count of computed regions (lazy eval) */
} Field;

static void field_init(Field *f) {
    memset(f, 0, sizeof(*f));
    f->ptr_region = -1;
}

static void field_move(Field *f, int new_region) {
    f->ptr_region = new_region;
    for (int r = 0; r < N_REGIONS; r++) {
        int dist = r - new_region;
        if (dist < 0) dist = -dist;
        if (dist > N_REGIONS / 2) dist = N_REGIONS - dist;

        /* Near (dist < 5): in-focus (exact, distort=0) */
        /* Far (dist >= 5): out-of-focus (distort>0, not computed unless read) */
        if (dist < 5) {
            if (!f->regions[r].active) {
                f->regions[r].active = 1;
                f->regions[r].distort = 0;
                f->n_active++;
            }
            f->regions[r].computed = 1;
            f->n_computed++;
        } else {
            if (f->regions[r].active) {
                f->regions[r].active = 0;
                f->n_active--;
            }
            /* Out of focus: distort = constant (hyperbolic), NOT computed */
            f->regions[r].distort = tess_distort(r, new_region, N_REGIONS);
            /* NOT computed — lazy eval (only compute when pointer reads) */
        }
    }
}

static void field_read(Field *f, int region) {
    /* Multi-pointer read: temporarily unfold (Voronoi mask) */
    if (!f->regions[region].computed) {
        f->regions[region].computed = 1;
        f->n_computed++;
        /* Cure: distort → 0 (exact) for this region only */
        f->regions[region].distort = 0;
    }
}

static void field_print(Field *f, const char *label) {
    printf("\n  [%s] pointer=region %d, active=%d, computed=%d/%d\n",
           label, f->ptr_region, f->n_active, f->n_computed, N_REGIONS);
    printf("  Region  State        Distort  Computed\n");
    printf("  ------  -----------  -------  --------\n");
    for (int r = 0; r < N_REGIONS; r++) {
        const char *state = f->regions[r].active ? "IN-FOCUS " : "OUT-FOCUS";
        printf("  %3d     %-11s  %7d  %s\n",
               r, state, f->regions[r].distort,
               f->regions[r].computed ? "yes" : "NO (lazy)");
    }
}

int main(void) {
    printf("=== Field Focus / Out-of-Focus Demonstration ===\n");

    Field f;
    field_init(&f);

    /* ── Step 1: Pointer at region 5 ── */
    printf("\n--- Step 1: Pointer moves to region 5 ---\n");
    field_move(&f, 5);
    field_print(&f, "ptr=5");
    CHECK(f.n_active > 0, "regions activated near pointer");
    CHECK(f.n_computed < N_REGIONS, "not all regions computed (lazy eval)");
    int active1 = f.n_active;
    int computed1 = f.n_computed;

    /* Verify: near regions are in-focus (distort=0) */
    CHECK(f.regions[5].active == 1, "pointer region is in-focus");
    CHECK(f.regions[5].distort == 0, "pointer region distort=0 (exact)");
    /* Far regions are out-of-focus (distort>0) */
    CHECK(f.regions[20].active == 0, "far region is out-of-focus");
    CHECK(f.regions[20].distort > 0, "far region has distort>0");
    CHECK(f.regions[20].computed == 0, "far region NOT computed (lazy)");

    /* ── Step 2: Pointer moves to region 20 (far away) ── */
    printf("\n--- Step 2: Pointer moves to region 20 (far) ---\n");
    field_move(&f, 20);
    field_print(&f, "ptr=20");
    /* Old regions near 5 should have deactivated (domino shrink) */
    CHECK(f.regions[5].active == 0, "old region 5 deactivated (domino shrink)");
    CHECK(f.regions[5].distort > 0, "old region 5 now has distort>0");
    /* New region 20 should be in-focus */
    CHECK(f.regions[20].active == 1, "new region 20 in-focus");
    CHECK(f.regions[20].distort == 0, "new region 20 distort=0 (exact)");

    /* ── Step 3: Rewind — pointer returns to region 5 ── */
    printf("\n--- Step 3: Rewind — pointer returns to region 5 ---\n");
    /* Use field_move (which reactivates near regions) */
    field_move(&f, 5);
    field_print(&f, "ptr=5 (rewind)");
    CHECK(f.regions[5].active == 1, "region 5 re-activated (rewind)");
    CHECK(f.regions[5].distort == 0, "region 5 distort=0 again (exact)");
    printf("  INFO: rewind restored %d active regions\n", f.n_active);

    /* ── Step 4: Multi-pointer read (Voronoi mask) ── */
    printf("\n--- Step 4: Multi-pointer read (out-of-focus region 30) ---\n");
    /* Region 30 is out of focus. Read it with multi-pointer (Voronoi mask). */
    int was_computed = f.regions[30].computed;
    field_read(&f, 30);
    CHECK(f.regions[30].computed == 1, "region 30 now computed (Voronoi unfold)");
    CHECK(f.regions[30].distort == 0, "region 30 cured → distort=0 (exact)");
    printf("  INFO: region 30 was %s, now computed + cured\n",
           was_computed ? "already computed" : "NOT computed (lazy)");

    /* ── Step 5: Lazy evaluation verification ── */
    printf("\n--- Step 5: Lazy evaluation verification ---\n");
    /* Count how many regions were NEVER computed */
    int never_computed = 0;
    for (int r = 0; r < N_REGIONS; r++) {
        if (!f.regions[r].computed) never_computed++;
    }
    CHECK(never_computed > 0, "some regions never computed (lazy eval)");
    printf("  INFO: %d/%d regions never computed (lazy — no wasted work)\n",
           never_computed, N_REGIONS);

    /* ── Step 6: Constant distort verification ── */
    printf("\n--- Step 6: Constant distort verification ---\n");
    /* Distort is deterministic: same distance → same distort */
    int d1 = tess_distort(5, 5, N_REGIONS);
    int d2 = tess_distort(5, 5, N_REGIONS);
    CHECK(d1 == d2, "distort deterministic (same input → same output)");
    CHECK(d1 == 0, "distort at pointer = 0 (exact)");
    /* Symmetric (two-sided hyperbolic) */
    int d3 = tess_distort(5, 20, N_REGIONS);
    int d4 = tess_distort(20, 5, N_REGIONS);
    CHECK(d3 == d4, "distort symmetric (two-sided)");

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);

    /* ── Summary ── */
    printf("\n=== Field Behavior Summary ===\n");
    printf("  In-focus regions:    %d (exact, distort=0, computed)\n", f.n_active);
    printf("  Out-of-focus regions: %d (distort>0, lazy eval)\n", N_REGIONS - f.n_active);
    printf("  Never computed:      %d (no wasted work)\n", never_computed);
    printf("  Total regions:       %d (cake(6)=42)\n", N_REGIONS);
    printf("\n  Concept verified:\n");
    printf("  - Pointer near → in-focus (exact, computed)\n");
    printf("  - Pointer far → out-of-focus (distort, NOT computed)\n");
    printf("  - Pointer returns → rewind (regions re-activate)\n");
    printf("  - Multi-pointer read → Voronoi unfold (cure distort→0)\n");
    printf("  - Lazy evaluation: only compute what's needed\n");

    return fail > 0 ? 1 : 0;
}
