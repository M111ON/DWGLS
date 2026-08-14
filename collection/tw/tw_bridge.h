/*
 * tw_bridge.h — Bridge: Triangle Wheel Capture → POGLS Pipeline
 * ══════════════════════════════════════════════════════════════
 *
 * Converts TWCaptureInt { zone(0-9), slot(0-59), resid, drain }
 * into POGLS pipeline inputs:
 *
 *   Shell 1 (Active routing):
 *     tw_to_tantrix()  → TantrixTile for lc_tantrix.h
 *     tw_to_ring_state() → ring_state(0-119) for ring_classify()
 *     tw_to_node()     → geo_jump node_id (0..20735)
 *     tw_to_shell_id() → shell_id for geo_shell_encode()
 *
 *   Shell 2 (Residual):
 *     tw_resid_to_cell() → {floor, col, row} within metatron
 *
 *   Shell 3 (Frozen):
 *     tw_is_frozen()  → true if drain + tick >= barrier
 *
 * SCALE NOTE: TW_SCALE=207360 (12^4*10) divides TRing=720.
 * TRing = 720 = 12 faces × 60 slots. Full dodecahedral mapping.
 *
 * Depends on: tw_capture_int.h, lc_tantrix.h, geo_jump.h,
 *             geo_shell.h, geo_field_ring.h, geo_dodeca_ring.h
 *
 * Geometry: 12 faces × 10 sectors × 6 slots = 720 positions
 *   face 0-11  → dodecahedron face
 *   zone 0-9   → sector within face (pentagon-pair)
 *   slot 0-5   → child slot within sector
 *
 * No malloc. No float. Frozen.
 * ══════════════════════════════════════════════════════════════
 */

#ifndef TW_BRIDGE_H
#define TW_BRIDGE_H

#include <stdint.h>
#include "tw_capture_int.h"
#include "lc_tantrix.h"
#include "geo_jump.h"
#include "geo_shell.h"
#include "geo_field_ring.h"
#include "geo_dodeca_ring.h"

/* ── Shell 1: TW → Tantrix ──────────────────────────────────── */

/*
 * Map TW zone (0-9) → tantrix spoke pair (0-3).
 * 10 zones across 4 spoke pairs: zone/2 or zone%4.
 * Using zone%4 for balanced distribution.
 */
static inline uint8_t tw_zone_to_spoke(uint8_t zone) {
    return zone % 4u;
}

/*
 * Map TW slot (0-5 local within sector) → entry/exit gate (0-3).
 * 6 slots across 4 gates: entry = slot%4, exit = (slot+1)%4.
 * Guarantees entry ≠ exit for valid routing.
 */
static inline uint8_t tw_slot_to_entry(uint8_t local_slot) {
    return local_slot % 4u;
}
static inline uint8_t tw_slot_to_exit(uint8_t local_slot) {
    return (local_slot + 1u) % 4u;
}

/*
 * Convert TWCaptureInt → TantrixTile for lc_tantrix.h routing.
 *
 * Primary capture: NORMAL tile routing from entry→exit through spoke.
 * Drain capture (boundary): MERGE tile combining primary + secondary.
 * No capture (drain=1, secondary only): route through secondary zone.
 */
static inline TantrixTile tw_to_tantrix(const TWCaptureInt *cap) {
    uint8_t local = cap->slot - (cap->zone / TW_SLOTS_PER) * TW_SLOTS_PER;
    uint8_t spoke = tw_zone_to_spoke(cap->zone);
    uint8_t entry = tw_slot_to_entry(local);
    uint8_t exit  = tw_slot_to_exit(local);

    if (cap->drain) {
        /* boundary: MERGE tile — primary and secondary merge */
        (void)spoke;
        (void)entry;
        (void)exit;
        return TANTRIX_MERGE;
    }

    return tantrix_make(entry, exit, spoke, TANTRIX_CLASS_NORMAL);
}

/*
 * Convert secondary (drain) zone → TantrixTile.
 * Used when drain=1 to route through the adjacent sector.
 */
static inline TantrixTile tw_drain_to_tantrix(const TWCaptureInt *cap) {
    if (!cap->drain) return TANTRIX_NULL;

    uint8_t local = cap->drain_slot
                  - (cap->drain_zone / TW_SLOTS_PER) * TW_SLOTS_PER;
    uint8_t spoke = tw_zone_to_spoke(cap->drain_zone);
    uint8_t entry = tw_slot_to_entry(local);
    uint8_t exit  = tw_slot_to_exit(local);

    return tantrix_make(entry, exit, spoke, TANTRIX_CLASS_NORMAL);
}

/* ── Shell 1: TW → Ring State ───────────────────────────────── */

/*
 * Convert TW zone+slot → ring_state (0-119) for ring_classify().
 *
 * TW zone 0-9 → ring 0-9 (direct mapping).
 * TW local slot 0-5 → face 0-11 (×2 scaling: 6→12 faces).
 * ring_state = ring × 12 + face.
 *
 * Globe A only (TW is single-globe). Globe B accessed via
 * dodeca_globe_offset(1) downstream.
 */
static inline uint8_t tw_to_ring_state(uint8_t zone, uint8_t slot) {
    uint8_t local = slot - (zone / TW_SLOTS_PER) * TW_SLOTS_PER;
    uint8_t ring  = zone % (RING_DODECA * RING_FLOWERS);  /* 0-9 */
    uint8_t face  = (local * 2u) % DODECA_FACES;          /* 0-11 */
    return (uint8_t)(ring * DODECA_FACES + face);
}

/* ── Shell 1: TW → geo_jump node_id ─────────────────────────── */

/*
 * Convert TW zone+slot → geo_jump node_id.
 * Uses geo_shell_encode() then geo_shell_to_node().
 * Side = 0 (Globe A) for clean captures.
 */
static inline uint32_t tw_to_node(uint8_t zone, uint8_t slot) {
    uint8_t face = zone % SHELL_FACES;      /* 0-11 */
    uint8_t ring = slot % SHELL_RINGS;      /* 0-11 */
    uint32_t sid = geo_shell_encode(face, ring, 0u);  /* side=A */
    return geo_shell_to_node(sid);
}

/*
 * Convert drain zone+slot → geo_jump node_id (secondary path).
 * Side = 0 (Globe A).
 */
static inline uint32_t tw_drain_to_node(const TWCaptureInt *cap) {
    if (!cap->drain) return 0u;
    uint8_t face = cap->drain_zone % SHELL_FACES;
    uint8_t ring = cap->drain_slot % SHELL_RINGS;
    uint32_t sid = geo_shell_encode(face, ring, 0u);
    return geo_shell_to_node(sid);
}

/* ── Shell 1: TW → shell_id ─────────────────────────────────── */

/*
 * Convert TWCaptureInt → shell_id for geo_shell.h system.
 * face = zone, ring = slot%12, side = 0 (A) or 1 (B if drain).
 */
static inline uint32_t tw_to_shell_id(const TWCaptureInt *cap) {
    uint8_t face = cap->zone % SHELL_FACES;
    uint8_t ring = cap->slot % SHELL_RINGS;
    uint8_t side = cap->drain ? 1u : 0u;
    return geo_shell_encode(face, ring, side);
}

/* ── Shell 1: TW → ring_classify pipeline ────────────────────── */

/*
 * Full pipeline: TWCaptureInt → RingClassified via ring_classify().
 * Caller supplies density, is_temporal, tick (from encoder/framing).
 * Globe = 0 (TW is Globe A).
 */
static inline RingClassified tw_to_ring_classified(
    const TWCaptureInt *cap,
    uint8_t  density,
    uint8_t  is_temporal,
    uint8_t  layer,
    uint32_t tick)
{
    uint8_t state = tw_to_ring_state(cap->zone, cap->slot);
    return ring_classify(state, density, is_temporal, layer, tick);
}

/* ── Shell 1: TW → full pipeline route ───────────────────────── */

/*
 * Full pipeline: TWCaptureInt → PipelineResult.
 * Combines ring_classify + geo_jump routing.
 */
static inline PipelineResult tw_pipeline_route(
    const TWCaptureInt *cap,
    uint8_t  density,
    uint8_t  is_temporal,
    uint8_t  layer,
    uint32_t tick,
    uint32_t seed_node)
{
    uint8_t state = tw_to_ring_state(cap->zone, cap->slot);
    return pipeline_route(state, density, is_temporal, layer, tick, seed_node);
}

/* ── Shell 2: Residual → Metatron cell ───────────────────────── */

/*
 * Normalize TW residual (int32 in TW_SCALE units) → metatron cell.
 * resid_x/y are in range [-TW_SCALE, +TW_SCALE].
 * Map to metatron col (1-4) and row (1-48) via quantization.
 *
 * col = (resid_x + TW_SCALE) * 4 / (2*TW_SCALE) + 1  → 1-4
 * row = (resid_y + TW_SCALE) * GEO_BLOCK / (2*TW_SCALE) + 1  → 1-48
 *
 * Returns floor = 1 (always middle floor for residual).
 */
typedef struct {
    uint8_t col;    /* 1-4 metatron column */
    uint8_t row;    /* 1-48 metatron row (GEO_BLOCK=48) */
    uint8_t floor;  /* 1 (middle floor for residual) */
    uint8_t _pad;
} TWMetatronCell;

static inline TWMetatronCell tw_resid_to_cell(int32_t resid_x, int32_t resid_y) {
    TWMetatronCell c;
    /* shift to unsigned: [-TW_SCALE, +TW_SCALE] → [0, 2*TW_SCALE] */
    int64_t sx = (int64_t)resid_x + TW_SCALE;
    int64_t sy = (int64_t)resid_y + TW_SCALE;
    if (sx < 0) sx = 0;
    if (sx > 2 * TW_SCALE) sx = 2 * TW_SCALE;
    if (sy < 0) sy = 0;
    if (sy > 2 * TW_SCALE) sy = 2 * TW_SCALE;

    c.col   = (uint8_t)(sx * 4 / (2 * TW_SCALE) + 1);
    c.row   = (uint8_t)(sy * GEO_BLOCK / (2 * TW_SCALE) + 1);
    c.floor = 1u;
    c._pad  = 0u;
    if (c.col > 4u) c.col = 4u;
    if (c.row > (uint8_t)GEO_BLOCK) c.row = (uint8_t)GEO_BLOCK;
    return c;
}

/*
 * Convert residual → node offset within current tower.
 * Uses Hilbert index for col→cell mapping.
 */
static inline uint32_t tw_resid_to_offset(int32_t resid_x, int32_t resid_y) {
    TWMetatronCell c = tw_resid_to_cell(resid_x, resid_y);
    uint32_t cell = _hilbert_idx(c.col - 1u, c.row - 1u, GEO_METATRON_COLS);
    return (c.floor - 1u) * GEO_METATRON_CELLS + cell;
}

/* ── Shell 3: Freeze trigger ─────────────────────────────────── */

/*
 * P5H barrier tick for Shell 3 freeze.
 * TW drain=1 → tensor near sector boundary → freeze at tick 12.
 * frozen address = POGLS wallet address.
 */
#define TW_P5H_BARRIER_TICK  12u

static inline int tw_is_frozen(uint8_t drain, uint32_t tick) {
    return drain && (tick >= TW_P5H_BARRIER_TICK);
}

/*
 * Freeze → address: when frozen, assign POGLS address.
 * address = face * SHELL_FULL/SHELL_FACES + layer * GEO_TOWER + residual_cell.
 * This becomes the wallet entry.
 */
static inline uint32_t tw_freeze_address(const TWCaptureInt *cap,
                                          uint32_t tick) {
    if (!tw_is_frozen(cap->drain, tick)) return 0u;

    uint32_t face_base = (uint32_t)(cap->zone % SHELL_FACES)
                       * (SHELL_FULL / SHELL_FACES);
    uint32_t layer_off = (tick % SHELL_RINGS) * GEO_TOWER;
    uint32_t cell_off  = tw_resid_to_offset(cap->resid_x, cap->resid_y);
    return GEO_WRAP(face_base + layer_off + cell_off);
}

/* ── Combined: TWCaptureInt → full POGLS result ──────────────── */

typedef struct {
    uint32_t       node;          /* primary geo_jump node_id (0..20735)    */
    uint32_t       drain_node;    /* secondary node (0 if no drain)   */
    TantrixTile    tile;          /* tantrix routing tile             */
    TantrixTile    drain_tile;    /* secondary tile (MERGE or NULL)   */
    uint32_t       shell_id;      /* shell_id for shell system        */
    uint8_t        ring_state;    /* ring_state for ring_classify     */
    uint8_t        frozen;        /* 1 if Shell 3 freeze active       */
    uint32_t       freeze_addr;   /* POGLS wallet address (if frozen) */
    RingClassified ring;          /* full ring classification         */
} TWBridgeResult;

/*
 * Master bridge: TWCaptureInt → TWBridgeResult.
 * Caller supplies face, density, is_temporal, layer, tick, seed_node.
 *
 * face: 0-11, which dodecahedron face this capture belongs to.
 *       For single-face operation, use face=0.
 *       For 12-face iteration, pass the face index.
 */
static inline TWBridgeResult tw_bridge(
    const TWCaptureInt *cap,
    uint8_t  face,
    uint8_t  density,
    uint8_t  is_temporal,
    uint8_t  layer,
    uint32_t tick,
    uint32_t seed_node)
{
    TWBridgeResult r;
    (void)face; /* node_id encodes pentagon natively */

    /* Shell 1: primary path */
    r.node       = tw_to_node(cap->zone, cap->slot);
    r.drain_node = tw_drain_to_node(cap);
    r.tile       = tw_to_tantrix(cap);
    r.drain_tile = tw_drain_to_tantrix(cap);
    r.shell_id   = tw_to_shell_id(cap);
    r.ring_state = tw_to_ring_state(cap->zone, cap->slot);

    /* Shell 1: full pipeline */
    r.ring = tw_to_ring_classified(cap, density, is_temporal, layer, tick);

    /* Shell 3: freeze */
    r.frozen      = (uint8_t)tw_is_frozen(cap->drain, tick);
    r.freeze_addr = tw_freeze_address(cap, tick);

    return r;
}

#endif /* TW_BRIDGE_H */
