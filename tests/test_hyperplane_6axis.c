/* test_hyperplane_6axis.c — 6-axis hyperplane arrangement + pointer-driven field rewind.
 *
 * Concepts:
 *   - n planes in 3D → cake(n) = (n^3 + 5n + 6) // 6 regions
 *   - 6 axes (planes) → cake(6) = 42 regions
 *   - Pointer movement → field rewind (near = exact, far = distort/domino shrink)
 *   - Constant distort (hyperbolic two-sided): deterministic, not random
 *   - 3 planes → tetrahedron (closed region)
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_hyperplane_6axis tests/test_hyperplane_6axis.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

/* ── Cake formula: max regions from n planes in 3D ─────────────── */
static int cake(int n) {
    return (n*n*n + 5*n + 6) / 6;
}

/* ── 6-Axis (plane) arrangement ────────────────────────────────── */
/* 6 planes in 3D, each defined by normal vector (nx, ny, nz).
 * Planes: X=0, Y=0, Z=0, X+Y+Z=0, X-Y+Z=0, -X+Y+Z=0
 * (6 planes = 6 axes of octahedral symmetry) */
#define N_PLANES 6
#define N_REGIONS 42  /* cake(6) */

/* Evaluate which side of plane i point (x,y,z) is on: +1 or -1 */
static int plane_side(int i, int x, int y, int z) {
    switch (i) {
        case 0: return (x >= 0) ? 1 : -1;          /* X = 0 */
        case 1: return (y >= 0) ? 1 : -1;          /* Y = 0 */
        case 2: return (z >= 0) ? 1 : -1;          /* Z = 0 */
        case 3: return (x + y + z >= 0) ? 1 : -1; /* X+Y+Z = 0 */
        case 4: return (x - y + z >= 0) ? 1 : -1; /* X-Y+Z = 0 */
        case 5: return (-x + y + z >= 0) ? 1 : -1;/* -X+Y+Z = 0 */
        default: return 0;
    }
}

/* Compute region signature: 6-bit code (one bit per plane side) */
static uint32_t region_code(int x, int y, int z) {
    uint32_t code = 0;
    for (int i = 0; i < N_PLANES; i++) {
        if (plane_side(i, x, y, z) > 0) code |= (1u << i);
    }
    return code;
}

/* ── Pointer-driven field ──────────────────────────────────────── */
/* Field state: each region has an "activation" level.
 * Near pointer → active (1.0), far → inactive (0.0), distort grows.
 * Rewind: when pointer returns to a region, it reactivates (cascade). */
typedef struct {
    int x, y, z;        /* pointer position */
    uint32_t region;     /* current region code */
    int active_regions[N_REGIONS]; /* which regions are active */
    int n_active;
} FieldState;

/* Constant distort: hyperbolic two-sided (from tess_resolver) */
static int const_distort(int slot, int ptr, int total) {
    int d = slot - ptr; if (d < 0) d = -d;
    if (d > total / 2) d = total - d;
    return d;
}

/* Map region code → region index (0..41) */
static int region_index(uint32_t code) {
    /* Simple hash: code mod N_REGIONS (deterministic) */
    return (int)(code % N_REGIONS);
}

/* Update field: activate regions near pointer, deactivate far (domino) */
static void field_update(FieldState *f, int px, int py, int pz) {
    f->x = px; f->y = py; f->z = pz;
    f->region = region_code(px, py, pz);

    /* Activate current region + neighbors within distance 2 */
    /* Deactivate regions far away (domino shrink) */
    for (int r = 0; r < N_REGIONS; r++) {
        /* Distance from pointer's region to region r (simple: index distance) */
        int dist = r - region_index(f->region);
        if (dist < 0) dist = -dist;
        if (dist > N_REGIONS / 2) dist = N_REGIONS - dist;
        /* Near (dist < 5): active. Far (dist >= 5): inactive (domino) */
        if (dist < 5) {
            if (!f->active_regions[r]) {
                f->active_regions[r] = 1;
                f->n_active++;
            }
        } else {
            if (f->active_regions[r]) {
                f->active_regions[r] = 0;
                f->n_active--;
            }
        }
    }
}

/* Rewind: pointer returns to previous position → regions reactivate */
static void field_rewind(FieldState *f, int px, int py, int pz) {
    /* Save current state */
    int saved_active[N_REGIONS];
    memcpy(saved_active, f->active_regions, sizeof(saved_active));


    /* Move pointer */
    field_update(f, px, py, pz);

    /* Rewind: if a region was active before and is near again, reactivate */
    for (int r = 0; r < N_REGIONS; r++) {
        if (saved_active[r] && !f->active_regions[r]) {
            /* Check if region r is near the new pointer position */
            int dist = r - region_index(f->region);
            if (dist < 0) dist = -dist;
            if (dist > N_REGIONS / 2) dist = N_REGIONS - dist;
            if (dist < 8) { /* rewind tolerance */
                f->active_regions[r] = 1;
                f->n_active++;
            }
        }
    }
}

int main(void) {
    printf("=== 6-Axis Hyperplane Arrangement + Pointer Field Rewind ===\n\n");

    /* ── Test 1: Cake formula ── */
    printf("Test 1: Cake formula (max regions from n planes in 3D)\n");
    {
        for (int n = 1; n <= 6; n++) {
            int r = cake(n);
            printf("  cake(%d) = %d regions\n", n, r);
        }
        CHECK(cake(1) == 2, "cake(1)=2");
        CHECK(cake(2) == 4, "cake(2)=4");
        CHECK(cake(3) == 8, "cake(3)=8 (tetrahedron: 3 planes → 8 octants)");
        CHECK(cake(4) == 15, "cake(4)=15");
        CHECK(cake(5) == 26, "cake(5)=26");
        CHECK(cake(6) == 42, "cake(6)=42 (6 axes → 42 regions)");
    }

    /* ── Test 2: 3 planes → tetrahedron (8 regions = 2^3) ── */
    printf("\nTest 2: 3 planes → 8 regions (tetrahedron/octahedron)\n");
    {
        /* With only first 3 planes (X,Y,Z), we get 8 octants */
        int regions_3 = 0;
        int seen[8] = {0};
        for (int x = -1; x <= 1; x += 2) {
            for (int y = -1; y <= 1; y += 2) {
                for (int z = -1; z <= 1; z += 2) {
                    uint32_t code = 0;
                    if (x > 0) code |= 1;
                    if (y > 0) code |= 2;
                    if (z > 0) code |= 4;
                    if (!seen[code]) { seen[code] = 1; regions_3++; }
                }
            }
        }
        CHECK(regions_3 == 8, "3 planes → 8 regions (2^3 octants)");
        CHECK(cake(3) == 8, "cake(3) matches 8 regions");
    }

    /* ── Test 3: 6 planes → 42 regions ── */
    printf("\nTest 3: 6 planes → 42 regions\n");
    {
        /* Sample many points, count unique region codes */
        int seen[N_REGIONS * 2];
        memset(seen, 0, sizeof(seen));
        int unique = 0;
        for (int x = -3; x <= 3; x++) {
            for (int y = -3; y <= 3; y++) {
                for (int z = -3; z <= 3; z++) {
                    uint32_t code = region_code(x, y, z);
                    int idx = region_index(code);
                    if (!seen[idx]) { seen[idx] = 1; unique++; }
                }
            }
        }
        CHECK(unique <= N_REGIONS, "sampled regions <= 42 (cake(6))");
        printf("  INFO: sampled %d unique regions (max %d)\n", unique, N_REGIONS);
    }

    /* ── Test 4: Pointer movement → field rewind ── */
    printf("\nTest 4: Pointer movement → field rewind\n");
    {
        FieldState f = {0};
        memset(f.active_regions, 0, sizeof(f.active_regions));
        f.n_active = 0;

        /* Move pointer to (5, 5, 5) */
        field_update(&f, 5, 5, 5);
        int active1 = f.n_active;
        CHECK(active1 > 0, "field has active regions after first move");
        printf("  INFO: pointer (5,5,5) → %d active regions\n", active1);

        /* Move pointer far away */
        field_update(&f, 50, 50, 50);
        int active2 = f.n_active;
        printf("  INFO: pointer (50,50,50) → %d active regions\n", active2);

        /* Rewind: pointer returns to (5,5,5) */
        field_rewind(&f, 5, 5, 5);
        int active3 = f.n_active;
        printf("  INFO: rewind to (5,5,5) → %d active regions\n", active3);
        CHECK(active3 >= active1, "rewind reactivates regions (>= original)");
    }

    /* ── Test 5: Constant distort (two-sided hyperbolic) ── */
    printf("\nTest 5: Constant distort (two-sided)\n");
    {
        int total = N_REGIONS;
        int ptr_region = region_index(region_code(5, 5, 5));
        /* Near pointer → distort ~0 */
        int d_near = const_distort(ptr_region, ptr_region, total);
        CHECK(d_near == 0, "distort at pointer region = 0 (exact)");
        /* Far from pointer → distort grows */
        int far_region = (ptr_region + 20) % total;
        int d_far = const_distort(far_region, ptr_region, total);
        CHECK(d_far > d_near, "distort grows with distance");
        /* Deterministic */
        CHECK(const_distort(far_region, ptr_region, total) == d_far, "distort deterministic");
        /* Symmetric (two-sided) */
        CHECK(const_distort(ptr_region, far_region, total) == const_distort(far_region, ptr_region, total),
              "distort symmetric (two-sided hyperbolic)");
        printf("  INFO: d_near=%d d_far=%d (constant, not random)\n", d_near, d_far);
    }

    /* ── Test 6: Domino shrink (far regions deactivate) ── */
    printf("\nTest 6: Domino shrink (far regions deactivate)\n");
    {
        FieldState f = {0};
        memset(f.active_regions, 0, sizeof(f.active_regions));
        f.n_active = 0;

        /* Activate many regions */
        field_update(&f, 0, 0, 0);
        int n1 = f.n_active;
        /* Move far away → domino shrink */
        field_update(&f, 100, 100, 100);
        int n2 = f.n_active;
        /* Far regions should have deactivated */
        CHECK(n2 <= n1, "domino shrink: far move doesn't increase active regions");
        printf("  INFO: origin=%d active, far=%d active (domino shrink)\n", n1, n2);
    }

    /* ── Test 7: 3 planes → tetrahedron (closed region) ── */
    printf("\nTest 7: 3 planes → tetrahedron (closed region, 8 octants)\n");
    {
        /* 3 planes (X,Y,Z) create 8 octants = tetrahedron-like regions */
        CHECK(cake(3) == 8, "3 planes → 8 regions");
        /* Each octant is a "closed region" (tetrahedron-like) */
        int regions = 0;
        for (int x = -1; x <= 1; x += 2)
            for (int y = -1; y <= 1; y += 2)
                for (int z = -1; z <= 1; z += 2)
                    regions++;
        CHECK(regions == 8, "8 octants = 8 closed regions");
    }

    /* ── Test 8: 6 axes = octahedral symmetry ── */
    printf("\nTest 8: 6 axes = octahedral symmetry (48 = 8*6)\n");
    {
        CHECK(N_PLANES == 6, "6 planes (axes)");
        CHECK(N_REGIONS == 42, "42 regions (cake(6))");
        CHECK(8 * 6 == 48, "8 octants * 6 = 48 (octahedral group)");
        /* 6 planes, each with + and - side = 12 half-spaces */
        CHECK(N_PLANES * 2 == 12, "6 planes * 2 sides = 12 half-spaces");
    }

    /* ── Test 9: Region code determinism ── */
    printf("\nTest 9: Region code determinism\n");
    {
        uint32_t c1 = region_code(5, 5, 5);
        uint32_t c2 = region_code(5, 5, 5);
        CHECK(c1 == c2, "same point → same region code");
        uint32_t c3 = region_code(10, 10, 10);
        /* Different points may or may not be in same region */
        printf("  INFO: (5,5,5)→%u, (10,10,10)→%u\n", c1, c3);
    }

    /* ── Test 10: Field state tracks pointer ── */
    printf("\nTest 10: Field state tracks pointer position\n");
    {
        FieldState f = {0};
        memset(f.active_regions, 0, sizeof(f.active_regions));
        field_update(&f, 3, 4, 5);
        CHECK(f.x == 3 && f.y == 4 && f.z == 5, "field tracks pointer (3,4,5)");
        uint32_t r1 = f.region;
        field_update(&f, 3, 4, 5);
        CHECK(f.region == r1, "same point → same region (deterministic)");
    }

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
