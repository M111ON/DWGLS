/*
 * geo_fcc_dramtile.h — Face-Centered Cubic Lattice for DRamTile Addressing
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * FCC (Face-Centered Cubic) is the densest 3D sphere packing.
 * Maps 20736 address space to 3D lattice coordinates for GPU memory access.
 *
 * FCC lattice: positions where x+y+z is even, on a cubic grid.
 * Equivalently: interleaved simple cubic lattices.
 *
 * 20736 = 12^4 = 144 * 144
 *   DRamTile layout: 128 anchor rows × 162 columns = 20736
 *   FCC mapping: (x, y, z) where x+y+z ≡ 0 (mod 2)
 *
 * Key insight: DRamTile 128×162 = FCC sub-lattice of cubic 144^3
 *   - 144^3 = 2,985,984 positions
 *   - FCC density = π/(3√2) ≈ 0.7405
 *   - 20736 / 144^3 = 0.00694 ← 20736 is a slice, not the full volume
 *
 * Application:
 *   - GPU stride scheduling (deterministic prefetch windows)
 *   - 3D memory access pattern for DRamTile chain
 *   - Distance-preserving address mapping for tensor locality
 *
 * Properties:
 *   - Each FCC cell has 12 nearest neighbors (coordination number)
 *   - Shortest distance = a/√2 (a = lattice constant)
 *   - Wraps naturally: FCC is periodic in 3D
 *
 * NO MALLOC. All static inline. Header-only.
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_FCC_DRAMTILE_H
#define GEO_FCC_DRAMTILE_H

#include <stdint.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
   FCC CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define FCC_FULL        20736u    /* 12^4 = address space */
#define FCC_LATTICE     144u      /* cubic lattice side (12^2) */
#define FCC_DENSITY_NUM 7405u     /* π/(3√2) ≈ 0.7405 as integer × 10000 */
#define FCC_DENSITY_D   10000u

/* ═══════════════════════════════════════════════════════════════════════════
   FCC 3D COORDINATE
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t x, y, z;
} FCCCoord;

/* FCC validity: x+y+z must be even */
static inline uint32_t fcc_valid(uint32_t x, uint32_t y, uint32_t z)
{
    return ((x + y + z) & 1u) == 0u;
}

/* ═══════════════════════════════════════════════════════════════════════════
   FLAT ↔ FCC ADDRESS MAPPING
   ═══════════════════════════════════════════════════════════════════════════
   DRamTile: 128 anchor rows × 162 columns = 20736
   FCC: (x, y, z) on 144^3 lattice where x+y+z is even

   Mapping:
     flat = anchor * 162 + col     (DRamTile)
     anchor ∈ [0, 127], col ∈ [0, 161]

   FCC coordinate from flat:
     z = flat / (144*144)           (layer)
     rem = flat % (144*144)
     y = rem / 144                  (row in layer)
     x = rem % 144                  (col in layer)

   But we need x+y+z even. If not, shift x by 1.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline FCCCoord flat_to_fcc(uint32_t flat)
{
    /* Simple 2D → FCC slice: flat = y * 144 + x, z = 0
     * FCC validity: x+y+z even → ensure by adjusting z
     * This maps 20736 addresses to a single FCC layer (z=0 or z=1)
     */
    FCCCoord c;
    c.x = flat % FCC_LATTICE;
    c.y = flat / FCC_LATTICE;
    c.z = 0;

    /* Ensure FCC parity: x+y+z even */
    if (((c.x + c.y + c.z) & 1u) != 0u) {
        c.z = 1u;  /* flip z to fix parity */
    }
    return c;
}

static inline uint32_t fcc_to_flat(FCCCoord c)
{
    /* Inverse of flat_to_fcc: x = flat % 144, y = flat / 144, z = parity fix */
    if (c.x < FCC_LATTICE && c.y < FCC_LATTICE)
        return c.y * FCC_LATTICE + c.x;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   FCC NEIGHBORS — 12 nearest neighbors
   ═══════════════════════════════════════════════════════════════════════════
   Each FCC site has exactly 12 nearest neighbors.
   The 12 directions are the permutations of (±1, ±1, 0).
   ═══════════════════════════════════════════════════════════════════════════ */

#define FCC_NEIGHBORS 12u

static const int32_t FCC_DIR[12][3] = {
    {+1, +1,  0}, {+1, -1,  0}, {-1, +1,  0}, {-1, -1,  0},
    {+1,  0, +1}, {+1,  0, -1}, {-1,  0, +1}, {-1,  0, -1},
    { 0, +1, +1}, { 0, +1, -1}, { 0, -1, +1}, { 0, -1, -1},
};

/* Get neighbor coordinate (wrapping at lattice boundary) */
static inline FCCCoord fcc_neighbor(FCCCoord c, uint32_t dir)
{
    if (dir >= FCC_NEIGHBORS) return c;
    FCCCoord n;
    n.x = (uint32_t)(((int32_t)c.x + FCC_DIR[dir][0] + (int32_t)FCC_LATTICE) % (int32_t)FCC_LATTICE);
    n.y = (uint32_t)(((int32_t)c.y + FCC_DIR[dir][1] + (int32_t)FCC_LATTICE) % (int32_t)FCC_LATTICE);
    n.z = (uint32_t)(((int32_t)c.z + FCC_DIR[dir][2] + (int32_t)FCC_LATTICE) % (int32_t)FCC_LATTICE);
    return n;
}

/* Get neighbor flat address */
static inline uint32_t fcc_neighbor_flat(uint32_t flat, uint32_t dir)
{
    FCCCoord c = flat_to_fcc(flat);
    FCCCoord n = fcc_neighbor(c, dir);
    return fcc_to_flat(n);
}

/* ═══════════════════════════════════════════════════════════════════════════
   FCC DISTANCE
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t fcc_dist_sq(FCCCoord a, FCCCoord b)
{
    int32_t dx = (int32_t)a.x - (int32_t)b.x;
    int32_t dy = (int32_t)a.y - (int32_t)b.y;
    int32_t dz = (int32_t)a.z - (int32_t)b.z;
    return (uint32_t)(dx*dx + dy*dy + dz*dz);
}

/* ═══════════════════════════════════════════════════════════════════════════
   DRamTile ↔ FCC BRIDGE
   ═══════════════════════════════════════════════════════════════════════════
   DRamTile: 128 anchor × 162 col = 20736
   FCC: 144^3 lattice (subset where x+y+z even)

   Bridge: DRamTile (anchor, col) → FCC (x, y, z)
     anchor ∈ [0,127], col ∈ [0,161]
     z = anchor * 144 / 128 = anchor * 9/8   (maps 128→144)
     y = col * 144 / 162 = col * 8/9          (maps 162→144)
     x = (anchor + col) % 144                 (interleaving)
   ═══════════════════════════════════════════════════════════════════════════ */

static inline FCCCoord dramtile_to_fcc(uint32_t anchor, uint32_t col)
{
    FCCCoord c;
    c.z = (anchor * 9u) / 8u;            /* 128 → 144 */
    c.y = (col * 8u) / 9u;               /* 162 → 144 */
    c.x = (anchor + col) % FCC_LATTICE;  /* interleave */

    /* Ensure FCC parity */
    if (((c.x + c.y + c.z) & 1u) != 0u) {
        c.x = (c.x + 1u) % FCC_LATTICE;
    }
    return c;
}

/* ═══════════════════════════════════════════════════════════════════════════
   FCC STATS
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t valid_count;     /* positions with x+y+z even */
    uint32_t total_positions; /* 144^3 */
    uint32_t density_pct;     /* density × 100 */
} FCCStats;

static inline FCCStats fcc_verify(void)
{
    FCCStats st;
    st.total_positions = FCC_LATTICE * FCC_LATTICE * FCC_LATTICE;
    st.valid_count = 0;

    for (uint32_t z = 0; z < FCC_LATTICE; z++) {
        for (uint32_t y = 0; y < FCC_LATTICE; y++) {
            for (uint32_t x = 0; x < FCC_LATTICE; x++) {
                if (fcc_valid(x, y, z)) st.valid_count++;
            }
        }
    }

    st.density_pct = (st.valid_count * 100u) / st.total_positions;
    return st;
}

static inline void fcc_print_stats(const FCCStats *st)
{
    if (!st) return;
    printf("═══════════════════════════════════════════════\n");
    printf("  FCC DRamTile Stats\n");
    printf("═══════════════════════════════════════════════\n");
    printf("  Lattice:       %u^3 = %u\n", FCC_LATTICE, st->total_positions);
    printf("  FCC sites:     %u (x+y+z even)\n", st->valid_count);
    printf("  Density:       %u%% (theoretical: 74.05%%)\n", st->density_pct);
    printf("  Address space: %u (128×162 DRamTile)\n", FCC_FULL);
    printf("═══════════════════════════════════════════════\n");
}

#endif /* GEO_FCC_DRAMTILE_H */
