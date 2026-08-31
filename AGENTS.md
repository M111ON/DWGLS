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
    └── test_18tes_field.c · test_moe_expert.c
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

## 📋 Session Start

1. Check `core/geo_param_grid.h` — understand current GeoType
2. Check `core/kis_codec_v4.h` — baseline codec state
3. Run `make test` (if Makefile exists) or compile tests manually

## 🧵 Latest State (2026-08-31) — READ THIS FIRST

Session summary: vault `[[Memory/Sessions/2026-08-31_dwgls]]` (run `I:\tools\obsidian-memory\obsidian_mem.cmd newsession` or query it).

**Proven this session (all oracle-pass):**
- `tests/test_18tes_field.c` — 30/30: full-field roundtrip 20736 slots across 18 tess, mirror_z cross-tess boundary (cube 7→8), stride-37 covers all 20736 slots (coprime with 144 and 20736), global passive log, magnify glass antipodal property.
- `tests/test_moe_expert.c` — 30/30: MoE expert ↔ geometry address roundtrip (4 layers × 64 experts × 3 wtypes = 768 exhaustive), disk offset determinism, neighbor/sibling properties, capacity overflow, boundary conditions.
- `tools/moe_expert_demo.c` — 7/7: DtSlotRegion store/load practical verification — roundtrip 96 experts, cross-access flat↔geometry, batch store/load, random+reverse access, geometry coordinate access, metadata in slot.
- Soak re-run: **118/118 PASS** (117 TIER1 + 1 TIER2 soak), 0 regressions.

**Known issue:** `core/infra/geo_dram_tile.h` lacks include guard (`#pragma once` is per-path, not per-symbol). When both `-Icore` and `-Icore/infra` are in include path, double inclusion of `geo_dram_tile.h` causes redefinition errors. Workaround: inline `moe_expert_addr.h` functions in demo instead of including the header.

**MoE Expert Addressing (new):** `core/moe_expert_addr.h` — pure integer O(1) mapping: expert_id ↔ geometry coordinate (tess, cube, slot) via 18tes flat address space. Address formula: `flat = layer × 64 × 3 + expert × 3 + wtype` (mod 20736). Capacity: up to 6912 experts (20736 ÷ 3 weight types).

**KV finding:** llama state files NOT prefix-nested (98.8% bytes shift) → delta net loss; link b9733 llama.dll directly (llama-server slot-save broken).

**Branches STOCKED (ห้ามเปิดก่อน mainline เสร็จ):** docs/ARCHIMEDEAN-STOCK-2026-08-22.md — ภาษาที่ 4 Hosoya/circle view · snub chiral switch · Zeckendorf · circle-config catalog.

**MAINLINE DONE (2026-08-22):** `tools/geo_rid_graft.c` (`make rid-graft`) — RID slots → DtSlotRegion (twin mmap) → llama.cpp: A bake lossless (3 languages) · B unfold byte-identical (3 languages) · C real b9733 inference tokens identical + logits@0 BITWISE (151936 dims) · D damage drill localize+restore. llama reads weight storage addressed by RID language views.
**GEOfs DONE (2026-08-23):** `tools/geofs_rid.c` (`make geofs-rid`) — GeosVolume ⇄ RID slot region: G1 summon real files readback identical · G2 persist blob→parts→DtSlotRegion twin (3 languages) · G3 reload fresh volume all files byte-identical vs ORIGINAL sources · G4 damage flip→localize→re-bake. Twin mmap persists across destroy (7.9MB file). GeoFS = persistent slot region as filesystem layer.
**KV/state DONE (2026-08-23):** `tools/kv_rid_serve.c` (`make kv-rid`) — llama STATE ⇄ RID slot region: checkpoint mid-generation (@token 100, 1.29MB = 10 parts) · readback byte-identical through pent/tri/snub views · restore in FRESH context → logits@restore BITWISE (maxdiff 0, 151936 dims) + 24 post tokens identical · damage drill localize+re-bake. Lesson: logits capture index is position-sensitive — off-by-one shows as maxdiff ~10, not noise.

**4TH LANGUAGE DONE (2026-08-23):** golden-spiral (phyllotaxis/circle-packing) view — `tools/hosoya_view_probe.c` 11/11 oracle-pass (stride F(7)=13 mod 60: Euclid gcd + bijection + inverse 37≡13⁻¹ + Hosoya cell T(6,0) + φ-convergent 13/8 + mutation red stride-14) · wired as view "hosoya" in `gguf_roundtrip` → full GGUF 5156 parts lossless ×4 languages.
**CHIRAL SWITCH DONE (2026-08-23):** mirror enantiomorph view "snubR" (complement all diagonal bits) — snubL≠snubR 30/30 squares, both lossless on full GGUF.
**ZECKENDORF DONE (2026-08-23):** `tools/zeckendorf_probe.c` 9/9 — existence+non-consecutive (1..4000) · uniqueness brute-force leaf-count oracle · reversed-code bijection on 60 slots · mutation red. View "zeck" wired → `gguf_roundtrip` serves **×6 languages: pent/tri/snubL/snubR/hosoya/zeck**, full GGUF lossless each.
**CIRCLE-CONFIG CATALOG DONE (2026-08-23):** `tools/circle_config_probe.c` 13/13 — contact-degree catalog: deg3=dodeca · deg4=RID(E=120) · deg5=snub(E=150, both enantiomorphs uniform) · deg6=hex packing; `geo_cell_classify` 8-parity bridge non-degenerate. Lesson: uffind return = ROOT not parity — bits must come from the out-param (root-as-bit → all-same diagonals → deg 4/6 alternating).
**CHAIN-13 RESOLVED (2026-08-23):** transcription error — true word "21212212" (len 8) sum=13 ✓ digit-sum ladder ครบทุก chain; palindrome 5/6 (W₆ even-len+odd-sum impossible); W₆ = W₅∥W₄ → Fibonacci recurrence as word-concatenation. zeckendorf_probe 9/9 with source-image values.

**Next:** MoE streaming → expert routing → llama.cpp integration · geometry quantization map · geo_dram_tile.h include guard fix.

## 🔧 Build (manual)

```bash
# Tests
gcc -O2 -Wall -o tests/kis_codec_v4_test tests/kis_codec_v4_test.c -lm
./tests/kis_codec_v4_test
```
