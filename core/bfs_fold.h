/*
 * bfs_fold.h — Explicit fold: compact homes on demand (2026-09-13)
 * ═══════════════════════════════════════════════════════════════════
 * The engine self-compacts homes over ~500 ticks of drift (measured:
 * spread 14932 -> 58, then silent). This operator does the same thing
 * NOW, in one step, for callers that cannot wait for drift.
 *
 * OPERATOR: all used blocks' homes -> compact span [0, n) in block order,
 * deltas reset to 0, scale_at_write = current scale (same invariant as a
 * re-anchor, N blocks at once). Bytes never move (blocks are slots;
 * home_pos is only a number). Reads unaffected (read path indexes blocks,
 * never homes). Planets unaffected (digests cover bytes, not homes).
 * Contraction direction (homes shrink) = free.
 *
 * DELIBERATELY NOT LOGGED to fg_log: gear events feed planet_replay's
 * W-walk; a non-scale event would corrupt that correspondence. The fold
 * is counted (fs->fold_count) — history by counter, not by fake event.
 * A future consumer that needs home history gets its own log, not fg_log.
 *
 * Header-only, int-only, no malloc. Owns nothing (mutates fs + breath).
 */
#ifndef BFS_FOLD_H
#define BFS_FOLD_H

#include <stdint.h>
#include "bfs_breath.h"

/* compact all used homes into [0, n); returns blocks compacted */
static inline uint32_t bfs_fold_compact(BreathingFS *fs, BFSBreath *b) {
    if (!fs) return 0;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        if (fs->block_owner[i] == 0xFFFFFFFF) continue;
        fs->block_meta[i].home_pos = slot;
        fs->block_meta[i].current_pos = slot;
        fs->block_meta[i].delta = 0;
        if (b && b->fs == fs) {
            b->live[i].home_pos = slot;
            b->live[i].scale_at_write = b->cur_scale;
            b->live[i].delta = 0;
        }
        slot++;
    }
    fs->fold_count++;
    return slot;
}

#endif /* BFS_FOLD_H */
