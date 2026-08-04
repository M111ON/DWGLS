/* test_cube_in_dodeca.c — Verify cube-in-dodecahedron mapping */
#include <stdio.h>
#include <math.h>
#include "../core/geo_cube_in_dodeca.h"

#define TEST_PASS(name) printf("  [PASS] %s\n", name)
#define TEST_FAIL(name, msg) printf("  [FAIL] %s: %s\n", name, msg)

int main(void) {
    printf("=== Cube-in-Dodecahedron Test Suite ===\n\n");
    
    int total = 0, passed = 0;
    
    /* Test 1: Cube vertices are in dodeca */
    total++;
    if (verify_cube_in_dodeca()) {
        TEST_PASS("Cube vertices are subset of dodeca vertices");
        passed++;
    } else {
        TEST_FAIL("Cube vertices in dodeca", "Not all cube vertices found in dodeca");
    }
    
    /* Test 2: φ ratio */
    total++;
    double ratio = phi_ratio();
    if (verify_phi_ratio(1e-6)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "φ ratio = %.10f (error %.2e)", ratio, fabs(ratio - PHI));
        TEST_PASS(msg);
        passed++;
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "φ ratio = %.10f, expected %.10f", ratio, PHI);
        TEST_FAIL("φ ratio", msg);
    }
    
    /* Test 3: Half-axes count */
    total++;
    if (verify_half_axes()) {
        TEST_PASS("6 half-axes = 6 faces (all unique vertices)");
        passed++;
    } else {
        TEST_FAIL("Half-axes", "Invalid half-axis structure");
    }
    
    /* Test 4: Cell types from parity */
    total++;
    {
        int cell_ok = 1;
        for (uint32_t nx = 0; nx < 4; nx++) {
            for (uint32_t ny = 0; ny < 4; ny++) {
                for (uint32_t nz = 0; nz < 4; nz++) {
                    CellType ct = cell_type_from_parity(nx, ny, nz);
                    uint8_t px, py, pz;
                    cell_type_to_parity(ct, &px, &py, &pz);
                    if (px != (nx & 1) || py != (ny & 1) || pz != (nz & 1)) {
                        cell_ok = 0;
                        break;
                    }
                }
                if (!cell_ok) break;
            }
            if (!cell_ok) break;
        }
        if (cell_ok) {
            TEST_PASS("Cell type parity mapping (8 types)");
            passed++;
        } else {
            TEST_FAIL("Cell type parity", "Roundtrip failed");
        }
    }
    
    /* Test 5: Address mapping */
    total++;
    {
        /* n=0 should give unit cube vertices */
        Vec3D v0 = cube_address_to_xyz(0, 0);
        Vec3D v1 = cube_address_to_xyz(0, 1);
        double dist = vec3_distance(v0, v1);
        double expected = 2.0; /* distance between (1,1,1) and (1,1,-1) = 2 */
        if (fabs(dist - expected) < 1e-10) {
            TEST_PASS("Address mapping n=0 gives unit cube");
            passed++;
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Distance = %.6f, expected %.6f", dist, expected);
            TEST_FAIL("Address mapping", msg);
        }
    }
    
    /* Test 6: Scaling by generation */
    total++;
    {
        Vec3D v0 = cube_address_to_xyz(0, 0);
        Vec3D v1 = cube_address_to_xyz(1, 0);
        /* v1 should be φ times larger than v0 */
        double scale = v1.x / v0.x;
        if (fabs(scale - PHI) < 1e-6) {
            TEST_PASS("Generation scaling by φ");
            passed++;
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Scale = %.6f, expected %.6f", scale, PHI);
            TEST_FAIL("Generation scaling", msg);
        }
    }
    
    /* Test 7: Half-axis centers */
    total++;
    {
        /* X+ axis at n=0 should have center at (1, 0, 0) */
        Vec3D center = half_axis_center(CUBE_AXIS_X, CUBE_SIGN_POS, 0);
        if (fabs(center.x - 1.0) < 1e-10 &&
            fabs(center.y - 0.0) < 1e-10 &&
            fabs(center.z - 0.0) < 1e-10) {
            TEST_PASS("Half-axis center X+ at n=0 = (1,0,0)");
            passed++;
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Center = (%.4f, %.4f, %.4f)", center.x, center.y, center.z);
            TEST_FAIL("Half-axis center", msg);
        }
    }
    
    /* Test 8: Pentagon edge calculation */
    total++;
    {
        double pe = pentagon_edge();
        /* Pentagon edge should be positive and reasonable */
        if (pe > 0.0 && pe < 5.0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Pentagon edge = %.6f", pe);
            TEST_PASS(msg);
            passed++;
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Pentagon edge = %.6f (out of range)", pe);
            TEST_FAIL("Pentagon edge", msg);
        }
    }
    
    /* Print stats */
    printf("\n");
    geo_cube_in_dodeca_stats();
    
    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", passed, total);
    
    return (passed == total) ? 0 : 1;
}
