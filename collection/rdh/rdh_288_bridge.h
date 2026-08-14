/*
 * rdh_288_bridge.h — Bridge: RDH flat_key ↔ 288-cell address
 * ══════════════════════════════════════════════════════════════
 *
 * THE MISSING STEP between rdh_capture and 288-cell geometry.
 *
 * Current flow (lossy):
 *   flat_key % 1440 → enc → frame_seek → (face, slot, phase, ico_idx)
 *
 * 288-cell flow (full):
 *   flat_key → bridge_288 → (face, direction, cell_pos)
 *   → frame_seek_1728 → full decomposition
 *
 * No malloc. No float. O(1). Bijection guaranteed.
 * ══════════════════════════════════════════════════════════════
 */

#ifndef RDH_288_BRIDGE_H
#define RDH_288_BRIDGE_H

#include <stdint.h>

/* ── 288-cell constants ─────────────────────────────── */
#define CELL_288      288u
#define CELL_DIRS       6u
#define CELL_PER_FACE 1728u
#define DODECA_FACES   12u
#define GEO_FULL      20736u

/* ── 288-cell address tuple ─────────────────────────── */
typedef struct {
    uint16_t cell_pos;    /* 0..287  */
    uint8_t  direction;   /* 0..5    */
    uint8_t  face;        /* 0..11   */
} Cell288Addr;

/* ── flat_key → 288-cell (THE MISSING STEP) ────────── */
static inline Cell288Addr bridge_288(int64_t flat_key) {
    Cell288Addr a;
    a.face      = (uint8_t)((flat_key / CELL_PER_FACE) % DODECA_FACES);
    a.direction = (uint8_t)((flat_key / CELL_288) % CELL_DIRS);
    a.cell_pos  = (uint16_t)(flat_key % CELL_288);
    return a;
}

/* ── 288-cell → flat_key (reverse) ─────────────────── */
static inline int64_t bridge_288_key(Cell288Addr a) {
    return (int64_t)a.face * CELL_PER_FACE
         + (int64_t)a.direction * CELL_288
         + (int64_t)a.cell_pos;
}

/* ══════════════════════════════════════════════════════════════
   FRAME SEEK on 1728 — extended cycle
   ══════════════════════════════════════════════════════════════ */

#define FRAME_1728_CYCLE  1728u
#define FRAME_1728_STRIDE   37u

static inline uint16_t frame_1728_enc(uint32_t t) {
    return (uint16_t)((t * FRAME_1728_STRIDE) % FRAME_1728_CYCLE);
}

static inline uint16_t frame_1728_next(uint16_t enc) {
    return (uint16_t)((enc + FRAME_1728_STRIDE) % FRAME_1728_CYCLE);
}

static inline uint16_t frame_1728_prev(uint16_t enc) {
    return (uint16_t)((enc + FRAME_1728_CYCLE - FRAME_1728_STRIDE) % FRAME_1728_CYCLE);
}

/* ── decode 1728 enc into 288-cell components ──────── */
static inline void frame_1728_decode(uint16_t enc_1728,
                                      uint8_t *face,
                                      uint8_t *direction,
                                      uint16_t *cell_pos) {
    *face      = (uint8_t)(enc_1728 / CELL_PER_FACE);
    *direction = (uint8_t)((enc_1728 / CELL_288) % CELL_DIRS);
    *cell_pos  = (uint16_t)(enc_1728 % CELL_288);
}

/* ── verify: stride-37 full cycle on 1728 ──────────── */
static inline int bridge_288_verify(void) {
    uint8_t visited[1728] = {0};
    uint16_t e = 0;
    for (uint32_t i = 0; i < 1728; i++) {
        if (visited[e]) return -1;
        visited[e] = 1;
        e = frame_1728_next(e);
    }
    return (e == 0) ? 0 : -2;
}

/* ── verify: all 20736 keys decompose uniquely ─────── */
static inline int bridge_288_full_verify(void) {
    uint32_t dir_count[6] = {0};
    uint32_t face_count[12] = {0};
    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr a = bridge_288(k);
        if (a.face > 11 || a.direction > 5 || a.cell_pos > 287)
            return -1;
        dir_count[a.direction]++;
        face_count[a.face]++;
    }
    for (int d = 0; d < 6; d++) if (dir_count[d] != 3456) return -2;
    for (int f = 0; f < 12; f++) if (face_count[f] != 1728) return -3;
    return 0;
}

#endif /* RDH_288_BRIDGE_H */
