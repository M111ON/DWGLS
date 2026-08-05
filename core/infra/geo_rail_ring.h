/*
 * geo_rail_ring.h — Rail Ring: Angular LUT on TRing
 * ═══════════════════════════════════════════════════════════════
 * 3 rings offset by 120° = 480 ticks.
 * RAIL_RING_SIZE = 1440 matches FRAME_CYCLE.
 *
 * ptr = theta << 2  (4 enc per degree, no aliasing)
 *
 * Sacred constants:
 *   RAIL_RING_SIZE    = 1440  (matches FRAME_CYCLE)
 *   RAIL_FRAME_STRIDE = 37    (gcd(37,1440)=1)
 *   RAIL_FRAME_CYCLE  = 1440  (matches FRAME_CYCLE)
 *
 * No malloc. No float. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_RAIL_RING_H
#define GEO_RAIL_RING_H

#include <stdint.h>

#define RAIL_RING_SIZE    1440u
#define RAIL_FRAME_STRIDE    37u
#define RAIL_FRAME_CYCLE  1440u

typedef struct {
    uint16_t enc;      /* TRing enc (0..1439) */
    uint8_t  zone;     /* 0..23 */
    uint8_t  slot;     /* 0..59 */
} RailRingEntry;

typedef struct {
    RailRingEntry A[RAIL_RING_SIZE];
    RailRingEntry B[RAIL_RING_SIZE];
    RailRingEntry C[RAIL_RING_SIZE];
} RailRing;

/* Build ring from TRing stride-37 walk, offset 120° apart */
static inline void rail_ring_build(RailRing *r) {
    for (uint16_t i = 0; i < RAIL_RING_SIZE; i++) {
        uint16_t base = (uint16_t)((i * RAIL_FRAME_STRIDE) % RAIL_FRAME_CYCLE);
        r->A[i].enc  = base;
        r->B[i].enc  = (base + 480)  % RAIL_FRAME_CYCLE;
        r->C[i].enc  = (base + 960)  % RAIL_FRAME_CYCLE;
        for (int l = 0; l < 3; l++) {
            RailRingEntry *e = (l==0)?&r->A[i]:(l==1)?&r->B[i]:&r->C[i];
            e->zone = (uint8_t)(e->enc / 60);
            e->slot = (uint8_t)(e->enc % 60);
        }
    }
}

/* Verify ring coverage: all 1440 enc positions visited exactly once */
static inline int rail_ring_verify(const RailRing *r) {
    uint8_t seen[1440] = {0};
    for (uint16_t i = 0; i < RAIL_RING_SIZE; i++) {
        if (r->A[i].enc >= 1440) return -1;
        if (seen[r->A[i].enc]) return -2;
        seen[r->A[i].enc] = 1;
    }
    for (int i = 0; i < 1440; i++) {
        if (!seen[i]) return -3;
    }
    return 0;
}

#endif /* GEO_RAIL_RING_H */
