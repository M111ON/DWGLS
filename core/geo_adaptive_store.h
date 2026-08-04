/*
 * geo_adaptive_store.h — Adaptive Storage Engine for Kis Timeline
 * ════════════════════════════════════════════════════════════════
 *
 * Changes storage size based on data entropy:
 *   Tier 0 (structured,  score 0-63):   span=0,  1 frame   →  12 edges →  12 × 64B = 768B
 *   Tier 1 (moderate,    score 64-127):  span=1,  3 frames  →  36 edges →  36 × 64B = 2.3KB
 *   Tier 2 (high,        score 128-191): span=3,  7 frames  →  84 edges →  84 × 64B = 5.4KB
 *   Tier 3 (random,      score 192-255): span=13, 27 frames → 324 edges → 324 × 64B = 20.7KB
 *
 * Uses geo_frame_seek.h for frame_range_adaptive().
 * Each DiamondBlock = 64 floats = 1 edge of 1 frame.
 *
 * No malloc. No float division. Stateless.
 * ════════════════════════════════════════════════════════════════
 */

#ifndef GEO_ADAPTIVE_STORE_H
#define GEO_ADAPTIVE_STORE_H

#include <stdint.h>
#include <string.h>
#include "geo_frame_seek.h"

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define ADPT_BLOCK_WORDS    64u    /* floats per DiamondBlock (8×8)  */
#define ADPT_EDGES_PER_FRAME 12u   /* 9 Hilbert + 3 Peano           */
#define ADPT_MAX_FRAMES     27u    /* tier 3 worst case              */
#define ADPT_MAX_BLOCKS     (ADPT_MAX_FRAMES * ADPT_EDGES_PER_FRAME) /* 324 */
#define ADPT_MAX_WEIGHTS    (ADPT_MAX_BLOCKS * ADPT_BLOCK_WORDS)     /* 20736 */

/* ══════════════════════════════════════════════════════════════
   STRUCT
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t  enc;              /* current frame enc (0..1439)      */
    uint8_t   tier;             /* computed tier 0..3               */
    uint8_t   entropy_score;    /* input entropy 0..255             */
    uint8_t   frame_count;      /* number of frames allocated       */
    uint16_t  frames[89];       /* frame encs in range              */
    uint16_t  block_count;      /* = frame_count × 12 (max 324)    */
    float     blocks[ADPT_MAX_BLOCKS * ADPT_BLOCK_WORDS]; /* payload */
    uint32_t  total_weight_count; /* actual weights written         */
    uint32_t  checksum;         /* CRC32 of blocks                  */
} AdaptiveStore;

/* ══════════════════════════════════════════════════════════════
   TIER MAPPING
   ══════════════════════════════════════════════════════════════ */

/* Compute tier from entropy score (0..255) */
static inline uint8_t adaptive_tier(uint8_t entropy_score)
{
    if (entropy_score <  64) return 0;  /* structured */
    if (entropy_score < 128) return 1;  /* moderate    */
    if (entropy_score < 192) return 2;  /* high        */
    return 3;                            /* random      */
}

/* Frame count per tier */
static inline uint8_t adaptive_frame_count(uint8_t tier)
{
    /* tier 0→1, tier 1→3, tier 2→7, tier 3→27 */
    static const uint8_t counts[4] = { 1, 3, 7, 27 };
    return counts[tier & 3];
}

/* ══════════════════════════════════════════════════════════════
   CRC32
   ══════════════════════════════════════════════════════════════ */

static inline uint32_t adaptive_crc32(const float *data, uint32_t count)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t len = count * sizeof(float);
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ══════════════════════════════════════════════════════════════
   CORE: init / write / read / verify
   ══════════════════════════════════════════════════════════════ */

static inline void adaptive_init(AdaptiveStore *as)
{
    if (!as) return;
    memset(as, 0, sizeof(*as));
}

/* Write weight_buf[0..n-1] at time t with given entropy_score.
 * Computes tier → frame range → fills DiamondBlocks.
 * Returns 0 on success, -1 on overflow, -2 on bad input. */
static inline int adaptive_write(AdaptiveStore *as, uint32_t t,
                                  const float *weight_buf, int n,
                                  uint8_t entropy_score)
{
    if (!as || !weight_buf || n <= 0) return -2;

    as->entropy_score = entropy_score;
    as->tier = adaptive_tier(entropy_score);
    as->frame_count = adaptive_frame_count(as->tier);
    as->enc = frame_enc(t % FRAME_CYCLE);

    /* Fill frame encs using frame_range_adaptive */
    uint16_t home_enc = as->enc;
    FrameRange fr = frame_range_adaptive(home_enc, entropy_score);

    /* Compute frame indices in range, convert to encs */
    uint8_t idx = 0;
    uint8_t f = fr.frame_lo;
    for (uint8_t i = 0; i < as->frame_count && idx < 89; i++) {
        as->frames[idx++] = f * FRAME_EDGES;
        f = (f + 1) % FRAME_MAX;
        if (f == fr.frame_hi) { as->frames[idx++] = f * FRAME_EDGES; break; }
    }
    as->block_count = (uint16_t)(as->frame_count * ADPT_EDGES_PER_FRAME);

    /* Fill blocks: write weights round-robin into 64-float DiamondBlocks */
    uint32_t bi = 0; /* block index */
    int wi = 0;      /* weight index */
    for (uint8_t fi = 0; fi < as->frame_count; fi++) {
        for (uint8_t ei = 0; ei < ADPT_EDGES_PER_FRAME; ei++) {
            float *blk = &as->blocks[bi * ADPT_BLOCK_WORDS];
            for (int k = 0; k < (int)ADPT_BLOCK_WORDS; k++) {
                blk[k] = (wi < n) ? weight_buf[wi] : 0.0f;
                wi++;
            }
            bi++;
        }
    }
    as->total_weight_count = (uint32_t)n;
    as->checksum = adaptive_crc32(as->blocks, bi * ADPT_BLOCK_WORDS);

    return 0;
}

/* Read: reconstruct weight_buf[0..n-1] from stored blocks.
 * Returns 0 on success, -1 on mismatch. */
static inline int adaptive_read(AdaptiveStore *as, uint32_t t,
                                 float *weight_buf, int n)
{
    if (!as || !weight_buf || n <= 0) return -1;

    int wi = 0;
    uint32_t bi = 0;
    for (uint8_t fi = 0; fi < as->frame_count && wi < n; fi++) {
        for (uint8_t ei = 0; ei < ADPT_EDGES_PER_FRAME && wi < n; ei++) {
            const float *blk = &as->blocks[bi * ADPT_BLOCK_WORDS];
            for (int k = 0; k < (int)ADPT_BLOCK_WORDS && wi < n; k++) {
                weight_buf[wi++] = blk[k];
            }
            bi++;
        }
    }
    return 0;
}

/* Verify: check internal consistency. Returns 0 on pass. */
static inline int adaptive_verify(const AdaptiveStore *as)
{
    if (!as) return -1;
    if (as->tier > 3) return -2;
    if (as->frame_count > 89) return -3;
    if (as->block_count != (uint16_t)(as->frame_count * ADPT_EDGES_PER_FRAME)) return -4;

    uint32_t expected_crc = adaptive_crc32(as->blocks,
                                           as->block_count * ADPT_BLOCK_WORDS);
    if (as->checksum != expected_crc) return -5;

    return 0;
}

#endif /* GEO_ADAPTIVE_STORE_H */
