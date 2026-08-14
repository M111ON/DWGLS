/* beam_formula_test.c — Value→Cell formula (no sort, no permutation)
 *
 * Formula: zone + position → cell coordinate
 *   zone  = code >> 4  (0..15)  = class
 *   pos   = code & 0xF (0..15)  = sub-class
 *   cell  = f(zone, pos)        = deterministic, O(1)
 *
 * Key: f() must cluster same-zone values together
 *      and distribute sub-positions within zone space
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "gguf_reader.h"

#define DIM     10
#define N_CELLS (DIM*DIM*DIM)
#define N_ZONES 16
#define POS_PER_ZONE 16

static uint8_t cubes[N_ZONES][N_CELLS];

/* ── Formula: zone + position → cell ──────────────────────────
 *
 * Zone occupies a 3D sub-cube of the full 10×10×10 cube.
 * 16 zones → each zone gets a region.
 * Position 0..15 maps within the zone region.
 *
 * Layout: zone z → (zx, zy, zz) grid 4×4×1 = 16 sub-regions
 *   Each sub-region = 2×2×5 = 20 cells (or similar)
 *   Position 0..15 → specific cell within sub-region
 */

/* Zone → sub-cube origin (4×4×1 layout on X-Y plane) */
static inline void zone_origin(int zone, int *ox, int *oy, int *oz) {
    /* 16 zones on 4×4 grid in X-Y, Z=0 */
    *ox = (zone % 4) * (DIM / 4);  /* 0, 2, 5, 7 */
    *oy = (zone / 4) * (DIM / 4);
    *oz = 0;
}

/* Position → offset within sub-cube (2×2×5 = 20 cells, use 16) */
static inline void pos_offset(int pos, int *dx, int *dy, int *dz) {
    /* 16 positions → 4×4×1 layout within sub-region */
    *dx = pos % 4;
    *dy = pos / 4;
    *dz = 0;
}

/* Full formula: weight code → cell in 10×10×10 cube */
static inline void code_to_cell(int code, int *cx, int *cy, int *cz) {
    int zone = code >> 4;
    int pos = code & 0xF;
    int ox, oy, oz, dx, dy, dz;
    zone_origin(zone, &ox, &oy, &oz);
    pos_offset(pos, &dx, &dy, &dz);
    *cx = (ox + dx) % DIM;
    *cy = (oy + dy) % DIM;
    *cz = (oz + dz) % DIM;
}

/* Project one zone cube */
static int project_zone(int zone, uint64_t *active, uint64_t *ghost) {
    uint8_t *cube = cubes[zone];
    uint64_t n_active = 0;
    for (int i = 0; i < N_CELLS; i++) n_active += cube[i];
    if (n_active == 0) { *active = 0; *ghost = 0; return 0; }

    uint8_t mx[DIM][DIM], my[DIM][DIM], mz[DIM][DIM];
    memset(mx, 0, sizeof(mx));
    memset(my, 0, sizeof(my));
    memset(mz, 0, sizeof(mz));

    for (int x = 0; x < DIM; x++)
        for (int y = 0; y < DIM; y++)
            for (int z = 0; z < DIM; z++)
                if (cube[x*DIM*DIM + y*DIM + z]) {
                    mx[y][z] = 1;
                    my[x][z] = 1;
                    mz[x][y] = 1;
                }

    uint64_t n_ghost = 0;
    for (int x = 0; x < DIM; x++)
        for (int y = 0; y < DIM; y++)
            for (int z = 0; z < DIM; z++) {
                uint8_t r = mx[y][z] & my[x][z] & mz[x][y];
                if (r && !cube[x*DIM*DIM + y*DIM + z]) n_ghost++;
            }

    *active = n_active;
    *ghost = n_ghost;
    return 1;
}

/* Value distribution per zone */
static int zone_count[N_ZONES];

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] :
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf";

    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("Cannot open %s\n", path); return 1; }

    int idx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { idx = (int)i; break; }
    }
    if (idx < 0) { printf("No Q8_0 tensor\n"); gguf_close(gf); return 1; }

    GGUF_Tensor *t = &gf->tensors[idx];
    uint64_t n_blocks = t->n_weights / 32;
    printf("Tensor: %s  (%d weights)\n", t->name, (int)t->n_weights);

    uint64_t data_start = gf->tensor_data_start + t->offset;
    data_start = (data_start + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)data_start, SEEK_SET);

    /* ═══════════════════════════════════════════════════════════
       Test 1: Formula mapping — value → cell via zone+pos formula
       ═══════════════════════════════════════════════════════════ */
    printf("\n═══ Test 1: Formula mapping (no sort, no permutation) ═══\n");
    fseek(gf->fp, (long)data_start, SEEK_SET);
    memset(cubes, 0, sizeof(cubes));
    memset(zone_count, 0, sizeof(zone_count));

    uint64_t grand_active = 0, grand_ghost = 0;
    int zones_used = 0;
    uint64_t cells_filled = 0;

    for (uint64_t b = 0; b < n_blocks && cells_filled < 100*N_CELLS; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;

        for (int i = 0; i < 32; i++) {
            int code = (int)w[i] + 128;  /* 0..255 */
            int zone = code >> 4;
            int cx, cy, cz;
            code_to_cell(code, &cx, &cy, &cz);
            uint32_t cell = cx*DIM*DIM + cy*DIM + cz;
            cubes[zone][cell % N_CELLS] = 1;
            zone_count[zone]++;
            cells_filled++;
        }
    }

    for (int z = 0; z < N_ZONES; z++) {
        uint64_t active, ghost;
        if (project_zone(z, &active, &ghost)) {
            zones_used++;
            grand_active += active;
            grand_ghost += ghost;
        }
    }

    printf("Cubes filled: %d  Cells: %d\n", (int)(cells_filled/N_CELLS), (int)cells_filled);
    printf("Active: %d  Ghost: %d (%.1f%%)\n",
        (int)grand_active, (int)grand_ghost,
        grand_active ? (double)grand_ghost/grand_active*100 : 0);

    /* Cost */
    uint64_t n_cubes = cells_filled / N_CELLS + (cells_filled % N_CELLS ? 1 : 0);
    uint64_t map_bits = n_cubes * N_ZONES * 3 * DIM * DIM;
    uint64_t ghost_bits = grand_ghost * 10;
    uint64_t raw_bits = cells_filled * 10;

    printf("\n  Map:   %d bits\n", (int)map_bits);
    printf("  Ghost: %d bits\n", (int)ghost_bits);
    printf("  Total: %d bits\n", (int)(map_bits + ghost_bits));
    printf("  Raw:   %d bits\n", (int)raw_bits);
    printf("  Ratio: %.4fx\n", (double)(map_bits + ghost_bits) / (double)raw_bits);
    printf("  Sort cost: ZERO\n");

    /* ═══════════════════════════════════════════════════════════
       Test 2: Collision analysis — how many values share same cell?
       ═══════════════════════════════════════════════════════════ */
    printf("\n═══ Test 2: Cell collision analysis ═══\n");

    /* Count how many Q8 codes map to each cell per zone */
    int collision[16] = {0};
    int unique_cells[16] = {0};

    for (int code = 0; code < 256; code++) {
        int zone = code >> 4;
        int cx, cy, cz;
        code_to_cell(code, &cx, &cy, &cz);
        uint32_t cell = cx*DIM*DIM + cy*DIM + cz;

        /* Check if this cell already has data from a different code */
        static int zone_cells[16][N_CELLS];
        static int zone_cells_init = 0;
        if (!zone_cells_init) {
            memset(zone_cells, 0, sizeof(zone_cells));
            zone_cells_init = 1;
        }

        if (zone_cells[zone][cell % N_CELLS]) {
            collision[zone]++;
        } else {
            zone_cells[zone][cell % N_CELLS] = 1;
            unique_cells[zone]++;
        }
    }

    printf("Zone  Collision  Unique cells  Codes/Cell\n");
    for (int z = 0; z < N_ZONES; z++) {
        int codes_in_zone = 0;
        for (int c = z*16; c < (z+1)*16; c++) codes_in_zone++;
        double density = (double)codes_in_zone / (double)unique_cells[z];
        printf("  %2d   %3d        %3d           %.1f\n",
            z, collision[z], unique_cells[z], density);
    }

    /* ═══════════════════════════════════════════════════════════
       Test 3: Sub-cube projection (per zone, project within zone space)
       ═══════════════════════════════════════════════════════════ */
    printf("\n═══ Test 3: Projection per zone (within zone sub-cube) ═══\n");

    /* Recount with collision info */
    fseek(gf->fp, (long)data_start, SEEK_SET);
    memset(cubes, 0, sizeof(cubes));

    /* Use collision-aware cell assignment: if cell collision, use next free cell */
    static int zone_used[16][N_CELLS];
    memset(zone_used, 0, sizeof(zone_used));

    cells_filled = 0;
    uint64_t collision_count = 0;

    for (uint64_t b = 0; b < n_blocks && cells_filled < 100*N_CELLS; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;

        for (int i = 0; i < 32; i++) {
            int code = (int)w[i] + 128;
            int zone = code >> 4;
            int cx, cy, cz;
            code_to_cell(code, &cx, &cy, &cz);
            uint32_t cell = cx*DIM*DIM + cy*DIM + cz;

            /* If collision, find next free cell in zone */
            if (zone_used[zone][cell % N_CELLS]) {
                collision_count++;
                for (uint32_t c = 0; c < N_CELLS; c++) {
                    if (!zone_used[zone][c]) {
                        cell = c;
                        break;
                    }
                }
            }

            zone_used[zone][cell % N_CELLS] = 1;
            cubes[zone][cell % N_CELLS] = 1;
            cells_filled++;
        }
    }

    grand_active = 0; grand_ghost = 0; zones_used = 0;
    for (int z = 0; z < N_ZONES; z++) {
        uint64_t active, ghost;
        if (project_zone(z, &active, &ghost)) {
            zones_used++;
            grand_active += active;
            grand_ghost += ghost;
        }
    }

    printf("Collisions: %d / %d (%.1f%%)\n",
        (int)collision_count, (int)cells_filled,
        (double)collision_count/cells_filled*100);
    printf("Active: %d  Ghost: %d (%.1f%%)\n",
        (int)grand_active, (int)grand_ghost,
        grand_active ? (double)grand_ghost/grand_active*100 : 0);

    fclose(gf->fp);
    return 0;
}
