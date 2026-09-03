/*
 * bfs_fan24.h — Fan24 Gear Event Format for BreathingFS Scale Log
 * ════════════════════════════════════════════════════════════════════
 * Bridges fan24_gear.h (ring-24 CRT bijection, 8-bit events) into
 * BreathingFS's scale-change logging.
 *
 * OLD: uint32_t delta_log[256] = 16-bit events, 256 cap
 * NEW: FGLogFull fg_log     = 8-bit events, 256 cap, full field
 *
 * Savings: 50% per event (16b → 8b). Same lossless guarantee.
 * RIM mode: 3 bits/event when all Δ ≡ 0 mod 24.
 *
 * All static inline, zero malloc.
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef BFS_FAN24_H
#define BFS_FAN24_H

#include "fan24_gear.h"

/* ── Gear event push (replaces uint32_t delta_log push) ──────────
 * Encodes from→to as 8-bit gear event {q:10b|dc:3b|dx:2b} on the
 * full field [0,20736). Returns 0 ok, -1 overflow, -2 home (Δ==0). */
static inline int bfs_gear_push(FGXLog *g, uint32_t from, uint32_t to) {
    if (!g) return -1;
    return fgx_log_push(g, from, to);
}

/* ── Reconstruct: backward walk from reader's current position ────
 * cur_w: the scale the READER currently stands at.
 * out_w: filled with chain [..., cur_w], length returned.
 * Lossless at ANY entry point. */
static inline uint32_t bfs_gear_reconstruct(const FGXLog *g, uint32_t cur_w,
                                            uint32_t *out_w, uint32_t cap) {
    if (!g || !out_w || cap < g->hdr.n + 1u) return 0;
    return fgx_reconstruct(g, cur_w, out_w, cap);
}

/* ── RIM check: all events are multiples of 24 ─────────────────── */
static inline int bfs_gear_is_rim(const FGXLog *g) {
    if (!g) return 0;
    return fgx_log_is_rim(g);
}

/* ── Event count ───────────────────────────────────────────────── */
static inline uint32_t bfs_gear_count(const FGXLog *g) {
    return g ? g->hdr.n : 0;
}

/* ── Bytes used ────────────────────────────────────────────────── */
static inline uint32_t bfs_gear_bytes(const FGXLog *g) {
    return g ? (uint32_t)(sizeof(FGLogHeader) + g->hdr.n * sizeof(FGGearEv)) : 0;
}

#endif /* BFS_FAN24_H */
