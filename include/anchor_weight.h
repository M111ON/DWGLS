/* anchor_weight.h — wire RDH capture INTO the anchor (core/anchor_route.h).
 *
 * Spec (owner, 2026-10-10, restated from the 3x3 drawing):
 *   anchor = 3x3 = 8 outer cells + centre.
 *     each cell holds a 64-bit 0/1 pattern: RDH capture at +offset OVERLAID
 *     (XOR) with RDH capture at -offset ("+-offset ทับ"). 9 patterns per
 *     face (8 outer + centre); the fold of all 9 becomes the centre letter.
 *   centre = one LETTER, all 6 directions must differ, A-Z/a-z (52).
 *     Notation A[##] / a[##]: the letter is base + weight[##]; the PAIR is
 *     case-folded (A and a = pair 0..25, same identity) while each side may
 *     carry its own weight (case bit does not enter the pattern).
 *   Pattern source: RDH 64 -> 16 -> 4 -> 1 (reduced boolean first; 64 raw
 *     kept for the full-fidelity path).
 *   Weight lives FIXED at the anchor (primary chain), branching to the
 *     tombstone per-walk weight (so the same pair may weigh differently on
 *     different walks).  memory #7537.
 *
 * What this file adds on top of anchor_route.h (which already has the
 *   centroid router + MOD-37 placement):
 *   - aw_rdh_pattern(): per cell, RDH(+offset) XOR RDH(-offset) -> 9x64-bit
 *     patterns (8 outer + centre) -> 16->4->1 fold.
 *   - aw_letter(): derive the centre letter from the folded pattern; the
 *     52-letter pool is walked with a stride coprime to 52 so a single
 *     anchor's 6 directions differ and the letter never repeats in one
 *     anchor.  (52 = 4*13; stride 37 is coprime to 52 -> full cycle.)
 *     aw_pair_id() folds case (letter % 26): Aa = same pair.
 *   - aw_weight(): per-cell weight = popcount of that cell's 16-bit fold,
 *     fixed at the anchor; weight[AW_CENTER] is the centre letter's [##].
 *     case-opposite branch keeps the pair id.
 *   - aw_anchor_build(): the whole per-anchor structure, deterministic from
 *     (bytes, len, is_upper) using rdh_capture only (no RNG, no state).
 *
 * Derive-don't-store: the pattern/letters/weights are computed from the
 * anchor's own seed bytes in O(1); nothing is materialised on disk here.
 *
 * Header-only, std-only. Uses rdh_capture (core/rdh_capture.h) for the
 * pattern and anchor_route.h's MOD-37 idea for the letter walk.
 */
#ifndef ANCHOR_WEIGHT_H
#define ANCHOR_WEIGHT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "rdh_capture.h"   /* rdh_capture, RDHConfig, RDH_CAPTURE_144 */

/* ── constants ─────────────────────────────────────────────────────── */
#define AW_OUTER      8u    /* 3x3 minus centre */
#define AW_CELLS      9u    /* 3x3: 8 outer + centre (index AW_CENTER) */
#define AW_CENTER     8u    /* centre cell index */
#define AW_LETTERS    52u   /* A-Z a-z */
#define AW_PAIRS      26u   /* case-folded pairs: Aa = same pair */
#define AW_STRIDE     37u   /* coprime to 52; also the placement stride */
#define AW_OFFSET     13u   /* +-offset magnitude (fixed, arbitrary) */

/* Capture width = a rung of the 64>16>4>1 ladder (owner: "แล้วแต่สะดวก" —
 * small spaces capture narrow, never opening full). Widths are the rungs
 * themselves: 64, 16, 4, 1. A narrow capture runs fewer RDH groups and the
 * fold chain starts at the captured rung; group salts depend only on g, so
 * narrow bits are always a PREFIX of the full 64 (proven in test). */

/* 8 outer slot positions of the 3x3 (skip the centre), row-major */
static const int AW_SLOT_RC[AW_OUTER][2] = {
    {0,0},{0,1},{0,2},
    {1,0},      /* centre (1,1) skipped */ {1,2},
    {2,0},{2,1},{2,2}
};

/* ── per-anchor structure ───────────────────────────────────────────── */
typedef struct {
    uint64_t pattern[AW_CELLS];  /* 64-bit 0/1 pattern per cell (8 outer + centre) */
    uint16_t fold16[AW_CELLS];   /* 64 -> 16 fold */
    uint8_t  fold4[AW_CELLS];    /* 16 ->  4 fold */
    uint8_t  fold1[AW_CELLS];    /*  4 ->  1 fold (final boolean) */
    uint8_t  weight[AW_CELLS];   /* per-cell weight (fixed at anchor);
                                    weight[AW_CENTER] is the centre letter's [##] */
    uint8_t  letter;             /* centre letter index 0..51 */
    uint8_t  dir_letter[6];      /* the 6 directions' letters (all differ) */
    uint8_t  is_upper;           /* case of the centre letter */
    uint8_t  width;              /* capture rung: 64, 16, 4 or 1 */
} aw_anchor_t;

/* 64 -> 16 -> 4 -> 1 fold: OR-reduce halves at each level (a boolean
 * summary that never loses a set bit; owner: "ลดรูปเป็น 64>16>4>1") */
static inline uint16_t aw_fold64_16(uint64_t p) {
    uint16_t o = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t nib = (p >> (i*4)) & 0xF;
        o |= (uint16_t)(nib != 0) << i;
    }
    return o;
}
static inline uint8_t aw_fold16_4(uint16_t p) {
    uint8_t o = 0;
    for (int i = 0; i < 4; i++)
        o |= (uint8_t)(((p >> (i*4)) & 0xF) != 0) << i;
    return o;
}
static inline uint8_t aw_fold4_1(uint8_t p) { return (p != 0) ? 1u : 0u; }

/* rung helpers: groups per width, bit mask per width. Group salts depend
 * only on g (not on the total), so a narrow capture is always a PREFIX of
 * the full 64-bit pattern — space pays only for what it opens. */
static inline int aw_groups_for(int width) {
    if (width == 64) return 8;
    if (width == 16) return 2;
    if (width == 4 || width == 1) return 1;
    return -1;
}

static inline uint64_t aw_width_mask(int width) {
    if (width == 64) return ~0ULL;
    if (width > 0 && width < 64) return (1ULL << (uint64_t)width) - 1ULL;
    return 0ULL;
}

/* one 64-bit RDH capture for a (cell, group, sign): the seed bytes shifted
 * by cell/group salt plus sign*AW_OFFSET (uint8 wrap). Deterministic. */
static inline uint64_t aw_rdh_half_n(const uint8_t *seed, size_t len, int cell,
                                     int sign, int groups) {
    RDHConfig cfg = RDH_CAPTURE_144;
    uint64_t p = 0;
    for (int g = 0; g < groups; g++) {
        /* perturb the seed per (cell, group, sign) without touching the
         * caller's buffer */
        uint8_t tmp[64];
        size_t n = len ? len : 1;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        int off = cell * 31 + g * 7 + sign * (int)AW_OFFSET;
        for (size_t i = 0; i < n; i++)
            tmp[i] = (uint8_t)(seed[i] + off);
        int64_t k = rdh_capture(tmp, n, &cfg);
        uint8_t byte = (uint8_t)(k & 0xFF);
        p |= (uint64_t)byte << (g * 8);
    }
    return p;
}

static inline uint64_t aw_rdh_half(const uint8_t *seed, size_t len, int cell,
                                   int sign) {
    return aw_rdh_half_n(seed, len, cell, sign, 8);
}

/* one cell's pattern at a rung: +offset OVERLAID (XOR) with -offset,
 * masked to the rung width. cell 0..7 = outer, AW_CENTER (8) = centre. */
static inline uint64_t aw_cell_pattern_w(const uint8_t *seed, size_t len,
                                         int cell, int width) {
    int groups = aw_groups_for(width);
    if (groups < 0) return 0;
    uint64_t p = aw_rdh_half_n(seed, len, cell, +1, groups)
               ^ aw_rdh_half_n(seed, len, cell, -1, groups);
    return p & aw_width_mask(width);
}

/* one cell's 64-bit pattern: +offset OVERLAID (XOR) with -offset.
 * cell 0..7 = outer slots, cell AW_CENTER (8) = centre. */
static inline uint64_t aw_rdh_pattern(const uint8_t *seed, size_t len, int cell) {
    return aw_cell_pattern_w(seed, len, cell, 64);
}

/* centre letter from the folded pattern: walk the 52-pool by STRIDE and
 * pick the index the pattern selects. Coprime stride => the 6 directions
 * land on 6 distinct letters for a single anchor. */
static inline uint8_t aw_letter_from(const aw_anchor_t *a, int dir) {
    uint8_t base = a->letter;
    return (uint8_t)((base + (uint32_t)dir * AW_STRIDE) % AW_LETTERS);
}

/* build the whole per-anchor structure from seed bytes + a case bit,
 * capturing DIRECTLY at the rung `width` (64, 16, 4 or 1 — anything else
 * is rejected). dir_seed selects which 6 letters the 6 directions show;
 * they are forced distinct by construction (stride coprime to 52,
 * dir < 6 < 52). Folds start at the captured rung: a 16-wide capture IS
 * the 16 level (zero-extended), and the chain continues 16->4->1, so a
 * small space never pays for bits it does not open. Weight = popcount of
 * the rung value (range scales: 0..16 at 64/16, 0..4 at 4, 0..1 at 1).
 * Returns 0 ok, -1 bad width/args. */
static inline int aw_anchor_build_w(const uint8_t *seed, size_t len,
                                    uint8_t is_upper, int width,
                                    aw_anchor_t *out) {
    if (!out || aw_groups_for(width) < 0) return -1;
    memset(out, 0, sizeof(*out));   /* clear pad bytes: the struct is memcmp-safe */
    out->width = (uint8_t)width;
    for (int s = 0; s < (int)AW_CELLS; s++) {
        uint64_t p = aw_cell_pattern_w(seed, len, s, width);
        out->pattern[s] = p;
        /* fold from the captured rung (narrow values are already their
         * level; the OR-reduce below is identity on them where exact) */
        uint16_t f16 = (width == 64) ? aw_fold64_16(p) : (uint16_t)p;
        out->fold16[s]  = f16;
        out->fold4[s]   = aw_fold16_4(out->fold16[s]);
        out->fold1[s]   = aw_fold4_1(out->fold4[s]);
        /* weight = popcount of the 16-fold (0..16), fixed at the anchor.
         * weight[AW_CENTER] is the centre letter's [##] (same rule). */
        uint16_t v = out->fold16[s];
        uint8_t w = 0;
        while (v) { w += (uint8_t)(v & 1u); v >>= 1; }
        out->weight[s] = w;
    }
    out->is_upper = is_upper ? 1u : 0u;
    /* centre letter chosen from the aggregate of all 9 cells */
    uint32_t agg = 0;
    for (int s = 0; s < (int)AW_CELLS; s++) agg += out->fold16[s];
    out->letter = (uint8_t)((agg * AW_STRIDE) % AW_LETTERS);
    for (int d = 0; d < 6; d++) out->dir_letter[d] = aw_letter_from(out, d);
    return 0;
}

/* full-width build (the original entry point): rung 64. */
static inline void aw_anchor_build(const uint8_t *seed, size_t len,
                                   uint8_t is_upper, aw_anchor_t *out) {
    aw_anchor_build_w(seed, len, is_upper, 64, out);
}

/* case-folded pair id: A and a = pair 0..25, same identity (owner:
 * "Aa = pair, weight ต่างได้"). The case bit never enters the pattern;
 * each side keeps its own weight (is_upper + weight[AW_CENTER]). */
static inline uint8_t aw_pair_id(const aw_anchor_t *a) {
    return (uint8_t)(a->letter % AW_PAIRS);
}

/* route bias from fixed weights: a deterministic integer pull toward the
 * heavier cells, added to the anchor's normal nearest-centroid score.
 * Slot range covers all 9 cells (centre included). Returns a signed bias
 * in [-8, +8] at full width (weights 0..16); at narrow rungs weights are
 * smaller so the pull skews negative — same formula, honest range. */
static inline int aw_weight_bias(const aw_anchor_t *a, int slot) {
    if (!a || slot < 0 || slot >= (int)AW_CELLS) return 0;
    return (int)a->weight[slot] - 8;
}

#endif /* ANCHOR_WEIGHT_H */
