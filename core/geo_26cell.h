/* geo_26cell.h — 26-Cell Hollow Cube + Pair LetterCube identity.
 *
 * 3x3x3 = 27 cells; center (1,1,1) is VOID (removed) -> 26 cells remain.
 * 26 cells map 1:1 to 26 letter pairs (A-Z / a-z): bijection.
 * Void stays open -> repeated loop cycle (no dead end).
 *
 * Self-contained: only <stdint.h>. No deps.
 */
#ifndef GEO_26CELL_H
#define GEO_26CELL_H

#include <stdint.h>

#define CELL_DIM   3u
#define CELL_TOTAL 27u   /* 3^3 */
#define CELL_VOID  13u   /* center index: 1 + 3*1 + 9*1 */
#define CELL_N     26u   /* 27 - 1 */

static inline uint32_t cell_idx(uint8_t x, uint8_t y, uint8_t z) {
    return (uint32_t)x + (uint32_t)y * CELL_DIM + (uint32_t)z * CELL_DIM * CELL_DIM;
}

static inline int cell_is_void_idx(uint32_t idx) { return idx == CELL_VOID; }

/* compact index 0..25 for non-void cells (skip void) */
static inline uint32_t cell_compact(uint32_t idx) {
    /* idx in 0..26 except 13; compact = idx if idx<13 else idx-1 */
    return (idx < CELL_VOID) ? idx : (idx - 1u);
}

static inline uint32_t compact_to_idx(uint32_t c) {
    /* c in 0..25; idx = c if c<13 else c+1 */
    return (c < CELL_VOID) ? c : (c + 1u);
}

/* letter pair for compact cell c: upper = 'A'+c, lower = 'a'+c */
static inline char cell_upper(uint32_t c) { return (char)('A' + c); }
static inline char cell_lower(uint32_t c) { return (char)('a' + c); }

/* bijection checks */
static inline int cell_roundtrip_ok(uint32_t idx) {
    if (cell_is_void_idx(idx)) return 1; /* void is its own "undefined" */
    uint32_t c = cell_compact(idx);
    uint32_t back = compact_to_idx(c);
    return back == idx;
}

#endif /* GEO_26CELL_H */
