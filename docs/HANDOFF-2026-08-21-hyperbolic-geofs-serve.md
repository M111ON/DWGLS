# HANDOFF — 2026-08-21: Hyperbolic Walk → GeoFS Multi-Volume → Serve + KV Remap / DramTile

สองสายงานต่อเนื่องจาก HANDOFF-2026-08-17: (1) สาย serve จริง — hyperbolic centroid-walk
→ fixed-frame 1tes → GeoFS → multi-volume → bake GGUF ทั้งโมเดล + tensor pull,
(2) สาย KV cache — skeleton+delta remap park/resume ผ่าน Diamond Shell / Rail /
GeoFS bridge / DRamTile twin store. ปิดวันด้วย **TIER1 109/109 + TIER2 4/4 ✅**

---

## 🛰️ สาย A: Serve chain (commits 1983a0d..2566112)

### Hyperbolic redesign = deterministic centroid walk
- `1983a0d` เลิก float/Cayley → **centroid walk + key frame + f(step)** — int ล้วน
- `9afa0ee` stride 27 (3³) เพิ่มเป็น axis 3 — intermediate scatter (9/27/81)
- `c63b592` docs(tetra): orientation S4 ไม่ใช่ A4 (probe-verified)

### Fixed-frame 1tes allocator
- `255feca` pin tesseract 0 (1tes = 1152 blocks, 73KB) — deterministic block allocator
  `tess_flat(0, cell, slot)`; cells 2..7 เลี่ยง header 0..255
- `ce64cae` `core/geo_pipeline_fixed.h`: interior scatter **อยู่ใน cell เท่านั้น**
  `addr = base + (stride·b % 144)` — ไม่มี distortion ข้าม field
  - pipe_cell_base / pipe_alloc_in_cell / pipe_scatter_in_cell / pipe_place_cell

### GeoFS multi-volume (Option A)
- `9387151` `core/geofs_multivol.h`: GeosMV เป็นเจ้าของ volumes แบบ lazy-open;
  ไฟล์หนึ่งไฟล์อยู่ใน volume เดียว (ไม่ stripe); placement ลอง volume เดิมด้วย
  `geos_hyper_find_seed` ก่อน → auto-open ใหม่; name→volume directory = linear scan
  (no hash), resolve ครั้งเดียวเป็น inode แล้ว reuse
- test: 8 MB real-GGUF slice ข้าม 8 volumes ✅

### Perf
- `90b59bc` hyper seq read parity — incremental stride walk ไม่หาร per-block
- `5344c85` O(1) seed search + inode-resolved hyper access APIs

### ★ geos_mv_serve — bake + pull ทั้งโมเดล (`2566112`)
- `tools/geos_mv_serve.c`: weight-pull layer สำหรับ llama.cpp/GPU เรียกใช้ —
  bake ทุก tensor เป็น 128KB hyper parts ข้าม volumes ที่ auto-open,
  pull กลับด้วยชื่อ, verify, seq sweep + random whole-tensor pulls
- **Qwen2.5-0.5B (675.7 MB, 291 tensors) → 5305 parts ใน 569 volumes —
  VERIFY byte-identical 0 mismatches (memcmp vs source GGUF = oracle อิสระ);
  SEQ 2.4 GB/s · RAND avg 1.35 ms / 1.5 GB/s**
- **Baseline raw-mmap memcpy (phase 5, hardware/pattern เดียวกัน)**:
  SEQ 8.65 GB/s → geos **ช้ากว่า 3.5×**; RAND whole-tensor
  0.90 ms → geos **ช้ากว่า 1.5×** — ทั้งสอง path ช้ากว่า raw mmap ทั้งคู่
  → verdict: **ยังไม่ competitive ที่ raw throughput/latency** — จุด trade off
  อยู่ที่ capability ที่ mmap ทำไม่ได้ (dedup / multi-view / deterministic replay);
  seq stream เป็นเป้า optimize ถัดไป
- fix: rename local `hyper` → `is_hyper` (ชนกับ Windows header symbol เมื่อ
  gguf_reader.h include ก่อน)

```
GGUF ──bake──▶ GeosMV (vol, seed, axis) ──pull(name)──▶ byte-identical ✅
                ▲ hyperbolic scatter ใน pinned cell (stride 9/27/81)
```

---

## 🧠 สาย B: KV Remap / DramTile cluster (uncommitted → commit วันนี้)

### kv_remap.h — Adaptive skeleton+delta (3 tier)
| change | strategy |
|---|---|
| 0–15% | XOR delta compressed (small, precise) |
| 15–85% | byte-offset ranges (topology, fast) |
| 85%+ | rebuild skeleton (baseline ใหม่) |

compression auto-select: <64B → RLE; else Diamond Shell

### ระบบรอบตัว
- **diamond_shell_codec.h (v3)** — FLAT=2B / SPARSE=66B / DENSE=66B per 64B chunk;
  discriminator = fold_fibo_intersect popcount (ไม่ใช่ entropy proxy เหมือน v2);
  v2 = 3D shell + real fibo discriminator; pure-content (no metadata override)
- **kv_remap_diamond.h** — แทน RLE ด้วย Diamond Shell: benchmark A พิสูจน์
  **2.32× vs RLE 2.10× @ 40% change**
- **kv_remap_rail.h** — layer-based segment chunking: lane สแกนเป็นกลุ่ม layer
  (K then V, byte-by-byte), patch decompress→copy per layer; states PARK/SCAN
- **kv_geofs_bridge.h** — park/resume agent KV ผ่าน GeosVolume:
  `<agent>.skel` (compressed skeleton) + `<agent>.delta` ([type][pct][payload])
- **kv_dramtile_bridge.h** — production backing: skeleton+delta blobs อยู่ใน
  mmap'd twin file ("disk at RAM speed", zero-copy dt_get) — scale เป็น GB,
  ไฟล์เดียวจุ agent park ได้ทุกตัว (wire format เดียวกับ geofs bridge)

### ฐานรากที่มาด้วย
- **dramtile_store.h/.c** — twin store: VirtualAlloc/mmap reserve ≥4GB, page-lock,
  anonymous init; zero-copy reads
- **geo_dram_tile.h** — zero-copy addressing:
  `dram_addr = anchor_id × 128 + hilbert_8x8(x,y,layer)`;
  anchor ∈ [0,161] = FRAME_ICO_NODES (81×2 poles) → 162×128 = **20736 = GEO_FULL**
- **rdh_addr.h** — RDH (Ring-Wedge-Mirror) bijection:
  key = ((ring×n_wedges+wedge)×n_mirror+mirror)×max_u+u — no hash, O(1)
- **rdh_capture.h** — 1 entry point: `rdh_capture(data,len,cfg) → flat key`;
  enc = flat_key % 1440 → face/slot/phase downstream
- **pogls_fold.h (V3.6)** — Diamond Block 64B (1 cache line, ห้ามขยาย),
  Two-World A/B switch gate ENGINE_ID bit6, HoneycombSlot (Tails state),
  3-layer verify XOR → Fibo Intersect → Merkle; Core Law A = floor(θ×2²⁰) unchanged

### Tests (TIER1 ใหม่ 5 ตัว)
`test_kv_remap` · `test_kv_remap_diamond` · `test_kv_geofs_bridge` ·
`test_kv_rail_geofs` · `test_kv_dramtile` — ผ่านหมด; suite รวม **109/109 + TIER2 4/4**

---

## 🧪 Addendum — REAL multi-turn KV measurement (2026-08-22)

สายใหม่: `tools/kv_dump_turns.c` (link ตรงกับ b9733 `llama.dll`, GTX 1050 Ti CPU path)
→ dump **KV snapshot จริง 4 turn** จาก Qwen2.5-0.5B (29/33/33/41 tokens,
0.34→1.60 MB, ~12 KB/token) แล้วผ่าน `tools/kv_real_multiturn_bench.c`
(kv_remap + DRamTile twin store):

| ผลลัพธ์ | ตัวเลข | verdict |
|---|---|---|
| Lossless park/resume บน KV จริง | memcmp ครบทุก turn **ALL OK** | ✅ capability ผ่าน |
| Skeleton compression บน real KV | **4.09×** (random bench เคย = 1.0×) | ✅ real KV มีโครงสร้าง จับได้จริง |
| Byte-level delta บน serialized state | delta/KV = **100–107%**, resume ช้ากว่า memcpy floor 20–45× | ❌ **NET LOSS** |

**เหตุผล (วัดยืนยัน):** state file ของ `llama_state_seq_get_data` ไม่ใช่
prefix-nested — turn1 vs turn2 ต่างกัน 98.8% ใน region เดิม (first diff @12)
→ append-only property อยู่ที่ **live KV cache** ไม่ใช่ **serialized form**
และ public API ไม่เปิด raw cache buffer

**Verdict ตรงตัวเลข:** การประหยัดต่อ agent ตอนนี้ *พิสูจน์แล้ว* เฉพาะฝั่ง
compression (4.09×); โครงเรื่อง "delta ∝ events" บน serialized interface
**ยังไม่ผ่าน** — ต้อง hook คนละจุด (patch llama.cpp ให้ expose raw K/V
per-layer pointer หรือรัน KV บน stack ของเราเอง) ก่อนถึงจะวัดได้จริง

## 🧊 Addendum 2 — Cube-view serving on real GGUF (2026-08-22)

`tools/geo_cube_serve.c`: bake Qwen2.5-0.5B parts (128KB × 5305) into the
geometry window ผ่าน `iso_fold` (tes/cell/slot → anchor/hilbert/layer) แล้ว
stream กลับมาใน **ทั้ง 6 cube views** (S₃ axis permutations):

| ผลลัพธ์ | ตัวเลข |
|---|---|
| VERIFY lossless | byte-identical 0 bad ✓ |
| 6-view sweep XOR | **ทุก view ตรง source** ✓ |
| Sweep throughput | **8.1–8.5 GB/s ≈ 95% ของ raw memcpy** (เทียบ name-based SEQ 2.4 GB/s = 28%) |

**ความหมาย:** baked copy เดียว เดินได้ 6 ลำดับ (6 face-views) โดยไม่ copy
— addressing เป็น closed-form math ไม่มี directory → overhead แทบหาย

## 🫁 Addendum 3 — Breathing PROVEN on real weights (2026-08-22)

`tools/geo_breathing_test.c`: ปิด gap สำคัญของ Read–Write Identity —
**scale≠1 บน weights จริง พิสูจน์แล้ว** (ก่อนหน้านี้ ratio=1 เท่านั้น):

```
bake Qwen2.5 @scale1 → breathe in {expand×2, shuffle S₃}
                     → breathe out {unshuffle, collapse} → home
ทุก state: memcmp 5305/5305 ✓ · holes clean ✓ · bijection ✓
whole-window XOR MATCH · DIAG 0/20736 slots differ ✅
carried state = 4 events / 32 bytes
```

**บทเรียนที่ encode ลงในเทส (แต่ละอันคือ failure จริงก่อนหน้า):**
1. v1 tautology (`L/R==fid`) — verify ต้องมี transform ที่มีผลจริง
2. breathe in = **relocate bytes จริง** ไม่ใช่แค่ re-label
3. event log unwind **LIFO** — collapse หลัง shuffle ไม่ matched = invalid
4. vacated set = origins ∖ destinations (empty ใน bijection แท้, ไม่ว่างหลัง expand)

## ⏭️ ขั้นต่อไป (เปิดไว้)

0. **Expose raw K/V cache buffers** — patch llama.cpp (source ที่ I:\llama.cpp
   build dir configure MinGW ไว้; gcc 8 เก่าเกิน ต้อง gcc ≥9) ให้เห็น pointer
   ต่อ layer → วัด delta ที่ layer จริงซึ่ง append-only จริง
1. **ฝัง geos_mv_serve เข้า llama.cpp** — callback อ่าน weight จาก pull API
   (แทน mmap gguf ตรงๆ) — pull ช้ากว่า raw mmap 3.5× (SEQ) / 1.5× (RAND)
   → ต้องให้ค่าที่ได้มาจาก capability (dedup/multi-view/replay) หรือ optimize
   walk path ให้แตะ parity ก่อน
2. **KV park/resume end-to-end บน inference จริง** — snapshot ระหว่าง generate →
   resume session ใหม่ผ่าน dramtile twin store (lossless ✅ แล้วบน state file)
3. **Rail SCAN threshold tuning** — layer-group scan vs flat range บน workload จริง
4. **18tes (GEO_COMPOUND_144)** — ยัง FUTURE (เดิม); 1tes fixed frame ใช้งานจริงแล้ว
5. **Tied-embedding dedup wire-in** (จาก 08-17) — registry {id→home} ยังไม่ได้เชื่อม

## ⚠️ ข้อควรระวัง

- `hyper` เป็น symbol ที่ชนกับ Windows headers — local var ต้องตั้ง `is_hyper`
- Diamond Block 64B = sacred (1 cache line); World A lanes 0-3 frozen
- กฎเดิม: ratio < 1.0 ต้องพิสูจน์ด้วย decode; expected ต้องมาจาก oracle อิสระ
