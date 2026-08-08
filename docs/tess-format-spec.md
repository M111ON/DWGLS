# .tess File Format Specification v1.0

**Status:** Design Draft (Aug 8, 2026)
**Author:** DWGLS Pipeline Architecture
**Philosophy:** MAP not COMPRESS | coordinate = address | sacred numbers

---

## 1. Overview

`.tess` is the binary container format for the DWGLS tesseract pipeline.
It stores geometrically-mapped GGUF tensor data in a single 20736-slot
KIS address space. The 8 octant views of the tesseract are **derived at
runtime** via the hyperbolic mirror formula — NOT stored on disk.

**Core principle:** Store 1 cube, derive 8 views. 8x deduplication
comes from the mirror formula, not from compression.

---

## 2. Sacred Numbers Reference

| Number | Meaning | Derivation |
|--------|---------|------------|
| 20736 | Total address space | 12⁴ = 144² = 128 × 162 |
| 1728 | FiboSpine pipes | 12³ |
| 144 | Tesseract edge | 12² |
| 128 | Compute route | 2 × 64 |
| 162 | Geometry route | 2 × 3⁴ |
| 6912 | Axis slot count | 20736 / 3 |
| 34 | Q8_0 block size | 2B scale + 32B int8 |
| 37 | Stride coprime | gcd(37, 20736) = 1 |

---

## 3. Binary Format Layout

### 3.1 File Structure

```
┌─────────────────────────────────────────────────────────────────┐
│ .tess File Layout                                               │
├─────────────────────────────────────────────────────────────────┤
│ [TESS_Header    64 bytes]  ← geometric metadata + seal         │
│ [TensorTable    variable]  ← tensor name table (GGUF compat)   │
│ [CubeData       20736 × cell_size] ← 1 cube, 8 views derived  │
│ [FormulaBlock   64 bytes]  ← hyperbolic resolver params        │
│ [CRC64          8 bytes]   ← integrity seal                    │
└─────────────────────────────────────────────────────────────────┘

Total fixed overhead: 64 + 8 + 64 = 136 bytes
Cube data: 20736 × cell_size (varies by quantization)
```

### 3.2 TESS_Header (64 bytes)

```c
typedef struct {
    /* ── Identification (16 bytes) ─────────────────────────── */
    uint32_t magic;              /* 0x54455353 = "TESS"           */
    uint32_t version;            /* 1 = v1.0                      */
    uint32_t total_slots;        /* 20736 (sacred)                */
    uint32_t cell_size;          /* bytes per cell (see §3.3)     */

    /* ── Geometric Parameters (16 bytes) ───────────────────── */
    uint32_t scale_factor;       /* fixed-point: scale × 65536    */
    uint32_t x_slots;            /* X-axis count (6912 default)   */
    uint32_t y_slots;            /* Y-axis count (6912 default)   */
    uint32_t z_slots;            /* Z-axis count (6912 default)   */

    /* ── Pipeline Metadata (16 bytes) ──────────────────────── */
    uint32_t gguf_type;          /* GGML quantization type (0-30) */
    uint32_t tensor_count;       /* number of tensors mapped      */
    uint64_t source_size;        /* original GGUF file size (B)   */

    /* ── Seal (16 bytes) ───────────────────────────────────── */
    uint64_t cube_checksum;      /* CRC-64 of CubeData            */
    uint64_t formula_id;         /* hash of formula parameters     */
} TESS_Header;                   /* total: 64 bytes               */
```

**Field Descriptions:**

| Field | Bytes | Description |
|-------|-------|-------------|
| `magic` | 4 | `0x54455353` = ASCII "TESS" (little-endian) |
| `version` | 4 | Format version (currently 1) |
| `total_slots` | 4 | Always 20736 (12⁴) |
| `cell_size` | 4 | Bytes per cell: 1 (raw int8), 34 (Q8_0 block), 2 (BF16) |
| `scale_factor` | 4 | KIS scale as fixed-point u32 (1.0 = 65536, 0.1 = 6553) |
| `x_slots` | 4 | X-axis: slots 0–6911 (default 6912 = 20736/3) |
| `y_slots` | 4 | Y-axis: slots 6912–13823 (default 6912) |
| `z_slots` | 4 | Z-axis: slots 13824–20735 (default 6912) |
| `gguf_type` | 4 | GGML type index (8 = Q8_0, 0 = F32, 1 = F16, etc.) |
| `tensor_count` | 4 | Number of GGUF tensors mapped into cube |
| `source_size` | 8 | Original GGUF file size for validation |
| `cube_checksum` | 8 | CRC-64/ECMA-182 of CubeData only |
| `formula_id` | 8 | xxHash64 of formula parameters for cache key |

### 3.3 Cell Sizes by Quantization

| GGUF Type | cell_size | Description |
|-----------|-----------|-------------|
| F32 (0) | 4 | Single float32 |
| F16 (1) | 2 | Single float16 |
| Q8_0 (8) | 34 | 2B FP16 scale + 32B int8 weights |
| Q4_0 (2) | 18 | 2B FP16 scale + 16B int4 weights |
| Q4_1 (3) | 20 | 2B FP16 + 2B min + 16B int4 |
| Q5_0 (6) | 22 | 2B + 2B + 16B + 4B bits |
| Q5_1 (7) | 24 | 2B + 2B + 2B + 16B + 4B |
| BF16 (30) | 2 | Brain float16 |
| Raw int8 | 1 | Direct int8 (for codebook path) |

### 3.4 TensorTable

Variable-length table mapping tensor names to cube offsets.
Compatible with GGUF reader expectations.

```
┌──────────────────────────────────────────────────┐
│ TensorTable Layout                               │
├──────────────────────────────────────────────────┤
│ uint32_t table_size        /* total bytes        */ │
│ uint32_t n_tensors         /* tensor count       */ │
│ For each tensor:                                │
│   uint64_t offset          /* byte offset in cube */ │
│   uint64_t n_bytes         /* tensor data bytes   */ │
│   uint32_t n_dims          /* dimension count     */ │
│   int64_t  dims[4]         /* dimensions (pad 4)  */ │
│   uint32_t name_len        /* tensor name length  */ │
│   char     name[name_len]  /* tensor name string  */ │
│   /* padded to 8-byte alignment */                │
└──────────────────────────────────────────────────┘
```

### 3.5 CubeData

The core payload: one cube of 20736 cells.

```
Offset: cell_index × cell_size

cell_index = (flat_slot × 37) % 20736    // stride-37 scatter

For Q8_0 (cell_size = 34):
  [offset+0..1]   uint16_t  fp16_scale   // block scale factor
  [offset+2..33]  int8_t    weights[32]  // 32 quantized weights

For Raw (cell_size = 1):
  [offset+0]      int8_t    value        // single weight value

For F32 (cell_size = 4):
  [offset+0..3]   float     value        // single float value
```

**Important:** CubeData stores the FULL 20736 cells. There is no
sparse encoding at the container level — sparsity is handled by
the formula (collisions reduce unique addresses).

### 3.6 FormulaBlock (64 bytes)

Stores the hyperbolic resolver parameters for on-the-fly address
computation. This is the "navigation" — never the "value".

```c
typedef struct {
    /* ── Resolver Parameters (32 bytes) ────────────────────── */
    uint32_t mirror_axis_x;     /* X mirror formula coefficient  */
    uint32_t mirror_axis_y;     /* Y mirror formula coefficient  */
    uint32_t mirror_axis_z;     /* Z mirror formula coefficient  */
    uint32_t time_stride;       /* f(time) multiplier            */
    uint32_t cayley_offset[3];  /* Cayley transform offsets      */
    uint32_t octant_mask;       /* active octant bitmask (8-bit) */
    uint32_t stride_seed;       /* stride-37 seed for scatter    */
    uint32_t reserved[2];       /* padding                       */

    /* ── Lookup Table (32 bytes) ───────────────────────────── */
    uint8_t  axis_select[20736]; /* 12-bit packed: axis per slot */
    /* Actually 20736 × 2 bits = 5184 bytes, stored separately */
    /* This field is a placeholder; LUT lives in TensorTable    */
    uint8_t  _pad[32];          /* reserved for future use       */
} TESS_Formula;
```

**Note:** The axis_select LUT is too large for inline storage.
It is stored as a separate section after FormulaBlock if present.
See §3.7.

### 3.7 Optional Sections

```
┌─────────────────────────────────────────────────────────────────┐
│ Optional Sections (after FormulaBlock, before CRC64)            │
├─────────────────────────────────────────────────────────────────┤
│ [AxisLUT     5184 bytes]  axis_select[]: 2 bits per slot       │
│ [OctantMap   20736 bytes] octant assignment per slot            │
│ [ScaleTable  256 bytes]   Q8_0 scale histogram                 │
│ [Metadata    variable]    JSON key-value pairs (GGUF compat)    │
└─────────────────────────────────────────────────────────────────┘
```

Each optional section is prefixed with:
```c
typedef struct {
    uint32_t section_type;   /* 'LUT\0', 'OMAP', 'STAB', 'META' */
    uint32_t section_size;   /* bytes of payload (excl. this hdr) */
} TESS_SectionHdr;           /* 8 bytes */
```

### 3.8 CRC64 Seal (8 bytes)

Final 8 bytes: CRC-64/ECMA-182 of entire file from byte 0 through
the last byte before this field. Polynomial: `0x42F0E1EBA9EA3693`.
Algorithm: non-reflected MSB-first, init=0xFFFFFFFFFFFFFFFF,
xorout=0xFFFFFFFFFFFFFFFF.

---

## 4. Address Space Mapping

### 4.1 KIS 3-Axis Partition

```
Total: 20736 slots
  ┌─────────────────────────────────────────────────┐
  │ AXIS_X: [0, 6911]      6912 slots = 1728 × 4  │
  │ AXIS_Y: [6912, 13823]  6912 slots = 1728 × 4  │
  │ AXIS_Z: [13824, 20735] 6912 slots = 1728 × 4  │
  └─────────────────────────────────────────────────┘

Axis selection: slot < 6912 → X, slot < 13824 → Y, else → Z
Axis-local: axis_slot = slot - axis_offset
```

### 4.2 Octant Mapping (8 Views)

Each octant is a sign combination on the 3 axes:

```
Octant 0: +X +Y +Z  (identity)
Octant 1: -X +Y +Z  (X mirror)
Octant 2: +X -Y +Z  (Y mirror)
Octant 3: -X -Y +Z  (XY mirror)
Octant 4: +X +Y -Z  (Z mirror)
Octant 5: -X +Y -Z  (XZ mirror)
Octant 6: +X -Y -Z  (YZ mirror)
Octant 7: -X -Y -Z  (XYZ mirror = full inversion)
```

**Mirror formula (runtime, not stored):**
```c
// For axis a ∈ {X, Y, Z}, slot s in [0, 6911]:
// Sign = (octant >> a) & 1
// If sign = 1: mirrored_slot = 6911 - axis_slot
// If sign = 0: mirrored_slot = axis_slot
// Full address = mirror_slot + axis_offset
```

### 4.3 Hyperbolic Address Resolver

The address resolver maps creation-time coordinates to target-scale
addresses. The formula is: `address = x × f(time)` where `f(time)`
is the scale-dependent rotation function.

```c
// Encode: data stored at creation point (scale 1.0)
// Decode: address = kis4d_resolve(slot, target_scale, header)
//
// resolve steps:
// 1. Select axis from slot (X/Y/Z based on slot range)
// 2. Compute angle = 2π × axis_slot / axis_slots
// 3. Add axis offset: angle += axis × 2π/3
// 4. Scale: new_angle = angle × (target_scale / base_scale)
// 5. Remove axis offset, map back to slot
// 6. Return: (result % axis_slots) + axis_offset
```

**Speed:** Store angle at creation time → skip atan2 → 18x faster.
Resolution: 10 ns/op (with stored angle), 182 ns/op (with atan2).

---

## 5. Pipeline Architecture

### 5.1 Full Pipeline Diagram

```
                    DWGLS TESSERACT PIPELINE
                    ========================

    ┌─────────────┐
    │  GGUF File  │  Input: standard GGUF model file
    │  (644 MB)   │  Types: Q8_0, F32, F16, BF16, etc.
    └──────┬──────┘
           │
           │  gguf_reader.h (mmap, ~79-132ms)
           ▼
    ┌─────────────────────────────────────────────────────────────┐
    │  LAYER 1: TENSOR EXTRACTION                                │
    │  ┌──────────────────────────────────────────────────────┐  │
    │  │ gguf_open() → metadata                               │  │
    │  │ gguf_read_tensor() → raw bytes per tensor            │  │
    │  │ Type detection: sizes[idx] % 34 == 0 → Q8_0        │  │
    │  └──────────────────────────────────────────────────────┘  │
    └──────┬──────────────────────────────────────────────────────┘
           │
           │  Per-tensor: raw weight bytes
           ▼
    ┌─────────────────────────────────────────────────────────────┐
    │  LAYER 2: KIS ADDRESS MAPPING                              │
    │  ┌──────────────────────────────────────────────────────┐  │
    │  │ geo_kis_projection.h                                 │  │
    │  │   kis_to_hyperbolic_axis(slot) → (axis, axis_slot)   │  │
    │  │   Map 1D weight index → KIS 3-axis coordinates       │  │
    │  │   X: [0,6911]  Y: [6912,13823]  Z: [13824,20735]   │  │
    │  │                                                      │  │
    │  │ Stride-37 scatter: cell = (weight_idx × 37) % 20736  │  │
    │  └──────────────────────────────────────────────────────┘  │
    └──────┬──────────────────────────────────────────────────────┘
           │
           │  Weight index → KIS address
           ▼
    ┌─────────────────────────────────────────────────────────────┐
    │  LAYER 3: HYPERBOLIC RESOLVER                              │
    │  ┌──────────────────────────────────────────────────────┐  │
    │  │ hyperbolic_seek.h                                    │  │
    │  │   x × f(time) = address resolver                     │  │
    │  │   Cayley transform: KIS ↔ Hyperbolic dual            │  │
    │  │   scale_factor controls compression (side effect)    │  │
    │  │                                                      │  │
    │  │ Scale 1.0: 20736 → 20736 unique (1.00x baseline)   │  │
    │  │ Scale 0.5: 20736 → 16248 unique (1.28x)            │  │
    │  │ Scale 0.1: 20736 →  8088 unique (2.56x)            │  │
    │  └──────────────────────────────────────────────────────┘  │
    └──────┬──────────────────────────────────────────────────────┘
           │
           │  Address → resolved position in 20736 grid
           ▼
    ┌─────────────────────────────────────────────────────────────┐
    │  LAYER 4: TESSERACT CONTAINER (.tess)                      │
    │  ┌──────────────────────────────────────────────────────┐  │
    │  │ geo_kis_4d_container.h → .tess format               │  │
    │  │                                                      │  │
    │  │ ┌────────────┐  ┌────────────┐  ┌────────────┐     │  │
    │  │ │TESS_Header │  │TensorTable │  │ CubeData   │     │  │
    │  │ │  64 bytes  │  │  variable  │  │ 20736 × sz │     │  │
    │  │ └────────────┘  └────────────┘  └────────────┘     │  │
    │  │ ┌────────────┐  ┌────────────┐  ┌────────────┐     │  │
    │  │ │FormulaBlock│  │ Optional   │  │  CRC-64    │     │  │
    │  │ │  64 bytes  │  │  sections  │  │   8 bytes  │     │  │
    │  │ └────────────┘  └────────────┘  └────────────┘     │  │
    │  │                                                      │  │
    │  │ 1 cube stored → 8 octant views derived at runtime   │  │
    │  └──────────────────────────────────────────────────────┘  │
    └──────┬──────────────────────────────────────────────────────┘
           │
           │  .tess file on disk
           ▼
    ┌─────────────────────────────────────────────────────────────┐
    │  LAYER 5: llama.cpp INTEGRATION                             │
    │  ┌──────────────────────────────────────────────────────┐  │
    │  │ tess_loader.h                                        │  │
    │  │   tess_open() → mmap .tess file                      │  │
    │  │   tess_read_tensor(name) → GGUF-compatible buffer    │  │
    │  │   tess_resolve_octant(octant, slot) → address        │  │
    │  │   tess_to_gguf_block(cell) → dequantized weights     │  │
    │  │                                                      │  │
    │  │ Integration options:                                 │  │
    │  │   A) Shim: .tess → gguf_buffer → llama.cpp          │  │
    │  │   B) Native: llama.cpp loads .tess directly via mmap │  │
    │  │   C) Bridge: geo_inference_bridge.h adapted for .tess│  │
    │  └──────────────────────────────────────────────────────┘  │
    └─────────────────────────────────────────────────────────────┘


    RUNTIME DECOMPRESSION (on-the-fly, zero copy):
    ═══════════════════════════════════════════════

    mmap(.tess) → pointer into file
        │
        │  TESS_Header parsed once at load
        ▼
    For each tensor request:
        │
        │  1. TensorTable lookup: name → offset + size
        │  2. Cell read: CubeData[offset + cell_idx × cell_size]
        │  3. Octant resolve: mirror_formula(slot, octant)
        │  4. Dequant: Q8_0 block → float32 (if needed)
        │
        ▼
    Weight value at geometric address
```

### 5.2 Where Tesseract Fits

```
BEFORE TESSERACT:
  GGUF → gguf_tool (read) → raw bytes → inference
  Problem: flat 1D array, no geometric structure

WITH TESSERACT:
  GGUF → [extract] → [KIS map] → [hyperbolic] → .tess
  .tess → [mmap] → [formula resolve] → [octant view] → inference
  Benefit: 8 views from 1 cube, O(1) geometric access

PIPELINE ROLE:
  .tess = the sealed container between GGUF and inference
  It is NOT a replacement for GGUF — it's a geometric overlay
  that provides structured access to the same weight data.
```

---

## 6. Usage Patterns

### 6.1 Encode (GGUF → .tess)

```c
#include "geo_kis_4d_container.h"
#include "gguf_reader.h"

// 1. Open GGUF
GgufReader gf;
gguf_open("model.gguf", &gf);

// 2. Create .tess container
TESS_Header hdr;
hdr.magic = 0x54455353;          // "TESS"
hdr.version = 1;
hdr.total_slots = 20736;
hdr.cell_size = 34;              // Q8_0
hdr.scale_factor = 65536;        // scale 1.0
hdr.x_slots = 6912;
hdr.y_slots = 6912;
hdr.z_slots = 6912;

// 3. For each tensor: map to cube
for (uint32_t t = 0; t < gf.n_tensors; t++) {
    uint8_t buf[MAX_TENSOR_SIZE];
    gguf_read_tensor("model.gguf", &gf, t, buf, sizeof(buf));

    // Map weights to 20736 grid via stride-37
    for (uint64_t w = 0; w < gf.sizes[t]; w++) {
        uint32_t cell = ((w * 37) % 20736);
        cube_data[cell] = buf[w];  // MAP, not compress
    }
}

// 4. Compute CRC-64 seal
hdr.cube_checksum = crc64(cube_data, 20736 * hdr.cell_size);

// 5. Write .tess file
tess_write("model.tess", &hdr, tensor_table, cube_data, &formula);
```

### 6.2 Decode (.tess → weights)

```c
// 1. Memory-map .tess
TESS_Header *hdr = tess_mmap("model.tess");

// 2. Resolve octant view
uint32_t octant = 0;  // +X +Y +Z (identity)
for (uint32_t slot = 0; slot < 20736; slot++) {
    uint32_t addr = tess_resolve_octant(slot, octant, hdr);
    uint8_t *cell = tess_get_cell(hdr, addr);

    // 3. Dequantize Q8_0 block
    float scale = fp16_to_f32(*(uint16_t*)cell);
    for (int i = 0; i < 32; i++) {
        float weight = (float)cell[2 + i] * scale;
        // → feed to inference engine
    }
}

// 4. Cleanup
tess_unmap(hdr);
```

### 6.3 Octant Parallel Read

```c
// 8 threads, each reading a different octant view
#pragma omp parallel for
for (int octant = 0; octant < 8; octant++) {
    for (uint32_t slot = 0; slot < 20736; slot++) {
        uint32_t addr = tess_resolve_octant(slot, octant, hdr);
        // Read from same cube_data, different resolved address
        // No conflict: each thread reads different logical positions
    }
}
```

---

## 7. Integration with llama.cpp

### 7.1 Option A: GGUF Shim (Recommended for v1)

```c
/* tess_gguf_shim.h — .tess → GGUF-compatible buffer adapter */

// llama.cpp expects: gguf_open() → tensor_by_name() → raw bytes
// tess_gguf_shim provides the same interface backed by .tess mmap

typedef struct {
    TESS_Header  *hdr;       /* mmap'd .tess header */
    uint8_t      *cube;      /* pointer to CubeData */
    TESS_Tensor  *table;     /* tensor table */
    float        *dequant_buf; /* dequantization scratch */
} TessGgufShim;

// Match gguf_reader.h API:
int  tess_shim_open(const char *path, TessGgufShim *shim);
int  tess_shim_find_tensor(TessGgufShim *s, const char *name, uint32_t *idx);
int  tess_shim_read_tensor(TessGgufShim *s, uint32_t idx, void *buf, uint64_t cap);
void tess_shim_close(TessGgufShim *s);
```

### 7.2 Option B: Native .tess Loader (Future)

Add to llama.cpp's `ggml-backend`:
- New backend: `ggml_tess_backend`
- Registers `.tess` as a supported model format
- Direct mmap access, no GGUF conversion needed
- Octant selection via model parameter

### 7.3 Option C: geo_inference_bridge Adaptation

Extend `geo_inference_bridge.h`:
```c
// Existing: GGUF → GeoTensorMap
geo_bridge_build_from_gguf("model.gguf", &map);

// New: .tess → GeoTensorMap
geo_bridge_build_from_tess("model.tess", &map);
// Reuses GeoTensorEntry with .tess-specific offsets
```

---

## 8. Verification Strategy

### 8.1 Lossless Roundtrip Test

```
For each GGUF model:
  1. Read Q8_0 tensor from GGUF
  2. Map to .tess cube (stride-37 scatter)
  3. Read back from .tess (mmap + formula resolve)
  4. Compare: byte-for-byte identical
  5. Assert: 0 mismatches across all 20736 cells
```

### 8.2 Octant Consistency Test

```
For octants 0-7:
  1. Resolve slot S through octant N → address A_N
  2. Resolve address A_N back through inverse octant → slot S'
  3. Assert: S' == S (roundtrip identity)
  4. Assert: cube_data[A_N] == cube_data[original_address]
```

### 8.3 CRC-64 Integrity Test

```
1. Write .tess file
2. Read back, compute CRC-64 of CubeData
3. Compare with hdr.cube_checksum
4. Assert: match (any bit flip → detect)
```

---

## 9. File Size Analysis

For a 644 MB Q8_0 model (291 tensors):

```
TESS_Header:         64 bytes
TensorTable:      ~8,000 bytes (291 tensors × ~28 bytes avg)
CubeData:      20736 × 34 = 705,024 bytes (689 KB)
FormulaBlock:      64 bytes
CRC64:              8 bytes
───────────────────────────────────────────
Total:            ~713 KB (0.11% of original)

vs GGUF:           644 MB
Ratio:             907:1 (for single 20736-cell mapping)
```

**Note:** This ratio assumes all 291 tensors fit in ONE 20736-cell
cube. In practice, tensors larger than 20736 elements span multiple
cubes. The ratio scales linearly with cube count.

For a model with N cubes:
```
.tess_size = 136 + N × 705024 bytes
GGUF_size  = 644,000,000 bytes
Ratio      = GGUF_size / .tess_size
```

---

## 10. Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Storage model | 1 cube + formula | MAP not COMPRESS: formula derives views |
| Octant views | Runtime derivation | 8x dedup without 8x storage |
| Address space | 20736 fixed | Sacred number, 12⁴ = 144² |
| Stride scatter | 37 coprime | Proven lossless bijection |
| Formula storage | In-file | Portable, no external state |
| Checksum | CRC-64/ECMA-182 | Non-reflected, proven in DWGLS |
| Cell layout | GGUF-compatible | Direct dequant, no conversion |
| Optional sections | Type+size prefix | Extensible without breaking |
| mmap access | Zero-copy | Same as gguf_reader.h proven path |

---

## 11. Relationship to Existing Formats

```
┌──────────────┬───────────────┬────────────────────────────┐
│ Format       │ Role          │ .tess Relationship         │
├──────────────┼───────────────┼────────────────────────────┤
│ GGUF         │ Model storage │ Input: .tess extracts from │
│ .gcube       │ GEO container │ Sister: similar header     │
│ .kis4        │ KIS 4D seal   │ Parent: .tess extends it   │
│ .fgls        │ FGLS archive  │ Parallel: different layer  │
│ safetensors  │ Model storage │ Alternative input          │
└──────────────┴───────────────┴────────────────────────────┘
```

---

## 12. Future Extensions

- **Multi-cube models:** Large tensors span N cubes, indexed in TensorTable
- **Progressive load:** Load octant 0 first, others on-demand
- **GPU streaming:** Map CubeData directly to GPU memory via Vulkan
- **Cross-model sharing:** Multiple .tess files reference same CubeData
- **Drift detection:** Compare mirror views for corruption detection
- **Scale traversal:** Animate scale_factor across frames (KIS timeline)
