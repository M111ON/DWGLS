#ifndef GEAR2_H
#define GEAR2_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * Gear 2 — Pinned Memory Mirror
 *
 * Instead of per-tensor ggml_backend_tensor_set() (251× context switch +
 * driver staging copy), create a single pinned host buffer whose layout
 * mirrors the GPU backend buffer exactly.
 *
 *   SID face data  →  memcpy (CPU)  →  Pinned Mirror  →  cudaMemcpyAsync (×1 DMA)  →  GPU Buffer
 *
 * Offsets are identical between mirror and GPU because ggml allocates
 * tensors contiguously within a backend buffer (alignment gaps only).
 *
 * This is the foundation for Gear 3 (async pipeline), Gear 4 (spatial SID),
 * and Gear 5 (predictive prefetch).
 */

#define GEAR2_MAX_TENSORS 512

/* Per-tensor mapping: offset within both mirror and GPU buffer */
typedef struct {
    uint32_t offset;          /* byte offset from buffer base */
    uint32_t size;            /* tensor byte size */
    int      ft_idx;          /* index into found_tensors[] */
} Gear2TensorMap;

/* Memory region: one GPU buffer = one pinned mirror */
typedef struct {
    void            *gpu_base;     /* ggml_backend_buffer_get_base() */
    void            *pinned_base;  /* cudaMallocHost() mirror */
    size_t           total_size;   /* buffer size */
    int              n_tensors;    /* tensors in this region */
    Gear2TensorMap   map[GEAR2_MAX_TENSORS];
} Gear2Region;

/* Gear 2 context */
typedef struct {
    Gear2Region  *regions;         /* array of regions (one per GPU backend buffer) */
    int           n_regions;       /* number of regions */
    int           enabled;         /* gear2 active */
    int           use_ibridge;     /* icosa bridge available */
    void         *gpu_ctx;         /* icosa bridge context */
    IcosaBridge  *bridge;          /* function table (checks for NULL batch/pin) */

    /* Temporary buffers for batch memcpy API (reused across calls) */
    void        **batch_dst;
    void        **batch_src;
    size_t       *batch_sizes;
    int           batch_cap;

    /* Stats */
    int           n_gpu_tensors;   /* total GPU tensors tracked */
    uint64_t      bytes_copied;    /* total bytes transferred */
    int           n_transfers;     /* total number of DMA transfers */
    int           layout_contiguous; /* verified contiguous status */
} Gear2Ctx;

/* Forward declaration for early-error cleanup */
static inline void gear2_destroy(Gear2Ctx *ctx);

/* ── Init: scan GPU-offloaded tensors, allocate pinned mirrors ── */
static inline int gear2_init(Gear2Ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    if (!g_opt_twin_gpu || !g_ibridge_ctx) {
        fprintf(stderr, "[gear2] no icosa bridge — disabled\n");
        return -1;
    }

    ctx->gpu_ctx = g_ibridge_ctx;
    ctx->bridge  = &g_ibridge;

    int has_batch = (g_ibridge.batch_memcpy_h2d && g_ibridge.batch_memcpy_d2h);
    int has_pin   = (g_ibridge.pin_host && g_ibridge.unpin_host);
    if (!has_batch || !has_pin) {
        fprintf(stderr, "[gear2] bridge missing batch/pin functions — disabled\n");
        return -1;
    }
    ctx->use_ibridge = 1;

    /* Count unique GPU backend buffers among SID-swappable tensors */
    /* We use a simple heuristic: collect all GPU tensors, group by buffer ptr */
    struct ggml_backend_buffer *bufs[GEAR2_MAX_TENSORS];
    int n_bufs = 0;

    for (int s = 0; s < n_sid_swaps; s++) {
        int fi = sid_swaps[s].ft_idx;
        struct ggml_tensor *t = found_tensors[fi].ptr;
        if (!t || !t->buffer) continue;
        if (!ggml_backend_buffer_is_host(t->buffer)) {
            /* Check if we already have this buffer */
            int found = 0;
            for (int b = 0; b < n_bufs; b++) {
                if (bufs[b] == t->buffer) { found = 1; break; }
            }
            if (!found && n_bufs < GEAR2_MAX_TENSORS) {
                bufs[n_bufs++] = t->buffer;
            }
        }
    }

    if (n_bufs == 0) {
        fprintf(stderr, "[gear2] no GPU-offloaded tensors found\n");
        return -1;
    }

    ctx->n_regions = n_bufs;
    ctx->regions = (Gear2Region*)calloc((size_t)n_bufs, sizeof(Gear2Region));
    if (!ctx->regions) {
        fprintf(stderr, "[gear2] region alloc failed\n");
        return -1;
    }

    /* For each buffer, populate tensor map and allocate pinned mirror */
    RegionInitLoop:
    for (int b = 0; b < n_bufs; b++) {
        Gear2Region *reg = &ctx->regions[b];
        reg->gpu_base   = ggml_backend_buffer_get_base(bufs[b]);
        reg->total_size = ggml_backend_buffer_get_size(bufs[b]);

        /* Collect all SID-swappable tensors in this buffer, sorted by offset */
        int nt = 0;
        for (int s = 0; s < n_sid_swaps; s++) {
            int fi = sid_swaps[s].ft_idx;
            struct ggml_tensor *t = found_tensors[fi].ptr;
            if (!t || t->buffer != bufs[b]) continue;
            if (nt >= GEAR2_MAX_TENSORS) break;
            reg->map[nt].offset = (uint32_t)((uint8_t*)t->data - (uint8_t*)reg->gpu_base);
            reg->map[nt].size   = (uint32_t)found_tensors[fi].nbytes;
            reg->map[nt].ft_idx = fi;
            nt++;
        }

        if (nt == 0) continue;

        /* Sort by offset (bubble sort — small N, one-time cost) */
        for (int i = 0; i < nt - 1; i++) {
            for (int j = 0; j < nt - 1 - i; j++) {
                if (reg->map[j].offset > reg->map[j+1].offset) {
                    Gear2TensorMap tmp = reg->map[j];
                    reg->map[j] = reg->map[j+1];
                    reg->map[j+1] = tmp;
                }
            }
        }

        /* Verify contiguous layout */
        reg->n_tensors = nt;
        uint32_t expected_next = 0;
        int contiguous = 1;
        size_t total_gap = 0;
        for (int i = 0; i < nt; i++) {
            if (reg->map[i].offset > expected_next + 4096) {
                /* Gap > 4KB — significant hole */
                contiguous = 0;
                total_gap += (size_t)(reg->map[i].offset - expected_next);
            }
            expected_next = reg->map[i].offset + reg->map[i].size;
        }

        /* Allocate pinned host mirror */
        int pin_ok = 0;
        if (reg->total_size > 0) {
            pin_ok = (g_ibridge.pin_host(g_ibridge_ctx, &reg->pinned_base, reg->total_size) == 0);
        }

        fprintf(stderr, "[gear2] region %d: %d tensors, %zu MB GPU, pinned=%d contiguous=%d",
                b, nt, reg->total_size >> 20, pin_ok, contiguous);
        if (!contiguous)
            fprintf(stderr, " gap=%zu KB", total_gap >> 10);
        fprintf(stderr, "\n");

        if (pin_ok) {
            ctx->n_gpu_tensors += nt;
            if (contiguous) ctx->layout_contiguous = 1;
        }
    }

    /* Allocate batch arrays */
    ctx->batch_cap = ctx->n_gpu_tensors > 0 ? ctx->n_gpu_tensors : 64;
    ctx->batch_dst   = (void**)calloc((size_t)ctx->batch_cap, sizeof(void*));
    ctx->batch_src   = (void**)calloc((size_t)ctx->batch_cap, sizeof(void*));
    ctx->batch_sizes = (size_t*)calloc((size_t)ctx->batch_cap, sizeof(size_t));

    if (!ctx->batch_dst || !ctx->batch_src || !ctx->batch_sizes) {
        fprintf(stderr, "[gear2] batch array alloc failed\n");
        gear2_destroy(ctx);
        return -1;
    }

    if (ctx->n_gpu_tensors > 0) {
        ctx->enabled = 1;
        fprintf(stderr, "[gear2] init OK: %d GPU tensors across %d regions\n",
                ctx->n_gpu_tensors, ctx->n_regions);
    }

    return ctx->enabled ? 0 : -1;
}

/* ── Apply: copy face data → pinned mirror → single H2D DMA ── */
static inline void gear2_apply(Gear2Ctx *ctx,
    int n_apply, int *apply_ft_idx, void **apply_sid, size_t *apply_sz)
{
    (void)n_apply; (void)apply_ft_idx; (void)apply_sid; (void)apply_sz;
    if (!ctx->enabled) return;

    /* Phase 1: memcpy face data → pinned mirror at GPU-matching offsets */
    for (int r = 0; r < ctx->n_regions; r++) {
        Gear2Region *reg = &ctx->regions[r];
        if (!reg->pinned_base) continue;
        for (int i = 0; i < reg->n_tensors; i++) {
            int fi = reg->map[i].ft_idx;
            /* Find apply_sid for this tensor */
            for (int a = 0; a < n_apply; a++) {
                if (apply_ft_idx[a] == fi) {
                    memcpy((uint8_t*)reg->pinned_base + reg->map[i].offset,
                           apply_sid[a], reg->map[i].size);
                    break;
                }
            }
        }
    }

    /* Phase 2: single cudaMemcpyAsync per region */
    for (int r = 0; r < ctx->n_regions; r++) {
        Gear2Region *reg = &ctx->regions[r];
        if (!reg->pinned_base || reg->total_size == 0) continue;
        ctx->bridge->memcpy_h2d(ctx->gpu_ctx, reg->gpu_base,
                                 reg->pinned_base, reg->total_size);
        ctx->bytes_copied += reg->total_size;
        ctx->n_transfers++;
    }
}

/* ── Restore: copy original data → pinned mirror → single H2D DMA ── */
static inline void gear2_restore(Gear2Ctx *ctx,
    int n_restore, int *restore_ft_idx, void **restore_data)
{
    (void)n_restore; (void)restore_ft_idx; (void)restore_data;
    if (!ctx->enabled) return;

    /* Phase 1: memcpy original data → pinned mirror */
    for (int r = 0; r < ctx->n_regions; r++) {
        Gear2Region *reg = &ctx->regions[r];
        if (!reg->pinned_base) continue;
        for (int i = 0; i < reg->n_tensors; i++) {
            int fi = reg->map[i].ft_idx;
            for (int a = 0; a < n_restore; a++) {
                if (restore_ft_idx[a] == fi) {
                    memcpy((uint8_t*)reg->pinned_base + reg->map[i].offset,
                           restore_data[a], reg->map[i].size);
                    break;
                }
            }
        }
    }

    /* Phase 2: single cudaMemcpyAsync per region */
    for (int r = 0; r < ctx->n_regions; r++) {
        Gear2Region *reg = &ctx->regions[r];
        if (!reg->pinned_base || reg->total_size == 0) continue;
        ctx->bridge->memcpy_h2d(ctx->gpu_ctx, reg->gpu_base,
                                 reg->pinned_base, reg->total_size);
        ctx->bytes_copied += reg->total_size;
        ctx->n_transfers++;
    }
}

/* ── Destroy: free pinned mirrors ── */
static inline void gear2_destroy(Gear2Ctx *ctx) {
    if (ctx->regions) {
        for (int r = 0; r < ctx->n_regions; r++) {
            if (ctx->regions[r].pinned_base && ctx->bridge && ctx->bridge->unpin_host) {
                ctx->bridge->unpin_host(ctx->gpu_ctx, ctx->regions[r].pinned_base);
            }
        }
        free(ctx->regions);
        ctx->regions = NULL;
    }
    free(ctx->batch_dst);
    free(ctx->batch_src);
    free(ctx->batch_sizes);
    ctx->batch_dst = NULL;
    ctx->batch_src = NULL;
    ctx->batch_sizes = NULL;
    if (ctx->n_transfers > 0) {
        fprintf(stderr, "[gear2] stats: %llu bytes in %d transfers\n",
                (unsigned long long)ctx->bytes_copied, ctx->n_transfers);
    }
    memset(ctx, 0, sizeof(*ctx));
}

#endif /* GEAR2_H */
