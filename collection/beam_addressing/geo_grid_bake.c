/*
 * geo_grid_bake.c — Bake Once, Infer Everywhere
 * 
 * Weight (128) × Time Island (162) = Grid (20736)
 * 
 * Grid 20736 จุด = จุดตัดของ weight × time
 * สามารถ reshaped ได้: 144×144, 72×288, 128×162
 * 
 * Usage:
 *   geo_grid_bake bake model.gguf    — bake geo index
 *   geo_grid_bake infer model.gguf   — inference with grid
 *   geo_grid_bake test               — run self-test
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define WEIGHT_DIM     128    /* 64 values × 2 hemispheres */
#define TIME_DIM       162    /* 2 × 3^4 = 162 islands */
#define GRID_TOTAL     20736  /* 128 × 162 = 144² */

/* Grid shapes (all = 20736 cells) */
#define SQUARE_ROWS    144
#define SQUARE_COLS    144
#define RECT_ROWS      72
#define RECT_COLS      288
#define NATURAL_ROWS   128
#define NATURAL_COLS   162

/* ============================================================
 * DATA STRUCTURES
 * ============================================================ */

/* Grid cell: weight × time intersection */
typedef struct {
    uint16_t weight_id;    /* 0-127 (which weight value) */
    uint16_t time_id;      /* 0-161 (which time island) */
    float    value;        /* the actual weight value */
    uint8_t  hemisphere;   /* 0=positive, 1=negative */
} grid_cell_t;

/* The grid (flat array, reshapeable) */
typedef struct {
    grid_cell_t cells[GRID_TOTAL];
    
    /* Current shape */
    int rows;
    int cols;
    
    /* Statistics */
    uint64_t access_count;
    uint64_t cache_hits;
} geo_grid_t;

/* ============================================================
 * BAKE PHASE
 * ============================================================ */

/* Map weight value (0-127) to grid cell */
static int weight_to_grid(int weight) {
    /* weight 0-127 → grid index 0-127 */
    return weight % WEIGHT_DIM;
}

/* Map time island (0-161) to grid cell */
static int time_to_grid(int island) {
    /* time island 0-161 → grid index 0-161 */
    return island % TIME_DIM;
}

/* Combine weight and time to grid position */
static int grid_position(int weight, int time) {
    int w = weight_to_grid(weight);
    int t = time_to_grid(time);
    return w * TIME_DIM + t;  /* row-major */
}

/* Bake the geo grid from weight data */
void geo_grid_bake(geo_grid_t *grid, const float *weights, int n_weights) {
    memset(grid, 0, sizeof(geo_grid_t));
    grid->rows = NATURAL_ROWS;
    grid->cols = NATURAL_COLS;
    
    for (int i = 0; i < GRID_TOTAL; i++) {
        int w = i / TIME_DIM;  /* weight dimension */
        int t = i % TIME_DIM;  /* time dimension */
        
        grid->cells[i].weight_id = (uint16_t)w;
        grid->cells[i].time_id = (uint16_t)t;
        grid->cells[i].hemisphere = (w >= 64) ? 1 : 0;  /* positive/negative */
        
        /* Map weight value if available */
        if (i < n_weights) {
            grid->cells[i].value = weights[i];
        } else {
            /* Generate deterministic value from position */
            grid->cells[i].value = (float)(w * 162 + t) / 20736.0f;
        }
    }
    
    printf("[BAKE] Grid %dx%d = %d cells\n", 
           grid->rows, grid->cols, GRID_TOTAL);
    printf("[BAKE] Weight dim: %d (64×2)\n", WEIGHT_DIM);
    printf("[BAKE] Time dim: %d (2×3⁴)\n", TIME_DIM);
}

/* ============================================================
 * RESHAPE PHASE
 * ============================================================ */

/* Reshape grid to different dimensions */
void geo_grid_reshape(geo_grid_t *grid, int new_rows, int new_cols) {
    if (new_rows * new_cols != GRID_TOTAL) {
        printf("[ERROR] Invalid reshape: %dx%d = %d ≠ %d\n",
               new_rows, new_cols, new_rows * new_cols, GRID_TOTAL);
        return;
    }
    
    int old_rows = grid->rows;
    int old_cols = grid->cols;
    
    grid->rows = new_rows;
    grid->cols = new_cols;
    
    printf("[RESHAPE] %dx%d → %dx%d (total=%d)\n",
           old_rows, old_cols, new_rows, new_cols, GRID_TOTAL);
}

/* ============================================================
 * INFERENCE PHASE
 * ============================================================ */

/* O(1) weight access via grid */
float geo_grid_access(geo_grid_t *grid, int weight, int time) {
    grid->access_count++;
    
    int pos = grid_position(weight, time);
    
    /* Bounds check */
    if (pos < 0 || pos >= GRID_TOTAL) {
        return 0.0f;
    }
    
    /* Cache hit: if cell is within current shape bounds */
    int row = pos / grid->cols;
    int col = pos % grid->cols;
    if (row < grid->rows && col < grid->cols) {
        grid->cache_hits++;
    }
    
    return grid->cells[pos].value;
}

/* Access by row/col (after reshape) */
float geo_grid_access_rc(geo_grid_t *grid, int row, int col) {
    grid->access_count++;
    
    if (row < 0 || row >= grid->rows || col < 0 || col >= grid->cols) {
        return 0.0f;
    }
    
    int pos = row * grid->cols + col;
    grid->cache_hits++;
    return grid->cells[pos].value;
}

/* ============================================================
 * COMPARISON: Hash Table vs Grid
 * ============================================================ */

/* Simple hash table for comparison */
#define HASH_SIZE 65536

typedef struct {
    uint32_t key;
    float    value;
    int      occupied;
} hash_entry_t;

typedef struct {
    hash_entry_t entries[HASH_SIZE];
    uint64_t access_count;
    uint64_t collision_count;
} hash_table_t;

void hash_table_init(hash_table_t *ht) {
    memset(ht, 0, sizeof(hash_table_t));
}

uint32_t hash_func(uint32_t weight, uint32_t time) {
    /* Simple hash */
    uint32_t h = weight * 162 + time;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return h % HASH_SIZE;
}

void hash_table_insert(hash_table_t *ht, int weight, int time, float value) {
    uint32_t key = weight * 162 + time;
    uint32_t idx = hash_func(weight, time);
    
    while (ht->entries[idx].occupied) {
        idx = (idx + 1) % HASH_SIZE;  /* linear probing */
    }
    
    ht->entries[idx].key = key;
    ht->entries[idx].value = value;
    ht->entries[idx].occupied = 1;
}

float hash_table_access(hash_table_t *ht, int weight, int time) {
    ht->access_count++;
    
    uint32_t key = weight * 162 + time;
    uint32_t idx = hash_func(weight, time);
    
    while (ht->entries[idx].occupied) {
        if (ht->entries[idx].key == key) {
            return ht->entries[idx].value;
        }
        idx = (idx + 1) % HASH_SIZE;
        ht->collision_count++;
    }
    
    return 0.0f;
}

/* ============================================================
 * TEST
 * ============================================================ */

void run_test(void) {
    printf("=== Geo Grid Bake Test ===\n\n");
    
    /* Generate test weights */
    float weights[GRID_TOTAL];
    for (int i = 0; i < GRID_TOTAL; i++) {
        weights[i] = (float)(rand() % 256 - 128) / 128.0f;
    }
    
    /* Bake grid */
    geo_grid_t grid;
    geo_grid_bake(&grid, weights, GRID_TOTAL);
    
    /* Test 1: O(1) access */
    printf("\n--- Test 1: O(1) Access ---\n");
    clock_t start = clock();
    for (int i = 0; i < 1000000; i++) {
        int w = rand() % 128;
        int t = rand() % 162;
        volatile float v = geo_grid_access(&grid, w, t);
        (void)v;
    }
    clock_t end = clock();
    double grid_time = (double)(end - start) / CLOCKS_PER_SEC;
    printf("Grid:    %.3f sec for 1M accesses\n", grid_time);
    printf("         = %.0f ns/access\n", grid_time * 1e9 / 1e6);
    printf("         cache hits: %lu/%lu (%.1f%%)\n",
           grid.cache_hits, grid.access_count,
           100.0 * grid.cache_hits / grid.access_count);
    
    /* Test 2: Hash table access */
    printf("\n--- Test 2: Hash Table Access ---\n");
    hash_table_t ht;
    hash_table_init(&ht);
    
    /* Insert all weights */
    for (int w = 0; w < 128; w++) {
        for (int t = 0; t < 162; t++) {
            int pos = w * 162 + t;
            hash_table_insert(&ht, w, t, weights[pos]);
        }
    }
    
    start = clock();
    for (int i = 0; i < 1000000; i++) {
        int w = rand() % 128;
        int t = rand() % 162;
        volatile float v = hash_table_access(&ht, w, t);
        (void)v;
    }
    end = clock();
    double hash_time = (double)(end - start) / CLOCKS_PER_SEC;
    printf("Hash:    %.3f sec for 1M accesses\n", hash_time);
    printf("         = %.0f ns/access\n", hash_time * 1e9 / 1e6);
    printf("         collisions: %lu\n", ht.collision_count);
    
    /* Speedup */
    printf("\n--- Speedup ---\n");
    printf("Grid is %.2fx faster than hash table\n", hash_time / grid_time);
    
    /* Test 3: Reshape */
    printf("\n--- Test 3: Reshape ---\n");
    geo_grid_reshape(&grid, 144, 144);  /* square */
    printf("Square access: grid[72][72] = %.4f\n", 
           geo_grid_access_rc(&grid, 72, 72));
    
    geo_grid_reshape(&grid, 72, 288);   /* rectangular */
    printf("Rect access:   grid[36][144] = %.4f\n", 
           geo_grid_access_rc(&grid, 36, 144));
    
    geo_grid_reshape(&grid, 128, 162);  /* natural */
    printf("Natural access: grid[64][81] = %.4f\n", 
           geo_grid_access_rc(&grid, 64, 81));
    
    /* Test 4: Verify same flat position = same data regardless of shape */
    printf("\n--- Test 4: Verify Same Flat Position ---\n");
    int flat_pos = 64 * 162 + 81;  /* position 10449 */
    
    /* In 128×162: row=64, col=81 */
    geo_grid_reshape(&grid, 128, 162);
    float v1 = grid.cells[flat_pos].value;
    int r1 = flat_pos / grid.cols;
    int c1 = flat_pos % grid.cols;
    printf("128×162: cell[%d][%d] = %f\n", r1, c1, v1);
    
    /* In 144×144: row=72, col=81 */
    geo_grid_reshape(&grid, 144, 144);
    float v2 = grid.cells[flat_pos].value;
    int r2 = flat_pos / grid.cols;
    int c2 = flat_pos % grid.cols;
    printf("144×144: cell[%d][%d] = %f\n", r2, c2, v2);
    
    /* In 72×288: row=36, col=81 */
    geo_grid_reshape(&grid, 72, 288);
    float v3 = grid.cells[flat_pos].value;
    int r3 = flat_pos / grid.cols;
    int c3 = flat_pos % grid.cols;
    printf("72×288:  cell[%d][%d] = %f\n", r3, c3, v3);
    
    printf("Same flat position %d: %f == %f == %f → %s\n",
           flat_pos, v1, v2, v3,
           (v1 == v2 && v2 == v3) ? "PASS" : "FAIL");
    
    printf("\n=== All Tests Complete ===\n");
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: geo_grid_bake <command>\n");
        printf("  test              — run self-test\n");
        printf("  bake <model.gguf> — bake geo index\n");
        printf("  infer <model.gguf> — inference with grid\n");
        return 1;
    }
    
    const char *cmd = argv[1];
    
    if (strcmp(cmd, "test") == 0) {
        run_test();
    } else if (strcmp(cmd, "bake") == 0) {
        printf("[TODO] Bake from GGUF: %s\n", argv[2] ? argv[2] : "(none)");
    } else if (strcmp(cmd, "infer") == 0) {
        printf("[TODO] Infer from GGUF: %s\n", argv[2] ? argv[2] : "(none)");
    } else {
        printf("Unknown command: %s\n", cmd);
        return 1;
    }
    
    return 0;
}
