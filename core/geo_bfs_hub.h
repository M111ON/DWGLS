/*
 * geo_bfs_hub.h — Breathing FS ↔ GeoPipeline Bridge (Phase 2)
 * ════════════════════════════════════════════════════════════════════
 * Connects the BIMG image (Phase 1 storage) to the existing geometric
 * pipeline — NO new geometry, NO new sync primitives. Everything reuses:
 *
 *   geo_cell_addr.h   — block offset → (gen,face,slot) → (pipe_id,tick)
 *   fibo_spine.h      — pipe ceremony: tick → 11 → jet_bridge_hop
 *   gear_lock.h       — CPU tick sync (128 × 162 = 20736 = GEO_FULL)
 *   bfs_persist.h     — mmap zero-copy payload access (Phase 1)
 *
 * Flow (mirrors geo_rail_hub_pull exactly):
 *   file/block index → flat id → pipe_id/tick
 *       → tick pipe to 11 → verify JB_BRIDGING
 *       → jet_bridge_hop → gear_cpu_tick
 *       → return zero-copy pointer INTO the mapped image (no malloc, no copy)
 *
 * Sacred: 20736 = 128×162 (gear) = 1728×12 (spine) = 144×144 (BMP blocks)
 * All headers static inline, zero malloc on hot path.
 */
#ifndef GEO_BFS_HUB_H
#define GEO_BFS_HUB_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "bfs_persist.h"
#include "geo_cell_addr.h"
#include "infra/fibo_spine.h"
#include "infra/gear_lock.h"

typedef struct {
    BFSMmapFS  map;         /* mapped BIMG image (zero-copy payloads) */
    FiboSpine  spine;       /* pipe/tick ceremony                    */
    P5HRibcage ribcage;     /* freeze/barrier at tick 12             */
    GearLock   gear;        /* CPU↔GPU sync                          */
    uint32_t   pulls;
    uint32_t   bridges;
    uint32_t   errors;
    uint8_t    is_open;
} BFSHub;

/* ═══════════════ OPEN / CLOSE ═══════════════ */
static inline int bfs_hub_open(BFSHub *h, const char *img_path)
{
    if (!h || !img_path) return -1;
    memset(h, 0, sizeof(*h));
    int rc = bfs_mmap_open(img_path, &h->map);
    if (rc != 0) return rc;
    fibo_spine_init(&h->spine);
    p5h_ribcage_init(&h->ribcage, &h->spine);
    /* gear lock sync source = live mapped byte 0 of header (scale byte) */
    h->gear.c144_ref = h->map.map_ptr;
    h->is_open = 1;
    return 0;
}

static inline void bfs_hub_close(BFSHub *h)
{
    if (!h) return;
    if (h->is_open) {
        p5h_ribcage_free(&h->ribcage);
        bfs_mmap_close(&h->map);
    }
    memset(h, 0, sizeof(*h));
}

/* ═══════════════ ADDRESS: file index → flat id → (pipe,tick) ═══════════════
 * Each BMP file owns consecutive blocks starting at home_block;
 * its k-th block is flat id (home_block + k) in [0, 20736). */
static inline uint32_t bfs_hub_flat_id(const BFSHub *h, uint32_t file_idx,
                                       uint32_t block_k)
{
    const BreathingFS *fs = &h->map.fs;
    if (!h->is_open || file_idx >= fs->n_files) return 0xFFFFFFFFu;
    const BFSFileEntry *fe = &fs->files[file_idx];
    if (!fe->valid || block_k >= fe->n_blocks) return 0xFFFFFFFFu;
    return (uint32_t)(fe->home_block + block_k) % BFS_TOTAL_SLOTS;
}

/* ═══════════════ PULL: file → zero-copy pointer into mapping ═══════════════
 * Replicates the rail_hub ceremony so the payload is delivered through
 * the same pipe/bridge/gate the GPU pipeline expects.
 *   data_out = pointer INTO the mmap (decode NOT performed — raw encoded)
 *   n_out    = payload byte count (encoded size)
 * Caller decodes via dyn_decode if needed (see bfs_hub_pull_decoded). */
static inline int bfs_hub_pull(BFSHub *h, uint32_t file_idx, uint32_t block_k,
                               const uint8_t **data_out, uint32_t *n_out)
{
    if (!h || !h->is_open || !data_out || !n_out) return -1;
    const BreathingFS *fs = &h->map.fs;
    if (file_idx >= fs->n_files || !fs->files[file_idx].valid) return -2;

    uint32_t flat = bfs_hub_flat_id(h, file_idx, block_k);
    if (flat == 0xFFFFFFFFu) return -3;

    uint32_t bi = flat;               /* block index == flat id here */
    if (bi >= BFS_BLOCKS) return -3;
    uint32_t p_off = h->map.enc_off[bi];
    uint16_t esz = fs->block_encoded_size[bi];
    if (esz == 0 || p_off == 0 || p_off + esz > h->map.map_size) return -4;

    /* ── pipe ceremony (same as geo_rail_hub_pull) ── */
    uint16_t pipe_id = 0; uint8_t tick = 0;
    geo_cell_addr_offset_to_pipe(flat, &pipe_id, &tick);

    uint32_t guard = 0;
    while (fibo_spine_pipe_is_bridge(&h->spine, pipe_id) == 0u) {
        uint8_t lt = fibo_spine_pipe_tick(&h->spine, pipe_id);
        if (lt == 0xFFu) { h->errors++; return -5; }
        if (++guard >= FS_TICKS_PER_CYCLE) break;
    }
    if (!fibo_spine_pipe_is_bridge(&h->spine, pipe_id)) {
        h->errors++;
        return -5;
    }

    /* ── jet bridge + gear tick ── */
    uint32_t hops = jet_bridge_hop(&h->spine, pipe_id, (const void *)0, 0, NULL);
    h->bridges += (hops > 0) ? 1u : 0u;
    gear_cpu_tick(&h->gear);
    h->pulls++;

    *data_out = h->map.map_ptr + p_off;
    *n_out = esz;
    return 0;
}

/* ═══════════════ PULL + DECODE: full file content (lossless) ═══════════════
 * Decodes all blocks into out (caller allocates total_bytes).
 * Validates RDH bijection per block. Returns 0 on success. */
static inline int bfs_hub_pull_file(BFSHub *h, uint32_t file_idx,
                                    int8_t *out, uint32_t out_size)
{
    if (!h || !h->is_open || !out) return -1;
    const BreathingFS *fs = &h->map.fs;
    if (file_idx >= fs->n_files || !fs->files[file_idx].valid) return -2;
    const BFSFileEntry *fe = &fs->files[file_idx];
    if (out_size < fe->total_bytes) return -3;

    for (uint32_t k = 0; k < fe->n_blocks; k++) {
        const uint8_t *enc; uint32_t esz;
        int rc = bfs_hub_pull(h, file_idx, k, &enc, &esz);
        if (rc != 0) return rc;

        uint32_t bi = bfs_hub_flat_id(h, file_idx, k);
        if (bi >= BFS_BLOCKS) return -4;
        uint32_t offset = k * BFS_SLOTS_BLOCK;
        uint32_t bsz = BFS_SLOTS_BLOCK;
        if (offset + bsz > fe->total_bytes) bsz = fe->total_bytes - offset;

        DynContainer dc;
        dyn_init(&dc);
        dc.header.strategy = fs->block_meta[bi].strategy;
        dc.header.payload_size = (uint16_t)esz;
        memcpy(dc.payload, enc, esz);
        dc.header.checksum = dyn_crc32(dc.payload, esz);
        int drc = dyn_decode(&dc, out + offset, BFS_SLOTS_BLOCK);
        if (drc != 0) { h->errors++; return -5; }
    }
    return 0;
}

/* ═══════════════ BATCH PULL ═══════════════ */
static inline uint32_t bfs_hub_pull_batch(BFSHub *h, const uint32_t *file_idxs,
                                          uint32_t n_files,
                                          const int8_t **outs,
                                          uint32_t *n_outs)
{
    uint32_t ok = 0;
    for (uint32_t i = 0; i < n_files; i++) {
        const BreathingFS *fs = &h->map.fs;
        if (file_idxs[i] >= fs->n_files) continue;
        uint32_t total = fs->files[file_idxs[i]].total_bytes;
        int rc = bfs_hub_pull_file(h, file_idxs[i], (int8_t *)outs[i], total);
        if (rc == 0) { n_outs[i] = total; ok++; }
    }
    return ok;
}

/* ═══════════════ STATS ═══════════════ */
static inline void bfs_hub_stats(const BFSHub *h)
{
    if (!h) return;
    printf("  BFSHub: files=%u pulls=%u bridges=%u errors=%u gear_worlds=%u\n",
           h->map.fs.n_files, h->pulls, h->bridges, h->errors,
           h->gear.cpu_worlds);
    FiboSpineStats st = fibo_spine_stats(&h->spine);
    printf("  Spine:  active=%u bridged=%u resident=%u frozen=%u ticks=%u mode=%s\n",
           st.active_pipes, st.bridged_pipes, st.resident_pipes,
           st.frozen_pipes, (unsigned)st.total_ticks,
           fibo_spine_mode_name(st.mode));
    const BreathingFS *fs = &h->map.fs;
    printf("  Seeker: scale=%.4f pos=%u home=%u %s\n",
           fs->seeker.scale, fs->seeker.current_pos, fs->seeker.home_pos,
           fs->seeker.is_hyperbolic ? "[HYPERBOLIC]" : "");
}

#endif /* GEO_BFS_HUB_H */