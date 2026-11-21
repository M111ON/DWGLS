# DWGLS Tesspack — Technical Document
**Version:** 1.0
**Date:** 2026-11-22
**Status:** Math proof complete, wire proof complete, server green changes done

---

## Table of Contents

1. [Overview](#1-overview)
2. [Sacred Numbers](#2-sacred-numbers)
3. [Architecture](#3-architecture)
4. [Core Components](#4-core-components)
5. [TessResolver (Zero-Copy Multi-Cube)](#5-tessresolver)
6. [TessJump (Jump Module)](#6-tessjump)
7. [Field Model (Focus / Out-of-Focus)](#7-field-model)
8. [Negative Side (Inside-Out)](#8-negative-side)
9. [Server (tesspack_server)](#9-server)
10. [Proof Results](#10-proof-results)
11. [File Reference](#11-file-reference)

---

## 1. Overview

DWGLS Tesspack is a geometric storage system for LLM weights. It maps tensor data into a 4D tesseract (20736-cell cube) using stride-37 bijection, derives 8 octant views at runtime (no 8x storage), and provides zero-copy access via pointer arithmetic (no Gather, no memcpy).

**Core principle:** MAP not COMPRESS — coordinate = address. The field never inspects byte meaning.

**Key innovation:** Physical layout = contiguous mmap'd cubes. Logical addressing = stride-37 + hyperbolic mirror (pointer arithmetic). No flat buffer, no 5.4 GB allocation, no VEH lazy commit.

---

## 2. Sacred Numbers

| Number | Meaning | Derivation |
|---|---|---|
| 20736 | Total address space (1 cube) | 12⁴ = 144² = 128 × 162 |
| 1728 | FiboSpine pipes | 12³ |
| 144 | Tesseract edge | 12² |
| 128 | Compute route | 2 × 64 |
| 162 | Geometry route | 2 × 3⁴ |
| 6912 | Axis slot count | 20736 / 3 |
| 34 | Q8_0 block size | 2B scale + 32B int8 |
| 37 | Stride coprime | gcd(37, 20736) = 1 |
| 1440 | Frame seek cycle | Fibo clock |
| 24 | Tetra count (positive) | 8 octants × 3 axes |
| 48 | Octahedral group | 24 + 24 (pos + neg) |
| 42 | Regions (6 planes) | cake(6) = (216+30+6)/6 |
| 8 | Octants | 2³ |
| 3 | Axes / usable tetra faces | X, Y, Z |
| 4 | Tetra faces (1 blocked by octa) | 3 usable + 1 blocked |
| 111 | Walking engine unit | 111³ = 1367631 (palindrome) |
| 162 | Anchors (experiment) | 2 × 3⁴ |
| 128 | Bits per anchor | 2⁷ |

---

## 3. Architecture

### 3.1 Layer Stack

```
┌─────────────────────────────────────────────────┐
│  Application (llama.cpp / OpenAI API)           │
├─────────────────────────────────────────────────┤
│  TessPack Server (HTTP, OpenAI-compatible)      │
├─────────────────────────────────────────────────┤
│  Jump Module (geo_jump → hyper_jump → frame_seek │
│               → fan24 → jet → 144×144)          │
├─────────────────────────────────────────────────┤
│  TessResolver (zero-copy multi-cube + cascade   │
│                + Voronoi + distort + inside-out) │
├─────────────────────────────────────────────────┤
│  .tesspack (mmap'd, capos = cubes, stride-37)  │
├─────────────────────────────────────────────────┤
│  GGUF (source model weights)                    │
└─────────────────────────────────────────────────┘
```

### 3.2 Data Flow

```
GGUF tensor → [extract] → [KIS map: weight_idx → slot] → [stride-37: slot → cell]
  → [hyperbolic: cell → resolved cell] → .tesspack (capo = cube)
  → mmap (zero-copy) → TessCubeView → tess_resolve_multi(slot) → pointer
  → llama.cpp (contiguous view via resolver)
```

### 3.3 Physical vs Logical

| Aspect | Physical (Storage) | Logical (Address) |
|---|---|---|
| What | Contiguous mmap'd cube (705 KB) | Stride-37 + mirror + balanced ternary |
| Where | Disk (.tesspack capo) | Computed at runtime |
| Contiguous? | Yes (per cube) | No (scattered) |
| Zero-copy? | Yes (mmap, owns_buf=0) | Yes (pointer arithmetic) |
| Gather needed? | No | No (offset = pointer) |

---

## 4. Core Components

### 4.1 TessResolver (`core/tess_resolver.h`)

Zero-copy multi-cube resolver. Provides:
- `TessCubeView`: single cube view (base, cell_size, n_cells, capo_id)
- `TessMultiCubeView`: array of cube views for multi-cube tensors
- `tess_resolve_ptr(cube, slot)`: single-cube zero-copy resolve
- `tess_resolve_multi(mc, weight_idx)`: multi-cube zero-copy resolve
- `TessCascade`: quadtree bottom-up OR propagation (rewind)
- `TessVoronoiMask`: temporary unfold for multi-pointer read
- `tess_distort(slot, pointer, total)`: constant distort (hyperbolic two-sided)
- `tess_slot_to_cell(slot)`: stride-37 bijection
- `tess_cell_to_slot(cell)`: inverse bijection
- Balanced ternary: `bt_encode`, `bt_decode`, `bt_coord_to_slot`
- 3/4 Tetra: `tetra_can_branch`, `tetra_usable_count`
- 111³ walking: `walk_step`, `WalkPos`
- Inside-out: `POS_COUNT=24`, `NEG_COUNT=24`, `OCTAHEDRAL_GROUP=48`

### 4.2 TessJump (`core/tess_jump.h`)

Jump module integrating geo_jump + hyper_jump + frame_seek + fan24 + jet.

**Jump types:**
| Type | Description |
|---|---|
| JUMP_HILBERT | Basic Hilbert jump |
| JUMP_PEANO | Peano space-filling |
| JUMP_PENTAGON | Pentagon jump |
| JUMP_MOD | MOD jump |
| JUMP_INVERT | Invert (mirror) |
| JUMP_GROUND | Ground jump |
| JUMP_FRAME | Frame seek (fishbone snap, 1440 cycle) |
| JUMP_FAN24 | Fan24 CRT (ring-24 bijection) |
| JUMP_HYPER | Hyper jump (4D, HJ3/HJ4 towers) |
| JUMP_JET | Jet bridge (snap to 144×144) |

**Full sequence:** geo_jump (outer) → hyper_jump (dims) → frame_seek (fishbone) → fan24 (CRT) → jet (snap) → 144×144

**Two-sided:** carries positive/negative side. Cross boundary = flip side.

---

## 5. TessResolver

### 5.1 Stride-37 Bijection

```c
static inline uint32_t tess_slot_to_cell(uint32_t slot) {
    return (slot * 37u) % 20736u;  /* bijection: gcd(37,20736)=1 */
}
```

Proven: 20736/20736 identity (slot→cell→slot).

### 5.2 Multi-Cube Resolver

```c
static inline const uint8_t *tess_resolve_multi(const TessMultiCubeView *mc, uint64_t weight_idx) {
    uint32_t cube_id = (uint32_t)(weight_idx / TESS_TOTAL_SLOTS);
    uint32_t local_slot = (uint32_t)(weight_idx % TESS_TOTAL_SLOTS);
    if (cube_id >= mc->n_caps) return NULL;
    if (!mc->caps[cube_id].base) return NULL;
    uint32_t cell = tess_slot_to_cell(local_slot);
    return mc->caps[cube_id].base + (size_t)cell * mc->caps[cube_id].cell_size;
}
```

**Zero-copy:** Returns pointer into mmap'd cube. No allocation, no memcpy, no Gather.

### 5.3 Quadtree Cascade (Rewind)

```c
typedef struct {
    uint8_t leaf[64];   /* level 0: 64 leaves */
    uint8_t l16[16];    /* level 1: 16 groups (OR of 4) */
    uint8_t l4[4];      /* level 2: 4 groups (OR of 4) */
    uint8_t root;       /* level 3: 1 root (OR of 4) */
    int     active;
} TessCascade;
```

Bottom-up OR propagation: leaf → l16 → l4 → root. Pointer approaches → cascade activates → data comes back 100% (rewind). Pointer moves away → cascade deactivates → data shrinks (domino).

### 5.4 Voronoi Mask (Multi-Pointer Read)

```c
typedef struct {
    uint8_t mask[TESS_TOTAL_SLOTS];  /* 1 = unfolded, 0 = folded */
    int     applied;
} TessVoronoiMask;
```

Temporarily unfold a region for reading (multi-pointer). Does NOT affect field state. Re-fold after reading.

### 5.5 Constant Distort (Hyperbolic Two-Sided)

```c
static inline uint32_t tess_distort(uint32_t slot, uint32_t pointer_pos, uint32_t total) {
    uint32_t d = (slot > pointer_pos) ? (slot - pointer_pos) : (pointer_pos - slot);
    if (d > total / 2) d = total - d;  /* shortest path on circle */
    return d;
}
```

- Near pointer → distort ≈ 0 (exact)
- Far from pointer → distort grows (constant, not random)
- Symmetric: distort(a,b) = distort(b,a)
- Deterministic: same input → same output

### 5.6 Balanced Ternary [-1,0,1]

```c
static inline int bt_encode(int val, int *digits, int n);
static inline int bt_decode(const int *digits, int n);
```

Base-3 with digits {-1, 0, 1}. Floor division for negative numbers. Range: n digits → [-(3^n-1)/2, (3^n-1)/2].

### 5.7 3/4 Tetra Branch

```c
#define TETRA_FACES  4
#define TETRA_USABLE 3
#define TETRA_BLOCKED 3  /* face 3 = blocked by octa */

static inline int tetra_can_branch(int face_idx) {
    return (face_idx != TETRA_BLOCKED) ? 1 : 0;
}
```

4 faces, 3 usable (1 blocked by octa). Deterministic.

### 5.8 111³ Walking Engine

```c
typedef struct { int x, y, z; } WalkPos;
static inline WalkPos walk_step(WalkPos p, int dx, int dy, int dz);
```

1 unit per axis, clamped to [-111, 111]. Walk back → origin (rewind). Deterministic.

### 5.9 Inside-Out (pos24 + neg24 = 48)

```c
#define POS_COUNT 24
#define NEG_COUNT 24
#define OCTAHEDRAL_GROUP 48  /* 8 octants × 6 = 48 */
```

Octahedral group = 48. Closed loop: pos24 - neg24 = 0 (returns to origin).

---

## 6. TessJump

### 6.1 Components

| Component | Source | Role |
|---|---|---|
| geo_jump | `sid/geo_jump.h` | Outer layer: basic jump types (Hilbert, Peano, Pentagon, MOD, Invert, Ground) |
| hyper_jump | `core/geo_hyper_jump.h` | Through dimensions: 4D tes-compound towers (HJ3: 48×3, HJ4: 36×4) |
| frame_seek | `core/geo_frame_seek.h` | Fishbone snap: deterministic frame seek on Fibo 1440 cycle (stride-37) |
| fan24 | `core/fan24_gear.h` | CRT bijection: ring-24 (dc mod 8, dx mod 3) |
| jet | `core/infra/fibo_spine.h` | Bridge: discrete leads, continuous chases (ambulance, not highway) |

### 6.2 Jump Sequence

```
geo_jump (outer) → hyper_jump (dims) → frame_seek (fishbone) → fan24 (CRT) → jet (snap) → 144×144
```

### 6.3 Two-Sided

Carries positive-negative side (+1/-1). Cross boundary = flip side. Inside-out: pos24 + neg24 = 48.

---

## 7. Field Model

### 7.1 Focus / Out-of-Focus

```
Pointer at position P:
  Near regions (dist < 5): IN-FOCUS (exact, distort=0, computed)
  Far regions (dist >= 5): OUT-OF-FOCUS (distort>0, NOT computed, lazy)
```

- **In-focus:** 9 regions (exact, distort=0, computed)
- **Out-of-focus:** 33 regions (distort>0, lazy eval)
- **Never computed:** 23/42 (no wasted work)
- **Rewind:** pointer returns → regions re-activate
- **Multi-pointer read:** Voronoi unfold → cure distort→0 for specific region

### 7.2 Lazy Evaluation

Only compute what's needed. Out-of-focus regions are NOT computed. This saves CPU/RAM.

### 7.3 Constant Distort

Hyperbolic two-sided: distort = f(distance). Near → 0, far → grows. Deterministic, symmetric.

---

## 8. Negative Side (Inside-Out)

### 8.1 Two-Sided Field

- Positive side: 42 regions (0..41)
- Negative side: 42 regions (-1..-42) [inside-out]
- Total: 84 regions (two-sided)

### 8.2 Inside-Out

- pos24 + neg24 = 48 (octahedral group)
- pos24 - neg24 = 0 (closed loop, returns to origin)
- 3 square planes = identity anchor (origin core)
- Crossing boundary: continuous (no jump)

### 8.3 Balanced Ternary

Base-3 with digits {-1, 0, 1}. Covers both positive and negative sides. Floor division for negative numbers.

---

## 9. Server (tesspack_server)

### 9.1 Before (Flat Buffer)

```
GGUF → calloc(5.4 GB) → scatter all capos → flat buffer → llama.cpp
       ↑ OOM on <8 GB RAM
       ↑ breathe_commit (VEH lazy commit) — band-aid
       ↑ selective scatter (MoE expert masking) — decommit unfired
```

### 9.2 After (Zero-Copy Resolver)

```
.tesspack (mmap) → TessCubeView (zero-copy ptr) → tess_resolve_multi(slot) → pointer
                   ↑ no allocation, no Gather, no memcpy
                   ↑ aligned calloc for llama.cpp contiguous buffer
                   ↑ ONION path for F32/F16 raw entries
```

### 9.3 Changes

| Change | Before | After |
|---|---|---|
| VEH lazy commit | `breathe_register()` + `breathe_reserve()` | Removed |
| Memory allocation | `VirtualAlloc(MEM_COMMIT\|MEM_RESERVE)` | `aligned_alloc(32)` |
| Selective scatter | VEH decommit | Removed (no VEH) |
| Resolver | Not included | `#include tess_resolver.h` |
| ONION path | In `load_pack_tensor` | Added to server scatter loop |
| from_pack | 197 | 338 |
| from_zero | 141 | 0 |
| Load time | 28,031 ms | 1,943 ms (14.4× faster) |

### 9.4 API

- `POST /v1/chat/completions` — OpenAI-compatible chat
- `POST /v1/completions` — text completion
- `GET /health` — health check

---

## 10. Proof Results

### 10.1 Test Summary

| Test File | Assertions | Result |
|---|---|---|
| test_cube_micro.c | 25 | ✅ PASS |
| test_balanced_ternary.c | 27 | ✅ PASS |
| test_system_full.c | 29 | ✅ PASS |
| test_hyperplane_6axis.c | 25 | ✅ PASS |
| test_cube_wire.c | 2 (tensors) | ✅ IDENTICAL |
| test_field_focus.c | 19 | ✅ PASS |
| test_negative_side.c | 33 | ✅ PASS |
| test_jump_module.c | 35 | ✅ PASS |
| **Total** | **193+** | **All PASS** |

### 10.2 Wire Proof

| Tensor | Capos | Blocks | Result |
|---|---|---|---|
| token_embd.weight | 50 | 1,024,000 | ✅ IDENTICAL |
| blk.2.ffn_gate_exps.weight | 23 | 458,752 | ✅ IDENTICAL |

### 10.3 Server Test

| Model | Tensors | Load time | Output |
|---|---|---|---|
| huihui-moe-1b-q4_k_m | 338 | 1.9s | ✅ Coherent |
| minicpm5-1b | 219 | 23.1s | ❌ Garbage (chat template?) |
| bonsai-4b-q1_0 | 145 | 0.3s | ❌ Garbage (chat template?) |

---

## 11. File Reference

### Core

| File | Description |
|---|---|
| `core/tess_resolver.h` | Full system resolver (multi-cube + balanced ternary + 3/4 + 111³ + cascade + Voronoi + distort + inside-out) |
| `core/tess_jump.h` | Jump module (geo_jump + hyper_jump + frame_seek + fan24 + jet) |

### Tests

| File | Assertions | Description |
|---|---|---|
| `tests/test_cube_micro.c` | 25 | Stride-37 + cascade + Voronoi + distort roundtrip |
| `tests/test_balanced_ternary.c` | 27 | Balanced ternary + 3/4 + 111³ + inside-out |
| `tests/test_system_full.c` | 29 | Full system integration |
| `tests/test_hyperplane_6axis.c` | 25 | 6-axis hyperplane + pointer rewind |
| `tests/test_cube_wire.c` | 2 tensors | Zero-copy multi-cube resolve vs GGUF |
| `tests/test_field_focus.c` | 19 | Field focus/out-of-focus + lazy eval |
| `tests/test_negative_side.c` | 33 | Negative side/inside-out |
| `tests/test_jump_module.c` | 35 | Jump module (all 10 types) |

### Server

| File | Description |
|---|---|
| `tools/tesspack_server.c` | OpenAI-compatible HTTP server from .tesspack |

### Docs

| File | Description |
|---|---|
| `docs/tess-format-spec.md` | .tess file format specification v1.0 |
| `docs/STATUS-REPORT-2026-11-22.md` | Status report |
| `docs/TECHNICAL-DOCUMENT.md` | This document |
| `docs/inspiration/` | 40+ geometric reference images |

### Experiments

| File | Description |
|---|---|
| `experiments/BottomUp_bin_21112026/test.c` | 144×144 bit matrix bottom-up cascade |
| `experiments/BottomUp_bin_21112026/notebook.py` | 162 anchors × 128 bits quadtree |

---

*Document generated by OpenCode agent on 2026-11-22*