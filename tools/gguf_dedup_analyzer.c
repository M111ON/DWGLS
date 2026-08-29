/*
 * tools/gguf_dedup_analyzer.c — analyze GGUF for weight dedup potential
 * ════════════════════════════════════════════════════════════════════════
 * Reads GGUF via memory-mapped bulk reader, hashes each tensor's data,
 * groups by hash to find exact duplicates and near-duplicates.
 *
 * BUILD: gcc -O2 -std=c11 -o gguf_dedup_analyzer tools/gguf_dedup_analyzer.c -lm
 * USAGE: gguf_dedup_analyzer.exe <model.gguf> [--near THRESHOLD]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../core/gguf_reader.h"

/* ── xxHash64 (simplified, for hashing tensor data) ────────────── */
static uint64_t xxh64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0x165667B19D37E5E3ULL;
    const uint64_t prime1 = 0x9E3779B185EBCA87ULL;
    const uint64_t prime2 = 0xC2B2AE3D27D4EB4FULL;
    const uint64_t prime3 = 0x27D4EB2F165667C5ULL;
    const uint64_t prime4 = 0x165667B19D37E5E3ULL;

    size_t i = 0;
    /* process 8-byte chunks */
    for (; i + 8 <= len; i += 8) {
        uint64_t k;
        memcpy(&k, p + i, 8);
        k *= prime2; k = (k << 31) | (k >> 33); k *= prime1;
        h ^= k; h = (h << 27) | (h >> 37); h = h * 5 + 0x165667B19D37E5E3ULL;
    }
    /* process remaining bytes */
    uint64_t k = 0;
    for (; i < len; i++) {
        k ^= (uint64_t)p[i] << ((i % 8) * 8);
    }
    if (k != 0) {
        k *= prime3; k = (k << 31) | (k >> 33); k *= prime4;
        h ^= k;
    }
    /* final mix */
    h ^= len;
    h ^= h >> 33; h *= prime2; h ^= h >> 29; h *= prime3; h ^= h >> 32;
    return h;
}

/* ── simple stats for each tensor type ─────────────────────────── */
static const char *ggml_type_name(uint8_t dt) {
    switch(dt) {
        case 0:  return "f32";
        case 1:  return "f16";
        case 2:  return "q4_0";
        case 3:  return "q4_1";
        case 7:  return "q8_0";
        case 8:  return "q8_1";
        case 10: return "q2_K";
        case 11: return "q3_K";
        case 12: return "q4_K";
        case 13: return "q5_K";
        case 14: return "q6_K";
        case 16: return "iq2_xxs";
        case 17: return "iq3_xs";
        case 18: return "iq1_s";
        case 19: return "iq4_nl";
        case 20: return "iq3_s";
        case 21: return "iq2_s";
        case 22: return "iq4_xs";
        case 23: return "iq1_m";
        default: return "unknown";
    }
}

/* ── Hash entry for dedup tracking ─────────────────────────────── */
typedef struct {
    uint64_t hash;
    int      tensor_idx;
} HashEntry;

static int cmp_hash(const void *a, const void *b) {
    uint64_t ha = ((const HashEntry *)a)->hash;
    uint64_t hb = ((const HashEntry *)b)->hash;
    if (ha < hb) return -1;
    if (ha > hb) return 1;
    return 0;
}

static int cmp_size_desc(const void *a, const void *b) {
    size_t sa = *(const size_t *)a;
    size_t sb = *(const size_t *)b;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return 0;
}

/* ── Per-layer analysis ────────────────────────────────────────── */
typedef struct {
    const char *name;
    uint32_t    size;
    uint8_t     dtype;
    uint64_t    hash;
} TensorInfo;

/* Extract layer index from name like "blk.3.attn_q.weight" → 3 */
static int extract_layer(const char *name) {
    if (strncmp(name, "blk.", 4) == 0) {
        return atoi(name + 4);
    }
    return -1;
}

/* Extract component from name */
static const char *extract_component(const char *name) {
    const char *dot = strchr(name, '.');
    if (!dot) return name;
    dot = strchr(dot + 1, '.');
    if (!dot) return name;
    return dot + 1;
}

/* ── Main ──────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [--near THRESHOLD]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    float near_threshold = 0.0f;
    if (argc > 3 && strcmp(argv[2], "--near") == 0) {
        near_threshold = (float)atof(argv[3]);
    }

    GgufReader reader;
    memset(&reader, 0, sizeof(reader));

    if (gguf_open(path, &reader) != 0) {
        fprintf(stderr, "ERROR: cannot open %s\n", path);
        return 1;
    }

    printf("=== GGUF Weight Dedup Analyzer ===\n");
    printf("file: %s\n", path);
    printf("tensors: %u\n", reader.n_tensors);
    printf("data offset: %llu bytes\n\n", (unsigned long long)reader.data_offset);

    /* Compute file size */
    uint64_t file_sz = 0;
    if (reader.base) {
        file_sz = reader.base_sz;
    } else {
        FILE *f = fopen(path, "rb");
        if (f) { _fseeki64(f, 0, SEEK_END); file_sz = _ftelli64(f); fclose(f); }
    }
    printf("file size: %.2f MB\n\n", (double)file_sz / (1024.0 * 1024.0));

    /* ── Phase 1: Collect tensor metadata ───────────────────────── */
    uint32_t n = reader.n_tensors;
    TensorInfo *tensors = (TensorInfo *)malloc(n * sizeof(TensorInfo));
    size_t *sizes = (size_t *)malloc(n * sizeof(size_t));
    uint64_t total_weight_bytes = 0;

    for (uint32_t i = 0; i < n; i++) {
        tensors[i].name = reader.names[i];
        tensors[i].size = reader.sizes[i];
        tensors[i].dtype = reader.dtypes[i];
        sizes[i] = reader.sizes[i];
        total_weight_bytes += reader.sizes[i];
    }

    printf("─── TENSOR SIZE DISTRIBUTION ───\n");
    printf("total weight data: %.2f MB\n", (double)total_weight_bytes / (1024.0 * 1024.0));
    printf("tensor count: %u\n\n", n);

    /* Size histogram */
    size_t size_buckets[] = {0, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216, UINT64_MAX};
    const char *size_labels[] = {"0-1KB", "1-4KB", "4-16KB", "16-64KB", "64-256KB", "256KB-1MB", "1-4MB", "4-16MB", "16MB+"};
    int nbuckets = 9;
    printf("%-12s %6s %12s\n", "Size Range", "Count", "Total MB");
    printf("%-12s %6s %12s\n", "----------", "-----", "--------");
    for (int b = 0; b < nbuckets; b++) {
        int count = 0;
        double total = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (sizes[i] >= size_buckets[b] && sizes[i] < size_buckets[b + 1]) {
                count++;
                total += sizes[i];
            }
        }
        if (count > 0) {
            printf("%-12s %6d %10.2f MB\n", size_labels[b], count, total / (1024.0 * 1024.0));
        }
    }
    printf("\n");

    /* Type distribution */
    printf("─── TYPE DISTRIBUTION ───\n");
    printf("%-10s %6s %12s\n", "Type", "Count", "Total MB");
    printf("%-10s %6s %12s\n", "----", "-----", "--------");
    uint8_t type_seen[256] = {0};
    for (uint32_t i = 0; i < n; i++) type_seen[reader.dtypes[i]] = 1;
    for (int t = 0; t < 256; t++) {
        if (!type_seen[t]) continue;
        int count = 0;
        double total = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (reader.dtypes[i] == t) {
                count++;
                total += reader.sizes[i];
            }
        }
        printf("%-10s %6d %10.2f MB\n", ggml_type_name(t), count, total / (1024.0 * 1024.0));
    }
    printf("\n");

    /* ── Phase 2: Hash each tensor ──────────────────────────────── */
    printf("─── HASHING TENSORS ───\n");
    printf("hashing %u tensors...\n", n);

    HashEntry *entries = (HashEntry *)malloc(n * sizeof(HashEntry));
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = reader.offsets[i] + reader.data_offset;
        uint32_t sz = reader.sizes[i];
        const uint8_t *data = reader.base + off;
        entries[i].hash = xxh64(data, sz);
        entries[i].tensor_idx = i;
        if ((i + 1) % 1000 == 0 || i == n - 1) {
            printf("  hashed %u/%u\n", i + 1, n);
        }
    }
    printf("\n");

    /* ── Phase 3: Find exact duplicates ─────────────────────────── */
    printf("─── EXACT DUPLICATE ANALYSIS ───\n");
    qsort(entries, n, sizeof(HashEntry), cmp_hash);

    int n_groups = 0;
    int n_dup_tensors = 0;
    uint64_t dup_saved_bytes = 0;

    uint32_t gi = 0;
    while (gi < n) {
        uint32_t gj = gi + 1;
        while (gj < n && entries[gj].hash == entries[gi].hash) gj++;
        int group_size = gj - gi;
        if (group_size > 1) {
            n_groups++;
            n_dup_tensors += group_size - 1;
            uint64_t tensor_sz = reader.sizes[entries[gi].tensor_idx];
            dup_saved_bytes += tensor_sz * (group_size - 1);
            if (n_groups <= 20) {
                printf("  GROUP %d (%d tensors, %.2f KB each):\n", n_groups, group_size,
                       (double)tensor_sz / 1024.0);
                for (uint32_t k = gi; k < gj && k < gi + 5; k++) {
                    printf("    [%u] %s\n", entries[k].tensor_idx, reader.names[entries[k].tensor_idx]);
                }
                if (group_size > 5) printf("    ... and %d more\n", group_size - 5);
            }
        }
        gi = gj;
    }

    printf("\n");
    printf("exact duplicate groups: %d\n", n_groups);
    printf("duplicate tensors: %d (of %u = %.1f%%)\n", n_dup_tensors, n, 100.0 * n_dup_tensors / n);
    printf("potential savings: %.2f MB (%.1f%% of %.2f MB)\n",
           (double)dup_saved_bytes / (1024.0 * 1024.0),
           100.0 * dup_saved_bytes / total_weight_bytes,
           (double)total_weight_bytes / (1024.0 * 1024.0));
    printf("\n");

    /* ── Phase 4: Per-layer similarity (same shape + same type) ──── */
    if (near_threshold > 0) {
        printf("─── NEAR-DUPLICATE ANALYSIS (threshold: %.2f%%) ───\n", near_threshold * 100);
        printf("comparing tensors with same shape + type...\n");

        /* Group by (size, dtype) first */
        typedef struct { int idx; } IdxEntry;
        int n_near_groups = 0;
        uint64_t near_saved = 0;

        /* Simple O(n²) comparison for same-size groups */
        uint8_t *visited = (uint8_t *)calloc(n, 1);
        for (uint32_t i = 0; i < n; i++) {
            if (visited[i]) continue;
            uint32_t si = reader.sizes[i];
            uint8_t  di = reader.dtypes[i];
            uint64_t off_i = reader.offsets[i] + reader.data_offset;
            const uint8_t *data_i = reader.base + off_i;

            int group_count = 1;
            for (uint32_t j = i + 1; j < n; j++) {
                if (visited[j]) continue;
                if (reader.sizes[j] != si || reader.dtypes[j] != di) continue;

                uint64_t off_j = reader.offsets[j] + reader.data_offset;
                const uint8_t *data_j = reader.base + off_j;

                /* Hamming distance ratio */
                int diffs = 0;
                int total_bytes = si < 1024 ? si : 1024; /* sample first 1KB */
                for (int b = 0; b < total_bytes; b++) {
                    if (data_i[b] != data_j[b]) diffs++;
                }
                float diff_ratio = (float)diffs / total_bytes;
                if (diff_ratio <= near_threshold) {
                    visited[j] = 1;
                    group_count++;
                }
            }
            if (group_count > 1) {
                n_near_groups++;
                near_saved += (uint64_t)(si) * (group_count - 1);
                if (n_near_groups <= 10) {
                    printf("  NEAR GROUP %d (%d tensors, %.1f%% diff):\n", n_near_groups, group_count, near_threshold * 100);
                    printf("    [%u] %s\n", i, reader.names[i]);
                    for (uint32_t j = i + 1; j < n && j <= i + 3; j++) {
                        if (visited[j] || reader.sizes[j] != si || reader.dtypes[j] != di) continue;
                        printf("    [%u] %s\n", j, reader.names[j]);
                    }
                }
            }
            visited[i] = 1;
        }
        free(visited);

        printf("\n");
        printf("near-duplicate groups: %d\n", n_near_groups);
        printf("potential near-dup savings: %.2f MB (%.1f%%)\n",
               (double)near_saved / (1024.0 * 1024.0),
               100.0 * near_saved / total_weight_bytes);
        printf("\n");
    }

    /* ── Phase 5: Layer-by-layer structure analysis ──────────────── */
    printf("─── LAYER STRUCTURE ANALYSIS ───\n");
    printf("%-6s %-20s %6s %10s\n", "Layer", "Component", "Type", "Size KB");
    printf("%-6s %-20s %6s %10s\n", "-----", "---------", "----", "--------");

    int max_layer = -1;
    for (uint32_t i = 0; i < n; i++) {
        int L = extract_layer(reader.names[i]);
        if (L > max_layer) max_layer = L;
    }

    /* Print first 3 layers as sample */
    int layers_shown = 0;
    for (int L = 0; L <= max_layer && layers_shown < 3; L++) {
        int found = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (extract_layer(reader.names[i]) == L) { found = 1; break; }
        }
        if (!found) continue;
        for (uint32_t i = 0; i < n; i++) {
            if (extract_layer(reader.names[i]) != L) continue;
            const char *comp = extract_component(reader.names[i]);
            printf("  %-4d %-20s %6s %10.2f\n", L, comp,
                   ggml_type_name(reader.dtypes[i]),
                   (double)reader.sizes[i] / 1024.0);
        }
        layers_shown++;
        if (L < max_layer) printf("  ...\n");
    }
    printf("\n");

    /* ── Summary ─────────────────────────────────────────────────── */
    printf("════════════════════════════════════════\n");
    printf("SUMMARY\n");
    printf("════════════════════════════════════════\n");
    printf("File:           %.2f MB\n", (double)file_sz / (1024.0 * 1024.0));
    printf("Weight data:    %.2f MB\n", (double)total_weight_bytes / (1024.0 * 1024.0));
    printf("Tensor count:   %u\n", n);
    printf("Exact dups:     %d groups → save %.2f MB (%.1f%%)\n",
           n_groups, (double)dup_saved_bytes / (1024.0 * 1024.0),
           100.0 * dup_saved_bytes / (total_weight_bytes > 0 ? total_weight_bytes : 1));
    printf("════════════════════════════════════════\n");

    free(tensors);
    free(sizes);
    free(entries);
    gguf_close(&reader);
    return 0;
}
