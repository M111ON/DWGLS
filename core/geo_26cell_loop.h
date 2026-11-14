/* geo_26cell_loop.h — 26-Cell Void Loop Cycle (Lo Shu pairs).
 *
 * Lo Shu pairs summing to 10: (1,9),(2,8),(3,7),(4,6). (5,5) is Void (center) -> cut out.
 * Two modes:
 *   SEPARATE (0): exclude (1,9) pair -> 3 directions
 *   EXTENDED (1): include (1,9) -> 4 directions (defines direction)
 *
 * Self-contained: only <stdint.h>.
 */
#ifndef GEO_26CELL_LOOP_H
#define GEO_26CELL_LOOP_H

#include <stdint.h>
#include "geo_26cell.h"

#define LOOP_SEPARATE 0u
#define LOOP_EXTENDED 1u

/* Lo Shu value for a compact cell (0..25): map to {1,2,3,4,6,7,8,9} (skip 5) */
static inline uint8_t loop_cell_value(uint32_t c) {
    static const uint8_t vals[8] = {1,2,3,4,6,7,8,9};
    return vals[c % 8u];
}

static inline uint8_t loop_pair(uint8_t v) { return (uint8_t)(10u - v); }

/* mode-aware value: in SEPARATE mode, (1,9) excluded -> remap 1->2, 9->8 */
static inline uint8_t loop_value_for_mode(uint8_t v, uint32_t mode) {
    if (mode == LOOP_SEPARATE) {
        if (v == 1) return 2;
        if (v == 9) return 8;
    }
    return v;
}

/* next cell following the pair direction (search forward with wrap) */
static inline uint32_t loop_next(uint32_t c, uint32_t mode) {
    uint8_t v = loop_value_for_mode(loop_cell_value(c), mode);
    uint8_t target = loop_pair(v);
    for (uint32_t step = 1; step <= CELL_N; step++) {
        uint32_t ni = (c + step) % CELL_N;
        if (loop_cell_value(ni) == target) return ni;
    }
    return c; /* no match: stay */
}

/* count distinct directions (pairs) used in a mode */
static inline uint32_t loop_num_directions(uint32_t mode) {
    return (mode == LOOP_SEPARATE) ? 3u : 4u;
}

#endif /* GEO_26CELL_LOOP_H */
