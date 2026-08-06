/* ═══════════════════════════════════════════════════════════════════════════
 * geo_cell_addr.h — Cell Coordinate Mapping for the Rail Hub
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Pure O(1) arithmetic layer between the GeoTensorHub and the Fibo Spine:
 *
 *   tensor block index (tensor_offset)
 *       │
 *       │  geo_cell_addr_from_offset()   ← this file
 *       ▼
 *   (gen, face, slot)   — 3D cube address, uint8/uint8/uint16
 *       │
 *       │  geo_cell_addr_to_pipe()       ← this file
 *       ▼
 *   (pipe_id, tick)     — into the 1728 × 12 Fibo Spine slot space
 *       │
 *       │  rdh_key() (in infra/rdh_addr.h)  ← not redefined here, imported
 *       ▼
 *   Cable coordinates feeding jet_bridge_hop() etc.
 *
 * DESIGN CONSTRAINTS:
 *   - No malloc in any path (cold or hot)
 *   - No float / double anywhere
 *   - No compute — only shift / mask / modulus arithmetic
 *   - All functions static inline; header-only
 *
 * CHILD OF SACRED CONSTANTS (see infra/gear_lock.h, infra/fibo_spine.h):
 *   GEAR_GEO_FULL   = 128 × 162             = 20736
 *   FS_SLOTS         = FS_PIPES × TICKS      = 1728 × 12  = 20736 (same number)
 *
 *   So a 14-bit "flat cell id" (0 .. 20735) folds in two complementary ways:
 *     1) cube  address: (gen[3], face[3], slot[8])
 *     2) rail  address: (pipe_id = id % 1728, tick = id / 1728 % 12)
 *   Both views land on the SAME cell — that's the zero-copy guarantee.
 *
 * IMPORTS:
 *   gear_lock.h    — GEAR_GEO_FULL (=20736) base constant
 *   fibo_spine.h   — FS_PIPES, FS_TICKS_PER_CYCLE slot-time constants
 *   geo_cube_addr.h— GeoCubeAddr struct (uint8 gen, uint8 face, uint16 slot)
 *   geo_cell_classify.h — geo_cell_classify (3-bit parity for cell type)
 * ═══════════════════════════════════════════════════════════════════════════ */
#pragma once

#include <stdint.h>

#include "infra/gear_lock.h"     /* GEAR_GEO_FULL        */
#include "infra/fibo_spine.h"    /* FS_PIPES, FS_TICKS_PER_CYCLE */
#include "geo_cube_addr.h"      /* GeoCubeAddr          */
#include "geo_cell_classify.h"   /* geo_cell_classify()  */

/* ── Bit-field layout of the flat cell id ─────────────────────────────
 *   bits 0..2   (3 bits)  → generation   (0..7)
 *   bits 3..5   (3 bits)  → face          (0..7, only 6 used)
 *   bits 6..13  (8 bits)  → slot          (0..20735/64 = cap)
 *   total covered = 14 bits → range [0, 16384) ⊂ [0, 20736)
 * ────────────────────────────────────────────────────────────────── */
#define CELL_GEN_BITS    3u
#define CELL_FACE_BITS   3u
#define CELL_SLOT_BITS   8u
#define CELL_GEN_MASK   ((1u << CELL_GEN_BITS)  - 1u)   /* 0x7 */
#define CELL_FACE_MASK  ((1u << CELL_FACE_BITS) - 1u)   /* 0x7 */
#define CELL_SLOT_MASK  ((1u << CELL_SLOT_BITS) - 1u)   /* 0xFF */
#define CELL_GEN_SHIFT   0u
#define CELL_FACE_SHIFT  (CELL_GEN_BITS)                 /* 3 */
#define CELL_SLOT_SHIFT  (CELL_GEN_BITS + CELL_FACE_BITS) /* 6 */

/* ── Concrete addresses used by callers ─────────────────────────────── */

/* Reuse GeoCubeAddr from geo_cube_addr.h; this just minimizes the static
 * inline surface — the existing geo_cube_addr(g,f,s) sets the cell_type
 * already, but it ALSO initializes w_time (a double). To keep this layer
 * free of all float usage, we produce a uint-only variant here. */
typedef struct {
    uint8_t  generation;   /* 0..7   */
    uint8_t  face;         /* 0..5   */
    uint16_t slot;         /* 0..255 */
    uint8_t  cell_type;    /* 3-bit parity (computed) */
} GeoCellAddr;

/* ═══════════════════════════════════════════════════════════════
   geo_cell_addr_from_offset — tensor_offset → (gen, face, slot)
   ═══════════════════════════════════════════════════════════════
   O(1): three shifts + three masks. No loops. No math libraries.
   The "cell" returned is a 14-bit linear id that folds into both
   the GEAR_GEO_FULL cube grid and the FS_SLOTS rail grid.
   ═══════════════════════════════════════════════════════════════ */
static inline GeoCellAddr geo_cell_addr_from_offset(uint32_t tensor_offset) {
    GeoCellAddr a;
    a.generation = (uint8_t)((tensor_offset >> CELL_GEN_SHIFT)  & CELL_GEN_MASK);
    a.face        = (uint8_t)((tensor_offset >> CELL_FACE_SHIFT) & CELL_FACE_MASK);
    a.slot        = (uint16_t)((tensor_offset >> CELL_SLOT_SHIFT) & CELL_SLOT_MASK);
    /* 3-bit parity identical to geo_cube_addr's computation */
    a.cell_type   = (uint8_t)(((a.generation & 1) << 2)
                            | ((a.face       & 1) << 1)
                            |  (a.slot       & 1));
    return a;
}

/* ═══════════════════════════════════════════════════════════════
   geo_cell_addr_to_pipe — (gen, face, slot) → (pipe_id, tick)
   ═══════════════════════════════════════════════════════════════
   Fold the flat id into the FS_PIPES × FS_TICKS_PER_CYCLE rail space.
   The flat id (3 + 3 + 8 = 14 bits, capped at 20736) is re-derived
   bit-exactly via the masks, so we never need to think about the
   "1-cell-of-wobble" in GEAR_GEO_FULL's reserved range.

       pipe_id = (flat id) % FS_PIPES            → [0, 1727]
       tick    = (flat id) / FS_PIPES            → [0, 11]

   FS_SLOTS = FS_PIPES × FS_TICKS_PER_CYCLE = 1728 × 12 = 20736 = GEAR_GEO_FULL.
   This is the conservation that makes zero-copy work: cell N in cube space is
   tick-(N/1728) on pipe (N%1728) — same physical weight byte.
   ═══════════════════════════════════════════════════════════════ */

/* Two helpers that callers want individually */
static inline uint16_t geo_cell_addr_pipe_id(uint32_t flat_id) {
    return (uint16_t)(flat_id % FS_PIPES);
}

static inline uint8_t geo_cell_addr_tick(uint32_t flat_id) {
    return (uint8_t)((flat_id / FS_PIPES) % FS_TICKS_PER_CYCLE);
}

/* Combined — try once, return both fields */
static inline void geo_cell_addr_to_pipe(GeoCellAddr addr,
                                          uint16_t *pipe_id,
                                          uint8_t  *tick) {
    /* Reconstitute the flat id (3 packing constants + mask) */
    uint32_t flat = ((uint32_t)addr.generation << CELL_GEN_SHIFT)
                  | ((uint32_t)addr.face       << CELL_FACE_SHIFT)
                  | ((uint32_t)addr.slot       << CELL_SLOT_SHIFT);
    /* Wrap into the slot cube — reject if >= GEAR_GEO_FULL */
    if (flat >= GEAR_GEO_FULL) flat = flat % GEAR_GEO_FULL;
    *pipe_id = (uint16_t)(flat % FS_PIPES);
    *tick    = (uint8_t)((flat / FS_PIPES) % FS_TICKS_PER_CYCLE);
}

/* Roundtrip convenience: tensor_offset → (pipe_id, tick) in one shot */
static inline void geo_cell_addr_offset_to_pipe(uint32_t tensor_offset,
                                                  uint16_t *pipe_id,
                                                  uint8_t  *tick) {
    GeoCellAddr a = geo_cell_addr_from_offset(tensor_offset);
    geo_cell_addr_to_pipe(a, pipe_id, tick);
}
