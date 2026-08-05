/* ═══════════════════════════════════════════════════════════════════════════
 * test_monitor.c — Simulate Inference + Show Realtime Monitor
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   1. Simulate loading GGUF → GEO tensors
 *   2. Track RAM usage during load
 *   3. Track FrustumBlock access
 *   4. Track geometric space expansion
 *   5. Print realtime stats + timeline
 *   6. Export CSV for visualization
 *
 * Compile:
 *   gcc -std=c11 -Wall -O2 -I../core test_monitor.c -o test_monitor.exe
 *
 * Run:
 *   ./test_monitor.exe
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "geo_monitor.h"
#include "geo_tensor_map.h"

/* ═══════════════════════════════════════════════════════════════
   SIMULATION: Load tensors one by one
   ═══════════════════════════════════════════════════════════════ */

static void simulate_tensor_load(GeoMonitor *mon, const char *name,
                                 uint32_t n_blocks, uint32_t n_cells)
{
    /* Simulate reading blocks from disk */
    uint64_t disk_bytes = (uint64_t)n_blocks * GEO_MON_FBLOCK_SZ;
    geo_monitor_disk_read(mon, disk_bytes);

    /* Simulate RAM allocation */
    geo_monitor_alloc(mon, disk_bytes, name);

    /* Track tensor access */
    geo_monitor_tensor_access(mon, name, n_blocks);

    /* Track block access */
    for (uint32_t i = 0; i < n_blocks && i < 100; i++) {
        geo_monitor_block_access(mon, i, name);
    }

    /* Activate geometric cells */
    for (uint32_t i = 0; i < n_cells; i++) {
        geo_monitor_cell_activate(mon, i);
    }

    /* Small delay to simulate work */
    struct timespec ts = {0, 1000000}; /* 1ms */
    nanosleep(&ts, NULL);
}

/* ═══════════════════════════════════════════════════════════════
   SIMULATION: Free tensors (eviction)
   ═══════════════════════════════════════════════════════════════ */

static void simulate_tensor_free(GeoMonitor *mon, const char *name,
                                 uint32_t n_blocks)
{
    uint64_t disk_bytes = (uint64_t)n_blocks * GEO_MON_FBLOCK_SZ;
    geo_monitor_free(mon, disk_bytes);
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("===============================================================\n");
    printf("  GEO Monitor — Inference Simulation\n");
    printf("===============================================================\n\n");

    GeoMonitor mon;
    geo_monitor_init(&mon);

    /* Phase 1: Load embedding layers */
    printf(">>> Phase 1: Loading embeddings...\n");
    simulate_tensor_load(&mon, "token_embd.weight", 29544, 20736);
    geo_monitor_print(&mon);
    printf("\n");

    /* Phase 2: Load layer 0 */
    printf(">>> Phase 2: Loading layer 0...\n");
    simulate_tensor_load(&mon, "blk.0.attn_q.weight", 175, 288);
    simulate_tensor_load(&mon, "blk.0.attn_k.weight", 25, 288);
    simulate_tensor_load(&mon, "blk.0.attn_v.weight", 25, 288);
    simulate_tensor_load(&mon, "blk.0.ffn_up.weight", 946, 576);
    simulate_tensor_load(&mon, "blk.0.ffn_down.weight", 946, 576);
    geo_monitor_print(&mon);
    printf("\n");

    /* Phase 3: Load layers 1-5 */
    printf(">>> Phase 3: Loading layers 1-5...\n");
    for (int layer = 1; layer <= 5; layer++) {
        char name[64];
        snprintf(name, sizeof(name), "blk.%d.attn_q.weight", layer);
        simulate_tensor_load(&mon, name, 175, 288);
        snprintf(name, sizeof(name), "blk.%d.ffn_up.weight", layer);
        simulate_tensor_load(&mon, name, 946, 576);
    }
    geo_monitor_print(&mon);
    printf("\n");

    /* Phase 4: Evict some layers (simulate KV cache eviction) */
    printf(">>> Phase 4: Evicting layers 1-3...\n");
    for (int layer = 1; layer <= 3; layer++) {
        char name[64];
        snprintf(name, sizeof(name), "blk.%d.attn_q.weight", layer);
        simulate_tensor_free(&mon, name, 175);
    }
    geo_monitor_print(&mon);
    printf("\n");

    /* Phase 5: Load remaining layers */
    printf(">>> Phase 5: Loading layers 6-23...\n");
    for (int layer = 6; layer <= 23; layer++) {
        char name[64];
        snprintf(name, sizeof(name), "blk.%d.attn_q.weight", layer);
        simulate_tensor_load(&mon, name, 175, 288);
        snprintf(name, sizeof(name), "blk.%d.ffn_up.weight", layer);
        simulate_tensor_free(&mon, name, 946);  /* free after use */
    }
    geo_monitor_print(&mon);
    printf("\n");

    /* Phase 6: Load output */
    printf(">>> Phase 6: Loading output...\n");
    simulate_tensor_load(&mon, "output.weight", 29544, 20736);
    simulate_tensor_load(&mon, "output_norm.weight", 1, 896);
    geo_monitor_print(&mon);
    printf("\n");

    /* Timeline */
    geo_monitor_print_timeline(&mon);

    /* Summary */
    geo_monitor_summary(&mon);

    /* Export CSV */
    const char *csv_path = "I:/tmp_test/monitor_timeline.csv";
    if (geo_monitor_export_csv(&mon, csv_path) == 0) {
        printf("\n  CSV exported: %s\n", csv_path);
    }

    return 0;
}
