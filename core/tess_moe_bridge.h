/* tess_moe_bridge.h — .tess Capo ↔ MoE DtSlotRegion Bridge
 *
 * MAP not COMPRESS for MoE: expert weight blocks live in .tess capos
 * (20736-slot scattered layout). Bridge provides geometric addressing
 * to extract a single expert's slice without loading the whole stack.
 *
 * Data model:
 *   - Stacked tensor: [n_layers × n_experts × blocks_per_expert] cells
 *     baked into one (or sharded) .tess capo file(s) via stride-37 scatter.
 *   - Each wtype (gate/up/down) may be a separate stack (separate .tess)
 *     or interleaved; the bridge handles both via wtype-aware offset.
 *
 * Two use modes:
 *   1) Direct serve: open .tess capo, load one expert's blocks on demand
 *      (streaming inference — no DtSlotRegion).
 *   2) Bulk bake: copy .tess capo elements into DtSlotRegion at
 *      moe_expert_to_flat() slots (so moe_load_weight can serve them).
 *
 * Depends: geo_tess_container.h, moe_expert_addr.h, dramtile_store.h (for mode 2)
 */
#ifndef TESS_MOE_BRIDGE_H
#define TESS_MOE_BRIDGE_H

#include <stdint.h>
#include <string.h>
#include "geo_tess_container.h"
#include "moe_expert_addr.h"
#include "infra/dramtile_store.h"

/* ── offset helpers ─────────────────────────────────────────── */

/* Base element index within a .tess stack for one expert's one wtype.
 * Stack layout (per wtype): layer-major, expert-minor.
 *   idx = (layer * n_experts + expert) * blocks_per_expert
 * For interleaved (wtype stride), add wtype*... offset; caller picks. */
static inline uint32_t tess_moe_block_offset(uint32_t layer, uint32_t expert,
                                             uint32_t n_experts,
                                             uint32_t blocks_per_expert) {
    return (layer * n_experts + expert) * blocks_per_expert;
}

/* Interleaved variant: 3 wtypes interleaved in one stack.
 *   idx = ((layer * n_experts + expert) * 3 + wtype) * blocks_per_expert */
static inline uint32_t tess_moe_block_offset_interleaved(uint32_t layer,
                                                         uint32_t expert,
                                                         uint32_t wtype,
                                                         uint32_t n_experts,
                                                         uint32_t blocks_per_expert) {
    return ((layer * n_experts + expert) * MOE_WEIGHT_TYPES + wtype) * blocks_per_expert;
}

/* ── direct serve (mode 1) ─────────────────────────────────── */

/* Load one expert's blocks directly from an open .tess capo.
 * Returns bytes written, or 0 on error (range out of bounds).
 * Stack must contain at least offset+blocks_per_expert elements. */
static inline int tess_moe_load_expert_blocks(const TESS_CapoReader *r,
                                              uint32_t layer, uint32_t expert,
                                              uint32_t n_experts,
                                              uint32_t blocks_per_expert,
                                              void *dst) {
    uint32_t off = tess_moe_block_offset(layer, expert, n_experts, blocks_per_expert);
    return tess_capo_load_range(r, off, blocks_per_expert, dst);
}

/* Interleaved variant. */
static inline int tess_moe_load_expert_blocks_interleaved(const TESS_CapoReader *r,
                                                          uint32_t layer, uint32_t expert,
                                                          uint32_t wtype,
                                                          uint32_t n_experts,
                                                          uint32_t blocks_per_expert,
                                                          void *dst) {
    uint32_t off = tess_moe_block_offset_interleaved(layer, expert, wtype, n_experts, blocks_per_expert);
    return tess_capo_load_range(r, off, blocks_per_expert, dst);
}

/* Single-block variant (e.g., load block k of expert). */
static inline int tess_moe_load_expert_one(const TESS_CapoReader *r,
                                           uint32_t layer, uint32_t expert,
                                           uint32_t n_experts,
                                           uint32_t blocks_per_expert,
                                           uint32_t block_idx, void *dst) {
    if (block_idx >= blocks_per_expert) return 0;
    uint32_t off = tess_moe_block_offset(layer, expert, n_experts, blocks_per_expert) + block_idx;
    return tess_capo_load_elem(r, off, dst);
}

/* ── bulk bake to DtSlotRegion (mode 2) ──────────────────────
 * Copies every expert's blocks from .tess capo into DtSlotRegion,
 * one slot per block. Region must have at least total_blocks slots
 * (each slot holds one cell, e.g. 144 B for Q4_K).
 * Returns 0 on success, -1 on error.
 *
 * The dt_slot_* calls require dramtile_store.h to be included by the
 * translation unit before this header.
 */

/* Mode-2 bake: .tess stack (one wtype) → DtSlotRegion.
 * Each block i in the stack goes to flat = base_flat + i,
 * where base_flat is the flat address for (layer0, expert0, wtype).
 * Uses dt_slot_put directly. */
static inline int tess_moe_bake_to_region(const TESS_CapoReader *r,
                                          DtSlotRegion *region,
                                          uint32_t base_layer,
                                          uint32_t n_layers,
                                          uint32_t n_experts,
                                          uint32_t blocks_per_expert,
                                          uint32_t wtype) {
    uint32_t total = n_layers * n_experts * blocks_per_expert;
    if (r->n_elems < total) return -1;
    uint32_t base_flat = moe_expert_to_flat(base_layer, 0, wtype);
    for (uint32_t i = 0; i < total; i++) {
        uint8_t cell[TESS_CELL_Q8_K]; /* max cell */
        if (tess_capo_load_elem(r, i, cell) == 0) return -1;
        uint32_t flat = (base_flat + i) % TESS_TOTAL;
        if (!dt_slot_put(region, flat, cell, r->cell_size)) return -1;
    }
    return 0;
}

#endif /* TESS_MOE_BRIDGE_H */
