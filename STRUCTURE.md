# Codebase Structure

## Directory Layout

```
DWGLS-native-fs/
├── `core/`            # Header-only geometric address space (188 headers)
├── `core/infra/`      # Zero-copy tiles, GPU pipeline, rail sync
├── `tools/`           # Pack/serve/bench/probe CLIs (C + Python)
├── `tests/`           # Tiered C tests (272 sources, `make test-*`)
├── `test/`            # Single legacy C test
├── `bench/`           # Deploy and benchmark scripts
├── `scripts/`         # Shell staging and deploy helpers
├── `docs/`            # Design notes, handoffs, specs, excalidraw masters
├── `experiments/`     # Archived probe campaigns, one dated folder per campaign
├── `graft/`           # Local graft cache and index (git-ignored)
├── `colab-pack/`      # Colab LoRA training pack and deploy scripts
├── `beam_addressing/` # Beam timer header
├── `collection/`      # Beam/RDH/tw collections
├── `build/`           # Compiled binaries and test outputs (generated)
├── `build_arm/`       # ARM-compiled test binaries (generated)
├── `tess_out/`        # `.tess` bake output (generated)
├── `tess_out_fresh/`  # Fresh `.tess` bake output (generated)
├── `DWGLS-portable/`  # Portable binary + model bundle
├── `DWGLS-portable-test/` # Portable bundle test harness
├── `_recovery/`       # Recovery scripts and gap archives
├── `deprecated/`      # Frozen history, never delete
├── `Makefile`         # Tiered test runner and CLI builders
├── `config.json`      # Runtime configuration
├── `dwgls_gui_server.py` # Browser GUI server
├── `dwgls_gui.html`   # Browser GUI page
```

## Directory Purposes

**`core/`:**
- Purpose: Hold the entire address-space implementation as self-contained headers
- Contains: `*.h` only, `static inline` logic, no compiled library
- Key files: `core/geo_param_grid.h`, `core/kis_codec_v6.h`, `core/geo_box_axes.h`, `core/geo_tess_container.h`, `core/clim_record.h`, `core/geofs_mdim.h`, `core/breathing_fs.h`, `core/gguf_reader.h`, `core/gguf_box.h`, `core/scale_bridge.h`, `core/zone_card_v3.h`, `core/moe_expert_addr.h`, `core/geo_net_walk.h`, `core/anchor_route.h`, `core/kv_cold_base.h`, `core/mv_node.h`, `core/mm_route.h`, `core/mm_wang.h`

**`core/infra/`:**
- Purpose: Hold zero-copy and sync primitives under the geometry layer
- Contains: DRAM tile, GPU scatter, rail/phase sync, jet phase select, trialty serve headers
- Key files: `core/infra/geo_dram_tile.h`, `core/infra/geo_gpu_pipeline.h`, `core/infra/geo_rail_sync.h`, `core/infra/jet_select.h`

**`tools/`:**
- Purpose: Hold every runnable entry point except tests
- Contains: `*.c` CLIs, `*.py` servers and converters
- Key files: `tools/tess_bake.c`, `tools/tess_load.c`, `tools/tess_assemble.c`, `tools/tess_gguf_pack.c`, `tools/tesspack_assemble.c`, `tools/tesspack_server.c`, `tools/probe_moe_assemble.c`, `tools/gguf_lazy_serve.c`, `tools/dual_lazy_serve.c`, `tools/field_qa.c`, `tools/geo_field_query.c`, `tools/tess_window_bench.c`, `tools/moe_expert_bake.c`, `tools/moe_expert_route.c`, `tools/moe_expert_graft.c`, `tools/anchor_route_cli.c`, `tools/maze_walk_cli.c`, `tools/kv_cold_base.c`, `tools/kv_cold_delta.c`, `tools/kv_cold_reanchor.c`, `tools/kv_cold_prefix.c`, `tools/kv_cold_chat.c`, `tools/kv_delta_map.c`, `tools/kv_quant_3level.c`, `tools/kv_quant_threshold.c`, `tools/bake_q4.c`, `tools/gguf_tnames.c`, `tools/merge_probe.c`, `tools/mdim_cli.c`, `tools/dwgls_server.py`

**`tests/`:**
- Purpose: Hold tiered verification sources compiled on demand by `Makefile`
- Contains: `*.c` tests, one file per subsystem check
- Key files: `tests/test_tesspack.c`, `tests/test_scale_bridge.c`

**`docs/`:**
- Purpose: Hold design records, handoffs, specs, and visual masters
- Contains: `*.md` notes, `*.excalidraw` sources, rendered `*.svg`
- Key files: `docs/PIPELINE-MAP.md`, `docs/LEGACY_TESTS.md`, `docs/HYBRID-GATE-DOCTRINE-2026-09-18.md`, `docs/DUALWORLD-GENESIS-2026-09-18.md`, `docs/ANN-CLIMATE-CAMPAIGN-2026-09-24.md`

**`experiments/`:**
- Purpose: Hold dated probe campaigns as archived source outside the build path
- Contains: `experiments/ann-climate-2026-09-24/sift/*.c` SIFT probes, `experiments/ann-climate-2026-09-24/chatmap/*.py` chatmap experiments, campaign record in `docs/ANN-CLIMATE-CAMPAIGN-2026-09-24.md`
- Rule: Archive a probe campaign here under `<topic>-YYYY-MM-DD/` once its record lands in `docs/`.

**`bench/`:**
- Purpose: Hold deploy-pack and benchmark scripts
- Contains: `make_*.py` packagers, `*_bench.c`, `fs_bench.c`, `gpu_bandwidth_bench.cu`, `gpu_launch_bench.cu`, `gpu_batch_break_even.cu`, `tpu_jax_bench.py`

**`scripts/`:**
- Purpose: Hold staging and environment helpers
- Contains: shell and cmd scripts
- Key files: `scripts/stage-zc2-dlls.cmd`, `scripts/build_termux.sh`, `scripts/deploy_termux.sh`, `scripts/run_gpu_benches.sh`

**`colab-pack/`:**
- Purpose: Hold LoRA training and remote-GPU deploy bundle
- Contains: training scripts, trace JSONL, deploy shells

**`deprecated/`:**
- Purpose: Preserve frozen experiments and superseded code as history
- Contains: old `core/`, `docs/`, `tests/`, `PasteBin/` snapshots
- Rule: Move dead code here. Never delete it.

**`build/`, `build_arm/`, `tess_out/`, `tess_out_fresh/`:**
- Purpose: Hold generated binaries and baked `.tess` output
- Contains: compiled test binaries, CLI executables, capo files
- Rule: Regenerate with `make`. Never hand-edit.

**`DWGLS-portable/`, `DWGLS-portable-test/`:**
- Purpose: Hold portable binary + model bundle and its test harness
- Contains: `bin/` executables, `models/` GGUF files

**`_recovery/`:**
- Purpose: Hold recovery scripts and gap archives
- Contains: extraction scripts, `gap_*` date-range snapshots

## Key File Locations

**Entry Points:** `tools/tess_bake.c`: GGUF to `.tess` encode
**Entry Points:** `tools/tess_load.c`: `.tess` decode and verify, with `--dram` tile path
**Entry Points:** `tools/tess_assemble.c`: `.tess` directory to GGUF rebuild
**Entry Points:** `tools/tess_gguf_pack.c`: GGUF direct to `.tesspack`
**Entry Points:** `tools/tesspack_assemble.c`: `.tesspack` to standalone GGUF
**Entry Points:** `tools/tesspack_server.c`: OpenAI-compatible HTTP serve from `.tesspack`
**Entry Points:** `tools/gguf_lazy_serve.c`: single-model lazy mmap serve
**Entry Points:** `tools/dual_lazy_serve.c`: two-model co-serve with isolated evict
**Entry Points:** `tools/field_qa.c`: prompt answering from baked field with sourceless delta MoE
**Entry Points:** `tools/geo_field_query.c`: tensor name to chain position to field bytes
**Entry Points:** `tools/moe_expert_bake.c`: MoE expert bake by geometric address
**Entry Points:** `tools/moe_expert_route.c`: top-K expert routing and serve
**Entry Points:** `tools/moe_expert_graft.c`: MoE expert graft, rebuild GGUF, verify inference
**Entry Points:** `tools/mdim_cli.c`: GeoFS volume CRUD
**Entry Points:** `dwgls_gui_server.py`: browser GUI server with `dwgls_gui.html`
**Configuration:** `config.json`: runtime settings
**Configuration:** `Makefile`: test tiers, group runners, all CLI build rules
**Core Logic:** `core/geo_param_grid.h`: shape family and codec sizing
**Core Logic:** `core/kis_codec_v6.h`: index-to-slot helix and residuals
**Core Logic:** `core/geo_tess_container.h`: `.tess` and `.tesspack` formats
**Core Logic:** `core/geofs_mdim.h`: multidimensional volume and journal
**Core Logic:** `core/gguf_box.h`: llama.cpp graft routing
**Tests:** `tests/test_tesspack.c`: pack roundtrip proof
**Tests:** `tests/test_scale_bridge.c`: scale-ring alignment oracle
**Tests:** `tests/test_kineticfan_field.c`: kineticfan field fit (net-walk + fan24 gear)
**Tests:** `tests/test_jet_select_prod.c`: `jet_select` policy grid vs independent oracles and `geo_pipeline_tick` wire-in
**Tests:** `tests/test_jet_coalesce_bench.c`: `geo_pipeline_want` coalesce ratio (sparse/dense/sustained vs bridge dispatch)

## Naming Conventions

**Files:** `snake_case` with subsystem prefix: `geo_*` geometry, `kis_*` codec, `tess*` container, `gguf_*` model ingest, `moe_*` experts, `kv_*` cache remap, `test_*` verification, `bench_*` measurement
**Examples:** `core/geo_tess_container.h`, `core/kis_codec_v6.h`, `tools/moe_expert_route.c`, `tests/test_scale_bridge.c`
**Directories:** lowercase single words: `core`, `tools`, `tests`, `docs`, `bench`, `scripts`
**Tests:** `tests/<subsystem>_<check>.c`, built as `build/test-<name>`, run with `make test-<name>`
**Binaries:** `build/` holds compiled tools without extension on MSYS2 and with `.exe` on Windows shell paths

## Where to Add New Code

**New geometry header:** `core/geo_<name>.h` — keep std-only includes, expose `static inline` functions, add no build step
**New codec version:** `core/kis_codec_v<name>.h` — keep slot math integer-only, preserve 20736 grid constants
**New pack/serve CLI:** `tools/<name>.c` — include from `core/` with `-Icore`, add a `Makefile` target beside the existing pack rules
**New probe:** `tools/<name>_probe.c` — follow `tools/lora_accuracy_probe.c` shape: parse args, run on real model bytes, print run receipts
**New experiment:** `experiments/<topic>-YYYY-MM-DD/` — keep probe source that is out of the build path; record findings in `docs/<TOPIC>-YYYY-MM-DD.md`
**New test:** `tests/test_<name>.c` — derive expectations from spec or math, add the name to the correct `Makefile` group (`KIS`, `TESS`, `GEO`, `GGUF`, `BFS`, `CAP`, `GHOST`, `KV`, `SIXICO`, `FIBO`, `WALK`)
**New bench:** `bench/<name>.c`, `bench/<name>.cu`, or `tools/<name>_bench.c` — follow `tools/geo_speed_bench.c` shape; run the CUDA GPU benches through `scripts/run_gpu_benches.sh`
**New docs:** `docs/<TOPIC>-YYYY-MM-DD.md` — record runs and receipts, never restate code without verifying it
**Shared infra:** `core/infra/<name>.h` — use for zero-copy, sync, and pipeline primitives shared across tools
