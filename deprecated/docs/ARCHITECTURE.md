# Architecture

## Pattern Overview

**Overall:** Geometric-Field-as-Address-Space — every byte in the system is reachable via a geometry-derived coordinate. No hash, no lookup table, no collision. Coordinate = address.

**Key Characteristics:**
- **MAP not COMPRESS** — geometry provides the addressing template; codebook provides dedup
- **Timeline-first** — int-only, base-2 scale, no zero, fully deterministic and replayable
- **Sacred constants**: 20736 (= 144² = 12⁴), 1728 (= 12³), 144, 12, 18 (tesseracts)
- **Header-only C** — all core logic lives in `.h` files; tests and tools are thin `.c` wrappers
- **Lossless-verified** — every encode path has a decode roundtrip test proving bitwise identity

## Layers

### Geometry Primitives (`core/geo_octant.h`, `core/geo_cube_addr.h`, `core/geo_param_grid.h`)
- Purpose: define the slot-level coordinate system — octant identity, cube-in-dodecahedron addressing, and the parameterized geometry grid (GeoType enum → props table)
- Location: `core/geo_octant.h`, `core/geo_cube_addr.h`, `core/geo_cube_in_dodeca.h`, `core/geo_param_grid.h`
- Contains: octant zero-sum binding, cube face/slot addressing, GeoType switch (12→192 variants)
- Depends on: `<stdint.h>` only (self-contained)
- Used by: every higher layer

### Tesseract Wiring (`core/geo_tess_wiring.h`, `core/geo_tesseract_dense.h`)
- Purpose: map between Rescope layout (tesseract × cube × slot) and flat address (0..20735). One tesseract = 8 cubes × 144 slots = 1152. 18 tesseracts = 20736 full field.
- Location: `core/geo_tess_wiring.h`, `core/geo_tesseract_dense.h`
- Contains: `tess_to_flat` / `flat_to_tess` address conversion, DenseTesseract struct (bipolar: store 4 valid cubes, derive 4 antipodal)
- Depends on: `geo_octant.h`, `geo_unified.h`
- Used by: `.tess` container, MoE expert mapping, breathing FS, GPU scatter decode

### KIS Codec Stack (`core/kis_codec_v4.h` → `v6b.h`)
- Purpose: lossless codec for quantized weights — codebook (distinct values) + position permutation (delta + varint)
- Location: `core/kis_codec_v4.h`, `core/kis_codec_v5.h`, `core/kis_codec_v6.h`, `core/kis_codec_v6b.h`
- Contains: 2-layer encode/decode (Layer 1: codebook bitmap+RLE; Layer 2: delta-encoded position permutation)
- Depends on: `<stdint.h>`, `<stdlib.h>`
- Used by: `tests/kis_codec_v*_test.c`, container integrations

### .tess Container (`core/geo_tess_container.h`)
- Purpose: single-tensor binary container — stride-37 scatter encode, CRC-64 integrity, capo addressing for multi-cube tensors
- Location: `core/geo_tess_container.h` (1370 lines)
- Contains: TESS_Header (64B), TESS_Formula (64B), cell size tables for all GGML types (F32/F16/Q8_0/Q4_0/Q4_K/Q5_K/Q6_K/Q8_K), KIS 3-axis decomposition (X/Y/Z = 6912 each), 8 octant derivation
- Depends on: `geo_octant.h`, `geo_voronoi_mask.h`, `geo_tess_wiring.h`
- Used by: `tools/tess_bake.c`, `tools/tess_load.c`, `tools/tess_assemble.c`, `tools/tess_gguf_pack.c`, `tools/tess_packer.c`, all tess tests

### .tesspack Container (`tools/tess_packer.c`, `tools/tesspack_assemble.c`)
- Purpose: single-file archive of many `.tess` capos — index + concatenated capo payloads
- Location: `tools/tess_packer.c`, `tools/tesspack_assemble.c`, `core/geo_tess_container.h`
- Contains: pack header, capo index, mmap-based reader
- Depends on: `geo_tess_container.h`, `gguf_reader.h`
- Used by: `make tess-pack`, `make tesspack-assemble`, `tools/tesspack_graft.c`, `tools/tesspack_llama_view.c`, `tools/tesspack_server.c`

### Breathing Filesystem (`core/breathing_fs.h`, `core/bfs_*.h`)
- Purpose: geometric file system where compression = space movement. Seeker at position X writes/reads there. Scale changes move the window.
- Location: `core/breathing_fs.h`, `core/bfs_breath.h`, `core/bfs_fan24.h`, `core/bfs_fold.h`, `core/bfs_magnify.h`, `core/bfs_persist.h`, `core/bfs_seek_anchor.h`
- Contains: BreathingSeeker (scale, window, position), 20736-slot field with 144 blocks, persistence (planet watchers), fold/magnify operations
- Depends on: `geo_planet.h`, `bfs_v6b_adapter.h`
- Used by: `tests/test_bfs_*.c`, `tests/test_breathing_fs.c`

### Scale Bridge (`core/scale_bridge.h`)
- Purpose: unify BFS continuous seeker and tess gear ring on ONE timeline — 1 gear tooth = 1 semitone = ×2^(1/12)
- Location: `core/scale_bridge.h`
- Contains: `sbr_w_to_scale(W)` → s = 2^(-W/12), ring of 144 teeth, hyperbolic boundary at W=12 (s=0.5), 6 axis-aware W rings (from `geo_box_axes.h`)
- Depends on: `geo_box_axes.h`
- Used by: `tests/test_scale_bridge.c` (35/35 PASS)

### GGUF Integration (`core/gguf_reader.h`, `core/gguf_box.h`, `core/gguf_index.h`)
- Purpose: memory-mapped GGUF reader (bulk parse via pointer arithmetic, zero syscalls during parse) + box routing + window chain for graft path
- Location: `core/gguf_reader.h`, `core/gguf_box.h`, `core/gguf_index.h`
- Contains: `GgufReader` struct (mmap-based, handles v3 u64 fields), GGUF box (header scion + zero-copy body), window chain pointers
- Depends on: `<windows.h>` / `<sys/mman.h>` for mmap
- Used by: all graft/server/lazy-serve tools, tess_gguf_pack, test_gguf_box

### DWGLS Shell + Codec Interface (`core/dwgls_shell.h`, `core/dwgls_codec.h`)
- Purpose: universal 32-byte container shell (magic + geometry + integrity) + swappable codec vtable (encode/decode/verify/estimate)
- Location: `core/dwgls_shell.h`, `core/dwgls_codec.h`
- Contains: DWGLS_Shell header (32B packed), codec_id enum (KIS_FRAME=1 through USER_START=64), vtable (6 function pointers)
- Depends on: `<stdint.h>`
- Used by: `core/dwgls_codec_kis.h`, `core/dwgls_codec_tess.h`, `core/dwgls_codec_gcube.h`, etc.

### MoE Expert Mapping (`core/moe_expert_addr.h`, `core/moe_expert_store.h`)
- Purpose: map (layer, expert_num, weight_type) → geometry coordinate → flat address → DtSlotRegion slot. Expert_id IS the geometry coordinate.
- Location: `core/moe_expert_addr.h`, `core/moe_expert_store.h`, `core/tess_moe_bridge.h`
- Contains: `moe_expert_to_flat`, `moe_flat_to_expert`, `moe_expert_to_geom` — O(1) integer math, no hash
- Depends on: `geo_tess_wiring.h`
- Used by: `tools/moe_expert_bake.c`, `tools/moe_expert_graft.c`, `tools/moe_expert_stream.c`, `tools/moe_expert_route.c`

### Platonic Field (`core/geo_octant.h`, `core/geo_voronoi_mask.h`, `core/geo_tesseract_dense.h`)
- Purpose: octant identity (8 views of a cube) + Voronoi masking (24 seeds → 24 cells, spotlight, gravity) — the foundational field that all storage builds on
- Location: `core/geo_octant.h`, `core/geo_voronoi_mask.h`
- Contains: zero-sum binding (i+j+k ∈ {0,1}), MaskedPointer (cell_id + local offset), spotlight radius, gravity attractor
- Depends on: `<stdint.h>`, `<math.h>`
- Used by: `.tess` container, breathing FS, geo_hyperbolic_*

### Hyperbolic Route (`core/geo_hyperbolic_walk.h`, `core/geo_hyperbolic_store.h`)
- Purpose: deterministic walk on the hyperbolic side of the field — key-frame grid + f(step) reconstruction, no float addressing
- Location: `core/geo_hyperbolic_walk.h`, `core/geo_hyperbolic_store.h`, `core/hyp_fusion.h`
- Contains: HWFrames (aperture, depth, axis), key-frame centroids, stride-1 full-coverage bijection
- Depends on: `tri_hex_tess.h`
- Used by: `tests/test_geo_hyperbolic.c`, `tests/test_hyp_fusion.c`

### GPU Scatter Decode (`bench/tess_scatter_decode.cu`)
- Purpose: standalone CUDA kernel — decode .tesspack capos on GPU via stride-37 scatter gather, no llama.cpp dependency
- Location: `bench/tess_scatter_decode.cu`, `bench/tess_scatter_bench_v2.cu`
- Contains: mmap → cudaHostRegister → stride-37 scatter → sig32 XOR-fold verify, zero-copy H2D
- Depends on: CUDA runtime, `geo_tess_container.h` constants
- Used by: `colab-pack/deploy_scatter_decode.sh`

### Infra Layer (`core/infra/`)
- Purpose: cross-cutting infrastructure — DRamTile (zero-copy slot region), GearLock (clock domain sync), GearShift (scale transitions), FiboSpine (fibonacci checkpoint), cJSON (config parsing)
- Location: `core/infra/`
- Contains: `dramtile_container.h`, `dramtile_store.h`, `gear_lock.h`, `gear_shift.h`, `fibo_spine.h`, `geo_dram_tile.h`, `geo_gpu_pipeline.h`, `geo_rail_ring.h`, `geo_rail_sync.h`, `geo_spoke_sync.h`, `geo_twin_rebalance.h`, `geo_triality_serve.h`, `tess_scatter_gpu.h`, `tring.h`, `cJSON.c`/`cJSON.h`, `config.c`/`config.h`
- Depends on: Windows API (`VirtualAlloc`) or POSIX mmap
- Used by: DRamTile-based tools, GPU pipeline, moe_expert_store

### Testing (`tests/`)
- Purpose: 300+ test files — each is a standalone C file that compiles and runs, verifying lossless roundtrip or geometric invariant
- Location: `tests/`
- Contains: `test_*.c`, `kis_codec_v*_test.c`, `*_probe.c`, Python analysis scripts
- Depends on: core headers + Makefile auto-discovery
- Used by: `make test`, `make test-smoke`, `make test-<group>`

### Tools (`tools/`)
- Purpose: CLI utilities — bake/load/assemble/pack/verify/serve for all formats
- Location: `tools/`
- Contains: `tess_bake.c`, `tess_load.c`, `tess_assemble.c`, `tess_packer.c`, `tess_gguf_pack.c`, `tesspack_assemble.c`, `tesspack_graft.c`, `tesspack_server.c`, `moe_expert_bake.c`, `gguf_graft_*.c`, `gguf_lazy_serve.c`, `geo_speed_bench.c`, `field_trainer.c`, `handoff.py`, `session_trail.py`
- Depends on: core headers, Makefile targets
- Used by: `make tess-bake`, `make moe-bake`, `make graft-llama`, `make tess-server`

### Benchmarks (`bench/`)
- Purpose: performance measurement — GPU scatter, bandwidth, isometric map, FS operations
- Location: `bench/`
- Contains: `tess_scatter_bench.cu`, `tess_scatter_decode.cu`, `fs_bench.c`, `rail_bench.c`, Colab deploy scripts
- Depends on: CUDA runtime, core headers
- Used by: Colab benchmark runs

### Deprecated (`deprecated/`)
- Purpose: archived code from earlier development phases — preserved for history, never used in current system
- Location: `deprecated/core/`, `deprecated/tests/`, `deprecated/tools/`, `deprecated/PasteBin/`
- Contains: `dwgls_tesseract_codec.h`, `dwgls_dynamic_codec.h`, `dwgls_codec_kisv6.h`, zone-era headers
- Rule: do not reopen unless new evidence

## Data Flow

### GGUF → .tesspack (Bake Pipeline)
1. GGUF file → `tools/tess_gguf_pack.c` — parse header + tensors via `core/gguf_reader.h`
2. Per tensor → `core/geo_tess_container.h` stride-37 scatter encode → `.tess` capo
3. Pack capos → `tools/tess_packer.c` → single `.tesspack` file
4. Integrity: `hdr[8]` sig32 XOR-fold per capo, CRC-64 on decode

### .tesspack → GGUF (Assemble Pipeline)
1. `.tesspack` file → `tools/tesspack_assemble.c` — parse pack header + index
2. Per capo → decode stride-37 scatter → raw weight bytes
3. Rebuild GGUF header from embedded metadata → output `.gguf`
4. Verify: `tools/gguf_stream_compare.c` byte-for-byte against original

### .tesspack → llama.cpp Inference (Graft Path)
1. `.tesspack` → mmap via `tools/tesspack_llama_view.c` or `tools/tesspack_bridge.c`
2. Header scion from GGUF box (`core/gguf_box.h`) + zero-copy body from mmap
3. `ggml_backend_buffer` callback repoints `t->data` into mmap pages
4. Pages fault in at generation time, not load time (lazy-serve)
5. Verify: compare logits + tokens bitwise with original GGUF inference

### GPU Scatter Decode
1. `.tesspack` → mmap on host (`bench/tess_scatter_decode.cu`)
2. Parse pack header + index on CPU
3. Per capo: read TESS_Header → cell_size → `cudaHostRegister` pin pages
4. GPU kernel: `weight_idx → slot = (idx*37)%20736 → read cell bytes`
5. sig32 XOR-fold verify on GPU
6. Output: decoded weight array per capo

### MoE Expert Routing
1. GGUF → `tools/moe_expert_bake.c` → DtSlotRegion via `core/moe_expert_addr.h` geometric addressing
2. Router gate → select top-K experts per layer
3. `tools/moe_expert_route.c` → rebuild GGUF from routed experts
4. `tools/moe_expert_graft.c` → load through llama.cpp → compare logits/tokens

## Key Abstractions

### GeoType (Geometry Grid)
- Purpose: enumerate all supported geometry variants from dodeca root
- Location: `core/geo_param_grid.h`
- Pattern: enum + switch-based props table, one family tree

### TESS_Header + TESS_Formula
- Purpose: binary container metadata — magic, cell counts, axis decomposition, integrity hash, formula describing scatter parameters
- Location: `core/geo_tess_container.h`
- Pattern: fixed-size packed structs (64B each), LUT for cell sizes

### DWGLS_Shell
- Purpose: universal 32-byte file header — identifies format, declares geometry, validates integrity
- Location: `core/dwgls_shell.h`
- Pattern: packed struct with codec_id dispatch

### BreathingSeeker
- Purpose: continuous scale-aware cursor over the 20736-slot field
- Location: `core/breathing_fs.h`
- Pattern: struct with scale/window/position, `seeker_scale()` adjusts hyperbolic boundary

### GgufReader
- Purpose: memory-mapped GGUF parser with bulk pointer-arithmetic mode
- Location: `core/gguf_reader.h`
- Pattern: mmap-based reader, bounded pointer buffer (GBuf) for safe parsing

### DRamTile
- Purpose: zero-copy slot region — direct-addressed weight storage with GearLock clock sync
- Location: `core/infra/dramtile_store.h`, `core/infra/geo_dram_tile.h`
- Pattern: VirtualAlloc(MEM_RESERVE) + MEM_COMMIT active regions only

### MaskedPointer
- Purpose: two-part address (cell_id + local) with axis context — seeker constrained to Voronoi cell boundary
- Location: `core/geo_voronoi_mask.h`
- Pattern: position-dependent masking + spotlight window + gravity attractor

## Entry Points

**Test Runner:**
- Location: `Makefile`
- Triggers: `make test`, `make test-smoke`, `make test-<group>`
- Responsibilities: auto-discover `tests/*.c`, compile per-test, run, report PASS/FAIL

**Tess Bake:**
- Location: `tools/tess_bake.c`
- Triggers: `make tess-bake GGUF=<path>`
- Responsibilities: GGUF → .tess files per tensor

**Tesspack Assemble:**
- Location: `tools/tesspack_assemble.c`
- Triggers: `make tesspack-assemble TESSPACK=<path>`
- Responsibilities: .tesspack → standalone GGUF

**Tess Server:**
- Location: `tools/tesspack_server.c`
- Triggers: `make tess-server`
- Responsibilities: OpenAI-compatible HTTP API served directly from .tesspack via mmap

**GPU Scatter Decode:**
- Location: `bench/tess_scatter_decode.cu`
- Triggers: `./tess_scatter_decode model.tesspack [--verify]`
- Responsibilities: CUDA kernel decode + sig32 verify, no llama.cpp dependency

**MoE Bake/Graft/Route/Stream:**
- Location: `tools/moe_expert_bake.c`, `tools/moe_expert_graft.c`, `tools/moe_expert_route.c`, `tools/moe_expert_stream.c`
- Triggers: `make moe-bake`, `make moe-graft`, `make moe-route`, `make moe-stream`
- Responsibilities: MoE expert roundtrip through geometric addressing

## Error Handling

**Strategy:** Fail-fast with return codes. Every function returns `int32_t` (bytes written) or negative on error. Tests assert on return value. No exceptions, no longjmp. Integrity verified via CRC-64 (`.tess` format) and sig32 XOR-fold (`.tesspack` per-capo). Decode always re-encodes to verify lossless.

## Cross-Cutting Concerns

**Integrity:** CRC-64 (ECMA-182) per `.tess` capo, sig32 XOR-fold per `.tesspack` capo in `hdr[8]`, xxHash64 option in DWGLS shell. Every encode path has a decode roundtrip test.

**Storage:** Memory-mapped I/O everywhere — `mmap` on Linux/Colab, `VirtualAlloc(MEM_RESERVE)` + `MEM_COMMIT` on Windows. DRamTile zero-copy path avoids H2D copy on GPU.

**Scale:** Single global timeline. `s(t) = s₀·kᵗ`, no zero, no end. BFS seeker and tess gear ring unified via `scale_bridge.h` — 1 tooth = 1 semitone = ×2^(1/12).

**Configuration:** `config.json` at project root, parsed via `core/infra/cJSON.c`. Minimal — most constants are sacred (compile-time).

**Platform:** Windows (MSYS2/MinGW) primary, Linux (Colab) for GPU benchmarks. Build: `gcc -O2 -Wall -I. -Icore -Icore/infra`. GPU: `nvcc -O3 -arch=sm_75`.
