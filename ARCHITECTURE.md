# Architecture

## Pattern Overview

**Overall:** Header-only C geometric address space — MAP not COMPRESS.

**Key Characteristics:**
- Coordinate is address. No hash, no lookup table for address resolution. Static-geometry LUT only.
- Headers carry logic as `static inline` functions. No separate library binary. Every tool compiles with `-Icore`.
- One field size: 20736 slots (144², 18 tes × 8 cube × 144). All subsystems share it.
- Lossless is binary truth: decode, compare every value at every position.
- Integer-only addressing. Never compute vertex/face/projection/coordinate at runtime.

## Layers

**Geometry parameter grid:**
- Purpose: Select shape from one dodeca-rooted family. Provide vertex mask and slot capacity.
- Location: `core/geo_param_grid.h`
- Contains: `GeoType` enum, `GeoProps` table, sort → distinct-count → codebook-size codec
- Depends on: stdint/stdlib only
- Used by: codec and container layers for mask sizing

**KIS codec (field, not pipeline):**
- Purpose: Map weight index to field slot. Store value as data at slot.
- Location: `core/kis_codec_v6.h`, `core/kis_codec_v5.h`, `core/kis_codec_v4.h`
- Contains: `v6_slot(i) = (i*37) % 20736` helix, histogram codebook, varint/bitmap residual modes
- Depends on: `core/geo_param_grid.h` for grid sizing
- Used by: `tools/tess_bake.c`, `tools/tess_load.c`, tess roundtrip tools

**Box-axis addressing:**
- Purpose: Give outer identity without changing the 20736-cell box. Six deterministic paths over one box.
- Location: `core/geo_box_axes.h`
- Contains: `GBA_Address` (axis, position, local), xyz square axes + ijk triangle axes
- Depends on: `core/geo_octant.h`
- Used by: `core/scale_bridge.h`, `core/moe_expert_addr.h`

**Tess container:**
- Purpose: Define `.tess` single-cube and `.tesspack` multi-capo file formats. Stride-37 scatter with CRC-64 per capo.
- Location: `core/geo_tess_container.h`, `core/geo_tess_window.h`, `core/geo_tesseract_addr.h`
- Contains: magic/version/header structs, capo addressing, mmap pack reader
- Depends on: `core/geo_octant.h`, `core/geo_voronoi_mask.h`
- Used by: `tools/tess_bake.c`, `tools/tess_load.c`, `tools/tess_assemble.c`, `tools/tess_packer.c`, `tools/tess_gguf_pack.c`, `tools/tesspack_assemble.c`, `tools/tess_load_stream.c`

**Scale bridge:**
- Purpose: Unify continuous scale and discrete gear ring on one coordinate. One tooth = one semitone = ×2^(1/12).
- Location: `core/scale_bridge.h`
- Contains: `sbr_w_to_scale`, `sbr_position_to_w`, `sbr_gba_to_w`, ring constant 144, hyper boundary W=12
- Depends on: `core/geo_box_axes.h`
- Used by: seeker, gear, and magnify code paths

**GeoFS multidimensional volume:**
- Purpose: Serve one 20736-slot × 64 B volume through flat/cube/rail/time/cell views with a timeline journal.
- Location: `core/geofs_mdim.h`
- Contains: slot layout, trit-fold name bonding, run-span chains, write-ahead journal, `recover`, `state_at`
- Depends on: stdint/string/stdio/stdlib only
- Used by: `tools/mdim_cli.c`

**Breathing FS:**
- Purpose: Map compression to seeker movement. Seeker position is placement.
- Location: `core/breathing_fs.h`
- Contains: `BreathingSeeker`, scale/window/position state, hyperbolic flag
- Depends on: `core/bfs_v6b_adapter.h`, `core/bfs_magnify.h`, `core/bfs_fan24.h`, `core/geo_planet.h`
- Used by: breathing-filesystem tests and CLI tools

**GGUF ingest and graft:**
- Purpose: Parse real GGUF by mmap. Route tensor requests by zero-copy pointer. Rebuild servable GGUF from field data.
- Location: `core/gguf_reader.h`, `core/gguf_index.h`, `core/gguf_box.h`
- Contains: bulk mmap reader, tensor/offset/size/dtype tables, `GGUFBox` routing table, mock GGUF header
- Depends on: `core/gnn_fan24_model.h`, `core/hyp_fusion.h`
- Used by: `tools/gguf_lazy_serve.c`, `tools/dual_lazy_serve.c`, `tools/gguf_graft_generate.c`, `tools/geo_rid_graft.c`, `tools/tesspack_server.c`

**KV and ghost placement:**
- Purpose: Route live KV-cache and small-slot data through geometric addresses.
- Location: `core/kv_remap.h`, `core/kv_remap_diamond.h`, `core/geo_ghost_lift.h`, `core/geo_ghost_envelope.h`
- Contains: remap tables, diamond-path routing, lift/envelope transforms
- Depends on: geometry address headers
- Used by: KV bench and serve tools

**MoE expert mapping:**
- Purpose: Map (layer, expert, weight-type) to flat field address to disk offset with pure integer math.
- Location: `core/moe_expert_addr.h`, `core/moe_expert_store.h`
- Contains: `moe_expert_to_flat`, `moe_flat_to_expert`, capacity 6912 experts over 20736 flats
- Depends on: `core/geo_tess_wiring.h`
- Used by: `tools/moe_expert_bake.c`, `tools/moe_expert_route.c`, `tools/moe_expert_graft.c`

**Gate descriptors:**
- Purpose: Attach a 12-byte verdict card to each zone. Route load/augment/halt decisions before serving.
- Location: `core/zone_card_v3.h`
- Contains: `ZoneCard3`, 7 reserved specials (RED/BLUE/GREEN/BLACK/WHITE/GOLD/WILDCARD), `ZCardJob` single-use spent mask, hidden-WILDCARD dev flag
- Depends on: stdint/stddef only
- Used by: gate probes and LoRA eval harnesses (`tools/lora_accuracy_probe.c`)

**Zero-copy infra:**
- Purpose: Move bytes without copies. DRAM tile slots, GPU scatter descriptors, rail sync.
- Location: `core/infra/geo_dram_tile.h`, `core/infra/geo_gpu_pipeline.h`
- Contains: tile containers, scatter descriptors, phase/rail sync primitives
- Depends on: core geometry headers
- Used by: bench tools and `--dram` decode paths

## Data Flow

**GGUF pack pipeline:**
1. Parse GGUF tables by mmap — `core/gguf_reader.h`
2. Scatter each tensor with stride-37 KIS mapping — `core/kis_codec_v6.h`
3. Write `.tess` capos with CRC-64 — `tools/tess_bake.c`
4. Bundle capos into one `.tesspack` — `tools/tess_gguf_pack.c`
5. Reassemble standalone GGUF with no source file — `tools/tesspack_assemble.c`

**Lazy serve pipeline:**
1. Bake field file from GGUF — `tools/gguf_lazy_serve.c`
2. Parse KV from field, zero-copy tensor pointers into llama.cpp callback — `core/gguf_box.h`
3. Fault mmap pages on read during generation — `tools/gguf_lazy_serve.c`
4. Co-serve two models in one process with per-model fields and isolated evict — `tools/dual_lazy_serve.c`
5. Serve OpenAI-compatible HTTP directly from `.tesspack` — `tools/tesspack_server.c`

**MoE expert pipeline:**
1. Bake attention projection tensors by geometric address — `tools/moe_expert_bake.c`
2. Select top-K experts per layer through router gate — `tools/moe_expert_route.c`
3. Rebuild GGUF from routed experts, verify logits and tokens bitwise — `tools/moe_expert_graft.c`

**GeoFS file pipeline:**
1. Bond name by trit fold, probe with stride-37 — `core/geofs_mdim.h`
2. Commit runs one journal frame per run — `core/geofs_mdim.h`
3. Switch entry pointer in one atomic frame — `core/geofs_mdim.h`
4. Drive CRUD from CLI — `tools/mdim_cli.c`

## Key Abstractions

**GeoType:**
- Purpose: One enum selects every supported shape from the dodeca root
- Location: `core/geo_param_grid.h`
- Pattern: Parameterized family with props table. `GEO_COMPOUND_144` (V=144, E=576, F=576, C=144) is the working field.

**V6 slot helix:**
- Purpose: Give every weight index a unique slot independent of its value
- Location: `core/kis_codec_v6.h`
- Pattern: `slot(i) = (i*37) % 20736`. 37 is coprime with 20736, so the map is bijective.

**GBA_Address:**
- Purpose: Address the larger field as axis path + position + local box slot
- Location: `core/geo_box_axes.h`
- Pattern: 6 axes (xyz square, ijk triangle). Position is monotonic identity with floor 0. Local is slot in one 20736-cell box.

**Tess capo:**
- Purpose: Address multi-cube tensors as one cube per capo. Cube 0 is index frame.
- Location: `core/geo_tess_container.h`
- Pattern: Fixed 64 B header + 64 B formula + CRC. 1 tesseract = 8 cube × 144 = 1152 slots. 18 tes = 20736.

**MDIM volume and views:**
- Purpose: Expose one byte buffer through five coordinate transforms with full history
- Location: `core/geofs_mdim.h`
- Pattern: Flat/cube/rail/time/cell views are pure arithmetic over the same bytes. Journal frames give `state_at(F)` for free.

**GGUFBox:**
- Purpose: Proxy between llama.cpp and mmap'd model bytes
- Location: `core/gguf_box.h`
- Pattern: Build tensor routing table once. Hand llama.cpp a mock header. Return direct data pointers on request.

**ZoneCard3 and ZCardJob:**
- Purpose: Carry per-zone gate verdict plus single-use job scope
- Location: `core/zone_card_v3.h`
- Pattern: 12 B packed card. Specials live at 0xFFF8–0xFFFE. Job holds a 7-bit spent mask. WILDCARD stays hidden unless dev flag is set.

**MoE flat address:**
- Purpose: Convert expert identity to field slot to disk offset
- Location: `core/moe_expert_addr.h`
- Pattern: `flat = (layer*64*3 + expert*3 + wtype) % 20736`. `offset = flat * BLOCK_SIZE`.

**BreathingSeeker:**
- Purpose: Make seeker position the placement decision
- Location: `core/breathing_fs.h`
- Pattern: Scale sets window (K/scale, floored at 1e-6). Window larger than space means hyperbolic mode.

## Entry Points

**Make targets:**
- Location: `Makefile`
- Triggers: Developer or CI shell invocation
- Responsibilities: Compile and run tiered test groups (`test-smoke`, `test-kis`, `test-tess`, `test-geo`, `test-gguf`, `test-bfs`, `test-cap`, `test-ghost`, `test-kv`, `test-6ico`, `test-fibo`, `test-walk`), build serve/pack/bench binaries (`dual-lazy-serve`, `tess-bake`, `tess-gguf-pack`, `tesspack-assemble`, `moe-bake`, `moe-route`, `graft-*`)

**Pack and serve CLIs:**
- Location: `tools/tess_bake.c`, `tools/tess_load.c`, `tools/tess_assemble.c`, `tools/tess_gguf_pack.c`, `tools/tesspack_assemble.c`, `tools/tesspack_server.c`, `tools/gguf_lazy_serve.c`, `tools/dual_lazy_serve.c`
- Triggers: `make <target>` or direct binary invocation with GGUF/pack paths
- Responsibilities: Encode, decode, assemble, pack, serve, and verify model bytes

**MoE CLIs:**
- Location: `tools/moe_expert_bake.c`, `tools/moe_expert_route.c`, `tools/moe_expert_graft.c`
- Triggers: `make moe-bake`, `make moe-route`, direct invocation
- Responsibilities: Bake experts, route top-K, rebuild and verify inference

**GeoFS CLI:**
- Location: `tools/mdim_cli.c`
- Triggers: `make mdim` then `./build/mdim_cli help`
- Responsibilities: Create, summon, get, list, info, view, history, and unsummon volume entries

**GUI server:**
- Location: `dwgls_gui_server.py`, `dwgls_gui.html`
- Triggers: `launch_dwgls_gui.bat` or direct Python invocation
- Responsibilities: Serve browser GUI over the working tree

**Eval probes:**
- Location: `tools/lora_accuracy_probe.c`, `tools/lora_diverge_probe.c`
- Triggers: Direct binary invocation with base model and adapter paths
- Responsibilities: Measure per-token delta, free-run divergence, and paired accuracy

## Error Handling

**Strategy:** Fail loud on integrity loss. Halt on gate refusal. Prove ratios by decode.

- Verify by full decode-and-compare at every position. Encode-only claims prove nothing.
- Corrupt journal frames and torn commits return explicit corrupt errors. Never roll back partially.
- Gate refusals use negative verdicts (`ZGATE_HALT`, `ZGATE_ALREADY_SPENT`, `ZGATE_HIDDEN`). Callers halt or escalate, never retry in a loop without reroute.
- Tests use explicit pass/fail counters with oracle-derived expectations. Expected values come from spec or math, never from the function under test.

## Cross-Cutting Concerns

**Logging:** Per-phase counters and run receipts in tools. Gate checks emit verdict codes. No central logger.
**Caching:** mmap is the cache. Windowed reads fault pages on demand. Whole-view unmap+remap is the eviction unit on Windows.
**Storage:** `.tess` per-tensor capos, `.tesspack` single-file bundles, MDIM volume files, per-model field binaries. Runtime config lives in `config.json`.
