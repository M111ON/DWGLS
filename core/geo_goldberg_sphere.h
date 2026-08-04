/*
 * geo_goldberg_sphere.h — Goldberg Polyhedron Coordinate Fabric
 * ══════════════════════════════════════════════════════════════
 *
 * Concept:
 *   World Sphere = Goldberg GP(n,0) — uniform subdivision, whole sphere
 *   12 Pentagon  = topological anchors (fixed, all levels)
 *   10(n²-1) Hexagon = subdivided tiles (resolution = gp_level)
 *
 *   Diamond Field = Hexagonal magnifier/lens
 *     → points at any tile_id
 *     → local 64B window = Diamond coords inside that tile
 *
 *   Tring = perpendicular fiber channel between dimension layers
 *     → same tile_id, different gp_level = different dimension
 *     → compute on-the-fly (stateless, no storage)
 *     → tick = encode(tile_id, dim_depth) via bit-pack
 *
 * Address space:
 *   GpAddr { tile_id: uint32_t, dim: uint8_t } → TringNode
 *   tile_id maps to pentagon(0..11) or hexagon(12..N)
 *   dim 0..7 = subdivision depth (gp_level 1..8)
 *
 * Invariants (FROZEN):
 *   Pentagon count = 12 (Euler: F-E+V=2, always)
 *   gp_face_count(n) = 10n² + 2
 *   Pentagon tile_id = 0..11 (fixed across all levels)
 *   tick = (tile_id << 8) | dim   → deterministic, no table
 *
 * Design:
 *   Stateless — no alloc beyond Tring slots
 *   Fixed gp_level per file — uniform sphere, middleware-safe
 *   Diamond lens = read/write window into any tile
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include "tring.h"

/* ── Constants ──────────────────────────────────────────────── */

#define GP_PENT_COUNT     12u       /* always 12, Euler invariant      */
#define GP_MAX_LEVEL       8u       /* dim 0..7, gp_level 1..8         */
#define GP_MAX_DIM         8u       /* alias                           */
#define GP_CHUNK_SZ       64u       /* Diamond lens window = 64B       */

/* ── Face count for GP(n,0) ─────────────────────────────────── */
/* Total tiles = 10n² + 2  (12 pent + 10(n²-1) hex)            */
static inline uint32_t gp_face_count(uint8_t level) {
    uint32_t n = level;
    return 10u * n * n + 2u;
}

/* Pentagon tile_ids: 0..11 fixed — pure check, no table needed */
static inline int gp_is_pentagon(uint32_t tile_id) {
    return tile_id < GP_PENT_COUNT;
}

/* ── GpAddr — 5 bytes logical, packed to uint64_t ───────────── */
typedef struct {
    uint32_t tile_id;   /* 0..gp_face_count(level)-1               */
    uint8_t  dim;       /* 0..GP_MAX_DIM-1 (dimension / depth)     */
} GpAddr;

/* Encode GpAddr → tring tick (deterministic, O(1), no table)   */
static inline uint32_t gp_addr_to_tick(GpAddr a) {
    return (a.tile_id << 8) | (a.dim & 0x7Fu);
}

/* Decode tick → GpAddr */
static inline GpAddr gp_tick_to_addr(uint32_t tick) {
    GpAddr a;
    a.tile_id = tick >> 8;
    a.dim     = (uint8_t)(tick & 0x7Fu);
    return a;
}

/* ── Sector layout — round-robin remainder distribution ─────── */
/*
 * (10n²-10) / 12 is not exact for most levels.
 * Fix: first (remainder) sectors get (base+1) hexagons, rest get base.
 * → zero gap, all tile_ids 0..faces-1 addressed, roundtrip exact.
 *
 * Validated: GP(1..8,0) all PASS, gap=0.
 */
static inline uint32_t gp_hex_in_sector(uint8_t level, uint8_t sector) {
    uint32_t total_hex = gp_face_count(level) - GP_PENT_COUNT;
    uint32_t base = total_hex / GP_PENT_COUNT;
    uint32_t rem  = total_hex % GP_PENT_COUNT;
    return base + (sector < rem ? 1u : 0u);
}

/* Base tile_id of first hex in sector s (O(12) = O(1)) */
static inline uint32_t gp_sector_base(uint8_t level, uint8_t sector) {
    uint32_t b = GP_PENT_COUNT;
    for (uint8_t s = 0; s < sector; s++)
        b += gp_hex_in_sector(level, s);
    return b;
}

/* tile_id from (pentagon_anchor, hex_offset)
 * hex_offset 0 = the pentagon itself, 1..n = hexagons in sector */
static inline uint32_t gp_tile_id(uint8_t level,
                                   uint8_t pent_anchor,
                                   uint32_t hex_offset)
{
    if (pent_anchor >= GP_PENT_COUNT) return 0;
    if (hex_offset == 0) return pent_anchor;
    return gp_sector_base(level, pent_anchor) + (hex_offset - 1u);
}

/* Reverse: tile_id → pentagon anchor (O(12) scan = O(1)) */
static inline uint8_t gp_tile_to_pent(uint8_t level, uint32_t tile_id) {
    if (gp_is_pentagon(tile_id)) return (uint8_t)tile_id;
    for (uint8_t s = 0; s < GP_PENT_COUNT; s++) {
        uint32_t base = gp_sector_base(level, s);
        uint32_t sz   = gp_hex_in_sector(level, s);
        if (tile_id >= base && tile_id < base + sz) return s;
    }
    return GP_PENT_COUNT - 1;
}

/* ── Diamond Lens — 64B window into a GP tile ───────────────── */
/*
 * Diamond lens maps GpAddr → 64B chunk stored in Tring.
 * gp_level is fixed per GpSphere (whole-file uniform).
 *
 * write: encode 64B chunk into tring at computed tick
 * read : retrieve chunk, returns NULL if not written
 *
 * Pentagon tiles act as zone resets (context boundary for skel).
 */

typedef struct {
    Tring   *tring;     /* shared Tring — caller owns               */
    uint8_t  gp_level;  /* fixed for this sphere (1..8)             */
    uint32_t face_max;  /* gp_face_count(gp_level) — cached         */
} GpSphere;

static inline void gp_sphere_init(GpSphere *s, Tring *tring, uint8_t level) {
    s->tring    = tring;
    s->gp_level = (level < 1) ? 1 : (level > GP_MAX_LEVEL ? GP_MAX_LEVEL : level);
    s->face_max = gp_face_count(s->gp_level);
}

/* Write 64B chunk to sphere at (tile_id, dim) */
static inline uint32_t gp_lens_write(GpSphere *s,
                                      uint32_t  tile_id,
                                      uint8_t   dim,
                                      const uint8_t chunk[GP_CHUNK_SZ])
{
    if (tile_id >= s->face_max || dim >= GP_MAX_DIM) return UINT32_MAX;
    GpAddr a = { tile_id, dim };
    uint32_t tick = gp_addr_to_tick(a);
    /* reuse slot if already written (idempotent for fixed-level sphere) */
    if (tick < s->tring->capacity && s->tring->nodes[tick] != NULL) {
        TringNode *node = s->tring->nodes[tick];
        if (node->size == GP_CHUNK_SZ)
            memcpy(node->data, chunk, GP_CHUNK_SZ);
        return tick;
    }
    return tring_push(s->tring, chunk, GP_CHUNK_SZ);
}

/* Read 64B chunk from sphere — returns NULL if absent */
static inline const uint8_t *gp_lens_read(const GpSphere *s,
                                            uint32_t tile_id,
                                            uint8_t  dim)
{
    if (tile_id >= s->face_max || dim >= GP_MAX_DIM) return NULL;
    GpAddr a = { tile_id, dim };
    uint32_t tick = gp_addr_to_tick(a);
    uint32_t sz;
    return tring_read(s->tring, tick, &sz);
}

/* ── Tring fiber: traverse all dims for a tile_id ───────────── */
/*
 * "Tring as perpendicular channel" — iterate over all dimension
 * layers for the same tile_id. Compute on-the-fly, no table.
 *
 * Usage:
 *   GpFiber fb; gp_fiber_init(&fb, &sphere, tile_id);
 *   while (gp_fiber_next(&fb, &dim, &chunk)) { ... }
 */
typedef struct {
    const GpSphere *sphere;
    uint32_t        tile_id;
    uint8_t         cur_dim;
} GpFiber;

static inline void gp_fiber_init(GpFiber *fb,
                                  const GpSphere *s,
                                  uint32_t tile_id)
{
    fb->sphere  = s;
    fb->tile_id = tile_id;
    fb->cur_dim = 0;
}

/* Returns 1 if a chunk was found, advances dim. 0 = exhausted. */
static inline int gp_fiber_next(GpFiber *fb,
                                 uint8_t *out_dim,
                                 const uint8_t **out_chunk)
{
    while (fb->cur_dim < GP_MAX_DIM) {
        const uint8_t *chunk = gp_lens_read(fb->sphere,
                                             fb->tile_id,
                                             fb->cur_dim);
        uint8_t d = fb->cur_dim++;
        if (chunk) { *out_dim = d; *out_chunk = chunk; return 1; }
    }
    return 0;
}

/* ── Pentagon anchor walk — zone boundary detection ─────────── */
/*
 * Pentagon tiles = natural zone reset points for skeleton encoder.
 * Use this to check if transitioning tile_ids crosses a pentagon.
 *
 * Returns 1 if tile_id is a pentagon (zone boundary).
 * Diamond lens should trigger skel_enc_zone_reset() on 1.
 */
static inline int gp_is_zone_boundary(uint32_t tile_id) {
    return gp_is_pentagon(tile_id);  /* tile_id 0..11 = seam anchors */
}

/* ── Sphere iterator — Hilbert-compatible tile traversal ────── */
/*
 * Pentagon-first ordering: visit all 12 pentagons first (shell 0
 * anchors), then hexagons grouped by pentagon sector.
 * Compatible with OnionShell Hilbert traversal order.
 *
 * tile_seq[i] = tile_id at position i (no alloc — compute inline)
 */
static inline uint32_t gp_seq_tile(uint8_t level, uint32_t seq_pos) {
    uint32_t total = gp_face_count(level);
    if (seq_pos >= total) return UINT32_MAX;
    /* Pentagon-first: 0..11 direct, hexagons after */
    return seq_pos;  /* identity: tile_id == seq_pos in linear layout */
}

/* ── Encode helper: flat file offset → GpAddr ───────────────── */
/*
 * Maps a 64B chunk index (from pipeline) to a GpAddr.
 * dim = chunk_idx / face_max  (which sphere layer)
 * tile = chunk_idx % face_max (position on sphere)
 *
 * Stateless: same chunk_idx always → same GpAddr.
 */
static inline GpAddr gp_chunk_to_addr(uint8_t level, uint64_t chunk_idx) {
    uint32_t face_max = gp_face_count(level);
    GpAddr a;
    a.tile_id = (uint32_t)(chunk_idx % face_max);
    a.dim     = (uint8_t)((chunk_idx / face_max) & 0x7Fu);
    return a;
}

/* Reverse: GpAddr → chunk_idx */
static inline uint64_t gp_addr_to_chunk(uint8_t level, GpAddr a) {
    uint32_t face_max = gp_face_count(level);
    return (uint64_t)a.dim * face_max + a.tile_id;
}
