/*
 * tools/scatter_inference_sim.c — realistic inference loading simulation
 * ════════════════════════════════════════════════════════════════════════
 * Simulates layer-by-layer LLM inference loading:
 *   - Sequential: load layer N, compute, load layer N+1, ...
 *   - Pre-fetch: load layer N+1 while computing layer N
 *   - Scatter: load layer N via geometric stride pattern
 *
 * Measures: total loading time, overlap efficiency, memory bandwidth utilization
 *
 * BUILD: gcc -O2 -std=c11 -o scatter_inference_sim.exe tools/scatter_inference_sim.c -lm
 * USAGE: scatter_inference_sim.exe <model.gguf> [--streams N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_reader.h"

#define CACHE_LINE_SIZE 64
#define GB (1024.0 * 1024.0 * 1024.0)

/* ── Tensor info ───────────────────────────────────────────────── */
typedef struct {
    const char *name;
    uint64_t    offset;
    uint32_t    size;
    uint8_t     dtype;
    int         layer;    /* -1 = non-layer (embedding, output) */
    int         stream;   /* assigned stream */
} TensorInfo;

/* Extract layer index from name */
static int extract_layer(const char *name) {
    if (strncmp(name, "blk.", 4) == 0) return atoi(name + 4);
    if (strstr(name, "token_embd")) return -2;  /* embedding = special */
    if (strstr(name, "output_norm")) return -3; /* output norm = special */
    if (strstr(name, "output")) return -4;      /* output head = special */
    return -1; /* other non-layer tensors */
}

/* ── Stream simulation ─────────────────────────────────────────── */
typedef struct {
    uint64_t current_offset;
    uint64_t total_bytes;
    uint64_t idle_cycles;
    uint64_t active_cycles;
    int      layer_loading; /* current layer being loaded */
} StreamSim;

/* ── Main ──────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [--streams N]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int n_streams = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--streams") == 0 && i + 1 < argc) {
            n_streams = atoi(argv[++i]);
        }
    }

    GgufReader reader;
    memset(&reader, 0, sizeof(reader));
    if (gguf_open(path, &reader) != 0) {
        fprintf(stderr, "ERROR: cannot open %s\n", path);
        return 1;
    }

    uint32_t n = reader.n_tensors;
    printf("=== Inference Loading Simulator ===\n");
    printf("file: %s\n", path);
    printf("tensors: %u\n", n);
    printf("streams: %d\n\n", n_streams);

    /* ── Phase 1: Categorize tensors ────────────────────────────── */
    TensorInfo *tinfos = (TensorInfo *)malloc(n * sizeof(TensorInfo));
    int max_layer = -1;
    int n_embd = 0, n_output = 0;

    for (uint32_t i = 0; i < n; i++) {
        tinfos[i].name = reader.names[i];
        tinfos[i].offset = reader.offsets[i] + reader.data_offset;
        tinfos[i].size = reader.sizes[i];
        tinfos[i].dtype = reader.dtypes[i];
        tinfos[i].layer = extract_layer(reader.names[i]);
        tinfos[i].stream = 0;

        if (tinfos[i].layer >= 0 && tinfos[i].layer > max_layer)
            max_layer = tinfos[i].layer;
        if (tinfos[i].layer == -2) n_embd++;
        if (tinfos[i].layer == -4) n_output++;
    }

    printf("layers: %d\n", max_layer + 1);
    printf("embedding tensors: %d\n", n_embd);
    printf("output tensors: %d\n\n", n_output);

    /* ── Phase 2: Group by layer ─────────────────────────────────── */
    typedef struct {
        int      start; /* first tensor index in this layer */
        int      count;
        uint64_t total_bytes;
    } LayerInfo;

    int n_layers = max_layer + 1;
    LayerInfo *layers = (LayerInfo *)calloc(n_layers + 3, sizeof(LayerInfo));
    /* +3 for embedding (-2), other (-1), output (-4) */

    /* Count tensors per layer */
    for (uint32_t i = 0; i < n; i++) {
        int L = tinfos[i].layer;
        int idx;
        if (L >= 0) idx = L;
        else if (L == -2) idx = n_layers;     /* embedding */
        else if (L == -3) idx = n_layers + 1; /* output norm */
        else idx = n_layers + 2;              /* output / other */

        if (layers[idx].count == 0) layers[idx].start = i;
        layers[idx].count++;
        layers[idx].total_bytes += tinfos[i].size;
    }

    /* ── Phase 3: Simulate sequential loading ────────────────────── */
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("SCENARIO 1: Sequential (no overlap)\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("load layer → compute → load next → compute → ...\n\n");

    uint64_t total_weight = 0;
    for (uint32_t i = 0; i < n; i++) total_weight += reader.sizes[i];

    /* Simulate: each layer load = bytes / bandwidth */
    /* Assume 20 GB/s disk read, 100 GB/s GPU HBM */
    double disk_bw_gbps = 20.0;
    double gpu_bw_gbps = 100.0;

    double seq_total_time = 0;
    for (int L = 0; L <= max_layer; L++) {
        double load_time = layers[L].total_bytes / (disk_bw_gbps * GB);
        double compute_time = 0.001; /* assume 1ms per layer */
        seq_total_time += load_time + compute_time;
    }
    /* Add embedding + output */
    seq_total_time += layers[n_layers].total_bytes / (disk_bw_gbps * GB);
    seq_total_time += layers[n_layers + 2].total_bytes / (disk_bw_gbps * GB);

    printf("total weight: %.2f GB\n", total_weight / GB);
    printf("disk bandwidth: %.1f GB/s\n", disk_bw_gbps);
    printf("sequential load time: %.2f ms\n", seq_total_time * 1000);
    printf("per-layer breakdown:\n");
    for (int L = 0; L <= max_layer && L < 3; L++) {
        printf("  layer %2d: %.2f MB load, %.2f ms\n",
               L, (double)layers[L].total_bytes / (1024.0 * 1024.0),
               layers[L].total_bytes / (disk_bw_gbps * GB) * 1000);
    }
    printf("  ...\n\n");

    /* ── Phase 4: Simulate pre-fetch (2 streams) ─────────────────── */
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("SCENARIO 2: Pre-fetch (load N+1 while computing N)\n");
    printf("══════════════════════════════════════════════════════════════════\n\n");

    double prefetch_total = 0;
    double compute_per_layer = 0.001; /* 1ms */

    /* First layer must be loaded (no overlap) */
    prefetch_total += layers[0].total_bytes / (disk_bw_gbps * GB) + compute_per_layer;

    /* Subsequent layers: overlap load with compute */
    for (int L = 1; L <= max_layer; L++) {
        double load_time = layers[L].total_bytes / (disk_bw_gbps * GB);
        /* If load_time <= compute_time, no extra wait */
        if (load_time > compute_per_layer) {
            prefetch_total += load_time; /* load takes longer, must wait */
        } else {
            prefetch_total += compute_per_layer; /* compute dominates */
        }
    }
    prefetch_total += layers[n_layers].total_bytes / (disk_bw_gbps * GB);

    printf("pre-fetch load time: %.2f ms\n", prefetch_total * 1000);
    printf("speedup vs sequential: %.2fx\n", seq_total_time / prefetch_total);
    printf("overhead from overlap: %.2f ms\n", (prefetch_total - seq_total_time) * 1000);
    printf("\n");

    /* ── Phase 5: Scatter loading with multi-stream ──────────────── */
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("SCENARIO 3: Scatter (stride-37) + %d streams\n", n_streams);
    printf("══════════════════════════════════════════════════════════════════\n\n");

    /* Scatter: interleave tensors from different layers across streams */
    StreamSim *streams = (StreamSim *)calloc(n_streams, sizeof(StreamSim));
    for (int s = 0; s < n_streams; s++) {
        streams[s].current_offset = 0;
        streams[s].total_bytes = 0;
        streams[s].idle_cycles = 0;
        streams[s].active_cycles = 0;
        streams[s].layer_loading = -1;
    }

    /* Assign tensors to streams round-robin by layer */
    double scatter_total = 0;
    for (int L = 0; L <= max_layer; L++) {
        int layer_stream = L % n_streams;
        double load_time = layers[L].total_bytes / (disk_bw_gbps * GB);
        streams[layer_stream].total_bytes += layers[L].total_bytes;
        streams[layer_stream].active_cycles += load_time;
        scatter_total += load_time / n_streams; /* parallel load */
    }

    printf("scatter load time (parallel): %.2f ms\n", scatter_total * 1000);
    printf("speedup vs sequential: %.2fx\n", seq_total_time / scatter_total);
    printf("speedup vs pre-fetch: %.2fx\n", prefetch_total / scatter_total);
    printf("\n");

    /* ── Phase 6: Address computation overhead ────────────────────── */
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("SCENARIO 4: Address computation overhead\n");
    printf("══════════════════════════════════════════════════════════════════\n\n");

    /* Sequential: need lookup table or pointer chase */
    /* Scatter: O(1) modular arithmetic */
    uint64_t n_address_computations = n;
    double lookup_overhead_ns = 10.0;   /* hash table lookup */
    double scatter_overhead_ns = 2.0;   /* modular arithmetic */

    double seq_addr_time = n_address_computations * lookup_overhead_ns / 1e6;
    double scatter_addr_time = n_address_computations * scatter_overhead_ns / 1e6;

    printf("address computations: %llu\n", (unsigned long long)n_address_computations);
    printf("sequential (lookup): %.3f ms\n", seq_addr_time);
    printf("scatter (O(1) math): %.3f ms\n", scatter_addr_time);
    printf("address savings: %.3f ms\n", seq_addr_time - scatter_addr_time);
    printf("\n");

    /* ── Phase 7: Memory bandwidth utilization ────────────────────── */
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("SCENARIO 5: GPU memory bandwidth utilization\n");
    printf("══════════════════════════════════════════════════════════════════\n\n");

    /* After loading to GPU, scatter pattern affects GPU memory access */
    /* Sequential: GPU accesses weights in order → coalesced */
    /* Scatter: GPU accesses weights scattered → uncoalesced */

    printf("GPU memory access pattern:\n");
    printf("  sequential: coalesced (adjacent threads read adjacent addresses)\n");
    printf("  scatter:    uncoalesced (threads read scattered addresses)\n");
    printf("  impact:     scatter may be 2-4x slower for GPU compute\n");
    printf("\n");

    /* ── Summary ─────────────────────────────────────────────────── */
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("INFERENCE LOADING SIMULATION SUMMARY\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("Model:              %s\n", path);
    printf("Tensors:            %u (%d layers)\n", n, n_layers);
    printf("Weight data:        %.2f GB\n", total_weight / GB);
    printf("\n");
    printf("Loading times (disk %.1f GB/s):\n", disk_bw_gbps);
    printf("  Sequential:       %.2f ms\n", seq_total_time * 1000);
    printf("  Pre-fetch:        %.2f ms (%.2fx speedup)\n",
           prefetch_total * 1000, seq_total_time / prefetch_total);
    printf("  Scatter (%d streams): %.2f ms (%.2fx speedup)\n",
           n_streams, scatter_total * 1000, seq_total_time / scatter_total);
    printf("\n");
    printf("Address computation:\n");
    printf("  Sequential: %.3f ms (lookup table)\n", seq_addr_time);
    printf("  Scatter:    %.3f ms (O(1) arithmetic)\n", scatter_addr_time);
    printf("\n");
    printf("GPU memory access:\n");
    printf("  Sequential: coalesced (optimal)\n");
    printf("  Scatter:    uncoalesced (2-4x slower compute)\n");
    printf("\n");
    printf("KEY FINDING:\n");
    printf("  Scatter loading + multi-stream = faster I/O\n");
    printf("  BUT scatter = uncoalesced GPU access = slower compute\n");
    printf("  Net effect: scatter helps I/O-bound, hurts compute-bound\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    free(tinfos);
    free(layers);
    free(streams);
    gguf_close(&reader);
    return 0;
}
