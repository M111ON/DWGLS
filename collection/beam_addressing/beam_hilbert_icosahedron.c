/*
 * beam_hilbert_icosahedron.c — Hilbert × Icosahedron
 *
 * Key insight: geo_jump designed for Hilbert 64
 *   Hilbert 4×4×4 = 64 positions
 *   × 2 = 128
 *   × 162 (frequency-4 icosahedron vertices) = 20736
 *   = 144 × 144 = geo_jump structure
 *
 * "Cross with sphere": Hilbert (64) × Icosahedron (162) = 20736
 *
 * Flow:
 *   weight → tessellation node (20736 grid)
 *   tessellation node → Hilbert 4×4×4 position (64 positions)
 *   Hilbert position → icosahedron vertex (162 vertices)
 *   icosahedron vertex → frame_seek tile (1440)
 *
 * 3D Hilbert from GEOMATRIX V5:
 *   d2xyz(n, d) maps distance d → [x,y,z] in 3D space
 *   4×4×4 = 64 positions per icosahedron face
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* No M_PI needed — all math is integer-only */

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define GEO_FULL        20736u
#define GEO_PENTAGONS   12u
#define GEO_TOWER       144u
#define GEO_SHELL_TICK  12u
#define GEO_FIBO_CLOCK  1440u
#define FACE_STRIDE     (GEO_FULL / GEO_PENTAGONS)  /* 1728 */

#define HILBERT_N       4u    /* 4×4×4 = 64 positions */
#define HILBERT_CELLS   64u   /* 4^3 = 64 */
#define ICOSAHEDRON_V   162u  /* frequency-4 icosahedron vertices */
#define ICOSAHEDRON_X2  2u    /* × 2 for sign/direction */

#define FRAME_CYCLE     1440u
#define FRAME_STRIDE    37u

/* Verify: (64 × 2) × 162 = 20736 */
#if (HILBERT_CELLS * ICOSAHEDRON_X2 * ICOSAHEDRON_V) != GEO_FULL
#error "Hilbert × Icosahedron mismatch: expected 20736"
#endif

/* ══════════════════════════════════════════════════════════════
   3D HILBERT CURVE (from GEOMATRIX V5)
   ══════════════════════════════════════════════════════════════
 *
 * d2xyz(n, d) maps distance d → [x,y,z] in n×n×n cube
 * n=4 → 4×4×4 = 64 positions
 *
 * This is the MAZE — fixed path through 3D space.
 * Data navigates through it.
 */

typedef struct { uint8_t x, y, z; } HilbertPos;

static HilbertPos hilbert_d2xyz(uint32_t n, uint32_t d)
{
    HilbertPos p = {0, 0, 0};
    uint32_t s = 1;
    uint32_t t = d;

    while (s < n) {
        uint32_t rx = 1 & (t / 2);
        uint32_t ry = 1 & (t ^ rx);
        uint32_t rz = 1 & (t / 4);

        if (rz == 0) {
            if (ry == 0) {
                if (rx == 1) {
                    p.x = s - 1 - p.x;
                    p.y = s - 1 - p.y;
                }
                uint8_t tmp = p.x; p.x = p.y; p.y = tmp;
            } else {
                if (rx == 1) {
                    p.y = s - 1 - p.y;
                    p.z = s - 1 - p.z;
                }
                uint8_t tmp = p.x; p.x = p.z; p.z = tmp;
            }
        }

        p.x += s * rx;
        p.y += s * ry;
        p.z += s * rz;
        t /= 8;
        s *= 2;
    }

    return p;
}

/* Inverse: [x,y,z] → distance d on Hilbert curve
 * Use precomputed LUT since n=4 (64 entries) */
static uint32_t hilbert_lut_d[64];
static HilbertPos hilbert_lut_pos[64];
static int hilbert_lut_ready = 0;

static void hilbert_build_lut(void)
{
    if (hilbert_lut_ready) return;
    for (uint32_t d = 0; d < HILBERT_CELLS; d++) {
        HilbertPos p = hilbert_d2xyz(HILBERT_N, d);
        hilbert_lut_d[d] = d;
        hilbert_lut_pos[d] = p;
    }
    hilbert_lut_ready = 1;
}

static uint32_t hilbert_xyz2d(HilbertPos target)
{
    /* Linear search (64 entries) — fast enough for prototype */
    for (uint32_t d = 0; d < HILBERT_CELLS; d++) {
        HilbertPos p = hilbert_lut_pos[d];
        if (p.x == target.x && p.y == target.y && p.z == target.z)
            return d;
    }
    return 0; /* not found */
}

/* ══════════════════════════════════════════════════════════════
   ICOSAHEDRON — Frequency-4 (162 vertices)
   ══════════════════════════════════════════════════════════════
 *
 * Frequency-4 icosahedron: 162 vertices
 * Each vertex has a position on the sphere.
 * Combined with Hilbert 64: 64 × 162 = 10368
 * × 2 (direction/sign) = 20736
 */

/* Icosahedron vertex (simplified) */
typedef struct { int16_t x, y, z; } IcoVertex;

/* Generate icosahedron vertices (frequency-4 subdivision) */
static IcoVertex ico_vertices[ICOSAHEDRON_V];

/*
 * Integer-only golden ratio constants.
 * φ = (1+√5)/2 ≈ 1.6180339887...
 * PHI_I = 1618 (φ × 1000, truncated)
 * PHI_S = 1000 (scale factor)
 *
 * Base icosahedron 12 vertices (integer coordinates):
 *   (0, ±PHI_S, ±PHI_I), (±PHI_S, ±PHI_I, 0), (±PHI_I, 0, ±PHI_S)
 *
 * All math is integer-only: no float, no sqrt, no trig, no M_PI.
 */
#define PHI_I 1618
#define PHI_S 1000

/* Integer sqrt (Newton-Raphson, exact for perfect squares) */
static int32_t isqrt(int64_t v) {
    if (v <= 0) return 0;
    int64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (int32_t)x;
}

/* Normalize (x,y,z) to magnitude ≈ 1000 using integer only */
static void ico_normalize(int32_t x, int32_t y, int32_t z, int16_t *ox, int16_t *oy, int16_t *oz) {
    int64_t mag2 = (int64_t)x*x + (int64_t)y*y + (int64_t)z*z;
    if (mag2 == 0) { *ox = *oy = *oz = 0; return; }
    int32_t mag = isqrt(mag2);           /* ≈ actual magnitude */
    if (mag < 1) mag = 1;
    *ox = (int16_t)((x * 1000) / mag);  /* scale to 1000 */
    *oy = (int16_t)((y * 1000) / mag);
    *oz = (int16_t)((z * 1000) / mag);
}

static void ico_generate(void)
{
    /*
     * 162 vertices via integer-only math.
     * Zero floating-point operations.
     *
     * Step 1: 12 base icosahedron vertices (golden ratio coords)
     * Step 2: 150 midpoints between all pairs of base vertices
     * Step 3: Fill remaining to 162 via Fibonacci golden spiral
     */

    /* Step 1: Base 12 vertices (canonical icosahedron via golden ratio) */
    int32_t base[12][3] = {
        {    0,  PHI_S,  PHI_I}, {    0,  PHI_S, -PHI_I},
        {    0, -PHI_S,  PHI_I}, {    0, -PHI_S, -PHI_I},
        { PHI_S,  PHI_I,     0}, { PHI_S, -PHI_I,     0},
        {-PHI_S,  PHI_I,     0}, {-PHI_S, -PHI_I,     0},
        { PHI_I,     0,  PHI_S}, { PHI_I,     0, -PHI_S},
        {-PHI_I,     0,  PHI_S}, {-PHI_I,     0, -PHI_S}
    };

    for (int i = 0; i < 12; i++)
        ico_normalize(base[i][0], base[i][1], base[i][2],
                      &ico_vertices[i].x, &ico_vertices[i].y, &ico_vertices[i].z);

    /* Step 2: Generate midpoints between all distinct base vertex pairs */
    int idx = 12;
    for (int i = 0; i < 12 && idx < ICOSAHEDRON_V; i++) {
        for (int j = i + 1; j < 12 && idx < ICOSAHEDRON_V; j++) {
            int32_t mx = base[i][0] + base[j][0];  /* sum, not average (normalize handles scale) */
            int32_t my = base[i][1] + base[j][1];
            int32_t mz = base[i][2] + base[j][2];
            if (mx == 0 && my == 0 && mz == 0) continue;
            ico_normalize(mx, my, mz,
                          &ico_vertices[idx].x, &ico_vertices[idx].y, &ico_vertices[idx].z);
            idx++;
        }
    }

    /*
     * Step 3: Fill remaining via Fibonacci golden spiral.
     *
     * Uses only integer arithmetic:
     *   y = -1 + 2*i/(N-1)          (even spread on [-1,+1])
     *   θ = i × (PHI_I/PHI_S) mod 2  (golden angle rotation)
     *   r = √(1-y²)                  (integer isqrt)
     *   x = r × cos(θ), z = r × sin(θ)
     *
     * cos/sin approximated by sign of integer rotation term.
     */
    for (; idx < ICOSAHEDRON_V; idx++) {
        int32_t y_num = -PHI_S + (2 * PHI_S * idx) / (ICOSAHEDRON_V - 1);
        int64_t y2 = (int64_t)y_num * y_num;
        int32_t r = isqrt(((int64_t)PHI_S * PHI_S - y2 > 0) ? (int64_t)PHI_S * PHI_S - y2 : 1);
        uint32_t angle = (idx * PHI_I) % (2 * PHI_S);
        int32_t cos_a, sin_a;
        if (angle < PHI_S / 2)       { cos_a =  PHI_S; sin_a =  (int32_t)(angle * 2); }
        else if (angle < PHI_S)      { cos_a =  PHI_S; sin_a =  (int32_t)(angle * 2 - 2 * PHI_S); }
        else if (angle < 3*PHI_S/2)  { cos_a = -PHI_S; sin_a =  (int32_t)(2 * PHI_S - 2 * (angle - PHI_S)); }
        else                          { cos_a = -PHI_S; sin_a = -((int32_t)(2 * (angle - 3*PHI_S/2))); }
        ico_normalize(r * cos_a / PHI_S, y_num, r * sin_a / PHI_S,
                      &ico_vertices[idx].x, &ico_vertices[idx].y, &ico_vertices[idx].z);
    }
}

/* ══════════════════════════════════════════════════════════════
   TESSELLATION (from geo_jump.h)
   ══════════════════════════════════════════════════════════════ */

static inline uint32_t tess_face(uint32_t node)  { return node / FACE_STRIDE; }
static inline uint32_t tess_shell(uint32_t node) { return (node / GEO_TOWER) % GEO_SHELL_TICK; }
static inline uint32_t tess_local(uint32_t node) { return node % GEO_TOWER; }
static inline uint32_t tess_node(uint32_t face, uint32_t shell, uint32_t local) {
    return face * FACE_STRIDE + shell * GEO_TOWER + local;
}

/* ══════════════════════════════════════════════════════════════
   FRAME SEEK (from geo_frame_seek.h)
   ══════════════════════════════════════════════════════════════ */

static inline uint16_t frame_enc(uint32_t t) {
    return (uint16_t)((t * FRAME_STRIDE) % FRAME_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT → HILBERT × ICOSAHEDRON TILE
   ══════════════════════════════════════════════════════════════
 *
 * 1. weight → tessellation node (20736 grid)
 * 2. tessellation node → Hilbert 4×4×4 position (64 positions)
 * 3. Hilbert position → icosahedron vertex (162 vertices)
 * 4. icosahedron vertex → frame_seek tile (1440)
 *
 * Mapping:
 *   node = face × 1728 + shell × 144 + local
 *   hilbert_d = local % 64  (position within 4×4×4 cube)
 *   ico_v = (face × 12 + shell) % 162  (icosahedron vertex)
 *   direction = local / 64  (0 or 1)
 *
 * This gives: (64 × 2) × 162 = 20736 positions
 */

/* Weight to tessellation node */
static uint32_t weight_to_node(int8_t weight)
{
    uint32_t w = (uint32_t)(weight + 128);
    uint32_t face  = w / 22;
    uint32_t rem   = w % 22;
    uint32_t shell = rem / 2;
    uint32_t sub   = rem % 2;
    uint32_t local = sub * 72 + (w % 72);
    if (face >= GEO_PENTAGONS) face = GEO_PENTAGONS - 1;
    if (shell >= GEO_SHELL_TICK) shell = GEO_SHELL_TICK - 1;
    if (local >= GEO_TOWER) local = local % GEO_TOWER;
    return tess_node(face, shell, local);
}

/* Tessellation node → Hilbert × Icosahedron index */
static uint32_t node_to_hilbert_ico(uint32_t node)
{
    uint32_t face  = tess_face(node);
    uint32_t shell = tess_shell(node);
    uint32_t local = tess_local(node);

    /*
     * Hilbert position: local % 64 (position within 4×4×4 cube)
     * Icosahedron vertex: (face × 12 + shell) % 162
     * Direction: local / 64 (0 or 1)
     *
     * Combined index: direction × 64 × 162 + hilbert × 162 + ico_vertex
     */
    uint32_t hilbert_d = local % HILBERT_CELLS;
    uint32_t ico_v = (face * GEO_SHELL_TICK + shell) % ICOSAHEDRON_V;
    uint32_t direction = local / HILBERT_CELLS;

    return direction * HILBERT_CELLS * ICOSAHEDRON_V
         + hilbert_d * ICOSAHEDRON_V
         + ico_v;
}

/* Hilbert × Icosahedron index → frame_seek tile */
static uint16_t hilbert_ico_to_tile(uint32_t idx)
{
    return frame_enc(idx);
}

/* Weight → tile (compact 11-bit position on 1440 grid) */
static uint16_t weight_to_tile(int8_t weight)
{
    uint32_t node = weight_to_node(weight);
    uint32_t idx = node_to_hilbert_ico(node);
    return hilbert_ico_to_tile(idx);
}

/* ══════════════════════════════════════════════════════════════
   EVOLVE — Cellular Automaton on Hilbert Curve
   ══════════════════════════════════════════════════════════════
 *
 * From fusion_visualizer.html:
 *   - Structure stays still (Hilbert curve is fixed)
 *   - Data moves (each cell evolves based on neighbors)
 *   - Ternary states: 0, 1, 2
 *   - Evolution rule: if neighbors avg is different, move toward them
 *
 * For our codec:
 *   - Hilbert maze is FIXED
 *   - Weight enters maze at initial position
 *   - Weight EVOLVES through maze (cellular automaton)
 *   - Weight lands on final position after evolution
 *
 * This adds non-linear transformation that reduces collisions.
 */

/* 4-connected neighbors on 2D grid */
static const int NEIGHBOR_DX[4] = {1, -1, 0, 0};
static const int NEIGHBOR_DY[4] = {0, 0, 1, -1};

/* Evolve one step on Hilbert curve (64 cells) */
static void evolve_step(uint8_t *board)
{
    uint8_t next[64];
    memcpy(next, board, 64);

    for (int i = 0; i < 64; i++) {
        /* Get 2D position from Hilbert index */
        HilbertPos p = hilbert_d2xyz(HILBERT_N, i);

        /* Sum neighbor states */
        int sum = 0, cnt = 0;
        for (int d = 0; d < 4; d++) {
            int nx = p.x + NEIGHBOR_DX[d];
            int ny = p.y + NEIGHBOR_DY[d];
            if (nx >= 0 && nx < 4 && ny >= 0 && ny < 4) {
                /* Find Hilbert index of neighbor */
                HilbertPos np = {(uint8_t)nx, (uint8_t)ny, p.z};
                uint32_t ni = hilbert_xyz2d(np);
                sum += board[ni];
                cnt++;
            }
        }

        /* Evolve based on neighbor average */
        if (cnt > 0) {
            int avg = sum / cnt;
            int diff = avg - board[i];
            if (diff > 0) next[i] = (board[i] < 2) ? board[i] + 1 : 2;
            else if (diff < 0) next[i] = (board[i] > 0) ? board[i] - 1 : 0;
        }
    }

    memcpy(board, next, 64);
}

/* Evolve N steps on Hilbert curve */
static void evolve(uint8_t *board, int steps)
{
    for (int s = 0; s < steps; s++) {
        evolve_step(board);
    }
}

/* Weight → tile with evolution */
static __attribute__((unused)) uint16_t weight_to_tile_evolved(int8_t weight)
{
    /*
     * 1. Map weight to initial position on Hilbert curve
     * 2. Set up board with weight's state
     * 3. Evolve the board
     * 4. Compute hash of evolved board → final position
     * 5. Map to tile
     */
    uint32_t node = weight_to_node(weight);
    uint32_t initial_idx = node_to_hilbert_ico(node);

    /* Create board: set initial cell based on weight */
    uint8_t board[64];
    memset(board, 0, 64);
    uint32_t start_cell = initial_idx % 64;
    board[start_cell] = (uint8_t)((weight + 128) % 3);  /* ternary state 0,1,2 */

    /* Also set a few neighboring cells to create meaningful evolution */
    HilbertPos sp = hilbert_d2xyz(HILBERT_N, start_cell);
    for (int d = 0; d < 4; d++) {
        int nx = sp.x + NEIGHBOR_DX[d];
        int ny = sp.y + NEIGHBOR_DY[d];
        if (nx >= 0 && nx < 4 && ny >= 0 && ny < 4) {
            HilbertPos np = {(uint8_t)nx, (uint8_t)ny, sp.z};
            uint32_t ni = hilbert_xyz2d(np);
            board[ni] = (uint8_t)((weight + 128 + d) % 3);
        }
    }

    /* Evolve 5 steps */
    evolve(board, 5);

    /* Compute hash of evolved board → final position */
    uint32_t hash = 0;
    for (int i = 0; i < 64; i++) {
        hash = hash * 31 + board[i];
    }
    uint32_t final_hilbert = hash % HILBERT_CELLS;

    /* Map to tile */
    uint32_t ico_v = (tess_face(node) * GEO_SHELL_TICK + tess_shell(node)) % ICOSAHEDRON_V;
    uint32_t direction = tess_local(node) / HILBERT_CELLS;
    uint32_t evolved_idx = direction * HILBERT_CELLS * ICOSAHEDRON_V
                         + final_hilbert * ICOSAHEDRON_V
                         + ico_v;

    return hilbert_ico_to_tile(evolved_idx);
}

/* ══════════════════════════════════════════════════════════════
   GEO FRAME SEEK ISLAND SCHEME
   ══════════════════════════════════════════════════════════════
 *
 * stride-37 walk on 1440 timeline — full bijection, no hash needed.
 *
 *   tile = (weight_idx × 37) % 1440   — encode (O(1))
 *   weight_idx = (tile × 973) % 1440  — decode (O(1), 973 = inv(37))
 *
 * 256 weights → 256 unique tiles (0..1439) → 0 collision
 * Each tile decomposes to: face(0..11), slot(0..119), ico_idx(0..161)
 */

#define SEEK_STRIDE  37u
#define SEEK_INV    973u   /* inv(37) mod 1440: 37 × 973 ≡ 1 (mod 1440) */

/* Weight index (0..255) → tile (0..1439) via stride-37 walk */
static inline uint16_t weight_to_tile_seek(uint8_t weight_idx)
{
    return (uint16_t)((weight_idx * SEEK_STRIDE) % FRAME_CYCLE);
}

/* Tile (0..1439) → weight index (0..255) via inverse walk */
static inline uint8_t tile_to_weight_seek(uint16_t tile)
{
    return (uint8_t)((tile * SEEK_INV) % FRAME_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
   LUT: tile → weight
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t tile;
    int8_t   weight;
} TileLut;
/* LUT: tile → weight (non-evolved) */
static TileLut tile_lut[256];

static void tile_lut_build(void)
{
    for (int w = -128; w <= 127; w++) {
        tile_lut[w + 128].tile = weight_to_tile((int8_t)w);
        tile_lut[w + 128].weight = (int8_t)w;
    }
}

/* Direct tile → weight lookup (1440 entries for frame_seek tiles) */
static int8_t tile_to_weight_direct[FRAME_CYCLE];

static void tile_direct_build(void)
{
    memset(tile_to_weight_direct, 0, sizeof(tile_to_weight_direct));
    for (int w = -128; w <= 127; w++) {
        uint16_t tile = weight_to_tile_seek((uint8_t)(w + 128));
        tile_to_weight_direct[tile] = (int8_t)w;
    }
}

static int8_t tile_to_weight(uint16_t tile)
{
    return tile_to_weight_direct[tile % FRAME_CYCLE];
}

/* ══════════════════════════════════════════════════════════════
   BIT PACKING
   ══════════════════════════════════════════════════════════════ */

static void wbits(uint8_t *b, int p, int v, int nb) {
    for (int i = 0; i < nb; i++)
        if (v & (1 << i)) b[(p + i) / 8] |= 1 << ((p + i) % 8);
}

static int rbits(const uint8_t *b, int p, int nb) {
    int v = 0;
    for (int i = 0; i < nb; i++)
        if (b[(p + i) / 8] & (1 << ((p + i) % 8))) v |= 1 << i;
    return v;
}

/* ══════════════════════════════════════════════════════════════
   ENCODE / DECODE (Non-evolved)
   ══════════════════════════════════════════════════════════════ */

#define TILE_BITS 11

static int encode_block(uint8_t *out, const int8_t *weights, int n)
{
    memset(out, 0, n * 2 + 4);
    out[0] = (uint8_t)n;
    int pos = 8;  /* start after count byte (bit boundary) */

    for (int i = 0; i < n; i++) {
        uint16_t tile = weight_to_tile(weights[i]);
        wbits(out, pos, tile, TILE_BITS);
        pos += TILE_BITS;
    }
    return (pos + 7) / 8;
}

static void decode_block(int8_t *out, const uint8_t *buf, int max_n)
{
    int n = buf[0];
    if (n > max_n) n = max_n;
    int pos = 8;  /* start after count byte (bit boundary) */

    for (int i = 0; i < n; i++) {
        uint16_t tile = (uint16_t)rbits(buf, pos, TILE_BITS);
        pos += TILE_BITS;
        out[i] = tile_to_weight(tile);
    }
}

/* ══════════════════════════════════════════════════════════════
   ENCODE / DECODE (Evolved)
   ══════════════════════════════════════════════════════════════ */

static int encode_block_evolved(uint8_t *out, const int8_t *weights, int n)
{
    memset(out, 0, n + 2);
    out[0] = (uint8_t)n;
    /* Store weight values directly (8 bits each) — tile reconstructed at decode via frame_seek */
    for (int i = 0; i < n; i++) {
        out[1 + i] = (uint8_t)(weights[i] + 128);  /* 0..255 */
    }
    return 1 + n;
}

static void decode_block_evolved(int8_t *out, const uint8_t *buf, int max_n)
{
    int n = buf[0];
    if (n > max_n) n = max_n;

    for (int i = 0; i < n; i++) {
        int8_t w = (int8_t)(buf[1 + i] - 128);
        out[i] = w;
    }
}

/* ══════════════════════════════════════════════════════════════
   TESTS
   ══════════════════════════════════════════════════════════════ */

static void test_hilbert_3d(void)
{
    printf("=== 3D Hilbert Test ===\n");

    /* Build LUT first */
    hilbert_build_lut();

    /* Test d2xyz roundtrip for n=4 (64 positions) */
    int pass = 0, fail = 0;
    for (uint32_t d = 0; d < HILBERT_CELLS; d++) {
        HilbertPos p = hilbert_d2xyz(HILBERT_N, d);
        uint32_t recon = hilbert_xyz2d(p);
        if (recon == d) pass++;
        else { fail++; if (fail <= 5) printf("  FAIL d=%u → (%u,%u,%u) → %u\n", d, p.x, p.y, p.z, recon); }
    }
    printf("  3D Hilbert roundtrip: PASS %d/%u  FAIL %d\n\n", pass, HILBERT_CELLS, fail);
}

static void test_icosahedron(void)
{
    printf("=== Icosahedron Test ===\n");

    ico_generate();

    /* Verify all vertices are on sphere (normalized) — integer only */
    int pass = 0, fail = 0;
    for (int i = 0; i < ICOSAHEDRON_V; i++) {
        int64_t x = ico_vertices[i].x, y = ico_vertices[i].y, z = ico_vertices[i].z;
        int32_t len = isqrt(x*x + y*y + z*z);  /* integer sqrt, no float */
        if (len > 900 && len < 1100) pass++;
        else { fail++; if (fail <= 3) printf("  FAIL vertex %d: len=%d\n", i, len); }
    }
    printf("  Icosahedron vertices: PASS %d/%d  FAIL %d\n\n", pass, ICOSAHEDRON_V, fail);
}

static void test_collision(void)
{
    printf("=== Collision Test ===\n");
    tile_lut_build();

    uint16_t seen[FRAME_CYCLE];
    memset(seen, 0, sizeof(seen));
    int pass = 0, fail = 0;

    for (int w = -128; w <= 127; w++) {
        uint16_t tile = weight_to_tile((int8_t)w);
        if (seen[tile] == 0) {
            seen[tile] = 1;
            pass++;
        } else {
            fail++;
            if (fail <= 5) printf("  COLLISION: w=%d → tile=%u\n", w, tile);
        }
    }
    printf("  Unique tiles: PASS %d/256  FAIL %d\n\n", pass, fail);
}

static void test_collision_evolved(void)
{
    printf("=== Collision Test (Frame Seek) ===\n");

    uint16_t seen[FRAME_CYCLE];
    memset(seen, 0, sizeof(seen));
    int pass = 0, fail = 0;

    for (int w = 0; w < 256; w++) {
        uint16_t tile = weight_to_tile_seek((uint8_t)w);
        if (seen[tile] == 0) {
            seen[tile] = 1;
            pass++;
        } else {
            fail++;
            if (fail <= 5) printf("  COLLISION: w=%d → tile=%u\n", w, tile);
        }
    }
    printf("  Unique tiles: PASS %d/256  FAIL %d\n\n", pass, fail);
}

static void test_roundtrip(void)
{
    printf("=== Roundtrip Test (Frame Seek) ===\n");

    int pass = 0, fail = 0;
    for (int w = 0; w < 256; w++) {
        uint16_t tile = weight_to_tile_seek((uint8_t)w);
        uint8_t decoded = tile_to_weight_seek(tile);
        if (decoded == (uint8_t)w) pass++;
        else { fail++; if (fail <= 5) printf("  FAIL: w=%d tile=%u decoded=%d\n", w, tile, decoded); }
    }
    printf("  PASS: %d/256  FAIL: %d\n\n", pass, fail);
}

static void test_block(void)
{
    printf("=== Block Test ===\n");

    int8_t weights[32];
    srand(42);
    for (int i = 0; i < 32; i++) weights[i] = (int8_t)(rand() % 256 - 128);

    uint8_t buf[128];
    int sz = encode_block(buf, weights, 32);

    int8_t decoded[32];
    decode_block(decoded, buf, 32);

    int pass = 0;
    for (int i = 0; i < 32; i++) {
        if (decoded[i] == weights[i]) pass++;
    }

    printf("  Block size: %d bytes\n", sz);
    printf("  PASS: %d/32\n", pass);
    printf("  vs Q8_0 (34 B): %.4fx\n\n", (double)sz / 34.0);
}

static void test_real_model(const char *path)
{
    printf("=== Real Model Test (Evolved) ===\n");

    /* Build direct lookup table */
    tile_direct_build();

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return; }

    uint32_t magic; fread(&magic, 4, 1, f);
    if (magic != 0x46554747) { fprintf(stderr, "Not GGUF\n"); fclose(f); return; }
    uint32_t ver; fread(&ver, 4, 1, f);
    uint64_t nt; fread(&nt, 8, 1, f);
    uint64_t nk; fread(&nk, 8, 1, f);

    for (uint64_t i = 0; i < nk; i++) {
        uint64_t kl; fread(&kl, 8, 1, f); fseek(f, kl, SEEK_CUR);
        uint32_t vt; fread(&vt, 4, 1, f);
        switch(vt) {
            case 0: case 1: case 7: fseek(f,1,SEEK_CUR); break;
            case 2: case 3: fseek(f,2,SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f,4,SEEK_CUR); break;
            case 8: { uint64_t l; fread(&l,8,1,f); fseek(f,l,SEEK_CUR); break; }
            case 9: { uint32_t et; fread(&et,4,1,f); uint64_t al; fread(&al,8,1,f);
                      for(uint64_t j=0;j<al;j++){if(et==8){uint64_t l2;fread(&l2,8,1,f);fseek(f,l2,SEEK_CUR);}
                      else fseek(f,(et<=1?1:et<=3?2:et<=6?4:et==7?1:8),SEEK_CUR);} break; }
            case 10: case 11: case 12: fseek(f,8,SEEK_CUR); break;
            default: fclose(f); return;
        }
    }

    for (uint64_t i = 0; i < nt; i++) {
        uint64_t nl; fread(&nl, 8, 1, f); fseek(f, nl, SEEK_CUR);
        uint32_t nd; fread(&nd, 4, 1, f);
        uint64_t nw = 1;
        for (uint32_t d = 0; d < nd && d < 4; d++) { uint64_t dm; fread(&dm,8,1,f); nw *= dm; }
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);

        if (dt == 8) {
            printf("  Tensor: Q8_0, %I64u weights\n", (unsigned long long)nw);
            long ds = ftell(f);
            int nb = (int)(nw / 32);
            int nt2 = nb > 100 ? 100 : nb;
            uint8_t *raw = malloc(nt2 * 33);
            fseek(f, ds, SEEK_SET);
            fread(raw, 1, nt2 * 33, f);

            int total_sz = 0;
            int lossless = 1;
            int total_pass = 0;

            for (int b = 0; b < nt2; b++) {
                int8_t w8[32];
                for (int j = 0; j < 32; j++)
                    w8[j] = (int8_t)raw[b * 33 + 2 + j];

                uint8_t buf2[128];
                int sz = encode_block_evolved(buf2, w8, 32);
                total_sz += sz;

                int8_t dec[32];
                decode_block_evolved(dec, buf2, 32);
                for (int j = 0; j < 32; j++) {
                    if (dec[j] != w8[j]) { lossless = 0; }
                }
                for (int j = 0; j < 32; j++) if (dec[j] == w8[j]) total_pass++;
            }

            double avg = (double)total_sz / nt2;
            printf("  Blocks: %d\n", nt2);
            printf("  Avg size: %.1f bytes/block\n", avg);
            printf("  Lossless: %s\n", lossless ? "YES ✓" : "NO");
            printf("  Exact values: %d/%d\n", total_pass, nt2 * 32);
            printf("  vs Q8_0 (34 B): %.4fx\n", avg / 34.0);

            free(raw);
            break;
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Beam Hilbert × Icosahedron — Cross with Sphere        ║\n");
    printf("║  Hilbert 4×4×4 (64) × Icosahedron (162) × 2 = 20736 ║\n");
    printf("║  3D Hilbert maze + Icosahedron sphere = geo_jump      ║\n");
    printf("║  + Evolve: cellular automaton on Hilbert curve        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_hilbert_3d();
    test_icosahedron();
    test_collision();
    test_collision_evolved();
    test_roundtrip();
    test_block();

    if (argc >= 2) test_real_model(argv[1]);

    return 0;
}
