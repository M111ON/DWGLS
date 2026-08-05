/* ═══════════════════════════════════════════════════════════════════════════
 * geo_sid_loader.h — GEO-Aware Tensor Loader
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PURPOSE:
 *   Extend sid_loader to support GEO as tensor data source.
 *   Same interface as sid_loader_load(), but reads from FrustumBlocks
 *   instead of raw GGUF offsets.
 *
 * PIPELINE:
 *   tensor name → geo_tensor_map_find() → GEO block range → fread FrustumBlock
 *   → return raw tensor bytes (caller feeds to llama_model)
 *
 * USAGE:
 *   // Option A: Use GEO source
 *   GeoSidLoaderCtx gctx;
 *   geo_sid_open_gguf(&gctx, gguf_path, &cache);  // read GGUF for metadata
 *   geo_sid_open_geo(&gctx, geo_path);             // attach GEO data source
 *   geo_sid_load(&gctx, "blk.0.attn_q.weight", buf, &src, &sz);
 *
 *   // Option B: Use GGUF source (fallback, same as sid_loader)
 *   geo_sid_open_gguf(&gctx, gguf_path, &cache);
 *   geo_sid_load(&gctx, "blk.0.attn_q.weight", buf, &src, &sz);
 *
 * DEPENDS: geo_tensor_map.h, sid_loader.h (from runner/), gguf_index.h
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_SID_LOADER_H
#define GEO_SID_LOADER_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geo_tensor_map.h"

/* Include the existing SID loader for cache + GGUF support */
#include "../../FGLS_new/runner/sid_loader.h"

/* ═══════════════════════════════════════════════════════════════
   GEO SOURCE TYPES
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    GEO_SID_SRC_GGUF = 0,   /* read from GGUF file (default) */
    GEO_SID_SRC_GEO  = 1,   /* read from GEO file (FrustumBlocks) */
} GeoSidSource;

/* ═══════════════════════════════════════════════════════════════
   GEO SID LOADER CONTEXT
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    /* Base SID loader (GGUF metadata + cache) */
    SIDLoaderCtx   sid;

    /* GEO-specific */
    GeoSidSource   source;       /* current data source */
    FILE          *geo_file;     /* GEO file handle (NULL if not attached) */
    GeoTensorMap   tensor_map;   /* tensor name → GEO block mapping */

    /* Stats */
    uint64_t       geo_hits;     /* cache hits via GEO path */
    uint64_t       geo_reads;    /* reads from GEO file */
    uint64_t       gguf_reads;   /* reads from GGUF file (fallback) */
} GeoSidLoaderCtx;

/* ═══════════════════════════════════════════════════════════════
   INIT / CLOSE
   ═══════════════════════════════════════════════════════════════ */

/* Open GGUF for metadata + cache */
static inline int geo_sid_open_gguf(GeoSidLoaderCtx *ctx, const char *gguf_path,
                                     SIDCache *cache)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->source = GEO_SID_SRC_GGUF;

    if (sid_loader_open(&ctx->sid, gguf_path, cache) != 0) {
        return -1;
    }

    /* Build tensor map directly from GGUFTensorIndex (no extra gguf_reader needed) */
    geo_tensor_map_init(&ctx->tensor_map);
    {
        uint32_t current_block = 0;
        GGUFTensorIndex *idx = &ctx->sid.idx;
        strncpy(ctx->tensor_map.header.model_name, gguf_path, 63);
        /* Extract just filename */
        const char *slash = strrchr(gguf_path, '/');
        if (!slash) slash = strrchr(gguf_path, '\\');
        if (slash) strncpy(ctx->tensor_map.header.model_name, slash + 1, 63);

        for (uint64_t i = 0; i < idx->n_tensors; i++) {
            uint32_t data_size = (uint32_t)idx->sizes[i];
            uint32_t n_geo_blocks = (data_size + GEO_FBLOCK_SZ - 1) / GEO_FBLOCK_SZ;
            if (n_geo_blocks == 0) n_geo_blocks = 1;

            if (ctx->tensor_map.header.n_tensors < GEO_MAX_TENSORS) {
                GeoTensorEntry *e = &ctx->tensor_map.tensors[ctx->tensor_map.header.n_tensors];
                memset(e, 0, sizeof(*e));
                strncpy(e->name, idx->names[i], GEO_TENSOR_NAME_MAX - 1);
                e->dtype = idx->dtypes[i];
                e->data_size = data_size;
                e->geo_block_start = current_block;
                e->geo_block_count = n_geo_blocks;
                e->geo_data_offset = (uint64_t)current_block * GEO_FBLOCK_SZ;
                ctx->tensor_map.header.n_tensors++;
                ctx->tensor_map.header.total_blocks += n_geo_blocks;
                ctx->tensor_map.header.total_data_size += data_size;
                current_block += n_geo_blocks;
            }
        }
    }

    return 0;
}

/* Attach GEO file as data source */
static inline int geo_sid_open_geo(GeoSidLoaderCtx *ctx, const char *geo_path) {
    ctx->geo_file = fopen(geo_path, "rb");
    if (!ctx->geo_file) {
        fprintf(stderr, "geo_sid: failed to open GEO: %s\n", geo_path);
        return -1;
    }

    /* Verify GEO magic */
    char magic[4];
    if (fread(magic, 1, 4, ctx->geo_file) != 4 || memcmp(magic, "GEOF", 4) != 0) {
        fprintf(stderr, "geo_sid: not a GEO file: %s\n", geo_path);
        fclose(ctx->geo_file);
        ctx->geo_file = NULL;
        return -1;
    }
    fseek(ctx->geo_file, 0, SEEK_SET);

    ctx->source = GEO_SID_SRC_GEO;
    fprintf(stderr, "geo_sid: GEO source attached: %s\n", geo_path);
    return 0;
}

/* Close everything */
static inline void geo_sid_close(GeoSidLoaderCtx *ctx) {
    if (ctx->geo_file) {
        fclose(ctx->geo_file);
        ctx->geo_file = NULL;
    }
    sid_loader_close(&ctx->sid);
    memset(ctx, 0, sizeof(*ctx));
}

/* ═══════════════════════════════════════════════════════════════
   LOAD TENSOR — unified interface
   ═══════════════════════════════════════════════════════════════ */

/*
 * Load tensor data by name.
 * - If source = GEO: read from FrustumBlock via tensor map
 * - If source = GGUF: read from GGUF file (fallback)
 * - Cache is always checked first (same as sid_loader)
 *
 * Returns 0 on success, sets *data and *size.
 */
static inline int geo_sid_load(GeoSidLoaderCtx *ctx, const char *name,
                                uint8_t *read_buf,
                                uint8_t **data, size_t *size)
{
    /* Step 1: Check cache (same as sid_loader) */
    uint8_t *cached; size_t cached_sz;
    if (sid_cache_get(ctx->sid.cache, name, &cached, &cached_sz) == 0) {
        *data = cached;
        *size = cached_sz;
        ctx->sid.cache_hits++;
        return 0;
    }

    /* Step 2: Load from current source */
    if (ctx->source == GEO_SID_SRC_GEO && ctx->geo_file) {
        /* === GEO PATH === */
        const GeoTensorEntry *tensor = geo_tensor_map_find(&ctx->tensor_map, name);
        if (!tensor) {
            /* Tensor not in map — fallback to GGUF */
            goto fallback_gguf;
        }

        /* Seek to tensor's block range in GEO file (skip 32B header) */
        uint64_t offset = 32 + (uint64_t)tensor->geo_block_start * GEO_FBLOCK_SZ;
        uint64_t to_read = (uint64_t)tensor->geo_block_count * GEO_FBLOCK_SZ;

        fseek(ctx->geo_file, (long)offset, SEEK_SET);
        if (fread(read_buf, 1, (size_t)to_read, ctx->geo_file) != to_read) {
            fprintf(stderr, "geo_sid: GEO read failed for %s\n", name);
            goto fallback_gguf;
        }

        /* Store in cache if not norm */
        if (!sid_loader_is_norm(name)) {
            uint16_t tring = 0;
            sid_cache_put(ctx->sid.cache, name, tring, read_buf, (size_t)to_read);
        }

        *data = read_buf;
        *size = (size_t)tensor->data_size;  /* actual tensor size, not padded */
        ctx->geo_reads++;
        return 0;

    fallback_gguf:
        /* Fall through to GGUF path */
        ;
    }

    /* === GGUF PATH (default/fallback) === */
    int64_t ti = sid_loader_find(&ctx->sid, name);
    if (ti < 0) return -1;

    uint64_t sz = ctx->sid.idx.sizes[ti];
    uint64_t off = ctx->sid.idx.offsets[ti];
    fseek(ctx->sid.gguf_file, off, SEEK_SET);
    if (fread(read_buf, 1, sz, ctx->sid.gguf_file) != sz) return -1;

    ctx->sid.bytes_read += sz;
    ctx->sid.file_hits++;

    if (!sid_loader_is_norm(name)) {
        uint16_t tring = 0;
        sid_cache_put(ctx->sid.cache, name, tring, read_buf, (size_t)sz);
    }

    *data = read_buf;
    *size = (size_t)sz;
    ctx->gguf_reads++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   STATS
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_sid_stats(const GeoSidLoaderCtx *ctx) {
    printf("===============================================================\n");
    printf("  GEO SID Loader Stats\n");
    printf("---------------------------------------------------------------\n");
    printf("  Source:         %s\n", ctx->source == GEO_SID_SRC_GEO ? "GEO" : "GGUF");
    printf("  GGUF tensors:   %llu\n", (unsigned long long)ctx->sid.n_tensors);
    printf("  GEO mapped:     %u\n", ctx->tensor_map.header.n_tensors);
    printf("---------------------------------------------------------------\n");
    printf("  Cache hits:     %llu\n", (unsigned long long)ctx->sid.cache_hits);
    printf("  GGUF reads:     %llu\n", (unsigned long long)ctx->gguf_reads);
    printf("  GEO reads:      %llu\n", (unsigned long long)ctx->geo_reads);
    printf("  Total bytes:    %llu\n", (unsigned long long)ctx->sid.bytes_read);
    printf("===============================================================\n");
}

#endif /* GEO_SID_LOADER_H */
