/*
 * geo_phase_rail.h — Phase Rail: Multi-lane Synchronization
 * ═══════════════════════════════════════════════════════════════
 * 3-lane phase rail with PARK/OPEN/REWIND states.
 *
 * Architecture:
 *   theta[3] → ptr[3] = theta << 2 (4 enc per degree)
 *   Lane state from peer comparison (modular distance)
 *   ParkCondition for source confirmation
 *
 * Sacred constants:
 *   RAIL_LANES = 3
 *   RAIL_RING_SIZE = 1440
 *
 * No malloc. No float. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_PHASE_RAIL_H
#define GEO_PHASE_RAIL_H

#include <stdint.h>
#include <stdbool.h>
#include "geo_rail_ring.h"
#include "geo_rail_sync.h"

#define RAIL_LANES 3

typedef enum { LANE_PARK=0, LANE_OPEN=1, LANE_REWIND=2 } LaneState;

typedef struct {
    uint16_t expected_phase;
    uint8_t  source_lane;
    bool     confirmed;
} ParkCondition;

typedef struct {
    uint16_t     theta[RAIL_LANES];
    uint16_t     ptr[RAIL_LANES];
    LaneState    state[RAIL_LANES];
    ParkCondition park[RAIL_LANES];
    uint8_t      active;
} PhaseRail;

/* angular ptr: θ → Ring index (4 enc per degree) */
static inline uint16_t rail_ptr(uint16_t theta) {
    return theta << 2;
}

/* gate: modular distance → lane state
 * dist=0: PARK (synced)
 * dist>120: REWIND (too far, need to rewind)
 * else: OPEN (diverged but recoverable)
 */
static inline LaneState rail_gate(uint16_t p, uint16_t q) {
    uint16_t dist = rail_angular_dist(p, q);
    if (dist == 0)  return LANE_PARK;
    if (dist > 120) return LANE_REWIND;
    return LANE_OPEN;
}

static inline void rail_init(PhaseRail *r, uint16_t tA, uint16_t tB, uint16_t tC) {
    r->theta[0]=tA; r->theta[1]=tB; r->theta[2]=tC;
    for (int i=0;i<RAIL_LANES;i++) r->ptr[i] = rail_ptr(r->theta[i]);
    r->active = (r->ptr[0]^r->ptr[1]) | (r->ptr[1]^r->ptr[2]);
}

static inline void rail_step(PhaseRail *r, uint16_t step) {
    for (int i=0;i<RAIL_LANES;i++)
        r->theta[i] = (r->theta[i] + step) % 360;

    /* Update ptr after theta changes */
    for (int i=0;i<RAIL_LANES;i++)
        r->ptr[i] = rail_ptr(r->theta[i]);

    int peers[3][2] = {{1,2},{0,2},{0,1}};
    for (int i=0;i<RAIL_LANES;i++) {
        LaneState g = rail_gate(r->theta[peers[i][0]], r->theta[peers[i][1]]);
        if (g == LANE_PARK) {
            ParkCondition *pc = &r->park[i];
            if (!pc->confirmed)
                r->state[i] = LANE_REWIND;
            else if (rail_sync_ready(r->theta[pc->source_lane], pc->expected_phase))
                r->state[i] = LANE_OPEN;
        } else {
            r->state[i] = g;
        }
    }
    r->active = (r->ptr[0]^r->ptr[1]) | (r->ptr[1]^r->ptr[2]);
}

static inline void rail_confirm(PhaseRail *r, uint8_t lane, uint16_t src_phase, uint8_t src_lane) {
    r->park[lane].confirmed     = rail_sync_arriving(r->theta[src_lane], src_phase);
    r->park[lane].expected_phase = src_phase;
    r->park[lane].source_lane   = src_lane;
}

/* Verify phase rail */
static inline int rail_phase_rail_verify(const PhaseRail *r) {
    for (int i = 0; i < RAIL_LANES; i++) {
        if (r->ptr[i] != rail_ptr(r->theta[i])) return -1;
        if (r->theta[i] >= 360) return -2;
    }
    uint8_t expected_active = (r->ptr[0]^r->ptr[1]) | (r->ptr[1]^r->ptr[2]);
    if (r->active != expected_active) return -3;
    return 0;
}

#endif /* GEO_PHASE_RAIL_H */
