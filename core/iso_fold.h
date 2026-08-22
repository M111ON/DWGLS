/*
 * iso_fold.h — Fold 1 tesseract (1152) onto 9 DRamTile anchors (9×128)
 * ══════════════════════════════════════════════════════════════════════
 * Structural identity (derived from Core Law 128×162 = 20736 = 144×144):
 *
 *   1 tesseract = 8 cells × 144 slots          = 1152
 *   9 anchors   = 9 × 128 tiles                = 1152
 *   ⇒ 18 tesseracts × 9 anchors = 162 anchors  = GEO_FULL ✅
 *
 * Fold maps (tes, cell, slot) through the flat index onto the hardware
 * decomposition (anchor, hilbert_8x8, layer) — reusing geo_dram_tile.h
 * unchanged. rot90 (iso_rot90.h) acts INSIDE a cell before folding, so
 * the triangle↔square bridge composes across views:
 *
 *   timeline view : 144×144 cells×slots   (KIS scale × slot)
 *   geometry view : 12×12 = (4×4)×(3×3)   (iso_rot90)
 *   hardware view : 162 anchors × 128     (geo_dram_tile)
 *
 * All pure int. No table, no hash, no float.
 */
#ifndef ISO_FOLD_H
#define ISO_FOLD_H

#include <stdint.h>
#include "geo_dram_tile.h"
#include "iso_rot90.h"

#define ISO_TES_CELLS        8u                                   /* cubes */
#define ISO_TES_SLOTS        ((uint32_t)ISO_SLOTS)                /* 144   */
#define ISO_TES_SIZE         (ISO_TES_CELLS * ISO_TES_SLOTS)      /* 1152  */
#define ISO_ANCHORS_PER_TES  (ISO_TES_SIZE / DRAM_CELLS_PER)      /* 9     */
#define ISO_N_TES            (DRAM_FULL / ISO_TES_SIZE)           /* 18    */

/* ── Inverse Hilbert (d2xy) — recovers (x,y) from curve position ── */
static inline void iso_hilbert_inv(uint32_t d, uint32_t *ox, uint32_t *oy) {
    uint32_t x = 0, y = 0;
    for (uint32_t s = 1; s < DRAM_GRID_X; s <<= 1) {
        uint32_t rx = 1u & (d >> 1);
        uint32_t ry = 1u & (d ^ rx);
        if (ry == 0) {
            if (rx == 1) { x = s - 1u - x; y = s - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
        x += s * rx;
        y += s * ry;
        d >>= 2;
    }
    *ox = x;
    *oy = y;
}

/* ── Fold: (tes, cell, slot) → hardware parts ────────────────────── */
typedef struct {
    uint32_t anchor;       /* 0..161 */
    uint32_t x, y;         /* 0..7 hilbert grid */
    uint32_t layer;        /* 0..1 */
} IsoFold;

static inline IsoFold iso_fold(uint32_t tes, uint32_t cell, uint32_t slot) {
    uint32_t g    = tes * ISO_TES_SIZE + cell * ISO_TES_SLOTS + slot;
    IsoFold f;
    f.anchor      = g / DRAM_CELLS_PER;
    uint32_t off  = g % DRAM_CELLS_PER;
    f.layer       = off / (DRAM_GRID_X * DRAM_GRID_Y);
    iso_hilbert_inv(off % (DRAM_GRID_X * DRAM_GRID_Y), &f.x, &f.y);
    return f;
}

/* ── Unfold: hardware parts → flat index (reuses dram_addr) ──────── */
static inline uint32_t iso_unfold(const IsoFold *f) {
    return dram_addr(f->anchor, f->x, f->y, f->layer);
}

#endif /* ISO_FOLD_H */
