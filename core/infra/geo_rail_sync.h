/*
 * geo_rail_sync.h — Rail Synchronization Primitives
 * ═══════════════════════════════════════════════════════════════
 * Modular angular distance for multi-lane phase synchronization.
 *
 * Sacred constants:
 *   RAIL_SYNC_THRESH = 24  (1440/6 = 240 enc per spoke, 10% threshold)
 *   RAIL_DEGREES     = 360 (full circle)
 *
 * No malloc. No float. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_RAIL_SYNC_H
#define GEO_RAIL_SYNC_H

#include <stdint.h>
#include <stdbool.h>

#define RAIL_SYNC_THRESH 24
#define RAIL_DEGREES     360

/* Modular angular distance (0..179) */
static inline uint16_t rail_angular_dist(uint16_t a, uint16_t b) {
    uint16_t d = (a > b) ? (a - b) : (b - a);
    return (d < (RAIL_DEGREES - d)) ? d : (RAIL_DEGREES - d);
}

/* Sync ready: exact match */
static inline bool rail_sync_ready(uint16_t src, uint16_t expected) {
    return rail_angular_dist(src, expected) == 0;
}

/* Sync arriving: within threshold */
static inline bool rail_sync_arriving(uint16_t src, uint16_t expected) {
    return rail_angular_dist(src, expected) < RAIL_SYNC_THRESH;
}

/* Verify sync functions */
static inline int rail_sync_verify(void) {
    if (rail_angular_dist(100, 100) != 0) return -1;
    if (rail_angular_dist(0, 180) != 180) return -2;
    if (rail_angular_dist(180, 0) != 180) return -3;
    if (rail_angular_dist(10, 15) != 5) return -4;
    if (rail_angular_dist(355, 5) != 10) return -5;
    if (!rail_sync_ready(100, 100)) return -6;
    if (rail_sync_ready(100, 101)) return -7;
    if (!rail_sync_arriving(100, 105)) return -8;
    if (rail_sync_arriving(100, 124)) return -9;  /* dist=24 == THRESH */
    return 0;
}

#endif /* GEO_RAIL_SYNC_H */
