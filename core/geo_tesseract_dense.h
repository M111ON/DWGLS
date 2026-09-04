/*
 * geo_tesseract_dense.h — Dense Tesseract: 1 tesseract, deterministic
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * "มันแค่ tesseract อยู่เฉยๆ อันเดียว" — user design
 *
 * 1 tesseract = 8 cubes × 144 slots = 1152 slots
 * 4 cubes are "valid" (zero-sum ≤ 1), 4 are "antipodal" (derived)
 * Store only valid side → bipolar 1/2 compression
 *
 * Dependencies: geo_octant.h
 */

#ifndef GEO_TESSERACT_DENSE_H
#define GEO_TESSERACT_DENSE_H

#include "geo_octant.h"

/* ═══════════════ CONSTANTS ═══════════════ */

#define TD_VALID_CUBES   4u   /* zero-sum ≤ 1: cubes {0,1,2,4} */
#define TD_SLOTS_PER     OCT_CELLS   /* 144 per cube */
#define TD_TOTAL         OCT_PER_TESS /* 1152 = 8 × 144 */
#define TD_HALF          (TD_VALID_CUBES * TD_SLOTS_PER) /* 576 = store half */

/* ═══════════════ TESSERACT STRUCT ═══════════════ */

/*
 * DenseTesseract: 1152 slots = 8 cubes × 144
 *
 * data[] = flat array, addressed by (cube × 144 + slot)
 * valid_mask = bitmask of which cubes are stored (4 valid cubes)
 *
 * Bipolar: store only valid cubes {0,1,2,4}
 * Antipodal cubes {7,6,5,3} are derived on read
 */
typedef struct {
    uint16_t data[TD_TOTAL];       /* 1152 slots */
    uint8_t  valid_mask;           /* bitmap: bit=cube stored */
} DenseTesseract;

/* ═══════════════ BASIC OPERATIONS ═══════════════ */

/* Initialize: zero all slots, mark valid cubes */
static inline void td_init(DenseTesseract *t) {
    for (uint32_t i = 0; i < TD_TOTAL; i++) t->data[i] = 0;
    t->valid_mask = 0;
    for (uint32_t c = 0; c < OCT_CUBES; c++) {
        if (oct_is_valid(c)) t->valid_mask |= (1u << c);
    }
}

/* Write slot in a specific cube */
static inline void td_write(DenseTesseract *t, uint32_t cube, uint32_t slot, uint16_t val) {
    t->data[cube * TD_SLOTS_PER + (slot % TD_SLOTS_PER)] = val;
}

/* Read slot from a specific cube */
static inline uint16_t td_read(DenseTesseract *t, uint32_t cube, uint32_t slot) {
    return t->data[cube * TD_SLOTS_PER + (slot % TD_SLOTS_PER)];
}

/* ═══════════════ BIPOLAR READ ═══════════════ */

/*
 * Bipolar read: given ANY cube index, return value.
 * If cube is valid (0,1,2,4): direct read
 * If cube is antipodal (3,5,6,7): derive from valid partner
 *
 * Derivation: for now, identity (same value at antipodal position)
 * This is the "store half, compute half" compression.
 * Real derivation will use limacon/geometric transform.
 */
static inline uint16_t td_bipolar_read(DenseTesseract *t, uint32_t cube, uint32_t slot) {
    uint32_t c = cube % OCT_CUBES;
    uint32_t s = slot % TD_SLOTS_PER;
    if (oct_is_valid(c)) {
        return td_read(t, c, s);
    } else {
        /* Antipodal: read from valid partner */
        uint32_t partner = oct_tetra_of(c);
        return td_read(t, partner, s);
    }
}

/* ═══════════════ WRITE WITH ANTIPODAL SYNC ═══════════════ */

/*
 * Write to valid cube, sync antipodal partner.
 * If writing to valid cube → also update antipodal slot
 * If writing to invalid cube → redirect to valid partner
 */
static inline void td_bipolar_write(DenseTesseract *t, uint32_t cube, uint32_t slot, uint16_t val) {
    uint32_t c = cube % OCT_CUBES;
    uint32_t s = slot % TD_SLOTS_PER;
    if (oct_is_valid(c)) {
        td_write(t, c, s, val);
        /* Sync antipodal partner */
        uint32_t anti = oct_antipode_cube(c);
        td_write(t, anti, s, val);
    } else {
        /* Redirect to valid partner */
        uint32_t partner = oct_tetra_of(c);
        td_write(t, partner, s, val);
    }
}

/* ═══════════════ FLAT ADDRESSING ═══════════════ */

/* Write to flat address (decompose to cube+slot, then bipolar write) */
static inline void td_flat_write(DenseTesseract *tess_id, uint32_t flat, uint16_t val) {
    uint32_t cube = oct_cube_of(flat);
    uint32_t slot = oct_slot_of(flat);
    td_bipolar_write(tess_id, cube, slot, val);
}

/* Read from flat address (bipolar read) */
static inline uint16_t td_flat_read(DenseTesseract *tess_id, uint32_t flat) {
    uint32_t cube = oct_cube_of(flat);
    uint32_t slot = oct_slot_of(flat);
    return td_bipolar_read(tess_id, cube, slot);
}

/* ═══════════════ STORAGE SIZE ═══════════════ */

/* Storage footprint: valid cubes only */
static inline uint32_t td_storage_size(void) {
    return TD_HALF * sizeof(uint16_t); /* 576 × 2 = 1152 bytes */
}

/* Full footprint (all 8 cubes) */
static inline uint32_t td_full_size(void) {
    return TD_TOTAL * sizeof(uint16_t); /* 1152 × 2 = 2304 bytes */
}

/* ═══════════════ VERIFICATION ═══════════════ */

/* Verify: write all valid cubes, bipolar read matches */
static inline int td_verify(DenseTesseract *t) {
    /* Write pattern to all valid cubes */
    for (uint32_t c = 0; c < OCT_CUBES; c++) {
        if (!oct_is_valid(c)) continue;
        for (uint32_t s = 0; s < TD_SLOTS_PER; s++) {
            uint16_t val = (uint16_t)(c * TD_SLOTS_PER + s);
            td_write(t, c, s, val);
        }
    }

    /* Bipolar read should return same values */
    for (uint32_t c = 0; c < OCT_CUBES; c++) {
        for (uint32_t s = 0; s < TD_SLOTS_PER; s++) {
            uint16_t got = td_bipolar_read(t, c, s);
            if (oct_is_valid(c)) {
                uint16_t expected = (uint16_t)(c * TD_SLOTS_PER + s);
                if (got != expected) return 0;
            } else {
                /* Antipodal: should match valid partner */
                uint32_t partner = oct_tetra_of(c);
                uint16_t expected = (uint16_t)(partner * TD_SLOTS_PER + s);
                if (got != expected) return 0;
            }
        }
    }

    /* Flat address round-trip */
    for (uint32_t flat = 0; flat < TD_TOTAL; flat++) {
        uint16_t val = (uint16_t)(flat + 0xA000);
        td_flat_write(t, flat, val);
        uint16_t got = td_flat_read(t, flat);
        if (got != val) return 0;
    }

    return 1;
}

#endif /* GEO_TESSERACT_DENSE_H */
