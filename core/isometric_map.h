/*
 * isometric_map.h — Unified Isometric Cube Addressing
 * ═══════════════════════════════════════════════════════════════
 *
 * Synthesizes recall findings into one practical header:
 *   - Isometric projection: 3 cube faces → hex tiling (image 1)
 *   - Peano traversal: space-filling curve across faces (image 2)
 *   - skeleton_lookup: stride-37 O(1) zone/enc (skeleton_index.h)
 *   - CRC32C signature: hardware-accelerated cache key (g7b.cpp)
 *   - Gear system: adaptive 128*n slots (bermuda v3)
 *   - Ghost delete + signed state: fail-deadly proof (ehcp_poc.c)
 *   - ThirdEye film model: accumulate/develop cycle (handoff_v424)
 *   - Hex-19: 13-node core + 36 residual triangles per face
 *   - PFC: topology stable, subdivision replaceable
 *   - 17n lattice: 17/18/144/289/162 sacred constants
 *
 * All int, no float, no malloc, no hash tables.
 * Coordinate = address. MAP not COMPRESS.
 *
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef ISOMETRIC_MAP_H
#define ISOMETRIC_MAP_H

#include <stdint.h>
#include <string.h>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif

/* ── Sacred Constants (frozen) ─────────────────────────────── */

#define IM_FACES          12u    /* dodecahedron faces                    */
#define IM_FACE_SZ        60u    /* slots per face (12 × 60 = 720)        */
#define IM_WALK_LEN       720u   /* full walk: 12 faces × 60 slots        */
#define IM_STRIDE         37u    /* prime, gcd(37, 2^k) = 1 for all k    */
#define IM_HEX19_CORE     13u    /* center + ring1 + 6 anchors            */
#define IM_HEX19_TOTAL    19u    /* full hex cluster per face             */
#define IM_HEX19_SCRATCH  36u    /* residual triangles per face           */
#define IM_HEX19_ALL_SCRATCH 432u /* 12 × 36 total scratch               */
#define IM_GEO_FULL       144u   /* = 12 × 12, geometry anchor            */
#define IM_LATTICE_17     17u    /* bridge prime, axis of symmetry        */
#define IM_GATE_18        18u    /* crossing event (17→18→17)              */
#define IM_DYNAMICS       144u   /* active space = Fib(12) = 12²           */
#define IM_LATTICE_289    289u   /* 17² full lattice                      */
#define IM_NODE_MAX       162u   /* 2 × 3⁴, World A×B junction            */
#define IM_FOUR_SIDES     4u     /* 4 directions on square grid (Peano)   */

/* ── Axes ──────────────────────────────────────────────────── */

typedef enum {
    IM_AXIS_X  = 0,   /* World A — binary, structured, CPU              */
    IM_AXIS_Y  = 1,   /* World B — ternary, adaptive, GPU               */
    IM_AXIS_Z  = 2,   /* World C — bridge prime, routing                 */
    IM_AXIS_NA = 3    /* unused / self-partner                          */
} im_axis_t;

/* ── Signed State (from ehcp_poc.c) ────────────────────────── */

typedef enum {
    IM_STATE_ACTIVE   =  1,   /* live, routeable                         */
    IM_STATE_GHOST    =  0,   /* deleted, path amnesia                   */
    IM_STATE_REVERSED = -1    /* direction flipped                       */
} im_state_t;

/* ── ThirdEye Film Stage (from handoff_v424) ────────────────── */

typedef enum {
    IM_FILM_ACCUMULATE = 0,   /* te_expose: silent accumulate            */
    IM_FILM_DEVELOP    = 1    /* te_develop: flush at 144 ops            */
} im_film_stage_t;

/* ── Face-Isometric Mapping Table ───────────────────────────── *
 *  Each row: (axis, polarity)                                      *
 *  polarity 0 = positive pole (ring 1)                              *
 *  polarity 1 = negative pole (ring 2)                              *
 *                                                                   *
 *  Derivation: hex-19 expansion modules and PFC boundary contract. */

typedef struct {
    uint8_t axis;       /* im_axis_t */
    uint8_t polarity;   /* 0 = positive pole, 1 = negative pole        */
} im_face_t;

static const im_face_t IM_FACE_TABLE[IM_FACES] = {
    /* face  0: X+, positive pole */ { IM_AXIS_X, 0 },
    /* face  1: Y+, positive pole */ { IM_AXIS_Y, 0 },
    /* face  2: Z+, positive pole */ { IM_AXIS_Z, 0 },
    /* face  3: X-, positive pole */ { IM_AXIS_X, 0 },
    /* face  4: Y-, positive pole */ { IM_AXIS_Y, 0 },
    /* face  5: Z-, positive pole */ { IM_AXIS_Z, 0 },
    /* face  6: X+, negative pole */ { IM_AXIS_X, 1 },
    /* face  7: Y+, negative pole */ { IM_AXIS_Y, 1 },
    /* face  8: Z+, negative pole */ { IM_AXIS_Z, 1 },
    /* face  9: X-, negative pole */ { IM_AXIS_X, 1 },
    /* face 10: Y-, negative pole */ { IM_AXIS_Y, 1 },
    /* face 11: Z-, negative pole */ { IM_AXIS_Z, 1 },
};

/* ── Metatron Cross Partner (from skeleton_index.h) ───────── *
 *  Self-inverse: CROSS[CROSS[f]] == f                               *
 *  GOLD: partner=face, no-op. CROSS: partner opposite ring.         */

static const uint8_t IM_CROSS[IM_FACES] = {
    9, 10, 11, 6, 7, 8,    /* ring 1 → ring 2 */
    3,  4,  5, 0, 1, 2,    /* ring 2 → ring 1 */
};

/* ── Peano Curve LUTs (from isometric cube image) ─────────── *
 *  2D ↔ 1D bit-interleaving for square subgrid within each face. */

static const uint8_t IM_PEANO_X[IM_FOUR_SIDES] = { 0, 0, 1, 1 };
static const uint8_t IM_PEANO_Y[IM_FOUR_SIDES] = { 0, 1, 1, 0 };

/* ── Gear Table (from bermuda v3) ─────────────────────────── *
 *  128×n slots, each gear's WALK_LEN = smallest 12-multiple       *
 *  ≥ slots with gcd(37, WALK_LEN) = 1.                           */

typedef struct {
    uint16_t slots;      /* total addressable slots                   */
    uint16_t wl;         /* walk length (coprime 37)                  */
    uint8_t  order;      /* peano order = log2(slots / IM_GEO_FULL)  */
    uint8_t  pad;        /* slots - N for snap_gear                   */
} im_gear_t;

#define IM_GEAR_BASE  IM_GEO_FULL   /* 144 */
#define IM_GEAR_COUNT 4u

static const im_gear_t IM_GEARS[IM_GEAR_COUNT] = {
    /* gear 0: n=4  */ {  512,  516, 1, 0 },   /* pad = 512 - 512 = 0 */
    /* gear 1: n=8  */ { 1024, 1032, 2, 0 },   /* ANCHOR, gate_18 clean */
    /* gear 2: n=16 */ { 2048, 2052, 3, 0 },
    /* gear 3: n=32 */ { 4096, 4104, 4, 0 },
};

/* ── Data Structures ───────────────────────────────────────── */

/* skeleton_lookup result (from skeleton_index.h) */
typedef struct {
    uint8_t  zone;      /* 0..11 pentagon sector                       */
    uint8_t  axis;      /* im_axis_t X/Y/Z                             */
    uint8_t  polarity;  /* 0 = positive, 1 = negative                  */
    uint8_t  partner;   /* 0..11 Metatron cross partner                */
    uint16_t enc;       /* 0..719 walk position                        */
    uint8_t  peano;     /* 0..3 sub-grid position within cell           */
} im_skel_t;

/* Full isometric address decomposition */
typedef struct {
    uint16_t addr;      /* flat address 0..719                         */
    im_skel_t skel;     /* skeleton lookup result                      */
    uint8_t  gear;      /* active gear index 0..3                      */
    uint8_t  rhombus;   /* local rhombus index 0..18 within face       */
} im_addr_t;

/* Ghost node state (from ehcp_poc.c) — append-only, never clear occupied */
typedef struct {
    uint32_t address;       /* Hilbert/rhombus address                  */
    uint32_t pair_addr;     /* paired node address                     */
    int8_t   state;         /* im_state_t: +1 / 0 / -1                 */
    uint8_t  occupied;      /* slot taken — NEVER cleared              */
} im_ghost_t;

/* ThirdEye Film — accumulate silently, develop every 144 ops           */
typedef struct {
    uint64_t theta_sum;     /* accumulated theta (expose per op)        */
    uint32_t op_count;      /* counts toward 144                        */
    uint8_t  stage;         /* im_film_stage_t                          */
} im_film_t;

/* CRC32C accumulator (from g7b.cpp — hardware CRC32C path)             */
typedef struct {
    uint64_t state[6];      /* 384-bit state (same as StateR)           */
    uint32_t crc;           /* CRC32C signature                         */
} im_sig_t;

/* ── Core Functions ─────────────────────────────────────────── */

/*
 * skeleton_lookup — O(1) address → geometry coordinates.
 * 7 ops, 0 branch. Source: skeleton_index.h.
 */
static inline im_skel_t im_skel(uint16_t addr)
{
    im_skel_t s;
    uint16_t pos = (uint16_t)(addr % IM_WALK_LEN);
    s.enc    = (uint16_t)((pos * IM_STRIDE) % IM_WALK_LEN);
    s.zone   = (uint8_t)(s.enc / IM_FACE_SZ);
    s.axis   = IM_FACE_TABLE[s.zone].axis;
    s.polarity = IM_FACE_TABLE[s.zone].polarity;
    s.partner  = IM_CROSS[s.zone];
    s.peano    = 0;
    return s;
}

/*
 * im_decompose — flat address → full isometric decomposition.
 * One skeleton_lookup + gear determination.
 */
static inline im_addr_t im_decompose(uint16_t addr)
{
    im_addr_t a;
    a.addr = addr;
    a.skel = im_skel(addr);
    a.gear = 0;
    a.rhombus = (uint8_t)(a.skel.enc % IM_HEX19_CORE);
    return a;
}

/*
 * im_face_axis — return axis label for a face index.
 */
static inline im_axis_t im_face_axis(uint8_t zone)
{
    return (im_axis_t)IM_FACE_TABLE[zone].axis;
}

/*
 * im_face_polarity — return polarity (0=positive, 1=negative).
 */
static inline uint8_t im_face_polarity(uint8_t zone)
{
    return IM_FACE_TABLE[zone].polarity;
}

/*
 * im_face_partner — return Metatron cross partner face.
 */
static inline uint8_t im_face_partner(uint8_t zone)
{
    return IM_CROSS[zone];
}

/* ── Isometric Projection (from cube image) ────────────────── *
 *                                                                   *
 *  Standard isometric angles:                                        *
 *    X-axis = 30° (right-down)                                       *
 *    Y-axis = 150° (left-down)                                       *
 *    Z-axis = 90° (straight up)                                      *
 *                                                                   *
 *  cos(30°) = 0.866, sin(30°) = 0.5                                 *
 *  2/sqrt(3) ≈ 1.155, 1/sqrt(3) ≈ 0.577                             *
 *                                                                   *
 *  Project cube (x,y,z) → screen (sx, sy):                           *
 *    sx = (x - y) * cos30                                            *
 *    sy = -z + (x + y) * sin30                                       */

#define IM_COS30_NUM   866   /* cos(30°) × 1000                       */
#define IM_COS30_DEN  1000
#define IM_SIN30_NUM     1   /* sin(30°) = 0.5 → 500/1000             */
#define IM_SIN30_DEN     2
#define IM_2_SQRT3_NUM   2
#define IM_2_SQRT3_DEN   3   /* 2/sqrt(3) ≈ 1.155                     */

typedef struct {
    int16_t sx;         /* screen x                                    */
    int16_t sy;         /* screen y                                    */
} im_screen_t;

typedef struct {
    uint8_t axis;       /* X=0, Y=1, Z=2                              */
    uint8_t t;          /* 0..IM_FACE_SZ-1 sub-position on axis        */
} im_local_t;

/*
 * im_project — cube (x,y,z) → isometric screen (sx, sy).
 * Integer-only: multiply by 866/1000 for cos30.
 */
static inline im_screen_t im_project(int16_t x, int16_t y, int16_t z)
{
    im_screen_t s;
    s.sx = (int16_t)(((int32_t)(x - y) * IM_COS30_NUM) / IM_COS30_DEN);
    s.sy = (int16_t)(-z + ((int32_t)(x + y) * IM_SIN30_NUM) / IM_SIN30_DEN);
    return s;
}

/*
 * im_rhombus_param — given sub_addr within a face, compute
 *   local rhombus coordinates and sub-axis.
 *
 *   rhombus_size = floor(sqrt(IM_FACE_SZ)) = 7 (7×7 = 49 < 60)
 *   For 60 slots: 7×8 + 4 extra, or use 6×10 = 60.
 *
 *   Simpler: 60 = 6 × 10. 6 rows, 10 columns.
 *   Or: 60 = 5 × 12. 5 sub-positions along each axis within face.
 *
 *   Using hex-19: 19 cells in 6 directions from center.
 *   Rhombus param: axis + t (0..59 mapped to direction + offset).
 */
static inline im_local_t im_rhombus_param(uint16_t sub_addr)
{
    im_local_t l;
    uint8_t dir  = (uint8_t)((sub_addr / 10) % 6);
    l.t = (uint8_t)(sub_addr % 10);
    l.axis = IM_FACE_TABLE[dir % IM_FACES].axis;
    return l;
}

/* ── Peano Curve Traversal (from isometric cube image) ─────── *
 *                                                                   *
 *  Peano curve on 2² = 4 cells:                                     *
 *    0 → (0,0), 1 → (0,1), 2 → (1,1), 3 → (1,0)                   *
 *                                                                   *
 *  This is bit-interleaving: d=0→x=00,y=00, d=1→x=01,y=01, etc.     *
 *  Generalizes to higher orders via iterative bit-interleave.       *
 *                                                                   *
 *  The curve traverses all 3 cube faces continuously:               *
 *    Top (Z) → Left (X) → Right (Y) → back to Top                   *
 *  with no break at face boundaries.                                 */

static inline void im_peano_xy(uint32_t d, uint32_t order,
                                uint32_t *out_x, uint32_t *out_y)
{
    uint32_t x = 0, y = 0;
    for (uint32_t i = 0; i < order; i++) {
        uint32_t shift = 2 * i;
        uint32_t bits  = (d >> shift) & 3;
        x |= IM_PEANO_X[bits] << shift;
        y |= IM_PEANO_Y[bits] << shift;
    }
    *out_x = x;
    *out_y = y;
}

static inline uint32_t im_peano_d(uint32_t x, uint32_t y, uint32_t order)
{
    uint32_t d = 0;
    for (uint32_t i = 0; i < order; i++) {
        uint32_t shift = 2 * i;
        uint32_t x2 = (x >> shift) & 1;
        uint32_t y2 = (y >> shift) & 1;
        uint32_t idx = (y2 << 1) | x2;
        /* Peano inversion: find which d gives this (x,y) pair        */
        for (uint32_t j = 0; j < IM_FOUR_SIDES; j++) {
            if (IM_PEANO_X[j] == x2 && IM_PEANO_Y[j] == y2) {
                idx = j;
                break;
            }
        }
        d |= idx << shift;
    }
    return d;
}

/*
 * im_peano_traverse — next address following Peano order within face.
 * cur is a Peano index (1D position on curve); returns cur+1 mod 4^order.
 */
static inline uint32_t im_peano_traverse(uint32_t cur, uint32_t order)
{
    uint32_t total = 1u << (2 * order);
    return (cur + 1u) % total;
}

/* ── Gear System (from bermuda v3) ─────────────────────────── */

/*
 * im_snap_gear — find smallest gear that fits N slots.
 * Returns gear index 0..3.
 */
static inline uint8_t im_snap_gear(uint16_t n)
{
    for (uint8_t g = 0; g < IM_GEAR_COUNT; g++) {
        if (n <= IM_GEARS[g].slots) return g;
    }
    return IM_GEAR_COUNT - 1;
}

/*
 * im_gear_walk_len — WALK_LEN for a given gear.
 * Smallest 12-multiple ≥ slots, coprime with 37.
 */
static inline uint16_t im_gear_walk_len(uint8_t gear)
{
    return IM_GEARS[gear].wl;
}

/*
 * im_gear_modinv — modular inverse of 37 mod WL.
 * Used for CROSS self-inverse: enc→zone→CROSS→invert→addr.
 */
static inline uint16_t im_modinv37(uint16_t wl)
{
    int32_t t = 0, newt = 1;
    int32_t r = (int32_t)wl, newr = (int32_t)IM_STRIDE;
    while (newr != 0) {
        int32_t q = r / newr;
        int32_t tmp = newt; newt = t - q * newt; t = tmp;
        tmp = newr; newr = r - q * newr; r = tmp;
    }
    if (t < 0) t += (int32_t)wl;
    return (uint16_t)t;
}

/* ── CRC32C Signature (from g7b.cpp) ──────────────────────── *
 *  Software CRC32C using polynomial 0x1EDC6F41.                     *
 *  Hardware path (SSE4.2): _mm_crc32_u64 — faster.                  */

#define IM_CRC32C_POLY 0x1EDC6F41u

#ifdef __SSE4_2__

/*
 * Hardware CRC32C — SSE4.2 _mm_crc32_u64.
 * ~60ns for 64B vs ~830ns software (10× faster).
 */
static inline uint32_t im_crc32c_hw(uint32_t crc, uint64_t v)
{
    return _mm_crc32_u64(crc, v);
}

static inline uint32_t im_crc32c_block(const uint8_t *block, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    /* process 8 bytes at a time */
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t v;
        memcpy(&v, block + i, 8);
        crc = _mm_crc32_u64(crc, v);
    }
    /* remaining bytes */
    for (; i < len; i++)
        crc = _mm_crc32_u32(crc, block[i]);
    return crc ^ 0xFFFFFFFFu;
}

static inline uint32_t im_sig_fast(const uint64_t state[6])
{
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 6; i++)
        crc = _mm_crc32_u64(crc, state[i]);
    return crc ^ 0xFFFFFFFFu;
}

#else /* software fallback */

static inline uint32_t im_crc32c_byte(uint32_t crc, uint8_t b)
{
    crc ^= (uint32_t)b;
    for (int i = 0; i < 8; i++) {
        uint32_t mask = -(crc & 1u);
        crc = (crc >> 1) ^ (IM_CRC32C_POLY & mask);
    }
    return crc;
}

static inline uint32_t im_crc32c_block(const uint8_t *block, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++)
        crc = im_crc32c_byte(crc, block[i]);
    return crc ^ 0xFFFFFFFFu;
}

static inline uint32_t im_sig_fast(const uint64_t state[6])
{
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)state;
    for (int i = 0; i < 48; i++)
        crc = im_crc32c_byte(crc, p[i]);
    return crc ^ 0xFFFFFFFFu;
}

#endif /* __SSE4_2__ */

/* ── ThirdEye Film (from handoff_v424) ─────────────────────── *
 *                                                                   *
 *  Original: thirdeye_eval() every op → branch on state every frame  *
 *  New: te_expose() every op → accumulate silently                   *
 *       te_develop() every 144 ops → read pattern, reset roll        *
 *                                                                   *
 *  Film model: ThirdEye doesn't know what's there until              *
 *  the film is flushed (developed).                                  */

static inline void im_film_init(im_film_t *f)
{
    f->theta_sum = 0;
    f->op_count  = 0;
    f->stage     = IM_FILM_ACCUMULATE;
}

/*
 * im_film_expose — called every op. Accumulates theta silently.
 * No branch on state, no eval. Pure accumulation.
 */
static inline void im_film_expose(im_film_t *f, uint64_t theta)
{
    f->theta_sum += theta;
    f->op_count++;
}

/*
 * im_film_develop — called every 144 ops. Returns 1 if film is ready.
 * After develop, caller reads theta_sum as aggregate signal.
 */
static inline int im_film_develop(im_film_t *f)
{
    if (f->op_count < IM_DYNAMICS) return 0;
    /* Film ready: theta_sum holds 144 accumulated observations */
    f->op_count = 0;
    f->stage = IM_FILM_DEVELOP;
    return 1;
}

/*
 * im_film_reset — after develop, reset for next roll.
 */
static inline void im_film_reset(im_film_t *f)
{
    f->theta_sum = 0;
    f->stage = IM_FILM_ACCUMULATE;
}

/* ── Ghost Delete + Path Amnesia (from ehcp_poc.c) ────────── *
 *                                                                   *
 *  Ghost delete: state → 0, pair loses gradient.                     *
 *  occupied NEVER cleared (append-only invariant).                   *
 *  Path amnesia: route_follow returns UINT32_MAX for ghost.          *
 *                                                                   *
 *  This is the proof of concept for geometric sealing fail-deadly.   */

static inline void im_ghost_init(im_ghost_t *g, uint32_t addr,
                                  uint32_t pair_addr)
{
    g->address   = addr;
    g->pair_addr = pair_addr;
    g->state     = IM_STATE_ACTIVE;
    g->occupied  = 1;   /* slot marked — never cleared */
}

/*
 * im_ghost_delete — ghost-delete a node.
 * Sets state to GHOST on both node and pair.
 * Pair loses path back (amnesia).
 * occupied remains 1.
 */
static inline void im_ghost_delete(im_ghost_t *g, im_ghost_t *pair)
{
    g->state = IM_STATE_GHOST;
    if (pair) pair->state = IM_STATE_GHOST;
}

/*
 * im_ghost_reverse — flip direction of an active pair.
 * Ghost nodes cannot reverse (returns -1).
 */
static inline int im_ghost_reverse(im_ghost_t *g, im_ghost_t *pair)
{
    if (g->state == IM_STATE_GHOST) return -1;
    g->state = IM_STATE_REVERSED;
    if (pair) pair->state = IM_STATE_REVERSED;
    return 0;
}

/*
 * im_ghost_route — follow route from a node.
 * Ghost returns UINT32_MAX (path amnesia).
 * Reversed returns self (destination = source).
 * Active returns pair_addr.
 */
static inline uint32_t im_ghost_route(const im_ghost_t *g)
{
    if (g->state == IM_STATE_GHOST) return UINT32_MAX;
    if (g->state == IM_STATE_REVERSED) return g->address;
    return g->pair_addr;
}

/* ── Color Gate (from ehcp_poc.c) ──────────────────────────── */

typedef struct { uint8_t r, g, b; } im_rgb_t;

static inline int im_is_complement(im_rgb_t a, im_rgb_t b)
{
    return (a.r + b.r == 255) && (a.g + b.g == 255) && (a.b + b.b == 255);
}

typedef enum {
    IM_GATE_WARP,               /* complement pair → data exchange       */
    IM_GATE_COLLISION,          /* same color → conflict                  */
    IM_GATE_FILTER_BLOCK,       /* noise → no route                      */
    IM_GATE_GROUND_ABSORB       /* ghost → absorb into ground            */
} im_gate_t;

static inline im_gate_t im_color_gate(const im_ghost_t *a,
                                       const im_ghost_t *b,
                                       im_rgb_t ca, im_rgb_t cb)
{
    if (a->state == IM_STATE_GHOST || b->state == IM_STATE_GHOST)
        return IM_GATE_GROUND_ABSORB;
    if (im_is_complement(ca, cb)) return IM_GATE_WARP;
    if (ca.r == cb.r && ca.g == cb.g && ca.b == cb.b)
        return IM_GATE_COLLISION;
    return IM_GATE_FILTER_BLOCK;
}

/* ── Skeleton Decision Tree (from skeleton_index.h P0→P5) ─── *
 *                                                                   *
 *  P0 IDENTITY  diff==0              1B   (cheapest, first)          *
 *  P1 RAW       isect_pop >= 16     64B  (residual zone fast-reject) *
 *  P2 FLAT      all-zero chunk       1B                              *
 *  P3 DIFF      diff 1..48          10+nB                            *
 *  P4 BREF      byte-reverse match   2B                              *
 *  P5 GEOM      isect_pop < 16     ~20B  (fibo territory)            */

typedef enum {
    IM_STRAT_IDENTITY = 0,
    IM_STRAT_RAW      = 1,
    IM_STRAT_FLAT     = 2,
    IM_STRAT_DIFF     = 3,
    IM_STRAT_BREF     = 4,
    IM_STRAT_GEOM     = 5,
} im_strategy_t;

#define IM_CHUNK_SZ       64u
#define IM_ISECT_RAW_THR  16u
#define IM_DIFF_CEILING   48u

/* isect_pop: XOR-fold 8 words → popcount. Low=sparse/geometric. */
static inline uint8_t im_isect_pop(const uint8_t *b)
{
    const uint64_t *w = (const uint64_t *)b;
    uint64_t fold = w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    uint32_t v = (uint32_t)fold;
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    v = v * 0x01010101u;
    return (uint8_t)(v >> 24);
}

/* flat check: all bytes zero */
static inline int im_is_flat(const uint8_t *b)
{
    const uint64_t *w = (const uint64_t *)b;
    return !(w[0]|w[1]|w[2]|w[3]|w[4]|w[5]|w[6]|w[7]);
}

/* diff + bref in one pass */
static inline int im_diff_sym(const uint8_t *prev, const uint8_t *cur,
                               int *out_bref)
{
    int dc = 0, bref = 1;
    for (int i = 0; i < (int)IM_CHUNK_SZ; i++) {
        if (cur[i] != prev[i])                     dc++;
        if (cur[i] != prev[IM_CHUNK_SZ - 1u - i])  bref = 0;
        if (dc > (int)IM_DIFF_CEILING && !bref)     break;
    }
    *out_bref = bref && (dc > 0);
    return dc;
}

/*
 * im_decide — P0→P5 short-circuit decision for one 64B chunk.
 * Returns strategy. For IM_STRAT_DIFF, cost = 10 + dc.
 */
static inline im_strategy_t im_decide(const uint8_t *chunk,
                                       const uint8_t *prev,
                                       int has_prev)
{
    /* P0: IDENTITY — zero cost check */
    if (has_prev) {
        const uint64_t *a = (const uint64_t *)chunk;
        const uint64_t *b = (const uint64_t *)prev;
        if (a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3] &&
            a[4]==b[4] && a[5]==b[5] && a[6]==b[6] && a[7]==b[7])
            return IM_STRAT_IDENTITY;
    }

    /* P1: isect fast-reject → RAW */
    if (im_isect_pop(chunk) >= IM_ISECT_RAW_THR)
        return IM_STRAT_RAW;

    /* P2: FLAT */
    if (im_is_flat(chunk))
        return IM_STRAT_FLAT;

    /* P3+P4: diff/bref */
    if (has_prev) {
        int bref, dc = im_diff_sym(prev, chunk, &bref);
        if (dc > 0 && dc <= (int)IM_DIFF_CEILING) return IM_STRAT_DIFF;
        if (bref)                                  return IM_STRAT_BREF;
    }

    /* P5: GEOM — low isect confirmed above */
    return IM_STRAT_GEOM;
}

/* ── Unified Pipeline ──────────────────────────────────────── *
 *                                                                   *
 *  addr → skeleton_lookup → zone/axis/pole/partner                   *
 *         → Peano traverse (within face)                             *
 *         → Gear snap (adaptive resolution)                          *
 *         → CRC32C signature (cache key)                             *
 *         → ThirdEye accumulate (silent observation)                 *
 *         → Decision tree P0→P5 (encode strategy)                    *
 *         → Ghost route (if sealed)                                  */

typedef struct {
    im_skel_t  skel;           /* current skeleton position            */
    im_film_t  film;           /* ThirdEye accumulator                 */
    uint32_t   addr;           /* current flat address                 */
    uint8_t    gear;           /* current gear index                   */
    uint8_t    has_prev;       /* 0 = cold start                       */
    uint32_t   chunk_count;    /* total chunks processed               */
    uint32_t   strategy_hits[6]; /* strategy histogram                 */
    uint8_t    prev[IM_CHUNK_SZ]; /* previous chunk for diff/identity  */
} im_ctx_t;

static inline void im_ctx_init(im_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    im_film_init(&ctx->film);
}

/*
 * im_process_chunk — one-step pipeline for a 64B chunk.
 * Returns strategy used. Updates film, counters, prev.
 */
static inline im_strategy_t im_process_chunk(im_ctx_t *ctx,
                                              uint16_t addr,
                                              const uint8_t *chunk)
{
    /* 1. Skeleton lookup */
    ctx->skel = im_skel(addr);
    ctx->addr = addr;

    /* 2. Decision tree */
    im_strategy_t s = im_decide(chunk, ctx->prev, ctx->has_prev);

    /* 3. Update film */
    im_film_expose(&ctx->film, (uint64_t)s);
    if (im_film_develop(&ctx->film)) {
        /* Film developed — 144 ops accumulated, ready for inspection */
        im_film_reset(&ctx->film);
    }

    /* 4. Update counters */
    ctx->strategy_hits[s]++;
    ctx->chunk_count++;
    memcpy(ctx->prev, chunk, IM_CHUNK_SZ);
    ctx->has_prev = 1;
    return s;
}

/* ════════════════════════════════════════════════════════════════
 * Tuning Components (from recall list)
 * ════════════════════════════════════════════════════════════════ */

/* ── 1. L1+L2 Cache Hierarchy (from g7b.cpp) ───────────────── *
 *  L1 ultra-hot: 2 slots (most recently accessed).                *
 *  L2 set-associative: 64 sets × 4-way, LRU eviction.           *
 *  Hot pages stay in RAM; cold evicted to breathing FS.          */

#define IM_L1_SLOTS   2u
#define IM_L2_SETS    64u
#define IM_L2_WAYS    4u

typedef struct {
    uint16_t addr[IM_L1_SLOTS];
    uint32_t sig[IM_L1_SLOTS];     /* CRC32C signature per slot */
    uint8_t  valid[IM_L1_SLOTS];
} im_l1_t;

typedef struct {
    uint16_t addr[IM_L2_SETS][IM_L2_WAYS];
    uint32_t sig[IM_L2_SETS][IM_L2_WAYS];
    uint8_t  lru[IM_L2_SETS];      /* 4-bit LRU counter per set */
} im_l2_t;

typedef struct {
    im_l1_t  l1;
    im_l2_t  l2;
    uint32_t l1_hits;
    uint32_t l2_hits;
    uint32_t misses;
} im_cache_t;

static inline void im_cache_init(im_cache_t *c)
{
    memset(c, 0, sizeof(*c));
}

/* L1 probe: check both slots */
static inline int im_cache_l1_probe(const im_cache_t *c, uint16_t addr)
{
    for (uint8_t i = 0; i < IM_L1_SLOTS; i++) {
        if (c->l1.valid[i] && c->l1.addr[i] == addr) return (int)i;
    }
    return -1;
}

/* L2 probe: check 4-way set */
static inline int im_cache_l2_probe(const im_cache_t *c, uint16_t addr)
{
    uint16_t set = addr % IM_L2_SETS;
    for (uint8_t w = 0; w < IM_L2_WAYS; w++) {
        if (c->l2.addr[set][w] == addr) return (int)w;
    }
    return -1;
}

/* L1 insert: promote to slot 0, shift existing to slot 1 */
static inline void im_cache_l1_insert(im_cache_t *c, uint16_t addr,
                                       uint32_t sig)
{
    /* shift slot 1 ← slot 0 */
    c->l1.addr[1]  = c->l1.addr[0];
    c->l1.sig[1]   = c->l1.sig[0];
    c->l1.valid[1] = c->l1.valid[0];
    /* new entry in slot 0 */
    c->l1.addr[0]  = addr;
    c->l1.sig[0]   = sig;
    c->l1.valid[0] = 1;
}

/* L2 insert: evict LRU way in set */
static inline void im_cache_l2_insert(im_cache_t *c, uint16_t addr,
                                       uint32_t sig)
{
    uint16_t set = addr % IM_L2_SETS;
    uint8_t  way = (uint8_t)(c->l2.lru[set] % IM_L2_WAYS);
    c->l2.addr[set][way] = addr;
    c->l2.sig[set][way]  = sig;
    c->l2.lru[set]++;
}

/*
 * im_cache_lookup — unified lookup: L1 → L2 → miss.
 * Returns hit level (0=L1, 1=L2, -1=miss). Updates counters.
 */
static inline int im_cache_lookup(im_cache_t *c, uint16_t addr)
{
    int l1 = im_cache_l1_probe(c, addr);
    if (l1 >= 0) { c->l1_hits++; return 0; }

    int l2 = im_cache_l2_probe(c, addr);
    if (l2 >= 0) {
        c->l2_hits++;
        /* promote to L1 */
        im_cache_l1_insert(c, addr, c->l2.sig[addr % IM_L2_SETS][l2]);
        return 1;
    }

    c->misses++;
    return -1;
}

/* ── 2. PermBlockFast Bit-Permutation (from g7b.cpp) ──────── *
 *  Bit-level address scramble. Seed-derived, no LUT.             *
 *  murmurhash3 finalizer: avalanche properties, compact.        */

typedef struct {
    uint32_t seed;
} im_perm_t;

static inline void im_perm_init(im_perm_t *p, uint32_t seed)
{
    p->seed = seed;
}

/* permute: scatter address bits through seed */
static inline uint32_t im_permute(uint32_t addr, const im_perm_t *p)
{
    uint32_t v = addr ^ p->seed;
    v *= 0xCC9E2D51u;
    v = (v << 15) | (v >> 17);
    v *= 0x1B873593u;
    v ^= v >> 16;
    v *= 0x85EBCA6Bu;
    v ^= v >> 13;
    v *= 0xC2B2AE35u;
    v ^= v >> 16;
    return v;
}

/* unpermute: inverse scramble (brute-force inverse via forward) */
static inline uint32_t im_unpermute(uint32_t permuted, const im_perm_t *p)
{
    /* For murmurhash3 finalizer, inverse is expensive.
     * Use a 12-bit forward scan (20736 max) for small address spaces. */
    for (uint32_t i = 0; i < 20736u; i++) {
        if (im_permute(i, p) == permuted) return i;
    }
    return UINT32_MAX; /* not found */
}

/* ── 3. RewindBuffer Ring Buffer (from g7b.cpp) ───────────── *
 *  Tracks last N state changes for undo/rollback.                *
 *  Append-only writes, head wraps, count caps at capacity.      */

#define IM_REWIND_CAP  16u

typedef struct {
    uint16_t addr[IM_REWIND_CAP];
    uint32_t old_val[IM_REWIND_CAP];
    uint8_t  head;
    uint8_t  count;
} im_rewind_t;

static inline void im_rewind_init(im_rewind_t *r)
{
    r->head  = 0;
    r->count = 0;
}

/* record: push a state change before overwriting */
static inline void im_rewind_push(im_rewind_t *r, uint16_t addr,
                                   uint32_t old_val)
{
    r->addr[r->head]    = addr;
    r->old_val[r->head] = old_val;
    r->head = (r->head + 1u) % IM_REWIND_CAP;
    if (r->count < IM_REWIND_CAP) r->count++;
}

/* pop: undo most recent change, return 0 on success, -1 if empty */
static inline int im_rewind_pop(im_rewind_t *r, uint16_t *addr,
                                 uint32_t *old_val)
{
    if (r->count == 0) return -1;
    r->head = (r->head + IM_REWIND_CAP - 1u) % IM_REWIND_CAP;
    *addr    = r->addr[r->head];
    *old_val = r->old_val[r->head];
    r->count--;
    return 0;
}

/* ── 4. geo_route XOR-Drop + Countdown (from handoff_v424) ── *
 *  XOR-drop: core^inv == 0xFFFF...FF → DROP (identity check).   *
 *  Countdown: counts down from 144, triggers develop at 0.      */

typedef struct {
    uint32_t core;
    uint32_t inv;
} im_xorpair_t;

/* check if XOR-drop condition met */
static inline int im_xor_drop(const im_xorpair_t *xp)
{
    return (xp->core ^ xp->inv) == 0xFFFFFFFFu;
}

typedef struct {
    uint16_t remaining;     /* counts down from IM_DYNAMICS */
    uint16_t total_ops;     /* total ops since last trigger */
} im_countdown_t;

static inline void im_countdown_init(im_countdown_t *cd)
{
    cd->remaining  = IM_DYNAMICS;
    cd->total_ops  = 0;
}

/* tick: decrement countdown, returns 1 when reached 0 (trigger) */
static inline int im_countdown_tick(im_countdown_t *cd)
{
    cd->remaining--;
    cd->total_ops++;
    if (cd->remaining == 0) {
        cd->remaining = IM_DYNAMICS;
        return 1;
    }
    return 0;
}

/* ── 5. Shadow Protocol (from bermuda v3) ──────────────────── *
 *  Residual storage: offset + shadow address + index pair.       *
 *  Shadow holds explicit difference from primary.               */

typedef struct {
    uint32_t offset;        /* base address of primary */
    uint32_t shadow;        /* shadow copy / residual address */
    uint16_t idx_in;        /* input index */
    uint16_t idx_out;       /* output index */
} im_shadow_t;

static inline void im_shadow_init(im_shadow_t *s, uint32_t primary_addr,
                                   uint32_t shadow_addr)
{
    s->offset  = primary_addr;
    s->shadow  = shadow_addr;
    s->idx_in  = 0;
    s->idx_out = 0;
}

/* advance: increment both indices */
static inline void im_shadow_advance(im_shadow_t *s)
{
    s->idx_in++;
    s->idx_out++;
}

/* compute: return shadow address for current index */
static inline uint32_t im_shadow_addr(const im_shadow_t *s)
{
    return s->shadow + s->idx_out;
}

/* ── 6. 36-Triangle Residual Scratch (from hex-19) ────────── *
 *  12 faces × 36 triangles = 432 scratch slots.                *
 *  Per-face: triangle index 0..35 maps to residual address.    */

#define IM_SCRATCH_PER_FACE  36u
#define IM_SCRATCH_TOTAL     432u

typedef struct {
    uint16_t addr;          /* scratch slot address (0..431) */
    uint8_t  face;          /* owning face 0..11 */
    uint8_t  tri_idx;       /* triangle index 0..35 */
} im_scratch_t;

/* map (face, tri_idx) → scratch address */
static inline uint16_t im_scratch_addr(uint8_t face, uint8_t tri_idx)
{
    return (uint16_t)(face * IM_SCRATCH_PER_FACE + tri_idx);
}

/* inverse: scratch address → (face, tri_idx) */
static inline void im_scratch_inverse(uint16_t addr, uint8_t *face,
                                       uint8_t *tri_idx)
{
    *face    = (uint8_t)(addr / IM_SCRATCH_PER_FACE);
    *tri_idx = (uint8_t)(addr % IM_SCRATCH_PER_FACE);
}

/* ── 7. Hilbert L-Block Connector (from hilibert-lblock) ──── *
 *  floor (16B) + block (32B) = 48B per connector.              *
 *  Links L-blocks along Hilbert curve within a face.           */

#define IM_CONN_FLOOR_SZ  16u
#define IM_CONN_BLOCK_SZ  32u
#define IM_CONN_TOTAL_SZ  (IM_CONN_FLOOR_SZ + IM_CONN_BLOCK_SZ)

typedef struct {
    uint8_t floor_data[IM_CONN_FLOOR_SZ];
    uint8_t block_data[IM_CONN_BLOCK_SZ];
} im_connector_t;

/* GeoJump variant: 3 × 16B = 48B per block, 144 cells/tower */
#define IM_CONN_GJ_CELLS   3u
#define IM_CONN_GJ_PER     (IM_CONN_GJ_CELLS * IM_CONN_FLOOR_SZ)
#define IM_CONN_GJ_TOWER  144u

static inline void im_connector_floor(im_connector_t *c, const uint8_t *data,
                                       uint32_t len)
{
    uint32_t n = len < IM_CONN_FLOOR_SZ ? len : IM_CONN_FLOOR_SZ;
    memcpy(c->floor_data, data, n);
}

static inline void im_connector_block(im_connector_t *c, const uint8_t *data,
                                       uint32_t len)
{
    uint32_t n = len < IM_CONN_BLOCK_SZ ? len : IM_CONN_BLOCK_SZ;
    memcpy(c->block_data, data, n);
}

#endif /* ISOMETRIC_MAP_H */
