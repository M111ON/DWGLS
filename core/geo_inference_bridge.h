/* ═══════════════════════════════════════════════════════════════════════════
 * geo_inference_bridge.h — GGUF → GEO Mapping Bridge
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PURPOSE:
 *   Read GGUF file, extract tensor metadata, build GeoTensorMap.
 *   Enables inference engine to resolve tensor names → GEO block ranges.
 *
 * USAGE:
 *   GeoTensorMap map;
 *   geo_bridge_build_from_gguf("model.gguf", &map);
 *   geo_tensor_map_write(&map, "model.geo.meta");
 *
 *   // Later, resolve tensor:
 *   const GeoTensorEntry *e = geo_tensor_map_find(&map, "blk.0.attn_q.weight");
 *   // e->geo_block_start, e->geo_block_count → read from GEO file
 *
 * DEPENDS: gguf_reader.h (from runner/), geo_tensor_map.h
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_INFERENCE_BRIDGE_H
#define GEO_INFERENCE_BRIDGE_H

#include "geo_tensor_map.h"

/* GGUF reader — use the one from runner/ */
#include "../../FGLS_new/runner/gguf_reader.h"

/* ═══════════════════════════════════════════════════════════════
   GGUF TYPE SIZE COMPUTATION
   ═══════════════════════════════════════════════════════════════ */

/* GGML type info: {type_size, block_size} */
static const struct { uint16_t tsz; uint16_t blck; } GEO_TINFO[31] = {
    {4,   1},   /* F32    = 0  */
    {2,   1},   /* F16    = 1  */
    {18,  32},  /* Q4_0   = 2  */
    {20,  32},  /* Q4_1   = 3  */
    {0,   0},   /* 4 removed */
    {0,   0},   /* 5 removed */
    {22,  32},  /* Q5_0   = 6  */
    {24,  32},  /* Q5_1   = 7  */
    {34,  32},  /* Q8_0   = 8  */
    {36,  32},  /* Q8_1   = 9  */
    {84,  256}, /* Q2_K   = 10 */
    {110, 256}, /* Q3_K   = 11 */
    {144, 256}, /* Q4_K   = 12 */
    {176, 256}, /* Q5_K   = 13 */
    {210, 256}, /* Q6_K   = 14 */
    {292, 256}, /* Q8_K   = 15 */
    {2,   256}, /* IQ2_XXS= 16 */
    {2,   256}, /* IQ2_XS = 17 */
    {2,   256}, /* IQ3_XXS= 18 */
    {1,   256}, /* IQ1_S  = 19 */
    {2,   32},  /* IQ4_NL = 20 */
    {1,   256}, /* IQ3_S  = 21 */
    {1,   256}, /* IQ2_S  = 22 */
    {2,   256}, /* IQ4_XS = 23 */
    {1,   1},   /* I8     = 24 */
    {2,   1},   /* I16    = 25 */
    {4,   1},   /* I32    = 26 */
    {8,   1},   /* I64    = 27 */
    {8,   1},   /* F64    = 28 */
    {1,   256}, /* IQ1_M  = 29 */
    {2,   1},   /* BF16   = 30 */
};

/* Compute tensor data size from dimensions + dtype */
static inline uint64_t geo_tensor_data_size(uint32_t n_dims,
                                            const int64_t dims[4],
                                            uint32_t dtype)
{
    if (dtype >= 31 || GEO_TINFO[dtype].tsz == 0) return 0;

    uint64_t n_elems = 1;
    for (uint32_t d = 0; d < n_dims; d++) {
        n_elems *= (uint64_t)dims[d];
    }

    uint64_t blck = GEO_TINFO[dtype].blck;
    uint64_t tsz  = GEO_TINFO[dtype].tsz;
    if (blck == 0) return 0;

    return (n_elems / blck) * tsz;
}

/* ═══════════════════════════════════════════════════════════════
   BUILD MAP FROM GGUF
   ═══════════════════════════════════════════════════════════════ */

/*
 * Read GGUF file and build GEO tensor map.
 * Each tensor maps to sequential GEO FrustumBlocks.
 *
 * Returns: number of tensors mapped, or 0 on error.
 */
static inline uint32_t geo_bridge_build_from_gguf(const char *gguf_path,
                                                   GeoTensorMap *map,
                                                   const char *model_name_override)
{
    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        fprintf(stderr, "geo_bridge: failed to open GGUF: %s\n", gguf_path);
        return 0;
    }

    geo_tensor_map_init(map);

    /* Set model name */
    if (model_name_override) {
        strncpy(map->header.model_name, model_name_override, 63);
    } else {
        /* Extract filename from path */
        const char *slash = strrchr(gguf_path, '/');
        if (!slash) slash = strrchr(gguf_path, '\\');
        strncpy(map->header.model_name, slash ? slash + 1 : gguf_path, 63);
    }

    uint32_t current_block = 0;  /* sequential GEO block counter */

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        /* Read tensor info from GGUF reader */
        const char *name = gguf.names[i];
        uint64_t data_off = gguf.offsets[i];
        uint32_t data_size = gguf.sizes[i];

        /* We need dimensions and dtype — re-read from GGUF file */
        /* For now, compute from sizes: assume Q8_0 (34B per 32 weights) */
        /* A more complete version would re-parse the GGUF header */

        /* Map to GEO blocks */
        uint32_t n_geo_blocks = (data_size + GEO_FBLOCK_SZ - 1) / GEO_FBLOCK_SZ;

        /* Build entry with available info */
        GeoTensorEntry entry;
        memset(&entry, 0, sizeof(entry));

        strncpy(entry.name, name, GEO_TENSOR_NAME_MAX - 1);
        entry.dtype = GEO_DTYPE_Q8_0;  /* default — would be read from GGUF */
        entry.data_size = data_size;
        entry.n_elems = data_size * 32 / 34;  /* approximate for Q8_0 */
        entry.n_dims = 2;  /* most tensors are 2D */
        entry.dims[0] = 0;  /* unknown without re-parsing */
        entry.dims[1] = 0;

        entry.geo_block_start = current_block;
        entry.geo_block_count = n_geo_blocks;
        entry.geo_data_offset = (uint64_t)current_block * GEO_FBLOCK_SZ;

        /* Add to map */
        if (map->header.n_tensors < GEO_MAX_TENSORS) {
            map->tensors[map->header.n_tensors++] = entry;
            map->header.total_blocks += n_geo_blocks;
            map->header.total_data_size += data_size;
            current_block += n_geo_blocks;
        }
    }

    return map->header.n_tensors;
}

/* ═══════════════════════════════════════════════════════════════
   RESOLVE TENSOR → GEO ADDRESS
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Given a tensor name, return the GEO block range.
 * Use this in inference engine to read weights from GEO file.
 *
 * Example:
 *   GeoTensorEntry *e = geo_bridge_resolve(&map, "blk.0.attn_q.weight");
 *   if (e) {
 *       // Read e->geo_block_count blocks starting at e->geo_block_start
 *       fseek(geo_file, e->geo_data_offset, SEEK_SET);
 *       fread(buffer, e->geo_block_count * GEO_FBLOCK_SZ, 1, geo_file);
 *   }
 */
static inline const GeoTensorEntry* geo_bridge_resolve(const GeoTensorMap *map,
                                                       const char *tensor_name)
{
    return geo_tensor_map_find(map, tensor_name);
}

/* ═══════════════════════════════════════════════════════════════
   GEO FILE I/O HELPERS
   ═══════════════════════════════════════════════════════════════ */

/* GEO file header (same as GEOF format) */
typedef struct {
    char     magic[4];    /* "GEOF" */
    uint32_t version;
    uint32_t gp_level;
    uint32_t n_blocks;
    uint64_t orig_size;
    uint8_t  digest[8];   /* xxh64 */
    uint8_t  pad[8];
} GeoFileHeader;

/*
 * Read a tensor's weight data from GEO file.
 * Caller provides buffer of sufficient size.
 *
 * Returns: bytes read, or 0 on error.
 */
static inline size_t geo_bridge_read_tensor(const char *geo_path,
                                            const GeoTensorEntry *tensor,
                                            void *buffer,
                                            size_t buffer_size)
{
    FILE *f = fopen(geo_path, "rb");
    if (!f) return 0;

    /* Skip GEO header (32 bytes) */
    fseek(f, 32, SEEK_SET);

    /* Seek to tensor's block range */
    uint64_t offset = (uint64_t)tensor->geo_block_start * GEO_FBLOCK_SZ;
    fseek(f, (long)offset, SEEK_SET);

    /* Read blocks */
    size_t to_read = tensor->geo_block_count * GEO_FBLOCK_SZ;
    if (to_read > buffer_size) to_read = buffer_size;

    size_t read = fread(buffer, 1, to_read, f);
    fclose(f);

    return read;
}

/* ═══════════════════════════════════════════════════════════════
   PRINT MAPPING TABLE
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_bridge_print_mapping(const GeoTensorMap *map) {
    printf("===============================================================\n");
    printf("  GEO ↔ GGUF Tensor Mapping\n");
    printf("===============================================================\n");
    printf("  %-40s %8s %8s %10s\n", "Tensor", "Block#", "Blocks", "Size");
    printf("  %-40s %8s %8s %10s\n", "------", "------", "------", "----");

    for (uint32_t i = 0; i < map->header.n_tensors; i++) {
        const GeoTensorEntry *e = &map->tensors[i];
        printf("  %-40s %8u %8u %8.1f KB\n",
               e->name,
               e->geo_block_start,
               e->geo_block_count,
               e->data_size / 1024.0);
    }

    printf("---------------------------------------------------------------\n");
    printf("  Total: %u tensors, %u blocks, %.1f MB\n",
           map->header.n_tensors,
           map->header.total_blocks,
           map->header.total_data_size / 1024.0 / 1024.0);
    printf("===============================================================\n");
}

#endif /* GEO_INFERENCE_BRIDGE_H */
