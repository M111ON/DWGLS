/* ═══════════════════════════════════════════════════════════════════════════
 * geo_tensor_hub.h — Geometry Tensor Hub
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Sits between llama.cpp and geometry storage.
 *
 *   llama.cpp calls:  hub_load("blk.0.attn_q.weight", &buf, &n_elems, &dtype)
 *   hub internals:    name → geotype → cell classify → gcube read → return
 *
 * ARCHITECTURE:
 *
 *   llama.cpp
 *       │  geo_tensor_hub_load(tensor_name, &data, &n, &dtype)
 *       │
 *   ┌───▼──────────────────────────────────────────┐
 *   │  GEO TENSOR HUB                              │
 *   │                                              │
 *   │  1. gguf_index → tensor name → offset, size   │
 *   │  2. geo_tensor_map → FrustumBlock range        │
 *   │  3. geo_cell_classify → cell types per (g,f,s)  │
 *   │  4. geo_cell_prune  → which types to load      │
 *   │  5. geo_cube_container → read DiamondBlocks     │
 *   │                                              │
 *   │  return weights[]                             │
 *   └──────────────────────────────────────────────┘
 *       │                   │
 *   .gcube              GGUF (metadata only)
 *
 * HUB IS THIN:  no allocation, no caching, no threads.
 * Just call get→route→read→return.  O(1) per tensor.
 *
 * DEPENDS:
 *   gguf_index.h         (FGLS: parse GGUF metadata)
 * lpipeline.h       (DWGLS: tensor→FrustumBlock mapping)
 *   geo_cell_prune.h     (DWGLS: cell-type classification)
 *   geo_cube_container.h (DWGLS: DiamondBlock read)
 *   geo_frame_seek.h     (DWGLS: O(1) seek)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_TENSOR_HUB_H
#define GEO_TENSOR_HUB_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geo_cube_container.h"
#include "gguf_index.h"

/* ═══════════════════════════════════════════════════════════
   HUB CONTEXT — state for one model
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    const char       *gguf_path;   /* GGUF for metadata (names, offsets) */
    const char       *gcube_path;  /* .gcube for actual tensor data */
    GGUFTensorIndex   idx;         /* GGUF tensor index */
    GCubeContainer   *cube;         /* loaded .gcube if available */
    int               is_open;
} GeoTensorHub;

/* ═══════════════════════════════════════════════════════════
   HUB OPEN — load GGUF metadata + index .gcube
   ═══════════════════════════════════════════════════════════ */

static inline int geo_hub_open(GeoTensorHub *hub,
                                const char *gguf_path,
                                const char *gcube_path)
{
    memset(hub, 0, sizeof(*hub));
    hub->gguf_path = gguf_path;
    hub->gcube_path = gcube_path;

    /* Load tensor index from GGUF (metadata only — no weights) */
    if (gguf_idx_open(gguf_path, &hub->idx) != 0) {
        fprintf(stderr, "[HUB] Cannot open GGUF index: %s\n", gguf_path);
        return -1;
    }

    /* Load .gcube with actual weight data */
    hub->cube = (GCubeContainer *)malloc(sizeof(GCubeContainer));
    if (!hub->cube) { gguf_idx_close(&hub->idx); return -1; }

    if (gcube_read(hub->cube, gcube_path) != 0) {
        fprintf(stderr, "[HUB] Cannot open .gcube: %s\n", gcube_path);
        free(hub->cube);
        gguf_idx_close(&hub->idx);
        return -1;
    }

    hub->is_open = 1;
    printf("[HUB] Open: %u tensors, %u blocks\n",
           hub->cube->header.n_tensors,
           hub->cube->header.total_blocks);

    return 0;
}

/* ═══════════════════════════════════════════════════════════
   HUB LOAD — signal from llama.cpp: "give me weights for this tensor"
   ═══════════════════════════════════════════════════════════ */

static inline int geo_hub_load(GeoTensorHub *hub,
                                const char *tensor_name,
                                uint8_t **data_out,
                                uint32_t *n_elems_out,
                                uint32_t *dtype_out)
{
    if (!hub->is_open || !hub->cube) return -1;

    /* 1. Find tensor in GGUF index */
    int64_t idx = -1;
    for (uint64_t i = 0; i < hub->idx.n_tensors; i++) {
        if (strcmp(hub->idx.names[i], tensor_name) == 0) {
            idx = (int64_t)i;
            break;
        }
    }
    if (idx < 0) {
        fprintf(stderr, "[HUB] tensor not found: %s\n", tensor_name);
        return -1;
    }

    uint32_t dtype = hub->idx.dtypes[idx];
    /* uint64_t size = hub->idx.sizes[idx]; */ /* metadata only */

    /* 2. Find in .gcube by name */
    const GCubeTensorEntry *ge = gcube_find(hub->cube, tensor_name);
    if (!ge) {
        fprintf(stderr, "[HUB] tensor not in .gcube: %s\n", tensor_name);
        return -1;
    }

    /* 3. Read block data */
    const uint8_t *block_data = gcube_tensor_data(hub->cube, ge);
    if (!block_data) return -1;

    uint32_t n_elems = ge->n_elems;
    uint32_t data_sz = ge->data_size;

    /* 4. Allocate output */
    uint8_t *out = (uint8_t *)malloc(data_sz);
    if (!out) return -1;
    memcpy(out, block_data, data_sz);

    *data_out    = out;
    *n_elems_out = n_elems;
    *dtype_out   = dtype;

    /* Debug: one-line log */
    printf("[HUB] ✓ %-40s %u elems/%uB\n", tensor_name, n_elems, data_sz);

    return 0;
}

/* ═══════════════════════════════════════════════════════════
   HUB CLOSE
   ═══════════════════════════════════════════════════════════ */

static inline void geo_hub_close(GeoTensorHub *hub) {
    if (hub->cube) {
gcube_free(hub->cube);
        free(hub->cube);
        hub->cube = NULL;
    }
    gguf_idx_close(&hub->idx);
    hub->is_open = 0;
}

/* ═══════════════════════════════════════════════════════════
   HUB BATCH — load all tensors at once (for pre-fetch)
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    char     name[64];
    uint8_t *data;
    uint32_t n_elems;
    uint32_t dtype;
} GeoTensorBatchEntry;

typedef struct {
    GeoTensorBatchEntry *entries;
    uint32_t              count;
} GeoTensorBatch;

static inline int geo_hub_load_all(GeoTensorHub *hub,
                                    GeoTensorBatch *batch)
{
    if (!hub->is_open || !hub->cube) return -1;

    uint32_t n = hub->cube->header.n_tensors;
    batch -> count = n;
    batch->entries = (GeoTensorBatchEntry *)calloc(n, sizeof(GeoTensorBatchEntry));
    if (!batch->entries) return -1;

    for (uint32_t i = 0; i < n; i++) {
        const GCubeTensorEntry *ge = &hub->cube->tensors[i];

        /* Find in GGUF for dtype */
        uint32_t dtype = 8;  /* default Q8_0 */
        for (uint64_t j = 0; j < hub->idx.n_tensors; j++) {
            if (strcmp(hub->idx.names[j], ge->name) == 0) {
                dtype = hub->idx.dtypes[j];
                break;
            }
        }

        const uint8_t *block_data = gcube_tensor_data(hub->cube, ge);
        uint32_t data_sz = ge->data_size;

        strncpy(batch->entries[i].name, ge->name, 63);
        batch->entries[i].data     = (uint8_t *)malloc(data_sz);
        batch->entries[i].n_elems  = ge->n_elems;
        batch->entries[i].dtype    = dtype;

        if (batch->entries[i].data && block_data) {
            memcpy(batch->entries[i].data, block_data, data_sz);
        }
    }

    printf("[HUB] batch-load: %u tensors\n", n);
    return 0;
}

static inline void geo_hub_batch_free(GeoTensorBatch *batch) {
    if (!batch || !batch->entries) return;
    for (uint32_t i = 0; i < batch->count; i++) {
        if (batch->entries[i].data) free(batch->entries[i].data);
    }
    free(batch->entries);
}

#endif /* GEO_TENSOR_HUB_H */