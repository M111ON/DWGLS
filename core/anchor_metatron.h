/* core/anchor_metatron.h — A1/A4/A6: wire the Metatron 13-cell facts, the
 * 96-piece grid and the hex address into LIVE ANCHOR ADDRESSES.
 *
 * Owner spec (2026-10-05, card task-t28a0929b): the numbers are geometry, so
 * they must DRIVE something, not just be counted in a test:
 *   A1  Metatron 13 = 1 core + 6 frustum faces + 6 octa midpoints  -> an address
 *   A4  96 = 48 (cube edges) + 24 (tetra faces) + 24 (cube faces); 96*216=20736
 *       -> a WORKING grid: 96 pieces x 216 slots tiles the 20736 field exactly
 *   A6  hex address -> the anchor's 6 directions + centre as a HexTile, packed
 *       by the existing hex codec (core/hex_tile.h + core/geo_hex_layer.h)
 *
 * Owner warning carried from A3 (memory #7581): the frustum RAY key does NOT
 * predict nearest (nearest-shares-ray ~1%). These addresses are IDENTITY /
 * placement keys, NOT a retrieval order — do not wire them as a prune key.
 *
 * derive-don't-store: no table is materialised; every value is an O(1)
 * function of the anchor bytes. Header-only, std-only. Deterministic.
 */
#ifndef ANCHOR_METATRON_H
#define ANCHOR_METATRON_H

#include <stdint.h>
#include <stddef.h>

#include "anchor_weight.h"   /* aw_anchor_t, AW_CELLS, AW_CENTER, AW_OUTER */
#include "hex_tile.h"        /* HexTile, hex_tile_classify */
#include "geo_hex_layer.h"   /* gpx4_geo_hex_encode_sub / decode_sub */

/* ── the shared field size (owner: 20736 = 144^2 = 12^4) ─────────────── */
#define AM_FIELD      20736u

/* ── A1: Metatron 13 cells ───────────────────────────────────────────── */
#define AM_META_CORE   1u
#define AM_META_FACES  6u
#define AM_META_OCTA   6u
#define AM_META_CELLS  13u   /* 1 + 6 + 6 */

/* tetra edges as 6 index pairs -> the 6 octa vertices (A5.1). */
static const int AM_TETRA_EDGE[6][2] = {
    {0,1},{0,2},{0,3},{1,2},{1,3},{2,3}
};

/* the anchor's 13 cell identities, in Metatron order:
 *   [0]     core  (centre letter + case + centre weight)
 *   [1..6]  frustum faces (6 direction letters + their slot weights)
 *   [7..12] octa midpoints (folds of the 6 tetra edge endpoints)
 * Pairwise-distinct for a non-degenerate anchor (the A5 gate proves it). */
static inline void am_metatron13(const aw_anchor_t *a, uint64_t out[AM_META_CELLS]) {
    if (!a || !out) return;
    out[0] = ((uint64_t)a->letter << 40)
           | ((uint64_t)(a->is_upper & 1u) << 32)
           | (uint64_t)a->weight[AW_CENTER];
    for (int f = 0; f < 6; f++) {
        out[1 + f] = ((uint64_t)a->dir_letter[f] << 40)
                   | ((uint64_t)a->fold16[f] << 16)
                   | (uint64_t)a->weight[f];
    }
    for (int e = 0; e < 6; e++) {
        uint64_t u = a->pattern[AM_TETRA_EDGE[e][0]];
        uint64_t v = a->pattern[AM_TETRA_EDGE[e][1]];
        out[7 + e] = u ^ (v << 1) ^ ((uint64_t)e * 0x9E3779B97F4A7C15ull);
    }
}

/* A1 wiring: fold the 13 cells into ONE field address in [0, 20736). This is
 * what makes the 13-cell facts a live consumer rather than a count — the
 * anchor now has a Metatron address. */
static inline uint32_t am_metatron_addr(const aw_anchor_t *a) {
    uint64_t c[AM_META_CELLS];
    am_metatron13(a, c);
    uint64_t h = 1469598103934665603ull;               /* FNV-1a 64 */
    for (int i = 0; i < (int)AM_META_CELLS; i++) {
        h ^= c[i]; h *= 1099511628211ull;
    }
    return (uint32_t)(h % (uint64_t)AM_FIELD);
}

/* ── A4: the 96-piece grid as a WORKING grid ─────────────────────────────
 * 96 pieces, each owning 216 slots (20736 / 96 = 216 = 6^3). The pieces
 * split 48 (cube edges) + 24 (tetra faces) + 24 (cube faces). A working
 * grid = (piece, slot) <-> flat, with flat covering [0,20736) exactly. */
#define AM_GRID_PIECES      96u
#define AM_GRID_CUBE_EDGE   48u   /* 8*6 */
#define AM_GRID_TETRA_FACE  24u   /* 3*4*2 */
#define AM_GRID_CUBE_FACE   24u   /* 4*6 */
#define AM_GRID_SLOTS       216u  /* 6^3 = 20736/96 */

/* piece -> group: 0 = cube-edge(48), 1 = tetra-face(24), 2 = cube-face(24). */
static inline int am_grid_group(int piece) {
    if (piece < 0 || piece >= (int)AM_GRID_PIECES) return -1;
    if (piece < (int)AM_GRID_CUBE_EDGE) return 0;
    if (piece < (int)(AM_GRID_CUBE_EDGE + AM_GRID_TETRA_FACE)) return 1;
    return 2;
}

/* (piece, slot) -> flat field address; 0xFFFFFFFF on out-of-range (fail-loud). */
static inline uint32_t am_grid_flat(int piece, uint32_t slot) {
    if (piece < 0 || piece >= (int)AM_GRID_PIECES) return 0xFFFFFFFFu;
    if (slot >= AM_GRID_SLOTS) return 0xFFFFFFFFu;
    return (uint32_t)piece * AM_GRID_SLOTS + slot;
}

/* flat field address -> piece; -1 on out-of-range. */
static inline int am_grid_piece_of(uint32_t flat) {
    if (flat >= AM_FIELD) return -1;
    return (int)(flat / AM_GRID_SLOTS);
}

/* flat field address -> slot within its piece. */
static inline uint32_t am_grid_slot_of(uint32_t flat) {
    if (flat >= AM_FIELD) return 0xFFFFFFFFu;
    return flat % AM_GRID_SLOTS;
}

/* A4 wiring: place the anchor's Metatron address into the 96x216 grid. This
 * is the grid as a live consumer: the anchor gets a (piece, slot) coordinate
 * and the exact flat address back. */
static inline void am_grid_place(const aw_anchor_t *a, int *piece, uint32_t *slot) {
    uint32_t addr = am_metatron_addr(a) % AM_FIELD;
    if (piece) *piece = (int)(addr / AM_GRID_SLOTS);
    if (slot)  *slot  = addr % AM_GRID_SLOTS;
}

/* ── A6: hex address ─────────────────────────────────────────────────────
 * The anchor's 6 frustum directions are the HexTile's 6 ring cells and the
 * centre is cell 6. Each cell is an 8-bit slice of the anchor's REAL identity
 * (its folded pattern), so the packed address carries anchor entropy rather
 * than a near-constant derived from the 52-letter alphabet alone. The
 * existing hex codec (geo_hex_layer.h) packs it: type 2b + xor-diff 4b +
 * centre 8b = 14 bits — a COARSE address by construction, not a hash. */
static inline void am_hex_tile(const aw_anchor_t *a, HexTile *t) {
    if (!a || !t) return;
    /* ring cell i = low byte of direction i's fold, mixed with its weight */
    for (int i = 0; i < 6; i++) {
        uint32_t v = (uint32_t)(a->fold16[i] & 0xFFu);
        v = (v * 31u) + a->dir_letter[i] + a->weight[i];
        t->c[i] = (uint8_t)(v ^ (v >> 8));
    }
    /* centre = low byte of the centre cell's full pattern (8 real bits) */
    uint32_t c = (uint32_t)(a->pattern[AW_CENTER] & 0xFFu);
    c = (c * 31u) + a->letter + a->weight[AW_CENTER];
    t->c[6] = (uint8_t)(c ^ (c >> 8));
}

static inline uint16_t am_hex_address(const aw_anchor_t *a) {
    HexTile t;
    am_hex_tile(a, &t);
    return gpx4_geo_hex_encode_sub(&t);
}

/* A6 roundtrip: decode an address back to (type, centre value). The packed
 * form is an address (lossy on the residual), so the stable fields are the
 * class type and the centre value — assert THOSE round-trip. */
static inline int am_hex_roundtrip(const aw_anchor_t *a,
                                   uint8_t *type_out, uint8_t *center_out) {
    HexTile t;
    am_hex_tile(a, &t);
    uint16_t addr = gpx4_geo_hex_encode_sub(&t);
    GeoHexInfo info = gpx4_geo_hex_decode_sub(addr);
    uint8_t want_type = hex_tile_classify(&t);
    if (type_out)   *type_out = info.tile_type;
    if (center_out) *center_out = info.center_val;
    return (info.tile_type == want_type) && (info.center_val == t.c[6]);
}

/* ── the composed anchor address ─────────────────────────────────────────
 * All three facts in one record: the Metatron field address, its grid
 * coordinate, and the hex address. Consumers that need one of them call the
 * single function above; consumers that want the whole identity use this. */
typedef struct {
    uint32_t metatron_addr;   /* [0, 20736) — A1 */
    uint16_t piece;           /* [0, 96)    — A4 */
    uint16_t slot;            /* [0, 216)   — A4 */
    uint16_t hex_addr;        /* 14-bit packed — A6 */
} am_addr_t;

static inline am_addr_t am_anchor_address(const aw_anchor_t *a) {
    am_addr_t r;
    int piece = 0; uint32_t slot = 0;
    r.metatron_addr = am_metatron_addr(a);
    am_grid_place(a, &piece, &slot);
    r.piece = (uint16_t)piece;
    r.slot  = (uint16_t)slot;
    r.hex_addr = am_hex_address(a);
    return r;
}

#endif /* ANCHOR_METATRON_H */
