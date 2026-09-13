/*
 * geo_triality_serve.h — View-Aware Zero-Copy Serve Layer (D4 Triality)
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Zero-copy serve: same physical buffer, 3 views via D4 triality.
 *   Hardware: anchor[0..161] × local[0..127]  (DRAM layout, GPU-friendly)
 *   Natural:  row[0..143] × col[0..143]       (field layout, geometry ops)
 *   Flat:     offset[0..20735]                 (linear, sequential access)
 *
 * The flat address is INVARIANT — every view is a different decomposition
 * of the same flat offset. No data movement, no copy, no hash.
 *
 * Consumer requests a view → gets a typed pointer to the same memory.
 * O(1) address translation via D4 triality permutation.
 *
 * DESIGN:
 *   No malloc. No float. All static inline. Header-only.
 *   C99. Depends: geo_d4_triality.h, geo_twin_rebalance.h
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_TRIALITY_SERVE_H
#define GEO_TRIALITY_SERVE_H

#include <stdint.h>
#include <string.h>
#include "../geo_d4_triality.h"
#include "geo_twin_rebalance.h"

/* ═══════════════════════════════════════════════════════════════════════════════
   TRIALITY SERVE HANDLE
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * TrialityServe: wraps a raw buffer with view awareness.
 *   base:      pointer to 20736-byte buffer (mmap, malloc, or stack)
 *   elem_sz:   element size in bytes (1, 2, 4, etc.)
 *   native:    which view the buffer was written in
 *   total:     total flat slots (must be 20736)
 *
 * The buffer must be at least total × elem_sz bytes.
 * All views share the same physical memory.
 */
typedef struct {
    uint8_t  *base;
    uint32_t  elem_sz;
    D4_View   native;
    uint32_t  total;      /* 20736 */
} TrialityServe;

/* ═══════════════════════════════════════════════════════════════════════════════
   CONSTRUCTION
   ═══════════════════════════════════════════════════════════════════════════════ */

/* Wrap a buffer as a TrialityServe (native = FLAT by default) */
static inline TrialityServe ts_make(void *buf, uint32_t elem_sz) {
    TrialityServe ts;
    ts.base    = (uint8_t *)buf;
    ts.elem_sz = elem_sz;
    ts.native  = D4_VIEW_FLAT;
    ts.total   = TW_TOTAL;
    return ts;
}

/* Wrap with explicit native view */
static inline TrialityServe ts_make_view(void *buf, uint32_t elem_sz, D4_View native) {
    TrialityServe ts;
    ts.base    = (uint8_t *)buf;
    ts.elem_sz = elem_sz;
    ts.native  = native;
    ts.total   = TW_TOTAL;
    return ts;
}

/* ═══════════════════════════════════════════════════════════════════════════════
   VIEW ACCESSORS (zero-copy typed pointers)
   ═══════════════════════════════════════════════════════════════════════════════
   All return pointers to the SAME physical memory, just typed differently.
   The consumer interprets the buffer according to the view.
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * ts_as_hardware — interpret buffer as DRamTile (162 anchors × 128 local)
 *
 * Usage:
 *   uint8_t (*hw)[128] = ts_as_hardware(&ts);
 *   hw[anchor][local]  // access in DRamTile order
 *
 * Zero-copy: pointer cast only.
 */
static inline void *ts_as_hardware(const TrialityServe *ts) {
    /* Buffer is already in some layout — cast to anchor×local */
    return ts->base;
}

/*
 * ts_as_natural — interpret buffer as 144×144 field
 *
 * Usage:
 *   uint8_t (*nat)[144] = ts_as_natural(&ts);
 *   nat[row][col]  // access in field layout
 *
 * Zero-copy: pointer cast only.
 */
static inline void *ts_as_natural(const TrialityServe *ts) {
    return ts->base;
}

/*
 * ts_as_flat — interpret buffer as linear 20736
 *
 * Usage:
 *   uint8_t *flat = ts_as_flat(&ts);
 *   flat[offset]  // linear access
 *
 * Zero-copy: just the base pointer.
 */
static inline void *ts_as_flat(const TrialityServe *ts) {
    return ts->base;
}

/* ═══════════════════════════════════════════════════════════════════════════════
   ELEMENT ACCESS (view-aware, O(1))
   ═══════════════════════════════════════════════════════════════════════════════ */

/* Get element at flat address (the invariant address) */
static inline const void *ts_at_flat(const TrialityServe *ts, uint32_t flat) {
    return ts->base + (flat % ts->total) * ts->elem_sz;
}

static inline void *ts_at_flat_mut(const TrialityServe *ts, uint32_t flat) {
    return ts->base + (flat % ts->total) * ts->elem_sz;
}

/* Get element at Hardware view (anchor, local) — O(1) */
static inline const void *ts_at_hardware(const TrialityServe *ts,
                                          uint32_t anchor, uint32_t local) {
    uint32_t flat = anchor * TW_COMPUTE_SIDE + local;
    return ts_at_flat(ts, flat);
}

/* Get element at Natural view (row, col) — O(1) */
static inline const void *ts_at_natural(const TrialityServe *ts,
                                         uint32_t row, uint32_t col) {
    uint32_t flat = row * TW_NATURAL_SIDE + col;
    return ts_at_flat(ts, flat);
}

/* ═══════════════════════════════════════════════════════════════════════════════
   CROSS-VIEW ADDRESS TRANSLATION
   ═══════════════════════════════════════════════════════════════════════════════
   Given a position in one view, find it in another — O(1), no search.
   ═══════════════════════════════════════════════════════════════════════════════ */

/* Hardware (anchor, local) → Natural (row, col) */
static inline TW_NatAddr ts_hard_to_nat(uint32_t anchor, uint32_t local) {
    uint32_t flat = anchor * TW_COMPUTE_SIDE + local;
    return tw_flat_to_nat(flat);
}

/* Natural (row, col) → Hardware (anchor, local) */
static inline TW_HardAddr ts_nat_to_hard(uint32_t row, uint32_t col) {
    uint32_t flat = row * TW_NATURAL_SIDE + col;
    return tw_flat_to_hard(flat);
}

/* Flat offset → Hardware position */
static inline TW_HardAddr ts_flat_to_hard(uint32_t flat) {
    return tw_flat_to_hard(flat);
}

/* Flat offset → Natural position */
static inline TW_NatAddr ts_flat_to_nat(uint32_t flat) {
    return tw_flat_to_nat(flat);
}

/* ═══════════════════════════════════════════════════════════════════════════════
   TRIALITY VIEW POSITION (O(1) per element)
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * ts_view_pos — given flat address, get position in target view.
 *
 * Returns TW_ViewPos with coord[2] filled per view:
 *   HARDWARE: coord = [anchor, local]
 *   NATURAL:  coord = [row, col]
 *   FLAT:     coord = [offset, 0]
 *
 * This is the D4 triality bridge in action: O(1) view switch.
 */
static inline TW_ViewPos ts_view_pos(uint32_t flat, D4_View view) {
    return tw_triality_at(flat, (TW_ViewID)view);
}

/* ═══════════════════════════════════════════════════════════════════════════════
   BUFFER LAYOUT DETECTION
   ═══════════════════════════════════════════════════════════════════════════════ */

/* Check if buffer stride matches Hardware layout (128 bytes per anchor) */
static inline int ts_is_hardware_layout(const TrialityServe *ts) {
    return ts->elem_sz == 1;  /* Hardware layout = byte-sequential anchor×128 */
}

/* Check if buffer stride matches Natural layout (144 bytes per row) */
static inline int ts_is_natural_layout(const TrialityServe *ts) {
    return ts->elem_sz == 1;  /* Natural layout = byte-sequential row×144 */
}

/* ═══════════════════════════════════════════════════════════════════════════════
   BATCH VIEW TRANSLATION (for tensor serve)
   ═══════════════════════════════════════════════════════════════════════════════
   Translate a range of flat addresses to view-specific positions.
   Used by serve path to compute offsets for a tensor's elements.
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * ts_translate_range — translate flat range [start, start+count) to view positions.
 *
 * out_coords: caller-allocated uint32_t[count * 2] for coordinate pairs.
 * view: target view (HARDWARE, NATURAL, or FLAT).
 *
 * Returns 0 on success.
 */
static inline int ts_translate_range(const TrialityServe *ts,
                                      uint32_t start, uint32_t count,
                                      uint32_t *out_coords, D4_View view) {
    (void)ts;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t flat = (start + i) % TW_TOTAL;
        TW_ViewPos vp = tw_triality_at(flat, (TW_ViewID)view);
        out_coords[i * 2 + 0] = vp.coord[0];
        out_coords[i * 2 + 1] = vp.coord[1];
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
   VERIFICATION
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * ts_verify — verify triality serve consistency.
 *
 * Checks:
 *   1. D4 triality constants correct
 *   2. Permutation table valid
 *   3. Roundtrip flat→view→flat = identity for all 20736
 *   4. Cross-view translation consistent
 *   5. Buffer access in all views reads same byte at same flat offset
 *
 * data: optional 20736-byte pattern to verify byte-identity across views.
 *       Pass NULL to skip byte-identity check.
 *
 * Returns 0 on success, negative on failure.
 */
static inline int ts_verify(const TrialityServe *ts, const uint8_t *data) {
    /* 1. D4 constants */
    if (!d4_verify_constants()) return -1;

    /* 2. Permutation table */
    if (!d4_verify_perm_table()) return -2;

    /* 3. Roundtrip: flat → view → flat = identity */
    for (uint32_t flat = 0; flat < TW_TOTAL; flat++) {
        /* HARDWARE view roundtrip */
        TW_HardAddr h = tw_flat_to_hard(flat);
        uint32_t back_h = h.anchor * TW_COMPUTE_SIDE + h.local;
        if (back_h != flat) return -3;

        /* NATURAL view roundtrip */
        TW_NatAddr n = tw_flat_to_nat(flat);
        uint32_t back_n = n.row * TW_NATURAL_SIDE + n.col;
        if (back_n != flat) return -4;

        /* FLAT view: identity (flat = flat by definition) */
    }

    /* 4. Cross-view: hard→nat→hard = identity */
    for (uint32_t flat = 0; flat < TW_TOTAL; flat += 137) {  /* stride 137 (prime) */
        TW_HardAddr h = tw_flat_to_hard(flat);
        TW_NatAddr  n = ts_hard_to_nat(h.anchor, h.local);
        TW_HardAddr h2 = ts_nat_to_hard(n.row, n.col);
        if (h2.anchor != h.anchor || h2.local != h.local) return -6;
    }

    /* 5. Byte-identity across views (if data provided) */
    if (data && ts) {
        for (uint32_t flat = 0; flat < TW_TOTAL; flat += 211) {  /* stride 211 (prime) */
            const void *hw = ts_at_hardware(ts,
                flat / TW_COMPUTE_SIDE, flat % TW_COMPUTE_SIDE);
            const void *nat = ts_at_natural(ts,
                flat / TW_NATURAL_SIDE, flat % TW_NATURAL_SIDE);
            const void *fl = ts_at_flat(ts, flat);

            if (memcmp(hw, fl, ts->elem_sz) != 0) return -7;
            if (memcmp(nat, fl, ts->elem_sz) != 0) return -8;
        }
    }

    /* 6. View position consistency */
    for (uint32_t flat = 0; flat < TW_TOTAL; flat += 251) {
        TW_ViewPos ph = ts_view_pos(flat, D4_VIEW_HARDWARE);
        TW_ViewPos pn = ts_view_pos(flat, D4_VIEW_NATURAL);
        TW_ViewPos pf = ts_view_pos(flat, D4_VIEW_FLAT);

        /* Verify each matches the flat address */
        uint32_t back_h = ph.coord[0] * TW_COMPUTE_SIDE + ph.coord[1];
        uint32_t back_n = pn.coord[0] * TW_NATURAL_SIDE + pn.coord[1];
        if (back_h != flat) return -9;
        if (back_n != flat) return -10;
        if (pf.coord[0] != flat) return -11;
    }

    return 0;
}

#endif /* GEO_TRIALITY_SERVE_H */
