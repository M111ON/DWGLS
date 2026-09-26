/* geo_hyper_seeker.h — viewport pointer over the 4D field (minimap model).
 *
 * The 144-tess is the minimap; the seeker is the viewport:
 * pan = hj jump across towers (regions), zoom = gate entry/transit/exit,
 * home = apex return. Mirrors the BreathingSeeker pattern
 * (position state + moves + view window).
 *
 * Header-only, integer-only. Depends on geo_hyper_jump.h + geo_gidpith_gates.h.
 */
#ifndef GEO_HYPER_SEEKER_H
#define GEO_HYPER_SEEKER_H

#include <stdint.h>
#include "geo_hyper_jump.h"
#include "geo_gidpith_gates.h"

#define HS_MODE48  48u   /* 48-tes x 3 towers */
#define HS_MODE36  36u   /* 36-tes x 4 towers */

typedef struct {
    uint32_t span;    /* 48 or 36 */
    uint32_t towers;  /* 3 or 4 */
    uint32_t tower;   /* current region */
    uint32_t local;   /* tes index inside region */
    uint32_t gate;    /* gate row bound to tower */
} HyperSeeker;

static inline uint32_t hs_pos(const HyperSeeker *s) {
    return s->tower * s->span + s->local;
}

static inline void hs_init(HyperSeeker *s, uint32_t span) {
    s->span = (span == HS_MODE36) ? HS_MODE36 : HS_MODE48;
    s->towers = (s->span == HS_MODE36) ? HJ_TOWERS4 : HJ_TOWERS3;
    s->tower = 0;
    s->local = 0;
    s->gate = hj_tower_gate(0);
}

/* pan: move viewport to next region (jump + mirror). */
static inline void hs_pan(HyperSeeker *s) {
    uint32_t p = hs_pos(s);
    uint32_t q = (s->span == HS_MODE36) ? hj4_jump(p) : hj3_jump(p);
    if (s->span == HS_MODE36) { s->tower = hj4_tower(q); s->local = hj4_local(q); }
    else                      { s->tower = hj3_tower(q); s->local = hj3_local(q); }
    s->gate = hj_tower_gate(s->tower > 2u ? 2u : s->tower);
}

/* home: viewport back to entry pole. */
static inline void hs_home(HyperSeeker *s) {
    s->tower = 0;
    s->local = 0;
    s->gate = hj_tower_gate(0);
}

/* view: visible region = [start, start+count) slots of current tower. */
static inline void hs_view(const HyperSeeker *s, uint32_t *start, uint32_t *count) {
    *start = s->tower * s->span;
    *count = s->span;
}

#endif /* GEO_HYPER_SEEKER_H */
