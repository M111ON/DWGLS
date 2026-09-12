/*
 * geo_twin_rebalance.h — Twin Rebalance: 128×162 ↔ 144×144 Address Mapping
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Core equation:
 *   128 × 162  =  144 × 144  =  20736  =  2⁸ × 3⁴
 *   (compute)(twin)(geometry)  (natural)(natural)
 *
 * Twin = rebalance factor 2 between binary and ternary:
 *   128 × 162 = 2⁷ × (2×3⁴) → 2⁸ × 3⁴
 *   144 × 144 = (2⁴×3²) × (2⁴×3²) → 2⁸ × 3⁴
 *
 * Three equivalent views:
 *   Hardware:  anchor[0..161] × local[0..127]  (DRAM layout)
 *   Natural:   row[0..143] × col[0..143]      (field layout)
 *   Flat:      offset[0..20735]                (common address)
 *
 * This header provides:
 *   1. Address mapping between all three views (O(1), no hash)
 *   2. Data redistribution between DRamTile and 144×144 field
 *   3. Verification of lossless roundtrip
 *   4. Composition with iso_hex5 (order-5 generator → order-6 hexagonal)
 *
 * DESIGN:
 *   No malloc. All static inline. Header-only.
 *   C99. Compatible with geo_dram_tile.h, geo_fractal_addr.h, iso_rot90.h (base transpose).
 *
 * DEPENDS: core/geo_dram_tile.h, core/geo_fractal_addr.h
 * ═══════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_TWIN_REBALANCE_H
#define GEO_TWIN_REBALANCE_H

#include <stdint.h>
#include <string.h>
#include "geo_dram_tile.h"
#include "../geo_fractal_addr.h"

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define TW_COMPUTE_SIDE   128u     /* binary side: 2⁷                */
#define TW_GEOM_SIDE      162u     /* ternary side: 2 × 3⁴           */
#define TW_NATURAL_SIDE   144u     /* natural side: 12² = 2⁴ × 3²   */
#define TW_TOTAL          20736u   /* 2⁸ × 3⁴ = 12⁴                 */

/* ═══════════════════════════════════════════════════════════════════════════
   VIEW STRUCTURES
   ═══════════════════════════════════════════════════════════════════════════ */

/* Hardware view: DRamTile layout (128 × 162) */
typedef struct {
    uint32_t anchor;    /* anchor index [0..161] */
    uint32_t local;     /* local offset [0..127] */
} TW_HardAddr;

/* Natural view: 144×144 field */
typedef struct {
    uint32_t row;       /* row [0..143] */
    uint32_t col;       /* col [0..143] */
} TW_NatAddr;

/* ═══════════════════════════════════════════════════════════════════════════
   CORE: FLAT ↔ HARDWARE (DRamTile)
   ═══════════════════════════════════════════════════════════════════════════
   flat = anchor × 128 + local
   ═══════════════════════════════════════════════════════════════════════════ */

static inline TW_HardAddr tw_flat_to_hard(uint32_t flat)
{
    TW_HardAddr h;
    h.anchor = flat / TW_COMPUTE_SIDE;
    h.local  = flat % TW_COMPUTE_SIDE;
    return h;
}

static inline uint32_t tw_hard_to_flat(TW_HardAddr h)
{
    return h.anchor * TW_COMPUTE_SIDE + h.local;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CORE: FLAT ↔ NATURAL (144×144 field)
   ═══════════════════════════════════════════════════════════════════════════
   flat = row × 144 + col
   ═══════════════════════════════════════════════════════════════════════════ */

static inline TW_NatAddr tw_flat_to_nat(uint32_t flat)
{
    TW_NatAddr n;
    n.row = flat / TW_NATURAL_SIDE;
    n.col = flat % TW_NATURAL_SIDE;
    return n;
}

static inline uint32_t tw_nat_to_flat(TW_NatAddr n)
{
    return n.row * TW_NATURAL_SIDE + n.col;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TWIN: HARDWARE ↔ NATURAL (the rebalance)
   ═══════════════════════════════════════════════════════════════════════════
   Compose: hard → flat → nat  and  nat → flat → hard
   ═══════════════════════════════════════════════════════════════════════════ */

static inline TW_NatAddr tw_hard_to_nat(TW_HardAddr h)
{
    return tw_flat_to_nat(tw_hard_to_flat(h));
}

static inline TW_HardAddr tw_nat_to_hard(TW_NatAddr n)
{
    return tw_flat_to_hard(tw_nat_to_flat(n));
}

/* ═══════════════════════════════════════════════════════════════════════════
   TWIN: FULL ADDRESS CONVERSION (flat ↔ both views)
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t tw_addr(uint32_t anchor, uint32_t local)
{
    return anchor * TW_COMPUTE_SIDE + local;
}

static inline TW_HardAddr tw_decompose_hard(uint32_t flat)
{
    return tw_flat_to_hard(flat);
}

static inline TW_NatAddr tw_decompose_nat(uint32_t flat)
{
    return tw_flat_to_nat(flat);
}

/* ═══════════════════════════════════════════════════════════════════════════
   DATA REDISTRIBUTION
   ═══════════════════════════════════════════════════════════════════════════
   DRamTile layout (src):  anchor-sequential
     [anchor0: 128 bytes][anchor1: 128 bytes]...[anchor161: 128 bytes]
     Total: 162 × 128 = 20736 bytes

   Natural layout (dst):   row-major
     [row0: 144 bytes][row1: 144 bytes]...[row143: 144 bytes]
     Total: 144 × 144 = 20736 bytes

   tw_redistribute: src[flat] → dst[flat] for all 20736 bytes
   (flat address is the common ground — same byte at same flat offset)
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * tw_rebalance_to_natural — DRamTile (128×162) → Natural (144×144)
 *
 * src: DRamTile layout [anchor × 128 + local]
 * dst: Natural layout  [row × 144 + col]
 * Both must be TW_TOTAL (20736) bytes.
 *
 * Byte at flat offset F is the same in both views:
 *   dst[F] = src[F]
 *
 * But the INTERPRETATION of F differs:
 *   In DRamTile: F = anchor × 128 + local → data is anchor-sequential
 *   In Natural:  F = row × 144 + col       → data is row-major
 *
 * This function copies bytes so that dst[row][col] = src[anchor][local]
 * where (row, col) and (anchor, local) map to the same flat address.
 */
static inline void tw_rebalance_to_natural(const uint8_t *src, uint8_t *dst)
{
    for (uint32_t anchor = 0; anchor < TW_GEOM_SIDE; anchor++) {
        for (uint32_t local = 0; local < TW_COMPUTE_SIDE; local++) {
            uint32_t flat = tw_addr(anchor, local);
            TW_NatAddr n = tw_flat_to_nat(flat);
            uint32_t dst_off = tw_nat_to_flat(n);
            dst[dst_off] = src[flat];
        }
    }
}

/*
 * tw_rebalance_to_hardware — Natural (144×144) → DRamTile (128×162)
 *
 * src: Natural layout  [row × 144 + col]
 * dst: DRamTile layout [anchor × 128 + local]
 */
static inline void tw_rebalance_to_hardware(const uint8_t *src, uint8_t *dst)
{
    for (uint32_t row = 0; row < TW_NATURAL_SIDE; row++) {
        for (uint32_t col = 0; col < TW_NATURAL_SIDE; col++) {
            uint32_t flat = tw_nat_to_flat((TW_NatAddr){row, col});
            TW_HardAddr h = tw_flat_to_hard(flat);
            uint32_t dst_off = tw_hard_to_flat(h);
            dst[dst_off] = src[flat];
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEMANTIC REBALANCE (the real twin operation)
   ═══════════════════════════════════════════════════════════════════════════
   The key insight: flat address is INVARIANT.
   DRamTile layout: buf[anchor * 128 + local]
   Natural layout:  buf[row * 144 + col]
   Same byte at same flat offset.

   The "twin" is the SEMANTIC transformation:
   - How you INDEX the buffer changes
   - The buffer content does NOT move

   This means twin is a ADDRESS-SPACE TRANSFORM, not a data transform.
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * tw_interpret_as_natural — treat DRamTile buffer as 144×144 field
 *
 * Returns a "view" pointer that interprets the same memory as row×col.
 * Usage:
 *   uint8_t dram_buf[20736];  // in DRamTile order
 *   uint8_t (*field)[144] = tw_interpret_as_natural(dram_buf);
 *   field[row][col]  // access as 144×144 field
 *
 * This is ZERO-COPY — just a pointer cast with proper type.
 */
static inline uint8_t (*tw_interpret_as_natural(uint8_t *buf))[TW_NATURAL_SIDE]
{
    /* flat = row * 144 + col, and buf[flat] is the same byte
     * whether we call it DRamTile or Natural.
     * The cast is valid because C arrays are contiguous. */
    return (uint8_t (*)[TW_NATURAL_SIDE])buf;
}

/*
 * tw_interpret_as_hardware — treat 144×144 buffer as DRamTile
 *
 * Returns pointer that interprets same memory as anchor×local.
 */
static inline uint8_t (*tw_interpret_as_hardware(uint8_t *buf))[TW_COMPUTE_SIDE]
{
    return (uint8_t (*)[TW_COMPUTE_SIDE])buf;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CROSS-VIEW QUERIES
   ═══════════════════════════════════════════════════════════════════════════
   Query data in one view while knowing its position in another.
   ═══════════════════════════════════════════════════════════════════════════ */

/* Given DRamTile position, find its 144×144 field position */
static inline TW_NatAddr tw_where_in_field(uint32_t anchor, uint32_t local)
{
    uint32_t flat = tw_addr(anchor, local);
    return tw_flat_to_nat(flat);
}

/* Given 144×144 field position, find its DRamTile position */
static inline TW_HardAddr tw_where_in_dram(uint32_t row, uint32_t col)
{
    uint32_t flat = tw_nat_to_flat((TW_NatAddr){row, col});
    return tw_flat_to_hard(flat);
}

/* ═══════════════════════════════════════════════════════════════════════════
   COMPOSITION WITH EXISTING SYSTEMS
   ═══════════════════════════════════════════════════════════════════════════ */

/* Compose with DRamTile: anchor → flat → field position */
static inline TW_NatAddr tw_dram_to_field(uint32_t anchor, uint32_t x,
                                           uint32_t y, uint32_t layer)
{
    uint32_t flat = dram_addr(anchor, x, y, layer);
    return tw_flat_to_nat(flat);
}

/* Compose with fractal: (h, x, y) → flat → DRamTile */
static inline TW_HardAddr tw_fractal_to_dram(uint32_t h, uint32_t x, uint32_t y)
{
    uint32_t flat = fractal_to_flat(h, x, y);
    return tw_flat_to_hard(flat);
}

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFICATION
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * tw_verify_bijection — check that flat→hard→flat and flat→nat→flat are identity
 * Returns 0 on success, -1 on failure.
 */
static inline int tw_verify_bijection(void)
{
    for (uint32_t flat = 0; flat < TW_TOTAL; flat++) {
        /* flat → hard → flat */
        TW_HardAddr h = tw_flat_to_hard(flat);
        if (tw_hard_to_flat(h) != flat) return -1;

        /* flat → nat → flat */
        TW_NatAddr n = tw_flat_to_nat(flat);
        if (tw_nat_to_flat(n) != flat) return -2;

        /* hard → nat → hard */
        TW_NatAddr n2 = tw_hard_to_nat(h);
        TW_HardAddr h2 = tw_nat_to_hard(n2);
        if (h2.anchor != h.anchor || h2.local != h.local) return -3;
    }
    return 0;
}

/*
 * tw_verify_bounds — check all addresses in valid range
 * Returns 0 on success.
 */
static inline int tw_verify_bounds(void)
{
    for (uint32_t a = 0; a < TW_GEOM_SIDE; a++) {
        for (uint32_t l = 0; l < TW_COMPUTE_SIDE; l++) {
            uint32_t flat = tw_addr(a, l);
            if (flat >= TW_TOTAL) return -1;
            TW_NatAddr n = tw_flat_to_nat(flat);
            if (n.row >= TW_NATURAL_SIDE || n.col >= TW_NATURAL_SIDE) return -2;
        }
    }
    return 0;
}

/*
 * tw_verify_data_roundtrip — redistribute data and verify lossless
 * src: DRamTile layout → rebalance → Natural layout → rebalance back → compare
 * Returns 0 on success (byte-identical).
 */
static inline int tw_verify_data_roundtrip(const uint8_t *src)
{
    uint8_t natural[TW_TOTAL];
    uint8_t back[TW_TOTAL];

    tw_rebalance_to_natural(src, natural);
    tw_rebalance_to_hardware(natural, back);

    for (uint32_t i = 0; i < TW_TOTAL; i++) {
        if (src[i] != back[i]) return -1;
    }
    return 0;
}

#endif /* GEO_TWIN_REBALANCE_H */
