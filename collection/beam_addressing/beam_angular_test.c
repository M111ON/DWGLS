/* beam_angular_test.c — Angular positioning test
 *
 * User concept:
 *   "สนามที่มีเพดาน positive พื้น negative
 *    tensor เดินหา location ของตัวเอง
 *    เอาเส้นกางออกเป็น angular
 *    ระยะข้อมูล = beam = radius ของ center-surface"
 *
 *   weight → beam length (|value|) → walks to ceiling/floor
 *          → angular position (θ, φ) on surface
 *          → weights ค่าเท่ากัน = radius เท่ากัน = ring เดียวกัน
 *          → cluster → projection lossless
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "gguf_reader.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DIM     10
#define N_CELLS (DIM*DIM*DIM)  /* 1000 */
#define N_ZONES 16

/* ── Beam angular mapping ──────────────────────────────────────
 *   Weight → beam length = |weight| → radius from center
 *   Beam direction = hash(param_index) → angle (θ, φ) on surface
 *   Surface position = (θ, φ) × radius → 2D projection coordinate
 *
 *   Key: weights with same value → same radius → same "ring"
 *   Same ring → cluster → projection lossless
 * ─────────────────────────────────────────────────────────────── */

/* Simple hash for beam direction (deterministic from index) */
static inline uint32_t beam_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x45d9f3b;
    x ^= x >> 16; x *= 0x45d9f3b;
    x ^= x >> 16;
    return x;
}

/* Angular position from weight value + index */
typedef struct {
    uint16_t theta;  /* 0..359 azimuth */
    uint16_t phi;    /* 0..359 elevation */
    uint8_t  zone;   /* 0..15 = upper nibble of beam code */
    uint8_t  ring;   /* 0..15 = lower nibble (position) */
} BeamAngular;

static BeamAngular weight_to_angular(int8_t weight, uint32_t index) {
    BeamAngular a;
    uint8_t code = (uint8_t)((int)weight + 128);  /* 0..255 */
    a.zone = code >> 4;
    a.ring = code & 0x0F;

    /* Angular position: direction from hash, radius from ring */
    uint32_t h = beam_hash(index);
    a.theta = (uint16_t)(h % 360);
    a.phi   = (uint16_t)((h / 360) % 360);
    return a;
}

/* ── Cube from angular positions ────────────────────────────── */
static uint8_t cubes[N_ZONES][N_CELLS];

/* Project one zone cube and count ghost */
static int project_zone(int zone, uint64_t *active, uint64_t *ghost) {
    uint8_t *cube = cubes[zone];
    uint64_t n_active = 0;
    for (int i = 0; i < N_CELLS; i++) n_active += cube[i];
    if (n_active == 0) { *active = 0; *ghost = 0; return 0; }

    /* 3-axis projection */
    uint8_t map_x[DIM][DIM], map_y[DIM][DIM], map_z[DIM][DIM];
    memset(map_x, 0, sizeof(map_x));
    memset(map_y, 0, sizeof(map_y));
    memset(map_z, 0, sizeof(map_z));

    for (int x = 0; x < DIM; x++)
        for (int y = 0; y < DIM; y++)
            for (int z = 0; z < DIM; z++) {
                if (cube[x*DIM*DIM + y*DIM + z]) {
                    map_x[y][z] = 1;
                    map_y[x][z] = 1;
                    map_z[x][y] = 1;
                }
            }

    uint64_t n_ghost = 0;
    for (int x = 0; x < DIM; x++)
        for (int y = 0; y < DIM; y++)
            for (int z = 0; z < DIM; z++) {
                uint8_t r = map_x[y][z] & map_y[x][z] & map_z[x][y];
                if (r && !cube[x*DIM*DIM + y*DIM + z]) n_ghost++;
            }

    *active = n_active;
    *ghost = n_ghost;
    return 1;
}

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

    /* ── PASS 1: fill cubes using beam angular mapping ──── */
    memset(cubes, 0, sizeof(cubes));
    uint64_t global_idx = 0;
    int n_cubes = 0;
    uint64_t cells_filled = 0;

    for (uint64_t b = 0; b < n_blocks; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;

        for (int i = 0; i < 32; i++) {
            BeamAngular a = weight_to_angular(w[i], (uint32_t)global_idx);
            uint32_t cell = cells_filled % N_CELLS;
            cubes[a.zone][cell] = 1;
            cells_filled++;
            global_idx++;

            if (cells_filled % N_CELLS == 0) {
                n_cubes++;
                if (n_cubes >= 100) goto done;
            }
        }
    }
    /* last partial cube */
    if (cells_filled % N_CELLS != 0) n_cubes++;

done:
    fclose(gf->fp);

    /* ── PASS 2: measure projection per zone per cube ──── */
    printf("\n=== Angular Beam Projection on Real GGUF ===\n");
    printf("Cubes: %d  Cells/cube: %d\n\n", n_cubes, N_CELLS);

    /* Re-read and process cube-by-cube for per-cube stats */
    gf = gguf_open(path);
    fseek(gf->fp, (long)((gf->tensor_data_start + gf->tensors[idx].offset + 31) & ~(uint64_t)31), SEEK_SET);

    uint64_t grand_active = 0, grand_ghost = 0;
    int zones_used = 0;

    memset(cubes, 0, sizeof(cubes));
    global_idx = 0;
    cells_filled = 0;
    n_cubes = 0;

    for (uint64_t b = 0; b < n_blocks; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;

        for (int i = 0; i < 32; i++) {
            BeamAngular a = weight_to_angular(w[i], (uint32_t)global_idx);
            uint32_t cell = cells_filled % N_CELLS;
            cubes[a.zone][cell] = 1;
            cells_filled++;
            global_idx++;

            if (cells_filled % N_CELLS == 0) {
                /* Measure this cube */
                for (int z = 0; z < N_ZONES; z++) {
                    uint64_t active, ghost;
                    if (project_zone(z, &active, &ghost)) {
                        zones_used++;
                        grand_active += active;
                        grand_ghost += ghost;
                    }
                }
                n_cubes++;
                memset(cubes, 0, sizeof(cubes));
                if (n_cubes >= 100) goto done2;
            }
        }
    }
    if (cells_filled % N_CELLS != 0) {
        for (int z = 0; z < N_ZONES; z++) {
            uint64_t active, ghost;
            if (project_zone(z, &active, &ghost)) {
                zones_used++;
                grand_active += active;
                grand_ghost += ghost;
            }
        }
        n_cubes++;
    }

done2:
    fclose(gf->fp);

    printf("Zones with data: %d\n", zones_used);
    printf("Total active cells: %d\n", (int)grand_active);
    printf("Total ghost cells: %d (%.1f%% ghost rate)\n",
        (int)grand_ghost, grand_active ? (double)grand_ghost/grand_active*100 : 0);

    /* Cost */
    uint64_t map_bits = (uint64_t)n_cubes * N_ZONES * 3 * DIM * DIM;
    uint64_t ghost_bits = grand_ghost * 10;
    uint64_t raw_bits = (uint64_t)cells_filled * 10;

    printf("\n=== Cost ===\n");
    printf("  Map:    %d bits\n", (int)map_bits);
    printf("  Ghost:  %d bits\n", (int)ghost_bits);
    printf("  Total:  %d bits\n", (int)(map_bits + ghost_bits));
    printf("  Raw:    %d bits (10b/cell)\n", (int)raw_bits);
    printf("  Ratio:  %.2fx\n", (double)(map_bits + ghost_bits) / (double)raw_bits);
    printf("  ⚡ Sort cost: ZERO (zone = natural beam class)\n");
    printf("  ⚡ Position cost: ZERO (runtime compute)\n");

    return 0;
}
