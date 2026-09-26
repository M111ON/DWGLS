/* geo_hyper_resolve.h — two-tier resolve: GJ coarse -> HJ fine.
 *
 * GJ points coarse (floor/region over the working set); HJ drills to the
 * exact 4D cell (tower + local + gate). The joint is the proven identity
 * HJ tower == GJ floor: coarse = (pos/span) region, fine = local + gate.
 *
 * Header-only, integer-only. Depends on geo_hyper_seeker.h.
 */
#ifndef GEO_HYPER_RESOLVE_H
#define GEO_HYPER_RESOLVE_H

#include <stdint.h>
#include "geo_hyper_seeker.h"

typedef struct {
    uint32_t tower;   /* coarse: GJ floor / HJ region */
    uint32_t local;   /* fine: tes index inside region */
    uint32_t gate;    /* gate row bound to tower */
} HjCell;

/* drill down: linear pos -> exact cell (GJ floor split + HJ gate bind). */
static inline HjCell hjr_resolve(uint32_t pos, uint32_t span) {
    uint32_t s = (span == HS_MODE36) ? HS_MODE36 : HS_MODE48;
    uint32_t p = pos % HJ_TOTAL;
    HjCell c;
    if (s == HS_MODE36) { c.tower = hj4_tower(p); c.local = hj4_local(p); }
    else                { c.tower = hj3_tower(p); c.local = hj3_local(p); }
    c.gate = hj_tower_gate(c.tower > 2u ? 2u : c.tower);
    return c;
}

/* point back up: exact cell -> linear pos. */
static inline uint32_t hjr_point(HjCell c, uint32_t span) {
    uint32_t s = (span == HS_MODE36) ? HS_MODE36 : HS_MODE48;
    return (c.tower * s + c.local) % HJ_TOTAL;
}

#endif /* GEO_HYPER_RESOLVE_H */
