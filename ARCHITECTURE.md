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
- Location: `core/geo_tess_container.h`, `core/geo_tess_window.h`, `core/geo_tesseract_addr.h`, `core/clim_record.h`
- Contains: magic/version/header structs, capo addressing, mmap pack reader, page-aligned 144 x 144 window views, `TPAK_OFF64` 64-bit pack offsets for >4GB packs, `TESS_SECTION_CLIM` climate-offset section
- Depends on: `core/geo_octant.h`, `core/geo_voronoi_mask.h`
- Used by: `tools/tess_bake.c`, `tools/tess_load.c`, `tools/tess_assemble.c`, `tools/tess_packer.c`, `tools/tess_gguf_pack.c`, `tools/tesspack_assemble.c`, `tools/tess_load_stream.c`, `tools/tess_window_bench.c`

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

**Fold-net walk:**
- Purpose: Model closed solid nets and open fan cartridges via edge gluing on committed face graphs. Pure topological walks, zero float coordinates.
- Location: `core/geo_net_walk.h`
- Contains: `nw_build_fan`, `nw_count_vertices`, `nw_count_edges`, `nw_reciprocal`, `nw_walk`, Euler characteristic checks ($V - E + F$)
- Depends on: stdint/stddef only
- Used by: `tests/test_net_walk.c`, `tests/test_wonder_cube.c`, `tests/test_poly11_oracle.c`, `tests/test_kineticfan_field.c`

**GGUF ingest and graft:**
- Purpose: Parse real GGUF by mmap. Route tensor requests by zero-copy pointer. Rebuild servable GGUF from field data.
- Location: `core/gguf_reader.h`, `core/gguf_index.h`, `core/gguf_box.h`
- Contains: bulk mmap reader, tensor/offset/size/dtype tables, `GGUFBox` routing table, mock GGUF header
- Depends on: `core/gnn_fan24_model.h`, `core/hyp_fusion.h`
- Used by: `tools/gguf_lazy_serve.c`, `tools/dual_lazy_serve.c`, `tools/gguf_graft_generate.c`, `tools/geo_rid_graft.c`, `tools/tesspack_server.c`, `tools/probe_moe_assemble.c`

**KV and ghost placement:**
- Purpose: Route live KV-cache and small-slot data through geometric addresses. Hold one F16 base clipboard and rebuild live state by resume plus teacher-decode token suffix.
- Location: `core/kv_remap.h`, `core/kv_remap_diamond.h`, `core/kv_cold_base.h`, `core/geo_ghost_lift.h`, `core/geo_ghost_envelope.h`
- Contains: remap tables, diamond-path routing, `KVColdBase` HOLD/resume with `KVCB`/`KVD` file headers, lift/envelope transforms
- Depends on: geometry address headers; `core/kv_cold_base.h` calls llama `llama_state_seq_*` in the tool TU
- Used by: KV bench and serve tools, `tools/kv_cold_base.c`, `tools/kv_cold_delta.c`, `tools/kv_cold_reanchor.c`, `tools/kv_cold_prefix.c`, `tools/kv_cold_chat.c`, `tools/kv_delta_map.c`, `tests/test_kv_delta.c`

**MoE expert mapping:**
- Purpose: Map (layer, expert, weight-type) to flat field address to disk offset with pure integer math. Read router gate dims from the file, never hardcode them.
- Location: `core/moe_expert_addr.h`, `core/moe_expert_store.h`
- Contains: `moe_expert_to_flat`, `moe_flat_to_expert`, capacity 6912 experts over 20736 flats
- Depends on: `core/geo_tess_wiring.h`
- Used by: `tools/moe_expert_bake.c`, `tools/moe_expert_route.c`, `tools/moe_expert_graft.c`

**Anchor routing and placement:**
- Purpose: Rank by semantic anchor centroids, place by node-sorted MOD-37 permutation. Same entries produce byte-identical anchors and order.
- Location: `core/anchor_route.h`
- Contains: `anch_assign`, `anch_route` top-b, deterministic Lloyd `anch_train`, `anch_perm` bucket-major perm, `anch_save`/`anch_load` with FNV-1a checksum
- Depends on: stdio/stdlib/string/stdint/float only
- Used by: `tools/gguf_lazy_serve.c` KV-index routing, `tools/anchor_route_cli.c`, `tools/maze_walk_cli.c`, `tests/test_anchor_route.c`, `tests/test_anchor_routed.c`

**Multiverse routing:**
- Purpose: Chain major routes through pinned Peano entry/exit cells. Run minor crossings through unpinned cells with Wang-tile edge gating on the same climate slide.
- Location: `core/mv_node.h`, `core/mm_route.h`, `core/mm_wang.h`, `core/clim_record.h`
- Contains: `MVNode` with entry/exit pin discipline, major chain step, minor cross predicate, deterministic Wang edge colors with on-demand overrides, `ClimRec` window-slide record (`TESS_SECTION_CLIM`)
- Depends on: stdint/stddef/stdio/string only
- Used by: `tests/test_mv_node.c`, `tests/test_mm_route.c`, `tests/test_mm_wang.c`, `tests/test_clim_record.c`

**Gate descriptors:**
- Purpose: Attach a 12-byte verdict card to each zone. Route load/augment/halt decisions before serving.
- Location: `core/zone_card_v3.h`
- Contains: `ZoneCard3`, 7 reserved specials (RED/BLUE/GREEN/BLACK/WHITE/GOLD/WILDCARD), `ZCardJob` single-use spent mask, hidden-WILDCARD dev flag
- Depends on: stdint/stddef only
- Used by: gate probes and LoRA eval harnesses (`tools/lora_accuracy_probe.c`)

**Zero-copy infra:**
- Purpose: Move bytes without copies. DRAM tile slots, GPU scatter descriptors, rail sync, jet merge phase selection.
- Location: `core/infra/geo_dram_tile.h`, `core/infra/geo_gpu_pipeline.h`, `core/infra/jet_select.h`
- Contains: tile containers, scatter descriptors, phase/rail sync primitives, `jet_select` strategy picker (C1 in-place, C3 spin at `WIN ≤ 2L-1`, B phase-align) wired into `geo_pipeline_tick`
- Depends on: core geometry headers
- Used by: bench tools, `--dram` decode paths, `tests/test_gpu_small_batch.c`, `tests/test_jet_select_prod.c`, `tests/test_jet_coalesce_bench.c` (selector math is re-derived independently in `tests/test_jet_phase_align.c`)

## Data Flow

**GGUF pack pipeline:**
1. Parse GGUF tables by mmap — `core/gguf_reader.h`
2. Scatter each tensor with stride-37 KIS mapping — `core/kis_codec_v6.h`
3. Write `.tess` capos with CRC-64 — `tools/tess_bake.c`
4. Bundle capos into one `.tesspack` — `tools/tess_gguf_pack.c`
5. Reassemble standalone GGUF with no source file — `tools/tesspack_assemble.c`

**Lazy serve pipeline:**
1. Bake field file from GGUF with persisted format v2 layout arrays (`kis.format.version`, `kis.layout.fpos`, delta tables `kis.delta.*`) — `tools/dual_lazy_serve.c`, `tools/gguf_lazy_serve.c`
2. Parse KV from field, zero-copy tensor pointers into llama.cpp callback — `core/gguf_box.h`
3. Fault mmap pages on read during generation — `tools/gguf_lazy_serve.c`
4. Serve session slots with per-sid KV reuse (`kv_reused`), page-aligned KV dump/restore, semantic index search, and lifecycle sweep — `tools/gguf_lazy_serve.c` (`POST /v1/state/search`, `GET /v1/state/lifecycle`, `/v1/state/dump`, `/v1/state/restore`, `POST /v1/route`)
5. Route hard prompts upstream with the difficulty scorer and verbatim `TIER_UPSTREAM` forward; expand fused `attn_qkv` into F32 Q/K/V splits through `split_carve` — `tools/gguf_lazy_serve.c`
6. Co-serve two models in one process with per-model fields, layout verification, and isolated evict — `tools/dual_lazy_serve.c`
7. Serve OpenAI-compatible HTTP directly from `.tesspack` with `TESS_NGPU` GPU-layer control, resolving each tensor through the ONION path first (raw F32/F16 entry at `capo_id == 0xFFFFFFFF`, exact-size `memcpy`, no scatter) before falling back to per-capo scatter decode — `tools/tesspack_server.c`
8. Answer prompts straight from a baked field with sourceless delta MoE decoding and no source GGUF — `tools/field_qa.c`
9. Resolve tensor name to chain position to bytes from the field mmap with no llama.cpp — `tools/geo_field_query.c`

**Windowed read pipeline:**
1. Prove one capo is one 20736-cell cube is one 144 x 144 window — `tests/test_window_ladder.c`
2. Map per-window page-aligned views with granularity-rounded offsets — `core/geo_tess_window.h`
3. Measure fread vs whole-file mmap vs windowed views in faults/MB/s/RSS — `tools/tess_window_bench.c`

**MoE expert pipeline:**
1. Bake attention projection tensors by geometric address — `tools/moe_expert_bake.c`
2. Select top-K experts per layer through the router gate with file-derived dims and sequential single-resident eval — `tools/moe_expert_route.c`
3. Rebuild GGUF from routed experts through the zero-copy pointer callback (`t->data` set, never memcpy; CPU `no_host`) and verify logits and tokens bitwise — `tools/moe_expert_graft.c`

**Cold-KV pipeline:**
1. HOLD one F16 base clipboard and prove byte-exact resume plus file roundtrip — `tools/kv_cold_base.c`, `core/kv_cold_base.h`
2. Spill token-id suffix deltas and rebuild live state as resume plus teacher-decode — `tools/kv_cold_delta.c`
3. Re-anchor every 10 generated steps on the K8V4 cold default behind the L2 first-divergence gate — `tools/kv_cold_reanchor.c`
4. Share one prefix-hashed base file across sessions and prove cross-process resume — `tools/kv_cold_prefix.c`
5. Run interactive chat that decodes only new tokens with prefix-HIT resume and re-anchor HOLD — `tools/kv_cold_chat.c`

**Anchor retrieval pipeline:**
1. Prove anchor train/assign/perm/save-load against inline oracles — `tests/test_anchor_route.c`
2. Prove routed search equals brute top-1 with fewer scored items — `tests/test_anchor_routed.c`
3. Run hierarchical PCA→coarse→fine→exact rank on SIFT1M exports — `tools/anchor_route_cli.c`
4. Walk the anchor kNN graph by greedy descent at matched budgets — `tools/maze_walk_cli.c`

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

**Anchor bucket:**
- Purpose: Decide by nearest centroid, place by node-sorted MOD-37 perm
- Location: `core/anchor_route.h`
- Pattern: `anch_assign` ranks, `anch_perm` stores bucket-major with `anch_slot(i) = (i*37) % n`. File carries `K/dim/ntrained/checksum` for deterministic rewrite.

**Cold base clipboard:**
- Purpose: Hold exactly one F16 base, spill tokens not bytes
- Location: `core/kv_cold_base.h`
- Pattern: `live(t_n) = resume(base@t0) + teacher-decode(tokens[t0..t_n])`. HOLD replaces, `clear` empties, files are `KVCB` base plus `KVD` token-suffix delta.

**Multiverse node:**
- Purpose: Pin major entry/exit, leave minors the rest
- Location: `core/mv_node.h`
- Pattern: `MVNode` is `(node, layer, slide)` plus Peano endpoints. Majors chain `exit→entry`; minors cross unpinned edge-adjacent cells at `|dlayer| ≤ 1` behind matching Wang colors.

**BreathingSeeker:**
- Purpose: Make seeker position the placement decision
- Location: `core/breathing_fs.h`
- Pattern: Scale sets window (K/scale, floored at 1e-6). Window larger than space means hyperbolic mode.

## Entry Points

**Make targets:**
- Location: `Makefile`
- Triggers: Developer or CI shell invocation
- Responsibilities: Compile and run tiered test groups (`test-smoke`, `test-kis`, `test-tess`, `test-geo`, `test-gguf`, `test-bfs`, `test-cap`, `test-ghost`, `test-kv`, `test-6ico`, `test-fibo`, `test-walk`, `GEO_FAST` including `test_kineticfan_field`, `test_clim_record`, `test_mv_node`, `test_mm_route`, `test_mm_wang`), build serve/pack/bench binaries (`dual-lazy-serve`, `tess-bake`, `tess-gguf-pack`, `tesspack-assemble`, `tess-window-bench`, `moe-bake`, `moe-route`, `graft-*`, `anchor-route`, `kv-delta-proof`, `kv-cold-base`, `kv-cold-delta`, `kv-cold-reanchor`, `kv-cold-prefix`, `kv-cold-chat`)

**Pack and serve CLIs:**
- Location: `tools/tess_bake.c`, `tools/tess_load.c`, `tools/tess_assemble.c`, `tools/tess_gguf_pack.c`, `tools/tesspack_assemble.c`, `tools/tesspack_server.c`, `tools/gguf_lazy_serve.c`, `tools/dual_lazy_serve.c`, `tools/field_qa.c`, `tools/geo_field_query.c`, `tools/tess_window_bench.c`, `tools/gguf_tnames.c`, `tools/bake_q4.c`
- Triggers: `make <target>` or direct binary invocation with GGUF/pack/field paths
- Responsibilities: Encode, decode, assemble, pack, quantize, serve, query, bench, and verify model bytes

**MoE CLIs:**
- Location: `tools/moe_expert_bake.c`, `tools/moe_expert_route.c`, `tools/moe_expert_graft.c`
- Triggers: `make moe-bake`, `make moe-route`, direct invocation
- Responsibilities: Bake experts, route top-K with file-derived dims, rebuild through the zero-copy callback and verify inference

**Cold-KV CLIs:**
- Location: `tools/kv_cold_base.c`, `tools/kv_cold_delta.c`, `tools/kv_cold_reanchor.c`, `tools/kv_cold_prefix.c`, `tools/kv_cold_chat.c`, `tools/kv_delta_map.c`, `tools/kv_dump_turns.c`, `tools/kv_quant_3level.c`, `tools/kv_quant_threshold.c`
- Triggers: `make kv-cold-base`, `make kv-cold-delta`, `make kv-cold-reanchor`, `make kv-cold-prefix`, `make kv-cold-chat`, `make kv-delta-proof`, direct invocation
- Responsibilities: Hold and resume the F16 base, spill token-suffix deltas, re-anchor cold generation, share prefix bases, map the one-token footprint, and measure KV-quant deviation with the 3-level (KL / greedy-match / seeded divergence) gate

**Retrieval CLIs:**
- Location: `tools/anchor_route_cli.c`, `tools/maze_walk_cli.c`
- Triggers: `make anchor-route`, direct invocation
- Responsibilities: Rank SIFT1M queries through hierarchical anchors and walk the anchor kNN graph at matched budgets

**GeoFS CLI:**
- Location: `tools/mdim_cli.c`
- Triggers: `make mdim` then `./build/mdim_cli help`
- Responsibilities: Create, summon, get, list, info, view, history, and unsummon volume entries

**GUI server:**
- Location: `dwgls_gui_server.py`, `dwgls_gui.html`
- Triggers: `launch_dwgls_gui.bat` or direct Python invocation
- Responsibilities: Serve browser GUI over the working tree

**Eval probes:**
- Location: `tools/lora_accuracy_probe.c`, `tools/lora_diverge_probe.c`, `tools/merge_probe.c`, `tools/probe_moe_assemble.c`
- Triggers: Direct binary invocation with base model, adapter, single-model paths, or a GGUF + `.tesspack` pair
- Responsibilities: Measure per-token delta, free-run divergence, paired accuracy, issue a mechanical load + greedy-decode graft verdict, and replicate `tesspack_server` assemble decisions per tensor with a printed `rc` receipt for every failing tensor

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
