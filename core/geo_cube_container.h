/* ═══════════════════════════════════════════════════════════════════════════
 * geo_cube_container.h — Cube Container Format (.gcube)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Binary format for storing geometric weight data.
 * Each tensor is stored as a set of DiamondBlocks (64 bytes each),
 * addressed by (generation, face, slot) — the cube address space.
 *
 * LAYOUT:
 *   FileHeader[64B] + TensorIndex[N*80B] + TensorData[variable]
 *
 * FileHeader (64 bytes, packed):
 *   magic          char[4]    "GCB\0"
 *   version        uint8_t    1
 *   flags          uint8_t    reserved
 *   n_tensors      uint16_t   number of tensors
 *   total_blocks   uint32_t   total DiamondBlocks
 *   total_weights  uint32_t   total weight count
 *   model_name[32] char[]     model name (null-padded)
 *   reserved[16]   uint8_t    zeros
 *
 * TensorIndexEntry (80 bytes, packed):
 *   name[48]       char[]     tensor name (null-padded)
 *   n_dims         uint8_t    1-4
 *   dtype          uint8_t    GGML type enum
 *   _pad[2]        uint8_t    zeros
 *   dims[4]        uint32_t   dimension sizes
 *   n_elems        uint32_t   total elements
 *   data_size      uint32_t   raw tensor bytes
 *   block_start    uint32_t   first DiamondBlock index
 *   block_count    uint32_t   number of DiamondBlocks
 *
 * TensorData:
 *   block_count × 64 bytes (DiamondBlock payloads)
 *
 * CRC32 over entire file (excludes this 4-byte CRC itself).
 *
 * DEPENDS: geo_cube_addr.h, geo_cell_classify.h
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_CUBE_CONTAINER_H
#define GEO_CUBE_CONTAINER_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define GCUBE_MAGIC         "GCB\0"
#define GCUBE_VERSION       1
#define GCUBE_FILE_HDR_SZ   64u
#define GCUBE_TENSOR_HDR_SZ 80u
#define GCUBE_BLOCK_SZ      64u     /* DiamondBlock = 64 bytes */
#define GCUBE_MAX_TENSORS   512u
#define GCUBE_MAX_NAME      48u
#define GCUBE_MAX_MODEL     32u

/* ═══════════════════════════════════════════════════════════════
   FILE HEADER (64 bytes)
   ═══════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    char     magic[4];        /* "GCB\0" */
    uint8_t  version;         /* 1 */
    uint8_t  flags;           /* reserved, 0 */
    uint16_t n_tensors;       /* number of tensors */
    uint32_t total_blocks;    /* total DiamondBlocks */
    uint32_t total_weights;   /* total weight count */
    char     model_name[GCUBE_MAX_MODEL]; /* null-padded */
    uint8_t  reserved[16];    /* zeros */
} GCubeFileHeader;
#pragma pack(pop)

/* ═══════════════════════════════════════════════════════════════
   TENSOR INDEX ENTRY (80 bytes)
   ═══════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    char     name[GCUBE_MAX_NAME];  /* tensor name, null-padded */
    uint8_t  n_dims;                /* 1-4 */
    uint8_t  dtype;                 /* GGML type enum */
    uint8_t  _pad[2];               /* alignment */
    uint32_t dims[4];               /* dimension sizes */
    uint32_t n_elems;               /* total elements */
    uint32_t data_size;             /* raw bytes */
    uint32_t block_start;           /* first block index */
    uint32_t block_count;           /* number of blocks */
} GCubeTensorEntry;
#pragma pack(pop)

/* ═══════════════════════════════════════════════════════════════
   IN-MEMORY CONTAINER
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    GCubeFileHeader  header;
    GCubeTensorEntry tensors[GCUBE_MAX_TENSORS];
    uint8_t         *blocks;      /* raw block data (malloc'd) */
    uint32_t         blocks_cap;  /* allocated capacity in bytes */
} GCubeContainer;

/* ═══════════════════════════════════════════════════════════════
   CRC32 (ISO 3309 / ITU-T V.42)
   ═══════════════════════════════════════════════════════════════ */

static inline uint32_t gcube_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ═══════════════════════════════════════════════════════════════
   INIT / FREE
   ═══════════════════════════════════════════════════════════════ */

static inline void gcube_init(GCubeContainer *c) {
    memset(c, 0, sizeof(*c));
    memcpy(c->header.magic, GCUBE_MAGIC, 4);
    c->header.version = GCUBE_VERSION;
}

static inline void gcube_free(GCubeContainer *c) {
    if (c->blocks) { free(c->blocks); c->blocks = NULL; }
    c->blocks_cap = 0;
}

/* ═══════════════════════════════════════════════════════════════
   ADD TENSOR — build in-memory container
   ═══════════════════════════════════════════════════════════════ */

/* Add a tensor with raw weight data. Data is copied into block storage.
 * Returns 0 on success, -1 if full. */
static inline int gcube_add_tensor(GCubeContainer *c,
                                    const char *name,
                                    uint8_t n_dims,
                                    const uint32_t dims[4],
                                    uint8_t dtype,
                                    uint32_t n_elems,
                                    const uint8_t *data,
                                    uint32_t data_size)
{
    if (c->header.n_tensors >= GCUBE_MAX_TENSORS) return -1;

    /* Calculate blocks needed */
    uint32_t n_blocks = (data_size + GCUBE_BLOCK_SZ - 1) / GCUBE_BLOCK_SZ;
    if (n_blocks == 0) n_blocks = 1;

    /* Ensure block storage is large enough */
    uint32_t needed = (c->header.total_blocks + n_blocks) * GCUBE_BLOCK_SZ;
    if (needed > c->blocks_cap) {
        uint32_t new_cap = c->blocks_cap ? c->blocks_cap * 2 : 1024 * 1024;
        while (new_cap < needed) new_cap *= 2;
        uint8_t *new_blocks = (uint8_t *)realloc(c->blocks, new_cap);
        if (!new_blocks) return -1;
        c->blocks = new_blocks;
        c->blocks_cap = new_cap;
    }

    /* Fill tensor entry */
    GCubeTensorEntry *e = &c->tensors[c->header.n_tensors];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, GCUBE_MAX_NAME - 1);
    e->n_dims = n_dims;
    e->dtype = dtype;
    for (int i = 0; i < n_dims && i < 4; i++) e->dims[i] = dims[i];
    e->n_elems = n_elems;
    e->data_size = data_size;
    e->block_start = c->header.total_blocks;
    e->block_count = n_blocks;

    /* Copy data into blocks (zero-padded to block boundary) */
    uint8_t *dst = c->blocks + e->block_start * GCUBE_BLOCK_SZ;
    memcpy(dst, data, data_size);
    /* Zero remaining bytes in last block */
    uint32_t valid = data_size % GCUBE_BLOCK_SZ;
    if (valid > 0) {
        memset(dst + data_size, 0, GCUBE_BLOCK_SZ - valid);
    }

    c->header.n_tensors++;
    c->header.total_blocks += n_blocks;
    c->header.total_weights += n_elems;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   SERIALIZE — write to file
   ═══════════════════════════════════════════════════════════════ */

static inline int gcube_write(const GCubeContainer *c, const char *path) {
    /* Compute sizes */
    uint32_t idx_bytes = (uint32_t)c->header.n_tensors * GCUBE_TENSOR_HDR_SZ;
    uint32_t blk_bytes = c->header.total_blocks * GCUBE_BLOCK_SZ;
    uint32_t data_bytes = GCUBE_FILE_HDR_SZ + idx_bytes + blk_bytes;

    /* Allocate buffer for everything except CRC */
    uint8_t *buf = (uint8_t *)malloc(data_bytes);
    if (!buf) return -1;

    /* Pack header */
    memcpy(buf, &c->header, GCUBE_FILE_HDR_SZ);

    /* Pack tensor index */
    uint32_t off = GCUBE_FILE_HDR_SZ;
    for (uint16_t i = 0; i < c->header.n_tensors; i++) {
        memcpy(buf + off, &c->tensors[i], GCUBE_TENSOR_HDR_SZ);
        off += GCUBE_TENSOR_HDR_SZ;
    }

    /* Pack block data */
    if (blk_bytes > 0 && c->blocks) {
        memcpy(buf + off, c->blocks, blk_bytes);
    }

    /* Compute CRC over data */
    uint32_t crc = gcube_crc32(buf, data_bytes);

    /* Write data + CRC */
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, data_bytes, f);
    fwrite(&crc, 4, 1, f);
    fclose(f);
    free(buf);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   DESERIALIZE — read from file
   ═══════════════════════════════════════════════════════════════ */

static inline int gcube_read(GCubeContainer *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    gcube_init(c);

    /* Read file header */
    if (fread(&c->header, GCUBE_FILE_HDR_SZ, 1, f) != 1) { fclose(f); return -2; }
    if (memcmp(c->header.magic, GCUBE_MAGIC, 4) != 0) { fclose(f); return -3; }
    if (c->header.version != GCUBE_VERSION) { fclose(f); return -4; }
    if (c->header.n_tensors > GCUBE_MAX_TENSORS) { fclose(f); return -5; }

    /* Read tensor index */
    for (uint16_t i = 0; i < c->header.n_tensors; i++) {
        if (fread(&c->tensors[i], GCUBE_TENSOR_HDR_SZ, 1, f) != 1) {
            fclose(f); return -6;
        }
    }

    /* Read block data */
    uint32_t total_block_bytes = c->header.total_blocks * GCUBE_BLOCK_SZ;
    if (total_block_bytes > 0) {
        c->blocks = (uint8_t *)malloc(total_block_bytes);
        c->blocks_cap = total_block_bytes;
        if (!c->blocks) { fclose(f); return -7; }
        if (fread(c->blocks, 1, total_block_bytes, f) != total_block_bytes) {
            fclose(f); return -8;
        }
    }

    /* Verify CRC32 — compute from in-memory data */
    uint32_t idx_bytes = (uint32_t)c->header.n_tensors * GCUBE_TENSOR_HDR_SZ;
    uint32_t data_bytes = GCUBE_FILE_HDR_SZ + idx_bytes + total_block_bytes;

    uint32_t stored_crc = 0;
    if (fread(&stored_crc, 4, 1, f) != 1) { fclose(f); return -9; }

    /* Reconstruct data buffer for CRC check */
    uint8_t *crc_buf = (uint8_t *)malloc(data_bytes);
    if (crc_buf) {
        /* Pack header + index + blocks in same order as gcube_write */
        uint32_t off = 0;
        /* We already have header in c->header, re-pack it */
        memcpy(crc_buf + off, &c->header, GCUBE_FILE_HDR_SZ);
        off += GCUBE_FILE_HDR_SZ;
        for (uint16_t i = 0; i < c->header.n_tensors; i++) {
            memcpy(crc_buf + off, &c->tensors[i], GCUBE_TENSOR_HDR_SZ);
            off += GCUBE_TENSOR_HDR_SZ;
        }
        if (total_block_bytes > 0 && c->blocks) {
            memcpy(crc_buf + off, c->blocks, total_block_bytes);
        }
        uint32_t computed_crc = gcube_crc32(crc_buf, data_bytes);
        free(crc_buf);
        if (computed_crc != stored_crc) { fclose(f); return -10; }
    }

    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   FIND TENSOR by name
   ═══════════════════════════════════════════════════════════════ */

static inline const GCubeTensorEntry* gcube_find(const GCubeContainer *c,
                                                  const char *name)
{
    for (uint16_t i = 0; i < c->header.n_tensors; i++) {
        if (strcmp(c->tensors[i].name, name) == 0)
            return &c->tensors[i];
    }
    return NULL;
}

/* Get pointer to tensor's block data */
static inline const uint8_t* gcube_tensor_data(const GCubeContainer *c,
                                                const GCubeTensorEntry *e)
{
    if (!c->blocks || !e) return NULL;
    return c->blocks + e->block_start * GCUBE_BLOCK_SZ;
}

/* ═══════════════════════════════════════════════════════════════
   VERIFY — read back and compare
   ═══════════════════════════════════════════════════════════════ */

static inline int gcube_verify(const GCubeContainer *original,
                                const char *path)
{
    GCubeContainer loaded;
    gcube_init(&loaded);

    int rc = gcube_read(&loaded, path);
    if (rc != 0) { gcube_free(&loaded); return rc; }

    /* Compare headers */
    int ok = 1;
    if (loaded.header.n_tensors != original->header.n_tensors) ok = 0;
    if (loaded.header.total_blocks != original->header.total_blocks) ok = 0;
    if (loaded.header.total_weights != original->header.total_weights) ok = 0;

    /* Compare tensor entries and data */
    for (uint16_t i = 0; i < original->header.n_tensors && ok; i++) {
        if (memcmp(&loaded.tensors[i], &original->tensors[i], GCUBE_TENSOR_HDR_SZ) != 0) {
            ok = 0;
        }
        uint32_t blk_bytes = original->tensors[i].block_count * GCUBE_BLOCK_SZ;
        const uint8_t *orig_data = gcube_tensor_data(original, &original->tensors[i]);
        const uint8_t *load_data = gcube_tensor_data(&loaded, &loaded.tensors[i]);
        if (orig_data && load_data && memcmp(orig_data, load_data, blk_bytes) != 0) {
            ok = 0;
        }
    }

    gcube_free(&loaded);
    return ok ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════
   STATS
   ═══════════════════════════════════════════════════════════════ */

static inline void gcube_stats(const GCubeContainer *c) {
    uint32_t hdr_total = GCUBE_FILE_HDR_SZ +
                         (uint32_t)c->header.n_tensors * GCUBE_TENSOR_HDR_SZ +
                         c->header.total_blocks * GCUBE_BLOCK_SZ + 4; /* +4 CRC */

    printf("===============================================================\n");
    printf("  GCube Container\n");
    printf("---------------------------------------------------------------\n");
    printf("  Model:          %s\n", c->header.model_name);
    printf("  Tensors:        %u\n", c->header.n_tensors);
    printf("  Blocks:         %u (× %u bytes = %u KB)\n",
           c->header.total_blocks, GCUBE_BLOCK_SZ,
           c->header.total_blocks * GCUBE_BLOCK_SZ / 1024);
    printf("  Weights:        %u\n", c->header.total_weights);
    printf("  File size:      %u bytes (%.1f KB)\n",
           hdr_total, hdr_total / 1024.0);
    printf("  Overhead:       %.1f%% (headers + alignment)\n",
           c->header.total_weights > 0 ?
           100.0 * (hdr_total - c->header.total_weights) / hdr_total : 0.0);
    printf("---------------------------------------------------------------\n");
    for (uint16_t i = 0; i < c->header.n_tensors; i++) {
        const GCubeTensorEntry *e = &c->tensors[i];
        printf("  [%2u] %-30s %ux%u %u blocks\n",
               i, e->name, e->n_elems, e->dtype, e->block_count);
    }
    printf("===============================================================\n");
}

#endif /* GEO_CUBE_CONTAINER_H */
