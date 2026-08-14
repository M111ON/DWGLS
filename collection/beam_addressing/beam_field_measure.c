/* beam_field_measure.c — Measure beam field projection on real GGUF */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "gguf_reader.h"

#define DIM 10
#define N_CELLS (DIM*DIM*DIM)  /* 1000 */

/* Binary cube per zone (16 zones × 1000 cells) */
static uint8_t zone_cubes[16][N_CELLS];

/* Projection maps: 3 axes × 100 cells each */
static uint8_t map_x[DIM][DIM];  /* plane y-z */
static uint8_t map_y[DIM][DIM];  /* plane x-z */
static uint8_t map_z[DIM][DIM];  /* plane x-y */

static void clear_maps(void) {
    memset(map_x, 0, sizeof(map_x));
    memset(map_y, 0, sizeof(map_y));
    memset(map_z, 0, sizeof(map_z));
}

static int project_and_measure(int zone, uint64_t *out_active, uint64_t *out_ghost) {
    uint8_t *cube = zone_cubes[zone];
    uint64_t active = 0;
    for (int i = 0; i < N_CELLS; i++) active += cube[i];
    if (active == 0) { *out_active = 0; *out_ghost = 0; return 0; }

    clear_maps();
    /* project */
    for (int x = 0; x < DIM; x++)
        for (int y = 0; y < DIM; y++)
            for (int z = 0; z < DIM; z++) {
                if (cube[x*DIM*DIM + y*DIM + z]) {
                    map_y[x][z] = 1;
                    map_z[x][y] = 1;
                    map_x[y][z] = 1;
                }
            }

    /* reconstruct + count ghost */
    uint64_t ghost = 0;
    for (int x = 0; x < DIM; x++)
        for (int y = 0; y < DIM; y++)
            for (int z = 0; z < DIM; z++) {
                uint8_t r = map_x[y][z] & map_y[x][z] & map_z[x][y];
                if (r && !cube[x*DIM*DIM + y*DIM + z]) ghost++;
            }

    *out_active = active;
    *out_ghost = ghost;
    return 1;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] :
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf";

    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("Cannot open %s\n", path); return 1; }

    /* Find first Q8_0 tensor */
    int idx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { idx = (int)i; break; }
    }
    if (idx < 0) { printf("No Q8_0 tensor\n"); gguf_close(gf); return 1; }

    GGUF_Tensor *t = &gf->tensors[idx];
    uint64_t n_blocks = t->n_weights / 32;
    printf("Tensor: %s\n", t->name);
    printf("Weights: %I64d  Blocks: %I64d\n",
        (unsigned long long)t->n_weights, (unsigned long long)n_blocks);

    uint64_t data_start = gf->tensor_data_start + t->offset;
    data_start = (data_start + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)data_start, SEEK_SET);

    /* Read Q8_0 blocks, fill cubes 10×10×10 = 1000 cells per cube */
    uint64_t total_zones = 0, total_ghost = 0;
    int n_cubes = 0;

    /* Process cubes until end of tensor or 100 cubes */
    uint64_t cells_filled = 0;
    memset(zone_cubes, 0, sizeof(zone_cubes));

    for (uint64_t b = 0; b < n_blocks && b < 100 * 32; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;

        /* Each weight → beam zone (upper nibble) */
        for (int i = 0; i < 32; i++) {
            uint8_t code = (uint8_t)((int)w[i] + 128);  /* 0..255 */
            uint8_t zone = code >> 4;                     /* 0..15 */
            uint32_t cell = cells_filled % N_CELLS;
            zone_cubes[zone][cell] = 1;
            cells_filled++;

            /* When cube full, measure & reset */
            if (cells_filled % N_CELLS == 0) {
                for (int z = 0; z < 16; z++) {
                    uint64_t active, ghost;
                    if (project_and_measure(z, &active, &ghost)) {
                        total_zones++;
                        total_ghost += ghost;
                    }
                }
                n_cubes++;
                memset(zone_cubes, 0, sizeof(zone_cubes));
                if (n_cubes >= 100) goto done;
            }
        }
    }
    /* Measure last partial cube */
    if (cells_filled % N_CELLS != 0) {
        for (int z = 0; z < 16; z++) {
            uint64_t active, ghost;
            if (project_and_measure(z, &active, &ghost)) {
                total_zones++;
                total_ghost += ghost;
            }
        }
        n_cubes++;
    }

done:
    fclose(gf->fp);

    printf("\n=== Beam Field Projection Results ===\n");
    printf("Cubes processed: %d\n", n_cubes);
    printf("Zones with data: %I64d\n", (unsigned long long)total_zones);
    printf("Total ghost cells: %I64d\n", (unsigned long long)total_ghost);

    /* Cost estimate */
    uint64_t map_bits = (uint64_t)n_cubes * 16 * 3 * DIM * DIM;
    uint64_t ghost_bits = total_ghost * 10;
    uint64_t raw_bits = (uint64_t)cells_filled * 10;

    printf("\n=== Cost Estimate ===\n");
    printf("  Map cost:    %I64d bits (16 zones × 3 maps × 100b × %d cubes)\n",
        (unsigned long long)map_bits, n_cubes);
    printf("  Ghost cost:  %I64d bits (%I64d × 10b)\n",
        (unsigned long long)ghost_bits, (unsigned long long)total_ghost);
    printf("  Total:       %I64d bits\n", (unsigned long long)(map_bits + ghost_bits));
    printf("  Raw (10b/c): %I64d bits\n", (unsigned long long)raw_bits);
    printf("  Ratio:       %.2fx\n", (double)(map_bits + ghost_bits) / (double)raw_bits);
    printf("  ⚡ NO permutation cost (zone = natural class)\n");
    printf("  ⚡ NO sort cost (zone = beam field distribution)\n");

    return 0;
}
