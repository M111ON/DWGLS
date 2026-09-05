# AGENTS.md — DWGLS (4Dimension Geometry + KIS Timeline)

## Core Architecture

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

**Infinite alternation: Ico ↔ Dodec through spiking**
- Spike = operation that transforms duals (face ↔ vertex)
- h-depth: spike = h→0 (infinite resolution), sealed = h→R (finite)

### Core Principle

> **MAP not COMPRESS** — Geometry IS the address space.
> Coordinate = data. No hash, no collision, no lookup table.

## Working Rules

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

### Test Integrity
- **expected ต้องมาจาก oracle อิสระเท่านั้น**: spec / คณิตศาสตร์ / ข้อมูลต้นทาง / reference implementation
  — ห้ามมาจากฟังก์ชันที่กำลังเทส (f(x) == f(x) = tautology)
- **ห้าม "run แล้วแปะ output เป็น expected"** — characterization test แบบนี้ freeze bug ให้เข้ากล่อง
- **ห้าม copy comment/spec จาก implementation ไปใส่ใน test** — spec ต้องมาก่อน code
- **ทุกเทสต้อง fail ได้จริง**: mutation check — เปลี่ยน logic 1 บรรทัดใน core → เทสต้องแดง

### Experiments = History
- **ทุก experiment/debug scratch เก็บไว้เป็นประวัติ** — ย้ายไป `deprecated/` (ไม่ delete)
- ลบได้เฉพาะ artifact ใหญ่ที่ regenerable จริง (เช่น .tesspack/.gguf) และต้องถามก่อน

### Design Principles (Timeline-First)
- เลือกใช้ **timeline-first**: int, base-2 scale, ไม่มี 0, ทุกสถานะ deterministic + replay ได้
- hyperbolic/residual = เก็บส่วนต่างที่ explicit; geometry = template เท่านั้น (ไม่ใช่ตัวคำนวณ)

## Rescope — Scale Timeline + 1 Tesseract (2026-08-14)

> เราไม่ได้สร้าง geometry — ใช้โครงสร้าง combinatorial เป็น template ในการ map ข้อมูลเท่านั้น

### Scale = Constant Magnification Rate
- scale = อัตราการขยายคงที่ (multiplicative): `s(t) = s₀·kᵗ` — **ไม่มี 0, ไม่มีที่สิ้นสุด**
- หน้าต่างที่เลือกใช้ = `(0, 20736)`; 20736 = 144² = 1728×12 = 18 tes × 8 cube × 144
- ทุกอย่างขยับพร้อมกันหมด (global scale เดียว) → **append ไม่ต้อง tag scale**

### Hyperbolic Side = Passive Scale-Change Log
- ฝั่ง hyperbolic เก็บ **log ของ scale-change events** (แต่ละ entry = route/path สั้นๆ)
- delta ∝ จำนวน scale-change events ไม่ใช่ขนาดข้อมูล
- อ่านที่ scale ตรง → lossless ตรงๆ (log ว่าง)
- อ่านที่ scale ไม่ตรง → replay log (deterministic) → lossless

### 1 Tesseract = Frame-as-Index
- **1 tesseract = 8 cube × 144 slots = 1152**
- **cube 0 = index frame** (144 slots = 8 blocks × 18): base/len/stride(route)/checksum
- cube 1..7 = data (1008 slots), scatter ด้วย route (stride coprime กับ 144)
- พิสูจน์แล้ว: `tests/test_tess_index_frame.c` (7/7), `test_tess_scale_log.c` (10/10),
  `test_tess_frame_seek.c` (8/8), `test_tess_magnify.c` (12/12), `test_tess_hex_delta.c` (10/10)

### 18tes (6ico compound) = DONE
- 18 tesseracts × 8 cube × 144 = 20736 — full field tested

### Working Rule: No Geometry Construction
- ห้าม compute vertex/face/projection/coordinate จริง — ใช้แค่โครงสร้างเป็นโครงร่างการ map
- int ล้วน, LUT static, modular arithmetic เท่านั้น

## Session Handoff

Trigger (any): compact/summarization happened · early messages gone · history noticeably short
Action:
1. Summarize: done / pending / next + ponytail mode + cwd
2. ASK user before `opencode new` — show summary, wait confirm
3. On confirm: `obsidian_mem.cmd endsession "summary" --proj DWGLS-native-fs`

> **Cross-platform handoff:** `python tools/handoff.py --summary "..." --proj DWGLS-native-fs`
> pushes to obsidian vault + cloud-memory worker (searchable from any platform)
>
> **Full-work trail:** `tools/session_trail.py` → `I:/tools/cloud-workspace/trail/`
> Automation: `obsidian_mem endsession` auto-collect-vault + collect-git

## Latest State

### Done (Proven Lossless)
- **Platonic Field Phase 1-4**: octant, limacon, tesseract_dense, voronoi_mask — all tests PASS
- **Integration**: `test_platonic_integration.c` 6/6 PASS
- **MoE pipeline**: bake 108/108 tensors lossless, graft BITWISE identical, streaming 93.8% bandwidth savings
- **.tess pipeline**: 291/291 tensors, 1181 .tess files (722 MB), all bitwise identical
- **.tesspack**: 2.87GB single file, 43,596 capos ALL PASS lossless
- **Tesspack graft/view/stream/breathe**: all proven on real Qwen3-4B-MoE
- **KV/state/GeoFS/RID**: all DONE
- **Breathing FS**: mmap RSS proof — 3.5GB file, 48MB load RSS, 1331MB peak
- **Baseline**: TIER1 118/121 PASS, TIER2 4/4 PASS

### Pending (Phase 5 — MoE Next)
- **Graft OOM fix**: Use `VirtualAlloc(MEM_RESERVE, 3.7GB)` + `MEM_COMMIT` only active regions. Streaming write via mmap.
- **DLL no_alloc bug**: `llama_model_init_from_user` force-allocates full buffer. Struct layout mismatch suspected.
- **Pack order ≠ layer order**: Assembled GGUF slower (1.83 vs 2.45 tok/s).
- **Next**: Fix graft OOM with VirtualAlloc RESERVE + streaming mmap write, then eliminate assemble step.

### Branches STOCKED (ห้ามเปิดก่อน mainline เสร็จ)
- docs/ARCHIMEDEAN-STOCK-2026-08-22.md — Hosoya/circle view · snub chiral · Zeckendorf · circle-config catalog

### Tools Reference
| Tool | Command | Purpose |
|------|---------|---------|
| tess-bake | `make tess-bake` | GGUF → .tess files |
| tess-load | `make tess-load` | .tess → raw weights |
| tess-stream | `make tess-stream` | streaming per-capo reader |
| tess-assemble | `make tess-assemble` | .tess + metadata → GGUF |
| tess-packer | `make tess-packer` | dir → .tesspack / unpack / info |
| tess-gguf-pack | `make tess-gguf-pack` | GGUF → .tesspack directly |
| moe-bake | `make moe-bake` | GGUF → DtSlotRegion |
| moe-graft | `make moe-graft` | DtSlotRegion → GGUF |
| moe-stream | `make moe-stream` | streaming top-K experts |
| moe-route | `make moe-route` | combined bake+route+graft |
| tess-graft | `make tess-graft` | .tesspack → GGUF |
| tess-view | `make tess-view` | .tesspack → assemble + inference verify |
| tess-breathe | `make tess-breathe` | mmap RSS measurement |

## Build

```bash
# Tests
gcc -O2 -Wall -o tests/kis_codec_v4_test tests/kis_codec_v4_test.c -lm
./tests/kis_codec_v4_test
```
