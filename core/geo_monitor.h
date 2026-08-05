/* ═══════════════════════════════════════════════════════════════════════════
 * geo_monitor.h — Realtime Memory/Structure Monitor
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PURPOSE:
 *   Track memory usage, FrustumBlock access, and geometric structure
 *   expansion during inference — in realtime.
 *
 * WHAT IT TRACKS:
 *   1. RAM: total allocated, peak, current
 *   2. FrustumBlocks: loaded, accessed, cached
 *   3. Tensors: which ones accessed, how many times
 *   4. Geometric space: 20736 cells — which ones active
 *   5. Disk I/O: bytes read from GEO file
 *   6. Timeline: history of allocations (for visualization)
 *
 * USAGE:
 *   GeoMonitor mon;
 *   geo_monitor_init(&mon);
 *
 *   // Track allocation
 *   geo_monitor_alloc(&mon, size, "blk.0.attn_q");
 *
 *   // Track block access
 *   geo_monitor_block_access(&mon, block_idx, "attn_q");
 *
 *   // Track tensor access
 *   geo_monitor_tensor_access(&mon, "blk.0.attn_q.weight", n_blocks);
 *
 *   // Print stats
 *   geo_monitor_print(&mon);
 *
 *   // Export timeline for visualization
 *   geo_monitor_export_csv(&mon, "monitor.csv");
 *
 * DEPENDS: stdint.h, stdio.h, string.h, time.h
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_MONITOR_H
#define GEO_MONITOR_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define GEO_MON_MAX_TENSORS     512
#define GEO_MON_MAX_HISTORY     10000    /* allocation history entries */
#define GEO_MON_MAX_TIMELINE    1000     /* timeline snapshots */
#define GEO_MON_NAME_MAX        64

/* Geometry constants */
#define GEO_MON_GEO_FULL        20736    /* total address space */
#define GEO_MON_FBLOCK_SZ       4896     /* FrustumBlock size */

/* ═══════════════════════════════════════════════════════════════
   DATA STRUCTURES
   ═══════════════════════════════════════════════════════════════ */

/* Single allocation record */
typedef struct {
    void     *ptr;
    uint64_t  size;
    char      label[GEO_MON_NAME_MAX];
    uint64_t  timestamp_us;    /* microseconds since init */
} GeoMonAlloc;

/* Block access record */
typedef struct {
    uint32_t  block_idx;
    char      tensor[GEO_MON_NAME_MAX];
    uint64_t  timestamp_us;
} GeoMonBlockAccess;

/* Tensor access record */
typedef struct {
    char      name[GEO_MON_NAME_MAX];
    uint32_t  n_blocks;
    uint64_t  total_bytes;
    uint64_t  access_count;
    uint64_t  timestamp_us;
} GeoMonTensorAccess;

/* Timeline snapshot */
typedef struct {
    uint64_t  timestamp_us;
    uint64_t  ram_current;       /* current RAM allocated */
    uint64_t  ram_peak;          /* peak RAM */
    uint32_t  blocks_loaded;     /* FrustumBlocks in memory */
    uint32_t  tensors_active;    /* tensors accessed so far */
    uint32_t  geo_cells_active;  /* cells in 20736 space used */
    uint64_t  disk_bytes_read;   /* total bytes read from disk */
} GeoMonSnapshot;

/* ═══════════════════════════════════════════════════════════════
   MAIN MONITOR STRUCT
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    /* Timing */
    struct timespec start_time;

    /* RAM tracking */
    uint64_t ram_current;
    uint64_t ram_peak;
    uint64_t ram_total_alloc;      /* cumulative */
    uint64_t ram_total_free;       /* cumulative */
    uint32_t alloc_count;
    uint32_t free_count;

    /* Block tracking */
    uint32_t blocks_loaded;        /* currently in memory */
    uint32_t blocks_accessed;     /* total accessed */
    uint64_t block_access_count;  /* total access ops */

    /* Tensor tracking */
    uint32_t n_tensors;
    GeoMonTensorAccess tensors[GEO_MON_MAX_TENSORS];

    /* Geometric space */
    uint8_t  geo_cells[GEO_MON_GEO_FULL / 8];  /* bitmap: which cells active */
    uint32_t geo_cells_active;

    /* Disk I/O */
    uint64_t disk_bytes_read;
    uint64_t disk_bytes_written;

    /* History */
    GeoMonAlloc alloc_history[GEO_MON_MAX_HISTORY];
    uint32_t alloc_history_idx;

    GeoMonBlockAccess block_history[GEO_MON_MAX_HISTORY];
    uint32_t block_history_idx;

    /* Timeline */
    GeoMonSnapshot timeline[GEO_MON_MAX_TIMELINE];
    uint32_t timeline_idx;
    uint64_t timeline_interval_us;  /* snapshot interval */
} GeoMonitor;

/* ═══════════════════════════════════════════════════════════════
   TIME HELPERS
   ═══════════════════════════════════════════════════════════════ */

static inline uint64_t geo_mon_time_us(const GeoMonitor *mon) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t sec = (uint64_t)(now.tv_sec - mon->start_time.tv_sec);
    uint64_t nsec = (uint64_t)(now.tv_nsec - mon->start_time.tv_nsec);
    return sec * 1000000ULL + nsec / 1000ULL;
}

/* ═══════════════════════════════════════════════════════════════
   FORWARD DECLARATIONS
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_monitor_snapshot(GeoMonitor *mon);

/* ═══════════════════════════════════════════════════════════════
   INIT
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_monitor_init(GeoMonitor *mon) {
    memset(mon, 0, sizeof(*mon));
    clock_gettime(CLOCK_MONOTONIC, &mon->start_time);
    mon->timeline_interval_us = 100000;  /* snapshot every 100ms */
}

/* ═══════════════════════════════════════════════════════════════
   TIMELINE SNAPSHOT (must be before alloc/free)
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_monitor_snapshot(GeoMonitor *mon) {
    uint64_t now = geo_mon_time_us(mon);
    uint64_t last = (mon->timeline_idx > 0) ?
        mon->timeline[mon->timeline_idx - 1].timestamp_us : 0;

    /* Only snapshot at interval */
    if (now - last < mon->timeline_interval_us) return;
    if (mon->timeline_idx >= GEO_MON_MAX_TIMELINE) return;

    GeoMonSnapshot *s = &mon->timeline[mon->timeline_idx++];
    s->timestamp_us = now;
    s->ram_current = mon->ram_current;
    s->ram_peak = mon->ram_peak;
    s->blocks_loaded = mon->blocks_loaded;
    s->tensors_active = mon->n_tensors;
    s->geo_cells_active = mon->geo_cells_active;
    s->disk_bytes_read = mon->disk_bytes_read;
}

/* ═══════════════════════════════════════════════════════════════
   MEMORY TRACKING
   ═══════════════════════════════════════════════════════════════ */

/* Track an allocation */
static inline void geo_monitor_alloc(GeoMonitor *mon, size_t size, const char *label) {
    mon->ram_current += size;
    mon->ram_total_alloc += size;
    mon->alloc_count++;

    if (mon->ram_current > mon->ram_peak) {
        mon->ram_peak = mon->ram_current;
    }

    /* Record in history */
    if (mon->alloc_history_idx < GEO_MON_MAX_HISTORY) {
        GeoMonAlloc *a = &mon->alloc_history[mon->alloc_history_idx++];
        a->ptr = NULL;
        a->size = size;
        strncpy(a->label, label, GEO_MON_NAME_MAX - 1);
        a->timestamp_us = geo_mon_time_us(mon);
    }

    /* Update timeline */
    geo_monitor_snapshot(mon);
}

/* Track a deallocation */
static inline void geo_monitor_free(GeoMonitor *mon, size_t size) {
    mon->ram_current -= size;
    mon->ram_total_free += size;
    mon->free_count++;

    /* Update timeline */
    geo_monitor_snapshot(mon);
}

/* ═══════════════════════════════════════════════════════════════
   BLOCK TRACKING
   ═══════════════════════════════════════════════════════════════ */

/* Track a FrustumBlock access */
static inline void geo_monitor_block_access(GeoMonitor *mon, uint32_t block_idx,
                                            const char *tensor)
{
    mon->blocks_accessed++;
    mon->block_access_count++;

    /* Record in history */
    if (mon->block_history_idx < GEO_MON_MAX_HISTORY) {
        GeoMonBlockAccess *b = &mon->block_history[mon->block_history_idx++];
        b->block_idx = block_idx;
        strncpy(b->tensor, tensor, GEO_MON_NAME_MAX - 1);
        b->timestamp_us = geo_mon_time_us(mon);
    }
}

/* Track disk read */
static inline void geo_monitor_disk_read(GeoMonitor *mon, uint64_t bytes) {
    mon->disk_bytes_read += bytes;
}

/* ═══════════════════════════════════════════════════════════════
   TENSOR TRACKING
   ═══════════════════════════════════════════════════════════════ */

/* Track a tensor access */
static inline void geo_monitor_tensor_access(GeoMonitor *mon, const char *name,
                                             uint32_t n_blocks)
{
    /* Find or add tensor */
    int idx = -1;
    for (uint32_t i = 0; i < mon->n_tensors; i++) {
        if (strcmp(mon->tensors[i].name, name) == 0) {
            idx = (int)i;
            break;
        }
    }

    if (idx < 0 && mon->n_tensors < GEO_MON_MAX_TENSORS) {
        idx = (int)mon->n_tensors++;
        memset(&mon->tensors[idx], 0, sizeof(GeoMonTensorAccess));
        strncpy(mon->tensors[idx].name, name, GEO_MON_NAME_MAX - 1);
    }

    if (idx >= 0) {
        mon->tensors[idx].n_blocks = n_blocks;
        mon->tensors[idx].total_bytes += (uint64_t)n_blocks * GEO_MON_FBLOCK_SZ;
        mon->tensors[idx].access_count++;
        mon->tensors[idx].timestamp_us = geo_mon_time_us(mon);
    }
}

/* ═══════════════════════════════════════════════════════════════
   GEOMETRIC SPACE TRACKING
   ═══════════════════════════════════════════════════════════════ */

/* Mark a cell in the 20736 space as active */
static inline void geo_monitor_cell_activate(GeoMonitor *mon, uint32_t cell_id) {
    if (cell_id >= GEO_MON_GEO_FULL) return;

    uint32_t byte_idx = cell_id / 8;
    uint8_t  bit_idx  = cell_id % 8;

    if (!(mon->geo_cells[byte_idx] & (1 << bit_idx))) {
        mon->geo_cells[byte_idx] |= (1 << bit_idx);
        mon->geo_cells_active++;
    }
}

/* ═══════════════════════════════════════════════════════════════
   PRINT STATS
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_monitor_print(const GeoMonitor *mon) {
    printf("===============================================================\n");
    printf("  GEO Monitor — Realtime Stats\n");
    printf("===============================================================\n");

    /* RAM */
    printf("  RAM:\n");
    printf("    Current:      %8.1f MB\n", mon->ram_current / 1024.0 / 1024.0);
    printf("    Peak:         %8.1f MB\n", mon->ram_peak / 1024.0 / 1024.0);
    printf("    Total alloc:  %8.1f MB (%u ops)\n",
           mon->ram_total_alloc / 1024.0 / 1024.0, mon->alloc_count);
    printf("    Total free:   %8.1f MB (%u ops)\n",
           mon->ram_total_free / 1024.0 / 1024.0, mon->free_count);

    /* Blocks */
    printf("  FrustumBlocks:\n");
    printf("    Accessed:     %u\n", mon->blocks_accessed);
    printf("    Access ops:   %lu\n", (unsigned long)mon->block_access_count);

    /* Tensors */
    printf("  Tensors:\n");
    printf("    Active:       %u\n", mon->n_tensors);

    /* Top 5 by access count */
    printf("    Top accessed:\n");
    GeoMonTensorAccess sorted[GEO_MON_MAX_TENSORS];
    uint32_t n = mon->n_tensors;
    memcpy(sorted, mon->tensors, n * sizeof(GeoMonTensorAccess));
    for (uint32_t i = 0; i < n && i < 5; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            if (sorted[j].access_count > sorted[i].access_count) {
                GeoMonTensorAccess tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    for (uint32_t i = 0; i < n && i < 5; i++) {
        printf("      %-35s %lu accesses, %.1f KB\n",
               sorted[i].name,
               (unsigned long)sorted[i].access_count,
               sorted[i].total_bytes / 1024.0);
    }

    /* Geometric space */
    printf("  Geometric Space:\n");
    printf("    Active cells: %u / %u (%.1f%%)\n",
           mon->geo_cells_active, GEO_MON_GEO_FULL,
           mon->geo_cells_active * 100.0 / GEO_MON_GEO_FULL);

    /* Disk I/O */
    printf("  Disk I/O:\n");
    printf("    Read:         %8.1f MB\n", mon->disk_bytes_read / 1024.0 / 1024.0);
    printf("    Written:      %8.1f MB\n", mon->disk_bytes_written / 1024.0 / 1024.0);

    /* Timeline */
    printf("  Timeline:\n");
    printf("    Snapshots:    %u\n", mon->timeline_idx);
    if (mon->timeline_idx > 0) {
        uint64_t elapsed = mon->timeline[mon->timeline_idx - 1].timestamp_us;
        printf("    Duration:     %.3f sec\n", elapsed / 1000000.0);
    }

    printf("===============================================================\n");
}

/* ═══════════════════════════════════════════════════════════════
   EXPORT CSV
   ═══════════════════════════════════════════════════════════════ */

static inline int geo_monitor_export_csv(const GeoMonitor *mon, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "time_ms,ram_mb,ram_peak_mb,blocks,tensors,geo_cells,disk_read_mb\n");

    for (uint32_t i = 0; i < mon->timeline_idx; i++) {
        const GeoMonSnapshot *s = &mon->timeline[i];
        fprintf(f, "%.3f,%.2f,%.2f,%u,%u,%u,%.2f\n",
                s->timestamp_us / 1000.0,
                s->ram_current / 1024.0 / 1024.0,
                s->ram_peak / 1024.0 / 1024.0,
                s->blocks_loaded,
                s->tensors_active,
                s->geo_cells_active,
                s->disk_bytes_read / 1024.0 / 1024.0);
    }

    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   PRINT TIMELINE BAR CHART (ASCII)
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_monitor_print_timeline(const GeoMonitor *mon) {
    printf("\n  RAM Usage Timeline:\n");
    printf("  Time(s)  RAM(MB)  Block\n");
    printf("  -------- -------- -----\n");

    uint32_t n = mon->timeline_idx;
    if (n == 0) return;

    /* Find max values for scaling */
    uint64_t max_ram = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (mon->timeline[i].ram_current > max_ram)
            max_ram = mon->timeline[i].ram_current;
    }

    /* Print every Nth snapshot */
    uint32_t step = (n > 30) ? n / 30 : 1;
    for (uint32_t i = 0; i < n; i += step) {
        const GeoMonSnapshot *s = &mon->timeline[i];
        double time_s = s->timestamp_us / 1000000.0;
        double ram_mb = s->ram_current / 1024.0 / 1024.0;

        /* Bar chart */
        int bar_len = (max_ram > 0) ?
            (int)(ram_mb * 40.0 / (max_ram / 1024.0 / 1024.0)) : 0;

        printf("  %7.2f  %7.1f  ", time_s, ram_mb);
        for (int j = 0; j < bar_len; j++) printf("█");
        printf("\n");
    }
}

/* ═══════════════════════════════════════════════════════════════
   SUMMARY
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_monitor_summary(const GeoMonitor *mon) {
    printf("===============================================================\n");
    printf("  MONITOR SUMMARY\n");
    printf("===============================================================\n");

    printf("  Model:    %u tensors\n", mon->n_tensors);
    printf("  RAM:      %.1f MB current, %.1f MB peak\n",
           mon->ram_current / 1024.0 / 1024.0,
           mon->ram_peak / 1024.0 / 1024.0);
    printf("  Blocks:   %u accessed\n", mon->blocks_accessed);
    printf("  Space:    %u / %u cells (%.1f%%)\n",
           mon->geo_cells_active, GEO_MON_GEO_FULL,
           mon->geo_cells_active * 100.0 / GEO_MON_GEO_FULL);
    printf("  Disk:     %.1f MB read\n", mon->disk_bytes_read / 1024.0 / 1024.0);

    /* Compression ratio hint */
    if (mon->disk_bytes_read > 0 && mon->ram_current > 0) {
        double ratio = (double)mon->disk_bytes_read / mon->ram_current;
        printf("  Ratio:    %.2fx (disk/ram)\n", ratio);
    }

    printf("===============================================================\n");
}

#endif /* GEO_MONITOR_H */
