/* ═══════════════════════════════════════════════════════════════════════════
 * geo_batch_convert.c — GEO SID Batch Converter
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Converts GGUF files → GCube container format (.gcube)
 *
 * Pipeline:
 *   GGUF → gguf_index (parse tensor metadata)
 *        → geo_tensor_map (compute FrustumBlock layout)
 *        → read tensor bytes from GGUF
 *        → gcube_add_tensor (pack into GCube)
 *        → gcube_write (serialize .gcube)
 *
 * COMPILE:
 *   gcc -O2 -Wall -Wextra \
 *       -I I:/DWGLS/core -I I:/FGLS_new/runner \
 *       -o batch_convert.exe \
 *       I:/DWGLS/tools/geo_batch_convert.c -lm
 *
 * RUN:
 *   geo_batch_convert.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf ./output
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

/* ── DWGLS headers ─────────────────────────────────────────── */
#include "geo_cube_container.h"

/* ── FGLS headers ──────────────────────────────────────────── */
#include "gguf_index.h"

/* ═══════════════════════════════════════════════════════════════
   BATCH CONVERT STATE
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    const char  *gguf_path;
    const char  *out_dir;
    FILE        *gguf_file;
    GGUFTensorIndex idx;
    GCubeContainer  cube;
    uint32_t    n_tensors;
    uint32_t    n_converted;
    uint32_t    n_skipped;
    uint64_t    total_bytes;
    uint64_t    converted_bytes;
    double      elapsed_sec;
} BatchState;

/* ═══════════════════════════════════════════════════════════════
   HELPERS
   ═══════════════════════════════════════════════════════════════ */

static double now_sec(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
#ifdef _WIN32
        mkdir(path);
#else
        mkdir(path, 0755);
#endif
    }
}

static const char *dtype_name(uint32_t ggml_type) {
    switch (ggml_type) {
        case 0:  return "F32";
        case 1:  return "F16";
        case 2:  return "Q4_0";
        case 3:  return "Q4_1";
        case 6:  return "Q5_0";
        case 7:  return "Q5_1";
        case 8:  return "Q8_0";
        case 10: return "Q2_K";
        case 11: return "Q3_K";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        case 15: return "Q8_K";
        default: return "???";
    }
}

/* ═══════════════════════════════════════════════════════════════
   OPEN / CLOSE
   ═══════════════════════════════════════════════════════════════ */

static int batch_open(BatchState *s, const char *gguf_path, const char *out_dir) {
    memset(s, 0, sizeof(*s));
    s->gguf_path = gguf_path;
    s->out_dir = out_dir;

    /* Open GGUF + parse tensor index */
    if (gguf_idx_open(gguf_path, &s->idx) != 0) {
        fprintf(stderr, "ERROR: Cannot open GGUF: %s\n", gguf_path);
        return -1;
    }

    s->gguf_file = fopen(gguf_path, "rb");
    if (!s->gguf_file) {
        gguf_idx_close(&s->idx);
        return -1;
    }

    /* Initialize GCube container */
    gcube_init(&s->cube);
    const char *base = strrchr(gguf_path, '/');
    if (!base) base = strrchr(gguf_path, '\\');
    if (!base) base = gguf_path; else base++;
    strncpy(s->cube.header.model_name, base, GCUBE_MAX_MODEL - 1);

    s->n_tensors = (uint32_t)s->idx.n_tensors;
    ensure_dir(out_dir);

    printf("Batch Converter\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  GGUF:      %s\n", gguf_path);
    printf("  Output:    %s/\n", out_dir);
    printf("  Tensors:   %u\n", s->n_tensors);
    printf("───────────────────────────────────────────────────────────\n");

    return 0;
}

static void batch_close(BatchState *s) {
    if (s->gguf_file) fclose(s->gguf_file);
    gguf_idx_close(&s->idx);
    gcube_free(&s->cube);
}

/* ═══════════════════════════════════════════════════════════════
   CONVERT ALL + WRITE
   ═══════════════════════════════════════════════════════════════ */

static int batch_convert_all(BatchState *s) {
    double t0 = now_sec();

    for (uint32_t i = 0; i < s->n_tensors; i++) {
        const char *name = s->idx.names[i];

        /* Read raw tensor data */
        uint64_t offset = s->idx.offsets[i];
        uint64_t size   = s->idx.sizes[i];
        if (size == 0) { s->n_skipped++; continue; }

        uint8_t *data = (uint8_t *)malloc((size_t)size);
        if (!data) { s->n_skipped++; continue; }

        fseeko(s->gguf_file, (long)offset, SEEK_SET);
        size_t rd = fread(data, 1, (size_t)size, s->gguf_file);
        if (rd != (size_t)size) { free(data); s->n_skipped++; continue; }

        /* Compute element count */
        uint32_t dt = s->idx.dtypes[i];
        uint32_t dsz = (dt == 0) ? 4 : (dt == 1) ? 2 : 1;
        uint32_t ne = (uint32_t)(size / dsz);
        if (ne == 0) ne = 1;
        uint32_t dims[4] = {ne, 1, 1, 1};

        gcube_add_tensor(&s->cube, name, 1, dims, dt, ne, data, (uint32_t)size);
        free(data);

        s->n_converted++;
        s->converted_bytes += size;

        if (s->n_converted % 50 == 0) {
            printf("    ... %u/%u tensors (%.1f MB)\n",
                   s->n_converted, s->n_tensors,
                   s->converted_bytes / 1048576.0);
        }
    }

    s->elapsed_sec = now_sec() - t0;

    /* Construct output path: <out_dir>/<model_name>.gcube */
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/%s.gcube",
             s->out_dir, s->cube.header.model_name);

    /* Strip .gguf from model name in path */
    char *dot = strstr(out_path, ".gguf.gcube");
    if (dot) memmove(dot, ".gcube", 6);

    int wrc = gcube_write(&s->cube, out_path);
    if (wrc != 0) {
        fprintf(stderr, "ERROR: cannot write %s\n", out_path);
        return -1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   REPORT
   ═══════════════════════════════════════════════════════════════ */

static void batch_report(BatchState *s, const char *out_path) {
    uint32_t hdr_sz = GCUBE_FILE_HDR_SZ +
                      s->cube.header.n_tensors * GCUBE_TENSOR_HDR_SZ;
    uint32_t blk_sz = s->cube.header.total_blocks * GCUBE_BLOCK_SZ;
    uint32_t file_sz = hdr_sz + blk_sz + 4;

    printf("\n───────────────────────────────────────────────────────────\n");
    printf("  CONVERSION COMPLETE\n");
    printf("───────────────────────────────────────────────────────────\n");
    printf("  Tensors:      %u/%u converted, %u skipped\n",
           s->n_converted, s->n_tensors, s->n_skipped);
    printf("  Data:         %.1f MB\n",
           s->converted_bytes / 1048576.0);
    printf("  GCube blocks:  %u (%u KB)\n",
           s->cube.header.total_blocks, blk_sz / 1024);
    printf("  File size:     %u bytes (%.1f KB)\n",
           file_sz, file_sz / 1024.0);
    printf("  Overhead:      %.1f%%\n",
           s->converted_bytes > 0 ?
           100.0 * (file_sz - (uint32_t)s->converted_bytes) / file_sz : 0.0);
    printf("  Time:          %.2f sec\n", s->elapsed_sec);
    printf("  Throughput:    %.1f MB/sec\n",
           s->elapsed_sec > 0 ?
           s->converted_bytes / s->elapsed_sec / 1048576.0 : 0);
    printf("───────────────────────────────────────────────────────────\n");
    printf("  OUTPUT:  %s\n", out_path);
    printf("═══════════════════════════════════════════════════════════\n");

    /* Print stats summary */
    gcube_stats(&s->cube);
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("GEO Batch Converter — GGUF → GCube\n");
        printf("Usage: %s <gguf_path> <output_dir>\n", argv[0]);

        printf("\nExample:\n");
        printf("  %s I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf ./output\n", argv[0]);
        return 1;
    }

    const char *gguf_path = argv[1];
    const char *out_dir = argv[2];

    /* Check GGUF exists */
    {
        FILE *test = fopen(gguf_path, "rb");
        if (!test) {
            fprintf(stderr, "ERROR: GGUF not found: %s\n", gguf_path);
            return 1;
        }
        fclose(test);
    }

    ensure_dir(out_dir);

    BatchState s;
    if (batch_open(&s, gguf_path, out_dir) != 0) return 1;

    /* Construct output path */
    char out_path[1024];
    const char *base = strrchr(gguf_path, '/');
    if (!base) base = strrchr(gguf_path, '\\');
    if (!base) base = gguf_path; else base++;
    snprintf(out_path, sizeof(out_path), "%s/%s.gcube", out_dir, base);
    /* Replace .gguf.gcube → .gcube */
    char *dot_gguf = strstr(out_path, ".gguf.gcube");
    if (dot_gguf) {
        size_t remaining = strlen(dot_gguf);
        memmove(dot_gguf, ".gcube", 6);
        /* Zero out dangling "guf" */
        memset(dot_gguf + 6, 0, remaining - 6);
    }

    if (batch_convert_all(&s) != 0) {
        batch_close(&s);
        return 1;
    }

    batch_report(&s, out_path);
    batch_close(&s);
    return 0;
}