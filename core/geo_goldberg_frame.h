/* ═══════════════════════════════════════════════════════════════════════════
 * geo_goldberg_frame.h — Goldberg(4,0) frame: T=16, 162 faces x 128 slots
 * ═══════════════════════════════════════════════════════════════════════════
 * WHAT THIS IS: an alternative partition of the same 20736 field
 *   (a naming scheme / view, like triality — NOT a storage layer):
 *   face = flat / 128  (face ∈ [0,162)), local = flat % 128.
 *   12 pentagon faces (canonical numbering: faces 0..11) carry the
 *   dodeca closure; 150 hexagon faces absorb the bulk. Uniform scale:
 *   every face holds exactly 128 slots (decision #814 — duality never
 *   rescales, faces never differ in size).
 *
 * VERIFIED IDENTITIES (not vibes): 12+150=162 · 162*128=20736 ·
 *   Euler 320-480+162=2 · edge incidences (12*5+150*6)/2=480.
 * CORRESPONDENCE (not claim): 162x128 == DRAM_ANCHORS x DRAM_CELLS_PER
 *   (geo_dram_tile.h) — the DRAM layout viewed as Goldberg; cross-checked
 *   in test, neither header includes the other.
 *
 * HONEST NOTES:
 * - geo_param_grid.h Goldberg rows (92/132/192) are ADDRESS-TEMPLATE sizes
 *   (codebook/mask capacities, consumed as such) — Euler does not apply to
 *   them and they are intentionally untouched. Real GP(3,0) = V180/E270/F92
 *   (Euler closes); the "92" in our table is its FACE count, and 92 does
 *   not evenly frame 20736 (225.39/face) — it lives in codec-size context
 *   (snub/RID F=92), never as a field frame.
 * - DEBT (named, not hidden): neighbor topology table (which faces share
 *   edges) is NOT here — needed for face-walking fold. Counts close
 *   without it; walks need it.
 *
 * Header-only, int-only, zero dependencies.
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_GOLDBERG_FRAME_H
#define GEO_GOLDBERG_FRAME_H

#include <stdint.h>

/* ── Goldberg(4,0): T = 4*4+4*0+0*0 = 16 ─────────────────────────── */
#define GP16_T            16u     /* triangulation number            */
#define GP16_FACES        162u    /* 10*T+2                           */
#define GP16_PENT         12u     /* pentagon faces (dodeca closure)  */
#define GP16_HEX          150u    /* hexagon faces (bulk)             */
#define GP16_SLOTS        128u    /* slots per face (uniform, #814)   */
#define GP16_TOTAL        20736u  /* GP16_FACES * GP16_SLOTS           */
#define GP16_EDGES        480u    /* 30*T (incidence-checked)          */
#define GP16_VERTS        320u    /* 20*T (Euler-checked)              */

/* canonical numbering: faces 0..11 are the pentagons (choice, documented) */
static inline int gp16_is_pentagon(uint32_t face) { return face < GP16_PENT; }

/* field <-> frame partition (alternative view of L0 flat) */
static inline uint32_t gp16_face(uint32_t flat) { return flat / GP16_SLOTS; }
static inline uint32_t gp16_local(uint32_t flat) { return flat % GP16_SLOTS; }
static inline uint32_t gp16_flat(uint32_t face, uint32_t local) {
    return face * GP16_SLOTS + local;
}

#endif /* GEO_GOLDBERG_FRAME_H */
