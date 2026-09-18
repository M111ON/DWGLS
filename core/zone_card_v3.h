// zone_card_v3.h — gate descriptor cards + 7 reserved special cards (colors)
// revives deprecated/PasteBin/zone_card.h (frozen, do not edit) with calibrated-fresh thresholds.
// layout: 12B card (id/type/entropy/pattern/locality/stability/neighbor L+R)
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ZCARD_SPARSE 0
#define ZCARD_BATCH  1
#define ZCARD_LZ     2
#define ZCARD_NONEIGH 0xFFFF

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t  card_type;
    uint8_t  entropy;
    uint16_t pattern;
    uint8_t  locality;
    uint8_t  stability;
    uint16_t neighbor_left;
    uint16_t neighbor_right;
} ZoneCard3;

_Static_assert(sizeof(ZoneCard3) == 12, "ZoneCard3 must be 12 bytes");

/* -- 7 reserved special cards: ids 0xFFF8..0xFFFE (0xFFFF stays NO_NEIGHBOR) -- */
#define ZCARD_RED      0xFFF8  /* alarm: integrity breach in zone -> gate HALTS loads here */
#define ZCARD_BLUE     0xFFF9  /* facts: zone backed by semantic/facts db -> retrieval-augmented */
#define ZCARD_GREEN    0xFFFA  /* go: verified healthy -> fast path, skip re-verify */
#define ZCARD_BLACK    0xFFFB  /* tomb: retired/dead -> do not resurrect without restore protocol */
#define ZCARD_WHITE    0xFFFC  /* blank: uninitialized/empty -> safe to write, nothing to preserve */
#define ZCARD_GOLD     0xFFFD  /* canonical: proven-good/promoted -> prefer these */
#define ZCARD_WILDCARD 0xFFFE  /* god hand: owner override -> bypasses all gate logic */

/* gate verdicts returned when a special card governs the zone */
#define ZGATE_HALT     -1  /* RED/BLACK: refuse load */
#define ZGATE_FAST      1  /* GREEN/GOLD: load without re-verify */
#define ZGATE_AUGMENT   2  /* BLUE: load + attach facts context */
#define ZGATE_BLANK    3  /* WHITE: fresh write path */
#define ZGATE_OVERRIDE  9  /* WILDCARD: owner decided, skip everything */

static inline int zone_card3_is_special(uint16_t id) {
    return id >= ZCARD_RED && id <= ZCARD_WILDCARD;
}
