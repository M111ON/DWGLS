/*
 * tools/scatter_load_sim.c — simulate sequential vs scatter loading patterns
 * ════════════════════════════════════════════════════════════════════════
 * Compares cache behavior of sequential tensor loading vs DWGLS
 * geometric stride (stride-37 on 144×144 grid).
 *
 * Also simulates multi-stream parallel loading.
 *
 * BUILD: gcc -O2 -std=c11 -o scatter_load_sim tools/scatter_load_sim.c -lm
 * USAGE: scatter_load_sim.exe <model.gguf>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_reader.h"

/* ── LRU Cache Simulation ─────────────────────────────────────── */
#define CACHE_LINE_SIZE 64  /* bytes per cache line */

typedef struct {
    uint64_t tag;
    int      valid;
    uint64_t lru_counter;
} CacheLine;

typedef struct {
    CacheLine *lines;
    int        n_lines;
    int        n_ways;       /* associativity */
    int        n_sets;
    uint64_t   hits;
    uint64_t   misses;
    uint64_t   accesses;
    uint64_t   lru_clock;
} SimCache;

static void cache_init(SimCache *c, int total_bytes, int n_ways) {
    c->n_lines = total_bytes / CACHE_LINE_SIZE;
    c->n_ways = n_ways;
    c->n_sets = c->n_lines / n_ways;
    c->lines = (CacheLine *)calloc(c->n_lines, sizeof(CacheLine));
    c->hits = c->misses = c->accesses = 0;
    c->lru_clock = 0;
}

static void cache_access(SimCache *c, uint64_t addr) {
    uint64_t line_addr = addr / CACHE_LINE_SIZE;
    uint64_t set_idx = line_addr % c->n_sets;
    int set_start = set_idx * c->n_ways;

    c->accesses++;
    c->lru_clock++;

    /* search for hit */
    for (int i = 0; i < c->n_ways; i++) {
        int idx = set_start + i;
        if (c->lines[idx].valid && c->lines[idx].tag == line_addr) {
            c->lines[idx].lru_counter = c->lru_clock;
            c->hits++;
            return;
        }
    }

    /* miss — find LRU way in set */
    int lru_way = 0;
    uint64_t min_lru = UINT64_MAX;
    for (int i = 0; i < c->n_ways; i++) {
        int idx = set_start + i;
        if (!c->lines[idx].valid) { lru_way = i; break; }
        if (c->lines[idx].lru_counter < min_lru) {
            min_lru = c->lines[idx].lru_counter;
            lru_way = i;
        }
    }
    int idx = set_start + lru_way;
    c->lines[idx].valid = 1;
    c->lines[idx].tag = line_addr;
    c->lines[idx].lru_counter = c->lru_clock;
    c->misses++;
}

static void cache_reset(SimCache *c) {
    for (int i = 0; i < c->n_lines; i++) c->lines[i].valid = 0;
    c->hits = c->misses = c->accesses = 0;
    c->lru_clock = 0;
}

static double cache_rate(SimCache *c) {
    return c->accesses > 0 ? 100.0 * c->hits / c->accesses : 0;
}

/* ── Scatter address: stride-37 on 144×144 grid ────────────────── */
#define GRID 144
#define STRIDE 37

static uint64_t scatter_offset(uint64_t tensor_offset, uint32_t tensor_size,
                                uint32_t slot_idx, uint32_t n_slots) {
    /* Map tensor data into grid slots */
    uint64_t slot_bytes = (uint64_t)tensor_size / n_slots;
    if (slot_bytes < CACHE_LINE_SIZE) slot_bytes = CACHE_LINE_SIZE;

    /* scatter: stride-37 walks through 144 positions */
    uint64_t grid_pos = (slot_idx * STRIDE) % GRID;
    return tensor_offset + grid_pos * slot_bytes;
}

/* ── Multi-stream simulation ───────────────────────────────────── */
#define MAX_STREAMS 4

typedef struct {
    uint64_t start;
    uint64_t end;
    int      active;
} StreamJob;

static int simulate_streams(uint64_t *offsets, uint32_t *sizes, uint32_t n_tensors,
                            int n_streams, uint64_t *bandwidth_save) {
    /* Simple simulation: assign tensors to streams round-robin */
    /* Measure gap between consecutive accesses to same stream */
    uint64_t total_gap = 0;
    uint64_t max_gap = 0;
    int n_gaps = 0;

    StreamJob streams[MAX_STREAMS];
    memset(streams, 0, sizeof(streams));

    for (uint32_t i = 0; i < n_tensors; i++) {
        int s = i % n_streams;
        if (streams[s].active) {
            uint64_t gap = offsets[i] > streams[s].end ?
                          offsets[i] - streams[s].end :
                          streams[s].start - (offsets[i] + sizes[i]);
            total_gap += gap;
            if (gap > max_gap) max_gap = gap;
            n_gaps++;
        }
        streams[s].start = offsets[i];
        streams[s].end = offsets[i] + sizes[i];
        streams[s].active = 1;
    }

    *bandwidth_save = n_gaps > 0 ? total_gap / n_gaps : 0;
    return n_gaps;
}

/* ── Main ──────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    GgufReader reader;
    memset(&reader, 0, sizeof(reader));
    if (gguf_open(path, &reader) != 0) {
        fprintf(stderr, "ERROR: cannot open %s\n", path);
        return 1;
    }

    uint32_t n = reader.n_tensors;
    printf("=== Scatter Loading Simulator ===\n");
    printf("file: %s\n", path);
    printf("tensors: %u\n\n", n);

    /* Collect tensor offsets */
    uint64_t *offsets = (uint64_t *)malloc(n * sizeof(uint64_t));
    uint32_t *sizes = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint64_t total_bytes = 0;
    uint64_t max_end = 0;

    for (uint32_t i = 0; i < n; i++) {
        offsets[i] = reader.offsets[i] + reader.data_offset;
        sizes[i] = reader.sizes[i];
        total_bytes += sizes[i];
        uint64_t end = offsets[i] + sizes[i];
        if (end > max_end) max_end = end;
    }

    printf("weight data: %.2f MB\n", (double)total_bytes / (1024.0 * 1024.0));
    printf("address range: 0 - %.2f MB\n\n", (double)max_end / (1024.0 * 1024.0));

    /* ── Cache sizes to test ────────────────────────────────────── */
    int cache_sizes_kb[] = {32, 64, 128, 256, 512};
    int n_cache = 5;

    printf("─── CACHE SIMULATION (4-way associative) ───\n");
    printf("%-10s %12s %12s %10s\n", "Cache KB", "Seq Hit%", "Scatter Hit%", "Δ (pp)");
    printf("%-10s %12s %12s %10s\n", "---------", "---------", "------------", "------");

    for (int ci = 0; ci < n_cache; ci++) {
        int cache_bytes = cache_sizes_kb[ci] * 1024;
        SimCache seq_cache, scatter_cache;
        cache_init(&seq_cache, cache_bytes, 4);
        cache_init(&scatter_cache, cache_bytes, 4);

        /* Sequential loading: load tensors in order */
        for (uint32_t i = 0; i < n; i++) {
            for (uint64_t b = offsets[i]; b < offsets[i] + sizes[i]; b += CACHE_LINE_SIZE) {
                cache_access(&seq_cache, b);
            }
        }

        /* Scatter loading: stride-37 pattern */
        for (uint32_t i = 0; i < n; i++) {
            /* Map tensor to grid positions */
            uint32_t n_slots = (sizes[i] + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
            if (n_slots > GRID) n_slots = GRID;
            if (n_slots == 0) n_slots = 1;

            for (uint32_t s = 0; s < n_slots; s++) {
                uint64_t scatter_off = scatter_offset(offsets[i], sizes[i], s, n_slots);
                /* Clamp to tensor bounds */
                if (scatter_off >= offsets[i] && scatter_off < offsets[i] + sizes[i]) {
                    cache_access(&scatter_cache, scatter_off);
                }
            }
        }

        double seq_hit = cache_rate(&seq_cache);
        double scat_hit = cache_rate(&scatter_cache);
        printf("%-10d %11.2f%% %11.2f%% %+9.2f\n",
               cache_sizes_kb[ci], seq_hit, scat_hit, scat_hit - seq_hit);

        free(seq_cache.lines);
        free(scatter_cache.lines);
    }
    printf("\n");

    /* ── Stream loading analysis ────────────────────────────────── */
    printf("─── MULTI-STREAM ANALYSIS ───\n");
    printf("measuring inter-tensor gaps for stream parallelism...\n\n");

    /* Compute gap statistics for sequential loading */
    uint64_t total_sequential_gap = 0;
    uint64_t max_sequential_gap = 0;
    int n_sequential_gaps = 0;
    for (uint32_t i = 1; i < n; i++) {
        if (offsets[i] > offsets[i-1] + sizes[i-1]) {
            uint64_t gap = offsets[i] - (offsets[i-1] + sizes[i-1]);
            total_sequential_gap += gap;
            if (gap > max_sequential_gap) max_sequential_gap = gap;
            n_sequential_gaps++;
        }
    }

    printf("sequential loading gaps:\n");
    printf("  total gap: %.2f MB\n", (double)total_sequential_gap / (1024.0 * 1024.0));
    printf("  avg gap: %.2f KB\n", n_sequential_gaps > 0 ?
           (double)total_sequential_gap / n_sequential_gaps / 1024.0 : 0);
    printf("  max gap: %.2f MB\n", (double)max_sequential_gap / (1024.0 * 1024.0));
    printf("  gap count: %d\n\n", n_sequential_gaps);

    /* Scatter loading gaps */
    uint64_t total_scatter_gap = 0;
    uint64_t max_scatter_gap = 0;
    int n_scatter_gaps = 0;

    /* Build scatter order: stride-37 across all tensors */
    typedef struct { uint32_t idx; uint64_t offset; } ScatterEntry;
    ScatterEntry *entries = (ScatterEntry *)malloc(n * sizeof(ScatterEntry));
    for (uint32_t i = 0; i < n; i++) {
        entries[i].idx = i;
        entries[i].offset = offsets[i];
    }
    /* Sort by scatter position */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            uint64_t pos_i = (entries[i].offset * STRIDE) % max_end;
            uint64_t pos_j = (entries[j].offset * STRIDE) % max_end;
            if (pos_i > pos_j) {
                ScatterEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    for (uint32_t i = 1; i < n; i++) {
        uint64_t prev_end = entries[i-1].offset + sizes[entries[i-1].idx];
        if (entries[i].offset > prev_end) {
            uint64_t gap = entries[i].offset - prev_end;
            total_scatter_gap += gap;
            if (gap > max_scatter_gap) max_scatter_gap = gap;
            n_scatter_gaps++;
        }
    }

    printf("scatter loading gaps (stride-37):\n");
    printf("  total gap: %.2f MB\n", (double)total_scatter_gap / (1024.0 * 1024.0));
    printf("  avg gap: %.2f KB\n", n_scatter_gaps > 0 ?
           (double)total_scatter_gap / n_scatter_gaps / 1024.0 : 0);
    printf("  max gap: %.2f MB\n", (double)max_scatter_gap / (1024.0 * 1024.0));
    printf("  gap count: %d\n\n", n_scatter_gaps);

    /* ── Tensor type grouping analysis ──────────────────────────── */
    printf("─── TENSOR TYPE GROUPING ───\n");
    printf("can we group by type for cache efficiency?\n\n");

    /* Group tensors by GGML type */
    uint8_t type_seen[256] = {0};
    for (uint32_t i = 0; i < n; i++) type_seen[reader.dtypes[i]] = 1;

    for (int t = 0; t < 256; t++) {
        if (!type_seen[t]) continue;
        int count = 0;
        uint64_t type_total = 0;
        uint64_t type_min_off = UINT64_MAX;
        uint64_t type_max_off = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (reader.dtypes[i] == t) {
                count++;
                type_total += sizes[i];
                if (offsets[i] < type_min_off) type_min_off = offsets[i];
                if (offsets[i] + sizes[i] > type_max_off) type_max_off = offsets[i] + sizes[i];
            }
        }
        if (count > 0) {
            printf("  type %2d: %4d tensors, %8.2f MB, range %.2f-%.2f MB\n",
                   t, count, (double)type_total / (1024.0 * 1024.0),
                   (double)type_min_off / (1024.0 * 1024.0),
                   (double)type_max_off / (1024.0 * 1024.0));
        }
    }
    printf("\n");

    /* ── Summary ─────────────────────────────────────────────────── */
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("SCATTER LOADING ANALYSIS SUMMARY\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("Model:           %s\n", path);
    printf("Tensors:         %u\n", n);
    printf("Weight data:     %.2f MB\n", (double)total_bytes / (1024.0 * 1024.0));
    printf("\n");
    printf("Cache hit rate:  sequential ≈ scatter (quantized weights are unique)\n");
    printf("Stream parallel: gap analysis above — smaller gaps = harder to overlap\n");
    printf("\n");
    printf("KEY INSIGHT: scatter loading helps when:\n");
    printf("  1. Tensors fit in cache (small models)\n");
    printf("  2. Large gaps between tensor groups (enables stream overlap)\n");
    printf("  3. Multiple GPU streams available\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    free(offsets);
    free(sizes);
    free(entries);
    gguf_close(&reader);
    return 0;
}
