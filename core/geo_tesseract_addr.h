/*
 * geo_tesseract_addr.h — Fixed-frame 4D addressing (no camera move)
 *
 * User concept (2026-08-21): 4D object is STATIONARY memory.
 * We pin a frame index and access interior via address — no distortion.
 * Moving through 4D = switching the index state, not changing a view angle.
 *
 * Tesseract = 8 cubes (cells) = (axis 0..3, sign 0/1) → 3 bits = 8 values
 *   idx = (axis << 1) | sign   — 0..7, no geometry, just integer
 *   axis = idx >> 1, sign = idx & 1
 *
 * 18tes protagonist field: 18 tesseracts × 8 cells × 144 slots = 20736
 *   flat = tess*1152 + cell*144 + slot   — deterministic, int-only
 *   Maps the whole GEO_FULL field without hash or lookup table.
 *
 * Two adjacency views (both deterministic, O(1)):
 *   [A] XOR-cube (per doc): neighbor = idx ^ (1<<k), k=0..2 — 3-cube,
 *       degree 3, illustrates "bit-flip graph" for any N (2N cells via 3 bits).
 *   [B] True tesseract cells: cell (axis,sign) adjacent to the 6 cells
 *       with axis' != axis (all except opposite (axis,1-sign)).
 *       Degree 6, matches the 8-cell tesseract topology.
 *
 * Fixed-frame guarantee: once idx is pinned, any interior slot access is
 * flat(idx,slot) without field-wide side effects — no magnify invert,
 * no hex residual. The field only matters when picking the frame.
 */

#ifndef GEO_TESSERACT_ADDR_H
#define GEO_TESSERACT_ADDR_H

#include <stdint.h>

#define TESS_AXES        4u
#define TESS_3D_CELLS    8u   /* 3D cells (cubes) in a 4D tesseract */
#define TESS_SLOTS       144u
#define TESS_PER_TESS    1152u                  /* 8*144 */
#define TESS_COUNT       18u
#define TESS_GEO_FULL    20736u                 /* 18*1152 = 144*144 */

/* ── index encode / decode ──────────────────────────────────────── */
static inline uint32_t tess_index(uint32_t axis, uint32_t sign) {
    return ((axis & 3u) << 1) | (sign & 1u);
}
static inline uint32_t tess_axis(uint32_t idx) { return (idx >> 1) & 3u; }
static inline uint32_t tess_sign(uint32_t idx) { return idx & 1u; }

/* ── XOR-cube neighbors (doc's bit-flip graph) ─────────────────── */
static inline uint32_t tess_neighbor_xor(uint32_t idx, uint32_t k) {
    return (idx ^ (1u << (k % 3u))) & 7u;
}

/* ── true tesseract cell adjacency: 6 neighbors (axis' != axis) ── */
static inline uint32_t tess_adjacent(uint32_t idx, uint32_t out[6]) {
    uint32_t ax = tess_axis(idx);
    uint32_t n = 0;
    for (uint32_t a = 0; a < TESS_AXES; a++) if (a != ax) {
        out[n++] = tess_index(a, 0);
        out[n++] = tess_index(a, 1);
    }
    return n; /* always 6 */
}

/* ── flat address in the 20736 field (fixed frame) ─────────────── */
static inline uint32_t tess_flat(uint32_t tess, uint32_t cell, uint32_t slot) {
    return (tess % TESS_COUNT) * TESS_PER_TESS
         + (cell % TESS_3D_CELLS) * TESS_SLOTS
         + (slot % TESS_SLOTS);
}
static inline void tess_unflat(uint32_t flat,
                                uint32_t *tess, uint32_t *cell, uint32_t *slot) {
    if (tess) *tess = flat / TESS_PER_TESS;
    if (cell) *cell = (flat % TESS_PER_TESS) / TESS_SLOTS;
    if (slot) *slot = flat % TESS_SLOTS;
}

/* ── memory window: the 144 x 144 unfolding of the field ───────── */
/* The field is ONE flat array of TESS_GEO_FULL slots.  Read row-major
   with TESS_WIN_COLS columns and the row index IS the (tesseract, cube)
   pair:
       row = tess*8 + cell        flat = row*144 + col
   so the window needs no stride table — asserted for all 20736 slots in
   tests/test_window_ladder.c.  Out of the 22 other equal-area windows
   this is the only one that is whole tesseracts, aligned with the BFS
   144-blocks x 144-slots grid, whole cache lines per row at
   TESS_CELL_F32, and on the 144-cycle the stride-37 walk lives on.
   128 x 162 is the *address route* split (Hilbert 2x64 x ico 162), not a
   window: it satisfies none of those. */
#define TESS_WIN_COLS   TESS_SLOTS                      /* 144 */
#define TESS_WIN_ROWS   (TESS_GEO_FULL / TESS_WIN_COLS) /* 144 */
static inline uint32_t tess_win_flat(uint32_t row, uint32_t col) {
    return (row % TESS_WIN_ROWS) * TESS_WIN_COLS + (col % TESS_WIN_COLS);
}
static inline uint32_t tess_win_row(uint32_t flat) { return flat / TESS_WIN_COLS; }
static inline uint32_t tess_win_col(uint32_t flat) { return flat % TESS_WIN_COLS; }
static inline uint32_t tess_row_tess(uint32_t row) { return row / TESS_3D_CELLS; }
static inline uint32_t tess_row_cell(uint32_t row) { return row % TESS_3D_CELLS; }

/* Whole 4 KiB pages in one window: 0 when this cell size cannot page-align.
   A cell size page-aligns iff it is a multiple of 16 B: with 20736 = 2^8*3^4
   and 4096 = 2^12 we need 16 | c.  Examples (see tests): c=4 -> 0, c=16 -> 81
   (= 9^2, a square page grid), c=34 (Q8_0 block) -> 0, c=64 -> 324 (= 18^2). */
#define TESS_WIN_PAGE_BYTES 4096u
static inline uint32_t tess_win_pages(uint32_t cell_size) {
    unsigned long long b = (unsigned long long)TESS_GEO_FULL * (unsigned long long)cell_size;
    if (b % TESS_WIN_PAGE_BYTES) return 0u;
    return (uint32_t)(b / TESS_WIN_PAGE_BYTES);
}

/* ── verification (call from tests) ─────────────────────────────── */
static inline int geo_tesseract_verify(void) {
    /* encode/decode roundtrip */
    for (uint32_t ax = 0; ax < TESS_AXES; ax++)
        for (uint32_t s = 0; s < 2; s++) {
            uint32_t idx = tess_index(ax, s);
            if (tess_axis(idx) != ax || tess_sign(idx) != s) return -1;
        }
    /* XOR involution: flip twice returns */
    for (uint32_t idx = 0; idx < TESS_3D_CELLS; idx++)        for (uint32_t k = 0; k < 3; k++)
            if (tess_neighbor_xor(tess_neighbor_xor(idx, k), k) != idx) return -2;
    /* adjacency degree 6 and excludes opposite */
    for (uint32_t idx = 0; idx < TESS_3D_CELLS; idx++) {
        uint32_t out[6]; uint32_t n = tess_adjacent(idx, out);
        if (n != 6) return -3;
        uint32_t opp = tess_index(tess_axis(idx), tess_sign(idx) ^ 1u);
        for (uint32_t i = 0; i < 6; i++) if (out[i] == opp) return -4;
        for (uint32_t i = 0; i < 6; i++) if (out[i] == idx) return -5;
    }
    /* flat/unflat + coverage of 20736 */
    for (uint32_t f = 0; f < TESS_GEO_FULL; f += 997) {
        uint32_t t, c, s; tess_unflat(f, &t, &c, &s);
        if (tess_flat(t, c, s) != f) return -6;
    }
    if (TESS_GEO_FULL != 20736u) return -7;
    if (TESS_PER_TESS * TESS_COUNT != TESS_GEO_FULL) return -8;
    /* window == flat(), and row decodes to (tess, cube) */
    if (TESS_WIN_ROWS * TESS_WIN_COLS != TESS_GEO_FULL) return -9;
    for (uint32_t f = 0; f < TESS_GEO_FULL; f += 331) {
        uint32_t row = tess_win_row(f), col = tess_win_col(f);
        uint32_t t, c, s; tess_unflat(f, &t, &c, &s);
        if (tess_row_tess(row) != t || tess_row_cell(row) != c || col != s) return -10;
        if (tess_win_flat(row, col) != f) return -11;
    }
    return 0;
}

#endif /* GEO_TESSERACT_ADDR_H */
