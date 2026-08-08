# Layered Container Architecture — Evolution Design
## DWGLS Unified Shell + Swappable Codec Modules

**Date:** 2026-08-08
**Status:** Design Proposal
**Philosophy:** MAP not COMPRESS | coordinate = address | sacred numbers

---

## Problem Statement

6 container formats with overlapping concerns:

| Container | Header | Magic | CRC | Purpose |
|-----------|--------|-------|-----|---------|
| `geo_kis_container.h` | 24B | `KIS\0KIS` (u64) | CRC64 | KIS timeline frame+block |
| `geo_kis_4d_container.h` | 48B | `KIS4` (u32) | CRC32 | 3-axis address map + resolve |
| `tesseract_container.h` | 32B | `TES4` (u32) | CRC32 | 8-octant tesseract views |
| `geo_cube_container.h` | 64B | `GCB\0` (char[4]) | CRC32 | Multi-tensor DiamondBlocks |
| `beam_entropy_container.h` | — | — | — | 8-bit BECCoord, 3-layer |
| `geo_tess_container.h` | 64B | `TESS` (u32) | CRC64 | .tess binary, stride-37 |

**Shared concerns across all 6:**
1. Identification (magic + version)
2. Geometric parameters (scale, axes, formula)
3. Payload layout (blocks, cells, tensors)
4. Integrity (CRC)
5. Address resolution (stride-37, Cayley, hyperbolic)

**Divergent concerns:**
- CRC width (32 vs 64)
- Header size (24–64B)
- Multi-tensor support (gcube yes, others no)
- Address formula embedded vs separate

---

## 1. Proposed Shell Format — `DWGLS_Shell`

**Principle:** The shell is the minimal self-describing envelope. It knows NOTHING about the codec. It only knows: identity, geometry, size, integrity.

### 1.1 Shell Header (32 bytes, packed)

```c
/* dwgls_shell.h — Universal Container Shell
 *
 * 32 bytes. Fixed. Every DWGLS file starts with this.
 * The shell identifies the container, declares geometry, and validates integrity.
 * The codec module handles everything after byte 32.
 *
 * SACRED: 20736, 1728, 144, 12
 * PRINCIPLE: MAP not COMPRESS | coordinate = address
 */

#ifndef DWGLS_SHELL_H
#define DWGLS_SHELL_H

#include <stdint.h>

#define DWGLS_SHELL_MAGIC    0x4457474Cu  /* "DWGL" — DWGLS universal */
#define DWGLS_SHELL_VERSION  1u
#define DWGLS_SHELL_SZ       32u

/* ── Codec IDs (registered, not magic-numbered) ──────────────── */
#define CODEC_NONE           0u   /* raw / passthrough */
#define CODEC_KIS_FRAME      1u   /* geo_kis_container: frame+block payload */
#define CODEC_KIS_4D         2u   /* geo_kis_4d_container: 3-axis resolve */
#define CODEC_TESSERACT      3u   /* tesseract_container: 8-octant views */
#define CODEC_GCUBE          4u   /* geo_cube_container: multi-tensor blocks */
#define CODEC_BEAM_ENTROPY   5u   /* beam_entropy_container: BECCoord 8-bit */
#define CODEC_TESS           6u   /* geo_tess_container: stride-37, .tess */
#define CODEC_KIS_CODEC_V6   7u   /* kis_codec_v6: sort+mask+codebook */
#define CODEC_DIAMOND_FIELD  8u   /* geo_diamond_field_v4: 5-path adaptive */
#define CODEC_USER_START     64u  /* user-defined codecs start here */

/* ── Integrity modes ─────────────────────────────────────────── */
#define INTEGRITY_NONE       0u
#define INTEGRITY_CRC32      1u   /* ISO 3309 / ITU-T V.42 */
#define INTEGRITY_CRC64      2u   /* ECMA-182 */
#define INTEGRITY_XXH64      3u   /* xxHash64 (fast, non-crypto) */

/* ── Shell Header (32 bytes packed) ──────────────────────────── */
#pragma pack(push, 1)
typedef struct {
    /* ── Identification (8 bytes) ────────────────────────────── */
    uint32_t magic;           /* 0x4457474C = "DWGL"               */
    uint16_t version;         /* shell format version (1)           */
    uint8_t  codec_id;        /* CODEC_* enum — what's inside       */
    uint8_t  integrity;       /* INTEGRITY_* — how to validate      */

    /* ── Geometry (8 bytes) ──────────────────────────────────── */
    uint32_t total_slots;     /* 20736 (sacred) or 0 if unknown     */
    uint32_t scale_factor;    /* fixed-point: scale × 65536         */

    /* ── Layout (8 bytes) ────────────────────────────────────── */
    uint32_t payload_size;    /* bytes after shell (codec-dependent) */
    uint32_t cell_size;       /* bytes per cell (1, 2, 4, 18, 34)   */

    /* ── Seal (8 bytes) ──────────────────────────────────────── */
    uint64_t checksum;        /* CRC/CRC64/XXH64 of shell+payload   */
} DWGLS_Shell;
#pragma pack(pop)

/* ═══════════════════════════════════════════════════════════════════════
   SHELL FUNCTIONS
   ═══════════════════════════════════════════════════════════════════════ */

/* ── shell_init ────────────────────────────────────────────────
 * Initialize shell from parameters. Zeroes reserved fields.
 */
static inline void dwgls_shell_init(DWGLS_Shell *s,
                                     uint8_t codec,
                                     uint32_t total_slots,
                                     uint32_t scale,
                                     uint32_t payload_sz,
                                     uint32_t cell_sz,
                                     uint8_t integrity_mode)
{
    s->magic        = DWGLS_SHELL_MAGIC;
    s->version      = DWGLS_SHELL_VERSION;
    s->codec_id     = codec;
    s->integrity    = integrity_mode;
    s->total_slots  = total_slots;
    s->scale_factor = scale;
    s->payload_size = payload_sz;
    s->cell_size    = cell_sz;
    s->checksum     = 0;  /* computed after payload is written */
}

/* ── shell_validate ────────────────────────────────────────────
 * Check magic + version. Returns 0=ok, -1=bad magic, -2=bad version.
 */
static inline int dwgls_shell_validate(const DWGLS_Shell *s)
{
    if (s->magic != DWGLS_SHELL_MAGIC) return -1;
    if (s->version > DWGLS_SHELL_VERSION) return -2;
    return 0;
}

/* ── shell_total_size ──────────────────────────────────────────
 * Total file size = shell(32) + payload.
 */
static inline uint32_t dwgls_shell_total_size(const DWGLS_Shell *s)
{
    return DWGLS_SHELL_SZ + s->payload_size;
}

/* ── shell_codec_name ──────────────────────────────────────────
 * Human-readable codec name.
 */
static inline const char* dwgls_shell_codec_name(uint8_t codec)
{
    switch (codec) {
        case CODEC_NONE:          return "raw";
        case CODEC_KIS_FRAME:     return "kis_frame";
        case CODEC_KIS_4D:        return "kis_4d";
        case CODEC_TESSERACT:     return "tesseract";
        case CODEC_GCUBE:         return "gcube";
        case CODEC_BEAM_ENTROPY:  return "beam_entropy";
        case CODEC_TESS:          return "tess";
        case CODEC_KIS_CODEC_V6:  return "kis_v6";
        case CODEC_DIAMOND_FIELD: return "diamond_field";
        default:                  return "user_defined";
    }
}

#endif /* DWGLS_SHELL_H */
```

### 1.2 File Layout

```
┌─────────────────────────────────────────────────────────────┐
│ Universal DWGLS File                                        │
├─────────────────────────────────────────────────────────────┤
│ [DWGLS_Shell  32B]     ← identity + geometry + integrity   │
│ [CodecPayload  var]    ← codec-specific encoded data       │
│ [CodecTail     opt]    ← codec-specific tail (CRC, LUT...) │
└─────────────────────────────────────────────────────────────┘
```

**Key design decisions:**
- Shell is exactly 32 bytes. No padding, no reserved fields. Every byte is used.
- `cell_size` encodes the quantization type (1=int8, 2=F16/BF16, 4=F32, 18=Q4_0, 34=Q8_0).
- `total_slots` is always 20736 for DWGLS files. 0 means "codec decides."
- `checksum` covers the ENTIRE file (shell + payload). The codec's own CRC becomes redundant — the shell owns integrity.
- `scale_factor` is in the shell because it's a geometric primitive (KIS scale), not a codec detail.

### 1.3 Why Not Variable-Length Header?

The tess-format-spec.md designed a 64-byte TESS_Header with optional sections (LUT, OctantMap, ScaleTable, Metadata). This was the right instinct but wrong mechanism:
- Variable-length headers break mmap zero-copy (can't compute payload offset without parsing)
- Optional sections create N² format combinations (codec × option set)
- The codec can store its own metadata as part of CodecPayload

**The shell is fixed. The codec payload is variable. The codec knows its own tail.**

---

## 2. Codec Interface Design

**Principle:** Every codec implements the same 6 functions. The shell calls them. Codecs don't call each other.

### 2.1 Codec Vtable

```c
/* dwgls_codec.h — Swappable Codec Interface
 *
 * Every codec registers a DWGLS_Codec vtable.
 * The shell dispatches through the vtable based on codec_id.
 * Codecs are stateless: encode/decode are pure functions.
 */

#ifndef DWGLS_CODEC_H
#define DWGLS_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "dwgls_shell.h"

/* ── Codec Context (optional, for codecs that need state) ────── */
typedef struct {
    uint32_t total_slots;     /* 20736 or codec-specific */
    uint32_t scale_factor;    /* fixed-point scale */
    uint32_t x_slots;         /* axis decomposition (0 = use default) */
    uint32_t y_slots;
    uint32_t z_slots;
    uint32_t user_data[4];    /* codec-specific parameters */
} DWGLS_CodecCtx;

/* ── Codec Info ──────────────────────────────────────────────── */
typedef struct {
    const char *name;         /* "tess", "gcube", etc. */
    uint8_t     codec_id;     /* CODEC_* constant */
    uint8_t     min_version;  /* minimum shell version needed */
    uint32_t    flags;        /* capability flags (see below) */
} DWGLS_CodecInfo;

/* ── Codec Flags ─────────────────────────────────────────────── */
#define CODEC_FLAG_MULTI_TENSOR   (1u << 0)  /* supports multiple tensors */
#define CODEC_FLAG_MMAP_FRIENDLY  (1u << 1)  /* payload is mmap-able */
#define CODEC_FLAG_SEQUENTIAL     (1u << 2)  /* optimized for seq access */
#define CODEC_FLAG_RANDOM_ACCESS  (1u << 3)  /* optimized for random access */
#define CODEC_FLAG_COMPRESSED     (1u << 4)  /* payload may be smaller */
#define CODEC_FLAG_DERIVED_VIEWS  (1u << 5)  /* generates views at runtime */

/* ── Codec Vtable ────────────────────────────────────────────── */
typedef struct {
    /* ── Metadata ──────────────────────────────────────────── */
    DWGLS_CodecInfo (*info)(void);

    /* ── Encode: raw weights → codec payload ──────────────────
     *   src:      raw weight data
     *   n_elems:  number of elements
     *   ctx:      codec context (scale, axes, user_data)
     *   dst:      output buffer (caller-allocated)
     *   dst_cap:  capacity of dst in bytes
     *   Returns:  bytes written, or negative on error
     */
    int32_t (*encode)(const void *src, uint32_t n_elems,
                      const DWGLS_CodecCtx *ctx,
                      void *dst, uint32_t dst_cap);

    /* ── Decode: codec payload → raw weights ──────────────────
     *   src:      codec payload (starts after DWGLS_Shell)
     *   src_len:  payload bytes
     *   ctx:      codec context
     *   dst:      output buffer (caller-allocated)
     *   dst_cap:  capacity of dst in bytes
     *   Returns:  bytes written (raw), or negative on error
     */
    int32_t (*decode)(const void *src, uint32_t src_len,
                      const DWGLS_CodecCtx *ctx,
                      void *dst, uint32_t dst_cap);

    /* ── Size: compute encoded size without encoding ──────────
     *   n_elems:  number of elements
     *   ctx:      codec context
     *   Returns:  payload size in bytes, 0 if unknown
     */
    uint32_t (*payload_size)(uint32_t n_elems,
                             const DWGLS_CodecCtx *ctx);

    /* ── Verify: check payload integrity ──────────────────────
     *   src:      codec payload
     *   src_len:  payload bytes
     *   Returns:  0=ok, negative=corrupt
     */
    int (*verify)(const void *src, uint32_t src_len);

    /* ── Resolve: address mapping (for codecs with geometry) ──
     *   slot:     input slot (0..20735)
     *   ctx:      codec context (scale, formula params)
     *   Returns:  resolved address in payload space
     */
    uint32_t (*resolve)(uint32_t slot, const DWGLS_CodecCtx *ctx);

} DWGLS_CodecVtable;
```

### 2.2 Codec Registration

```c
/* ── Built-in codec registry (compile-time, no malloc) ───────── */

/* Each codec defines a static const vtable */
extern const DWGLS_CodecVtable DWGLS_CODEC_RAW;
extern const DWGLS_CodecVtable DWGLS_CODEC_TESS;
extern const DWGLS_CodecVtable DWGLS_CODEC_GCUBE;
extern const DWGLS_CodecVtable DWGLS_CODEC_KIS_FRAME;
extern const DWGLS_CodecVtable DWGLS_CODEC_KIS_4D;
extern const DWGLS_CodecVtable DWGLS_CODEC_BEAM_ENTROPY;
extern const DWGLS_CodecVtable DWGLS_CODEC_KIS_V6;
extern const DWGLS_CodecVtable DWGLS_CODEC_DIAMOND_FIELD;

/* ── Lookup by codec_id ──────────────────────────────────────── */
static inline const DWGLS_CodecVtable* dwgls_codec_find(uint8_t codec_id)
{
    /* Static array — no malloc, O(N) scan but N ≤ 16 */
    static const DWGLS_CodecVtable *registry[] = {
        &DWGLS_CODEC_RAW,           /* 0 */
        &DWGLS_CODEC_KIS_FRAME,     /* 1 */
        &DWGLS_CODEC_KIS_4D,        /* 2 */
        &DWGLS_CODEC_TESSERACT,     /* 3 */
        &DWGLS_CODEC_GCUBE,         /* 4 */
        &DWGLS_CODEC_BEAM_ENTROPY,  /* 5 */
        &DWGLS_CODEC_TESS,          /* 6 */
        &DWGLS_CODEC_KIS_V6,        /* 7 */
        &DWGLS_CODEC_DIAMOND_FIELD, /* 8 */
    };
    if (codec_id < sizeof(registry)/sizeof(registry[0]))
        return registry[codec_id];
    return NULL;  /* unknown codec — user must register externally */
}
```

### 2.3 Codec Example: TESS

```c
/* codec_tess.h — TESS codec (stride-37, 8-octant, .tess format)
 *
 * Wraps the existing geo_tess_container.h logic as a DWGLS codec.
 * This is the PRIMARY codec — the one .tess files use.
 */

#include "dwgls_codec.h"
#include "geo_tess_container.h"  /* existing TESS_Header, resolve functions */

static DWGLS_CodecInfo tess_info(void) {
    return (DWGLS_CodecInfo){
        .name = "tess",
        .codec_id = CODEC_TESS,
        .min_version = 1,
        .flags = CODEC_FLAG_MMAP_FRIENDLY | CODEC_FLAG_RANDOM_ACCESS
               | CODEC_FLAG_DERIVED_VIEWS,
    };
}

static int32_t tess_encode(const void *src, uint32_t n_elems,
                            const DWGLS_CodecCtx *ctx,
                            void *dst, uint32_t dst_cap)
{
    /* 1. Scatter raw data through stride-37 into 20736 cells */
    /* 2. Write TESS_Header into first 64B of dst */
    /* 3. Write CubeData (20736 × cell_size) */
    /* 4. Return bytes written */
    (void)src; (void)n_elems; (void)ctx; (void)dst; (void)dst_cap;
    return -1; /* placeholder */
}

static int32_t tess_decode(const void *src, uint32_t src_len,
                            const DWGLS_CodecCtx *ctx,
                            void *dst, uint32_t dst_cap)
{
    /* 1. Read TESS_Header from src */
    /* 2. Scatter-gather: stride-37 inverse (×16813 mod 20736) */
    /* 3. Write de-scattered data to dst */
    (void)src; (void)src_len; (void)ctx; (void)dst; (void)dst_cap;
    return -1; /* placeholder */
}

static uint32_t tess_payload_size(uint32_t n_elems,
                                   const DWGLS_CodecCtx *ctx)
{
    /* TESS always stores 20736 cells */
    uint32_t cell_size = ctx->user_data[0]; /* or default from ctx */
    return 20736 * cell_size;
}

static int tess_verify(const void *src, uint32_t src_len)
{
    /* Check TESS_Header magic + CRC64 */
    (void)src; (void)src_len;
    return 0;
}

static uint32_t tess_resolve(uint32_t slot, const DWGLS_CodecCtx *ctx)
{
    /* stride-37 scatter: slot × 16813 mod 20736 */
    return (slot * 16813u) % 20736u;
}

const DWGLS_CodecVtable DWGLS_CODEC_TESS = {
    .info         = tess_info,
    .encode       = tess_encode,
    .decode       = tess_decode,
    .payload_size = tess_payload_size,
    .verify       = tess_verify,
    .resolve      = tess_resolve,
};
```

---

## 3. Migration Strategy

### 3.1 Backward Compatibility: Adapter Codecs

Each existing container becomes an adapter codec. The adapter reads the OLD format and wraps it in a DWGLS_Shell for interop.

```
OLD FORMAT                    ADAPTER CODEC               DWGLS_SHELL
┌──────────────┐              ┌──────────────────┐        ┌────────────┐
│ KIS_Header 24B│ ──adapt──→ │ codec_kis_frame  │ ──→    │ DWGL 32B   │
│ frame slots   │            │ (reads old layout)│        │ payload    │
│ blocks        │            │                   │        │            │
│ CRC64         │            │                   │        │            │
└──────────────┘              └──────────────────┘        └────────────┘
```

**Migration path:**
```
Phase 1: New files written with DWGLS_Shell + codec
Phase 2: Old files read via adapter (transparent to caller)
Phase 3: Old files optionally rewritten to new format
Phase 4: Adapter code removed (old formats deprecated)
```

### 3.2 File Extension Mapping

| Old Extension | New Extension | Adapter Needed? |
|---------------|---------------|-----------------|
| `.kis` | `.dwgls` | Yes (CODEC_KIS_FRAME) |
| `.kis4` | `.dwgls` | Yes (CODEC_KIS_4D) |
| `.tes4` | `.dwgls` | Yes (CODEC_TESSERACT) |
| `.gcube` | `.dwgls` | Yes (CODEC_GCUBE) |
| `.tess` | `.dwgls` | **NO** — .tess IS the native format |
| `.bec` | `.dwgls` | Yes (CODEC_BEAM_ENTROPY) |

**Key insight:** `.tess` already has the 64-byte TESS_Header. The migration for .tess is:
1. Read existing 64-byte TESS_Header
2. Construct DWGLS_Shell (32 bytes) from TESS_Header fields
3. Prepend shell to existing payload
4. Rewrite checksum

This is a TRIVIAL transformation — no data re-encoding needed.

### 3.3 Auto-Detection on Read

```c
/* dwgls_open.c — Universal open with auto-detection */

typedef struct {
    DWGLS_Shell           shell;
    const DWGLS_CodecVtable *codec;
    const uint8_t         *payload;  /* pointer into mmap or fread buffer */
    uint32_t               payload_len;
} DWGLS_File;

static inline int dwgls_open(DWGLS_File *f, const void *data, uint32_t len)
{
    if (len < DWGLS_SHELL_SZ) return -1;

    /* Try DWGLS shell first */
    const DWGLS_Shell *shell = (const DWGLS_Shell *)data;
    if (dwgls_shell_validate(shell) == 0) {
        f->shell = *shell;
        f->codec = dwgls_codec_find(shell->codec_id);
        f->payload = (const uint8_t *)data + DWGLS_SHELL_SZ;
        f->payload_len = shell->payload_size;
        return f->codec ? 0 : -3;  /* unknown codec */
    }

    /* Try legacy formats (magic sniffing) */
    const uint32_t *u32 = (const uint32_t *)data;
    switch (u32[0]) {
        case 0x54455353u:  /* "TESS" → geo_tess_container */
            /* Convert TESS_Header → DWGLS_Shell */
            f->shell.magic        = DWGLS_SHELL_MAGIC;
            f->shell.version      = 1;
            f->shell.codec_id     = CODEC_TESS;
            f->shell.integrity    = INTEGRITY_CRC64;
            f->shell.total_slots  = 20736;
            /* ... fill from TESS_Header ... */
            f->codec = &DWGLS_CODEC_TESS;
            f->payload = (const uint8_t *)data + 64;  /* skip TESS_Header */
            f->payload_len = len - 64;
            return 0;

        case 0x54455334u:  /* "TES4" → tesseract_container */
            f->shell.codec_id = CODEC_TESSERACT;
            f->codec = &DWGLS_CODEC_TESSERACT;
            /* ... */
            return 0;

        /* ... more legacy formats ... */
    }

    return -2;  /* unrecognized format */
}
```

---

## 4. Synergy Matrix — Which Codecs Benefit from Unification

| Codec | Unification Benefit | Why |
|-------|---------------------|-----|
| **TESS** | ★★★★★ PRIMARY | Already near-native; .tess = canonical DWGLS format |
| **GCube** | ★★★★ HIGH | Multi-tensor needs standard index; shell provides it |
| **KIS Frame** | ★★★ MEDIUM | Frame+block payload is simple; mainly needs CRC unification |
| **KIS 4D** | ★★★ MEDIUM | 3-axis resolve is codec logic; shell handles integrity |
| **Beam Entropy** | ★★ LOW | Headerless by design; shell adds identification only |
| **KIS Codec V6** | ★★★★ HIGH | Sort+mask+codebook needs standard container; enables GGUF compat |
| **Diamond Field** | ★★★ MEDIUM | 5-path adaptive is codec logic; shell adds identification |
| **Tesseract (TES4)** | ★★★ MEDIUM | Octant views are runtime-derived; shell standardizes header |

### 4.1 Specific Synergies

**TESS + KIS_4D Unification:**
- TESS already stores 1 cube, derives 8 views at runtime
- KIS_4D stores 3-axis address map
- In unified format: TESS codec stores cube + axis params; KIS_4D is an ACCESS PATTERN, not a codec
- **Resolution:** KIS_4D becomes a resolver function inside CODEC_TESS, not a separate codec

**GCube + TESS Unification:**
- GCube stores multi-tensor index + DiamondBlocks
- TESS stores single cube + stride-37 scatter
- In unified format: GCube's TensorTable becomes an optional section in TESS payload
- **Resolution:** CODEC_TESS absorbs CODEC_GCUBE for multi-tensor .tess files

**KIS Frame + Beam Entropy:**
- KIS Frame: frame slots (16B each) + DiamondBlocks
- Beam Entropy: 8-bit BECCoord, 3-layer separation
- Both are "codec strategies" for the same 20736 address space
- **Resolution:** Both become CODEC variants, sharing the same shell

---

## 5. Phased Roadmap

### Phase 1: Shell Foundation (Week 1)
**Goal:** `dwgls_shell.h` compiles, tests pass, .tess files readable.

| Task | Effort | Exit Criteria |
|------|--------|---------------|
| Write `dwgls_shell.h` | 2h | Compiles with `gcc -fsyntax-only` |
| Write `dwgls_codec.h` (vtable only) | 2h | Interface compiles |
| Write `test_shell.c` (init, validate, roundtrip) | 3h | 10/10 PASS |
| Write adapter: TESS_Header → DWGLS_Shell | 2h | `.tess` file opens as DWGLS |
| Update Makefile: `make shell` target | 1h | `make shell` builds + runs |
| Add to existing test: `make test` includes shell tests | 0.5h | 22+ PASS |

**Deliverable:** `dwgls_shell.h` + `dwgls_codec.h` + `test_shell.c`
**Files created:** `core/dwgls_shell.h`, `core/dwgls_codec.h`, `tests/test_shell.c`

### Phase 2: First Codec — TESS (Week 2)
**Goal:** TESS codec registered, .tess files roundtrip through shell.

| Task | Effort | Exit Criteria |
|------|--------|---------------|
| Write `codec_tess.h` wrapping geo_tess_container | 3h | Compiles |
| Register DWGLS_CODEC_TESS in vtable | 1h | codec_find(CODEC_TESS) works |
| Test: read .tess → shell → codec → decode → verify | 4h | 15/15 PASS |
| Test: create DWGLS file from raw data → .dwgls | 3h | Roundtrip lossless |
| Benchmark: shell overhead vs raw geo_tess_container | 2h | < 1% overhead |

**Deliverable:** `codec_tess.h` + `tests/test_codec_tess.c`
**Files created:** `core/codec_tess.h`, `tests/test_codec_tess.c`

### Phase 3: Adapter Layer (Week 3)
**Goal:** All 5 legacy formats readable through shell adapters.

| Task | Effort | Exit Criteria |
|------|--------|---------------|
| Adapter: geo_kis_container → DWGLS_Shell | 2h | `.kis` files open |
| Adapter: geo_kis_4d_container → DWGLS_Shell | 2h | `.kis4` files open |
| Adapter: tesseract_container → DWGLS_Shell | 2h | `.tes4` files open |
| Adapter: geo_cube_container → DWGLS_Shell | 3h | `.gcube` files open |
| Adapter: beam_entropy_container → DWGLS_Shell | 2h | `.bec` files open |
| Auto-detect test: feed mixed formats, verify detection | 3h | 20/20 PASS |

**Deliverable:** `dwgls_adapters.h` + `tests/test_adapters.c`
**Files created:** `core/dwgls_adapters.h`, `tests/test_adapters.c`

### Phase 4: Second Codec — GCube (Week 4)
**Goal:** GCube multi-tensor support in unified shell.

| Task | Effort | Exit Criteria |
|------|--------|---------------|
| Write `codec_gcube.h` wrapping geo_cube_container | 3h | Compiles |
| Multi-tensor: TensorTable as codec-level concept | 4h | 5-tensor roundtrip |
| Test: .gcube → shell → codec → decode each tensor | 4h | 20/20 PASS |
| Integration: geo_tensor_hub reads DWGLS_Shell files | 3h | Hub open/load works |

**Deliverable:** `codec_gcube.h` + `tests/test_codec_gcube.c`

### Phase 5: Production Hardening (Week 5-6)
**Goal:** End-to-end pipeline, benchmarks, documentation.

| Task | Effort | Exit Criteria |
|------|--------|---------------|
| mmap zero-copy path for DWGLS files | 4h | Pointer into mmap works |
| Integration with geo_rail_hub_pull | 6h | Full pipeline: DWGLS → llama.cpp |
| Benchmark: shell vs raw format (encode/decode/verify) | 4h | Report published |
| Update tess-format-spec.md to reference DWGLS shell | 2h | Spec complete |
| Deprecation notices on old format headers | 1h | `#warning` in old headers |

---

## 6. Design Rationale

### Why 32-byte shell, not 64-byte?
- The TESS_Header is 64 bytes but 32 bytes are "tail" (checksum + formula_id)
- DWGLS_Shell puts checksum IN the shell (not at file end)
- Formula params belong to the CODEC, not the shell
- 32 bytes = 2 cache lines on most CPUs = optimal for mmap

### Why not just extend TESS_Header?
- TESS_Header has `uint32_t checksum` (CRC32) — shell has `uint64_t checksum` (CRC64)
- TESS_Header has `uint32_t reserved[2]` — shell uses every byte
- TESS_Header has no `codec_id` — shell is codec-agnostic
- TESS_Header is .tess-specific — shell is format-universal

### Why static vtable, not dynamic registration?
- DWGLS is header-only C. No malloc, no linker tricks.
- Static array of 8-16 vtables = 64-128 bytes. Trivial.
- Compile-time known set = compiler can optimize away dead branches.
- User codecs can use `#define CODEC_USER_START 64u` for plugins.

### Why single checksum, not per-section?
- The shell's checksum covers everything. Period.
- Per-section checksums (like tess-format-spec's per-tensor CRC) are REDUNDANT.
- One CRC64 over 1MB = 1ms. Two CRC64 over 1MB = 2ms. No benefit.
- If section integrity is needed, the codec stores its own checksum in CodecPayload.

---

## 7. Sacred Geometry Alignment

The shell respects all sacred constants:

| Constant | Where in Shell |
|----------|---------------|
| 20736 | `total_slots` (always 20736 for DWGLS) |
| 1728 | Codec resolves internally (shell agnostic) |
| 144 | Codec resolves internally (shell agnostic) |
| 12 | Codec resolves internally (shell agnostic) |
| 128 × 162 | `cell_size` encodes the block format |

The shell is GEOMETRY-AWARE but not GEOMETRY-DEPENDENT. It declares
`total_slots = 20736` so codecs know the address space, but the shell
itself performs no geometric computation.

**This is the correct layering:** Shell = envelope. Codec = geometry.
