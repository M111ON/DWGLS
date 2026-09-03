# DWGLS Primitives Reference

> **Scanned:** 2026-09-01 | **Source:** `core/` headers (160+ files) | **Branch:** `feat/geo-native-fs`
> **Philosophy:** *MAP not COMPRESS* — Geometry IS the address space. Coordinate = data. No hash, no collision, no lookup table.

---

## 1 — Sacred Numbers (Immutable Constants)

These numbers are **frozen** — every subsystem derives from them. Breaking any = system-wide failure.

| Constant | Value | Derivation | Where Defined |
|---|---|---|---|
| **GEAR_GEO_FULL** | **20736** | 128 × 162 = 12⁴ = 144² = 1728 × 12 | `infra/gear_lock.h` |
| GEAR_CPU_WORLD | 128 | | `infra/gear_lock.h` |
| GEAR_GPU_WORLD | 162 | | `infra/gear_lock.h` |
| GEAR_C144_CYCLE | 144 | | `infra/gear_lock.h` |
| **FS_PIPES** | **1728** | 12 × 144 | `infra/fibo_spine.h` |
| **FS_TICKS** | **12** | 0..11 | `infra/fibo_spine.h` |
| FS_SLOTS | 20736 | FS_PIPES × FS_TICKS | `infra/fibo_spine.h` |
| TESS_TOTAL | 20736 | 18 × 1152 = 144 × 144 | `geo_tesseract_addr.h` |
| TESS_PER_TESS | 1152 | 8 × 144 | `geo_tesseract_addr.h` |
| TESS_CUBES | 8 | tesseract cells | `geo_tess_wiring.h` |
| TESS_CELLS | 144 | | `geo_tess_wiring.h` |
| TESS_N_TESS | 18 | 18 tesseracts (★ protagonist) | `geo_tess_wiring.h` |
| GEO_FULL_N | 3456 | 6 × 9 × 64 = 144 × 24 | `infra/geo_config.h` |
| GEO_SPOKES | 6 | 60° each | `infra/geo_config.h` |
| GEO_SLOTS | 576 | 9 × 64 = 24² | `infra/geo_config.h` |
| FRAME_CYCLE | 1440 | timeline cycle | `geo_frame_seek.h` |
| FRAME_STRIDE | 37 | coprime with 1440 | `geo_frame_seek.h` |
| FRAME_EDGES | 12 | 9 Hilbert + 3 Peano | `geo_frame_seek.h` |
| TRING_WALK_CYCLE | 720 | 6 spokes × 120 | `geo_tring_walk.h` |
| TRING_WALK_STRIDE | 37 | prime, gcd(37,720)=1 | `geo_tring_walk.h` |
| DRAM_FULL | 20736 | 162 × 128 | `geo_dram_tile.h` |

### Derivation Chain

```
12 (dodeca base)
 ├─ 12 × 12 = 144 (cycle)
 │    ├─ 144 × 144 = 20736 (GEO_FULL)
 │    │    ├─ 1728 × 12 = 20736 (Fibo Spine)
 │    │    ├─ 18 × 1152 = 20736 (Tesseract field)
 │    │    └─ 162 × 128 = 20736 (DRAM Tile)
 │    └─ 144 × 10 = 1440 (FRAME_CYCLE)
 ├─ 128 (CPU world)
 └─ 162 (GPU world / ico nodes)
```

---

## 2 — Geometry Primitives

### 2.1 GeoType Enum (Parametrized Grid)

> `core/geo_param_grid.h` — One family: Dodeca Root → all shapes derive from same parent.

| GeoType | verts | edges | faces | cells | Notes |
|---|---|---|---|---|---|
| GEO_DODEC_BASE | 20 | 30 | 12 | 1 | dodecahedron (root) |
| GEO_ICO_BASE | 20 | 30 | 20 | 1 | icosahedron (dual) |
| GEO_COMPOUND_24 | 24 | 48 | 24 | 6 | inverted dodeca compound |
| GEO_DODEC_EDGES | 30 | 60 | 32 | 1 | edge-based |
| GEO_COMPOUND_60 | 60 | 90 | 32 | 1 | pentakis dodeca |
| GEO_PENTAKIS_72 | 72 | 90 | 32 | 1 | 12 base + 60 pyramids |
| GEO_GOLDBERG_92 | 92 | 270 | 92 | 1 | goldberg dual |
| GEO_COMP_SPIKE_120 | 120 | 180 | 62 | 1 | spike compound |
| GEO_GOLDBERG_132 | 132 | 270 | 92 | 1 | goldberg level 2 |
| **GEO_COMPOUND_144** | **144** | **576** | **576** | **144** | **★ 6ico = 18tes** |
| GEO_GOLDBERG_192 | 192 | 270 | 92 | 1 | goldberg level 3 |

**★ GEO_COMPOUND_144** = V=144 · E=576 · F=576 · C=144 — the protagonist for KIS-timeline.

### 2.2 Cube-in-Dodecahedron

> `core/geo_cube_in_dodeca.h`

| Symbol | Value | Meaning |
|---|---|---|
| PHI | 1.6180339887... | golden ratio φ = (1+√5)/2 |
| DODECA_VERTS | 20 | dodecahedron vertices |
| CUBE_VERTS | 8 | cube vertices (subset of 20) |
| CUBE_AXES | 3 | X, Y, Z |
| CUBE_HALF_AXES | 6 | 3 axes × 2 signs |
| CellType | 8 | 2³ parity combinations: III, IID, IDI, IDD, DII, DID, DDI, DDD |

- Cube edge = φ × pentagon edge
- 5 cubes compound inside dodecahedron
- 3-bit parity: `(gen%2, face%2, slot%2)` → cell type 0..7

### 2.3 DRam Tile

> `core/geo_dram_tile.h` — Zero-copy geometry addressing. Sub-10ns per call.

| Constant | Value | Notes |
|---|---|---|
| DRAM_GRID_X | 8 | |
| DRAM_GRID_Y | 8 | |
| DRAM_LAYERS | 2 | base + phase-flip |
| DRAM_CELLS_PER | 128 | 8 × 8 × 2 |
| DRAM_ANCHORS | 162 | ico nodes (81 × 2 poles) |
| DRAM_FULL | 20736 | 162 × 128 |

**Core formula:** `dram_addr = anchor_id × 128 + hilbert_8x8(x, y, layer)`

- Hilbert curve on 8×8 grid: pure bit-interleave, O(log n) = 3 iterations
- 162 × 128 = 20736 = GEO_FULL ✓

### 2.4 Goldberg Decagram Layout

> `core/geo_goldberg_decagram.h`

| Constant | Value | Notes |
|---|---|---|
| GGD_SECTORS | 10 | decagram: 10 × 36° = 360° |
| GGD_PENTAGONS | 12 | Euler-fixed anchors |
| hex per sector | n²−1 | EXACT — the decagram fact |
| hex total | 10(n²−1) | |
| total faces | 10n² + 2 | |

- Pentagon pair (f, f+6): f on pole 0 (ring1), f+6 on pole 1 (ring2)
- Opposite decagram direction = inverted half (d+5 mod 10)

---

## 3 — Addressing Primitives

### 3.1 Tesseract Addressing

> `core/geo_tesseract_addr.h` — Fixed-frame 4D addressing (no camera move)

| Constant | Value | Notes |
|---|---|---|
| TESS_AXES | 4 | |
| TESS_3D_CELLS | 8 | 3D cells in a tesseract |
| TESS_SLOTS | 144 | per cell |
| TESS_PER_TESS | 1152 | 8 × 144 |
| TESS_COUNT | 18 | tesseracts |
| TESS_GEO_FULL | 20736 | 18 × 1152 |

**Key functions:**
- `tess_index(axis, sign)` → idx 0..7 (3 bits: axis<<1 | sign)
- `tess_flat(tess, cell, slot)` → flat address in [0, 20736)
- `tess_unflat(flat)` → (tess, cell, slot)
- `tess_neighbor_xor(idx, k)` → bit-flip graph (degree 3)
- `tess_adjacent(idx, out[6])` → true tesseract adjacency (degree 6)

### 3.2 Tess Wiring (Rescope ↔ Physical)

> `core/geo_tess_wiring.h`

| Mapping | Formula |
|---|---|
| Rescope → flat | `flat = tess×1152 + cube×144 + slot` |
| Flat → Rescope | `tess = flat / 1152, cube = (flat%1152)/144, slot = flat%144` |
| Scale seek | `scaled = (base + w×37) % 20736` |
| Magnify glass center | w = 72 (center of 144) |
| Magnify glass radius | w = 36 (144/4) |
| Antipode | `(w + 72) % 144` |

### 3.3 Cube Addressing (Generation-Indexed)

> `core/geo_cube_addr.h`

Address = (generation n, face 0-5, slot) — like floating-point = (exponent, mantissa)

| Concept | Function |
|---|---|
| w(time) | temporal position indicator (default 1.0) |
| w_scale(n, w) | `gen_scale(n) × w_time` |
| gen_scale(n) | φⁿ (golden ratio power) |
| slots_per_face(n) | round(φⁿ) |
| geo_cube_addr_to_flat | maps (gen, face, slot) → [0, 20736) |

### 3.4 Cell Addressing (Rail Hub)

> `core/geo_cell_addr.h` — Pure O(1) between GeoTensorHub and Fibo Spine

**Flat cell id (14-bit):**
```
bits 0..2  (3 bits) → generation  (0..7)
bits 3..5  (3 bits) → face         (0..7, only 6 used)
bits 6..13 (8 bits) → slot         (0..255)
```

**Zero-copy guarantee:** cell N in cube space = tick-(N/1728) on pipe (N%1728) — same physical byte.

### 3.5 MoE Expert Addressing

> `core/moe_expert_addr.h` — Pure integer O(1) mapping

| Constant | Value | Notes |
|---|---|---|
| MOE_MAX_LAYERS | 64 | |
| MOE_MAX_EXPERTS | 64 | per layer |
| MOE_WEIGHT_TYPES | 3 | gate, up, down |
| MOE_MAX_FLAT | 20736 | |

**Address:** `(layer, expert_num, weight_type)` → flat ∈ [0, 20736)

```
flat = (layer × 64 × 3 + expert × 3 + wtype) % 20736
```

- Geometry: `tess_to_flat(flat/1152, (flat%1152)/144, flat%144)`
- Disk offset: `flat × BLOCK_SIZE`
- Capacity: up to 6912 experts (20736 / 3 weight types)

---

## 4 — Codec Primitives

### 4.1 Geo Parametric Codec

> `core/geo_param_grid.h` — Sort → distinct count → codebook size

```
Encode: sort weights → codebook (distinct values) → idx-stream
Decode: read idx-stream → lookup value → reconstruct
```

| Component | Notes |
|---|---|
| Codebook | sorted unique floats |
| Idx stream | binary search → codebook index |
| Mask | bit per vertex (which slots used) |
| GEO_MAX_DISTINCT | 16M (1<<24) |

### 4.2 KIS Codec v4 (Lossless, Proven on Real GGUF)

> `core/kis_codec_v4.h`

**Two-layer architecture:**
1. **Layer 1 — Codebook:** active bitmap + RLE counts (~550B for ANY model)
2. **Layer 2 — Position Permutation:** delta encoding + varint

```
Encode: weights → codebook + permutation
Decode: codebook → sorted_values → permutation → original_positions → output
```

| Magic | Format |
|---|---|
| KCV4 (0x4B435634) | Full codec header |
| PERM (0x5045524D) | Permutation stream |

### 4.3 DRam Tile Addressing

> `core/geo_dram_tile.h`

```
dram_addr = anchor_id × 128 + hilbert_8x8(x, y, layer)
```

| Layer | Hilbert Range |
|---|---|
| layer 0 | 0..63 (base) |
| layer 1 | 64..127 (phase flip) |

---

## 5 — Timeline & Pipeline Primitives

### 5.1 Fibo Spine (1728 pipes × 12 ticks)

> `core/infra/fibo_spine.h` — The backbone pipeline

| Constant | Value | Notes |
|---|---|---|
| FS_PIPES | 1728 | 12 × 144 |
| FS_TICKS_PER_CYCLE | 12 | 0..11 |
| FS_SLOTS | 20736 | FS_PIPES × FS_TICKS |
| FS_JET_BRIDGE_TICK | 11 | exit tick |
| FS_REENTRY_TICK | 13 | mod 12 = 1 |
| FS_TICK_12 | 12 | barrier boundary (skipped) |

**Jet Bridge States:**
| State | Value | Meaning |
|---|---|---|
| JB_INACTIVE | 0 | normal tick |
| JB_ARMED | 1 | tick=10, will fire |
| JB_BRIDGING | 2 | tick=11 → entering residual |
| JB_RESIDENT | 3 | inside residual_space |
| JB_RETURNING | 4 | tick=13 → re-entering |

**Flow:** `∞ ← contraction ← 0 ← expansion → ∞` — enter ANYWHERE

### 5.2 Frame Seek (Stride-37 Fibo 1440)

> `core/geo_frame_seek.h`

| Constant | Value | Notes |
|---|---|---|
| FRAME_CYCLE | 1440 | |
| FRAME_STRIDE | 37 | coprime with 1440 |
| FRAME_EDGES | 12 | 9 Hilbert + 3 Peano |
| FRAME_H_ACTIVE | 9 | |
| FRAME_P_STEPS | 4 | |
| FRAME_ICO_NODES | 162 | |
| FRAME_PEANO_GRID | 81 | 3⁴ ternary |
| FRAME_MAX | 120 | |

**Key functions:**
- `frame_enc(t)` → `(t × 37) % 1440`
- `frame_next(enc)` → `(enc + 37) % 1440`
- `frame_cpair(enc)` → `(enc + 720) % 1440` (self-inverse)
- `frame_seek(t)` → DualFrame (Hilbert + Peano decomposition)

### 5.3 TRing Walk

> `core/geo_tring_walk.h` — tile index → TRing enc position

| Constant | Value | Notes |
|---|---|---|
| TRING_WALK_CYCLE | 720 | 6 spokes × 120 |
| TRING_WALK_STRIDE | 37 | prime, coprime to 720 |
| TRING_WALK_SPOKE_SZ | 120 | per spoke |
| TRING_WALK_SPOKES | 6 | |

```
enc(i) = (i × 37) % 720
spoke = enc / 120
polarity = (enc % 120 >= 60) ? 1 : 0
```

### 5.4 Walk Clock (Fibo Walk)

> `core/fibo_walk.h` — state = (seed, round, tick) — live route

- **Enter anywhere** — any (round, tick) is valid start
- Coverage: every chunk live at exactly 1 position (rq, rq%ticks)
- `fibo_walk_dist(a, b)` → forward ticks from a to b
- `fibo_walk_live(...)` → routes live at current (round, tick)

---

## 6 — Storage Primitives

### 6.1 Adaptive Storage Engine

> `core/geo_adaptive_store.h` — Tiered by entropy

| Tier | Entropy Score | Frames | Blocks | Size |
|---|---|---|---|---|
| 0 (structured) | 0-63 | 1 | 12 | 768B |
| 1 (moderate) | 64-127 | 3 | 36 | 2.3KB |
| 2 (high) | 128-191 | 7 | 84 | 5.4KB |
| 3 (random) | 192-255 | 27 | 324 | 20.7KB |

| Constant | Value | Notes |
|---|---|---|
| ADPT_BLOCK_WORDS | 64 | floats per DiamondBlock |
| ADPT_EDGES_PER_FRAME | 12 | 9 Hilbert + 3 Peano |
| ADPT_MAX_FRAMES | 27 | tier 3 worst case |
| ADPT_MAX_WEIGHTS | 20736 | |

### 6.2 KIS Container

> `core/geo_kis_container.h` — Binary format for adaptive-encoded weights

**Layout:** `Header[24B] + Payload[variable] + CRC[8B]`

| Field | Size | Notes |
|---|---|---|
| magic | 8B | `0x4B4953004B4953` ("KIS\0KIS") |
| version | 1B | 1 |
| tier | 1B | 0..3 |
| entropy | 1B | 0..255 |
| frame_cnt | 1B | frame slots |
| block_cnt | 2B | DiamondBlocks |
| weight_cnt | 4B | total weights |
| CRC-64 | 8B | ECMA-182 polynomial |

### 6.3 TESS Container (.tess file)

> `core/geo_tess_container.h` — Tesseract container format

**Header (64B):**
| Offset | Field | Value |
|---|---|---|
| 0 | magic | `0x54455353` ("TESS") |
| 4 | version | 1 |
| 8 | total_slots | 20736 |
| 12 | cell_size | varies by GGML type |
| 16 | scale_factor | fixed-point × 65536 |
| 20-28 | x/y/z_slots | 6912 each |
| 32 | gguf_type | quantization type |
| 36 | tensor_count | |
| 40-48 | source_size | original GGUF size |
| 48-56 | cube_checksum | CRC-64 |
| 56-64 | formula_id | hash of formula params |

**GGML → Cell Sizes:**
| Type | Cell Size |
|---|---|
| F32 | 4B |
| F16 | 2B |
| Q4_0 | 18B |
| Q4_K | 144B |
| Q5_K | 176B |
| Q6_K | 210B |
| Q8_0 | 34B |
| Q8_K | 292B |

**Stride-37 scatter:** `cell = (weight × 37) % 20736` — coprime, covers all slots.

### 6.4 Goldberg File (.ggf)

> `core/geo_goldberg_file.h` — Sphere persistence

**Layout:**
```
[GGFHeader 64B]  magic "GGF0" · version · level · n_spheres ·
                 n_chunks · n_bytes · crc32 · note
[sphere 0]       [count u32] → count × [tick u32][data 64B]
[sphere 1..n]
```

| Constant | Value |
|---|---|
| GGF_MAGIC | "GGF0" |
| GGF_VERSION | 1 |
| GGS_CHUNK | 64B |
| GGF_NOTE_LEN | 28B |

Three read modes:
1. **ggs_save/ggs_load** — streaming, verified write→verify→destroy
2. **GGFReader** — lazy seek (O(1) per node, no full load)
3. **GGFMap** — mmap zero-copy (pointer into page)

### 6.5 Residual Space (Bond-Only Storage)

> `core/residual_space.h` — Timeless zone, accessed only by bond_key

| Constant | Value | Notes |
|---|---|---|
| RS_DEFAULT_CAPACITY | 4096 | entry slots |
| RS_MAX_DATA_SIZE | 65536 | max bytes per entry |
| RS_BOND_KEY_RESERVED | 0 | never stored |

**Lifecycle:** FREEZE → THAW → EVICT → VERIFY

**Entry (36B header + data):**
| Field | Size | Notes |
|---|---|---|
| bond_key | 8B | lookup key (never 0) |
| origin_key | 8B | geo_key at birth |
| geo_key | 8B | original geo_key |
| data_size | 4B | payload bytes |
| timestamp | 4B | freeze timestamp |
| flags | 1B | RS_ENTRY_* |

### 6.6 Tring (Timeline Ring)

> `core/infra/tring.h` — Variable-size data indexed by tick

- **Node:** `[tick:8B][size:4B][_pad:4B][data[]]`
- Sparse pointer array indexed by tick
- GC via reference bitmap: O(capacity/64) words

---

## 7 — MoE Primitives

### 7.1 Expert Store

> `core/moe_expert_store.h` — Weight storage on DtSlotRegion

**Two modes:**
- **INLINE:** small weights stored directly in slot (demo/testing)
- **OFFSET:** slot stores `{offset, size}` → actual weights on disk (production)

**Meta (per wtype slot):**
| Field | Type | Notes |
|---|---|---|
| offset | uint32 | byte offset in backing file |
| size | uint32 | weight data size |
| quant_type | uint8 | 0=f32, 1=f16, 2=q8_0, 3=q4_0 |

### 7.2 Expert Bake/Graft/Stream Pipeline

| Tool | Make Target | Purpose |
|---|---|---|
| `moe_expert_bake.c` | `make moe-bake` | GGUF → DtSlotRegion (lossless weight pool) |
| `moe_expert_graft.c` | `make moe-graft` | DtSlotRegion → valid GGUF → inference |
| `moe_expert_stream.c` | `make moe-stream` | streaming top-K experts + Q4_K FFN matmul |

**Proven:** 108/108 tensors lossless from Qwen3-4B-MoE (2.8GB weight pool).

---

## 8 — GeoFS Primitives (Geometric Filesystem)

> `core/geofs_core.h` — POSIX filesystem on 20736 address space

| Constant | Value | Notes |
|---|---|---|
| GEOS_BLOCK_SZ | 64B | |
| GEOS_ADDR_SPACE | 20736 | |
| GEOS_MAX_INODES | 2048 | |
| GEOS_DATA_STORE_SIZE | ~1.3 MB | 20736 × 64B |
| GEOS_VOL_DATA_START | 256 | header(128) + dir(128) |

**Core operations:**
- `geos_create` / `geos_read` / `geos_write` / `geos_delete` — standard CRUD
- `geos_summon` / `geos_unsummon` — place/remove at geometric coordinate
- `geos_hyper_place` / `geos_hyper_read` — hyperbolic key-frame files

**Hyper files:** block addresses = `hw_at(&(HWRouter){seed, axis}, b)` — deterministic walk, no block list stored.

---

## 9 — Ghost Lift & Bond Primitives

### 9.1 Ghost Log Entry (5 bytes)

> `core/geo_ghost_lift.h`

| Field | Size | Notes |
|---|---|---|
| block_id | 2B | which pile |
| from_scale | 1B | birth scale w0 |
| to_scale | 1B | requested scale |
| flags | 1B | GHOST_FLAG_* |

### 9.2 Ghost Lift Flags

| Flag | Value | Meaning |
|---|---|---|
| GHOST_FLAG_LIFT | 0x01 | live lift (frozen + tracked) |
| GHOST_FLAG_EXPIRED | 0x02 | re-attached (audit trail) |
| GHOST_FLAG_DELTA | 0x04 | payload = delta blob |

### 9.3 Bond Key Mapping

```
origin_seed = rdh_addr(block_id, from_scale)  = block×256 + from
piece       = {geo_key: rdh_addr, shape: axis_shape, bond_L: a+1, bond_R: (a+1)<<24}
bond_key    = bond_L XOR bond_R
```

- `from_scale` is part of address → read with different from_scale → fail
- `to_scale` is NOT part of bond → same data reached by several routes

---

## 10 — Dual-World Placement

> `core/geo_dual_place.h` — Hilbert + Peano on 8×8 grid

| Concept | Value | Notes |
|---|---|---|
| GDP_GRID | 64 | 8×8 output grid |
| GDP_INNER | 36 | World B active (6×6 Peano) |
| GDP_BORDER | 28 | World A shadow (Hilbert border) |
| GDP_PEANO | 81 | 3⁴ ternary space |
| GDP_ICO | 162 | icosphere L2 (81×2) |

**Invariant:** 36 (inner) + 28 (border) = 64 (DiamondBlock) ✓

- World A (pole=0, Hilbert): features[0..63] → border 28 cells via stride-37 scatter
- World B (pole=1, Peano): features[81..116] → inner 36 cells

---

## 11 — Integrity & Verification Primitives

### CRC Primitives

| Function | Polynomial | Used By |
|---|---|---|
| CRC-64/ECMA | `0x42F0E1EBA9EA3693` | KIS container, TESS header |
| CRC-32 (zlib) | `0xEDB88320` | Adaptive store, .ggf files |

### Verification Functions

| Function | File | What It Checks |
|---|---|---|
| `geo_codec_verify()` | `geo_param_grid.h` | codec roundtrip binary truth |
| `geo_tesseract_verify()` | `geo_tesseract_addr.h` | flat/unflat + coverage |
| `geo_frame_seek_verify()` | `geo_frame_seek.h` | stride-37 full cycle on 1440 |
| `geo_dual_place_verify()` | `geo_dual_place.h` | LUT coverage + overlap + roundtrip |
| `geo_cube_in_dodeca_verify()` | `geo_cube_in_dodeca.h` | cube in dodeca + φ ratio |
| `verify_cell_classify()` | `geo_cell_classify.h` | all gen/face/slot → valid types |
| `dram_verify_hilbert()` | `geo_dram_tile.h` | Hilbert 8×8 covers 0..63 |
| `dram_verify_full()` | `geo_dram_tile.h` | full address space zero collisions |

---

## 12 — Hyperbolic Walk Primitives

> `core/geo_hyperbolic_store.h` + `core/geo_hyperbolic_walk.h`

| Constant | Axis | Stride | Orbit | Coverage |
|---|---|---|---|---|
| axis 0 | full | 1 | 20736 | full field |
| axis 1 | 9-stride | 9 | 2304 | coset |
| axis 2 | 81-stride | 81 | 256 | small files only |
| axis 3 | 27-stride | 27 | 768 | medium files |

**Key frame grid:** `HWFrames` — centroid at (aperture, depth), reconstruct any node by walking from nearest centroid.

---

## 13 — Dual-Place LUTs (Static Data)

> `core/geo_dual_place.h`

- **HILBERT_TO_GRID[64]:** `pos = (i × 37) % 64` — covers all 64 positions (gcd(37,64)=1)
- **PEANO_TO_GRID[81]:** 36 valid inner positions + 45 outside (0xFF)
- **BORDER_IDX[28]:** outer ring of 8×8 grid

---

## 14 — Key Data Structures Summary

| Struct | File | Purpose |
|---|---|---|
| `GeoType` | `geo_param_grid.h` | geometry family enum |
| `GeoProps` | `geo_param_grid.h` | verts/edges/faces/cells per type |
| `GeoCodec` | `geo_param_grid.h` | encode/decode context |
| `GeoCubeAddr` | `geo_cube_addr.h` | (generation, face, slot, w_time) |
| `GeoCellAddr` | `geo_cell_addr.h` | (generation, face, slot, cell_type) — uint-only |
| `FiboPipe` | `fibo_spine.h` | one pipe in the spine (12 ticks) |
| `FiboSpine` | `fibo_spine.h` | 1728 pipes + global state |
| `P5HRibcage` | `fibo_spine.h` | freeze wrapper (P5H integration) |
| `FiboWalkPos` | `fibo_walk.h` | walk clock position (round, tick, steps) |
| `FiboWalkRoute` | `fibo_walk.h` | chunk route (block, r0, rq) |
| `DualFrame` | `geo_frame_seek.h` | Hilbert + Peano decomposition of enc |
| `FrameRange` | `geo_frame_seek.h` | home frame + span for adaptive |
| `AdaptiveStore` | `geo_adaptive_store.h` | tiered weight storage |
| `KisHeader` | `geo_kis_container.h` | KIS binary container header (24B) |
| `TESS_Header` | `geo_tess_container.h` | .tess file header (64B) |
| `TESS_Formula` | `geo_tess_container.h` | formula block (64B) |
| `ResidualEntry` | `residual_space.h` | bond-keyed storage entry |
| `ResidualSpace` | `residual_space.h` | bond-keyed hash table |
| `GhostLogEntry` | `geo_ghost_lift.h` | 5B passive scale-change record |
| `GhostLog` | `geo_ghost_lift.h` | sorted ghost entries + wang |
| `GhostPairTable` | `geo_ghost_lift.h` | O(1) accelerator for ghost lookups |
| `GeosVolume` | `geofs_core.h` | GeoFS volume (1.3MB data store) |
| `GeosInode` | `geofs_core.h` | file metadata (geometric address) |
| `GeosAddr` | `geofs_core.h` | unified geometric address |
| `GoldbergStore` | `geo_goldberg_store.h` | decagram-Goldberg streaming store |
| `GGFHeader` | `geo_goldberg_file.h` | .ggf file header (64B) |
| `GGFReader` | `geo_goldberg_file.h` | lazy seek reader |
| `GGFMap` | `geo_goldberg_file.h` | mmap zero-copy reader |
| `MoeExpertMeta` | `moe_expert_store.h` | expert weight metadata (8B) |
| `GpSphere` | `geo_goldberg_sphere.h` | Goldberg sphere data |
| `Tring` | `infra/tring.h` | timeline ring (sparse tick array) |
| `TringNode` | `infra/tring.h` | variable-size node |
| `HWRouter` | `geo_hyperbolic_walk.h` | centroid walk router |
| `HWFrames` | `geo_hyperbolic_store.h` | key-frame grid |
| `GearLock` | `infra/gear_lock.h` | CPU/GPU tick counter |
| `RDHConfig` | `infra/rdh_addr.h` | mixed-radix config |
| `SortEntry` | `kis_codec_v4.h` | sort by code for permutation |

---

## Tags

#DWGLS #primitives #geometry #codec #addressing #sacred-numbers #reference
