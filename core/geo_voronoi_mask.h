/*
 * geo_voronoi_mask.h — Voronoi Pointer Masking for DWGLS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * "voronoi roots masks" — mask pointer/seeker to narrow window
 *
 * 24 seeds (E8 roots / fan24 ring) divide 20736 into 24 cells.
 * Pointer = (cell_id, local_offset) — two-part address.
 * Seeker can ONLY expand within cell boundary.
 * Observer sees small-range movement, can't determine true access pattern.
 *
 * Cell layout: 24 cells × 864 slots = 20736
 *   864 = 6 × 144 = 6 cubes per cell
 *
 * Dependencies: geo_octant.h
 */

#ifndef GEO_VORONOI_MASK_H
#define GEO_VORONOI_MASK_H

#include <stdint.h>
#include "geo_octant.h"

/* ═══════════════ CONSTANTS ═══════════════ */

#define VM_SEEDS        24u      /* 24 origins (E8 roots) */
#define VM_CELLS        VM_SEEDS /* 24 Voronoi cells */
#define VM_SLOTS_PER    864u     /* 20736 / 24 = 864 per cell */
#define VM_FULL         OCT_FULL /* 20736 */
#define VM_CUBES_PER    6u       /* 864 / 144 = 6 cubes per cell */

/* ═══════════════ MASKED POINTER ═══════════════ */

/*
 * MaskedPointer: two-part address
 *   cell_id: 0..23 (which Voronoi cell)
 *   local:   0..863 (offset within cell)
 *
 * External observer sees only local (small range).
 * cell_id is the "mask" — hides true position.
 */
typedef struct {
    uint16_t cell_id;    /* 0..23 */
    uint16_t local;      /* 0..863 */
} MaskedPointer;

/* ═══════════════ CELL MAPPING ═══════════════ */

/*
 * flat → cell: which Voronoi cell owns this address?
 * Simple partition: cell = flat / 864
 *
 * Geometric meaning: each cell spans 6 consecutive cubes (864 slots).
 * Cell 0: cubes 0-5, Cell 1: cubes 6-11, ..., Cell 23: cubes 138-143
 * (But cubes are 8 per tesseract, so cells cross tesseract boundaries)
 */
static inline uint32_t vm_cell_of(uint32_t flat) {
    return (flat % VM_FULL) / VM_SLOTS_PER;
}

/* flat → local: offset within cell */
static inline uint32_t vm_local_of(uint32_t flat) {
    return (flat % VM_FULL) % VM_SLOTS_PER;
}

/* flat → masked pointer */
static inline MaskedPointer vm_mask(uint32_t flat) {
    MaskedPointer p;
    p.cell_id = vm_cell_of(flat);
    p.local = vm_local_of(flat);
    return p;
}

/* masked pointer → flat */
static inline uint32_t vm_unmask(MaskedPointer p) {
    return (p.cell_id % VM_CELLS) * VM_SLOTS_PER + (p.local % VM_SLOTS_PER);
}

/* ═══════════════ MASKED SEEK ═══════════════ */

/*
 * Seek within cell boundary ONLY.
 * Delta is added to local offset, wraps around within cell.
 * cell_id never changes — this is the mask.
 *
 * Security: observer sees local move 0..863 (small range).
 * True position = cell_id × 864 + local (hidden).
 */
static inline MaskedPointer vm_masked_seek(MaskedPointer p, int32_t delta) {
    p.local = (uint16_t)((p.local + delta + VM_SLOTS_PER) % VM_SLOTS_PER);
    return p;
}

/*
 * Seek with overflow: if delta exceeds cell boundary,
 * wrap to next/prev cell (masked cross-cell navigation).
 */
static inline MaskedPointer vm_masked_seek_overflow(MaskedPointer p, int32_t delta) {
    int32_t new_local = (int32_t)p.local + delta;
    int32_t cell_delta = 0;

    while (new_local < 0) {
        new_local += VM_SLOTS_PER;
        cell_delta--;
    }
    while (new_local >= (int32_t)VM_SLOTS_PER) {
        new_local -= VM_SLOTS_PER;
        cell_delta++;
    }

    p.local = (uint16_t)new_local;
    p.cell_id = (uint16_t)((p.cell_id + cell_delta + VM_CELLS) % VM_CELLS);
    return p;
}

/* ═══════════════ MASKED READ/WRITE ═══════════════ */

/*
 * Read through mask: pointer restricted to cell.
 * data = backing store (20736 slots).
 */
static inline uint16_t vm_masked_read(const uint16_t *data, MaskedPointer p) {
    uint32_t flat = vm_unmask(p);
    return data[flat % VM_FULL];
}

/*
 * Write through mask: pointer restricted to cell.
 */
static inline void vm_masked_write(uint16_t *data, MaskedPointer p, uint16_t val) {
    uint32_t flat = vm_unmask(p);
    data[flat % VM_FULL] = val;
}

/* ═══════════════ CELL BOUNDARY CHECK ═══════════════ */

/*
 * Check if a flat address is within the same cell as the pointer.
 * Used for access control: reject out-of-cell access.
 */
static inline int vm_in_cell(MaskedPointer p, uint32_t flat) {
    return vm_cell_of(flat) == p.cell_id;
}

/*
 * Get cell boundaries: first and last flat address of a cell.
 */
static inline uint32_t vm_cell_start(uint32_t cell_id) {
    return (cell_id % VM_CELLS) * VM_SLOTS_PER;
}

static inline uint32_t vm_cell_end(uint32_t cell_id) {
    return vm_cell_start(cell_id) + VM_SLOTS_PER - 1;
}

/* ═══════════════ SEED POINTS ═══════════════ */

/*
 * 24 seed points (E8 roots / fan24 ring).
 * Each seed is the center of its Voronoi cell.
 * Seed flat address = cell start + center offset.
 *
 * For now, seeds are evenly distributed:
 *   seed[i] = i * 864 + 432 (center of each 864-slot cell)
 *
 * Real implementation would use fan24 E8 root positions.
 */
static inline uint32_t vm_seed_flat(uint32_t cell_id) {
    return (cell_id % VM_CELLS) * VM_SLOTS_PER + VM_SLOTS_PER / 2;
}

static inline MaskedPointer vm_seed_pointer(uint32_t cell_id) {
    return vm_mask(vm_seed_flat(cell_id));
}

/* ═══════════════ STATISTICS ═══════════════ */

typedef struct {
    uint32_t seeks;        /* total seeks */
    uint32_t overflows;    /* seeks that crossed cell boundary */
    uint32_t rejects;      /* out-of-cell access attempts */
    uint32_t reads;        /* masked reads */
    uint32_t writes;       /* masked writes */
} VMaskStats;

static inline void vm_stats_init(VMaskStats *s) {
    if (s) { for (uint32_t i = 0; i < sizeof(*s)/4; i++) ((uint32_t*)s)[i] = 0; }
}

static inline void vm_stats_print(const VMaskStats *s) {
    if (!s) return;
    printf("=== Voronoi Mask Stats ===\n");
    printf("  Seeks:     %u\n", s->seeks);
    printf("  Overflows: %u\n", s->overflows);
    printf("  Rejects:   %u\n", s->rejects);
    printf("  Reads:     %u\n", s->reads);
    printf("  Writes:    %u\n", s->writes);
    printf("  Cell size: %u slots\n", VM_SLOTS_PER);
    printf("  Cells:     %u\n", VM_CELLS);
    printf("==========================\n");
}

/* ═══════════════ VERIFICATION ═══════════════ */

static inline int vm_verify(void) {
    /* Check constant relationships */
    if (VM_CELLS * VM_SLOTS_PER != VM_FULL) return -1;
    if (VM_SLOTS_PER % VM_CUBES_PER != 0) return -2;
    if (VM_SLOTS_PER / VM_CUBES_PER != OCT_CELLS) return -3;

    /* Round-trip: flat → mask → unmask = flat */
    for (uint32_t flat = 0; flat < VM_FULL; flat++) {
        MaskedPointer p = vm_mask(flat);
        uint32_t rt = vm_unmask(p);
        if (rt != flat) return -4;

        /* Cell boundary check */
        if (!vm_in_cell(p, flat)) return -5;
        if (vm_cell_of(flat) >= VM_CELLS) return -6;
        if (vm_local_of(flat) >= VM_SLOTS_PER) return -7;
    }

    /* Seek wrap-around */
    for (uint32_t cell = 0; cell < VM_CELLS; cell++) {
        MaskedPointer p = vm_seed_pointer(cell);
        for (int32_t d = -100; d <= 100; d++) {
            MaskedPointer q = vm_masked_seek(p, d);
            if (q.cell_id != p.cell_id) return -8; /* must stay in cell */
            uint32_t flat_q = vm_unmask(q);
            if (vm_cell_of(flat_q) != cell) return -9;
        }
    }

    return 0;
}

#endif /* GEO_VORONOI_MASK_H */
