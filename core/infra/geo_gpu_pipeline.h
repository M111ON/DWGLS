/*
 * geo_gpu_pipeline.h — GPU Pipeline Orchestrator: DRamTile + GearLock + JetBridge
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * CPU-side orchestrator for the geometric GPU pull pipeline.
 * Builds the PointIndex that any CUDA kernel consumes.
 *
 * Pipeline flow:
 *   .gcube mmap → tensor blocks → DRamTile address → PointIndex → GPU kernel
 *
 * Architecture:
 *   1. RailHub opens .gcube (mmap, zero-copy)
 *   2. For each tensor block: compute DRamTile address (anchor + hilbert 8x8)
 *   3. Build PointIndex — flat array of (slot_id, dram_offset, chunk_size, checksum)
 *   4. GPU kernel reads chunks from DRamTile at computed offsets
 *   5. GearLock syncs CPU/GPU world counters after each bridge
 *
 * Sacred: 20736 = 162×128 (DRamTile) = 1728×12 (Spine) = 128×162 (Gear)
 *
 * DESIGN:
 *   No malloc on hot path. All static inline. Header-only.
 *   The PointIndex is stack-allocated for small tensors, heap-allocated for
 *   batch pulls (caller provides buffer).
 *
 * DEPENDS:
 *   core/infra/geo_dram_tile.h  — DRamTile addressing
 *   core/infra/fibo_spine.h     — FiboSpine pipe ceremony
 *   core/infra/gear_lock.h      — CPU/GPU world sync
 *   core/geo_rail_hub.h         — .gcube tensor pull
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_GPU_PIPELINE_H
#define GEO_GPU_PIPELINE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "geo_dram_tile.h"
#include "fibo_spine.h"
#include "gear_lock.h"
#include "jet_select.h"

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define GP_PIPE_TICKS       12u
#define GP_PIPES            1728u       /* 12 × 144                        */
#define GP_CHUNK_SZ         64u         /* bytes per DRamTile chunk          */
#define GP_MAX_PIPELINE_IDX 20736u      /* max point index entries           */

/* ═══════════════════════════════════════════════════════════════════════════
   POINT INDEX — CPU builds, GPU reads
   ═══════════════════════════════════════════════════════════════════════════
   Each entry maps one DRamTile chunk to its GPU-accessible offset.
   The GPU kernel iterates this array, reads each chunk, verifies checksum.
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t slot_id;       /* flat slot: pipe × 12 + tick  (0..20735)      */
    uint32_t dram_addr;     /* DRamTile address (anchor × 128 + hilbert)    */
    uint64_t byte_offset;   /* byte offset = dram_addr × GP_CHUNK_SZ       */
    uint32_t chunk_sz;      /* GP_CHUNK_SZ (64)                             */
    uint32_t ref_checksum;  /* expected XOR of chunk bytes                  */
} GeoPipelineEntry;

typedef struct {
    GeoPipelineEntry entries[GP_MAX_PIPELINE_IDX];
    uint32_t         n_entries;
    uint32_t         epoch;          /* monotonically increasing             */
    uint32_t         total_bytes;    /* sum of all chunk_sz                  */
} GeoPointIndex;

/* ═══════════════════════════════════════════════════════════════════════════
   GPU PIPELINE CONTEXT
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    FiboSpine    spine;      /* pipe/tick ceremony                          */
    GearLock     gear;       /* CPU/GPU world sync                          */
    GeoPointIndex pidx;     /* point index for current bridge window        */
    uint32_t     bridges;    /* total bridge events                         */
    uint32_t     chunks;     /* total chunks dispatched                     */
    uint32_t     errors;     /* address/crc errors                          */
    /* jet_select integration (owner policy 2026-09-24) */
    uint32_t     wants_ready;    /* wants arrived, not yet coalesced         */
    uint32_t     wants_coalesced;/* wants merged into a bridge dispatch      */
    int          jet_strat_last; /* last jet_select pick (JET_C3/B/C1)       */
    uint32_t     last_merge_tick;/* merge tick of last want                  */
    uint8_t      is_init;    /* 1 = initialized                             */
} GeoPipelineCtx;

/* ═══════════════════════════════════════════════════════════════════════════
   INIT / CLEANUP
   ═══════════════════════════════════════════════════════════════════════════ */

static inline void geo_pipeline_init(GeoPipelineCtx *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    fibo_spine_init(&ctx->spine);
    ctx->gear.c144_ref = NULL;
    ctx->jet_strat_last = -1;
    ctx->is_init = 1;
}

static inline void geo_pipeline_reset(GeoPipelineCtx *ctx)
{
    if (!ctx) return;
    ctx->pidx.n_entries  = 0;
    ctx->pidx.epoch      = 0;
    ctx->pidx.total_bytes = 0;
    ctx->bridges = 0;
    ctx->chunks  = 0;
    ctx->errors  = 0;
    ctx->wants_ready = 0;
    ctx->wants_coalesced = 0;
    ctx->jet_strat_last = -1;
    ctx->last_merge_tick = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   JET WANT — a want arrives at tick t with jet travel L; select strategy
   ═══════════════════════════════════════════════════════════════════════════
   Production wire-in of jet_select (owner policy, proven T19-T22).
   Returns the selected strategy (JET_C3 / JET_B / JET_C1).
   Records merge tick so callers can group coalesce batches.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int geo_pipeline_want(GeoPipelineCtx *ctx, uint32_t t, uint32_t L)
{
    if (!ctx || !ctx->is_init) return -1;
    int strat = jet_select((int)GP_PIPE_TICKS, (int)L);
    ctx->jet_strat_last   = strat;
    ctx->last_merge_tick  = (uint32_t)jet_merge(strat, (int)GP_PIPE_TICKS,
                                                (int)t, (int)L);
    ctx->wants_ready++;
    return strat;
}

/* ═══════════════════════════════════════════════════════════════════════════
   BUILD INDEX — compute DRamTile addresses for a contiguous block range
   ═══════════════════════════════════════════════════════════════════════════
   Given a base address (from rail_hub pull) and byte count:
     1. Decompose each chunk address into DRamTile components
     2. Compute mmap byte offset
     3. Build PointIndex entry

   The caller provides chunk_data for checksum pre-computation.
   If chunk_data is NULL, ref_checksum is set to 0 (no verify).
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t geo_pipeline_build_index(
    GeoPipelineCtx *ctx,
    uint32_t        base_addr,      /* starting DRamTile address (flat)     */
    uint32_t        total_bytes,    /* total bytes to index                 */
    const uint8_t  *chunk_data,     /* source data for checksum (or NULL)   */
    uint32_t        chunk_sz)       /* chunk size (usually GP_CHUNK_SZ)    */
{
    if (!ctx || !ctx->is_init) return 0;
    if (chunk_sz == 0) chunk_sz = GP_CHUNK_SZ;

    GeoPointIndex *pidx = &ctx->pidx;
    pidx->n_entries  = 0;
    pidx->total_bytes = 0;
    pidx->epoch++;

    uint32_t n_chunks = (total_bytes + chunk_sz - 1) / chunk_sz;
    if (n_chunks > GP_MAX_PIPELINE_IDX) n_chunks = GP_MAX_PIPELINE_IDX;

    for (uint32_t c = 0; c < n_chunks; c++) {
        uint32_t addr = (base_addr + c) % DRAM_FULL;

        GeoPipelineEntry *e = &pidx->entries[pidx->n_entries];
        e->slot_id     = addr;  /* flat slot in address space */
        e->dram_addr   = addr;
        e->byte_offset = dram_mmap_offset(addr, chunk_sz);
        e->chunk_sz    = chunk_sz;

        /* XOR checksum of source chunk */
        if (chunk_data) {
            uint8_t ck = 0;
            for (uint32_t b = 0; b < chunk_sz; b++)
                ck ^= chunk_data[c * chunk_sz + b];
            e->ref_checksum = ck;
        } else {
            e->ref_checksum = 0;
        }

        pidx->n_entries++;
        pidx->total_bytes += chunk_sz;
    }

    return pidx->n_entries;
}

/* ═══════════════════════════════════════════════════════════════════════════
   BUILD INDEX FROM SPINE — at bridge time, index all pipes' current ticks
   ═══════════════════════════════════════════════════════════════════════════
   Mirrors gpu_jet_puller_build_index from FGLS_new:
   iterates all pipes, reads local_tick, computes DRamTile address via
   RDH-equivalent formula (pipe/12 = ring group, tick = wedge position).
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t geo_pipeline_build_index_spine(
    GeoPipelineCtx *ctx)
{
    if (!ctx || !ctx->is_init) return 0;

    GeoPointIndex *pidx = &ctx->pidx;
    pidx->n_entries  = 0;
    pidx->total_bytes = 0;
    pidx->epoch++;

    for (uint16_t p = 0; p < GP_PIPES; p++) {
        FiboPipe *pipe = &ctx->spine.pipes[p];
        uint8_t tick = pipe->local_tick;

        /* Map (pipe, tick) → DRamTile address
         * pipe group = pipe / 12, tick position within group */
        uint32_t ring = p / GP_PIPE_TICKS;
        uint32_t wedge = tick;
        uint32_t addr = dram_addr(ring, wedge % DRAM_GRID_X,
                                  wedge / DRAM_GRID_X, 0);

        GeoPipelineEntry *e = &pidx->entries[pidx->n_entries];
        e->slot_id     = (uint32_t)p * GP_PIPE_TICKS + tick;
        e->dram_addr   = addr;
        e->byte_offset = dram_mmap_offset(addr, GP_CHUNK_SZ);
        e->chunk_sz    = GP_CHUNK_SZ;
        e->ref_checksum = 0;  /* caller provides data for verification */

        pidx->n_entries++;
        pidx->total_bytes += GP_CHUNK_SZ;
    }

    return pidx->n_entries;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TICK — advance spine, detect bridge, build index
   ═══════════════════════════════════════════════════════════════════════════
   Returns bridge count (1 if bridge fired, 0 otherwise).
   At bridge: builds point index, increments gear counters.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t geo_pipeline_tick(GeoPipelineCtx *ctx)
{
    if (!ctx || !ctx->is_init) return 0;

    uint8_t state = fibo_spine_tick(&ctx->spine);
    gear_cpu_tick(&ctx->gear);

    if (state == JB_BRIDGING) {
        uint32_t n = geo_pipeline_build_index_spine(ctx);
        ctx->bridges++;
        ctx->chunks += n;
        /* coalesce: every ready want joins this bridge dispatch */
        ctx->wants_coalesced += ctx->wants_ready;
        ctx->wants_ready = 0;
        gear_gpu_tick(&ctx->gear, n);
        return 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TICK N — advance N ticks, count bridges
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t geo_pipeline_tick_n(GeoPipelineCtx *ctx, uint32_t n)
{
    uint32_t bridges = 0;
    for (uint32_t i = 0; i < n; i++)
        bridges += geo_pipeline_tick(ctx);
    return bridges;
}

/* ═══════════════════════════════════════════════════════════════════════════
   PER-PIPE TICK — independent pipe advancement
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t geo_pipeline_pipe_tick(GeoPipelineCtx *ctx,
                                              uint16_t pipe_id)
{
    if (!ctx || !ctx->is_init) return 0xFF;
    return fibo_spine_pipe_tick(&ctx->spine, pipe_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFY — check DRamTile address space integrity
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int geo_pipeline_verify(GeoPipelineCtx *ctx)
{
    if (!ctx || !ctx->is_init) return -1;

    /* Verify DRamTile address space has zero collisions */
    int rc = dram_verify_full();
    if (rc != 0) return -2;

    /* Verify Hilbert curve covers 0..63 uniquely */
    rc = dram_verify_hilbert();
    if (rc != 0) return -3;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   QUERY INDEX — lookup entry by slot_id
   ═══════════════════════════════════════════════════════════════════════════ */

static inline const GeoPipelineEntry* geo_pipeline_find(
    const GeoPipelineCtx *ctx, uint32_t slot_id)
{
    if (!ctx) return NULL;
    const GeoPointIndex *pidx = &ctx->pidx;
    for (uint32_t i = 0; i < pidx->n_entries; i++) {
        if (pidx->entries[i].slot_id == slot_id)
            return &pidx->entries[i];
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
   STATS
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_bridges;
    uint32_t total_chunks;
    uint32_t total_bytes;
    uint32_t errors;
    uint32_t epoch;
    uint32_t gear_cpu_ops;
    uint32_t gear_gpu_ops;
    uint32_t gear_cpu_worlds;
    uint32_t gear_gpu_worlds;
    uint32_t wants_ready;
    uint32_t wants_coalesced;
    int      jet_strat_last;
    FiboSpineStats spine;
} GeoPipelineStats;

static inline GeoPipelineStats geo_pipeline_stats(const GeoPipelineCtx *ctx)
{
    GeoPipelineStats s;
    memset(&s, 0, sizeof(s));
    if (!ctx) return s;

    s.total_bridges  = ctx->bridges;
    s.total_chunks   = ctx->chunks;
    s.total_bytes    = ctx->pidx.total_bytes;
    s.errors         = ctx->errors;
    s.epoch          = ctx->pidx.epoch;
    s.gear_cpu_ops   = ctx->gear.cpu_ops;
    s.gear_gpu_ops   = ctx->gear.gpu_ops;
    s.gear_cpu_worlds = ctx->gear.cpu_worlds;
    s.gear_gpu_worlds = ctx->gear.gpu_worlds;
    s.wants_ready     = ctx->wants_ready;
    s.wants_coalesced = ctx->wants_coalesced;
    s.jet_strat_last  = ctx->jet_strat_last;
    s.spine           = fibo_spine_stats(&ctx->spine);

    return s;
}

static inline void geo_pipeline_print_stats(const GeoPipelineCtx *ctx)
{
    GeoPipelineStats s = geo_pipeline_stats(ctx);
    printf("═══════════════════════════════════════════════════\n");
    printf("  GeoPipeline — GPU Pipeline Stats\n");
    printf("═══════════════════════════════════════════════════\n");
    printf("  Bridges:      %u\n", s.total_bridges);
    printf("  Chunks:       %u\n", s.total_chunks);
    printf("  Total bytes:  %u (%.3f MB)\n",
           s.total_bytes, (double)s.total_bytes / 1e6);
    printf("  Errors:       %u\n", s.errors);
    printf("  Epoch:        %u\n", s.epoch);
    printf("  Gear: cpu=%u(%u) gpu=%u(%u)\n",
           s.gear_cpu_ops, s.gear_cpu_worlds,
           s.gear_gpu_ops, s.gear_gpu_worlds);
    printf("  Jet: wants_ready=%u coalesced=%u strat=%d\n",
           s.wants_ready, s.wants_coalesced, s.jet_strat_last);
    printf("  Spine: active=%u bridged=%u resident=%u frozen=%u\n",
           s.spine.active_pipes, s.spine.bridged_pipes,
           s.spine.resident_pipes, s.spine.frozen_pipes);
    printf("  DRamTile: %u anchors × %u cells = %u slots\n",
           DRAM_ANCHORS, DRAM_CELLS_PER, DRAM_FULL);
    printf("═══════════════════════════════════════════════════\n");
}

#endif /* GEO_GPU_PIPELINE_H */
