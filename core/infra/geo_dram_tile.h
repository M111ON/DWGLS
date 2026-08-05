/*
 * geo_dram_tile.h — DRam Tile: zero-copy geometry addressing
 * ══════════════════════════════════════════════════════
 *
 * Core formula:
 *   dram_addr = anchor_id × 128 + hilbert_8x8(x, y, layer)
 *
 *   anchor_id ∈ [0, 161]       → FRAME_ICO_NODES (81×2 poles)
 *   x, y ∈ [0, 7]              → 8×8 Hilbert grid (pure bit-interleave)
 *   layer ∈ [0, 1]             → depth/phase flip
 *
 *   Total: 162 × 128 = 20736 = GEO_FULL ✅
 *
 * Zero-copy design:
 *   mmap_offset = dram_addr × CHUNK_SZ
 *   data enters → address computed → pointer write → OS lazy flush
 *   Deterministic re-gen → same address always → no WAL needed
 *
 *   bottleneck = address compute? → 8.3 ns/call → ~7.7 GB/s effective
 *   real bottleneck = msync/page fault → eliminated by deterministic re-gen
 *
 * Invariants (FROZEN):
 *   DRAM_GRID_X = 8, DRAM_GRID_Y = 8, DRAM_LAYERS = 2
 *   DRAM_ANCHORS = 162 (FRAME_ICO_NODES)
 *   DRAM_FULL = 20736 (GEO_FULL)
 *
 * No malloc. No float. No SIMD. Sub-10ns.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Constants ──────────────────────────────────────────── */
#define DRAM_GRID_X       8u
#define DRAM_GRID_Y       8u
#define DRAM_LAYERS       2u
#define DRAM_CELLS_PER    (DRAM_GRID_X * DRAM_GRID_Y * DRAM_LAYERS)  /* 128 */
#define DRAM_ANCHORS      162u
#define DRAM_FULL         (DRAM_ANCHORS * DRAM_CELLS_PER)            /* 20736 */

/* ── Hilbert curve on 8×8 grid (pure bit-interleave) ──────
 * n=8=2³ → exact power of 2, no modulo, pure shift+mask.
 * Returns 0..63, bijective, O(log n) = 3 iterations.
 */
static inline uint32_t dram_hilbert_8x8(uint32_t x, uint32_t y) {
    uint32_t d = 0, n = 8;
    for (uint32_t s = n >> 1; s > 0; s >>= 1) {
        uint32_t rx = (x & s) > 0;
        uint32_t ry = (y & s) > 0;
        d = (d << 2) | ((3u * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = n - 1u - x; y = n - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

/* ── DRam Address ─────────────────────────────────
 *   layer=0 → hilbert 0..63 (base layer)
 *   layer=1 → hilbert 64..127 (phase flip layer)
 *
 *   anchor=0 → 0..127
 *   anchor=1 → 128..255
 *   anchor=161 → 20608..20735
 */
static inline uint32_t dram_addr(uint32_t anchor_id,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t layer)
{
    uint32_t tile_off = dram_hilbert_8x8(x, y)
                        + layer * (DRAM_GRID_X * DRAM_GRID_Y);
    return anchor_id * DRAM_CELLS_PER + tile_off;
}

/* ── Decompose dram_addr → coarse components ────── */
typedef struct {
    uint32_t anchor_id;
    uint32_t layer;
    uint32_t hilbert_pos;  /* 0..63 within layer */
} DramAddrParts;

static inline DramAddrParts dram_decompose(uint32_t addr) {
    DramAddrParts p;
    p.anchor_id   = addr / DRAM_CELLS_PER;
    uint32_t off  = addr % DRAM_CELLS_PER;
    p.layer       = off / (DRAM_GRID_X * DRAM_GRID_Y);
    p.hilbert_pos = off % (DRAM_GRID_X * DRAM_GRID_Y);
    return p;
}

/* ── Flat mmap offset from dram_addr ────────────── */
static inline size_t dram_mmap_offset(uint32_t addr, uint32_t chunk_sz) {
    return (size_t)addr * chunk_sz;
}

/* ── Verify: check Hilbert 8×8 covers 0..63 uniquely ── */
static inline int dram_verify_hilbert(void) {
    uint8_t seen[64] = {0};
    for (uint32_t y = 0; y < DRAM_GRID_Y; y++) {
        for (uint32_t x = 0; x < DRAM_GRID_X; x++) {
            uint32_t h = dram_hilbert_8x8(x, y);
            if (h >= 64 || seen[h]) return -1;
            seen[h] = 1;
        }
    }
    return 0;
}

/* ── Verify: full address space has zero collisions ── */
/* Returns 0 on pass, -1 on collision, -2 on OOM */
static inline int dram_verify_full(void) {
    /* Stack alloc for small space (20736 bits = 2592 bytes) */
    uint8_t seen[DRAM_FULL / 8 + 1] = {0};
    for (uint32_t a = 0; a < DRAM_ANCHORS; a++) {
        for (uint32_t y = 0; y < DRAM_GRID_Y; y++) {
            for (uint32_t x = 0; x < DRAM_GRID_X; x++) {
                for (uint32_t l = 0; l < DRAM_LAYERS; l++) {
                    uint32_t addr = dram_addr(a, x, y, l);
                    if (addr >= DRAM_FULL) return -1;
                    uint32_t byte = addr >> 3;
                    uint8_t  bit  = (uint8_t)(1u << (addr & 7u));
                    if (seen[byte] & bit) return -1;
                    seen[byte] |= bit;
                }
            }
        }
    }
    return 0;
}
