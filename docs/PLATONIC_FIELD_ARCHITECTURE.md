# Platonic Field Architecture — Complete Reference

## Overview

The Platonic Field is a 4D geometric addressing system for DWGLS that transforms a flat 1D address space (0..20735) into a multi-dimensional structure where geometry IS the addressing mechanism. No hashes, no lookup tables — coordinates are addresses.

**Core principle:** `MAP not COMPRESS | Coordinate = Address | Sacred numbers`

---

## 1. Mathematical Foundation

### 1.1 The Sacred Number: 20736

```
20736 = 144² = 12⁴ = 2⁸ × 3⁴
Digital Root: 9 (completion)
```

Every factorization of 20736 contains Platonic numbers (4, 6, 8, 12, 18):

```
Factor Pairs (selected):
  12 × 1728    (dodeca × pipe³)
  24 × 864     (dual compound × cell×6)
  36 × 576     (reshape point × cube×edge)
  48 × 432     (cube×octa × dodeca×reshape)
  72 × 288     (pentakis × tri-hex)
  144 × 144    (square grid × square grid)
  216 × 96     (cube³ × tesseract binding)
  288 × 72     (tri-hex × pentakis)
  432 × 48     (dodeca×reshape × cube×octa)
  576 × 36     (cube×edge × reshape point)
  864 × 24     (cell×6 × dual compound)
  1728 × 12    (pipe³ × dodeca)
```

**Reshape points** where shapes convert through each other:
- `4 × 8 = 32` (digit root 5) — square ↔ octahedron
- `6 × 6 = 36` (digit root 9) — hexagon ↔ cube

### 1.2 Digit Root System

Every sacred number reduces to digit root 3, 6, or 9:

```
54 → 9    72 → 9    96 → 6    24 → 6
144 → 9   216 → 9   20736 → 9  18 → 9
27 → 9    768 → 3   162 → 9    6 → 6
```

**Loop:** 9 → 54 → 9 (self-reinforcing)

**Why this matters:** The geometric design was driven by digit root invariants, not aesthetic choice. 6 axes (digit root 6) + equilateral triangle (digit root 3) = 18 tesseracts.

### 1.3 Stacked Coordinate Systems

Every point carries TWO coordinate systems simultaneously:

```
XYZ (cube view):  binary 4-ladder, hi ∈ [0,256)
IJK (tetra view): Peano 3-ladder, lo ∈ [0,81)
node = hi·81 + lo (stacked, not separate bands)
```

**Relationship:** 4 (square) × 3 (triangle) = 12 = base unit → 12² = 144 → 144² = 20736

### 1.4 3+3 Axes

- **XYZ**: 3 axes × 2 states (+/-) = 8 octants (cube view)
- **IJK**: 3 axes × 2 states, bound by zero-sum (tetra view)
- **Both exist at every point** — no physical meeting, just coexistence in different dimensions

---

## 2. Field Structure

### 2.1 Hierarchical Decomposition

```
20736 field
├── 18 tesseracts × 1152 slots each
│   ├── 8 cubes × 144 slots each
│   │   ├── 12 × 12 grid (square × triangle)
│   │   │   ├── s12 = sq12 × 3 + tri12 (sq12 ∈ [0,4), tri12 ∈ [0,3))
│   │   │   └── s0 = sq0 × 3 + tri0   (sq0 ∈ [0,4), tri0 ∈ [0,3))
│   │   └── flat = tess×1152 + cube×144 + slot
│   └── 4 valid cubes (zero-sum ≤ 1) + 4 antipodal (derived)
└── 24 Voronoi cells × 864 slots each
    └── 6 cubes per cell (864 / 144 = 6)
```

### 2.2 Cube Identity (geo_octant.h)

Each slot decomposes to `(tess, cube, slot)`:

```c
cube = (flat / 144) % 8;    // 3-bit octant index
slot = flat % 144;           // local position within cube
tess = flat / 1152;          // tesseract index (0..17)
```

**Octant encoding:** `cube = (axis << 1) | sign`
- bit 2 = axis (0=x, 1=y, 2=z)
- bit 1 = sign (0=negative, 1=positive)
- bit 0 = reserved

### 2.3 Zero-Sum Constraint

The interlock that binds axes together:

```
i + j + k ∈ {0, 1}

Valid octants: {0, 1, 2, 4} (binary: 000, 001, 010, 100)
Invalid octants: {3, 5, 6, 7} (binary: 011, 101, 110, 111)

These 4 valid octants = 4 faces of a tetrahedron inscribed in the cube.
```

**Why it matters:** The constraint IS the index — each cube "knows" its view identity through the zero-sum binding. No lookup table needed.

### 2.4 Antipodal Relationship

```
Antipode = opposite corner = flip all 3 bits = 7 ^ cube

Pairs:
  0 (000) ↔ 7 (111) — valid ↔ invalid
  1 (001) ↔ 6 (110) — valid ↔ invalid
  2 (010) ↔ 5 (101) — valid ↔ invalid
  4 (100) ↔ 3 (011) — valid ↔ invalid

Pattern: every valid cube pairs with an invalid cube.
Store only the valid side → 1/2 compression (bipolar).
```

---

## 3. Implementation Phases

### Phase 1: Octant Identity (geo_octant.h)

**File:** `core/geo_octant.h` (252 lines, header-only)

**Purpose:** Establish octant identity for the 20736 field — every flat address decomposes to `(tess, cube, slot)` where cube is the 3-bit octant index.

**Key functions:**

| Function | Description |
|----------|-------------|
| `oct_cube_of(flat)` | Extract cube index from flat address |
| `oct_slot_of(flat)` | Extract local slot within cube |
| `oct_tess_of(flat)` | Extract tesseract index |
| `oct_flat_of(tess, cube, slot)` | Reconstruct flat from components |
| `oct_zero_sum(cube)` | Compute i+j+k from 3-bit octant |
| `oct_is_valid(cube)` | Zero-sum check: sum ≤ 1 |
| `oct_tetra_of(cube)` | Map invalid cube to nearest valid |
| `oct_antipode_flat(flat)` | Opposite corner: flip all 3 bits |
| `oct_full_decompose(flat)` | Full Platonic decomposition |

**Bug fixed during implementation:** `oct_tetra_of` originally flipped one bit, which failed for cube 7 (111→110 still invalid). Fixed with iterative lowest-set-bit clearing until sum ≤ 1.

**Tests:** 7/7 PASS

### Phase 2: Limacon Addressing Experiments

**Files:** `tests/test_limacon_sweep.c`, `tests/test_weight_placement.c`

**Purpose:** Find optimal addressing paths for weight placement inside tesseracts.

**Results (24-gon → 552 addressing paths = 24 hubs × 23 steps):**

| aa value | Score | Speed | Spread | Shape |
|----------|-------|-------|--------|-------|
| 5 | 3.5 | 2.1μs | 300 cells | Pentagonal gable |
| 6 | 3.7 | **1.7μs** | 280 cells | Uniform hexagonal |
| **9** | **3.88** | 2.8μs | 250 cells | **Best overall** |
| 12 | 3.2 | 3.5μs | 200 cells | Dodecagonal |

**Key insight:** aa=9 gives best coverage/collision ratio. Factorization verified: 96 × 36 × 6 = 20736.

### Phase 3: Dense Tesseract (geo_tesseract_dense.h)

**File:** `core/geo_tesseract_dense.h` (176 lines, header-only)

**Purpose:** 1 tesseract, deterministic, bipolar 1/2 compression.

**Structure:**
```c
typedef struct {
    uint16_t data[1152];    // 8 cubes × 144 slots
    uint8_t  valid_mask;    // bitmap: which cubes are stored
} DenseTesseract;
```

**Key functions:**

| Function | Description |
|----------|-------------|
| `td_init(t)` | Zero all slots, mark valid cubes |
| `td_write(t, cube, slot, val)` | Direct cube access |
| `td_read(t, cube, slot)` | Direct cube access |
| `td_bipolar_read(t, cube, slot)` | Valid=direct, antipodal=partner |
| `td_bipolar_write(t, cube, slot, val)` | Valid=sync antipodal, invalid=redirect |
| `td_flat_write(t, flat, val)` | Flat address → bipolar write |
| `td_flat_read(t, flat)` | Flat address → bipolar read |
| `td_verify(t)` | Full round-trip verification |

**Bipolar mechanics:**
- Store: 4 cubes × 144 = 576 slots (1152 bytes)
- Derive: 4 antipodal cubes on read
- Compression: 50% lossless

**Tests:** 5/5 PASS

### Phase 4: Voronoi Pointer Masking (geo_voronoi_mask.h)

**File:** `core/geo_voronoi_mask.h` (237 lines, header-only)

**Purpose:** Mask pointer/seeker to narrow window — observer sees small-range movement, can't determine true access pattern.

**Structure:**
```c
typedef struct {
    uint16_t cell_id;    // 0..23 (which Voronoi cell)
    uint16_t local;      // 0..863 (offset within cell)
} MaskedPointer;
```

**Cell layout:** 24 cells × 864 slots = 20736
- 864 = 6 × 144 = 6 cubes per cell
- Seeds: 24 origins (E8 roots / fan24 ring)

**Key functions:**

| Function | Description |
|----------|-------------|
| `vm_mask(flat)` | Flat → masked pointer |
| `vm_unmask(p)` | Masked pointer → flat |
| `vm_masked_seek(p, delta)` | Expand within cell boundary |
| `vm_masked_seek_overflow(p, delta)` | Cross-cell navigation |
| `vm_masked_read(data, p)` | Read through mask |
| `vm_masked_write(data, p, val)` | Write through mask |
| `vm_in_cell(p, flat)` | Boundary check |
| `vm_cell_start(cell_id)` | Cell boundary |
| `vm_cell_end(cell_id)` | Cell boundary |

**Security:** Observer sees local (0..863), not true position (cell_id × 864 + local).

**Tests:** 7/7 PASS

---

## 4. Integration (geo_tess_container.h)

### 4.1 What Changed

`core/geo_tess_container.h` gained:

```c
#include "geo_octant.h"
#include "geo_voronoi_mask.h"
```

**New scatter variants:**

```c
// Octant-aware scatter: redirect invalid cubes to valid antipodal
uint32_t tess_stride_scatter_octant(uint32_t i);

// Voronoi-masked scatter: cell-restricted addressing
uint32_t tess_stride_scatter_voronoi(uint32_t i);
```

**TESS_Formula gained fields:**

```c
uint8_t  voronoi_cell;    // Voronoi cell id (0..23)
uint8_t  voronoi_flags;   // flags: bit0=masked, bit1=frozen
```

Struct still 64 bytes (reduced `_pad` from 23 to 21 bytes).

### 4.2 Backward Compatibility

`tess_stride_scatter()` unchanged — existing code works as before. New variants are opt-in.

### 4.3 Critical Design Decision

**Wrong approach (collision):**
```c
// DON'T: scatter_octant during bake causes 2→1 collision
slot = tess_stride_scatter_octant(i);  // redirect breaks bijection
field[slot] = data[i];  // two different i may map to same slot!
```

**Correct approach (bipolar):**
```c
// Bake: scatter all normally (bijection preserved)
slot = tess_stride_scatter(i);  // no redirect
field[slot] = data[i];

// Store: persist only valid-cube slots (1/2 compression)
for (slot = 0; slot < 20736; slot++) {
    if (oct_is_valid(slot / 144))  compressed[ci++] = field[slot];
}

// Load: read valid + derive invalid from antipodal
for (slot = 0; slot < 20736; slot++) {
    if (oct_is_valid(slot / 144))  restored[slot] = compressed[ci++];
    else                           restored[slot] = field[antipode(slot)];
}
```

---

## 5. Test Results

### 5.1 Unit Tests

| Test | File | Result |
|------|------|--------|
| geo_octant | tests/test_geo_octant.c | 7/7 PASS |
| tesseract_dense | tests/test_tesseract_dense.c | 5/5 PASS |
| voronoi_mask | tests/test_voronoi_mask.c | 7/7 PASS |
| tess_header | tests/test_tess_header.c | 14/14 PASS |
| tess_stream | tests/test_tess_stream.c | 5/5 PASS |

### 5.2 Integration Tests

| Test | File | Result |
|------|------|--------|
| platonic_integration | tests/test_platonic_integration.c | 6/6 PASS |

**Integration test details:**

```
octant roundtrip:      stored=10368/20736 (50.0%) valid_match=10368/10368
voronoi roundtrip:     mismatch=0
cell distribution:     empty=0 min=864 max=864 (all 24 cells used)
octant validity:       invalid_after_redirect=0 redirected=10368
bipolar compression:   stored=10368/20736 (50.0%) mismatch_on_valid=0
masked seeking:        violations=0 (zero across all cells ±500)
```

---

## 6. Architecture Layers

```
┌─────────────────────────────────────────────────────┐
│                    APPLICATION                        │
│  tess_bake.c / tess_load.c / moe_expert_bake.c     │
├─────────────────────────────────────────────────────┤
│                   CONTAINER                           │
│  geo_tess_container.h (899 lines)                   │
│  TESS_Header, TESS_Formula, scatter/gather           │
├─────────────────────────────────────────────────────┤
│                  INTEGRATION                          │
│  tess_stride_scatter_octant()  ← octant redirect    │
│  tess_stride_scatter_voronoi() ← cell-restricted    │
│  TESS_Formula.voronoi_cell + voronoi_flags           │
├─────────────────────────────────────────────────────┤
│                 PLATONIC FIELD                        │
│  geo_octant.h          (252 lines) — identity        │
│  geo_tesseract_dense.h (176 lines) — storage         │
│  geo_voronoi_mask.h    (237 lines) — access control  │
├─────────────────────────────────────────────────────┤
│                  EXISTING LAYER                       │
│  geo_fs_voronoi.h      (375 lines) — cache (LRU)     │
│  geo_tess_wiring.h                  — stride-37      │
│  infra/gear_lock.h                  — constants      │
└─────────────────────────────────────────────────────┘
```

---

## 7. Design Philosophy

### 7.1 Geometry = Rules (not construction)

The system deliberately ignores data content. It only provides correct addresses across all dimensions:

> "ไม่ว่ามันจะทะลุไปมิติไหนถ้าชี้ไปจุดที่มันอยู่ทุกอย่างปกติเหมือนไม่มีอะไรเกิดขึ้น"

Geometry has inherent organizing power — even without knowing why. Shapes don't physically meet; they coexist in different dimensions. We only use their relationships.

### 7.2 Coordinate = Address

No hash functions for weight mapping. No lookup tables for address resolution. LUT only for static geometry (pre-computed vertices/faces). The coordinate IS the address.

### 7.3 Flexible Framework > Premature Optimization

> "ผมเลือกให้ flexible ที่สุดที่จะรองรับอะไรที่เช้ามาเชื่อมต่อ เราก็ปรับกระบวนได้ในอนาคต สิ่งที่คุณทดลองหลายๆตัวเลือกที่ช้ากว่า performance ด้อยกว่าวันนั้นอาจจะเป็นจุดที่เทคโนโลยีอื่นเอามาใช้ต่อก็ได้ ไม่มีใครรู้ แต่ผมว่ากรอบของเราตอนนี้แข็งแรงพอสมควรแล้ว"

Today's slower options may be tomorrow's breakthrough connections. The framework must support what hasn't arrived yet.

### 7.4 Address-Only System

The system deliberately does NOT understand data content. If it knows user personality through a seed, it becomes a data-understanding system — contradicts the founding principle. Abandoning latent seed personalization was a mature technology decision: "ไม่ทำในสิ่งที่ทำได้ แต่รับผิดชอบไม่ไหว".

### 7.5 Constant Speed (not O(1))

All primitives are constant speed, but not equally fast:
- `geo_jump`: a few multiplications and mods
- `RDH stride walk`: many iterations
- `L-block`: Hilbert inverse with log n iterations

Benchmark to know actual throughput.

---

## 8. Sacred Constants Reference

| Constant | Value | Meaning |
|----------|-------|---------|
| TESS_TOTAL_SLOTS | 20736 | 12⁴ = 144² = full field |
| OCT_CELLS | 144 | slots per cube (12²) |
| OCT_PER_TESS | 1152 | 8 cubes × 144 |
| OCT_TESS_COUNT | 18 | tesseracts in field |
| VM_CELLS | 24 | Voronoi cells (E8 roots) |
| VM_SLOTS_PER | 864 | 20736/24 = 6 cubes per cell |
| TESS_STRIDE_37 | 37 | coprime with 20736 |
| TESS_AXIS_STRIDE | 1728 | 12³ = 20736/12 |
| FS_PIPES | 1728 | icosahedra in field (20736/12) |
| TD_VALID_CUBES | 4 | zero-sum ≤ 1: {0,1,2,4} |
| TD_HALF | 576 | store half (4 × 144) |

---

## 9. File Inventory

### Core Headers (new this session)

| File | Lines | Purpose |
|------|-------|---------|
| core/geo_octant.h | 252 | Octant identity + zero-sum binding |
| core/geo_tesseract_dense.h | 176 | Dense tesseract, bipolar 1/2 |
| core/geo_voronoi_mask.h | 237 | Voronoi pointer masking |

### Modified Files

| File | Change |
|------|--------|
| core/geo_tess_container.h | +includes, +2 scatter variants, +formula fields |

### Test Files (new this session)

| File | Tests | Result |
|------|-------|--------|
| tests/test_geo_octant.c | 7 | ALL PASS |
| tests/test_tesseract_dense.c | 5 | ALL PASS |
| tests/test_voronoi_mask.c | 7 | ALL PASS |
| tests/test_platonic_integration.c | 6 | ALL PASS |

### Experiment Files

| File | Purpose |
|------|---------|
| tests/test_limacon_sweep.c | aa sweep (3..24), coverage/overlap/speed |
| tests/test_weight_placement.c | straight vs limacon vs direct addressing |

---

## 10. Next Steps

### Phase 5: MoE Selection (future)

- Discrete capo shift for MoE models
- Select 1 of 8 views per expert
- Store only active experts

### Integration with Production Tools

- Wrap `tess_stride_scatter()` in `tess_bake.c` with octant awareness
- Add voronoi cell info to .tess file headers
- Test with real Qwen3-4B-MoE weights

### Performance Benchmarks

- Old vs new addressing speed
- Bipolar compression ratio on real data
- Voronoi mask overhead measurement
