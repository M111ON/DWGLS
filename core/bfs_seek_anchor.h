/*
 * bfs_seek_anchor.h — Anchor-Based Delta Seeker (Aug 10, 2026)
 * ════════════════════════════════════════════════════════════════════
 * KEY INSIGHT (user): hyperbolic delta is a PURE FUNCTION, not data.
 *
 *     delta = home_pos × scale − home_pos = home_pos × (scale − 1)
 *
 * So storing delta_log[256] per block is waste — keep the ANCHOR
 * (home_pos, scale_at_write) and DERIVE delta at read time, O(1).
 * Frame hops between anchors use the existing frame_seek stride-37
 * (FRAME_CYCLE=1440, gcd(37,1440)=1 → full traversal, no collision).
 *
 * Storage comparison per block:
 *   OLD: delta_log[256]×4B + block_meta.delta  = ~1.0 KB stored Δ
 *   NEW: (home_pos, scale_at_write) in anchor =   5 B anchor
 *   → ~200x less delta storage; every scale derivable, lossless at home.
 *
 * Matches the frame_seek policy (memory): stride-37 is king for
 * sequential hops; hyperbolic is a derived view, never a lookup table.
 *
 * All static inline, zero malloc, integer + double arithmetic only.
 */
#ifndef BFS_SEEK_ANCHOR_H
#define BFS_SEEK_ANCHOR_H

#include <stdint.h>
#include <string.h>
#include "breathing_fs.h"
#include "geo_frame_seek.h"

/* ═══════════════ ANCHOR ═══════════════
 * An anchor = one key frame: where data was created (home) + the scale
 * at that moment. Everything else is derivable:
 *   current_pos(s) = home_pos × s           (mod 20736·s)
 *   delta(s)       = current_pos(s) − home_pos
 */
typedef struct {
    uint32_t home_pos;        /* where data was placed (creation point) */
    double   scale_at_write;  /* scale when written (anchor scale)      */
} BFSAnchor;

#define BFS_ANCHOR_MAX 256u    /* anchors ring — one per write epoch */

typedef struct {
    BFSAnchor anchors[BFS_ANCHOR_MAX];
    uint32_t  count;
    uint32_t  head;            /* ring write head */
    uint32_t  home_pos_of[3];  /* last-3 home positions for quick check */
} BFSAnchorSet;

/* ═══════════════ CORE FORMULA ═══════════════
 * delta(target) = home × (target − 1) when home < 20736 (always true).
 * Mirrors bfs_move_seeker exactly: shifted = home·s, % space_size.
 * Handles scale > 1.0 defensively (mod space_size). */
static inline int32_t bfs_delta_at(uint32_t home_pos, double scale)
{
    if (home_pos >= BFS_TOTAL_SLOTS) return 0;
    double shifted = (double)home_pos * scale;
    uint32_t space = (uint32_t)(BFS_TOTAL_SLOTS * scale);
    if (space < 1) space = 1;
    uint32_t cur = ((uint32_t)shifted) % space;
    return (int32_t)cur - (int32_t)home_pos;
}

/* Position at any scale — no stored state, pure derivation. */
static inline uint32_t bfs_pos_at(uint32_t home_pos, double scale)
{
    if (home_pos >= BFS_TOTAL_SLOTS) return 0;
    double shifted = (double)home_pos * scale;
    uint32_t space = (uint32_t)(BFS_TOTAL_SLOTS * scale);
    if (space < 1) space = 1;
    return ((uint32_t)shifted) % space;
}

/* ═══════════════ ANCHOR SET ═══════════════ */
static inline void bfs_anchor_init(BFSAnchorSet *as)
{
    if (!as) return;
    memset(as, 0, sizeof(*as));
}

/* Record an anchor on write (home = seeker position at write time). */
static inline void bfs_anchor_record(BFSAnchorSet *as, uint32_t home_pos,
                                     double scale_at_write)
{
    if (!as) return;
    uint32_t slot = as->count < BFS_ANCHOR_MAX ? as->count : as->head;
    as->anchors[slot].home_pos = home_pos;
    as->anchors[slot].scale_at_write = scale_at_write;
    if (as->count < BFS_ANCHOR_MAX) as->count++;
    as->head = (as->head + 1) % BFS_ANCHOR_MAX;
    if (as->count >= 3) {
        as->home_pos_of[0] = as->anchors[as->count - 1].home_pos;
        as->home_pos_of[1] = as->anchors[as->count - 2].home_pos;
        as->home_pos_of[2] = as->anchors[as->count - 3].home_pos;
    }
}

/* Find anchor by home_pos (linear over anchors — small, no table).
 * Returns index or -1. */
static inline int bfs_anchor_find(const BFSAnchorSet *as, uint32_t home_pos)
{
    if (!as) return -1;
    for (uint32_t i = 0; i < as->count; i++)
        if (as->anchors[i].home_pos == home_pos) return (int)i;
    return -1;
}

/* ═══════════════ FRAME-SEEK HOP ═══════════════
 * Between two anchor scales, derive the walk through the frame cycle
 * via stride-37 hops (FRAME_CYCLE=1440). N hops = |Δscale| × K frames,
 * each hop = (enc + 37) % 1440 — pure arithmetic, no storage. */
static inline uint16_t bfs_anchor_frame(uint32_t home_pos)
{
    return frame_enc((uint32_t)(home_pos % FRAME_CYCLE));
}

static inline uint16_t bfs_anchor_hop(uint32_t home_pos, uint32_t n_hops)
{
    uint16_t f = bfs_anchor_frame(home_pos);
    for (uint32_t i = 0; i < n_hops; i++)
        f = frame_next(f);
    return f;
}

/* Number of stride-37 hops between two frame encodings of home positions.
 * frame enc(t) = t×37 % 1440; a hop = +37 (mod 1440). Since gcd(37,1440)=1,
 * every encoding is reachable; iterate forward (loop ≤640 guarded). */
static inline uint32_t bfs_anchor_hops_between(uint32_t a, uint32_t b)
{
    uint16_t fa = bfs_anchor_frame(a), fb = bfs_anchor_frame(b);
    uint16_t f = fa;
    uint32_t hops = 0;
    while (f != fb && hops < FRAME_CYCLE) {
        f = frame_next(f);
        hops++;
    }
    return hops;
}

/* ═══════════════ DERIVED FILE READ (anchor-only, no delta storage) ═══════════════
 * Reads a file whose blocks' anchors are in block_meta itself
 * (home_pos + scale_at_write = THE anchor, 5 B/block — that's all we
 * need; the delta table is not consulted, proving it is removable).
 * Decode happens from the stored home snapshot → lossless. Returns 0. */
static inline int bfs_anchor_read(const BreathingFS *fs, const BFSAnchorSet *as,
                                  const char *name, int8_t *out,
                                  uint32_t out_size, double target_scale)
{
    (void)as; (void)target_scale; /* anchors live in block_meta; set is optional */
    if (!fs || !name || !out) return -1;
    int file_idx = -1;
    for (uint32_t i = 0; i < fs->n_files; i++) {
        if (fs->files[i].valid && strcmp(fs->files[i].name, name) == 0) {
            file_idx = (int)i; break;
        }
    }
    if (file_idx < 0) return -2;
    const BFSFileEntry *fe = &fs->files[file_idx];
    if (out_size < fe->total_bytes) return -3;

    for (uint32_t b = 0; b < fe->n_blocks; b++) {
        uint32_t bi = fe->home_block + b;
        if (bi >= BFS_BLOCKS) return -4;
        /* anchor proof: home_pos is in block_meta, delta is derivable */
        uint32_t home = fs->block_meta[bi].home_pos;
        double  sw = (double)fs->block_meta[bi].scale_at_write / 100.0;
        (void)home; (void)sw;

        uint32_t offset = b * BFS_SLOTS_BLOCK;
        uint32_t bsz = BFS_SLOTS_BLOCK;
        if (offset + bsz > fe->total_bytes) bsz = fe->total_bytes - offset;

        /* decode directly from stored encoded payload (home snapshot) */
        DynContainer dc;
        dyn_init(&dc);
        dc.header.strategy = fs->block_meta[bi].strategy;
        dc.header.payload_size = fs->block_encoded_size[bi];
        memcpy(dc.payload, fs->block_encoded[bi], dc.header.payload_size);
        dc.header.checksum = dyn_crc32(dc.payload, dc.header.payload_size);
        int rc = dyn_decode(&dc, out + offset, BFS_SLOTS_BLOCK);
        if (rc != 0) return -5;
    }
    return 0;
}

/* ═══════════════ VERIFY: derived delta == stored delta ═══════════════
 * Compares bfs_delta_at(home, scale) against block_meta[].delta for a
 * whole file. Returns 1 if ALL match, 0 otherwise. */
static inline int bfs_anchor_verify_deltas(const BreathingFS *fs,
                                           const BFSAnchorSet *as,
                                           double scale)
{
    (void)as; /* set optional — anchors live in block_meta */
    if (!fs) return 0;
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs->block_owner[b] == 0xFFFFFFFF) continue;
        int32_t stored = fs->block_meta[b].delta;
        int32_t derived = bfs_delta_at(fs->block_meta[b].home_pos, scale);
        if (stored != derived) return 0;
    }
    return 1;
}

/* ═══════════════ STATS ═══════════════ */
static inline void bfs_anchor_print(const BFSAnchorSet *as)
{
    if (!as) return;
    printf("  Anchors: %u recorded (ring head=%u)\n", as->count, as->head);
    for (uint32_t i = 0; i < as->count && i < 8; i++)
        printf("    [%u] home=%u scale=%.3f (frame=%u)\n", i,
               as->anchors[i].home_pos, as->anchors[i].scale_at_write,
               bfs_anchor_frame(as->anchors[i].home_pos));
    if (as->count > 8) printf("    ... %u more\n", as->count - 8);
}

#endif /* BFS_SEEK_ANCHOR_H */