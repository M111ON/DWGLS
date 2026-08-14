/* ═══════════════════════════════════════════════════════════════════════════
 * bench_mdim_storage.c — GeoFS MDIM storage efficiency analysis
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geofs_mdim.h"

int main(void) {
    printf("GeoFS MDIM — Storage Efficiency Analysis\n");
    printf("Volume: %.1f MB (%u slots × %u B = %u bytes total)\n\n",
           MDIM_VOL_BYTES / 1048576.0, MDIM_SLOTS, MDIM_SLOT_SZ, MDIM_VOL_BYTES);

    printf("┌────────────────┬──────────┬──────────┬──────────┬──────────┬──────────┐\n");
    printf("│ File Size      │ Slots    │ Overhead │ Data     │ Ratio    │ Files/MB │\n");
    printf("├────────────────┼──────────┼──────────┼──────────┼──────────┼──────────┤\n");

    uint32_t sizes[] = {1, 10, 63, 100, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    int n_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < n_sizes; i++) {
        uint32_t sz = sizes[i];
        
        /* Calculate slots needed */
        uint32_t data_slots = (sz + MDIM_DATA_SLOT_BYTES - 1) / MDIM_DATA_SLOT_BYTES;
        uint32_t total_slots = 1 + data_slots;  /* FILE entry + DATA slots */
        if (sz > MDIM_RUN_BYTES) {
            /* Multi-run file */
            uint32_t n_runs = (sz + MDIM_RUN_BYTES - 1) / MDIM_RUN_BYTES;
            total_slots = 1 + n_runs;  /* FILE entry + LINK slots */
            for (uint32_t r = 0; r < n_runs; r++) {
                uint32_t run_sz = (r < n_runs - 1) ? MDIM_RUN_BYTES : (sz - r * MDIM_RUN_BYTES);
                total_slots += (run_sz + MDIM_DATA_SLOT_BYTES - 1) / MDIM_DATA_SLOT_BYTES + 1;
            }
        }
        
        uint32_t overhead_bytes = total_slots * MDIM_SLOT_SZ;
        double ratio = (double)sz / overhead_bytes * 100.0;
        double files_per_mb = (1048576.0 / overhead_bytes);
        
        printf("│ %12u B │ %8u │ %8u │ %8u │ %6.1f%%  │ %8.0f │\n",
               sz, total_slots, overhead_bytes, sz, ratio, files_per_mb);
    }

    printf("└────────────────┴──────────┴──────────┴──────────┴──────────┴──────────┘\n");

    /* Capacity analysis */
    printf("\nCapacity Analysis (1.3 MB volume):\n");
    printf("  Max files (1 B each): %u files\n", MDIM_SLOTS / 2);  /* FILE + DATA per file */
    printf("  Max files (63 B each): %u files\n", MDIM_SLOTS / 3);  /* FILE + 1 DATA per file */
    printf("  Max files (1 KB each): %u files\n", MDIM_SLOTS / 17); /* FILE + 16 DATA per file */
    printf("  Max single file: %u bytes (%.1f KB)\n", MDIM_MAX_FILE_BYTES, MDIM_MAX_FILE_BYTES / 1024.0);
    printf("  Journal capacity: %u changes per frame\n", MDIM_MAX_CHANGES);
    printf("  Journal ring: %u slots = %.1f KB\n", MDIM_JRNL_SLOTS, MDIM_JRNL_SLOTS * MDIM_SLOT_SZ / 1024.0);

    return 0;
}
