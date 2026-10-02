/* geo_hyper_wang.h - shared hyper-cell to Wang timeline adapter.
 *
 * The adapter owns only coordinate conversion. Wang integrity and gate policy
 * remain in geo_frame_seek_wang.h/hyp_fusion.h. One hyper cell gets ten Wang
 * timeline phases, so 144 cells cover FRAME_CYCLE exactly.
 */
#ifndef GEO_HYPER_WANG_H
#define GEO_HYPER_WANG_H

#include <stdint.h>
#include "geo_hyper_resolve.h"
#include "geo_frame_seek.h"

#define HYWANG_PHASES_PER_CELL (FRAME_CYCLE / HJ_TOTAL)

static inline uint16_t hywang_cell_to_timeline(uint32_t cell)
{
    return (uint16_t)((cell % HJ_TOTAL) * HYWANG_PHASES_PER_CELL);
}

static inline int hywang_timeline_to_cell(uint16_t timeline, uint32_t *cell)
{
    if (!cell || timeline >= FRAME_CYCLE ||
        (timeline % HYWANG_PHASES_PER_CELL) != 0u)
        return 0;
    *cell = timeline / HYWANG_PHASES_PER_CELL;
    return *cell < HJ_TOTAL;
}

static inline int hywang_resolve_roundtrip(uint32_t cell, uint32_t span)
{
    HjCell c = hjr_resolve(cell, span);
    return hjr_point(c, span) == (cell % HJ_TOTAL);
}

#endif /* GEO_HYPER_WANG_H */
