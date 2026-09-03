# DWGLS vs FGLS_new — What's Unique

> **FGLS_new** = original project at `I:\FGLS_new`
> **DWGLS** = current fork at `.\` (branch: `feat/geo-native-fs`)
> **Scanned:** 2026-09-01

---

## Summary

| Category | FGLS_new | DWGLS | Diff |
|---|---|---|---|
| Core headers | 86 unique | 104 unique | +18 shared |
| infra/ headers | 0 | 16 | **entirely new** |
| Test files | 6 | 200 | **+194 new tests** |
| Tools (.c) | 0 | 100+ | **entirely new** |
| Proven on real model | No | Yes (Qwen3-4B-MoE) | **new capability** |

---

## What DWGLS Added (Not in FGLS_new)

### 1 — Infrastructure Layer (`core/infra/`) — ENTIRELY NEW

The original FGLS_new had **no infra/ directory**. DWGLS created 16 infrastructure files:

| File | What It Provides |
|---|---|
| `gear_lock.h` | **GEAR_GEO_FULL = 20736** — the universal constant. CPU/GPU tick counters |
| `fibo_spine.h` | **1728 pipes × 12 ticks** — the entire Fibo Spine pipeline with Jet Bridge |
| `geo_config.h` | **All geometry constants** — single source of truth (SPOKES=6, SLOTS=576, FULL_N=3456) |
| `geo_dram_tile.h` | **DRAM Tile** — anchor×128 + Hilbert(8×8) addressing |
| `geo_spoke_sync.h` | Spoke synchronization for pipeline |
| `geo_phase_rail.h` | Phase rail for timeline |
| `geo_rail_ring.h` | Rail ring layout |
| `geo_rail_sync.h` | Rail synchronization |
| `gear_shift.h` | Gear shifting primitives |
| `rdh_addr.h` | **RDH mixed-radix** — ring×wedge addressing (the infra copy) |
| `tring.h` | **Timeline Ring** — sparse tick-indexed variable-size storage |
| `dramtile_store.h` | DRamTile store declarations |
| `dramtile_container.h` | DRamTile container |
| `geo_config.h` | Config system |
| `config.h/c` | Configuration |
| `cJSON.h/c` | JSON parser (vendored) |

**Key insight:** FGLS_new had no `gear_lock.h`, no `fibo_spine.h`, no `geo_config.h` — the entire 20736 address space concept was built in DWGLS.

### 2 — Addressing System — ENTIRELY NEW

| File | What It Provides |
|---|---|
| `geo_tesseract_addr.h` | **4D tesseract addressing** — 18tes × 8cube × 144 |
| `geo_tess_wiring.h` | **Rescope ↔ physical wiring** — tess_to_flat, flat_to_tess, index frame, passive log |
| `geo_tess_container.h` | **TESS binary format** — 64B header, formula block, CRC-64 |
| `geo_cube_addr.h` | **Generation-indexed cube** — (gen, face, slot) with w(time) |
| `geo_cell_addr.h` | **Cell addressing** — 14-bit flat, cube↔rail zero-copy |
| `geo_cell_classify.h` | **3-bit parity cell types** — III, IID, IDI...DDD |
| `geo_cell_prune.h` | Cell pruning |
| `moe_expert_addr.h` | **MoE expert ↔ geometry** — pure O(1) bijection |
| `geo_rdh_addr.h` | **RDH bijection** — block×256+from, reversible |
| `rdh_addr.h` | RDH infrastructure layer |
| `rdh_capture.h` | RDH name capture |

**Key insight:** FGLS_new had no tesseract addressing, no cell addressing, no MoE addressing, no RDH bijection. These are all DWGLS inventions.

### 3 — Ghost Lift System — ENTIRELY NEW

| File | What It Provides |
|---|---|
| `geo_ghost_envelope.h` | **ROI model** — envelope depth, footprint(k), gate-based lift decision |
| `geo_ghost_lift.h` | **Ghost log** — 5B entries, pair table accelerator, bond mapping |
| `geo_ghost_gear_adapter.h` | Ghost ↔ gear adapter |
| `ghost_delta.h` | **Delta compression** — subsample-2 + Huffman |
| `geo_cap_account.h` | **Capacity accounting** — ADMIT/REJECT/LIFT verdicts |
| `geo_cap_chain.h` (in tools) | Cap chain scanning |

**Key insight:** FGLS_new had no ghost lift, no envelope model, no capacity accounting. The entire "lift vs admit" decision system was built in DWGLS.

### 4 — Hyperbolic Walk — ENTIRELY NEW

| File | What It Provides |
|---|---|
| `geo_hyperbolic_walk.h` | **Centroid walk** — 4 axes (1,9,27,81), deterministic, reversible |
| `geo_hyperbolic_store.h` | **Key-frame grid** — centroid reconstruction, path walking |
| `hyperbolic_seek.h` | Hyperbolic seeking |
| `tri_hex_tess.h` | Triangle-hex tessellation |
| `hyp_fusion.h` | **Fusion** — S1 address + S2 gate + S3 weight in one decision |
| `iso_fold.h` | Isometric folding |
| `iso_rot90.h` | 90° rotation |
| `geo_chord.h` | Chord addressing |

**Key insight:** FGLS_new had no hyperbolic walk, no centroid reconstruction, no fusion system. The entire "walk the triangle field" capability was built in DWGLS.

### 5 — GeoFS (Geometric Filesystem) — ENTIRELY NEW

| File | What It Provides |
|---|---|
| `geofs_core.h` | **Full filesystem** — create/read/write/delete/summon on 20736 address space |
| `geofs_mdim.h` | Multi-dimensional GeoFS |
| `geofs_multivol.h` | Multi-volume support |
| `geo_fs_voronoi.h` | Voronoi-based filesystem |
| `geo_ggf_fs.h` | GGF-backed filesystem |
| `geo_zerocopy.h` | Zero-copy primitives |

**Key insight:** FGLS_new had no filesystem. The entire GeoFS was built in DWGLS.

### 6 — MoE System — ENTIRELY NEW

| File | What It Provides |
|---|---|
| `moe_expert_addr.h` | **Expert ↔ geometry** bijection |
| `moe_expert_store.h` | **Expert storage** on DtSlotRegion |
| `tied_dedup.h` | **Dedup registry** — byte-identical tensors, freeze once |

**Key insight:** FGLS_new had no MoE support. The entire expert addressing, storage, and dedup was built in DWGLS.

### 7 — Goldberg Storage Evolution

| File | FGLS_new | DWGLS |
|---|---|---|
| `geo_goldberg_decagram.h` | ❌ | ✅ **10-sector layout** — exact n²−1 per sector |
| `geo_goldberg_store.h` | ❌ | ✅ **Streaming multi-sphere** — RAM ≈ 1 sphere |
| `geo_goldberg_file.h` | ❌ | ✅ **.ggf persistence** — 3 read modes |
| `geo_ggf_walk.h` | ❌ | ✅ **Walk clock** — seed,round,tick |
| `geo_ggf_ckpt.h` | ❌ | ✅ **Checkpoint/replay** — manifest + CRC64 |
| `geo_goldberg_lut.h` | ✅ shared | ✅ shared |
| `geo_goldberg_sphere.h` | ✅ shared | ✅ shared |

**Key insight:** FGLS_new had basic Goldberg sphere + LUT. DWGLS added decagram layout, streaming store, file persistence, lazy read, mmap, and checkpoint/replay.

### 8 — Codec Evolution

| File | FGLS_new | DWGLS |
|---|---|---|
| `kis_codec.h` | ✅ (legacy) | removed |
| `kis_codec_v3.h` | ✅ (legacy) | removed |
| `kis_codec_v4.h` | ✅ shared | ✅ shared (lossless proven) |
| `kis_codec_v5.h` | ✅ shared | ✅ shared |
| `kis_codec_v6.h` | ✅ shared | ✅ shared |
| `kis_codec_v6b.h` | ❌ | ✅ **new version** |
| `codec_tess.h` | ❌ | ✅ **tess codec** |
| `diamond_shell_codec.h` | ❌ | ✅ |
| `diamond_shell_v2.h` | ❌ | ✅ |
| `dwgls_codec_*.h` (8 files) | ❌ | ✅ **DWGLS-specific codecs** |
| `huff_codec.h` | ❌ | ✅ **Huffman codec** for delta |

**Key insight:** DWGLS cleaned up legacy codecs (removed v3), kept proven v4/v5/v6, and added tess, diamond shell, and DWGLS-specific codecs.

### 9 — Unified Volume — ENTIRELY NEW

| File | What It Provides |
|---|---|
| `geo_unified.h` | **Single volume** — DRamTile + RDH + GearLock unified, <10ns ops |
| `geo_fast.h` | Fast geometric operations |
| `geo_mdim.h` | Multi-dimensional geometry |
| `geo_monitor.h` | Monitoring |

**Key insight:** FGLS_new had no unified volume. The "three views into 20736" concept was built in DWGLS.

### 10 — BFS & Breathing FS — ENTIRELY NEW

| File | What It Provides |
|---|---|
| `bfs_breath.h` | BFS breathing patterns |
| `bfs_persist.h` | BFS persistence |
| `bfs_seek_anchor.h` | BFS seek anchor |
| `breathing_fs.h` | Breathing filesystem |

**Key insight:** FGLS_new had no BFS system.

### 11 — KV Bridge — ENTIRELY NEW

| File | What It Provides |
|---|---|
| `kv_dramtile_bridge.h` | KV ↔ DRamTile bridge |
| `kv_geofs_bridge.h` | KV ↔ GeoFS bridge |
| `kv_remap.h` | KV remapping |
| `kv_remap_diamond.h` | KV diamond remap |
| `kv_remap_rail.h` | KV rail remap |

---

## What FGLS_new Had That DWGLS Removed

| File | What It Was |
|---|---|
| `bermuda_export.h` | Bermuda triangle export |
| `bermuda_shadow.h` | Bermuda shadow |
| `bond_to_geopixel.h` | Bond → geopixel mapping |
| `fabric_wire.h` | Fabric wire routing |
| `fabric_wire_drain.h` | Fabric wire drain |
| `fgls_tensor_archive.h` | Tensor archive format |
| `fgls_twin_store.h` | Twin store |
| `geo_addr_net.h` | Address network |
| `geo_compound_cfg.h` | Compound config |
| `geo_diamond_to_scan.h` | Diamond → scan |
| `geo_fec_rs.h` | Forward error correction |
| `geo_field_core.h` | Field core |
| `geo_flow_chunker.h` | Flow chunker |
| `geo_goldberg_tile.h` | Goldberg tile (replaced by decagram) |
| `geo_gp_frustum_bridge.h` | GP ↔ frustum bridge |
| `geo_gpx_anim.h` | GPX animation |
| `geo_letter_cube.h` | Letter cube |
| `geo_metatron_reshape.h` | Metatron reshape |
| `geo_metatron_route.h` | Metatron route |
| `geo_o4_connector.h` | O4 connector |
| `geo_onion_shell.h` | Onion shell |
| `geo_payload_store.h` | Payload store |
| `geo_pipeline_wire.h` | Pipeline wire |
| `geo_pixel.h` | Geopixel |
| `geo_rewind_wang.h` | Rewind Wang |
| `geo_store_reader.h` | Store reader |
| `geo_temporal_lut.h` | Temporal LUT |
| `geo_temporal_ring.h` | Temporal ring |
| `geo_tring_addr.h` | Tring address |
| `geo_tring_fec.h` | Tring FEC |
| `geo_tring_goldberg_wire.h` | Tring ↔ Goldberg wire |
| `geom_raw_bridge.h` | Raw bridge |
| `geom_router_bridge.h` | Router bridge |
| `geom_shadow_pipe.h` | Shadow pipe |
| `geom_weight_reconstruct.h` | Weight reconstruct |
| `geopixel_session_feed.h` | Geopixel session feed |
| `goldberg_shutter.h` | Goldberg shutter |
| `gpx4_container.h` | GPX4 container |
| `gpx5_container.h` | GPX5 container |
| `hamburger_encode.h` | Hamburger encoding |
| `heptagon_fence.h` | Heptagon fence |
| `hex_codec.h` | Hex codec |
| `hybrid_silk_selector.h` | Hybrid silk selector |
| `kis_chunk_codec.h` | Chunk codec |
| `kis_codec.h` | Legacy codec (replaced by v4) |
| `kis_codec_v3.h` | V3 codec (replaced by v4) |
| `kis_geom_codec.h` | Geom codec |
| `kis_geom_simple.h` | Simple geom codec |
| `kis_sort_mask.h` | Sort mask |
| `lc_delete.h` | LC delete |
| `lc_fs.h` | LC filesystem |
| `lc_gcfs_wire.h` | LC ↔ GCFS wire |
| `lc_hdr.h` | LC header |
| `lc_hdr_lazy.h` | LC lazy header |
| `lc_twin_gate.h` | LC twin gate |
| `lc_wire.h` | LC wire |
| `lcgw_adaptive.h` | LCGW adaptive |
| `llama_pogls_backend.h` | Llama backend |
| `pogls_1440.h` | POGLS 1440 |
| `pogls_atomic_reshape.h` | Atomic reshape |
| `pogls_bond_chain.h` | Bond chain |
| `pogls_geofield_export.h` | Geofield export |
| `pogls_model_index.h` | Model index |
| `pogls_pipeline.h` | Pipeline |
| `pogls_qrpn_phaseE.h` | QRPN phase E |
| `pogls_recon_file.h` | Recon file |
| `pogls_rotation.h` | Rotation |
| `skeleton_index.h` | Skeleton index |
| `tensor_track.h` | Tensor tracking v1 |
| `tensor_track_v1.h` | Tensor tracking v1 |
| `tgw_*.h` (10 files) | TGW dispatch/wire variants |

**Key insight:** DWGLS removed ~86 headers from FGLS_new. Most were legacy/unused: fabric wire, geopixel, metatron, hamburger, onion shell, temporal ring, etc. DWGLS cleaned up and focused on the core primitives.

---

## What's Shared (Both Projects)

| File | Status |
|---|---|
| `pogls_bond.h` | ✅ Bond system (foundation) |
| `pogls_config.h` | ✅ Config |
| `kis_codec_v4.h` | ✅ Lossless codec (proven) |
| `kis_codec_v5.h` | ✅ V5 codecs |
| `kis_codec_v6.h` | ✅ V6 codecs |
| `hex_tile.h` | ✅ Hex tile classification |
| `geo_param_grid.h` | ✅ Geometry grid |
| `geo_frame_seek.h` | ✅ Frame seek |
| `geo_frame_seek_wang.h` | ✅ Wang edge |
| `geo_dual_place.h` | ✅ Dual placement |
| `geo_adaptive_store.h` | ✅ Adaptive storage |
| `geo_kis_container.h` | ✅ KIS container |
| `geo_tring_walk.h` | ✅ TRing walk |
| `geo_hex_layer.h` | ✅ Hex layer |
| `geo_goldberg_lut.h` | ✅ Goldberg LUT |
| `geo_goldberg_sphere.h` | ✅ Goldberg sphere |
| `geo_diamond_field_v4.h` | ✅ Diamond field |
| `frustum_gcfs.h` | ✅ Frustum |
| `frustum_layout_v2.h` | ✅ Frustum layout |
| `beam_entropy_container.h` | ✅ Beam entropy |
| `entropy_container.h` | ✅ Entropy container |
| `fibo_tick.h` | ✅ Fibo tick |
| `lc_tantrix.h` | ✅ Tantrix |

---

## Unique to DWGLS: The Proven New Capabilities

| Capability | What It Does | Proven On |
|---|---|---|
| **20736 Address Space** | Universal field, 6 views, zero-copy | All subsystems |
| **Fibo Spine** | 1728 pipes × 12 ticks, Jet Bridge | Pipeline tests |
| **Tesseract 4D** | 18tes × 8cube × 144 addressing | 30/30 tests |
| **MoE Expert** | expert_id ↔ geometry bijection | Qwen3-4B (108/108) |
| **Ghost Lift** | ROI model, capacity accounting | 4 GGUF models |
| **Hyperbolic Walk** | Centroid reconstruction, 4 axes | Probe verified |
| **GeoFS** | Geometric filesystem on 20736 | G1-G4 drills |
| **DRAM Tile** | Hilbert curve addressing | Zero collision |
| **Residual Space** | Bond-keyed frozen storage | Unit tests |
| **Tied Dedup** | Byte-identical tensor dedup | Unit tests |
| **RDH Bijection** | block×256+from reversible | Sweep 2^24 |
| **Capacity Account** | ADMIT/REJECT/LIFT verdicts | 0 rejects on 4 GGUF |
| **Goldberg Decagram** | 10-sector exact layout | Lossless verify |
| **GGF Persistence** | 3 read modes (save/lazy/mmap) | File tests |
| **Delta Compression** | Subsample-2 + Huffman | Encode/decode |
| **Unified Volume** | DRamTile+RDH+GearLock <10ns | Pointer verify |
| **KV Bridge** | KV ↔ DRamTile/GeoFS | Bridge tests |
| **BFS Breathing** | Breathing filesystem patterns | Stability tests |
| **MoE Pipeline** | Bake→Graft→Stream tools | Inference identical |

---

## TL;DR

**FGLS_new** was a research project with many experimental headers (metatron, hamburger, geopixel, fabric wire, temporal ring, etc.) and only 6 test files.

**DWGLS** stripped out the experimental cruft (~86 headers removed) and built **21 proven subsystems** on top of the shared foundation (pogls_bond, kis_codec, hex_tile, geo_frame_seek, etc.). The biggest additions:

1. **The 20736 address space** (infra/) — didn't exist before
2. **Tesseract 4D addressing** — didn't exist before
3. **MoE expert system** — didn't exist before
4. **Ghost lift + capacity** — didn't exist before
5. **Hyperbolic walk** — didn't exist before
6. **GeoFS** — didn't exist before
7. **200 test files** (vs 6) — massive test coverage expansion
