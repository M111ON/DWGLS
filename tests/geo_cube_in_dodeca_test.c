/* geo_cube_in_dodeca_test.c — Verify cube-in-dodeca mapping */
#include "../core/geo_cube_in_dodeca.h"

int main(void) {
    printf("=== geo_cube_in_dodeca Test ===\n\n");
    
    /* 1. Stats & verification */
    geo_cube_in_dodeca_stats();
    printf("\n");
    
    /* 2. Test (n, k) addressing — generation n=3, all 6 faces */
    printf("DiamondBlock faces at generation n=3:\n");
    printf("─────────────────────────────────────\n");
    for (uint8_t axis = 0; axis < CUBE_AXES; axis++) {
        for (uint8_t sign = 0; sign < 2; sign++) {
            Vec3D center = half_axis_center(axis, sign, 3);
            const char *ax_name[] = {"X", "Y", "Z"};
            const char *sg_name[] = {"+", "-"};
            printf("  Face %s%s: center = (%.4f, %.4f, %.4f)\n",
                   ax_name[axis], sg_name[sign], center.x, center.y, center.z);
        }
    }
    printf("\n");
    
    /* 3. Cell types — all 8 parity combos */
    printf("Cell types (parity of nx, ny, nz):\n");
    printf("─────────────────────────────────────\n");
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t nx = i >> 2, ny = (i >> 1) & 1, nz = i & 1;
        CellType ct = cell_type_from_parity(nx, ny, nz);
        printf("  (%u,%u,%u) → CellType %d\n", nx, ny, nz, ct);
    }
    printf("\n");
    
    /* 4. Roundtrip: cell_type → parity → cell_type */
    printf("CellType roundtrip:\n");
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        uint8_t px, py, pz;
        cell_type_to_parity((CellType)i, &px, &py, &pz);
        CellType rt = cell_type_from_parity(px, py, pz);
        if (rt != (CellType)i) {
            printf("  FAIL: CellType %d → parity (%u,%u,%u) → CellType %d\n", i, px, py, pz, rt);
            ok = 0;
        }
    }
    printf("  %s\n", ok ? "PASS (8/8)" : "FAIL");
    
    /* 5. Generation scaling: gen_scale(n) = φ^n */
    printf("\nGeneration scaling:\n");
    for (uint32_t n = 0; n <= 6; n++) {
        printf("  n=%u: scale = %.6f (expected φ^%u = %.6f)\n",
               n, gen_scale(n), n, pow(PHI, n));
    }
    
    printf("\n=== ALL DONE ===\n");
    return 0;
}
