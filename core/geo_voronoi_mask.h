/*
 * geo_voronoi_mask.h — Voronoi Pointer Masking + Spotlight + Gravity for DWGLS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * "voronoi roots masks" — mask pointer/seeker to narrow window
 *
 * 24 seeds (E8 roots / fan24 ring) divide 20736 into 24 cells.
 * Pointer = (cell_id, local_offset) — two-part address.
 * Seeker can ONLY expand within cell boundary.
 * Observer sees small-range movement, can't determine true access pattern.
 *
 * NEW: Position-dependent masking (axis + position from geo_box_axes.h)
 * NEW: Spotlight — magnifier window that follows seeker
 * NEW: Gravity — attractor field that bends paths toward centers
 *
 * Cell layout: 24 cells × 864 slots = 20736
 *   864 = 6 × 144 = 6 cubes per cell
 *
 * Dependencies: geo_octant.h, geo_box_axes.h
 */

#ifndef GEO_VORONOI_MASK_H
#define GEO_VORONOI_MASK_H

#include <stdint.h>
#include <math.h>
#include "geo_octant.h"
#include "geo_box_axes.h"

/* ═══════════════ CONSTANTS ═══════════════ */

#define VM_SEEDS        24u      /* 24 origins (E8 roots) */
#define VM_CELLS        VM_SEEDS /* 24 Voronoi cells */
#define VM_SLOTS_PER    864u     /* 20736 / 24 = 864 per cell */
#define VM_FULL         OCT_FULL /* 20736 */
#define VM_CUBES_PER    6u       /* 864 / 144 = 6 cubes per cell */

/* Spotlight / Gravity constants */
#define VM_SPOTLIGHT_RADIUS   64u   /* slots visible in spotlight */
#define VM_GRAVITY_STRENGTH   16    /* fixed-point 1/16 = 0.0625 pull */
#define VM_GRAVITY_RANGE      256u  /* gravity influence radius */

/* ═══════════════ MASKED POINTER (POSITION-AWARE) ═══════════════ */

/*
 * MaskedPointer: two-part address with axis context
 *   axis:    which of 6 axes (GBA_AXIS_*)
 *   cell_id: 0..23 (which Voronoi cell)
 *   local:   0..863 (offset within cell)
 *   position: axis position (floor=0) — hidden identity
 *
 * External observer sees only (axis, local) — small range per cell.
 * cell_id + position is the "mask" — hides true global position.
 */
typedef struct {
    uint8_t  axis;         /* 0..5 (GBA_AXIS_*) */
    uint16_t cell_id;      /* 0..23 */
    uint16_t local;        /* 0..863 */
    uint32_t position;     /* axis position, floor = 0 */
} MaskedPointer;

/* ═══════════════ SPOTLIGHT STATE ═══════════════ */

/*
 * Spotlight: magnifier window around seeker
 *   center:   local offset in cell (0..863)
 *   radius:   visibility window (default 64 slots)
 *   intensity: 0..255 (fade at edges)
 */
typedef struct {
    uint16_t center;
    uint16_t radius;
    uint8_t  intensity;
} Spotlight;

static inline Spotlight vm_spotlight_make(uint16_t center, uint16_t radius) {
    Spotlight s;
    s.center = center % VM_SLOTS_PER;
    s.radius = radius > VM_SPOTLIGHT_RADIUS ? VM_SPOTLIGHT_RADIUS : radius;
    s.intensity = 255;
    return s;
}

static inline int vm_spotlight_contains(Spotlight s, uint16_t local) {
    uint16_t dist = (local > s.center) ? (local - s.center) : (s.center - local);
    return dist <= s.radius;
}

static inline uint8_t vm_spotlight_intensity(Spotlight s, uint16_t local) {
    uint16_t dist = (local > s.center) ? (local - s.center) : (s.center - local);
    if (dist > s.radius) return 0;
    /* linear falloff */
    return (uint8_t)(255 - (255 * dist) / (s.radius + 1));
}

/* ═══════════════ GRAVITY FIELD ═══════════════ */

/*
 * Gravity: attractor at cell centers (seeds)
 * Pulls seeker toward center when within range.
 * Fixed-point: pull = (distance * VM_GRAVITY_STRENGTH) >> 4
 */
static inline int16_t vm_gravity_pull(uint16_t local) {
    uint16_t center = VM_SLOTS_PER / 2;  /* 432 */
    int16_t dist = (int16_t)local - (int16_t)center;
    if (dist < 0) dist = -dist;
    if ((uint16_t)dist > VM_GRAVITY_RANGE) return 0;
    /* pull toward center: negative if local > center, positive if local < center */
    int16_t pull = (dist * VM_GRAVITY_STRENGTH) >> 4;
    return (local > center) ? -pull : pull;
}

/* ═══════════════ CELL MAPPING ═══════════════ */

/* flat → cell: which Voronoi cell owns this address? */
static inline uint32_t vm_cell_of(uint32_t flat) {
    return (flat % VM_FULL) / VM_SLOTS_PER;
}

/* flat → local: offset within cell */
static inline uint32_t vm_local_of(uint32_t flat) {
    return (flat % VM_FULL) % VM_SLOTS_PER;
}

/* GBA_Address → masked pointer (position-aware) */
static inline MaskedPointer vm_mask_gba(GBA_Address a) {
    MaskedPointer p;
    p.axis = (uint8_t)(a.axis % GBA_AXIS_COUNT);
    p.cell_id = (uint16_t)((a.local % VM_FULL) / VM_SLOTS_PER);
    p.local = (uint16_t)((a.local % VM_FULL) % VM_SLOTS_PER);
    p.position = (uint32_t)a.position;
    return p;
}

/* masked pointer → GBA_Address (for inter-box addressing) */
static inline GBA_Address vm_unmask_gba(MaskedPointer p) {
    GBA_Address a;
    a.axis = p.axis % GBA_AXIS_COUNT;
    a.local = (p.cell_id % VM_CELLS) * VM_SLOTS_PER + (p.local % VM_SLOTS_PER);
    a.position = p.position;
    return a;
}

/* masked pointer → flat (single-box) */
static inline uint32_t vm_unmask(MaskedPointer p) {
    return (p.cell_id % VM_CELLS) * VM_SLOTS_PER + (p.local % VM_SLOTS_PER);
}

/* flat → masked pointer (axis defaults to 0) */
static inline MaskedPointer vm_mask(uint32_t flat) {
    MaskedPointer p;
    p.axis = 0;
    p.cell_id = vm_cell_of(flat);
    p.local = vm_local_of(flat);
    p.position = 0;
    return p;
}

/* ═══════════════ MASKED SEEK (WITH SPOTLIGHT + GRAVITY) ═══════════════ */

/*
 * Seek within cell boundary ONLY.
 * Delta is added to local offset, wraps around within cell.
 * cell_id never changes — this is the mask.
 * Spotlight follows seeker. Gravity bends path toward center.
 *
 * Security: observer sees local move 0..863 (small range).
 * True position = cell_id × 864 + local (hidden).
 */
static inline MaskedPointer vm_masked_seek(MaskedPointer p, int32_t delta, Spotlight *spotlight) {
    /* Apply gravity first */
    int16_t grav = vm_gravity_pull(p.local);
    int32_t total_delta = delta + grav;

    int32_t new_local = (int32_t)p.local + total_delta;

    /* Wrap within cell */
    while (new_local < 0) {
        new_local += VM_SLOTS_PER;
    }
    while (new_local >= (int32_t)VM_SLOTS_PER) {
        new_local -= VM_SLOTS_PER;
    }

    p.local = (uint16_t)new_local;

    /* Update spotlight to follow seeker */
    if (spotlight) {
        spotlight->center = p.local;
        spotlight->intensity = 255;
    }

    return p;
}

/*
 * Seek with overflow: if delta exceeds cell boundary,
 * wrap to next/prev cell (masked cross-cell navigation).
 * Position increments on cell boundary crossing.
 */
static inline MaskedPointer vm_masked_seek_overflow(MaskedPointer p, int32_t delta, Spotlight *spotlight) {
    int16_t grav = vm_gravity_pull(p.local);
    int32_t total_delta = delta + grav;

    int32_t new_local = (int32_t)p.local + total_delta;
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

    /* Update position when crossing cells */
    if (cell_delta > 0) {
        p.position += (uint32_t)cell_delta;
    } else if (cell_delta < 0) {
        if ((uint32_t)(-cell_delta) > p.position) {
            p.position = 0;
        } else {
            p.position -= (uint32_t)(-cell_delta);
        }
    }

    p.cell_id = (uint16_t)((p.cell_id + cell_delta + VM_CELLS) % VM_CELLS);

    if (spotlight) {
        spotlight->center = p.local;
        spotlight->intensity = 255;
    }

    return p;
}

/* ═══════════════ POSITION-DEPENDENT MASK ═══════════════ */

/*
 * Position-dependent mask: cell partition shifts with axis position.
 * Different axes see different cell assignments for same flat address.
 * This makes the mask path-dependent.
 */
static inline uint32_t vm_cell_of_axis(uint32_t flat, uint32_t axis, uint32_t position) {
    uint32_t base_cell = vm_cell_of(flat);
    /* Axis-dependent permutation of cell assignment */
    uint32_t perm = (axis * 7 + position * 13) % VM_CELLS;  /* 7,13 coprime to 24 */
    return (base_cell + perm) % VM_CELLS;
}

static inline MaskedPointer vm_mask_gba_position(MaskedPointer p) {
    /* Recompute cell based on axis + position */
    uint32_t flat = vm_unmask(p);
    p.cell_id = vm_cell_of_axis(flat, p.axis, p.position);
    return p;
}

/* ═══════════════ MASKED READ/WRITE ═══════════════ */

static inline uint16_t vm_masked_read(const uint16_t *data, MaskedPointer p) {
    uint32_t flat = vm_unmask(p);
    return data[flat % VM_FULL];
}

static inline void vm_masked_write(uint16_t *data, MaskedPointer p, uint16_t val) {
    uint32_t flat = vm_unmask(p);
    data[flat % VM_FULL] = val;
}

/* Read with spotlight: only visible if in spotlight */
static inline int vm_masked_read_spotlight(const uint16_t *data, MaskedPointer p,
                                            Spotlight s, uint16_t *out) {
    if (!vm_spotlight_contains(s, p.local)) return 0;
    *out = vm_masked_read(data, p);
    return 1;
}

/* ═══════════════ CELL BOUNDARY CHECK ═══════════════ */

static inline int vm_in_cell(MaskedPointer p, uint32_t flat) {
    return vm_cell_of(flat) == p.cell_id;
}

static inline int vm_in_cell_axis(MaskedPointer p, uint32_t flat) {
    return vm_cell_of_axis(flat, p.axis, p.position) == p.cell_id;
}

static inline uint32_t vm_cell_start(uint32_t cell_id) {
    return (cell_id % VM_CELLS) * VM_SLOTS_PER;
}

static inline uint32_t vm_cell_end(uint32_t cell_id) {
    return vm_cell_start(cell_id) + VM_SLOTS_PER - 1;
}

/* ═══════════════ SEED POINTS ═══════════════ */

static inline uint32_t vm_seed_flat(uint32_t cell_id) {
    return (cell_id % VM_CELLS) * VM_SLOTS_PER + VM_SLOTS_PER / 2;
}

static inline MaskedPointer vm_seed_pointer(uint32_t cell_id) {
    return vm_mask(vm_seed_flat(cell_id));
}

/* ═══════════════ STATISTICS ═══════════════ */

typedef struct {
    uint32_t seeks;
    uint32_t overflows;
    uint32_t rejects;
    uint32_t reads;
    uint32_t writes;
    uint32_t gravity_pulls;
    uint32_t spotlight_hits;
    uint32_t spotlight_misses;
} VMaskStats;

static inline void vm_stats_init(VMaskStats *s) {
    if (s) { for (uint32_t i = 0; i < sizeof(*s)/4; i++) ((uint32_t*)s)[i] = 0; }
}

static inline void vm_stats_print(const VMaskStats *s) {
    if (!s) return;
    printf("=== Voronoi Mask Stats ===\n");
    printf("  Seeks:          %u\n", s->seeks);
    printf("  Overflows:      %u\n", s->overflows);
    printf("  Rejects:        %u\n", s->rejects);
    printf("  Reads:          %u\n", s->reads);
    printf("  Writes:         %u\n", s->writes);
    printf("  Gravity Pulls:  %u\n", s->gravity_pulls);
    printf("  Spotlight Hits: %u\n", s->spotlight_hits);
    printf("  Spotlight Miss: %u\n", s->spotlight_misses);
    printf("  Cell size:      %u slots\n", VM_SLOTS_PER);
    printf("  Cells:          %u\n", VM_CELLS);
    printf("==========================\n");
}

/* ═══════════════ VERIFICATION ═══════════════ */

static inline int vm_verify(void) {
    if (VM_CELLS * VM_SLOTS_PER != VM_FULL) return -1;
    if (VM_SLOTS_PER % VM_CUBES_PER != 0) return -2;
    if (VM_SLOTS_PER / VM_CUBES_PER != OCT_CELLS) return -3;

    /* Round-trip: flat → mask → unmask = flat */
    for (uint32_t flat = 0; flat < VM_FULL; flat++) {
        MaskedPointer p = vm_mask(flat);
        uint32_t rt = vm_unmask(p);
        if (rt != flat) return -4;

        if (!vm_in_cell(p, flat)) return -5;
        if (vm_cell_of(flat) >= VM_CELLS) return -6;
        if (vm_local_of(flat) >= VM_SLOTS_PER) return -7;
    }

    /* GBA round-trip */
    for (uint32_t axis = 0; axis < GBA_AXIS_COUNT; axis++) {
        for (uint32_t local = 0; local < VM_FULL; local += 137) {
            GBA_Address a = gba_make(axis, 0, local);
            MaskedPointer p = vm_mask_gba(a);
            GBA_Address rt = vm_unmask_gba(p);
            if (a.axis != rt.axis || a.local != rt.local) return -8;
        }
    }

    /* Seek wrap-around */
    for (uint32_t cell = 0; cell < VM_CELLS; cell++) {
        MaskedPointer p = vm_seed_pointer(cell);
        Spotlight s = vm_spotlight_make(p.local, VM_SPOTLIGHT_RADIUS);
        for (int32_t d = -100; d <= 100; d++) {
            MaskedPointer q = vm_masked_seek(p, d, &s);
            if (q.cell_id != p.cell_id) return -9;
            uint32_t flat_q = vm_unmask(q);
            if (vm_cell_of(flat_q) != cell) return -10;
            if (!vm_spotlight_contains(s, q.local)) return -11;
        }
    }

    /* Position-dependent mask changes cell assignment */
    MaskedPointer p1 = {.axis=0, .cell_id=0, .local=100, .position=0};
    MaskedPointer p2 = {.axis=1, .cell_id=0, .local=100, .position=0};
    if (vm_cell_of_axis(100, 0, 0) == vm_cell_of_axis(100, 1, 0)) return -12;

    /* Gravity pulls toward center */
    int16_t pull_up = vm_gravity_pull(200);  /* below center, pull positive (up) */
    int16_t pull_down = vm_gravity_pull(600); /* above center, pull negative (down) */
    if (pull_up <= 0 || pull_down >= 0) return -13;

    return 0;
}

#endif /* GEO_VORONOI_MASK_H */