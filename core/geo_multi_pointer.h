/* ═══════════════════════════════════════════════════════════════════════════
 * geo_multi_pointer.h — Multi-Pointer Engine
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 6 axes × 20736 slots = 6 different views into the same physical box.
 * Each axis decomposes 20736 differently:
 *   xyz (square): 6912 slots per axis × 3 = 20736
 *   ijk (triangle): 6912 slots per axis × 3 = 20736 (different mapping)
 *
 * A MultiPointer holds all 6 views for one flat slot index.
 * Given a flat slot, you can read it through any axis's coordinate system.
 *
 * Invariants:
 *   For any flat ∈ [0, 20736):
 *     unmask(multi.axis[flat][a]) == flat   for all a ∈ [0,5]
 *     multi.axis[flat] is unique per flat    (bijective)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_MULTI_POINTER_H
#define GEO_MULTI_POINTER_H

#include "geo_box_axes.h"
#include "geo_voronoi_mask.h"

/* MultiPointer: all 6 axis-views for a single flat slot */
typedef struct {
    MaskedPointer views[6];  /* views[GBA_AXIS_X] through views[GBA_AXIS_K] */
    uint64_t      flat;      /* original flat slot (for verification) */
} MultiPointer;

/*
 * Build all 6 axis-views for a flat slot.
 * Each view decomposes the same flat index through that axis's mapping.
 */
static inline MultiPointer mp_from_flat(uint64_t flat) {
    MultiPointer mp;
    mp.flat = flat;
    for (uint32_t a = 0; a < 6; a++) {
        GBA_Address addr = gba_make(a, 0, 0);  /* position=0, local=flat */
        addr.local = (uint32_t)(flat % 20736);
        mp.views[a] = vm_mask_gba(addr);
    }
    return mp;
}

/*
 * Resolve a MaskedPointer back to flat.
 * This is vm_unmask() — included here for API completeness.
 */
static inline uint64_t mp_resolve(MaskedPointer p) {
    return (uint64_t)vm_unmask(p);
}

/*
 * Cross-axis translation: given a pointer on axis_src, produce the
 * equivalent pointer on axis_dst.
 * Both point to the same physical flat slot.
 */
static inline MaskedPointer mp_translate(MaskedPointer p,
                                          uint32_t axis_src,
                                          uint32_t axis_dst) {
    uint64_t flat = mp_resolve(p);
    (void)axis_src;
    GBA_Address addr = gba_make(axis_dst, 0, 0);
    addr.local = (uint32_t)(flat % 20736);
    return vm_mask_gba(addr);
}

/*
 * Verify bijectivity: all 6 views resolve to the same flat slot.
 * Returns 0 on success, non-zero on failure.
 */
static inline int mp_verify(uint64_t flat) {
    MultiPointer mp = mp_from_flat(flat);
    for (uint32_t a = 0; a < 6; a++) {
        uint64_t resolved = mp_resolve(mp.views[a]);
        if (resolved != flat) return (int)(a + 1);
    }
    return 0;
}

/*
 * MultiPointerTable: pre-computed views for all 20736 slots.
 * Usage: build once, use for O(1) lookups in hot paths.
 */
typedef struct {
    MaskedPointer views[20736][6];  /* [flat][axis] */
    uint64_t      flats[20736];    /* verify array */
    int           built;
} MultiPointerTable;

static inline void mp_table_build(MultiPointerTable *t) {
    for (uint64_t f = 0; f < 20736; f++) {
        MultiPointer mp = mp_from_flat(f);
        for (uint32_t a = 0; a < 6; a++)
            t->views[f][a] = mp.views[a];
        t->flats[f] = f;
    }
    t->built = 1;
}

/* O(1) lookup: get any axis view for any flat slot */
static inline MaskedPointer mp_table_lookup(const MultiPointerTable *t,
                                             uint64_t flat, uint32_t axis) {
    return t->views[flat % 20736][axis % 6];
}

/* ═══════════════ SELF-TEST ═══════════════ */

static inline int mp_verify_all(void) {
    for (uint64_t f = 0; f < 20736; f++) {
        int r = mp_verify(f);
        if (r != 0) {
            fprintf(stderr, "mp_verify: FAIL flat=%lu axis=%d\n",
                    (unsigned long)f, r);
            return (int)(f * 10 + r);
        }
    }
    return 0;
}

#endif /* GEO_MULTI_POINTER_H */