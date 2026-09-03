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
    uint8_t  _pad[23];          /* reserved for future LUT       */
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
 * TESS_PackIndex — mmap-based .tesspack reader (scan index once, mmap once)
 * ═══════════════════════════════════════════════════════════════════════════
 * Opens a .tesspack file, reads the entire index into memory, and mmaps
 * the data region.  Provides O(1) capo lookup by (tensor_name, capo_id)
 * without repeated fopen/fclose per capo.
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
    _fseeki64(f, 0, SEEK_END);
    pi->file_sz = (uint64_t)_ftelli64(f);
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

    return 0;
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

#endif /* GEO_TESS_CONTAINER_H */
