/* ═══════════════════════════════════════════════════════════════════════════
 * geo_goldberg_nbr.h — GP(4,0) neighbor topology (generated, not hand-made)
 * ═══════════════════════════════════════════════════════════════════════════
 * Adjacency table derived by tools/gen_goldberg_nbr.py from icosa
 * combinatorics (barycentric integer lattice per face, shared points
 * unified by identity, dual neighbors = triangulation adjacency):
 *   - positional numbering (RULE side): faces 0..11 = pentagons at the 12
 *     icosa vertices; 12..101 = edge hexagons (12 + edge*3 + (k-1));
 *     102..161 = face-interior hexagons. Shared with geo_goldberg_frame.h
 *     canonical numbering (faces 0..11 pentagons) — cross-checked in test.
 *   - explicit adjacency (TABLE side): 5 neighbors per pentagon (pad 255),
 *     6 per hexagon; symmetric; incidence 12*5+150*6 = 960 = 2*480.
 * Pentagon isolation (no pent-pent edge) falls out of the construction
 * (shrunk faces never touch) and is re-asserted from the table in test.
 * Cyclic order around faces is NOT stored (sets only) — directed walks
 * that need it are named debt. Coverage (BFS over sets) works now.
 *
 * Header-only, int-only. Table: gp_nbr_table.inc (generated, no hand-edit).
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_GOLDBERG_NBR_H
#define GEO_GOLDBERG_NBR_H

#include <stdint.h>

#define GP_NBR_FACES 162u
#define GP_NBR_PENT  12u
#define GP_NBR_MAXDEG 6u
#define GP_NBR_NONE  255u

static const uint8_t GP_NBR[162][6] = {
#include "gp_nbr_table.inc"
};

/* degree from the table itself (single source of truth) */
static inline uint32_t gp_nbr_count(uint32_t f) {
    uint32_t n = 0;
    if (f >= GP_NBR_FACES) return 0;
    for (uint32_t k = 0; k < GP_NBR_MAXDEG; k++)
        if (GP_NBR[f][k] != GP_NBR_NONE) n++;
    return n;
}

static inline uint32_t gp_nbr_get(uint32_t f, uint32_t k) {
    if (f >= GP_NBR_FACES || k >= GP_NBR_MAXDEG) return GP_NBR_NONE;
    return GP_NBR[f][k];
}

#endif /* GEO_GOLDBERG_NBR_H */
