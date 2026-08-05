/*
 * geo_spoke_sync.h — Spoke Sync: 6-lane Synchronization for DGLS
 * ═══════════════════════════════════════════════════════════════
 * 6-lane sync for DGLS 6-spoke TRing architecture.
 *
 * Semantics aligned with rail_gate (geo_phase_rail.h):
 *   dist = 0   → LANE_PARK   (synced, exact match)
 *   dist ≤ 120 → LANE_OPEN   (close, recoverable)
 *   dist > 120 → LANE_REWIND (too far, need rewind)
 *
 * active bitmask: non-zero = at least one lane diverged.
 *   active == 0  → all synced
 *   active != 0  → divergence detected → brake/rewind
 *
 * Sacred constants:
 *   SPOKE_SYNC_LANES = 6
 *   SPOKE_SYNC_THRESH = 120 (align with rail_gate REWIND threshold)
 *   SPOKE_ENC_PER_SPOKE = 240
 *
 * No malloc. No float. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_SPOKE_SYNC_H
#define GEO_SPOKE_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include "geo_rail_sync.h"
#include "geo_phase_rail.h"

#define SPOKE_SYNC_LANES 6
#define SPOKE_SYNC_THRESH 120   /* align with rail_gate REWIND threshold */
#define SPOKE_ENC_PER_SPOKE 240

typedef struct {
    uint16_t theta[SPOKE_SYNC_LANES];
    uint8_t  state[SPOKE_SYNC_LANES];
    uint8_t  active;             /* bitmask: 0 = all PARK (synced) */
    uint16_t expected_phase[SPOKE_SYNC_LANES];
    uint8_t  confirmed[SPOKE_SYNC_LANES];
} SpokeSync;

static inline void spoke_sync_init(SpokeSync *ss) {
    for (int i = 0; i < SPOKE_SYNC_LANES; i++) {
        ss->theta[i] = 0;
        ss->state[i] = LANE_PARK;
        ss->expected_phase[i] = 0;
        ss->confirmed[i] = 0;
    }
    ss->active = 0;
}

static inline void spoke_sync_update(SpokeSync *ss, uint16_t enc) {
    uint8_t spoke = (uint8_t)(enc / SPOKE_ENC_PER_SPOKE);
    if (spoke >= SPOKE_SYNC_LANES) spoke = SPOKE_SYNC_LANES - 1;

    ss->theta[spoke] = (uint16_t)((uint32_t)enc % SPOKE_ENC_PER_SPOKE) * 360u / SPOKE_ENC_PER_SPOKE;

    uint8_t left = (spoke + 5u) % 6u;
    uint8_t right = (spoke + 1u) % 6u;

    uint16_t dist_left  = rail_angular_dist(ss->theta[spoke], ss->theta[left]);
    uint16_t dist_right = rail_angular_dist(ss->theta[spoke], ss->theta[right]);

    /* aligned with rail_gate: PARK=0, OPEN≤120, REWIND>120 */
    uint8_t new_state;
    if (dist_left == 0 && dist_right == 0) {
        new_state = LANE_PARK;
    } else if (dist_left <= SPOKE_SYNC_THRESH && dist_right <= SPOKE_SYNC_THRESH) {
        new_state = LANE_OPEN;
    } else {
        new_state = LANE_REWIND;
    }

    /* incremental bitmask update — no loop */
    uint8_t bit = (uint8_t)(1u << spoke);
    ss->active &= ~bit;                        /* clear old */
    ss->active |= (new_state != LANE_PARK) ? bit : 0;  /* set new */
    ss->state[spoke] = new_state;
}

static inline void spoke_sync_rewind(SpokeSync *ss, uint8_t spoke,
                                     uint16_t target_enc) {
    if (spoke >= SPOKE_SYNC_LANES) return;
    uint16_t target_theta = (uint16_t)((uint32_t)target_enc % SPOKE_ENC_PER_SPOKE)
                          * 360u / SPOKE_ENC_PER_SPOKE;
    ss->theta[spoke] = target_theta;
    /* incremental bitmask update — rewind always → PARK */
    uint8_t bit = (uint8_t)(1u << spoke);
    ss->active &= ~bit;
    ss->state[spoke] = LANE_PARK;
}

static inline int spoke_sync_verify(const SpokeSync *ss) {
    for (int i = 0; i < SPOKE_SYNC_LANES; i++) {
        if (ss->theta[i] >= 360) return -1;
        if (ss->state[i] > LANE_REWIND) return -2;
    }
    return 0;
}

#endif /* GEO_SPOKE_SYNC_H */
