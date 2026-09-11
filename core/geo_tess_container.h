/* geo_tess_container.h — .tess Binary Format: Tesseract Container
 *
 * FORMAT: .tess — single-cube container with 8-octant runtime derivation
 * CAPO: Multi-cube tensors use capo addressing (capo_id in TESS_Formula)
 *       Each capo = one cube (≤20736 blocks). Cube 0 = index. 8 octants derived.
 * PHILOSOPHY: MAP not COMPRESS | coordinate = address | sacred numbers
 *
 * Sacred numbers: 20736, 1728, 144, 12, 128, 162
 * KIS 3-axis: X(0-6911), Y(6912-13823), Z(13824-20735)
 * 8 octants = 8 views of same cube = tesseract (4D hypercube)
 *
 * BUILD: standalone or with -Icore
 * DEPENDS: none (self-contained)
 *
 * PIPELINE: GGUF → [extract] → [KIS map] → [hyperbolic] → .tess → llama.cpp
 */

#ifndef GEO_TESS_CONTAINER_H
#define GEO_TESS_CONTAINER_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Platonic Field: octant identity + Voronoi masking */
#include "geo_octant.h"
#include "geo_voronoi_mask.h"

/* OS/mmap headers for the .tesspack mmap reader */
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* ═══════════════════════════════════════════════════════════════════════════
   SACRED CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define GEO_TESS_MAGIC      0x54455353u  /* "TESS" little-endian */
#define TESS_VERSION        1u
#define TESS_TOTAL_SLOTS    20736u       /* 12^4 = 144^2 = 128 × 162 */
#define TESS_X_SLOTS        6912u        /* 20736 / 3 */
#define TESS_Y_SLOTS        6912u        /* 20736 / 3 */
#define TESS_Z_SLOTS        6912u        /* 20736 - 2 × 6912 */
#define TESS_AXIS_STRIDE    1728u        /* 12^3 = 20736 / 12 */
#define TESS_STRIDE_37      37u          /* coprime with 20736 */
#define TESS_HEADER_SIZE    64u
#define TESS_FORMULA_SIZE   64u
#define TESS_CRC_SIZE       8u

/* Cell sizes by GGML type */
#define TESS_CELL_F32       4u
#define TESS_CELL_F16       2u
#define TESS_CELL_BF16      2u
#define TESS_CELL_Q8_0      34u         /* 2B scale + 32B int8 */
#define TESS_CELL_Q4_0      18u         /* 2B scale + 16B int4 */
#define TESS_CELL_Q4_1      20u         /* 2B + 2B + 16B int4 */
#define TESS_CELL_Q5_0      22u
#define TESS_CELL_Q5_1      24u
#define TESS_CELL_RAW       1u          /* single int8 */
#define TESS_CELL_Q4_K      144u        /* K-quant block: 144B / 256 values */
#define TESS_CELL_Q5_K      176u        /* K-quant block: 176B / 256 values */
#define TESS_CELL_Q6_K      210u        /* K-quant block: 210B / 256 values */
#define TESS_CELL_Q8_K      292u        /* K-quant block: 292B / 256 values */

/* KIS axis indices */
#define TESS_AXIS_X         0u
#define TESS_AXIS_Y         1u
#define TESS_AXIS_Z         2u
#define TESS_NUM_AXES       3u
#define TESS_NUM_OCTANTS    8u

/* Optional section types */
#define TESS_SECTION_LUT    0x54554C00u  /* "LUT\0" */
#define TESS_SECTION OMAP   0x50414D4Fu  /* "OMAP" */
#define TESS_SECTION_STAB   0x42415453u  /* "STAB" */
#define TESS_SECTION_META   0x4154454Du  /* "META" */

/* GGML type indices (matching gguf_reader.h) */
#define TESS_GGML_F32       0u
#define TESS_GGML_F16       1u
#define TESS_GGML_Q4_0      2u
#define TESS_GGML_Q4_1      3u
#define TESS_GGML_Q5_0      6u
#define TESS_GGML_Q5_1      7u
#define TESS_GGML_Q8_0      8u
#define TESS_GGML_Q8_1      9u
#define TESS_GGML_BF16      30u
#define TESS_GGML_Q4_K      12u
#define TESS_GGML_Q5_K      13u
#define TESS_GGML_Q6_K      14u
#define TESS_GGML_Q8_K      15u

/* ═══════════════════════════════════════════════════════════════════════════
   HEADER STRUCT (64 bytes, packed)
   ═══════════════════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)

typedef struct {
    /* ── Identification (16 bytes) ─────────────────────────── */
    uint32_t magic;              /* 0x54455353 = "TESS"           */
    uint32_t version;            /* 1 = v1.0                      */
    uint32_t total_slots;        /* 20736 (sacred)                */
    uint32_t cell_size;          /* bytes per cell                */

    /* ── Geometric Parameters (16 bytes) ───────────────────── */
    uint32_t scale_factor;       /* fixed-point: scale × 65536    */
    uint32_t x_slots;            /* X-axis count (default 6912)   */
    uint32_t y_slots;            /* Y-axis count (default 6912)   */
    uint32_t z_slots;            /* Z-axis count (default 6912)   */

    /* ── Pipeline Metadata (16 bytes) ──────────────────────── */
    uint32_t gguf_type;          /* GGML quantization type        */
    uint32_t tensor_count;       /* number of tensors mapped      */
    uint64_t source_size;        /* original GGUF file size       */

    /* ── Seal (16 bytes) ───────────────────────────────────── */
    uint64_t cube_checksum;      /* CRC-64 of CubeData            */
    uint64_t formula_id;         /* hash of formula parameters     */
} TESS_Header;

/* Compile-time size check */
/* static_assert(sizeof(TESS_Header) == 64, "TESS_Header must be 64 bytes"); */

/* ═══════════════════════════════════════════════════════════════════════════
   FORMULA BLOCK (64 bytes, packed)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* ── Resolver Parameters (44 bytes) ────────────────────── */
    uint32_t mirror_axis_x;     /* X axis max (6912)            */
    uint32_t mirror_axis_y;     /* Y axis max (6912)            */
    uint32_t mirror_axis_z;     /* Z axis max (6912)            */
    uint32_t time_stride;       /* f(time) multiplier            */
    uint32_t cayley_offset[3];  /* Cayley transform offsets      */
    uint32_t octant_mask;       /* active octant bitmask (8-bit) */
    uint32_t stride_seed;       /* stride-37 seed for scatter    */
    uint32_t capo_id;           /* capo position (chunk index)   */

    /* ── Capo + LUT (24 bytes) ────────────────────────────── */
    uint8_t  capo_total;        /* total capos for this tensor   */
    uint8_t  voronoi_cell;      /* Voronoi cell id (0..23)        */
    uint8_t  voronoi_flags;     /* flags: bit0=masked, bit1=frozen */
    uint8_t  axis_id;           /* box axis (0-5, GBA_AXIS_*)     */
    uint32_t axis_position;     /* position on axis (identity)    */
    uint8_t  _pad[16];          /* reserved for future LUT       */
} TESS_Formula;                 /* total: 64 bytes               */

/* ═══════════════════════════════════════════════════════════════════════════
   OPTIONAL SECTION HEADER (8 bytes, packed)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t section_type;      /* 'LUT\0', 'OMAP', 'STAB', 'META' */
    uint32_t section_size;      /* bytes of payload                */
} TESS_SectionHdr;

/* ═══════════════════════════════════════════════════════════════════════════
   TENSOR TABLE ENTRY (variable length, packed)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t offset;            /* byte offset in CubeData        */
    uint64_t n_bytes;           /* tensor data bytes              */
    uint32_t n_dims;            /* dimension count (max 4)        */
    int64_t  dims[4];           /* dimensions (padded to 4)       */
    uint32_t name_len;          /* tensor name length             */
    /* char name[name_len] follows, padded to 8-byte alignment    */
} TESS_TensorEntry;

#pragma pack(pop)

/* ═══════════════════════════════════════════════════════════════════════════
   IN-MEMORY CONTAINER STRUCT
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    TESS_Header  *header;       /* pointer to mmap'd header       */
    TESS_Formula *formula;      /* pointer to formula block       */
    uint8_t      *cube_data;    /* pointer to CubeData            */
    uint64_t      cube_size;    /* total CubeData bytes           */
    void         *file_base;    /* mmap base (for cleanup)        */
    uint64_t      file_size;    /* total file size                */
} TESS_Container;

/* ═══════════════════════════════════════════════════════════════════════════
   ADDRESS RESOLUTION
   ═══════════════════════════════════════════════════════════════════════════ */

/* Select axis from flat slot */
static inline uint8_t tess_axis_select(uint32_t slot, const TESS_Header *h) {
    if (slot < h->x_slots) return TESS_AXIS_X;
    if (slot < h->x_slots + h->y_slots) return TESS_AXIS_Y;
    return TESS_AXIS_Z;
}

/* Get axis-local slot index */
static inline uint32_t tess_axis_slot(uint32_t slot, const TESS_Header *h) {
    if (slot < h->x_slots) return slot;
    if (slot < h->x_slots + h->y_slots) return slot - h->x_slots;
    return slot - h->x_slots - h->y_slots;
}

/* Get axis offset */
static inline uint32_t tess_axis_offset(uint8_t axis, const TESS_Header *h) {
    if (axis == TESS_AXIS_X) return 0;
    if (axis == TESS_AXIS_Y) return h->x_slots;
    return h->x_slots + h->y_slots;
}

/* ═══════════════════════════════════════════════════════════════════════════
   OCTANT RESOLUTION (8 mirror views)
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Octant sign encoding:
 *   bit 0 = X sign (0=+, 1=-)
 *   bit 1 = Y sign (0=+, 1=-)
 *   bit 2 = Z sign (0=+, 1=-)
 *
 * Octant 0 (+X+Y+Z) = identity (no mirror)
 * Octant 7 (-X-Y-Z) = full inversion
 */
static inline uint32_t tess_resolve_octant(uint32_t slot, uint8_t octant,
                                            const TESS_Header *h) {
    uint8_t axis = tess_axis_select(slot, h);
    uint32_t aslot = tess_axis_slot(slot, h);
    uint32_t axis_max = (axis == TESS_AXIS_X) ? h->x_slots :
                        (axis == TESS_AXIS_Y) ? h->y_slots : h->z_slots;

    /* Apply mirror in axis-local space: if sign bit set, flip within axis */
    uint8_t sign = (octant >> axis) & 1;
    uint32_t mirrored = sign ? (axis_max - 1 - aslot) : aslot;

    return mirrored + tess_axis_offset(axis, h);
}

/* ═══════════════════════════════════════════════════════════════════════════
   STRIDE-37 SCATTER (weight index → cube cell)
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t tess_stride_scatter(uint32_t weight_idx) {
    return (weight_idx * TESS_STRIDE_37) % TESS_TOTAL_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   SCALE-DRIVEN SCATTER (Phase 3: KIS-timeline integration)
   ═══════════════════════════════════════════════════════════════════════════
   At scale W the effective field shrinks: effective_slots = 20736 * 2^(-W/12).
   Scatter uses the same stride-37 but modulo effective_slots — different W
   gives a different scatter pattern (KIS principle: 6 values same position = 6
   data points from different topology).
   ═══════════════════════════════════════════════════════════════════════════ */

/* Compute effective field size from TESS_Header.scale_factor (uint16.16 fixed-point).
 * Returns TESS_TOTAL_SLOTS (20736) for W=0 (s=1.0). */
static inline uint32_t tess_effective_slots(const TESS_Header *h) {
    if (h->scale_factor >= 65536u) return TESS_TOTAL_SLOTS;
    return (uint32_t)(((uint64_t)TESS_TOTAL_SLOTS * h->scale_factor + 32768u) >> 16);
}

/* Scatter into arbitrary effective field size.
 * stride 37 is prime → coprime with any effective_slots not divisible by 37.
 * 20736 = 2^8 * 3^4, all powers-of-2 scaled values remain coprime. */
static inline uint32_t tess_stride_scatter_in(uint32_t weight_idx, uint32_t effective_slots) {
    return (weight_idx * TESS_STRIDE_37) % effective_slots;
}

/*
 * Octant-aware scatter: scatter + zero-sum validation.
 * If the scattered slot lands in an invalid cube (zero-sum > 1),
 * redirect to the antipodal valid cube.
 *
 * Returns: flat address in a valid cube.
 * Cost: scatter + oct_cube_of + oct_is_valid + oct_antipode_flat.
 */
static inline uint32_t tess_stride_scatter_octant(uint32_t weight_idx) {
    uint32_t slot = (weight_idx * TESS_STRIDE_37) % TESS_TOTAL_SLOTS;
    uint8_t cube = (uint8_t)(slot / OCT_CELLS);
    if (!oct_is_valid(cube)) {
        slot = oct_antipode_flat(slot);
    }
    return slot;
}

/*
 * Voronoi-masked scatter: scatter + cell-restricted addressing.
 * Decomposes flat address into (cell_id, local_offset).
 * Pointer stays within cell boundary — observer sees small range only.
 *
 * Returns: flat address (cell_id × 864 + local).
 * Cost: scatter + vm_mask + vm_unmask.
 */
static inline uint32_t tess_stride_scatter_voronoi(uint32_t weight_idx) {
    uint32_t slot = (weight_idx * TESS_STRIDE_37) % TESS_TOTAL_SLOTS;
    MaskedPointer p = vm_mask(slot);
    return vm_unmask(p);
}

static inline uint32_t tess_stride_gather(uint32_t cell_idx) {
    /* Inverse: cell = (weight × 37) mod 20736
     * Inverse multiplier: 37^(-1) mod 20736
     * Since gcd(37, 20736) = 1, inverse exists.
     * 37 × 16813 = 622,081 = 30 × 20736 + 1
     * So inverse = 16813 */
    return (cell_idx * 16813u) % TESS_TOTAL_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   HYPERBOLIC ADDRESS RESOLVER
   ═══════════════════════════════════════════════════════════════════════════ */

/* Always need math.h for Q8_0 dequantization (ldexpf) */
#include <math.h>

#ifndef TESS_PI
#define TESS_PI 3.14159265358979323846
#endif

/*
 * Resolve address through KIS projection.
 * Formula: x × f(time) = address
 *
 * Encode: data stored at creation point (scale 1.0)
 * Decode: address = tess_resolve(slot, target_scale, header)
 *
 * Speed: 10 ns/op with stored angle, 182 ns/op with atan2.
 */
static inline uint32_t tess_resolve(uint32_t slot, uint32_t target_scale,
                                     const TESS_Header *h) {
    uint8_t axis = tess_axis_select(slot, h);
    uint32_t aslot = tess_axis_slot(slot, h);
    uint32_t axis_slots = (axis == TESS_AXIS_X) ? h->x_slots :
                          (axis == TESS_AXIS_Y) ? h->y_slots : h->z_slots;

#ifndef TESS_NO_MATH
    /* Angle within axis */
    double angle = 2.0 * TESS_PI * (double)aslot / (double)axis_slots;

    /* Add axis offset (120° spacing for 3 axes) */
    angle += (double)axis * 2.0 * TESS_PI / 3.0;

    /* Apply scale transform */
    double ratio = (double)target_scale / (double)h->scale_factor;
    double new_angle = angle * ratio;

    /* Normalize to [0, 2π) */
    while (new_angle < 0) new_angle += 2.0 * TESS_PI;
    while (new_angle >= 2.0 * TESS_PI) new_angle -= 2.0 * TESS_PI;

    /* Remove axis offset */
    double a = new_angle;
    a -= (double)axis * 2.0 * TESS_PI / 3.0;
    if (a < 0) a += 2.0 * TESS_PI;

    /* Map back to slot */
    uint32_t result = (uint32_t)(a * (double)axis_slots / (2.0 * TESS_PI) + 0.5);
    return (result % axis_slots) + tess_axis_offset(axis, h);
#else
    /* Integer-only fallback (approximate) */
    (void)axis; (void)aslot; (void)axis_slots; (void)target_scale;
    return slot; /* identity at scale 1.0 */
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
   Q8_0 DEQUANTIZATION
   ═══════════════════════════════════════════════════════════════════════════ */

/* Q8_0 block: 2B FP16 scale + 32B int8 weights = 34 bytes
 * Scale convention: (u16 & 0x7FFF) / 1024.0f, sign in bit 15
 * NOT IEEE f16 — fixed-point 15-bit */
static inline float tess_q80_fp16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    float f;
    if (exp == 0)
        f = (float)mant / 1024.0f * 5.960464478e-8f;
    else if (exp == 31)
        f = mant ? __builtin_nanf("") : __builtin_inff();
    else {
        f = (float)mant / 1024.0f + 1.0f;
        f = ldexpf(f, (int)exp - 15);
    }
    return sign ? -f : f;
}

/* Decode Q8_0 block to float32 array */
static inline void tess_q80_decode(const uint8_t *block, float *out) {
    uint16_t scale_raw = *(const uint16_t *)block;
    float scale = tess_q80_fp16_to_f32(scale_raw);
    for (int i = 0; i < 32; i++) {
        out[i] = (float)(int8_t)block[2 + i] * scale;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CONTAINER INITIALIZATION
   ═══════════════════════════════════════════════════════════════════════════ */

/* Initialize header with default values */
static inline void tess_header_init(TESS_Header *h, uint32_t gguf_type,
                                     uint32_t cell_size) {
    memset(h, 0, sizeof(*h));
    h->magic         = GEO_TESS_MAGIC;
    h->version       = TESS_VERSION;
    h->total_slots   = TESS_TOTAL_SLOTS;
    h->cell_size     = cell_size;
    h->scale_factor  = 65536u;    /* scale 1.0 */
    h->x_slots       = TESS_X_SLOTS;
    h->y_slots       = TESS_Y_SLOTS;
    h->z_slots       = TESS_Z_SLOTS;
    h->gguf_type     = gguf_type;
    h->tensor_count  = 0;
    h->source_size   = 0;
    h->cube_checksum = 0;
    h->formula_id    = 0;
}

/* Initialize formula with default values */
static inline void tess_formula_init(TESS_Formula *f) {
    memset(f, 0, sizeof(*f));
    f->mirror_axis_x = 6912u;     /* axis max */
    f->mirror_axis_y = 6912u;
    f->mirror_axis_z = 6912u;
    f->time_stride   = 1u;
    f->octant_mask   = 0xFFu;     /* all 8 octants active */
    f->stride_seed   = TESS_STRIDE_37;
    f->capo_id       = 0;
    f->capo_total    = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   SCALE STAMPING (KIS-timeline integration)
   scale_factor = s × 65536 (uint32_t fixed-point 16.16)
   W=0 → s=1.0 → scale_factor=65536; W=12 → s=0.5 → scale_factor=32768
   ═══════════════════════════════════════════════════════════════════════════ */

/* Extract the KIS ring tooth W from a tess header's scale_factor. */
static inline uint32_t tess_scale_w(const TESS_Header *h) {
    double s = (double)h->scale_factor / 65536.0;
    if (s <= 0.0 || s > 1.0) return 0;
    /* W = round(-12 * log2(s)), wrapped to [0,144) */
    double teeth = -12.0 * log2(s);
    long long w = (teeth >= 0.0) ? (long long)(teeth + 0.5) : (long long)(teeth - 0.5);
    w %= 144;
    if (w < 0) w += 144;
    return (uint32_t)w;
}

/* Set scale_factor from a KIS ring tooth W. */
static inline void tess_set_scale_w(TESS_Header *h, uint32_t w) {
    double s = exp2(-(double)(w % 144) / 12.0);
    h->scale_factor = (uint32_t)(s * 65536.0 + 0.5);
}

/* Is this capo at the home scale (W=0, s=1.0)? */
static inline int tess_is_home_scale(const TESS_Header *h) {
    return h->scale_factor == 65536u || h->scale_factor == 65535u;
}

/* ═══════════════════════════════════════════════════════════════════════════
   VALIDATION
   ═══════════════════════════════════════════════════════════════════════════ */

/* Validate header fields */
static inline int tess_header_validate(const TESS_Header *h) {
    if (h->magic != GEO_TESS_MAGIC) return -1;       /* bad magic */
    if (h->version != TESS_VERSION) return -2;    /* bad version */
    if (h->total_slots != TESS_TOTAL_SLOTS) return -3; /* wrong slots */
    if (h->x_slots + h->y_slots + h->z_slots != TESS_TOTAL_SLOTS)
        return -4;                                 /* axis mismatch */
    if (h->cell_size == 0) return -5;             /* zero cell size */
    return 0;  /* valid */
}

/* Validate octant roundtrip (mirror is self-inverse: apply same octant twice) */
static inline int tess_octant_roundtrip(const TESS_Header *h) {
    for (uint32_t slot = 0; slot < TESS_TOTAL_SLOTS; slot++) {
        for (uint8_t oct = 0; oct < TESS_NUM_OCTANTS; oct++) {
            uint32_t addr = tess_resolve_octant(slot, oct, h);
            uint32_t back = tess_resolve_octant(addr, oct, h);
            if (back != slot) return 0;  /* roundtrip failed */
        }
    }
    return 1;  /* all roundtrips pass */
}

/* ═══════════════════════════════════════════════════════════════════════════
   CONVENIENCE: GGUF TYPE → CELL SIZE
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t tess_gguf_type_to_cell_size(uint32_t gguf_type) {
    switch (gguf_type) {
        case TESS_GGML_F32:  return TESS_CELL_F32;
        case TESS_GGML_F16:  return TESS_CELL_F16;
        case TESS_GGML_Q4_0: return TESS_CELL_Q4_0;
        case TESS_GGML_Q4_1: return TESS_CELL_Q4_1;
        case TESS_GGML_Q5_0: return TESS_CELL_Q5_0;
        case TESS_GGML_Q5_1: return TESS_CELL_Q5_1;
        case TESS_GGML_Q8_0: return TESS_CELL_Q8_0;
        case TESS_GGML_Q8_1: return 36u;  /* 2B + 2B + 32B */
        case TESS_GGML_BF16: return TESS_CELL_BF16;
        case TESS_GGML_Q4_K: return TESS_CELL_Q4_K;
        case TESS_GGML_Q5_K: return TESS_CELL_Q5_K;
        case TESS_GGML_Q6_K: return TESS_CELL_Q6_K;
        case TESS_GGML_Q8_K: return TESS_CELL_Q8_K;
        default: return 0;  /* unknown type */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CRC-64/ECMA-182 (non-reflected MSB-first)
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t tess_crc64(const uint8_t *data, uint64_t len) {
    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    const uint64_t poly = 0x42F0E1EBA9EA3693ULL;

    for (uint64_t i = 0; i < len; i++) {
        crc ^= (uint64_t)data[i] << 56;
        for (int j = 0; j < 8; j++) {
            crc = (crc & (1ULL << 63)) ? ((crc << 1) ^ poly) : (crc << 1);
        }
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
   STREAMING CAPO API (lazy per-capo decode)
   ═══════════════════════════════════════════════════════════════════════════
   For inference: open one capo file, decode only the elements you need,
   close when done. No big buffer. Like a guitar capo: shifts offset within
   source tensor while keeping 20736-slot cube structure.

   Usage:
     TESS_CapoReader r;
     tess_capo_open(&r, "tensor_capo3.tess");
     // decode element 42 from this capo:
     uint8_t cell[TESS_CELL_Q4_K];
     tess_capo_load_elem(&r, 42, cell);
     tess_capo_close(&r);
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    FILE         *f;           /* open file handle (NULL = closed) */
    uint8_t      *buf;         /* mmap'd or malloc'd file content */
    uint64_t      file_sz;     /* total file size */
    const TESS_Header  *hdr;  /* pointer into buf */
    const TESS_Formula *fml;  /* pointer into buf */
    const uint8_t *cube_data;  /* pointer to CubeData in buf */
    uint32_t      cube_bytes;  /* total_slots * cell_size */
    uint32_t      n_elems;     /* elements in this capo */
    uint32_t      cell_size;   /* bytes per cell */
    int           owns_buf;    /* 1 if we malloc'd buf, 0 if caller-provided */
} TESS_CapoReader;

/* Open a .tess capo file for streaming decode.
 * Returns 0 on success, negative on error.
 * Caller must call tess_capo_close() when done. */
static inline int tess_capo_open(TESS_CapoReader *r, const char *path) {
    memset(r, 0, sizeof(*r));
    r->f = fopen(path, "rb");
    if (!r->f) return -1;

    fseek(r->f, 0, SEEK_END);
    r->file_sz = (uint64_t)ftell(r->f);
    fseek(r->f, 0, SEEK_SET);

    if (r->file_sz < TESS_HEADER_SIZE + TESS_FORMULA_SIZE + TESS_CRC_SIZE) {
        fclose(r->f); r->f = NULL; return -2;
    }

    r->buf = (uint8_t *)malloc((size_t)r->file_sz);
    if (!r->buf) { fclose(r->f); r->f = NULL; return -3; }
    r->owns_buf = 1;

    if (fread(r->buf, 1, (size_t)r->file_sz, r->f) != (size_t)r->file_sz) {
        free(r->buf); r->buf = NULL; fclose(r->f); r->f = NULL; return -4;
    }
    fclose(r->f); r->f = NULL;

    r->hdr = (const TESS_Header *)r->buf;
    r->fml = (const TESS_Formula *)(r->buf + TESS_HEADER_SIZE);
    if (tess_header_validate(r->hdr) != 0) {
        free(r->buf); r->buf = NULL; return -5;
    }

    r->cell_size  = r->hdr->cell_size;
    r->cube_bytes = r->hdr->total_slots * r->cell_size;
    r->cube_data  = r->buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
    r->n_elems    = r->hdr->tensor_count ? r->hdr->tensor_count : r->hdr->total_slots;
    return 0;
}

/* Open from a buffer (caller owns memory, no copy).
 * Returns 0 on success. Buffer must stay valid until tess_capo_close(). */
static inline int tess_capo_open_buf(TESS_CapoReader *r, uint8_t *buf, uint64_t sz) {
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->file_sz = sz;
    r->owns_buf = 0;

    if (sz < TESS_HEADER_SIZE + TESS_FORMULA_SIZE + TESS_CRC_SIZE) return -2;

    r->hdr = (const TESS_Header *)buf;
    r->fml = (const TESS_Formula *)(buf + TESS_HEADER_SIZE);
    if (tess_header_validate(r->hdr) != 0) return -5;

    r->cell_size  = r->hdr->cell_size;
    r->cube_bytes = r->hdr->total_slots * r->cell_size;
    r->cube_data  = buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
    r->n_elems    = r->hdr->tensor_count ? r->hdr->tensor_count : r->hdr->total_slots;
    return 0;
}

/* Close reader, free buffer if we own it. */
static inline void tess_capo_close(TESS_CapoReader *r) {
    if (r->owns_buf && r->buf) { free(r->buf); r->buf = NULL; }
    if (r->f) { fclose(r->f); r->f = NULL; }
    r->hdr = NULL; r->fml = NULL; r->cube_data = NULL;
}

/* Load a single cell from this capo by element index.
 * dst must point to at least cell_size bytes.
 * Returns cell_size on success, 0 on out-of-range. */
static inline int tess_capo_load_elem(const TESS_CapoReader *r, uint32_t idx,
                                      void *dst) {
    if (idx >= r->n_elems) return 0;
    uint32_t slot = tess_stride_scatter(idx);
    if (slot >= TESS_TOTAL_SLOTS) slot = idx % TESS_TOTAL_SLOTS;
    uint32_t src_off = slot * r->cell_size;
    if (src_off + r->cell_size > r->cube_bytes) return 0;
    memcpy(dst, r->cube_data + src_off, r->cell_size);
    return (int)r->cell_size;
}

/* Load a range of elements [start, start+n) into dst.
 * Returns number of bytes written, or 0 on error.
 * dst must hold n * cell_size bytes. */
static inline int tess_capo_load_range(const TESS_CapoReader *r, uint32_t start,
                                       uint32_t n, void *dst) {
    if (start + n > r->n_elems) return 0;
    uint8_t *out = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = tess_stride_scatter(start + i);
        if (slot >= TESS_TOTAL_SLOTS) slot = (start + i) % TESS_TOTAL_SLOTS;
        uint32_t src_off = slot * r->cell_size;
        if (src_off + r->cell_size > r->cube_bytes) return 0;
        memcpy(out + (uint64_t)i * r->cell_size, r->cube_data + src_off, r->cell_size);
    }
    return (int)(n * r->cell_size);
}

/* Derive capo path from base path + capo index.
 * Same logic as capo_path() in tess_load.c. */
static inline void tess_capo_make_path(char *dst, size_t cap, const char *base, uint32_t c) {
    size_t len = strlen(base);
    if (len > 11 && strcmp(base + len - 11, "_capo0.tess") == 0) {
        snprintf(dst, cap, "%.*s_capo%u.tess", (int)(len - 11), base, c);
    } else if (len > 5 && strcmp(base + len - 5, ".tess") == 0) {
        snprintf(dst, cap, "%.*s_capo%u.tess", (int)(len - 5), base, c);
    } else {
        snprintf(dst, cap, "%s_capo%u", base, c);
    }
}

/* Verify CRC-64 of a capo reader's cube data.
 * Returns 1 if CRC matches, 0 if mismatch. */
static inline int tess_capo_verify_crc(const TESS_CapoReader *r) {
    uint64_t stored;
    memcpy(&stored, r->cube_data + r->cube_bytes, TESS_CRC_SIZE);
    return tess_crc64(r->cube_data, r->cube_bytes) == stored;
}

/* ═══════════════════════════════════════════════════════════════════════════
   .TESSPACK READER (single-file multi-tensor container)
   ═══════════════════════════════════════════════════════════════════════════
   .tesspack = single file containing all capos for all tensors.
   Index at end-of-file (offset stored in header bytes 12-15).

   Usage:
     TESS_CapoReader r;
     tess_capo_open_pack(&r, "model.tesspack", "blk.0.ffn_down_exps.weight", 3);
     // ... load elems ...
     tess_capo_close(&r);
   ═══════════════════════════════════════════════════════════════════════════ */

#define TPAK_MAGIC   0x5450414Bu  /* "TPAK" little-endian */
#define TPAK_VERSION 1u

/* Open a specific capo from a .tesspack file.
 * Scans the index for matching tensor_name + capo_id.
 * Returns 0 on success, negative on error.
 * Note: reads entire pack index into memory (one-time cost). */
static inline int tess_capo_open_pack(TESS_CapoReader *r, const char *pack_path,
                                       const char *tensor_name, uint32_t capo_id) {
    memset(r, 0, sizeof(*r));

    FILE *f = fopen(pack_path, "rb");
    if (!f) return -1;

    /* read pack header (64 bytes) */
    uint32_t hdr[16];
    if (fread(hdr, 1, 64, f) != 64 || hdr[0] != TPAK_MAGIC) {
        fclose(f); return -2;
    }
    uint32_t n_capos = hdr[2];
    uint64_t index_offset = hdr[3];

    /* scan index for matching entry */
    _fseeki64(f, (int64_t)index_offset, SEEK_SET);
    uint64_t capo_offset = 0;
    uint32_t capo_size = 0;
    int found = 0;

    for (uint32_t i = 0; i < n_capos; i++) {
        uint8_t name_len;
        if (fread(&name_len, 1, 1, f) != 1) break;
        if (name_len == 0) break;
        char name[256];
        if (fread(name, 1, name_len, f) != name_len) break;
        name[name_len] = 0;
        uint32_t cid;
        uint64_t offset;
        uint32_t size;
        if (fread(&cid, 4, 1, f) != 1) break;
        if (fread(&offset, 8, 1, f) != 1) break;
        if (fread(&size, 4, 1, f) != 1) break;

        if (strcmp(name, tensor_name) == 0 && cid == capo_id) {
            capo_offset = offset;
            capo_size = size;
            found = 1;
            break;
        }
    }

    if (!found) { fclose(f); return -6; }

    /* read capo data into buffer */
    r->f = f;
    _fseeki64(f, (int64_t)capo_offset, SEEK_SET);

    r->buf = (uint8_t *)malloc(capo_size);
    if (!r->buf) { fclose(f); r->f = NULL; return -3; }
    r->owns_buf = 1;
    r->file_sz = capo_size;

    if (fread(r->buf, 1, capo_size, f) != capo_size) {
        free(r->buf); r->buf = NULL; fclose(f); r->f = NULL; return -4;
    }
    fclose(f); r->f = NULL;

    r->hdr = (const TESS_Header *)r->buf;
    r->fml = (const TESS_Formula *)(r->buf + TESS_HEADER_SIZE);
    if (tess_header_validate(r->hdr) != 0) {
        free(r->buf); r->buf = NULL; return -5;
    }

    r->cell_size  = r->hdr->cell_size;
    r->cube_bytes = r->hdr->total_slots * r->cell_size;
    r->cube_data  = r->buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
    r->n_elems    = r->hdr->tensor_count ? r->hdr->tensor_count : r->hdr->total_slots;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Residual region — table-driven redirects for weight-tying & phantom tensors
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TPAK_RESIDUAL_SENTINEL 0x52455344u  /* "RESD" */

enum {
    TESS_TRANSFORM_NONE        = 0,  /* memcpy src → dst (same dtype) */
    TESS_TRANSFORM_TYPE_CAST   = 1,  /* dequantize src_type → dst_type */
    TESS_TRANSFORM_FP16_TO_F32 = 2,  /* f16 → f32 upcast */
    TESS_TRANSFORM_F32_TO_FP16 = 3,  /* f32 → f16 downcast */
    TESS_TRANSFORM_IDENTITY    = 4,  /* fill with 1.0f */
    TESS_TRANSFORM_ZERO        = 5,  /* fill with 0 */
};

#pragma pack(push, 1)
typedef struct {
    uint8_t  name_len;          /* length of tensor_name (max 255) */
    char     name[255];         /* tensor name (e.g. "output.weight") */
    uint8_t  src_len;           /* length of src_tensor_name */
    char     src[255];          /* source tensor name (e.g. "token_embd.weight") */
    uint8_t  src_type;          /* GGML type of source data in pack */
    uint8_t  dst_type;          /* GGML type callback should report */
    uint8_t  transform;         /* TESS_TRANSFORM_* enum */
    uint8_t  _pad;              /* alignment padding */
} TESS_ResidualEntry;           /* total: 522 bytes */
#pragma pack(pop)

/* fp16 → float conversion (IEEE 754 half-precision) */
static inline float fp16_to_float(uint16_t h) {
    uint32_t s = (h & 0x8000) << 16;
    uint32_t e = (h >> 10) & 0x1F;
    uint32_t m = h & 0x3FF;
    if (e == 0) {
        if (m == 0) return *(float *)&s;
        while (!(m & 0x400)) { m <<= 1; e++; }
        e++; m &= ~0x400;
    } else if (e == 31) {
        e = 127 + 15;
    } else {
        e += 127 - 15;
    }
    uint32_t f = s | (e << 23) | (m << 13);
    return *(float *)&f;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TESS_PackIndex — mmap-based .tesspack reader
 * ═══════════════════════════════════════════════════════════════════════════
 * Two open modes:
 *   tess_pack_open()      — reads index into malloc, mmaps data (original)
 *   tess_pack_open_mmap() — mmaps entire file, walks index from mmap (zero malloc)
 *
 * mmap-only mode: initial RSS = header (64 bytes), index pages fault in on
 * first walk, tensor data pages fault on access.  No malloc, no fread.
 */
typedef struct {
    uint8_t *base;          /* mmap base pointer (NULL on Windows without mmap) */
    uint64_t file_sz;       /* total file size */
    int      fd;            /* file descriptor for mmap */
    void    *mmap_ptr;      /* OS mmap handle (NULL if using fallback) */

    /* index entries */
    struct {
        char    name[256];
        uint32_t capo_id;
        uint64_t offset;
        uint32_t size;
    } *entries;
    uint32_t n_entries;

    /* pack header */
    uint32_t n_capos;
    uint64_t index_offset;

    /* residual section (version 2+) */
    const uint8_t *residual_data;
    uint32_t       residual_count;

    /* scale log section (version 3+) */
    const uint8_t *scale_log_data;   /* pointer to first ScaleLogEntry in mmap */
    uint32_t       scale_log_count;  /* number of entries */
    uint32_t       pack_version;     /* header version (1, 2, or 3) */
} TESS_PackIndex;

/* Free resources held by a PackIndex. */
static inline void tess_pack_close(TESS_PackIndex *pi) {
    if (pi->entries) { free(pi->entries); pi->entries = NULL; }
#ifdef _WIN32
    if (pi->mmap_ptr) { UnmapViewOfFile(pi->mmap_ptr); pi->mmap_ptr = NULL; }
    if (pi->fd >= 0)  { CloseHandle((HANDLE)(intptr_t)pi->fd); pi->fd = -1; }
#else
    if (pi->base && pi->base != MAP_FAILED) { munmap(pi->base, pi->file_sz); pi->base = NULL; }
    if (pi->fd >= 0) { close(pi->fd); pi->fd = -1; }
#endif
    pi->n_entries = 0;
}

/* Open a .tesspack file: read index + mmap data region.
 * Returns 0 on success, negative on error. */
static inline int tess_pack_open(TESS_PackIndex *pi, const char *pack_path) {
    memset(pi, 0, sizeof(*pi));
    pi->fd = -1;

    FILE *f = fopen(pack_path, "rb");
    if (!f) return -1;

    /* read header */
    uint32_t hdr[16];
    if (fread(hdr, 1, 64, f) != 64 || hdr[0] != TPAK_MAGIC) {
        fclose(f); return -2;
    }
    pi->n_capos = hdr[2];
    pi->index_offset = hdr[3];

    /* get file size */
#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END);
    pi->file_sz = (uint64_t)_ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    pi->file_sz = (uint64_t)ftello(f);
#endif
    fclose(f); f = NULL;

    /* read index into memory */
    pi->entries = (void *)malloc(pi->n_capos * sizeof(pi->entries[0]));
    if (!pi->entries) return -3;

    f = fopen(pack_path, "rb");
    if (!f) { free(pi->entries); pi->entries = NULL; return -1; }

    _fseeki64(f, (int64_t)pi->index_offset, SEEK_SET);
    uint32_t n = 0;
    for (uint32_t i = 0; i < pi->n_capos && n < pi->n_capos; i++) {
        uint8_t name_len;
        if (fread(&name_len, 1, 1, f) != 1 || name_len == 0) break;
        char name[256];
        if (fread(name, 1, name_len, f) != name_len) break;
        name[name_len] = 0;
        uint32_t cid;
        uint64_t offset;
        uint32_t size;
        if (fread(&cid, 4, 1, f) != 1) break;
        if (fread(&offset, 8, 1, f) != 1) break;
        if (fread(&size, 4, 1, f) != 1) break;

        strncpy(pi->entries[n].name, name, 255);
        pi->entries[n].name[255] = 0;
        pi->entries[n].capo_id = cid;
        pi->entries[n].offset  = offset;
        pi->entries[n].size    = size;
        n++;
    }
    fclose(f);
    pi->n_entries = n;

    /* mmap the entire file for zero-copy capo reads */
#ifdef _WIN32
    HANDLE hFile = CreateFileA(pack_path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return -4;
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) { CloseHandle(hFile); return -4; }
    pi->mmap_ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMap);
    CloseHandle(hFile);
    if (!pi->mmap_ptr) return -4;
    pi->base = (uint8_t *)pi->mmap_ptr;
#else
    int fd = open(pack_path, O_RDONLY);
    if (fd < 0) return -4;
    pi->base = (uint8_t *)mmap(NULL, pi->file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (pi->base == MAP_FAILED) { close(fd); pi->base = NULL; return -4; }
    pi->fd = fd;
#endif

    /* residual section (version 2+) — hdr[6]=offset, hdr[7]=count */
    {
        uint32_t r_off = hdr[6];
        uint32_t r_cnt = hdr[7];
        if (r_off > 0 && r_cnt > 0 && r_off + (uint64_t)r_cnt * 522 <= pi->file_sz) {
            pi->residual_data  = pi->base + r_off;
            pi->residual_count = r_cnt;
        }
    }

    /* scale log section (version 3+) — hdr[9]=offset, hdr[10]=count */
    pi->pack_version = hdr[1];
    {
        uint32_t sl_off = hdr[9];
        uint32_t sl_cnt = hdr[10];
        if (sl_off > 0 && sl_cnt > 0 && sl_off + (uint64_t)sl_cnt * 8 <= pi->file_sz) {
            pi->scale_log_data  = pi->base + sl_off;
            pi->scale_log_count = sl_cnt;
        }
    }

    return 0;
}

/* Reserved index entry name for embedded GGUF header.
 * When present, the entry stores raw GGUF header bytes (0..data_offset)
 * so the pack is self-contained for llama.cpp metadata (KV + tensor info). */
#define TPAK_GGUF_HEADER_NAME "__gguf_header__"

/* ═══════ Zero-malloc mmap mode ═══════════════════════════════════════════
 * Mmaps the entire file, parses header + walks index from the mmap pointer.
 * No malloc, no fread.  Index pages are faulted in on first walk.
 * The mmap_ptr field doubles as the "is mmap-only" flag (entries == NULL).
 * Use tess_pack_get_capo_mmap() for lookups.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Open a .tesspack in zero-malloc mmap mode.
 * Returns 0 on success.  Call tess_pack_close() to release. */
static inline int tess_pack_open_mmap(TESS_PackIndex *pi, const char *pack_path) {
    memset(pi, 0, sizeof(*pi));
    pi->fd = -1;
    pi->entries = NULL;  /* mmap-only mode: no malloc'd index */

    /* mmap the entire file — single syscall, no I/O for index */
#ifdef _WIN32
    HANDLE hFile = CreateFileA(pack_path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return -1;
    /* get file size */
    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return -1; }
    pi->file_sz = (uint64_t)li.QuadPart;
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) { CloseHandle(hFile); return -1; }
    pi->mmap_ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMap);
    CloseHandle(hFile);
    if (!pi->mmap_ptr) return -1;
    pi->base = (uint8_t *)pi->mmap_ptr;
#else
    int fd = open(pack_path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    pi->file_sz = (uint64_t)st.st_size;
    pi->base = (uint8_t *)mmap(NULL, pi->file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (pi->base == MAP_FAILED) { close(fd); pi->base = NULL; return -1; }
    pi->fd = fd;
#endif

    /* parse header from mmap — faults in page 0 only */
    if (pi->file_sz < 64) return -2;
    const uint32_t *hdr = (const uint32_t *)pi->base;
    if (hdr[0] != TPAK_MAGIC) return -2;
    pi->n_capos     = hdr[2];
    pi->index_offset = hdr[3];

    /* residual section (version 2+) */
    {
        uint32_t r_off = hdr[6];
        uint32_t r_cnt = hdr[7];
        if (r_off > 0 && r_cnt > 0 && r_off + (uint64_t)r_cnt * sizeof(TESS_ResidualEntry) <= pi->file_sz) {
            pi->residual_data  = pi->base + r_off;
            pi->residual_count = r_cnt;
        }
    }

    /* scale log section (version 3+) */
    pi->pack_version = hdr[1];
    {
        uint32_t sl_off = hdr[9];
        uint32_t sl_cnt = hdr[10];
        if (sl_off > 0 && sl_cnt > 0 && sl_off + (uint64_t)sl_cnt * 8 <= pi->file_sz) {
            pi->scale_log_data  = pi->base + sl_off;
            pi->scale_log_count = sl_cnt;
        }
    }

    return 0;
}

/* ═══════ Scale log query ═══════════════════════════════════════════════════
 * Replay the ΔW scale-change log to find the pack's current scale.
 * Returns the final W (ring position 0..143) after replaying all events.
 * If no log exists, returns 0 (home scale, s=1.0).
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline uint32_t tess_pack_get_scale_w(const TESS_PackIndex *pi) {
    uint32_t w = 0;
    const uint8_t *p = pi->scale_log_data;
    for (uint32_t i = 0; i < pi->scale_log_count && p; i++) {
        uint32_t capo_id = *(const uint32_t *)p;
        uint16_t old_w   = *(const uint16_t *)(p + 4);
        uint16_t new_w   = *(const uint16_t *)(p + 6);
        (void)capo_id; (void)old_w;  /* for now: just take the last new_w */
        w = new_w;
        p += 8;
    }
    return w;
}

/* Walk mmap'd index to find capo by name + capo_id.
 * The index is variable-length, so we must scan linearly.
 * Returns 0 on success, r->buf points directly into mmap. */
static inline int tess_pack_get_capo_mmap(TESS_PackIndex *pi, TESS_CapoReader *r,
                                          const char *tensor_name, uint32_t capo_id) {
    memset(r, 0, sizeof(*r));
    if (!pi->base || pi->index_offset >= pi->file_sz) return -6;

    const uint8_t *cur = pi->base + pi->index_offset;
    const uint8_t *end = pi->base + pi->file_sz;
    uint32_t tname_len = (uint32_t)strlen(tensor_name);

    for (uint32_t i = 0; i < pi->n_capos; i++) {
        if (cur + 1 > end) return -6;
        uint8_t name_len = *cur++;
        if (cur + name_len + 16 > end) return -6;
        const uint8_t *name_ptr = cur;
        cur += name_len;
        uint32_t cid    = *(const uint32_t *)cur;
        uint64_t offset = *(const uint64_t *)(cur + 4);
        uint32_t sz     = *(const uint32_t *)(cur + 12);
        cur += 16;

        if (cid == capo_id && name_len == (uint8_t)tname_len &&
            memcmp(name_ptr, tensor_name, name_len) == 0) {

            if (offset + sz > pi->file_sz) return -4;
            r->buf = pi->base + offset;
            r->owns_buf = 0;
            r->file_sz  = sz;
            r->f = NULL;
            r->hdr = (const TESS_Header *)r->buf;
            r->fml = (const TESS_Formula *)(r->buf + TESS_HEADER_SIZE);
            if (tess_header_validate(r->hdr) != 0) return -5;
            r->cell_size  = r->hdr->cell_size;
            r->cube_bytes = r->hdr->total_slots * r->cell_size;
            r->cube_data  = r->buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
            r->n_elems    = r->hdr->tensor_count ? r->hdr->tensor_count : r->hdr->total_slots;
            return 0;
        }
    }
    return -6;
}

/* Retrieve the embedded GGUF header from a .tesspack.
 * Returns pointer to the header bytes (in mmap) and sets *hdr_sz.
 * Returns NULL if not found.  The returned pointer is valid until
 * tess_pack_close().  Do NOT free it.
 * Works with both malloc-index and mmap-only modes. */
static inline const uint8_t *tess_pack_get_gguf_header(const TESS_PackIndex *pi,
                                                        uint64_t *hdr_sz) {
    if (pi->entries) {
        /* malloc-index mode: linear scan entries array */
        for (uint32_t i = 0; i < pi->n_entries; i++) {
            if (pi->entries[i].capo_id == 0 &&
                strcmp(pi->entries[i].name, TPAK_GGUF_HEADER_NAME) == 0) {
                uint64_t off = pi->entries[i].offset;
                uint32_t sz  = pi->entries[i].size;
                if (off + sz > pi->file_sz) return NULL;
                *hdr_sz = sz;
                return pi->base + off;
            }
        }
    } else if (pi->base && pi->index_offset < pi->file_sz) {
        /* mmap-only mode: walk mmap'd index for __gguf_header__ */
        const uint8_t *cur = pi->base + pi->index_offset;
        const uint8_t *end = pi->base + pi->file_sz;
        uint32_t gname_len = (uint32_t)strlen(TPAK_GGUF_HEADER_NAME);
        for (uint32_t i = 0; i < pi->n_capos; i++) {
            if (cur + 1 > end) break;
            uint8_t name_len = *cur++;
            if (cur + name_len + 16 > end) break;
            const uint8_t *name_ptr = cur;
            cur += name_len;
            uint32_t cid    = *(const uint32_t *)cur;
            uint64_t off    = *(const uint64_t *)(cur + 4);
            uint32_t sz     = *(const uint32_t *)(cur + 12);
            cur += 16;
            if (cid == 0 && name_len == (uint8_t)gname_len &&
                memcmp(name_ptr, TPAK_GGUF_HEADER_NAME, name_len) == 0) {
                if (off + sz > pi->file_sz) return NULL;
                *hdr_sz = sz;
                return pi->base + off;
            }
        }
    }
    *hdr_sz = 0;
    return NULL;
}

/* Open a specific capo from a mmapped pack index.
 * Returns 0 on success, sets r->buf to point into mmap (no copy needed). */
static inline int tess_pack_get_capo(TESS_PackIndex *pi, TESS_CapoReader *r,
                                     const char *tensor_name, uint32_t capo_id) {
    memset(r, 0, sizeof(*r));

    /* linear scan index (could be hash table for larger packs) */
    for (uint32_t i = 0; i < pi->n_entries; i++) {
        if (pi->entries[i].capo_id == capo_id &&
            strcmp(pi->entries[i].name, tensor_name) == 0) {

            uint64_t off = pi->entries[i].offset;
            uint32_t sz  = pi->entries[i].size;
            if (off + sz > pi->file_sz) return -4;

            /* point r->buf directly into mmap — no malloc, no fread */
            r->buf = pi->base + off;
            r->owns_buf = 0;  /* mmap owns the memory */
            r->file_sz  = sz;
            r->f = NULL;

            r->hdr = (const TESS_Header *)r->buf;
            r->fml = (const TESS_Formula *)(r->buf + TESS_HEADER_SIZE);
            if (tess_header_validate(r->hdr) != 0) return -5;

            r->cell_size  = r->hdr->cell_size;
            r->cube_bytes = r->hdr->total_slots * r->cell_size;
            r->cube_data  = r->buf + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
            r->n_elems    = r->hdr->tensor_count ? r->hdr->tensor_count : r->hdr->total_slots;
            return 0;
        }
    }
    return -6;  /* not found */
}

/* ═══════ ONION lookup (f16/f32 raw contiguous tensors) ═════════════════════
 * Searches the pack index for a tensor with capo_id == 0xFFFFFFFF (ONION).
 * Returns pointer to raw data (in mmap) and sets *data_sz to byte count.
 * Returns 0 on success, negative on error.  Data is valid until tess_pack_close().
 * ───────────────────────────────────────────────────────────────────────── */
#define TPAK_ONION_SENTINEL 0xFFFFFFFFu

static inline int tess_pack_find_onion(const TESS_PackIndex *pi,
                                       const char *tensor_name,
                                       const uint8_t **data_out,
                                       uint32_t *data_sz) {
    if (pi->entries) {
        /* malloc-index mode */
        for (uint32_t i = 0; i < pi->n_entries; i++) {
            if (pi->entries[i].capo_id == TPAK_ONION_SENTINEL &&
                strcmp(pi->entries[i].name, tensor_name) == 0) {
                uint64_t off = pi->entries[i].offset;
                uint32_t sz  = pi->entries[i].size;
                if (off + sz > pi->file_sz) return -4;
                *data_out = pi->base + off;
                *data_sz  = sz;
                return 0;
            }
        }
    } else if (pi->base && pi->index_offset < pi->file_sz) {
        /* mmap-only mode: walk index for capo_id == ONION_SENTINEL */
        const uint8_t *cur = pi->base + pi->index_offset;
        const uint8_t *end = pi->base + pi->file_sz;
        uint32_t tname_len = (uint32_t)strlen(tensor_name);
        for (uint32_t i = 0; i < pi->n_capos; i++) {
            if (cur + 1 > end) return -6;
            uint8_t name_len = *cur++;
            if (cur + name_len + 16 > end) return -6;
            const uint8_t *name_ptr = cur;
            cur += name_len;
            uint32_t cid    = *(const uint32_t *)cur;
            uint64_t off    = *(const uint64_t *)(cur + 4);
            uint32_t sz     = *(const uint32_t *)(cur + 12);
            cur += 16;
            if (cid == TPAK_ONION_SENTINEL &&
                name_len == (uint8_t)tname_len &&
                memcmp(name_ptr, tensor_name, name_len) == 0) {
                if (off + sz > pi->file_sz) return -4;
                *data_out = pi->base + off;
                *data_sz  = sz;
                return 0;
            }
        }
    }
    return -6;  /* not found */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TESS Residual Region — redirect/transform table for exceptional tensors
 * ═══════════════════════════════════════════════════════════════════════════
 * Weight-tying: output.weight phantom → redirect to token_embd.weight
 * Internal llama tensors: *.scale, *.input_scale → identity (1.0f fill)
 * No data duplication — table only. Source lives in SCATTER or ONION.
 *
 * Version 2 header: hdr[6]=residual_offset, hdr[7]=residual_count
 */

/* Find a residual redirect for a tensor name.
 * Returns pointer to residual entry, or NULL if not found. */
static inline const TESS_ResidualEntry *tess_pack_find_residual(
        const TESS_PackIndex *pi, const char *tensor_name)
{
    if (!pi->residual_data || pi->residual_count == 0) return NULL;
    uint32_t tname_len = (uint32_t)strlen(tensor_name);
    const uint8_t *cur = pi->residual_data;
    const uint8_t *end = cur + (uint64_t)pi->residual_count * sizeof(TESS_ResidualEntry);

    for (uint32_t i = 0; i < pi->residual_count; i++) {
        if (cur + sizeof(TESS_ResidualEntry) > end) return NULL;
        uint8_t nl = *cur;
        if (nl == (uint8_t)tname_len && memcmp(cur + 1, tensor_name, nl) == 0) {
            return (const TESS_ResidualEntry *)cur;
        }
        cur += sizeof(TESS_ResidualEntry);
    }
    return NULL;
}

/* Apply a residual transform: read source data, transform, write to dst.
 * Returns bytes written, or 0 on error. */
static inline int tess_pack_apply_residual(
        const TESS_PackIndex *pi,
        const TESS_ResidualEntry *re,
        uint32_t n_elems,
        void *dst)
{
    switch (re->transform) {
    case TESS_TRANSFORM_ZERO:
        memset(dst, 0, (size_t)n_elems * ggml_type_size(re->dst_type));
        return (int)((size_t)n_elems * ggml_type_size(re->dst_type));

    case TESS_TRANSFORM_IDENTITY: {
        float *f = (float *)dst;
        for (uint32_t k = 0; k < n_elems; k++) f[k] = 1.0f;
        return (int)(n_elems * sizeof(float));
    }

    case TESS_TRANSFORM_NONE:
    case TESS_TRANSFORM_TYPE_CAST:
    case TESS_TRANSFORM_FP16_TO_F32: {
        /* load source tensor data from pack (ONION or SCATTER) */
        const uint8_t *onion = NULL;
        uint32_t onion_sz = 0;

        if (tess_pack_find_onion(pi, re->src, &onion, &onion_sz) == 0) {
            /* ONION source — raw f16 blob */
            if (re->transform == TESS_TRANSFORM_FP16_TO_F32) {
                const uint16_t *f16 = (const uint16_t *)onion;
                float *f32 = (float *)dst;
                for (uint32_t k = 0; k < n_elems; k++)
                    f32[k] = fp16_to_float(f16[k]);
                return (int)(n_elems * sizeof(float));
            }
            /* same type: direct copy */
            uint32_t src_cs = ggml_type_size(re->src_type);
            uint64_t src_bytes = (uint64_t)n_elems * src_cs;
            if (onion_sz >= src_bytes) {
                memcpy(dst, onion, (size_t)src_bytes);
                return (int)src_bytes;
            }
        }

        /* SCATTER source: dequantize via capo reader */
        if (re->transform == TESS_TRANSFORM_TYPE_CAST &&
            re->src_type != re->dst_type) {
            /* Q8_0 → F32 dequantization: read raw Q8_0 blocks, expand to F32 */
            uint32_t blck = (uint32_t)ggml_blck_size(re->src_type); /* Q8_0=32 */
            uint32_t cells_per_capo = TESS_TOTAL_SLOTS;
            uint64_t q8_cells = (uint64_t)n_elems / blck;  /* number of Q8_0 blocks */
            float *out = (float *)dst;

            for (uint32_t c = 0; q8_cells > 0; c++) {
                TESS_CapoReader cr;
                if (tess_pack_get_capo_mmap((TESS_PackIndex *)pi, &cr, re->src, c) != 0)
                    return 0;
                uint32_t cells = (q8_cells >= cells_per_capo)
                               ? cells_per_capo : (uint32_t)q8_cells;
                uint8_t buf[TESS_TOTAL_SLOTS * 34]; /* raw Q8_0 blocks */
                uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, buf);
                if (got != cells * 34) return 0;
                /* dequantize Q8_0: 34 bytes per block → 32 floats */
                const uint8_t *q = buf;
                for (uint32_t b = 0; b < cells; b++) {
                    uint16_t s16;
                    memcpy(&s16, q, sizeof(uint16_t));
                    float scale = fp16_to_float(s16);
                    for (int j = 0; j < 32; j++)
                        *out++ = (float)((int8_t)q[2 + j]) * scale;
                    q += 34;
                }
                q8_cells -= cells;
            }
            return (int)(n_elems * sizeof(float));
        }

        return 0;
    }

    case TESS_TRANSFORM_F32_TO_FP16:
        /* pack-time only, not used at load time */
        return 0;

    default:
        return 0;
    }
}

#endif /* GEO_TESS_CONTAINER_H */
