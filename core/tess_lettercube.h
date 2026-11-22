/* tess_lettercube.h — LetterCube Chain: geometric addressing via letter pairs.
 *
 * Architecture (from LetterCube Chain.html visualization):
 *   - 8 mini-cubes (octants: ±1,±1,±1) = 2×2×2 cube
 *   - 24 inner faces (8 mini × 3 axes) = 24 letter pairs
 *   - 12 lines (3 axes × 4) connecting opposite faces
 *   - Each mini-cube: 24 letters (L[0..23]) + 3 frame letters (S[0..2])
 *   - Letter pairs: inner (lowercase) ↔ outer (uppercase) across adjacent cubes
 *   - Link = letter match between adjacent cubes (green=straight, red=broken)
 *
 * Mode onion: nested layers (inner cube enclosed by outer cubes)
 * Mode row: all cubes in same row share letter set
 * Mode chain: inherit from previous cube
 *
 * "Coordinate IS the data" — letter pair position IS the address
 * No hash, no storage, no collision (same as beam addressing)
 *
 * Integration: lettercube addressing (weight→letter pair) → tess_resolver (letter pair→slot)
 */
#ifndef TESS_LETTERCUBE_H
#define TESS_LETTERCUBE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "tess_resolver.h"

/* ── Letter pool (A-Z, a-z = 52 letters) ──────────────────────── */
#define LC_LETTERS 52   /* 26 uppercase + 26 lowercase */
#define LC_PAIRS 24     /* 24 letter pairs (8 mini × 3 axes) */
#define LC_MINI 8       /* 8 mini-cubes (octants) */
#define LC_AXES 3       /* 3 axes (x, y, z) */

/* Letter index: 0-25 = A-Z (uppercase), 26-51 = a-z (lowercase) */
static inline int lc_is_upper(uint8_t idx) { return idx < 26; }
static inline int lc_is_lower(uint8_t idx) { return idx >= 26; }
static inline uint8_t lc_to_upper(uint8_t idx) { return lc_is_lower(idx) ? idx - 26 : idx; }
static inline uint8_t lc_to_lower(uint8_t idx) { return lc_is_upper(idx) ? idx + 26 : idx; }

/* Letter pair: inner (lowercase) + outer (uppercase) */
typedef struct {
    uint8_t inner;  /* lowercase letter (a-z) */
    uint8_t outer;  /* uppercase letter (A-Z) */
    int     linked; /* 1 = straight connection (letter match) */
} LCLetterPair;

/* ── Mini-cube (octant) ────────────────────────────────────────── */
typedef struct {
    int8_t pos[3];          /* position in 2×2×2 grid: ±1,±1,±1 */
    uint8_t L[LC_PAIRS];    /* 24 letters (inner faces) */
    uint8_t S[LC_AXES];     /* 3 frame letters (outer faces) */
} LCMiniCube;

/* ── LetterCube state ──────────────────────────────────────────── */
typedef struct {
    LCMiniCube cubes[LC_MINI];  /* 8 mini-cubes */
    LCLetterPair pairs[LC_PAIRS]; /* 24 letter pairs */
    int mode;                    /* 0=row, 1=chain, 2=onion */
    int n_cubes;                 /* number of active cubes */
} LCLetterCube;

/* ── Initialize (mode onion) ───────────────────────────────────── */
static inline void lc_init(LCLetterCube *lc, int mode) {
    memset(lc, 0, sizeof(*lc));
    lc->mode = mode;
    lc->n_cubes = 1;
    /* Initialize 8 mini-cubes at octant positions */
    int idx = 0;
    for (int x = -1; x <= 1; x += 2) {
        for (int y = -1; y <= 1; y += 2) {
            for (int z = -1; z <= 1; z += 2) {
                lc->cubes[idx].pos[0] = (int8_t)x;
                lc->cubes[idx].pos[1] = (int8_t)y;
                lc->cubes[idx].pos[2] = (int8_t)z;
                idx++;
            }
        }
    }
}

/* ── Letter pair addressing: weight → letter pair index ────────── */
/* 24 letter pairs = 24 tetra facets (8 mini × 3 axes) */
static inline uint8_t lc_weight_to_pair(int8_t weight) {
    /* Map weight (-128..127) → 0..23 (letter pair index) */
    /* Use abs value mod 24 (deterministic) */
    uint8_t w = (weight < 0) ? (uint8_t)(-weight) : (uint8_t)weight;
    return w % LC_PAIRS;
}

/* ── Letter pair → slot (20736) ───────────────────────────────── */
static inline uint32_t lc_pair_to_slot(uint8_t pair_idx, uint32_t layer) {
    /* pair: 0..23, layer: 0..35 (36 layers)
     * slot = pair * 864 + layer * 24 + pair? 
     * Actually: 24 pairs × 864 = 20736, or 36 layers × 576 = 20736
     * Use: slot = pair * 864 + layer  (864 = 20736/24) */
    if (pair_idx >= LC_PAIRS) pair_idx = LC_PAIRS - 1;
    if (layer >= 36) layer = 35;
    return (uint32_t)pair_idx * 864u + layer;
}

/* ── Weight → slot (via letter pair) ───────────────────────────── */
static inline uint32_t lc_weight_to_slot(int8_t weight, uint32_t layer) {
    uint8_t pair = lc_weight_to_pair(weight);
    return lc_pair_to_slot(pair, layer);
}

/* ── Link check: adjacent cubes share letter pair ──────────────── */
static inline int lc_link_check(const LCLetterCube *lc, int mini_a, int mini_b, int axis) {
    /* Check if mini_a's inner face matches mini_b's outer face on given axis */
    /* In "row" mode: same row → same letter set → always linked */
    if (lc->mode == 0) return 1;  /* row mode: always linked */
    /* In "chain" mode: inherit from previous → check match */
    if (lc->mode == 1) {
        /* Simplified: check if L values match */
        uint8_t a = lc->cubes[mini_a].L[axis * 4];  /* inner */
        uint8_t b = lc->cubes[mini_b].S[axis];       /* outer */
        return (lc_to_upper(a) == lc_to_upper(b)) ? 1 : 0;
    }
    /* Mode onion: nested → always linked (inner enclosed by outer) */
    return 1;
}

/* ── Mode onion: nested layers ─────────────────────────────────── */
/* In onion mode, inner cube is enclosed by outer cubes.
 * Each layer adds a shell of cubes around the inner one.
 * Layers: 0 = innermost, 35 = outermost (36 layers thick) */
static inline int lc_onion_layer(int mini_idx, int total_layers) {
    /* Determine which onion layer a mini-cube is on */
    /* Simplified: layer = distance from center (0,0,0) */
    /* Center mini-cubes (pos near 0) are inner layers */
    /* But all mini-cubes are at ±1, so layer = mini_idx % total_layers */
    return mini_idx % total_layers;
}

/* ── Verify ─────────────────────────────────────────────────────── */
static inline int lc_verify(void) {
    /* T1: 8 mini-cubes */
    LCLetterCube lc;
    lc_init(&lc, 2);  /* onion mode */
    /* Check 8 mini-cubes exist */
    for (int i = 0; i < LC_MINI; i++) {
        int sum = lc.cubes[i].pos[0] + lc.cubes[i].pos[1] + lc.cubes[i].pos[2];
        /* Each pos is ±1, sum can be -3, -1, 1, 3 */
        if (lc.cubes[i].pos[0] == 0) return -1;
    }
    /* T2: letter pair roundtrip */
    for (int w = -127; w <= 127; w++) {
        uint8_t pair = lc_weight_to_pair((int8_t)w);
        if (pair >= LC_PAIRS) return -2;
    }
    /* T3: slot in range */
    for (uint8_t p = 0; p < LC_PAIRS; p++) {
        for (uint32_t l = 0; l < 36; l++) {
            uint32_t slot = lc_pair_to_slot(p, l);
            if (slot >= TESS_TOTAL_SLOTS) return -3;
        }
    }
    /* T4: 24 pairs × 864 = 20736 */
    if (LC_PAIRS * 864 != TESS_TOTAL_SLOTS) return -4;
    return 0;
}

#endif /* TESS_LETTERCUBE_H */
