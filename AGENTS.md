# AGENTS.md — DWGLS (4Dimension Geometry + KIS Timeline)

## 🎯 Core Architecture

### Parameterized Geometry Layer (`core/geo_param_grid.h`)

**One family: Dodeca Root** → all shapes derive from the same parent.

| GeoType | verts | edges | faces | cells | Notes |
|---------|-------|-------|-------|-------|-------|
| GEO_DODEC_BASE | 20 | 30 | 12 | 1 | dodecahedron (root) |
| GEO_ICO_BASE | 20 | 30 | 20 | 1 | icosahedron (dual) |
| GEO_COMPOUND_24 | 24 | 48 | 24 | 6 | inverted dodeca compound |
| GEO_DODEC_EDGES | 30 | 60 | 32 | 1 | edge-based |
| GEO_COMPOUND_60 | 60 | 90 | 32 | 1 | pentakis dodeca |
| GEO_PENTAKIS_72 | 72 | 90 | 32 | 1 | 12 base + 60 pyramids |
| GEO_GOLDBERG_92 | 92 | 270 | 92 | 1 | goldberg dual |
| GEO_COMP_SPIKE_120 | 120 | 180 | 62 | 1 | spike compound |
| GEO_GOLDBERG_132 | 132 | 270 | 92 | 1 | goldberg level 2 |
| **GEO_COMPOUND_144** | **144** | **576** | **576** | **144** | **★ 6ico = 18tes (protagonist)** |
| GEO_GOLDBERG_192 | 192 | 270 | 92 | 1 | goldberg level 3 |

**★ 6ico Compound (GEO_COMPOUND_144) — The Protagonist**
- V=144 · E=576 · F=576 · C=144
- "18tes" — 18-triangle tessellation field
- This is the WORKING field for KIS-timeline

**Mechanism:**
- Parameters before entry — you choose GeoType → selects shape
- `sort → distinct count → codebook size`
- `mask = how many distinct values fit in geometry vertices`
- **No hash, no lookup** — coordinate = address

### KIS-Timeline (`core/kis_codec_v4/v5/v6.h`)

**KIS = FIELD, not pipeline.**

```
∞ ← contraction ← 0 ← expansion → ∞
                    ↑
              enter anywhere
```

- **No start, no end, no zero entry point** — enter ANYWHERE
- **Forward** = expansion (spike → more vertices)
- **Backward** = contraction (seal → fewer vertices)
- **Like a balance scale** — place data anywhere on 0-20736
- **6 values same position** = 6 data points from different topology
- **Direction = value** = path data came from

**Loop transition:** dodeca ↔ icosahedron through spike vertex
```
Dodeca (12 pent) → spike → Ico-like (60 faces)
Ico (20 tri) → spike → Dodec-like (60 faces)
```

**Infinite alternation: Ico ↔ Dodec through spiking**
- Spike = operation that transforms duals (face ↔ vertex)
- h-depth: spike = h→0 (infinite resolution), sealed = h→R (finite)

### Core Principle

> **MAP not COMPRESS** — Geometry IS the address space.
> Coordinate = data. No hash, no collision, no lookup table.

## 📁 Structure

```
DWGLS/
├── core/           (17 headers)
│   ├── 4D Geometry (9)
│   │   ├── geo_param_grid.h      ← PARAMETERIZED GEOMETRY (start here)
│   │   ├── geo_dual_place.h      ← Hilbert+Peano 162→64 mapping
│   │   ├── geo_goldberg_sphere.h ← Goldberg polyhedra (gp_lens — GpSphere)
│   │   ├── geo_goldberg_lut.h    ← Goldberg lookup tables (bipolar pairs)
│   │   ├── geo_diamond_field_v4.h ← Diamond geometry
│   │   ├── frustum_gcfs.h        ← Frustum geometry
│   │   ├── frustum_layout_v2.h   ← Frustum layout
│   │   ├── geo_hex_layer.h       ← Hexagonal geometry
│   │   └── geo_tring_walk.h      ← Tring walk patterns
│   │
│   ├── KIS Timeline (8)
│   │   ├── kis_codec_v4.h        ← LOSSLESS proven on real GGUF
│   │   ├── kis_codec_v5.h        ← v5 codecs
│   │   ├── kis_codec_v6.h        ← v6 codecs
│   │   ├── geo_adaptive_store.h  ← Adaptive storage engine
│   │   ├── geo_kis_container.h   ← Container format (CRC-64)
│   │   ├── beam_entropy_container.h ← Beam code v2
│   │   ├── entropy_container.h   ← Entropy container
│   │   └── geofs_core.h          ← ★ GeoFS: geometric filesystem
│   │
│   └── Goldberg Storage (7) — T1.2f..o (layered DAG, bottom-up)
│       ├── geo_goldberg_decagram.h ← 10-sector layout: hex = 10(n²−1) ÷10
│       ├── geo_goldberg_store.h   ← ggs_store: streaming multi-sphere (RAM ~1 sphere)
│       ├── geo_goldberg_file.h    ← .ggf persist (ggs_save/load) + GGFReader lazy
│       │                            + GGFMap mmap (zero-copy) + ggf_save_map
│       ├── geo_ggf_walk.h         ← single read path: walk clock (seed,round,tick)
│       │                            + dedup registry + GGFMap zero-copy
│       ├── geo_ggf_ckpt.h         ← checkpoint/replay manifest (.mfp, provenance
│       │                            + CRC64) — fresh-process restore
│       ├── fibo_walk.h            ← walk clock: state = (seed, round, tick), live route
│       ├── moe_expert_addr.h     ← ★ MoE: expert_id ↔ geometry coordinate (pure int O(1))
│       ├── moe_expert_store.h    ← MoE: store/load on DtSlotRegion
│       └── tied_dedup.h           ← registry {tensor_id → home} (dedup ระดับไฟล์)
│
│   └── Platonic Field (4) — 4D mutual indexing, bipolar compression
│       ├── geo_octant.h          ← Phase 1: octant identity + zero-sum binding
│       ├── geo_tesseract_dense.h ← Phase 3: 1 tesseract, deterministic, bipolar 1/2
│       ├── geo_voronoi_mask.h    ← Phase 4: Voronoi pointer masking (24 cells × 864)
│       └── geo_fs_voronoi.h      ← Voronoi cell cache (LRU, hot/cold, existing)
│
├── core/infra/     (2 headers)
│   ├── gear_lock.h               ← GEAR_GEO_FULL = 20736
│   └── fibo_spine.h              ← FS_PIPES = 1728, FS_TICKS = 12
│
└── tests/          (TIER1 ~93 ไฟล์ — make test)
    ├── kis_codec_v4/v5/v6_test.c · kis_adaptive_deploy.c · kis_real_gguf_test.c
    ├── kis_map_roundtrip.c · section4_seal_residual.c · test_geo_fs.c
    ├── test_fibo_walk.c · test_tied_dedup.c · test_geo_dual_view.c
    ├── test_goldberg_decagram.c · test_goldberg_store.c · test_goldberg_file.c
    ├── test_goldberg_lazy.c · test_goldberg_mmap.c · test_ggf_walk.c
    ├── test_ggf_walk_mmap.c · test_ggf_ckpt_replay.c
    └── test_18tes_field.c · test_moe_expert.c · test_6ico_integration.c
```

## 🧭 Working Rules

### Geometry Constants (Sacred)
- **12**: dodecahedron base (12 faces)
- **20**: icosahedron base (20 faces)
- **24**: compound dodeca (inverted)
- **30**: edge count (both base)
- **60**: pentakis / compound-60
- **72**: pentakis-72
- **92**: goldberg-92
- **120**: spike compound
- **132/192**: goldberg levels
- **144**: 6ico compound (★ protagonist, 18tes)
- **576**: edges+faces of 6ico compound

### Coordinate = Address
- Geometry provides: mask bit per vertex (which slots used) + addressing
- No hash functions allowed for weight mapping
- No lookup tables for address resolution (LUT only for static geometry)

### Verification
- Lossless = decode → compare every value at every position
- `geo_codec_verify()` = binary truth
- Ratio < 1.0 must prove via decode (never trust encode-only)

### Test Integrity (ห้าม expected values แบบ circular)
- **expected ต้องมาจาก oracle อิสระเท่านั้น**: spec / คณิตศาสตร์ / ข้อมูลต้นทาง / reference implementation
  — ห้ามมาจากฟังก์ชันที่กำลังเทส (f(x) == f(x) = tautology, เทสไม่มีทาง fail)
- **ห้าม "run แล้วแปะ output เป็น expected"** — characterization test แบบนี้ freeze bug ให้เข้ากล่อง
  (ตัวเลข {1,3,7,27} ที่ copy จาก implementation constant มาเป็น expected = ผิดแบบเดียวกัน)
- **ห้าม copy comment/spec จาก implementation ไปใส่ใน test** — spec ต้องมาก่อน code ไม่ใช่เอามาจาก code
- **ทุกเทสต้อง fail ได้จริง**: mutation check — เปลี่ยน logic 1 บรรทัดใน core → เทสต้องแดง
- เทส wrapper ที่เทียบ `adapter(x)` กับ `core(x)` ที่ adapter เรียกข้างใน ต้องมี assertion อิสระ
  ของ core ด้วย (ตรวจ formula จริง เช่น bijection/permutation หรือค่า hand-computed ที่รู้แล้ว)
- ระวังกับดัก: เทสที่ตรวจแค่ "ค่าเดิมถูก persist กลับมาเหมือนเดิม" (เช่น expected = frame_enc(...)
  เทียบกับ field ที่ set ด้วย frame_enc เดียวกัน) พิสูจน์ได้แค่ wiring ไม่ได้พิสูจน์ formula

### Experiments = History (ห้ามลบ experiment)
- **ทุก experiment/debug scratch เก็บไว้เป็นประวัติ** — เอาไปเป็นบทเรียนได้ทีหลัง
- กฎ: experiment → ไปไว้ที่ `deprecated/` (ไม่ commit ลง main history, แต่ห้าม delete)
- ห้ามลบไฟล์ experiment แม้จะ "ไม่ใช้งานแล้ว / สร้างใหม่ได้" — ย้ายไป `deprecated/` แทน
- ลบได้เฉพาะ artifact ใหญ่ที่ regenerable จริง (เช่น .tesspack/.gguf ที่ bake ใหม่ได้) และต้องถามก่อนถ้าไม่ชัวร์

### Design Principles (Timeline-First)
- เลือกใช้ **timeline-first**: int, base-2 scale, ไม่มี 0, ทุกสถานะ deterministic + replay ได้
- hyperbolic/residual = เก็บส่วนต่างที่ explicit; geometry = template เท่านั้น (ไม่ใช่ตัวคำนวณ)
- เหตุผล + หลักฐาน: `docs/TIMELINE_FIRST_FOUNDATION.md`

## 🧭 Rescope — Scale Timeline + 1 Tesseract (2026-08-14)

> เราไม่ได้สร้าง geometry — ใช้โครงสร้าง combinatorial เป็น template ในการ map ข้อมูลเท่านั้น

### Scale = Constant Magnification Rate (ไม่ใช่ geometry)
- scale = อัตราการขยายคงที่ (multiplicative): `s(t) = s₀·kᵗ` — **ไม่มี 0, ไม่มีที่สิ้นสุด**
- ico/dodeca sealed/spike = พาหะนำเสนอเท่านั้น ไม่ใช่แก่นแท้
- หน้าต่างที่เลือกใช้ = `(0, 20736)`; 20736 = 144² = 1728×12 = 18 tes × 8 cube × 144
- ทุกอย่างขยับพร้อมกันหมด (global scale เดียว) → **append ไม่ต้อง tag scale**

### Hyperbolic Side = Passive Scale-Change Log
- ฝั่ง hyperbolic เก็บ **log ของ scale-change events** (แต่ละ entry = route/path สั้นๆ ของ transform)
- delta ∝ จำนวน scale-change events ไม่ใช่ขนาดข้อมูล → จุดที่ compression ได้จริง
- อ่านที่ scale ตรงกับตอน append → lossless ตรงๆ (log ว่าง)
- อ่านที่ scale ไม่ตรง → replay log (deterministic) → lossless อีกครั้ง

### 1 Tesseract = Frame-as-Index (scope ปัจจุบัน)
- **1 tesseract = 8 cube × 144 slots = 1152** (= 8 vertex × 144 scale positions)
- **cube 0 = index frame** (144 slots = 8 blocks × 18): base/len/stride(route)/checksum ของทุก cube
- cube 1..7 = data (1008 slots), scatter ด้วย route (stride coprime กับ 144 → เดินครบ)
- **มอง 1 frame → retrieve ครบ 8 cube** — lossless, deterministic
- **Magnify glass**: 20736÷4 = 5184 (36 scales × 144 vertices); กลาง window = glass,
  อัตรา invert กับฝั่งตรงข้าม (a_w × a_{w+72} ≡ 1 mod 144) + offset เล็กน้อย;
  ฝั่งตรงข้าม = hyperbolic side (compressed, เก็บ delta log)
- **Delta จริง (hex_tile)**: scale เปลี่ยน → hyperbolic side เก็บ residual layer
  ของ view (hex_tile predict+residual, 144 tiles × 7 cells); replay = decode → lossless;
  ฝั่งตรงข้าม (antipode) = เก็บ delta ครบ; structured data → delta เล็ก (FLAT tiles)
- พิสูจน์แล้ว: `tests/test_tess_index_frame.c` (7/7), `test_tess_scale_log.c` (10/10),
  `test_tess_frame_seek.c` (8/8), `test_tess_magnify.c` (12/12), `test_tess_hex_delta.c` (10/10)

### 18tes (6ico compound) = DONE
- 18 tesseracts × 8 cube × 144 = 20736 — full field tested `tests/test_18tes_field.c` (30/30)

### Working Rule: No Geometry Construction
- ห้าม compute vertex/face/projection/coordinate จริง — ใช้แค่โครงสร้าง (cube/octant/route/vertex)
  เป็นโครงร่างการ map: int ล้วน, LUT static, modular arithmetic เท่านั้น
- scale/hyperbolic/4D = ฟังก์ชัน map บน address ไม่ใช่ space ที่ต้องสร้าง

## 🔄 Session Handoff — Context Policy (ask-before-hop)

Trigger (any): compact/summarization happened · early messages gone · history noticeably short (context window strained)
Action:
1. Summarize: done / pending / next + ponytail mode + cwd
2. ASK user before `opencode new` — show summary, wait confirm
3. On confirm: `I:\tools\obsidian-memory\obsidian_mem.cmd endsession "summary" --proj DWGLS-native-fs` → `opencode run "Resume: <summary> | ponytail/full | I:\DWGLS-native-fs"`

> **Cross-session/cross-platform handoff:** `python tools/handoff.py --summary "..." --proj DWGLS-native-fs`
> pushes the summary to BOTH the obsidian vault (local, `[[Memory/Sessions/...]]`) AND the cloud-memory
> worker (remote — searchable from any platform via `search_memory "<term>"`, needs `CLOUD_MEMORY_API_KEY`
> from `I:/tools/cloud-workspace/.cloud_memory_key`). Use it for the summary in step 3 instead of the raw
> `endsession` call when the next session may run on another machine/client.

> **Full-work trail (handoff summary ไม่พอต่อยอด):** `tools/session_trail.py` เก็บงานทั้ง session
> (commits / vault note / บันทึก) ไว้ใน temp pool `I:/tools/cloud-workspace/trail/` ให้ `query` ได้ทันที
> โดยไม่ต้อง embed แล้ว `promote` เอาเข้า cloud-memory (source `trail/...`) + vault `Trail/` เมื่อถึงเวลา:
> `collect-git --proj X` · `collect-vault --proj X` · `query --proj X` · `promote --proj X`
> (Vectorize eventual consistency ~2-4 นาที ก่อน search เจอของที่เพิ่ง promote)
>
> **Automation (อัตโนมัติแล้ว):** `obsidian_mem endsession` จะ collect-vault + collect-git เข้า trail
> ให้อัตโนมัติ (hook ทำงานเมื่อมี `tools/session_trail.py` ใต้ cwd เท่านั้น) — ไม่ต้องรัน collect เอง
> ส่วน `promote` ถูก nudge ด้วย scheduled task รายชั่วโมง `session-trail-<proj>` (Windows popup
> เมื่อมี trail ที่ยังไม่ promote อายุเกิน N ชม., default 6):
> `tools/session_trail.py schedule --after-hours 6 --proj X` (ติดตั้ง) · `schedule --uninstall --proj X`
> · `nudge --proj X` (ดูสถานะ/นับถอยหลังด้วยตัวเอง) · `auto --proj X` (รัน catch-up collect+nudge เอง)
> Trail state อยู่ที่ `I:/tools/cloud-workspace/trail/<proj>/state.json` (last_collect/last_promote)

Guard: no auto-hop silent · no re-hop within 10 min · never hop on same summary twice (anti-loop)

## 📋 Session Start

1. Check `core/geo_param_grid.h` — understand current GeoType
2. Check `core/kis_codec_v4.h` — baseline codec state
3. Run `make test` (if Makefile exists) or compile tests manually

## 🧵 Latest State (2026-09-04) — READ THIS FIRST

Session summary: vault `[[Memory/Sessions/2026-09-04_dwgls-native-fs]]` (run `I:\tools\obsidian-memory\obsidian_mem.cmd newsession` or query it).

**Platonic Field (this session):**
- Phase 1 DONE: `core/geo_octant.h` — octant identity + zero-sum binding on 20736 field. 252 lines, header-only. Zero-sum i+j+k ∈ {0,1} selects 4 of 8 octants as tetra-active. Antipodal: valid↔invalid pairs (0↔7,1↔6,2↔5,4↔3). 7/7 tests PASS.
- Phase 2 DONE: Limacon addressing experiments — aa=9 best overall (3.88 score), aa=6 fastest (1.7μs), aa=5 best spread (300 cells). Factorization verified: 96×36×6=20736. 552 addressing paths = 24 hubs × 23 steps.
- Phase 3 DONE: `core/geo_tesseract_dense.h` — 1 tesseract, deterministic, bipolar 1/2 compression. 163 lines, header-only. 8 cubes × 144 slots = 1152. Valid cubes: 4 (zero-sum ≤ 1). 5/5 tests PASS.
- Phase 4 DONE: `core/geo_voronoi_mask.h` — Voronoi pointer masking. 195 lines, header-only. 24 cells × 864 slots = 20736. MaskedPointer = (cell_id, local_offset). Pointer restricted to cell boundary. 7/7 tests PASS.
- Integration DONE: `core/geo_tess_container.h` now includes geo_octant.h + geo_voronoi_mask.h. Added `tess_stride_scatter_octant()` (scatter + zero-sum validation) and `tess_stride_scatter_voronoi()` (scatter + cell-restricted). TESS_Formula gained `voronoi_cell` (0..23) + `voronoi_flags` fields, struct still 64 bytes. Existing `tess_stride_scatter()` unchanged — backward compatible.
- Integration test: `tests/test_platonic_integration.c` 6/6 PASS — proves octant roundtrip (50% store, lossless), voronoi roundtrip (mask→unmask), cell distribution (all 24 cells), bipolar compression (50% storage), masked seeking (zero violations).
- Design crystallization: geometry = rules that connect (not construction). Flexible framework > premature optimization. Both square (12×12) and triangle grids used simultaneously. Entire field can fold into 1728 icosahedra (20736/12 = FS_PIPES).

**Proven this session (all oracle-pass):**
- `tests/test_6ico_integration.c` — 25/25: cross-subsystem integration (geo_codec encode/decode lossless on 20736, cross-GeoType payload identity 4 types, MoE expert address roundtrip 6912, DtSlotRegion store/load 64 experts, stride-37 coprime coverage, 18tes field roundtrip, capacity overflow wrapping, cross-subsystem geo_codec + MoE).
- MoE bake: 108/108 tensors lossless from real Qwen3-4B-MoE (Q4_K stacked FFN experts) → DtSlotRegion via geometric addressing (2.8GB weight pool).
- MoE graft: DtSlotRegion pool → valid 3694 MB GGUF → inference BITWISE identical (maxdiff=0.000000, 151936 dims).
- MoE streaming: top-4/64 experts per layer (93.8% bandwidth savings), Q4_K dequant + SwiGLU FFN matmul, 36/36 layers PASS.
- `geo_dram_tile.h` include guard fix — `#ifndef GEO_DRAM_TILE_H` eliminates redefinition under dual `-I` paths.
- **Streaming capo load** (this session): `TESS_CapoReader` lazy per-capo decode API in `core/geo_tess_container.h:433` + `tools/tess_load_stream.c` CLI (info/load/range/scan). Synthetic 300×144B tensor → .tess → streaming decode: 300/300 match, CRC-64 verified. `tests/test_tess_stream.c` ALL PASS.
- **.tess ↔ MoE bridge** (this session): `tools/tess_moe_bridge.c` — GGUF → multi-capo .tess on disk → stream-serve individual expert blocks → verify lossless against original GGUF. Fixed critical bug: MoE tensors (64 experts × thousands of cells) exceed single capo capacity (20736), solved with multi-capo (ceil(total_cells/20736) capo files, `stream_load_range` spans capo boundaries). Real Qwen3-4B-MoE: **6912 experts (36 layers × 3 wtypes × 64 experts) ALL PASS bitwise identical**.

**Known issue:** `geo_dram_tile.h` infra copy has canonical include guard now (fixed). `dt_slot_init_twin` truncates file when creating new region — stream tool uses direct read-only mmap instead.

**MoE Expert Addressing (proven):** `core/moe_expert_addr.h` — pure integer O(1) mapping: expert_id ↔ geometry coordinate (tess, cube, slot) via 18tes flat address space. Real model tested: Qwen3-4B-MoE (shared attn + stacked FFN experts, not per-expert indexed). 108 experts in 36 layers × 64 experts × 3 wtypes = 20736 slots.

**MoE Pipeline Tools:**
- `tools/moe_expert_bake.c` (`make moe-bake`) — GGUF → DtSlotRegion (lossless weight pool)
- `tools/moe_expert_graft.c` (`make moe-graft`) — DtSlotRegion → valid GGUF → inference
- `tools/moe_expert_stream.c` (`make moe-stream`) — streaming top-K experts + Q4_K FFN matmul

**Pipeline Tools:**
- `tools/tess_bake.c` (`make tess-bake`) — GGUF → .tess files (scatter-encode, capo multi-chunk for >20736 blocks)
- `tools/tess_load.c` (`make tess-load`) — .tess → raw weights (multi-cube capo decode, CRC-64 verify)
- `tools/tess_load_stream.c` (`make tess-stream`) — streaming per-capo reader (info/load/range/scan)
- `tools/tess_assemble.c` (`make tess-assemble`) — .tess + original GGUF metadata → assembled GGUF
- `tools/tess_moe_bridge.c` (`make tess-moe-bridge`) — GGUF → multi-capo → stream-serve experts → verify lossless
- `tools/tess_packer.c` (`make tess-packer`) — pack dir of .tess → single .tesspack / unpack / info
- `tools/tess_gguf_pack.c` (`make tess-gguf-pack`) — GGUF → .tesspack directly (no intermediate files)
- `tools/moe_expert_route.c` (`make moe-route`) — combined bake+route+graft for MoE routing integration
- `tools/tesspack_graft.c` (`make tess-graft`) — .tesspack → valid GGUF (MoE weight graft)
- `tools/tesspack_llama_view.c` (`make tess-view`) — .tesspack → assemble full GGUF + llama.cpp inference verification
- `tools/tesspack_breathe_view.c` (`make tess-breathe`) — mmap RSS measurement: breathing_fs proof on real MoE model

**KIS + breathing_fs:**
- KIS v4/v5/v6/v6b × 6ico full field: all active codecs pass (v5 angular collision documented)
- v6b streaming codec plugged as default into breathing_fs, replacing DynContainer (DynContainer fully removed)
- Fan24 gear (ring-24 CRT bijection) + magnifier seeker (glass center, antipodal inversion) adapters integrated
- Header dedup: TESS_CELLS→TESS_ADDR_CELLS, TESS_MAGIC→TESS_CONTAINER_MAGIC, K-quant types added
- Full test suite: 116/118 PASS (2 pre-existing flaky bfs tests)

**.tess pipeline:** Capo multi-chunk proven lossless — 291/291 tensors, 1181 .tess files (722 MB), token_embd (206 capos) + output.weight (206 capos) all bitwise identical. Scatter throughput 0.7-3 GB/s, bake ~49 MB/s (I/O bound). Streaming capo load now available (no big buffer needed).

**.tesspack single-file container** (this session): `.tesspack` = all capos in one file with index-at-end (zip-style). Format: header[16] (magic/version/n_capos/index_offset) + sequential capo data + index entries (name_len+name+capo_id+offset+size). `tess_capo_open_pack()` in `core/geo_tess_container.h:600` provides random access. `tools/tess_packer.c` CLI: `pack <dir> <out> | info <pack> | unpack <pack> <dir>`. **Real Qwen3-4B-MoE: 43,596 capos (108 tensors × multi-capo, incl. F16 down at 1201 capos/layer) ALL PASS lossless** — 2.87GB single file replaces 13k individual .tess files. Uses `_fseeki64`/`_ftelli64` for >2GB files on Windows.

**MoE Streaming from .tesspack** (this session, Path B): `tools/moe_expert_stream_pack.c` (`make moe-stream-pack`) — GGUF router → top-K selection → stream only selected experts from single `.tesspack` file → dequant → SwiGLU FFN matmul → verify lossless. `TESS_PackIndex` (mmap once + scan index once) replaces per-capo fopen. **Real Qwen3-4B-MoE layer 0: 4/4 experts PASS (maxdiff=0, cos=1.0), 12/12 byte-identical, 93.8% bandwidth savings (4.9 MB streamed vs 77.8 MB full tensor).**

**Tesspack graft** (this session): `tools/tesspack_graft.c` (`make tess-graft`) — .tesspack → valid GGUF. `tesspack_load_tensor()` bulk-loads all capos for a tensor name from pack, `tesspack_graft_to_gguf()` writes assembled output. **Real Qwen3-4B-MoE: 108/108 MoE tensors loaded from pack (2801.7 MB), 326 from source, 0 skipped, output `F:/model/moe_tesspack_graft.gguf` (3694.1 MB).** Fixed critical bugs: cell count for quantized types (`size/GGUF_CELL_SIZE[dtype]` not `dims_product`), `tess_capo_load_range` return convention (0=error, positive=bytes), Windows >2GB ftell (`_ftelli64`), index entry format (content bytes not sizeof).

**Tesspack view → GGUF assembly** (this session): `tools/tesspack_llama_view.c` (`make tess-view`) — reads original GGUF header + metadata, opens .tesspack, assembles full GGUF on disk, verifies via llama.cpp. **Real Qwen3-4B-MoE: 12/12 PASS (T1-T12), logits BITWISE identical (diffs=0, maxdiff=0), all 151936 vocab dims.** from_pack=108/108 MoE tensors (2801.7 MB), from_source=326 non-MoE (886.4 MB). Per-capo direct write to body (no temp alloc) fixed 17 large ffn_down_exps tensors (1201 capos each). Assemble: 62.7s, write: 86s, total: 149s for 3.7 GB GGUF. DLL runtime fix: MSYS2 mingw64 libs replace ancient system MinGW 8.1 (2018). `tools/gcube_token_run.c` has llama.cpp include path reference.

**Tesspack I/O benchmark** (this session): `tests/bench_tesspack.c` (`make bench-tesspack`). Real Qwen3-4B-MoE (217 multi-capo tensors, 44319 capos, 3.9GB .tesspack): GGUF sequential read 109.3 MB/s, pack sequential read 103.4 MB/s, `tess_pack_open` mmap init 0.057s. **MoE stream (4/64 experts): 1.636s, 869 MB/s, 1421 MB read → 89.4% bandwidth savings, 9.4x speedup vs full expert read.** Full expert read: 15.4s, 86.9 MB/s. Bug found & fixed: `tess_capo_load_range` writes `n_elems × cell_size` bytes (Q4_K cell_size=210 → 4.3MB/capo), stack buffer overflow from undersized `uint8_t buf[20736]`. Fixed with dynamic allocation based on actual cell_size.

**Breathing FS proof** (this session): `tools/tesspack_breathe_view.c` — mmap-based RSS measurement on real Qwen3-4B-MoE (3.5GB GGUF). **mmap IS the breathing model**: 3.5GB file → 48MB RSS at load → 1331MB peak (36%) during MoE inference → 64% of file never paged into physical RAM. tesspack-assembled GGUF: 43MB load → 1326MB peak (identical breathing). Assembly step 103s (I/O bound). VirtualAlloc MEM_RESERVE 4.9GB proof: zero physical RAM, incremental MEM_COMMIT budget 1.3GB < 2GB limit on 8GB machine. **no_alloc=true does NOT work in b9733 DLL** — `llama_model_init_from_user` forces full allocation regardless (struct layout mismatch suspected, DLL compiled from different header version). Assembled GGUF inference slower (1.83 vs 2.45 tok/s) due to pack order ≠ layer order (tensor layout mismatch on disk).

**KV finding:** llama state files NOT prefix-nested (98.8% bytes shift) → delta net loss; link b9733 llama.dll directly (llama-server slot-save broken).

**Follow-up session baseline fix** (2026-09-03+): `core/geo_tess_container.h` gained .tesspack mmap reader but never included OS headers — `HANDLE`/`CreateFileA` on Windows, `mmap`/`open` elsewhere. Tests compiled only when another header happened to pull in windows.h first. Added canonical platform include block (stdio/stdlib + windows.h under `_WIN32`, mmap headers under `#else`, matching `geo_zerocopy.h`/`geo_mdim.h`). **Unblocked 6 BUILD FAILs:** test_shell, test_tess_header, test_fibo_dual_rail, test_tess_stream, test_tess_moe_bridge, test_tesspack. New baseline: **TIER1 118/121 PASS · TIER2 4/4 PASS** (was 112/121). 3 remaining RUN FAILs are WIP subsystems (bfs_persist rdh bijection, bfs_stability partial-block, hybrid_kv dt_put).

**Branches STOCKED (ห้ามเปิดก่อน mainline เสร็จ):** docs/ARCHIMEDEAN-STOCK-2026-08-22.md — ภาษาที่ 4 Hosoya/circle view · snub chiral switch · Zeckendorf · circle-config catalog.

**MAINLINE DONE (2026-08-22):** `tools/geo_rid_graft.c` (`make rid-graft`) — RID slots → DtSlotRegion (twin mmap) → llama.cpp: A bake lossless (3 languages) · B unfold byte-identical (3 languages) · C real b9733 inference tokens identical + logits@0 BITWISE (151936 dims) · D damage drill localize+restore. llama reads weight storage addressed by RID language views.
**GEOfs DONE (2026-08-23):** `tools/geofs_rid.c` (`make geofs-rid`) — GeosVolume ⇄ RID slot region: G1 summon real files readback identical · G2 persist blob→parts→DtSlotRegion twin (3 languages) · G3 reload fresh volume all files byte-identical vs ORIGINAL sources · G4 damage flip→localize→re-bake. Twin mmap persists across destroy (7.9MB file). GeoFS = persistent slot region as filesystem layer.
**KV/state DONE (2026-08-23):** `tools/kv_rid_serve.c` (`make kv-rid`) — llama STATE ⇄ RID slot region: checkpoint mid-generation (@token 100, 1.29MB = 10 parts) · readback byte-identical through pent/tri/snub views · restore in FRESH context → logits@restore BITWISE (maxdiff 0, 151936 dims) + 24 post tokens identical · damage drill localize+re-bake. Lesson: logits capture index is position-sensitive — off-by-one shows as maxdiff ~10, not noise.

## 🔧 Build (manual)

```bash
# Tests
gcc -O2 -Wall -o tests/kis_codec_v4_test tests/kis_codec_v4_test.c -lm
./tests/kis_codec_v4_test
```
