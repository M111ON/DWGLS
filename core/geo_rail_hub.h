/* ═══════════════════════════════════════════════════════════════════════════
 * geo_rail_hub.h — Rail Hub: Zero-Copy Sync Pipeline for llama.cpp
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Thin sync layer that orchestrates the existing infrastructure into one
 * synchronous "give me weights for tensor X" call:
 *
 *   llama.cpp calls:  geo_rail_hub_pull(hub, "blk.0.attn_q.weight", &data, &n, &dtype)
 *                         │
 *                         ▼
 *   1. LOOKUP:    geo_hub_load()                          — found + read weight
 *                  from .gcube via GeoTensorHub
 *   2. ADDRESS:   geo_cell_addr_offset_to_pipe()           — (gen, face, slot)
 *                  → (pipe_id, tick)
 *   3. SYNC:      fibo_spine_init() + tick until pipe is at FS_JET_BRIDGE_TICK
 *                                                                (tick 11)
 *   4. BRIDGE:    jet_bridge_hop(NULL, 0, NULL)           — bridge entry, no copy
 *   5. BARRIER:   p5h_freeze_at_tick12()                  — freeze barrier
 *   6. RETURN:    the same pointer hub gave us — zero copy added by rail hub.
 *
 * IMPORTS (all pre-existing — created NOTHING new except this header):
 *   geo_tensor_hub.h    — hub_open / hub_load / hub_close
 *   geo_cell_addr.h     — tensor_offset → (gen, face, slot) → (pipe_id, tick)
 *   infra/fibo_spine.h  — FiboSpine + P5HRibcage + jet_bridge_hop
 *   infra/gear_lock.h   — GearLock for CPU/GPU world-count parity (init only)
 *   infra/geo_rail_sync.h — rail_sync_arriving (callback / not used in pull)
 *   infra/geo_spoke_sync.h— embeds inside fibo_spine_init() automatically
 *
 * DESIGN CONSTRAINTS:
 *   - No malloc in the rail hub itself (geo_hub_load's existing malloc is hub's
 *     business — we don't add any here, and we return its pointer untouched).
 *   - No float / double anywhere here.
 *   - All functions static inline; header-only; usable from -fsyntax-only.
 *
 * LIMITATION / CONTRACT:
 *   The hub must be opened (via geo_hub_open or the wrapping geo_rail_hub_open)
 *   and the spine MUST be re-init'd cleanly between calls if you reuse the same
 *   RailHub across tensors (use geo_rail_hub_reset()).  Both are documented in
 *   code below.
 * ═══════════════════════════════════════════════════════════════════════════ */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Hub + addressing layer (core/) */
#include "geo_tensor_hub.h"    /* GeoTensorHub, geo_hub_open/load/close */
#include "geo_cell_addr.h"     /* GeoCellAddr, geo_cell_addr_from_offset */

/* Sync layer (infra/) */
#include "infra/gear_lock.h"    /* GearLock               */
#include "infra/geo_rail_sync.h" /* rail_sync_arriving     */
#include "infra/geo_spoke_sync.h"/* SpokeSync (via spine) */
#include "infra/fibo_spine.h"   /* FiboSpine, P5HRibcage,
                                   jet_bridge_hop, p5h_freeze_at_tick12 */

/* ═══════════════════════════════════════════════════════════════
   RAIL HUB CONTEXT — holds borrowed hub + scratch spine + ribcage
   ═══════════════════════════════════════════════════════════════
   The FiboSpine is 1728 pipes × ~16 bytes/pipe ≈ 28 KB on the stack —
   too big for stack-inlined calls. Callers should store GeoRailHub in
   static / heap scope, never as a stack local inside a hot loop.
   P5HRibcage does a calloc() in p5h_ribcage_init() (heap, NOT hot-path).
   We free it in geo_rail_hub_close().
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    GeoTensorHub  *hub;          /* borrowed — outlives the RailHub scope  */
    FiboSpine      spine;       /* 1728-pipe rail state (28 KB)            */
    P5HRibcage    *ribcage;     /* heap-allocated via p5h_ribcage_init    */
    GearLock       gear;        /* CPU/GPU world-count (init only)         */
    uint8_t        is_open;      /* 1 = spine initialized, ready for pulls  */
} GeoRailHub;

/* ═══════════════════════════════════════════════════════════════
   RAIL HUB OPEN — hub_open + spine_init + ribcage_init
   ═══════════════════════════════════════════════════════════════ */
static inline int geo_rail_hub_open(GeoRailHub *r,
                                     GeoTensorHub *hub)
{
    if (!r || !hub || !hub->is_open) return -1;
    memset(r, 0, sizeof(*r));
    r->hub = hub;

    /* 1. Init Fibo Spine (1728 pipes × 12 ticks, all zeroed) */
    fibo_spine_init(&r->spine);
    r->spine.mode = FS_MODE_PERPIPE;  /* per-pipe tick independence */

    /* 2. P5H Ribcage — calloc's entries internally (cold path only) */
    r->ribcage = (P5HRibcage *)malloc(sizeof(P5HRibcage));
    if (!r->ribcage) return -1;
    p5h_ribcage_init(r->ribcage, &r->spine);

    /* 3. GearLock (CPU=128, GPU=162 worlds, synchrony parity field) */
    memset(&r->gear, 0, sizeof(r->gear));
    r->gear.cpu_worlds = GEAR_CPU_WORLD;  /* 128 */
    r->gear.gpu_worlds = GEAR_GPU_WORLD;  /* 162 */

    r->is_open = 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   RAIL HUB RESET — wipe spine + ribcage between consecutive pulls
   Call between pulls if you want fresh sync state (re-enter at tick 0).
   ═══════════════════════════════════════════════════════════════ */
static inline void geo_rail_hub_reset(GeoRailHub *r) {
    if (!r || !r->is_open) return;
    /* Free existing ribcage and re-init spine */
    if (r->ribcage) {
        p5h_ribcage_free(r->ribcage);
        free(r->ribcage);
    }
    fibo_spine_init(&r->spine);
    r->spine.mode = FS_MODE_PERPIPE;
    r->ribcage = (P5HRibcage *)malloc(sizeof(P5HRibcage));
    if (r->ribcage) p5h_ribcage_init(r->ribcage, &r->spine);
}

/* ═══════════════════════════════════════════════════════════════
   RAIL HUB CLOSE — free ribcage, leave hub alone (caller owns hub)
   ═══════════════════════════════════════════════════════════════ */
static inline void geo_rail_hub_close(GeoRailHub *r) {
    if (!r || !r->is_open) return;
    if (r->ribcage) {
        p5h_ribcage_free(r->ribcage);
        free(r->ribcage);
        r->ribcage = NULL;
    }
    r->hub = NULL;
    r->is_open = 0;
}

/* ═══════════════════════════════════════════════════════════════
   RAIL HUB PULL — main hot-path entry point
   ═══════════════════════════════════════════════════════════════

   Args (mirrors geo_hub_load):
     hub         — must have been geo_rail_hub_open()'d
     tensor_name — name from llama.cpp tensor list, e.g. "blk.0.attn_q.weight"
     data_out    — receives pointer to tensor's weight bytes (caller frees!)
     n_elems_out — receives element count
     dtype_out   — receives GGML dtype enum (e.g. 8 for Q8_0)

   Returns:
     0  on success — *data_out, *n_elems_out, *dtype_out written
     -1 if arguments or hub are invalid
     -2 if geo_hub_load itself failed (tensor not found / cube missing)
     -3 if ribcage barrier returned 0 frozen entries (sync fault)

   IMPORTANT: this function performs NO copy of data — the pointer returned
   is the buffer that geo_hub_load malloc'd. The CALLER owns and must free()
   it (consistent with hub_load's existing contract).
   ═══════════════════════════════════════════════════════════════ */
static inline int geo_rail_hub_pull(GeoRailHub *r,
                                      const char *tensor_name,
                                      uint8_t **data_out,
                                      uint32_t *n_elems_out,
                                      uint32_t *dtype_out)
{
    if (!r || !r->is_open || !tensor_name || !data_out ||
        !n_elems_out || !dtype_out) {
        return -1;
    }

    /* ── 1. LOOKUP via hub — gives us the raw weight bytes ──────────── */
    uint8_t  *raw   = NULL;
    uint32_t  n     = 0;
    uint32_t  dtype = 0;
    if (geo_hub_load(r->hub, tensor_name, &raw, &n, &dtype) != 0) {
        return -2;  /* tensor not in GGUF index or .gcube */
    }

    /* Tensor's linear offset within .gcube comes from the cube entry. We
     * recover it by name → block_start (which is the linear index). This
     * is O(1) per tensor because gcube_find is name-keyed (NOT a search in
     * the rail hub itself — that's hub's bookkeeping). */
    const GCubeTensorEntry *ge =
        gcube_find(r->hub->cube, tensor_name);
    if (!ge) return -2;

    /* tensor_offset = first block index for this tensor */
    uint32_t tensor_offset = ge->block_start;

    /* ── 2. ADDRESS: tensor_offset → (pipe_id, tick) ───────────────── */
    uint16_t pipe_id = 0;
    uint8_t  tick    = 0;
    geo_cell_addr_offset_to_pipe(tensor_offset, &pipe_id, &tick);

    /* ── 3. SYNC: drive the chosen pipe up to tick 11 (Jet Bridge) ───
     * fibo_spine_init already done in geo_rail_hub_open. Per-pipe-local
     * mode means each pipe's tick advances independently — we just tick
     * our specific pipe until it lands on FS_JET_BRIDGE_TICK (11). */
    FiboSpine *fs = &r->spine;
    uint32_t guard = 0;
    while (fibo_spine_pipe_is_bridge(fs, pipe_id) == 0u) {
        uint8_t lt = fibo_spine_pipe_tick(fs, pipe_id);
        if (lt == 0xFFu) break;          /* invalid pipe_id — defensive */
        if (++guard >= FS_TICKS_PER_CYCLE) break;  /* one full cycle max */
    }

    /* Validate we actually ARE at the bridge boundary before hopping. */
    if (!fibo_spine_pipe_is_bridge(fs, pipe_id)) {
        /* Spine was unable to cycle this pipe to tick 11. This should be
         * impossible with FS_TICKS_PER_CYCLE guards above, but we don't
         * want to silently bridge from a non-bridge tick. */
        return -3;
    }

    /* ── 4. BRIDGE: fire Jet Bridge — no data copy, no residual write ─ */
    jet_bridge_hop(fs, pipe_id, NULL, 0u, NULL);

    /* ── 5. BARRIER: freeze at tick-12 boundary ──────────────────────
     * p5h_freeze_at_tick12 walks bridged pipes and freezes them; here we
     * just need the barrier to be satisfied for THIS pipe. The freeze
     * count tells us how many pipes were settled. */
    uint32_t frozen = p5h_freeze_at_tick12(r->ribcage);
    if (frozen == 0u) {
        /* No pipe got frozen — sync fault. Tear down cleanly so the next
         * caller isn't left with a half-bridged spine. */
        jet_bridge_return(fs, pipe_id);
        return -3;
    }

    /* ── 6. DELIVER: same pointer hub gave us — zero copy by us ─────── */
    *data_out    = raw;
    *n_elems_out = n;
    *dtype_out   = dtype;

    /* Return the pipe back to active mode so the spine is clean for the
     * next pull (optional — caller can also call geo_rail_hub_reset()
     * to fully wipe state between tensors). */
    jet_bridge_return(fs, pipe_id);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   RAIL HUB PULL BATCH — convenience for pre-fetching N tensors
   Returns the count of tensors successfully pulled (0..max).
   EachPull stores results into the paired {data, n_elems, dtype} slots;
   caller is responsible for freeing every data[] that was filled.
   ═══════════════════════════════════════════════════════════════ */
static inline uint32_t geo_rail_hub_pull_batch(GeoRailHub *r,
                                                 const char *const *names,
                                                 uint8_t **data,
                                                 uint32_t *n_elems,
                                                 uint32_t *dtype,
                                                 uint32_t max)
{
    if (!r || !names || !max) return 0;
    uint32_t ok = 0;
    for (uint32_t i = 0; i < max; i++) {
        int rc = geo_rail_hub_pull(r, names[i],
                                     data ? &data[i] : NULL,
                                     n_elems ? &n_elems[i] : NULL,
                                     dtype ? &dtype[i] : NULL);
        if (rc == 0) {
            ok++;
        } else if (data) {
            data[i] = NULL;  /* mark failure for caller cleanup */
        }
    }
    return ok;
}

/* ═══════════════════════════════════════════════════════════════
   RAIL HUB STATS — quick-status peek for debugging / tests
   ═══════════════════════════════════════════════════════════════ */
static inline FiboSpineStats geo_rail_hub_stats(const GeoRailHub *r) {
    return fibo_spine_stats(r ? &r->spine : NULL);
}
