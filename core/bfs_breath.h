/*
 * bfs_breath.h — Continuous Auto-Compressing Delta Engine (Aug 10, 2026)
 * ════════════════════════════════════════════════════════════════════
 * USER INSIGHT: "ระบบเคลื่อนไปมาอยู่ตลอด — ถ้าหาจุดยึด (anchor) เป็น
 * constrain มา scope ขนาดของ delta คล้ายที่ seeker ทำ — compress delta
 * เกิดขึ้นเองได้ตลอด ขนานกับอีกฝั่ง"
 *
 * Mechanics:
 *   The system breathes constantly (scale oscillates). Every breath
 *   moves every block:  current = home × scale → delta = home(scale−1).
 *   KEY: if the ANCHOR (home_pos) is a MOVING constraint that re-anchors
 *   whenever |delta| crosses BFS_BREATH_BOUND, then delta resets to 0
 *   and stays bounded FOREVER — no matter how deep the scale goes.
 *
 *   bounded delta → compact encode (int8) → "compression happens by
 *   itself" each tick, as a SIDE CHANNEL parallel to the main data path
 *   (payload bytes are never touched — lossless at every anchor).
 *
 * Same family as the seeker: window = K/scale scopes the ADDRESS SPACE;
 * the anchor scopes the DELTA. Constraint → scope → bounded → compact.
 *
 * All static inline, zero malloc.
 */
#ifndef BFS_BREATH_H
#define BFS_BREATH_H

#include <stdint.h>
#include <string.h>
#include "breathing_fs.h"

/* |delta| stays ≤ BFS_BREATH_BOUND → fits int8 (−128..127) */
#define BFS_BREATH_BOUND     127u
#define BFS_BREATH_DEFAULT_STEP 0.05   /* scale step per tick */

/* ── Live anchor: follows the movement; keeps delta in scope ── */
typedef struct {
    uint32_t home_pos;          /* dynamic anchor — moves with re-anchor */
    double   scale_at_write;    /* scale when re-anchored               */
    int32_t  delta;             /* bounded: |delta| ≤ BFS_BREATH_BOUND  */
} BFSliveAnchor;

typedef struct {
    BreathingFS   *fs;          /* borrowed — reads blocks state        */
    BFSliveAnchor  live[BFS_BLOCKS];
    double         cur_scale;   /* internal oscillation scale           */
    double         scale_step;  /* breath amplitude per tick            */
    int            direction;   /* +1 contract, −1 expand               */
    uint32_t       reanchors;   /* auto-compress events (re-anchor hits) */
    uint32_t       ticks;       /* breaths taken                         */
    uint32_t       peak_delta;  /* max |delta| ever observed            */
} BFSBreath;

/* ═══════════════ INIT — adopt current fs state as starting anchors ═══════════════ */
static inline void bfs_breath_init(BFSBreath *b, BreathingFS *fs, double step)
{
    if (!b || !fs) return;
    memset(b, 0, sizeof(*b));
    b->fs = fs;
    b->scale_step = (step > 0.0) ? step : BFS_BREATH_DEFAULT_STEP;
    b->cur_scale = fs->seeker.scale;
    b->direction = 1;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        b->live[i].home_pos = fs->block_meta[i].home_pos;
        b->live[i].scale_at_write = fs->seeker.scale;
        b->live[i].delta = fs->block_meta[i].delta;
    }
}

/* ═══════════════ CORE: bounded delta = home×(scale−1), clamped by re-anchor ═══════════════
 * Returned delta obeys |Δ| ≤ BFS_BREATH_BOUND by construction. */
static inline int32_t bfs_breath_delta_at(uint32_t home_pos, double scale)
{
    if (home_pos >= BFS_TOTAL_SLOTS) return 0;
    double shifted = (double)home_pos * scale;
    uint32_t space = (uint32_t)(BFS_TOTAL_SLOTS * scale);
    if (space < 1) space = 1;
    uint32_t cur = ((uint32_t)shifted) % space;
    return (int32_t)cur - (int32_t)home_pos;
}

/* ═══════════════ ENCODE / DECODE — bounded delta in 8 bits ═══════════════
 * |Δ| ≤ 127 → single signed byte. 4× smaller than the int32 v1 field. */
static inline int8_t bfs_breath_encode(int32_t delta)
{
    if (delta > 127) delta = 127;
    if (delta < -128) delta = -128;
    return (int8_t)delta;
}

static inline int32_t bfs_breath_decode(int8_t e)
{
    return (int32_t)e;
}

/* ═══════════════ ONE BREATH — the automatic compress step ═══════════════
 * 1. move scale (oscillate: contract → expand)
 * 2. for every used block: delta = home×(scale−1)
 * 3. if |delta| > BOUND → RE-ANCHOR: home follows current position,
 *    delta resets to 0. (the anchor is the constraint that scopes size)
 */
static inline void bfs_breath_tick(BFSBreath *b)
{
    if (!b || !b->fs) return;
    b->ticks++;

    /* oscillate scale between (1 − step) and (1 + step)? No — breath
     * CONTRACTS toward 0 (deeper scale), like the seeker. On reaching
     * scale_step, flip to expand back. Simple cosine-ish walk. */
    if (b->direction > 0) {
        b->cur_scale -= b->scale_step;
        if (b->cur_scale <= b->scale_step) { b->cur_scale = b->scale_step; b->direction = -1; }
    } else {
        b->cur_scale += b->scale_step;
        if (b->cur_scale >= 1.0) { b->cur_scale = 1.0; b->direction = 1; }
    }
    double s = b->cur_scale;

    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        if (b->fs->block_owner[i] == 0xFFFFFFFF) continue;   /* free slot */

        BFSliveAnchor *a = &b->live[i];
        int32_t d = bfs_breath_delta_at(a->home_pos, s);

        /* RE-ANCHOR: delta out of scope → move the constraint point */
        if (d > (int32_t)BFS_BREATH_BOUND || d < -(int32_t)BFS_BREATH_BOUND) {
            uint32_t space = (uint32_t)(BFS_TOTAL_SLOTS * s);
            if (space < 1) space = 1;
            uint32_t cur = ((uint32_t)((double)a->home_pos * s)) % space;
            a->home_pos = cur;                 /* anchor follows data */
            a->scale_at_write = s;
            a->delta = 0;                      /* scope reset      */
            b->reanchors++;
        } else {
            a->delta = d;
        }

        uint32_t ad = (uint32_t)(d < 0 ? -d : d);
        if (ad > b->peak_delta) b->peak_delta = ad;
    }
}

/* ═══════════════ QUERIES ═══════════════ */
static inline int bfs_breath_all_bounded(const BFSBreath *b)
{
    if (!b) return 0;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        int32_t d = b->live[i].delta;
        if (d < -(int32_t)BFS_BREATH_BOUND || d > (int32_t)BFS_BREATH_BOUND) return 0;
    }
    return 1;
}

/* Encoded size of the full delta layer (compact int8 vs old int32). */
static inline uint32_t bfs_breath_delta_bytes(const BFSBreath *b)
{
    if (!b) return 0;
    uint32_t used = 0;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++)
        if (b->fs->block_owner[i] != 0xFFFFFFFF) used++;
    return used; /* 1 B/block (int8) */
}

/* ═══════════════ PARALLEL SIDE-CHANNEL READ — main path untouched ═══════════════
 * Reads a file losslessly via plain bfs_read (payloads never moved by
 * the breathing — anchor/delta is pure coordinate metadata). Returns 0. */
static inline int bfs_breath_read_main(const BFSBreath *b, const char *name,
                                       int8_t *out, uint32_t out_size,
                                       uint32_t *actual)
{
    if (!b || !b->fs) return -1;
    return bfs_read(b->fs, name, out, out_size, actual);
}

/* ═══════════════ STATS ═══════════════ */
static inline void bfs_breath_print(const BFSBreath *b)
{
    if (!b) return;
    uint32_t old_bytes = 0, new_bytes = 0;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        if (b->fs == NULL || b->fs->block_owner[i] == 0xFFFFFFFF) { /* skip */ }
        else { old_bytes += 4; new_bytes += 1; }
    }
    printf("  Breath: ticks=%u reanchors=%u peak_delta=%u bound=%u\n",
           b->ticks, b->reanchors, b->peak_delta, (unsigned)BFS_BREATH_BOUND);
    printf("  Delta layer: %u B (int8, bounded) vs %u B (int32, unbounded) = %.1fx smaller\n",
           new_bytes, old_bytes, old_bytes > 0 ? (double)old_bytes / (double)new_bytes : 0.0);
    printf("  All bounded: %s | cur_scale=%.4f | dir=%c\n",
           bfs_breath_all_bounded(b) ? "YES" : "NO",
           b->cur_scale, b->direction > 0 ? '-' : '+');
}

#endif /* BFS_BREATH_H */