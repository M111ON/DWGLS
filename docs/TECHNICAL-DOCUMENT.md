# DWGLS Tesspack — Technical Document
**Version:** 1.1 (updated with critical review feedback)
**Date:** 2026-11-22
**Status:** Math proof complete, wire proof complete, server green changes done

---

## Table of Contents

1. [Overview](#1-overview)
2. [Sacred Numbers — Justified vs Design Choice](#2-sacred-numbers)
3. [Architecture](#3-architecture)
4. [Core Components](#4-core-components)
5. [TessResolver (Zero-Copy Multi-Cube)](#5-tessresolver)
6. [TessJump (Jump Module)](#6-tessjump)
7. [Field Model (Focus / Out-of-Focus)](#7-field-model)
8. [Negative Side (Inside-Out)](#8-negative-side)
9. [Server (tesspack_server)](#9-server)
10. [Proof Results](#10-proof-results)
11. [Critical Review & Open Questions](#11-critical-review)
12. [File Reference](#12-file-reference)

---

## 1. Overview

DWGLS Tesspack is a geometric storage system for LLM weights. It maps tensor data into a 4D tesseract (20736-cell cube) using stride-37 bijection, derives 8 octant views at runtime (no 8x storage), and provides zero-copy access via pointer arithmetic.

**Core principle:** MAP not COMPRESS — coordinate = address. The field never inspects byte meaning.

**Key innovation:** Physical layout = contiguous mmap'd cubes. Logical addressing = stride-37 + hyperbolic mirror (pointer arithmetic). No flat buffer, no 5.4 GB allocation, no VEH lazy commit.

**Honest caveat:** Byte-identical ≠ performance-identical. Stride-37 scatter hurts cache locality for sequential reads. The benefit is geometric structure (deterministic addressing, 8x view derivation, climate shift), NOT read throughput. See §11.2.

---

## 2. Sacred Numbers — Justified vs Design Choice

### 2.1 Mathematically Justified

| Number | Justification | Status |
|---|---|---|
| 37 (stride) | gcd(37, 20736) = 1 → bijection, lossless | ✅ Proven |
| 42 (regions) | cake(6) = (216+30+6)/6 = 42, mathematically derived | ✅ Proven |
| 48 (octahedral group) | |O| = 48 (rotations of cube/octahedron), group theory | ✅ Proven |
| 20736 (1 cube) | 12⁴ = 144² = 2⁸ × 3⁴, divisible by 3 axes × 8 octants | ✅ Justified (if 12 is justified) |
| 24 (tetra count) | 8 octants × 3 axes = 24 | ✅ Justified |
| 8 (octants) | 2³ (binary: 3 axes × 2 signs) | ✅ Justified |
| 3 (axes) | X, Y, Z (3D space) | ✅ Justified |
| 4 (tetra faces) | Tetrahedron has 4 faces; 1 blocked by octa = 3 usable | ✅ Justified |
| 34 (Q8_0 block) | 2B FP16 scale + 32B int8 = 34 bytes (GGUF spec) | ✅ GGUF standard |

### 2.2 Design Choices (Not Mathematically Necessary)

| Number | Value | Justification | Status |
|---|---|---|---|
| 12 | Base-12 | Chosen for divisibility (12⁴=20736, 12³=1728, 12²=144). Why 12 and not 10 or 16? **No mathematical proof given.** | ⚠️ Design choice |
| 1440 (frame cycle) | 1440 | 12 × 120 = 1440. Why 1440 and not 720 or 2160? **No justification.** | ⚠️ Design choice |
| 111 (walking unit) | 111 | 111³ = 1367631 (palindrome). Why 111 and not 100 or 128? **Aesthetic (palindrome), not engineering.** | ⚠️ Design choice |
| 162 | 2 × 3⁴ | Geometry route (icosahedron frequency-4). Why 162 and not 144 or 256? **Design choice.** | ⚠️ Design choice |
| 1728 | 12³ | FiboSpine pipes. Derived from 12 (design choice). | ⚠️ Derived from 12 |
| 144 | 12² | Tesseract edge. Derived from 12 (design choice). | ⚠️ Derived from 12 |

**Honest assessment:** Numbers 37, 42, 48, 8, 3, 4, 34 are mathematically justified. Numbers 12, 111, 1440, 162, 1728, 144 are design choices without mathematical necessity. The system works with any base, but 12 was chosen for its divisibility properties. This is not a flaw — many systems have arbitrary base choices (e.g., base-10, base-16) — but it should be documented as a design decision, not a mathematical law.

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
| Gather needed? | No (for access) | No (offset = pointer) |

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
- Balanced ternary: `bt_encode`, `bt_decode`, `bt_coord_to_slot`
- 3/4 Tetra: `tetra_can_branch`, `tetra_usable_count`
- 111³ walking: `walk_step`, `WalkPos`
- Inside-out: `POS_COUNT=24`, `NEG_COUNT=24`, `OCTAHEDRAL_GROUP=48`

### 4.2 TessJump (`core/tess_jump.h`)

Jump module integrating geo_jump + hyper_jump + frame_seek + fan24 + jet.

**Jump types:** 10 types (HILBERT, PEANO, PENTAGON, MOD, INVERT, GROUND, FRAME, FAN24, HYPER, JET)

**Full sequence:** geo_jump (outer) → hyper_jump (dims) → frame_seek (fishbone) → fan24 (CRT) → jet (snap) → 144×144

**Two-sided:** carries positive-negative side. Cross boundary = flip side.

---

## 5. TessResolver

### 5.1 Stride-37 Bijection

```c
static inline uint32_t tess_slot_to_cell(uint32_t slot) {
    return (slot * 37u) % 20736u;  /* bijection: gcd(37,20736)=1 */
}
```

**Proven:** 20736/20736 identity (slot→cell→slot).

**Why stride-37?** See §11.2 for honest analysis.

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

Bottom-up OR propagation: leaf → l16 → l4 → root. Pointer approaches → cascade activates → data comes back 100% (rewind). Pointer moves away → cascade deactivates → data shrinks (domino).

### 5.4 Voronoi Mask (Multi-Pointer Read)

Temporarily unfold a region for reading (multi-pointer). Does NOT affect field state. Re-fold after reading.

### 5.5 Constant Distort (Hyperbolic Two-Sided)

Near pointer → distort ≈ 0 (exact). Far from pointer → distort grows (constant, not random). Symmetric, deterministic.

### 5.6 Balanced Ternary [-1,0,1]

Base-3 with digits {-1, 0, 1}. Floor division for negative numbers.

### 5.7 3/4 Tetra Branch

4 faces, 3 usable (1 blocked by octa). Deterministic.

### 5.8 111³ Walking Engine

1 unit per axis, clamped to [-111, 111]. Walk back → origin (rewind). Deterministic.

### 5.9 Inside-Out (pos24 + neg24 = 48)

Octahedral group = 48. Closed loop: pos24 - neg24 = 0.

---

## 6. TessJump

### 6.1 Components

| Component | Source | Role |
|---|---|---|
| geo_jump | `sid/geo_jump.h` | Outer layer: basic jump types |
| hyper_jump | `core/geo_hyper_jump.h` | Through dimensions: 4D tes-compound towers |
| frame_seek | `core/geo_frame_seek.h` | Fishbone snap: deterministic frame seek on Fibo 1440 cycle |
| fan24 | `core/fan24_gear.h` | CRT bijection: ring-24 |
| jet | `core/infra/fibo_spine.h` | Bridge: discrete leads, continuous chases |

### 6.2 Jump Sequence

```
geo_jump (outer) → hyper_jump (dims) → frame_seek (fishbone) → fan24 (CRT) → jet (snap) → 144×144
```

### 6.3 Two-Sided

Carries positive-negative side. Cross boundary = flip side.

---

## 7. Field Model

### 7.1 Focus / Out-of-Focus

- In-focus (dist < 5): exact, distort=0, computed
- Out-of-focus (dist >= 5): distort>0, NOT computed, lazy
- Rewind: pointer returns → regions re-activate
- Multi-pointer read: Voronoi unfold → cure distort→0

### 7.2 Lazy Evaluation

Only compute what's needed. Out-of-focus regions are NOT computed.

**Honest caveat:** Lazy eval only helps if you skip weights entirely (MoE routing). For dense inference, all weights are read. See §11.7.

---

## 8. Negative Side (Inside-Out)

- Positive side: 42 regions (0..41)
- Negative side: 42 regions (-1..-42) [inside-out]
- Total: 84 regions (two-sided)
- Inside-out: pos24 - neg24 = 0 (closed loop)
- Balanced ternary: covers both sides [-1,0,1]

---

## 9. Server (tesspack_server)

### 9.1 Before (Flat Buffer)

```
GGUF → calloc(5.4 GB) → scatter all capos → flat buffer → llama.cpp
       ↑ OOM on <8 GB RAM
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

**Note on OOM fix:** The OOM was fixed by removing the flat buffer allocation (zero-copy multi-cube), NOT by changing the allocator. `aligned_alloc(32)` still allocates the same amount of memory — it just ensures 32-byte alignment for TENSOR_ALIGNMENT. The memory reduction comes from not allocating a 5.4 GB flat buffer.

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
| minicpm5-1b | 219 | 23.1s | ❌ Garbage |
| bonsai-4b-q1_0 | 145 | 0.3s | ❌ Garbage |

---

## 11. Critical Review & Open Questions

### 11.1 Load Time Contradiction (RESOLVED)

**Issue:** STATUS report said 27.5s, TECHNICAL said 1.9s.

**Resolution:** 27.5s was from the diagnostic build (slow capo_count scanning + source GGUF fallback for 141 tensors). 1.9s was from the wired version (ONION path + resolver, no fallback scanning). The correct number is **1.9s** (wired version). The 27.5s was a diagnostic artifact.

### 11.2 Why Stride-37? (Honest Answer)

**Question:** If the goal is zero-copy access, mmap alone gives zero-copy. Why permute with stride-37?

**Answer:** Stride-37 does NOT improve read performance. In fact, it hurts cache locality for sequential reads (non-contiguous access pattern → cache misses → slower inference).

**What stride-37 gives:**
1. **Deterministic addressing** — no hash, no collision, no lookup table. cell = (slot × 37) % 20736.
2. **8x view derivation** — mirror formula derives 8 octant views from 1 cube. Without stride-37 (or similar bijection), views would need separate storage.
3. **Climate shift (O(1) window move)** — RELABEL changes the offset record, not the data. Stride-37 ensures the shift is a bijection.
4. **Geometric structure** — the 20736-cell space has mathematical properties (divisibility, symmetry) that enable the hyperbolic mirror and cascade.

**What stride-37 does NOT give:**
- Read performance (sequential reads are slower due to scatter)
- Compression (MAP not COMPRESS — size is unchanged)
- Cache efficiency (non-contiguous access)

**Honest assessment:** Stride-37 is a design choice for geometric structure, not an engineering optimization for read throughput. The system trades read performance for deterministic addressing and view derivation. Whether this trade-off is worth it depends on the use case (storage efficiency vs inference speed).

**Open question:** Should we provide a contiguous "fast path" for inference while keeping stride-37 for storage? This would require a resolver that de-scatters at load time (Gather) — which the user explicitly said is wrong. This tension needs resolution.

### 11.3 OOM Fix (Clarified)

**Issue:** Document claimed aligned_alloc(32) fixed OOM. This is wrong.

**Clarification:** OOM was fixed by **removing the flat buffer allocation** (zero-copy multi-cube), NOT by changing the allocator. `aligned_alloc(32)` still allocates the same amount of memory — it just ensures 32-byte alignment for TENSOR_ALIGNMENT. The memory reduction comes from not allocating a 5.4 GB flat buffer.

**Correct statement:** "OOM fixed by eliminating flat buffer allocation. aligned_alloc ensures TENSOR_ALIGNMENT compliance."

### 11.4 Garbage Output (Open Question)

**Issue:** 2 of 3 models produce garbage (minicpm5-1b, bonsai-4b-q1_0). Document blamed chat template.

**More likely causes:**
1. **Non-weight tensors permuted** — norm weights, rope freqs, bias tensors should NOT be permuted (they're read sequentially by llama.cpp). The current server permutes ALL tensors. This could cause garbage.
2. **Q1_0 block layout** — bonsai is Q1_0, which has a different block size than Q8_0 (34 bytes). The resolver uses `cell_size_of()` which may not handle Q1_0 correctly.
3. **MoE expert tensors** — MoE routing may tolerate permutation (gather-based), but dense tensors don't.

**Recommended test:** Resolve only `token_embd.weight` (not all tensors) and compare output. If still garbage → data path issue. If coherent → permutation of non-weight tensors is the cause.

**Status:** NOT YET TESTED. User directive: "LFM ไว้หลังสุด" (LFM last). minicpm/bonsai are smaller models but same issue.

### 11.5 Sacred Numbers (Justified vs Design Choice)

See §2 for full table. Summary:
- **Justified:** 37, 42, 48, 8, 3, 4, 34
- **Design choice:** 12, 111, 1440, 162, 1728, 144

**Recommendation:** Document 12 as "chosen for divisibility" (not "mathematically necessary"). Document 111 as "chosen for palindrome property" (aesthetic, not engineering).

### 11.6 Jump Module Over-Engineering (Open Question)

**Issue:** 6 layers (geo_jump → hyper_jump → frame_seek → fan24 → jet → 144×144) and 10 jump types, but no benchmark comparing to flat path.

**Status:** Jump module is implemented and tested (35/35 PASS) but NOT wired into server. Whether it provides engineering benefit (not just aesthetic) is unproven.

**Recommendation:** Wire resolver into server first (simpler), benchmark, then evaluate if jump module adds value.

### 11.7 Lazy Eval / Field Focus (Honest Assessment)

**Issue:** "23/42 regions never computed" sounds good, but doesn't help llama.cpp inference speed.

**Analysis:** Lazy eval only helps if you skip weights entirely (MoE routing). For dense inference, all weights are read by llama.cpp in every forward pass. The "never computed" regions are out-of-focus, but llama.cpp still needs them.

**When lazy eval helps:**
- MoE routing (only fired experts are read)
- Sparse attention (only attended tokens)
- Dynamic pruning (skip low-importance weights)

**When lazy eval doesn't help:**
- Dense FFN (all weights read)
- Full attention (all K/V read)
- Normalization (all weights read)

**Status:** Concept proven (19/19 PASS) but performance benefit unproven. Needs MoE integration to demonstrate.

---

## 12. File Reference

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
| `tests/test_cube_wire.c` | 2 (tensors) | Zero-copy multi-cube resolve vs GGUF |
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

*Document updated by OpenCode agent on 2026-11-22 (v1.1 — added critical review)*