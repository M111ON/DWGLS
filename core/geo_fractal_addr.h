/*
 * geo_fractal_addr.h — Fractal Coordinate Addressing (multi-resolution)
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * 20736 = 12⁴ = tensor rank-4
 *   T[i, j, k, l]  i,j,k,l ∈ {0..11}
 *
 * Fractal coordinate: (h, x, y)
 *   h = height/level (resolution depth, 0..4)
 *   (x, y) = position in 2D grid at level h
 *
 * Hierarchy (all grid_w × grid_h × cell_size = 20736):
 *   h=4: grid 1×1,   cell=20736  → root
 *   h=3: grid 12×1,  cell=1728   → 12 groups
 *   h=2: grid 12×12, cell=144    → 144 regions
 *   h=1: grid 144×12,cell=12     → 1728 pipes
 *   h=0: grid 144×144,cell=1     → 20736 leaves
 *
 * flat address: flat = x × grid_h × cell_size + y × cell_size
 *
 * DEPENDS: none (pure geometry)
 * No malloc. No float. All static inline. Header-only.
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_FRACTAL_ADDR_H
#define GEO_FRACTAL_ADDR_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define FRACTAL_BASE        12u
#define FRACTAL_MAX_H       4u
#define FRACTAL_FULL        20736u  /* 12⁴ */

/* Per-level parameters:
 *   h  | grid_w  | grid_h  | cell_size | cells
 *   4  | 1       | 1       | 20736     | 1
 *   3  | 12      | 1       | 1728      | 12
 *   2  | 12      | 12      | 144       | 144
 *   1  | 144     | 12      | 12        | 1728
 *   0  | 144     | 144     | 1         | 20736
 */

static inline uint32_t fractal_cell_size(uint32_t h)
{
    /* cell_size = 12^h */
    uint32_t s = 1;
    for (uint32_t i = 0; i < h; i++) s *= FRACTAL_BASE;
    return s;
}

static inline uint32_t fractal_grid_w(uint32_t h)
{
    /* grid_w = 12^(floor((4-h)/2) + correction)
     * Simple: grid_w * grid_h * cell_size = 20736
     * grid_w = 12^ceil((4-h)/2), grid_h = 12^floor((4-h)/2) */
    uint32_t d = 4u - h;  /* "remaining digits" */
    uint32_t half = (d + 1) / 2;  /* ceil(d/2) */
    uint32_t w = 1;
    for (uint32_t i = 0; i < half; i++) w *= FRACTAL_BASE;
    return w;
}

static inline uint32_t fractal_grid_h(uint32_t h)
{
    uint32_t d = 4u - h;
    uint32_t half = d / 2;  /* floor(d/2) */
    uint32_t v = 1;
    for (uint32_t i = 0; i < half; i++) v *= FRACTAL_BASE;
    return v;
}

/* ═══════════════════════════════════════════════════════════════════════════
   FRACTAL ADDRESS — (h, x, y) ↔ flat
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t h;
    uint32_t x;
    uint32_t y;
} FractalAddr;

/* ── (h, x, y) → flat ── */
static inline uint32_t fractal_to_flat(uint32_t h, uint32_t x, uint32_t y)
{
    uint32_t csz = fractal_cell_size(h);
    uint32_t gh  = fractal_grid_h(h);
    return x * (gh * csz) + y * csz;
}

/* ── flat → (h, x, y) ── */
static inline FractalAddr fractal_from_flat(uint32_t flat, uint32_t h)
{
    uint32_t csz = fractal_cell_size(h);
    uint32_t gh  = fractal_grid_h(h);
    FractalAddr a;
    a.h = h;
    a.x = flat / (gh * csz);
    a.y = (flat / csz) % gh;
    return a;
}

/* ── offset within cell ── */
static inline uint32_t fractal_offset_in_cell(uint32_t flat, uint32_t h)
{
    return flat % fractal_cell_size(h);
}

/* ── parent at level h+1 ── */
static inline FractalAddr fractal_parent(uint32_t x, uint32_t y, uint32_t h)
{
    /* parent_x = x / (grid_w[h] / grid_w[h+1])
     * = x / 12^(ceil((4-h)/2) - ceil((3-h)/2))  ≈  x / 12 or x / 1 */
    uint32_t gw_h   = fractal_grid_w(h);
    uint32_t gw_h1  = fractal_grid_w(h + 1);
    uint32_t step_x = gw_h / gw_h1;
    return (FractalAddr){ h + 1, x / step_x, y / step_x };
}

/* ── child top-left at level h-1 ── */
static inline FractalAddr fractal_child_top_left(uint32_t px,
                                                  uint32_t py,
                                                  uint32_t h)
{
    uint32_t gw_h   = fractal_grid_w(h);
    uint32_t gw_hm1 = fractal_grid_w(h - 1);
    uint32_t scale  = gw_hm1 / gw_h;
    return (FractalAddr){ h - 1, px * scale, py * scale };
}

/* ═══════════════════════════════════════════════════════════════════════════
   BRIDGES — DRamTile, FiboSpine
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t fractal_to_dram(uint32_t flat) { return flat; }
static inline uint32_t dram_to_fractal(uint32_t dram) { return dram; }

static inline void fractal_to_pipe_tick(uint32_t flat,
                                         uint16_t *pipe_id,
                                         uint8_t  *tick)
{
    *pipe_id = (uint16_t)(flat / FRACTAL_BASE);
    *tick    = (uint8_t)(flat % FRACTAL_BASE);
}

static inline uint32_t pipe_tick_to_fractal(uint16_t pipe_id, uint8_t tick)
{
    return (uint32_t)pipe_id * FRACTAL_BASE + tick;
}

/* ═══════════════════════════════════════════════════════════════════════════
   ENTROPY SPLIT
   ═══════════════════════════════════════════════════════════════════════════ */

#define FRACTAL_ENTROPY_THRESHOLD  128u

static inline int fractal_should_split(uint8_t entropy, uint32_t h)
{
    if (h == 0) return 0;
    if (h >= FRACTAL_MAX_H) return 0;
    return entropy > FRACTAL_ENTROPY_THRESHOLD ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFY
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int fractal_verify(void)
{
    for (uint32_t h = 0; h <= FRACTAL_MAX_H; h++) {
        uint32_t gw = fractal_grid_w(h);
        uint32_t gh = fractal_grid_h(h);
        uint32_t cs = fractal_cell_size(h);
        if (gw * gh * cs != FRACTAL_FULL) return -1;

        for (uint32_t x = 0; x < gw; x++) {
            for (uint32_t y = 0; y < gh; y++) {
                uint32_t flat = fractal_to_flat(h, x, y);
                if (flat >= FRACTAL_FULL) return -2;
                FractalAddr a = fractal_from_flat(flat, h);
                if (a.x != x || a.y != y) return -3;
            }
        }
    }

    /* parent/child consistency at h=1→2 */
    for (uint32_t x = 0; x < fractal_grid_w(1); x++) {
        for (uint32_t y = 0; y < fractal_grid_h(1); y++) {
            FractalAddr p = fractal_parent(x, y, 1);
            if (p.h != 2) return -4;
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   D4 SYMMETRY OPERATIONS
   ═══════════════════════════════════════════════════════════════════════════
   D4 = {identity, rot90, rot180, rot270, mirror_h, mirror_v, mirror_d1, mirror_d2}
   8 elements acting on (x,y) within a grid of given dimensions.
   ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    FRACTAL_D4_ID     = 0,
    FRACTAL_D4_ROT90  = 1,
    FRACTAL_D4_ROT180 = 2,
    FRACTAL_D4_ROT270 = 3,
    FRACTAL_D4_MH     = 4,   /* mirror horizontal (left-right flip) */
    FRACTAL_D4_MV     = 5,   /* mirror vertical (top-bottom flip)   */
    FRACTAL_D4_MD1    = 6,   /* mirror diagonal (swap x,y)          */
    FRACTAL_D4_MD2    = 7    /* mirror anti-diagonal                */
} FractalD4Op;

/*
 * fractal_d4_apply — apply D4 operation to (x,y) within gw×gh grid
 * s_x = max x-index (gw-1), s_y = max y-index (gh-1)
 */
static inline FractalAddr fractal_d4_apply(uint32_t x, uint32_t y,
                                            uint32_t gw, uint32_t gh,
                                            FractalD4Op op)
{
    uint32_t sx = gw - 1;
    uint32_t sy = gh - 1;
    FractalAddr r;
    r.h = 0;  /* caller sets h */
    r.x = x; r.y = y;

    switch (op) {
    case FRACTAL_D4_ID:     break;
    case FRACTAL_D4_ROT90:  r.x = y;          r.y = sx - x; break;
    case FRACTAL_D4_ROT180: r.x = sx - x;     r.y = sy - y; break;
    case FRACTAL_D4_ROT270: r.x = sy - y;     r.y = x;      break;
    case FRACTAL_D4_MH:     r.x = sx - x;     break;
    case FRACTAL_D4_MV:     r.y = sy - y;     break;
    case FRACTAL_D4_MD1:    r.x = y;          r.y = x;      break;
    case FRACTAL_D4_MD2:    r.x = sy - y;     r.y = sx - x; break;
    }
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
   LINE-SUM FINGERPRINT (Einstein Summation on 3×3 sub-grid)
   ═══════════════════════════════════════════════════════════════════════════
   For a 3×3 grid:
     rows: R0, R1, R2   (3 row sums)
     cols: C0, C1, C2   (3 col sums)
     diag: D0 = G[0,0]+G[1,1]+G[2,2]
     anti: D1 = G[0,2]+G[1,1]+G[2,0]
   Total: 8 line sums = partial contraction of rank-2 tensor
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t row[3];    /* R0, R1, R2 */
    uint8_t col[3];    /* C0, C1, C2 */
    uint8_t diag;      /* main diagonal */
    uint8_t anti;      /* anti diagonal */
} FractalLineSum;

static inline FractalLineSum fractal_line_sum_3x3(const uint8_t grid[3][3])
{
    FractalLineSum ls;
    for (int r = 0; r < 3; r++) {
        ls.row[r] = grid[r][0] + grid[r][1] + grid[r][2];
    }
    for (int c = 0; c < 3; c++) {
        ls.col[c] = grid[0][c] + grid[1][c] + grid[2][c];
    }
    ls.diag = grid[0][0] + grid[1][1] + grid[2][2];
    ls.anti = grid[0][2] + grid[1][1] + grid[2][0];
    return ls;
}

/* n15 = count of line sums that equal 15 (magic constant) */
static inline uint8_t fractal_n15_count(FractalLineSum ls)
{
    uint8_t count = 0;
    for (int i = 0; i < 3; i++) {
        if (ls.row[i] == 15u) count++;
        if (ls.col[i] == 15u) count++;
    }
    if (ls.diag == 15u) count++;
    if (ls.anti == 15u) count++;
    return count;
}

#endif /* GEO_FRACTAL_ADDR_H */
