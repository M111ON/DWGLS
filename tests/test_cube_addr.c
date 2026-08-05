/* test_cube_addr.c — Verify geo_cube_addr with w(time) */
#include "../core/geo_cube_addr.h"

int main(void) {
    printf("=== GeoCubeAddr Test ===\n\n");
    
    /* 1. Stats */
    geo_cube_addr_stats(8);
    printf("\n");
    
    /* 2. Roundtrip verification */
    printf("Roundtrip verification (gen 0-5):\n");
    verify_addr_roundtrip(5);
    printf("\n");
    
    /* 3. w(time) modulation */
    printf("w(time) verification:\n");
    verify_w_time();
    printf("\n");
    
    /* 4. Sample addresses at different generations */
    printf("Sample addresses:\n");
    printf("─────────────────────────────────────────────────────\n");
    for (uint32_t g = 0; g <= 6; g++) {
        GeoCubeAddr a = geo_cube_addr(g, 0, 0);  /* face X+, slot 0 */
        Vec3D pos = geo_cube_addr_to_xyz(a);
        uint32_t flat = geo_cube_addr_to_flat(a);
        printf("  gen=%u face=X+ slot=0 → pos=(%.3f,%.3f,%.3f) flat=%u cell=%s\n",
               g, pos.x, pos.y, pos.z, flat, cell_type_name(a.cell_type));
    }
    printf("\n");
    
    /* 5. w(time) effect on same address */
    printf("w(time) effect (gen=3, face=X+, slot=0):\n");
    printf("─────────────────────────────────────────────────────\n");
    double w_values[] = {0.1, 0.5, 1.0, 2.0, 5.0, 10.0};
    for (int i = 0; i < 6; i++) {
        GeoCubeAddr a = geo_cube_addr(3, 0, 0);
        a = geo_cube_addr_w(a, w_values[i]);
        Vec3D pos = geo_cube_addr_to_xyz(a);
        printf("  w=%.1f → pos=(%.3f,%.3f,%.3f)\n",
               w_values[i], pos.x, pos.y, pos.z);
    }
    printf("\n");
    
    /* 6. Cell type distribution across faces */
    printf("Cell types across faces (gen=2):\n");
    printf("─────────────────────────────────────────────────────\n");
    for (uint8_t f = 0; f < CUBE_ADDR_FACES; f++) {
        GeoCubeAddr a = geo_cube_addr(2, f, 0);
        const char *face_names[] = {"X+", "X-", "Y+", "Y-", "Z+", "Z-"};
        printf("  face %s → cell_type=%s (%d)\n",
               face_names[f], cell_type_name(a.cell_type), a.cell_type);
    }
    printf("\n");
    
    printf("=== ALL DONE ===\n");
    return 0;
}
