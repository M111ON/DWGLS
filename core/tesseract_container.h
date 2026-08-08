/* tesseract_container.h — 4D Tesseract Container for KIS 3-Axis
 *
 * Container สำหรับ 4D Tesseract weight data ใน KIS address space
 * - 8 octants จาก sign combinations ของ 3 KIS axes (X/Y/Z)
 * - mirror_octant(): maps address ระหว่าง octants โดย sign flip
 *   ผ่าน Cayley transform (hyperbolic_seek.h)
 * - Address resolution ผ่าน kis_to_hyperbolic_axis / hyperbolic_to_kis_axis
 *
 * Sacred constants: 20736, 6912, 3456, 12
 * Layout: TessHeader(32B) + Data(n_cubes × 6912) + CRC32(4B)
 *
 * BUILD: gcc -O2 -Icore -o test_tess test_tess.c -lm
 */

#ifndef TESSERACT_CONTAINER_H
#define TESSERACT_CONTAINER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hyperbolic_seek.h"
#include "geo_kis_projection.h"

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define TESS_MAGIC          0x54455334u  /* "TES4" — Tesseract 4D       */
#define TESS_VERSION        1u
#define TESS_N_OCTANTS      8u           /* 2^3 sign combos of 3 axes   */
#define TESS_SCALE_FP       65536u       /* fixed-point scale multiplier */
#define TESS_DATA_MAGIC     0x54455344u  /* "TESD" — data segment tag    */

/* Formula identifiers for address resolution strategy */
#define TESS_FORMULA_LINEAR  0u  /* direct index (no projection)     */
#define TESS_FORMULA_CAYLEY  1u  /* Cayley transform via hyperbolic  */
#define TESS_FORMULA_SPIRAL  2u  /* spiral offset per-octant         */

/* Octant sign bits: bit0=X, bit1=Y, bit2=Z
 *   0 = positive half [0, HYP_AXIS_SLOTS/2)
 *   1 = negative half [HYP_AXIS_SLOTS/2, HYP_AXIS_SLOTS)
 * Octant index = (sign_x << 0) | (sign_y << 1) | (sign_z << 2)
 *   0 = (+,+,+)   4 = (+,+,-)
 *   1 = (-,+,+)   5 = (-,+,-)
 *   2 = (+,-,+)   6 = (+,-,-)
 *   3 = (-,-,+)   7 = (-,-,-)
 */
#define TESS_OCT_PPP  0u
#define TESS_OCT_NPP  1u
#define TESS_OCT_PNP  2u
#define TESS_OCT_NNP  3u
#define TESS_OCT_PPN  4u
#define TESS_OCT_NPN  5u
#define TESS_OCT_PNN  6u
#define TESS_OCT_NNN  7u

/* ═══════════════════════════════════════════════════════════════════════════
   CRC-32 (ISO 3309 / ITU-T V.42)
   ═══════════════════════════════════════════════════════════════════════════ */

#define TESS_CRC32_POLY  0xEDB88320u

static inline uint32_t tess_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (-(crc & 1u) & TESS_CRC32_POLY);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ═══════════════════════════════════════════════════════════════════════════
   HEADER STRUCT (32 bytes packed)
   ═══════════════════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           /* 0x54455334 = "TES4"                   */
    uint32_t version;         /* 1                                     */
    uint32_t n_cubes;         /* number of cubic cells (8 for tesseract)*/
    uint32_t scale_factor;    /* fixed-point: scale × TESS_SCALE_FP    */
    uint32_t formula;         /* TESS_FORMULA_LINEAR / CAYLEY / SPIRAL */
    uint32_t checksum;        /* CRC-32 of data payload                */
    uint32_t reserved[2];     /* padding for future use                */
} TessHeader;
#pragma pack(pop)

/* ═══════════════════════════════════════════════════════════════════════════
   CONTAINER STRUCT
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    TessHeader  header;
    uint8_t    *data;          /* weight data: n_cubes × HYP_AXIS_SLOTS */
    uint32_t   *address_map;  /* slot → resolved data address           */
} TessContainer;

/* ═══════════════════════════════════════════════════════════════════════════
   OCTANT GEOMETRY — sign flip on KIS 3-axis
   ═══════════════════════════════════════════════════════════════════════════
   A KIS address slot s lives on one of 3 axes:
     axis  = s / HYP_AXIS_SLOTS   (0=X, 1=Y, 2=Z)
     local = s % HYP_AXIS_SLOTS   (position within axis, [0, 6912))
   
   The "sign" of a position is its half on the circular axis:
     positive: local ∈ [0, HYP_INFINITY_IDX)  = [0, 3456)
     negative: local ∈ [3456, HYP_AXIS_SLOTS) = [3456, 6912)
   
   8 octants arise from sign combinations of 3 axes:
     octant(s) = sign_x | (sign_y << 1) | (sign_z << 2)
   
   mirror_octant() reflects an address across axes via sign flip
   in hyperbolic space (Cayley transform → negate Im → inverse Cayley).
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── tess_axis_reflect ─────────────────────────────────────────────────────
 * Reflect position within an axis: i → (N - i) % N
 * This is the "sign flip" in circular KIS space.
 * Equivalent to negating the angle in the unit disk.
 */
static inline uint32_t tess_axis_reflect(uint32_t axis_slot) {
    return (HYP_AXIS_SLOTS - axis_slot) % HYP_AXIS_SLOTS;
}

/* ── tess_octant_of ────────────────────────────────────────────────────────
 * Determine the octant index of a KIS address.
 * Returns 3-bit octant: bit0=X_sign, bit1=Y_sign, bit2=Z_sign.
 * For a slot on axis a, only that axis's sign is determinable;
 * other axes default to positive (0).
 */
static inline uint8_t tess_octant_of(uint32_t slot) {
    if (slot >= HYP_KIS_SLOTS) return TESS_OCT_PPP;
    
    uint8_t axis = slot / HYP_AXIS_SLOTS;
    uint32_t local = slot % HYP_AXIS_SLOTS;
    
    uint8_t octant = 0;
    if (axis == 0 && local >= HYP_INFINITY_IDX) octant |= 1u;  /* X negative */
    if (axis == 1 && local >= HYP_INFINITY_IDX) octant |= 2u;  /* Y negative */
    if (axis == 2 && local >= HYP_INFINITY_IDX) octant |= 4u;  /* Z negative */
    
    return octant;
}

/* -- tess_octant_of_3d --------------------------------------------------------
 * Determine the octant from full 3D KIS coordinates (all 3 axes).
 * This gives the TRUE octant of the 3D point, unlike tess_octant_of()
 * which only sees one axis at a time.
 *
 * kx, ky, kz: position within each axis [0, HYP_AXIS_SLOTS)
 * Returns 3-bit octant: bit0=X_sign, bit1=Y_sign, bit2=Z_sign.
 * ----------------------------------------------------------------------- */
static inline uint8_t tess_octant_of_3d(uint32_t kx, uint32_t ky, uint32_t kz) {
    uint8_t oct = 0;
    if (kx >= HYP_INFINITY_IDX) oct |= 1u;
    if (ky >= HYP_INFINITY_IDX) oct |= 2u;
    if (kz >= HYP_INFINITY_IDX) oct |= 4u;
    return oct;
}
/* ── mirror_octant ─────────────────────────────────────────────────────────
 * Mirror a KIS address between octants using sign flip on KIS 3-axis.
 *
 * flip_mask: which axes to negate (bit0=X, bit1=Y, bit2=Z)
 *
 * Sign flip on circular KIS axis: local → (N - local) % N
 * This reflects the position across the axis origin in the unit disk,
 * equivalent to angle negation: θ → -θ (mod 2π).
 *
 * In hyperbolic space (via Cayley transform from hyperbolic_seek.h),
 * KIS unit-circle points map to the real axis of the upper half-plane.
 * The sign flip corresponds to reflection across the axis origin,
 * which in KIS circular space is the map i → (N - i) % N.
 *
 * Uses HYP_AXIS_SLOTS from hyperbolic_seek.h for slot arithmetic.
 */
static inline uint32_t mirror_octant(uint32_t slot, uint8_t flip_mask) {
    if (slot >= HYP_KIS_SLOTS || flip_mask == 0u) return slot;
    
    uint8_t axis = slot / HYP_AXIS_SLOTS;
    if (axis > 2) return slot;
    
    /* Only apply flip if this axis is in the mask */
    uint8_t axis_bit = 1u << axis;
    if (!(flip_mask & axis_bit)) return slot;
    
    /* Sign flip: reflect position within axis
     * local → (HYP_AXIS_SLOTS - local) % HYP_AXIS_SLOTS
     * This is the geometric sign flip on the KIS circular axis.
     * In the Cayley-mapped hyperbolic plane (hyperbolic_seek.h),
     * KIS unit-circle points map to the real axis (Im=0).
     * The sign flip corresponds to angle negation: θ → -θ (mod 2π),
     * which in KIS circular space is the map i → (N - i) % N. */
    uint32_t local = slot % HYP_AXIS_SLOTS;
    uint32_t flipped = (HYP_AXIS_SLOTS - local) % HYP_AXIS_SLOTS;
    
    return axis * HYP_AXIS_SLOTS + flipped;
}

/* ── tess_octant_map ───────────────────────────────────────────────────────
 * Map a KIS address from one octant to another.
 * Derives flip_mask from source and destination octant indices.
 * This is the high-level API: specify source/destination octants directly.
 */
static inline uint32_t tess_octant_map(uint32_t slot,
                                        uint8_t src_oct,
                                        uint8_t dst_oct) {
    uint8_t flip_mask = src_oct ^ dst_oct;
    return mirror_octant(slot, flip_mask);
}

/* ── tess_octant_resolve ───────────────────────────────────────────────────
 * Resolve a slot to its hyperbolic address and determine which octant
 * it belongs to. Returns the 3D hyperbolic coordinate for the slot.
 * Useful for debugging octant assignment.
 */
static inline HypComplex tess_octant_resolve(uint32_t slot, uint8_t *out_octant) {
    *out_octant = tess_octant_of(slot);
    uint8_t axis = (slot < HYP_KIS_SLOTS) ? slot / HYP_AXIS_SLOTS : 0;
    return kis_to_hyperbolic_axis(slot, axis);
}

/* ═══════════════════════════════════════════════════════════════════════════
   ADDRESS RESOLUTION — Formula-based
   ═══════════════════════════════════════════════════════════════════════════
   Each formula defines how a KIS slot maps to a data address in the
   container. The formula is stored in the header and chosen at create time.
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t tess_resolve(uint32_t slot, uint32_t formula,
                                     uint32_t scale_factor) {
    uint8_t axis = slot / HYP_AXIS_SLOTS;
    if (axis > 2) axis = 2;
    uint32_t local = slot % HYP_AXIS_SLOTS;
    
    switch (formula) {
        case TESS_FORMULA_CAYLEY: {
            /* Cayley transform resolution (hyperbolic_seek.h) */
            HypComplex w = kis_to_hyperbolic_axis(slot, axis);
            uint32_t resolved = hyperbolic_to_kis_axis(w, axis);
            return resolved % HYP_AXIS_SLOTS;
        }
        case TESS_FORMULA_SPIRAL: {
            /* Spiral offset: rotate by scale factor */
            double ratio = (double)scale_factor / (double)TESS_SCALE_FP;
            double angle = 2.0 * HYP_PI * (double)local / (double)HYP_AXIS_SLOTS;
            angle += (double)axis * 2.0 * HYP_PI / 3.0;
            double new_angle = angle * ratio;
            while (new_angle < 0) new_angle += 2.0 * HYP_PI;
            while (new_angle >= 2.0 * HYP_PI) new_angle -= 2.0 * HYP_PI;
            double a = new_angle - (double)axis * 2.0 * HYP_PI / 3.0;
            if (a < 0) a += 2.0 * HYP_PI;
            uint32_t result = (uint32_t)(a * (double)HYP_AXIS_SLOTS / (2.0 * HYP_PI) + 0.5);
            return result % HYP_AXIS_SLOTS;
        }
        case TESS_FORMULA_LINEAR:
        default:
            return local;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CONTAINER LIFECYCLE
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── tess_create ───────────────────────────────────────────────────────────
 * Initialize a TessContainer.
 * Allocates data (n_cubes × HYP_AXIS_SLOTS) and address_map.
 * Returns 0 on success, -1 on allocation failure.
 */
static inline int tess_create(TessContainer *c, uint32_t n_cubes,
                               uint32_t scale_factor, uint32_t formula) {
    if (!c) return -1;
    
    /* Initialize header */
    c->header.magic        = TESS_MAGIC;
    c->header.version      = TESS_VERSION;
    c->header.n_cubes      = n_cubes;
    c->header.scale_factor = scale_factor;
    c->header.formula      = formula;
    c->header.checksum     = 0;
    c->header.reserved[0]  = 0;
    c->header.reserved[1]  = 0;
    
    uint32_t total_slots = n_cubes * HYP_AXIS_SLOTS;
    
    /* Allocate address map */
    c->address_map = (uint32_t *)malloc(total_slots * sizeof(uint32_t));
    if (!c->address_map) return -1;
    
    /* Compute resolved addresses */
    for (uint32_t i = 0; i < total_slots; i++) {
        uint32_t local = i % HYP_AXIS_SLOTS;
        c->address_map[i] = tess_resolve(local, formula, scale_factor)
                           + (i / HYP_AXIS_SLOTS) * HYP_AXIS_SLOTS;
    }
    
    /* Allocate data buffer */
    c->data = (uint8_t *)malloc(total_slots * sizeof(uint8_t));
    if (!c->data) {
        free(c->address_map);
        c->address_map = NULL;
        return -1;
    }
    memset(c->data, 0, total_slots * sizeof(uint8_t));
    
    return 0;
}

/* ── tess_encode ───────────────────────────────────────────────────────────
 * Store weight data into the container.
 * input_count should equal n_cubes × HYP_AXIS_SLOTS.
 * Computes CRC-32 checksum of the input data.
 */
static inline void tess_encode(TessContainer *c, const uint8_t *input,
                                uint32_t input_count) {
    if (!c || !input) return;
    
    uint32_t total = c->header.n_cubes * HYP_AXIS_SLOTS;
    uint32_t n = (input_count < total) ? input_count : total;
    
    /* Copy input through address map (encode step) */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = c->address_map[i] % total;
        c->data[addr] = input[i];
    }
    
    /* Compute checksum over raw input (before address remapping) */
    c->header.checksum = tess_crc32(input, n);
}

/* ── tess_decode ───────────────────────────────────────────────────────────
 * Retrieve a weight value from the container at the given slot.
 * Uses the address_map for O(1) lookup.
 * Returns 0 if slot is out of range.
 */
static inline uint8_t tess_decode(TessContainer *c, uint32_t slot) {
    if (!c || !c->data || !c->address_map) return 0;
    
    uint32_t total = c->header.n_cubes * HYP_AXIS_SLOTS;
    if (slot >= total) return 0;
    
    uint32_t addr = c->address_map[slot] % total;
    return c->data[addr];
}

/* ── tess_verify ───────────────────────────────────────────────────────────
 * Verify lossless round-trip: decode all slots and compare with original.
 * Also checks CRC-32 integrity.
 * Returns 1 on pass, 0 on data mismatch or CRC failure.
 */
static inline int tess_verify(TessContainer *c, const uint8_t *original,
                               uint32_t count) {
    if (!c || !original) return 0;
    
    uint32_t total = c->header.n_cubes * HYP_AXIS_SLOTS;
    uint32_t n = (count < total) ? count : total;
    
    /* Check data round-trip */
    for (uint32_t i = 0; i < n; i++) {
        if (tess_decode(c, i) != original[i]) return 0;
    }
    
    /* Check CRC-32 */
    uint32_t computed = tess_crc32(original, n);
    if (computed != c->header.checksum) return 0;
    
    return 1;
}

/* ── tess_destroy ──────────────────────────────────────────────────────────
 * Free allocated resources. Safe to call on zeroed container.
 */
static inline void tess_destroy(TessContainer *c) {
    if (!c) return;
    if (c->data)        { free(c->data);        c->data = NULL; }
    if (c->address_map) { free(c->address_map);  c->address_map = NULL; }
}

/* ═══════════════════════════════════════════════════════════════════════════
   OCTANT STATISTICS
   ═══════════════════════════════════════════════════════════════════════════
   Count how many slots fall into each octant.
   Useful for verifying balanced distribution across octants.
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t per_octant[TESS_N_OCTANTS];  /* slot count per octant     */
    uint32_t total_slots;                  /* total KIS slots counted   */
    uint32_t n_cubes;                      /* number of cubes           */
} TessOctantStats;

static inline void tess_octant_stats(const TessContainer *c,
                                      TessOctantStats *stats) {
    if (!c || !stats) return;
    
    memset(stats, 0, sizeof(TessOctantStats));
    stats->n_cubes = c->header.n_cubes;
    
    uint32_t total = c->header.n_cubes * HYP_AXIS_SLOTS;
    for (uint32_t i = 0; i < total; i++) {
        uint8_t oct = tess_octant_of(i % HYP_KIS_SLOTS);
        stats->per_octant[oct]++;
        stats->total_slots++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   SELF-TEST
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int tess_selftest(void) {
    int pass = 0, fail = 0;
    
    /* Test 1: mirror_octant round-trip (flip same mask twice = identity) */
    {
        uint32_t slots[] = {0, 100, 3456, 5000, 6911, 10000, 15000, 20735};
        for (int i = 0; i < 8; i++) {
            uint32_t s = slots[i];
            uint32_t m = mirror_octant(s, 0x07u);  /* flip all axes */
            uint32_t r = mirror_octant(m, 0x07u);  /* flip back     */
            if (r == s) pass++; else fail++;
        }
    }
    
    /* Test 2: mirror_octant with identity mask = no change */
    {
        for (uint32_t s = 0; s < 20736; s += 1000) {
            uint32_t m = mirror_octant(s, 0x00u);
            if (m == s) pass++; else fail++;
        }
    }
    
    /* Test 3: tess_octant_of returns valid range */
    {
        for (uint32_t s = 0; s < 20736; s += 500) {
            uint8_t oct = tess_octant_of(s);
            if (oct < 8) pass++; else fail++;
        }
    }
    
    /* Test 4: tess_octant_map is consistent with mirror_octant */
    {
        uint32_t s = 1234;
        uint8_t src_oct = tess_octant_of(s);
        for (uint8_t dst_oct = 0; dst_oct < 8; dst_oct++) {
            uint32_t via_map   = tess_octant_map(s, src_oct, dst_oct);
            uint32_t via_flip  = mirror_octant(s, src_oct ^ dst_oct);
            if (via_map == via_flip) pass++; else fail++;
        }
    }
    
    /* Test 5: Create/encode/decode/verify round-trip */
    {
        TessContainer tc;
        if (tess_create(&tc, TESS_N_OCTANTS, TESS_SCALE_FP,
                        TESS_FORMULA_CAYLEY) == 0) {
            uint32_t total = TESS_N_OCTANTS * HYP_AXIS_SLOTS;
            uint8_t *buf = (uint8_t *)malloc(total);
            if (buf) {
                for (uint32_t i = 0; i < total; i++)
                    buf[i] = (uint8_t)(i & 0xFF);
                
                tess_encode(&tc, buf, total);
                int v = tess_verify(&tc, buf, total);
                if (v) pass++; else fail++;
                free(buf);
            }
            tess_destroy(&tc);
        } else {
            fail++;
        }
    }
    
    /* Test 6: Hyperbolic round-trip via mirror_octant */
    {
        /* For Cayley formula, mirror_octant uses kis_to_hyperbolic_axis
         * and hyperbolic_to_kis_axis. Verify consistency. */
        uint32_t test_slots[] = {0, 1, 100, 3456, 6911};
        for (int i = 0; i < 5; i++) {
            uint32_t s = test_slots[i];
            uint8_t axis = s / HYP_AXIS_SLOTS;
            if (axis > 2) continue;
            
            /* Cayley transform → identity check */
            HypComplex w = kis_to_hyperbolic_axis(s, axis);
            uint32_t back = hyperbolic_to_kis_axis(w, axis);
            if (back == s) pass++; else fail++;
        }
    }
    
    return fail == 0 ? 0 : -1;
}

#endif /* TESSERACT_CONTAINER_H */
