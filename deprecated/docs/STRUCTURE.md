# Codebase Structure

## Directory Layout

```
DWGLS-native-fs/
├── core/                  # All logic: 161 header files, self-contained
│   ├── infra/             # Cross-cutting: DRamTile, GearLock, cJSON, config
│   ├── geo_*.h            # Geometry primitives, addressing, containers
│   ├── kis_codec_v*.h     # KIS codec stack (v4→v6b)
│   ├── dwgls_*.h          # Shell, codec interface, specific codecs
│   ├── bfs_*.h            # Breathing filesystem subsystem
│   ├── gguf_*.h           # GGUF reader, box, index
│   ├── moe_expert_*.h     # MoE expert geometric addressing
│   └── *.h                # 160+ more headers (hyperbolic, goldberg, etc.)
├── tests/                 # 300+ test files — standalone C, one per file
├── tools/                 # 150+ CLI tools — bake/load/assemble/pack/serve
├── bench/                 # Benchmarks — GPU scatter, FS speed, isometric
├── docs/                  # 100+ design docs, handoffs, reports, SVGs
├── scripts/               # Shell scripts (Termux build/deploy, file restore)
├── deprecated/            # Archived code from earlier phases (history only)
│   ├── core/              # Old codecs (kisv6, dynamic, tesseract)
│   ├── tests/             # Old tests
│   ├── tools/             # Old tools
│   └── PasteBin/          # Stashed headers
├── build/                 # Compiled test/tool binaries (gitignored)
├── colab-pack/            # Colab deploy scripts for GPU benchmarks
├── collection/            # Collected model files (not tracked)
├── beam_addressing/       # Beam addressing experiments
├── DWGLS-portable/        # Portable build artifacts
├── Makefile               # Build system — auto-discover tests, group targets
├── AGENTS.md              # Agent rules, architecture notes, tool reference
└── config.json            # Runtime configuration (minimal)
```

## Directory Purposes

**core/:**
- Purpose: all system logic — geometry, codecs, containers, filesystem, GGUF integration
- Contains: 161 `.h` header files (the entire system), 1 `.c` file (`dramtile_store.c`)
- Key files: `geo_param_grid.h` (GeoType enum), `geo_tess_container.h` (`.tess` format, 1370 lines), `geo_tess_wiring.h` (address mapping), `breathing_fs.h` (BFS), `scale_bridge.h` (timeline unification), `gguf_reader.h` (GGUF mmap parser), `dwgls_shell.h` (universal header), `moe_expert_addr.h` (MoE mapping)

**core/infra/:**
- Purpose: cross-cutting infrastructure shared by all subsystems
- Contains: DRamTile store, GearLock/GearShift clock domain, FiboSpine checkpoint, cJSON parser, GPU pipeline, rail/spoke/triality sync, config
- Key files: `dramtile_store.h`, `geo_dram_tile.h`, `gear_lock.h`, `gear_shift.h`, `fibo_spine.h`, `geo_gpu_pipeline.h`, `cJSON.c`, `config.c`, `geo_twin_rebalance.h`, `geo_triality_serve.h`

**tests/:**
- Purpose: 300+ standalone test files — each compiles and runs independently, verifying lossless roundtrip or geometric invariant
- Contains: `test_*.c` (system tests), `kis_codec_v*_test.c` (codec tests), `*_probe.c` (exploration), `*.py` (analysis scripts), `*.ps1` (extraction scripts)
- Key files: `test_tess_index_frame.c`, `test_tess_scale_log.c`, `test_scale_bridge.c`, `test_bfs_persist.c`, `test_tesspack.c`, `test_6ico_tesseract.c`, `bench_tesspack.c`

**tools/:**
- Purpose: CLI utilities for every format operation — bake, load, assemble, pack, verify, serve, benchmark
- Contains: 150+ `.c` files, 3 `.py` tools, 1 `.html` dashboard
- Key files: `tess_bake.c`, `tess_load.c`, `tess_assemble.c`, `tess_gguf_pack.c`, `tess_packer.c`, `tesspack_assemble.c`, `tesspack_graft.c`, `tesspack_server.c`, `tesspack_llama_view.c`, `moe_expert_bake.c`, `moe_expert_graft.c`, `moe_expert_route.c`, `gguf_graft_*.c`, `gguf_lazy_serve.c`, `geo_speed_bench.c`, `handoff.py`, `session_trail.py`

**bench/:**
- Purpose: performance measurement and GPU benchmarks
- Contains: CUDA kernels, C benchmarks, Colab deploy scripts, Vulkan compute shader
- Key files: `tess_scatter_decode.cu` (standalone GPU decode), `tess_scatter_bench_v2.cu` (DRamTile + sig32), `fs_bench.c`, `rail_bench.c`, `tess_scatter_colab.ipynb`, `make_deploy_*.py`

**docs/:**
- Purpose: design documents, handoff notes, reports, architecture diagrams
- Contains: 100+ `.md` files, `.excalidraw` masters, `.svg` renders, images
- Key files: `PIPELINE-MAP.md`, `PLATONIC_FIELD_ARCHITECTURE.md`, `OVERVIEW.md`, `INDEX.md`, `tess-format-spec.md`, `FS_FORMAT_SPEC.md`, `scale-bridge.svg`, `pipeline-map.svg`

**deprecated/:**
- Purpose: archived code from earlier development phases — preserved for history, never used
- Contains: old codecs (`dwgls_tesseract_codec.h`, `dwgls_dynamic_codec.h`), zone-era headers, old tests/tools
- Rule: do not reopen unless new evidence

## Key File Locations

**Entry Points:**
- `Makefile`: build system — `make test`, `make test-smoke`, `make tess-bake`, etc.
- `tools/tesspack_server.c`: OpenAI-compatible HTTP API from .tesspack
- `bench/tess_scatter_decode.cu`: standalone GPU decode entry

**Configuration:**
- `config.json`: runtime config (minimal)
- `AGENTS.md`: agent rules, architecture notes, tool reference

**Core Logic:**
- `core/geo_tess_container.h`: `.tess` binary format definition (1370 lines) — the central container
- `core/geo_tess_wiring.h`: Rescope ↔ flat address mapping (the glue)
- `core/scale_bridge.h`: BFS seeker ⇄ tess gear ring unification
- `core/geo_param_grid.h`: geometry family definition (GeoType → props)
- `core/gguf_reader.h`: GGUF mmap parser (handles v2/v3)
- `core/breathing_fs.h`: geometric filesystem with breathing seeker
- `core/dwgls_shell.h`: universal 32-byte container header

**Tests:**
- `tests/test_tess_index_frame.c`: tesseract frame-as-index (7/7 PASS)
- `tests/test_tess_scale_log.c`: scale log replay (10/10 PASS)
- `tests/test_scale_bridge.c`: timeline bridge (35/35 PASS)
- `tests/test_bfs_persist.c`: breathing FS persistence
- `tests/test_tesspack.c`: .tesspack container roundtrip
- `tests/test_6ico_tesseract.c`: 6ico compound field (144 vertices)
- `tests/bench_tesspack.c`: tesspack performance benchmark

**Tests (Grouped by Subsystem):**
- KIS: `tests/kis_codec_v4_test.c`, `tests/kis_codec_v5_test.c`, `tests/kis_4d_explore.c`
- TESS: `tests/test_tess_*.c` (26 tests)
- GEO: `tests/test_geo_*.c`, `tests/geo_*.c`, `tests/test_goldberg_*.c`
- BFS: `tests/test_bfs_*.c`, `tests/test_breathing_fs.c`
- GGUF: `tests/test_gguf_*.c`, `tests/test_safetensors_reader.c`
- MoE: `tests/test_moe_expert.c`
- KV: `tests/test_kv_*.c`, `tests/test_hybrid_kv.c`
- Ghost: `tests/test_ghost_*.c`

## Naming Conventions

**Files:** `geo_*.h` = geometry primitives, `bfs_*.h` = breathing FS, `kis_codec_v*.h` = KIS codec versions, `dwgls_*.h` = shell/codec interface, `moe_expert_*.h` = MoE mapping, `test_*.c` = standalone tests, `*_probe.c` = exploration/benchmark scripts

**Directories:** single-level flat for core (no nesting except `core/infra/`), flat for tests/tools/bench

**Constants:** `TESS_*`, `GEO_*`, `BFS_*`, `OCT_*`, `VM_*`, `MOE_*`, `SBR_*` — all uppercase, prefix matches subsystem

## Where to Add New Code

**New geometry type:** `core/geo_param_grid.h` — add enum value + props case
**New codec:** `core/dwgls_codec_*.h` — implement the 6-function vtable from `core/dwgls_codec.h`, register `codec_id` in `core/dwgls_shell.h`
**New container format:** `core/geo_*_container.h` — follow `geo_tess_container.h` pattern (header + formula + payload + integrity)
**New test:** `tests/test_<name>.c` — standalone C file, auto-discovered by Makefile `wildcard`, add to TIER1 or TIER2 list
**New CLI tool:** `tools/<name>.c` — add Makefile target, include core headers with `-Icore -Icore/infra`
**New GPU kernel:** `bench/<name>.cu` — compile with `nvcc -O3 -arch=sm_75`
**New breathing FS operation:** `core/bfs_<name>.h` — include from `breathing_fs.h`
**New MoE feature:** `core/moe_expert_*.h` or `tools/moe_expert_*.c`
**New design doc:** `docs/<NAME>.md` — use UPPERCASE prefix for reports, lowercase for specs
**New experiment:** `deprecated/` when done — never delete, preserve history
