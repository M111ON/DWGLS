---
luminaCreated: 2026-08-16T06:55:01.353Z
tags: []
luminaModified: 2026-08-16T06:55:01.353Z
luminaVersion: 1.3.11
---
# DWGLS Integration Points — Full Pipeline Map

**Date:** 2026-08-06
**Source Headers:** `core/geo_inference_bridge.h`, `core/geo_tensor_hub.h`, `core/geo_rail_hub.h`, `core/geo_zerocopy.h`, `core/geo_kis_projection.h`

---

## Pipeline Overview: GGUF → Geometry → Inference

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        OFFLINE CONVERSION                              │
│                                                                         │
│   model.gguf ──→ geo_inference_bridge.h ──→ .geo.meta                  │
│   (GGUF file)    (parse tensors)          (GeoTensorMap file)          │
│                                                                         │
│   Then convert to .gcube (DiamondBlock format)                         │
└─────────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                        RUNTIME LOADING                                  │
│                                                                         │
│   llama.cpp ──→ geo_rail_hub_pull(tensor_name)                         │
│                     │                                                   │
│         ┌───────────┴───────────┐                                       │
│         │   LOOKUP PHASE        │                                       │
│         │                       │                                       │
│         │   Option A:           │   Option B:                           │
│         │   geo_tensor_hub.h    │   geo_zerocopy.h                      │
│         │   (fread path)        │   (mmap path)                         │
│         │   GGUF index +        │   mmap .gcube +                       │
│         │   gcube_read +        │   pointer directly into               │
│         │   memcpy to buffer    │   mapped region                       │
│         └───────────┬───────────┘                                       │
│                     │                                                   │
│         ┌───────────┴───────────┐                                       │
│         │   ADDRESSING PHASE    │                                       │
│         │   geo_cell_addr.h     │                                       │
│         │   tensor_offset →     │                                       │
│         │   (pipe_id, tick)     │                                       │
│         └───────────┬───────────┘                                       │
│                     │                                                   │
│         ┌───────────┴───────────┐                                       │
│         │   SYNC / BRIDGE PHASE │                                       │
│         │   FiboSpine (1728     │                                       │
│         │   pipes × 12 ticks)   │                                       │
│         │   tick 0→11 drive     │                                       │
│         │   → jet_bridge_hop    │                                       │
│         │   → P5H freeze @ 12   │                                       │
│         └───────────┬───────────┘                                       │
│                     │                                                   │
│                     ▼                                                   │
│   Returns: pointer to weight data + n_elems + dtype                   │
│   (zero-copy: same pointer, no additional malloc/copy)                 │
└─────────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    GEOMETRIC REPRESENTATION                             │
│                                                                         │
│   geo_kis_projection.h                                                  │
│   ┌─────────────────────────────────────────────┐                       │
│   │  4D Tesseract → 3D KIS{x,y,z} projection   │                       │
│   │                                             │                       │
│   │  1D weight array → 3 axes via circular      │                       │
│   │  offsets (stride 1728, 3456)                │                       │
│   │                                             │                       │
│   │  x[i] = data[i]                            │                       │
│   │  y[i] = data[(i+1728) % n]                 │                       │
│   │  z[i] = data[(i+3456) % n]                 │                       │
│   │                                             │                       │
│   │  Error correction: 2-of-3 majority vote    │                       │
│   │  across axes for corruption detection       │                       │
│   └─────────────────────────────────────────────┘                       │
│   Sacred constants: 20736, 1728, 144, 12, 128, 162                     │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Per-Header Analysis

### 1. `geo_inference_bridge.h` — GGUF → GEO Mapping

| Aspect | Detail |
|--------|--------|
| **Connects** | GGUF file (model weights) → GeoTensorMap (geometry block addresses) |
| **Direction** | Offline/build-time: GGUF in, .geo.meta out |
| **Key APIs** | `geo_bridge_build_from_gguf(path, &map, override)` — parse GGUF, build map |
| | `geo_bridge_resolve(&map, "tensor.name")` — lookup tensor → GEO block range |
| | `geo_bridge_read_tensor(geo_path, &tensor, buf, sz)` — read weight from GEO |
| | `geo_bridge_print_mapping(&map)` — debug dump |
| **Data Flow** | GGUF tensor list → sequential GEO FrustumBlocks (GEO_FBLOCK_SZ each) |
| **Key Struct** | `GeoTensorEntry`: name, dtype, data_size, geo_block_start, geo_block_count |
| | `GeoFileHeader`: magic "GEOF", version, n_blocks, xxh64 digest |

### 2. `geo_tensor_hub.h` — Runtime Tensor Hub (fread path)

| Aspect | Detail |
|--------|--------|
| **Connects** | llama.cpp → .gcube weight data (via fread + memcpy) |
| **Direction** | Runtime: tensor name in → malloc'd weight buffer out |
| **Key APIs** | `geo_hub_open(&hub, gguf_path, gcube_path)` — load GGUF index + .gcube |
| | `geo_hub_load(&hub, name, &data, &n, &dtype)` — single tensor pull |
| | `geo_hub_load_all(&hub, &batch)` — batch pre-fetch all tensors |
| | `geo_hub_close(&hub)` — cleanup |
| **Data Flow** | tensor_name → GGUF index (metadata) → gcube_find → gcube_tensor_data → memcpy to new buffer |
| **Key Struct** | `GeoTensorHub`: GGUFTensorIndex + GCubeContainer |
| **Note** | Hub is **thin**: no cache, no threads. O(1) per tensor. Mallocs output buffer. |

### 3. `geo_zerocopy.h` — mmap Zero-Copy Path

| Aspect | Detail |
|--------|--------|
| **Connects** | .gcube file on disk → direct pointer into mmap'd region |
| **Direction** | Runtime: mmap, no fread, no malloc for block data |
| **Key APIs** | `geo_zerocopy_open(&z, path)` — mmap entire .gcube file |
| | `geo_zerocopy_load(&z, name, &data, &n, &dtype)` — pointer into mmap |
| | `geo_zerocopy_close(&z)` — unmap + release |
| **Data Flow** | .gcube file → CreateFileMapping/mmap → parse header in-place → blocks = mapped region pointer |
| **Platform** | Windows: `CreateFileMappingA` + `MapViewOfFile`; Unix: `mmap` |
| **Key Struct** | `GeoZeroCopy`: file handle, mapping handle, base pointer, embedded GCubeContainer |
| **Critical Detail** | `cube->blocks = p` (pointer into mmap, NOT malloc'd) |

### 4. `geo_rail_hub.h` — Orchestrating Sync Pipeline (Hot Path)

| Aspect | Detail |
|--------|--------|
| **Connects** | llama.cpp → full pipeline: lookup + address + sync + bridge + barrier |
| **Direction** | Runtime hot path: tensor_name in → weight pointer out (zero-copy) |
| **Key APIs** | `geo_rail_hub_open(&r, hub)` — init spine + ribcage + gear lock |
| | `geo_rail_hub_open_zc(&r, hub, &zc)` — same but with zero-copy mmap path |
| | `geo_rail_hub_pull(&r, name, &data, &n, &dtype)` — **main entry point** |
| | `geo_rail_hub_pull_batch(&r, names, ...)` — batch variant |
| | `geo_rail_hub_reset(&r)` — wipe spine between tensors |
| | `geo_rail_hub_close(&r)` — cleanup |
| **6-Step Pull Pipeline** | 1. **LOOKUP**: `geo_hub_load()` or `geo_zerocopy_load()` |
| | 2. **ADDRESS**: `geo_cell_addr_offset_to_pipe()` → (pipe_id, tick) |
| | 3. **SYNC**: `fibo_spine_pipe_tick()` until tick 11 (Jet Bridge) |
| | 4. **BRIDGE**: `jet_bridge_hop()` — no data copy |
| | 5. **BARRIER**: `p5h_freeze_at_tick12()` — freeze barrier |
| | 6. **DELIVER**: same pointer — zero copy added by rail hub |
| **Key Struct** | `GeoRailHub`: borrowed GeoTensorHub + borrowed GeoZeroCopy + FiboSpine (28KB) + P5HRibcage + GearLock |
| **Constraints** | No malloc in rail hub itself. No float/double. Header-only. Stack too big for hot loop (use static/heap). |

### 5. `geo_kis_projection.h` — 4D Tesseract → 3D KIS Geometry

| Aspect | Detail |
|--------|--------|
| **Connects** | 1D weight data → 3D geometric representation with error correction |
| **Direction** | Geometric transformation: weights in, KIS{x,y,z} axes out |
| **Key APIs** | `kis_project_4d_to_3d(x4,y4,z4,w4,scale)` — perspective projection 4D→3D |
| | `kis_axis_from_1d(&axes, data, n)` — 1D array → 3 axes via circular offsets |
| | `kis_axis_verify(&axes, coord)` — consistency check across axes |
| | `kis_axis_lock(&axes, coord)` — freeze all 3 axes, return value if consistent |
| | `kis_axis_correct(&axes, coord)` — 2-of-3 majority vote error correction |
| **Data Flow** | 1D weight[20736] → x[i]=data[i], y[i]=data[(i+1728)%n], z[i]=data[(i+3456)%n] |
| **Key Struct** | `KISAxes`: x[20736], y[20736], z[20736] + metadata |
| **Error Correction** | Majority vote across 3 axes; detects + corrects single-axis corruption |
| **Constants** | 20736 = 12⁴, 1728 = 12³, 3456 = 2×1728 |

---

## Complete Data Flow Map

```
PHASE 1: OFFLINE CONVERSION
═══════════════════════════
  model.gguf (Q4/Q8/IQ quantized weights)
       │
       ▼ geo_bridge_build_from_gguf()
  GeoTensorMap (tensor name → block range mapping)
       │
       ▼ (convert to DiamondBlock format)
  .gcube file (blocks in FrustumBlock layout)
       │
       ▼
  .geo.meta file (optional: mapping metadata)

PHASE 2: RUNTIME — TWO PATHS
═════════════════════════════

  PATH A: fread (geo_tensor_hub.h)
  ─────────────────────────────────
  .gcube → gcube_read() → malloc blocks → memcpy per tensor
  gguf_idx_open() → metadata lookup (names, dtypes)
  geo_hub_load() → malloc + fread + memcpy → return buffer

  PATH B: mmap zero-copy (geo_zerocopy.h)
  ────────────────────────────────────────
  .gcube → mmap entire file → pointer into mapped region
  No malloc, no fread, no memcpy for block data
  geo_zerocopy_load() → pointer arithmetic → return mmap pointer

PHASE 3: RAIL HUB ORCHESTRATION (geo_rail_hub.h)
══════════════════════════════════════════════════
  llama.cpp calls: geo_rail_hub_pull("blk.0.attn_q.weight")
       │
       ├─→ 1. LOOKUP:     hub_load() or zerocopy_load() → raw pointer + metadata
       ├─→ 2. ADDRESS:    tensor_offset → geo_cell_addr_offset_to_pipe() → (pipe_id=42, tick=7)
       ├─→ 3. SYNC:       FiboSpine tick drive: pipe 42 ticks 0→1→2→...→11
       ├─→ 4. BRIDGE:     jet_bridge_hop(pipe_id=42) — entry point, no data copy
       ├─→ 5. BARRIER:    p5h_freeze_at_tick12() — all pipes frozen at tick 12
       └─→ 6. DELIVER:    return same pointer from step 1 (zero copy)
                     │
                     ▼
  llama.cpp receives: { pointer, n_elems, dtype }
  → feeds into GGML compute graph

PHASE 4: GEOMETRIC REPRESENTATION (geo_kis_projection.h)
══════════════════════════════════════════════════════════
  Weight data (uint8_t[20736])
       │
       ▼ kis_axis_from_1d()
  KIS{x,y,z} axes:
    x[i] = data[i]
    y[i] = data[(i+1728) % n]
    z[i] = data[(i+3456) % n]
       │
       ├─→ kis_axis_verify() — check 3-axis consistency
       ├─→ kis_axis_lock()   — freeze at point, require agreement
       └─→ kis_axis_correct() — 2-of-3 majority vote error correction
       │
       ▼
  4D→3D projection: kis_project_4d_to_3d(x4,y4,z4,w4,scale)
    fixed-point math, no floats
    packs result as x3[12]|y3[12]|z3[8]
```

---

## Key Integration Points Summary

| # | Integration | From | To | Function |
|---|------------|------|-----|----------|
| 1 | GGUF parsing | gguf_reader.h / gguf_index.h | geo_inference_bridge.h | Extract tensor metadata from GGUF |
| 2 | GEO mapping | geo_inference_bridge.h | geo_tensor_map.h | Build name→block_range map |
| 3 | Hub open | geo_tensor_hub.h | geo_cube_container.h + gguf_index.h | Load GGUF metadata + .gcube blocks |
| 4 | Zero-copy open | geo_zerocopy.h | geo_cube_container.h | mmap .gcube, pointer into mapped region |
| 5 | Rail hub pull | geo_rail_hub.h | geo_tensor_hub.h OR geo_zerocopy.h | Lookup (fread or mmap path) |
| 6 | Cell addressing | geo_rail_hub.h | geo_cell_addr.h | tensor_offset → (pipe_id, tick) |
| 7 | Spine sync | geo_rail_hub.h | infra/fibo_spine.h | 1728-pipe tick drive to tick 11 |
| 8 | Bridge hop | geo_rail_hub.h | infra/fibo_spine.h | jet_bridge_hop — no data copy |
| 9 | Freeze barrier | geo_rail_hub.h | infra/fibo_spine.h | p5h_freeze_at_tick12 |
| 10 | KIS projection | geo_kis_projection.h | (standalone) | 1D weights → 3D geometric axes with error correction |

---

## Ownership & Lifecycle

```
geo_rail_hub (hot path)
  ├─ borrows → GeoTensorHub (opened once, outlives rail hub)
  ├─ borrows → GeoZeroCopy  (optional, mmap'd .gcube)
  ├─ owns    → FiboSpine    (28KB, reset between tensors)
  ├─ owns    → P5HRibcage   (heap, freed on close)
  └─ owns    → GearLock     (CPU=128, GPU=162 worlds)

Data ownership:
  - fread path:   caller owns malloc'd buffer, must free()
  - mmap path:    pointer into OS-mapped region, no free needed
  - Both paths:   rail hub passes pointer through without copy
```

---

## Key Design Constraints

1. **Zero-copy by default**: Rail hub returns the same pointer from lookup phase — no additional copy.
2. **No malloc in hot path**: Rail hub does zero allocation per pull. Hub's fread path mallocs once per tensor; mmap path never mallocs.
3. **No floats in rail hub or KIS**: All arithmetic is fixed-point (<<16) or integer.
4. **Header-only**: All five files are static inline, header-only, compile-time checked.
5. **Sacred geometry**: 20736 = 12⁴, 1728 = 12³ (FiboSpine pipes), 144 = 12², 12 = base unit.
6. **Error correction**: KIS axes provide 2-of-3 majority vote across 3 projections of same data.
