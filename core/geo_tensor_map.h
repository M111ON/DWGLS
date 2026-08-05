/* ═══════════════════════════════════════════════════════════════════════════
 * geo_tensor_map.h — GEO ↔ GGUF Tensor Mapping
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PURPOSE:
 *   Map tensor names (from GGUF) → GEO block ranges.
 *   Enables inference engine to resolve "blk.0.attn_q.weight" → FrustumBlock.
 *
 * STRUCTURE:
 *   GeoTensorMap: array of {name, block_start, block_count, n_elems, dtype}
 *   Stored in GEO header extension OR as .geo.meta sidecar file
 *
 * DEPENDS: stdint.h, stdio.h, string.h
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_TENSOR_MAP_H
#define GEO_TENSOR_MAP_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define GEO_TENSOR_MAP_MAGIC    0x5453454Du  /* "TSEM" = Tensor SEt Map */
#define GEO_TENSOR_MAP_VERSION  1
#define GEO_TENSOR_NAME_MAX     128
#define GEO_MAX_TENSORS         512          /* max tensors per model */

/* GEO block geometry */
#define GEO_FBLOCK_SZ           4896         /* FrustumBlock size */
#define GEO_DBLOCK_SZ           64           /* DiamondBlock size */
#define GEO_DBLOCKS_PER_FBLOCK  54           /* DiamondBlocks per FrustumBlock */

/* Q8_0 quantization */
#define Q8_0_BLOCK_SZ           34           /* 32 weights + 2B scale */
#define Q8_0_WEIGHTS_PER_BLOCK  32

/* ═══════════════════════════════════════════════════════════════
   GGML TYPE ENUM (matches ggml.h)
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    GEO_DTYPE_F32    = 0,
    GEO_DTYPE_F16    = 1,
    GEO_DTYPE_Q4_0   = 2,
    GEO_DTYPE_Q4_1   = 3,
    GEO_DTYPE_Q5_0   = 6,
    GEO_DTYPE_Q5_1   = 7,
    GEO_DTYPE_Q8_0   = 8,
    GEO_DTYPE_Q8_1   = 9,
    GEO_DTYPE_Q2_K   = 10,
    GEO_DTYPE_Q3_K   = 11,
    GEO_DTYPE_Q4_K   = 12,
    GEO_DTYPE_Q5_K   = 13,
    GEO_DTYPE_Q6_K   = 14,
    GEO_DTYPE_Q8_K   = 15,
    GEO_DTYPE_IQ4_NL = 20,
    GEO_DTYPE_BF16   = 30,
} GeoDType;

/* ═══════════════════════════════════════════════════════════════
   TENSOR ENTRY
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    char     name[GEO_TENSOR_NAME_MAX];  /* tensor name (e.g. "blk.0.attn_q.weight") */
    uint32_t n_dims;                      /* number of dimensions (1-4) */
    uint64_t dims[4];                     /* dimension sizes */
    uint32_t dtype;                       /* GGML type enum */
    uint64_t n_elems;                     /* total elements */
    uint64_t data_size;                   /* total bytes in GGUF */

    /* GEO mapping */
    uint32_t geo_block_start;             /* first FrustumBlock index */
    uint32_t geo_block_count;             /* number of FrustumBlocks */
    uint64_t geo_data_offset;             /* byte offset in GEO data section */
} GeoTensorEntry;

/* ═══════════════════════════════════════════════════════════════
   TENSOR MAP (header + entries)
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t magic;                       /* GEO_TENSOR_MAP_MAGIC */
    uint32_t version;                     /* GEO_TENSOR_MAP_VERSION */
    uint32_t n_tensors;                   /* number of tensors */
    uint32_t total_blocks;                /* total GEO blocks used */
    uint64_t total_data_size;             /* total bytes of weight data */
    char     model_name[64];              /* model name from GGUF */
} GeoTensorMapHeader;

typedef struct {
    GeoTensorMapHeader header;
    GeoTensorEntry     tensors[GEO_MAX_TENSORS];
} GeoTensorMap;

/* ═══════════════════════════════════════════════════════════════
   MAPPING FUNCTIONS
   ═══════════════════════════════════════════════════════════════ */

/* Initialize empty tensor map */
static inline void geo_tensor_map_init(GeoTensorMap *map) {
    memset(map, 0, sizeof(*map));
    map->header.magic = GEO_TENSOR_MAP_MAGIC;
    map->header.version = GEO_TENSOR_MAP_VERSION;
}

/* Add tensor entry to map */
static inline int geo_tensor_map_add(GeoTensorMap *map,
                                     const char *name,
                                     uint32_t n_dims,
                                     const uint64_t dims[4],
                                     uint32_t dtype,
                                     uint64_t n_elems,
                                     uint64_t data_size,
                                     uint32_t geo_block_start,
                                     uint32_t geo_block_count)
{
    if (map->header.n_tensors >= GEO_MAX_TENSORS) return -1;

    GeoTensorEntry *e = &map->tensors[map->header.n_tensors];
    memset(e, 0, sizeof(*e));

    strncpy(e->name, name, GEO_TENSOR_NAME_MAX - 1);
    e->n_dims = n_dims;
    for (uint32_t i = 0; i < n_dims && i < 4; i++) e->dims[i] = dims[i];
    e->dtype = dtype;
    e->n_elems = n_elems;
    e->data_size = data_size;
    e->geo_block_start = geo_block_start;
    e->geo_block_count = geo_block_count;
    e->geo_data_offset = (uint64_t)geo_block_start * GEO_FBLOCK_SZ;

    map->header.n_tensors++;
    map->header.total_blocks += geo_block_count;
    map->header.total_data_size += data_size;

    return 0;
}

/* Find tensor by name (O(n) linear search) */
static inline const GeoTensorEntry* geo_tensor_map_find(const GeoTensorMap *map,
                                                        const char *name)
{
    for (uint32_t i = 0; i < map->header.n_tensors; i++) {
        if (strcmp(map->tensors[i].name, name) == 0) {
            return &map->tensors[i];
        }
    }
    return NULL;
}

/* Find tensor by index */
static inline const GeoTensorEntry* geo_tensor_map_get(const GeoTensorMap *map,
                                                       uint32_t idx)
{
    if (idx >= map->header.n_tensors) return NULL;
    return &map->tensors[idx];
}

/* ═══════════════════════════════════════════════════════════════
   BUILD MAP FROM GGUF
   ═══════════════════════════════════════════════════════════════ */

/*
 * Build tensor map from GGUF file.
 * Maps each tensor to sequential GEO blocks.
 *
 * GGUF block size depends on dtype:
 *   Q8_0: 34B per 32 weights → n_blocks = ceil(n_elems / 32) * 34
 *   F32:  4B per weight → n_blocks = n_elems * 4
 *
 * GEO blocks: each FrustumBlock = 4896B
 *   n_geo_blocks = ceil(data_size / GEO_FBLOCK_SZ)
 */
static inline uint32_t geo_tensor_map_build_from_gguf(const char *gguf_path,
                                                       GeoTensorMap *map,
                                                       const char *model_name)
{
    /* This is a forward declaration — actual implementation
       requires GGUF reader. Use geo_inference_bridge.h for full impl. */
    (void)gguf_path; (void)map; (void)model_name;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   SERIALIZE / DESERIALIZE
   ═══════════════════════════════════════════════════════════════ */

/* Write tensor map to file (sidecar .geo.meta) */
static inline int geo_tensor_map_write(const GeoTensorMap *map, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* Write header */
    fwrite(&map->header, sizeof(GeoTensorMapHeader), 1, f);

    /* Write tensor entries */
    for (uint32_t i = 0; i < map->header.n_tensors; i++) {
        fwrite(&map->tensors[i], sizeof(GeoTensorEntry), 1, f);
    }

    fclose(f);
    return 0;
}

/* Read tensor map from file */
static inline int geo_tensor_map_read(GeoTensorMap *map, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Read header */
    if (fread(&map->header, sizeof(GeoTensorMapHeader), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (map->header.magic != GEO_TENSOR_MAP_MAGIC) {
        fclose(f);
        return -1;
    }

    /* Read tensor entries */
    for (uint32_t i = 0; i < map->header.n_tensors; i++) {
        if (fread(&map->tensors[i], sizeof(GeoTensorEntry), 1, f) != 1) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   STATISTICS / PRINT
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_tensor_map_stats(const GeoTensorMap *map) {
    printf("===============================================================\n");
    printf("  GEO Tensor Map\n");
    printf("---------------------------------------------------------------\n");
    printf("  Model:          %s\n", map->header.model_name);
    printf("  Tensors:        %u\n", map->header.n_tensors);
    printf("  Total blocks:   %u\n", map->header.total_blocks);
    printf("  Total data:     %lu bytes (%.1f MB)\n",
           (unsigned long)map->header.total_data_size,
           map->header.total_data_size / 1024.0 / 1024.0);
    printf("---------------------------------------------------------------\n");

    /* Group by prefix */
    uint32_t n_embed = 0, n_attn = 0, n_ffn = 0, n_output = 0, n_other = 0;
    uint64_t sz_embed = 0, sz_attn = 0, sz_ffn = 0, sz_output = 0, sz_other = 0;

    for (uint32_t i = 0; i < map->header.n_tensors; i++) {
        const GeoTensorEntry *e = &map->tensors[i];
        if (strstr(e->name, "token_embd")) { n_embed++; sz_embed += e->data_size; }
        else if (strstr(e->name, "output")) { n_output++; sz_output += e->data_size; }
        else if (strstr(e->name, "attn")) { n_attn++; sz_attn += e->data_size; }
        else if (strstr(e->name, "ffn")) { n_ffn++; sz_ffn += e->data_size; }
        else { n_other++; sz_other += e->data_size; }
    }

    printf("  By type:\n");
    printf("    embedding:  %2u tensors, %8.1f MB\n", n_embed, sz_embed/1024.0/1024.0);
    printf("    attention:  %2u tensors, %8.1f MB\n", n_attn, sz_attn/1024.0/1024.0);
    printf("    ffn:        %2u tensors, %8.1f MB\n", n_ffn, sz_ffn/1024.0/1024.0);
    printf("    output:     %2u tensors, %8.1f MB\n", n_output, sz_output/1024.0/1024.0);
    printf("    other:      %2u tensors, %8.1f MB\n", n_other, sz_other/1024.0/1024.0);
    printf("===============================================================\n");
}

#endif /* GEO_TENSOR_MAP_H */
