# ระบบนี้ทำงานยังไง และพิสูจน์ไปถึงไหนแล้ว

> วันที่ 2026-08-26 · branch `feat/geo-native-fs`
> TIER1 116/116 + TIER2 4/4 · soak 3/3 รอบ determinism ไม่มี flaky
> เอกสารนี้เขียนด้วยหลักเดียว: **ทุก claim ต้องชี้กลับไปที่หลักฐาน และทุกสิ่งที่ยังไม่พิสูจน์ต้องบอกว่ายังไม่พิสูจน์**

---

## 1. ระบบนี้คืออะไร

ระบบจัดเก็บข้อมูลแบบ **coordinate-addressed** — ข้อมูลถูกวางที่ตำแหน่งที่คำนวณจากโครงสร้างเรขาคณิต ไม่ใช่จากเนื้อหา ระบบรู้แค่ "ข้อมูลอยู่ตรงไหน" ไม่รู้ "ข้อมูลคืออะไร" — และนั่นคือจุดแข็ง因为它意味着 lossless โดยการออกแบบ ไม่ใช่โดยบังเอิญ

**เปรียบเทียบง่ายๆ:**
- ระบบ filesystem ทั่วไป = มี directory, มี hash, มี lookup table → ต้องจำว่าไฟล์ไหนอยู่ไหน
- ระบบนี้ = พิกัด = ที่อยู่ → ไม่ต้องจำ ไม่ต้อง lookup ไม่ต้อง hash

**ตัวเลขหลัก:**
- 1 slot = 128 bytes (固定 size, ทุก slot เท่ากัน)
- 144 slots = 1 cell ( tileSize)
- 1,152 slots = 1 tesseract (8 cubes × 144)
- 20,736 slots = 1 window (= 144² = 12⁴) — หน้าต่างที่ระบบทำงานด้วย

---

## 1.5 แผนผังสถาปัตยกรรม (ครบทุกชั้น)

> **Visual version:** `docs/dwgls-arch.svg` (vector) · `docs/dwgls-arch.png` (3840×2298)
> **ASCII version below** — ใช้ control-click เปิด SVG/PNG ใน IDE ได้

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        DWGLS Architecture                              │
│                     coordinate = address                               │
└─────────────────────────────────────────────────────────────────────────┘

┌─ LAYER 7: PERSISTENCE ────────────────────────────────────────────────┐
│  .ggf / sparse file / twin mmap                                        │
│  ┌──────────┐  ┌──────────────┐  ┌───────────────┐                    │
│  │ GGUF src │  │ twin 7.9MB   │  │ sparse 75%    │                    │
│  │ 675.7 MB │→→│ (cross-      │→→│ holes logical │                    │
│  │          │  │  destroy)    │  │ 2.72GB/0.68GB │                    │
│  └──────────┘  └──────────────┘  └───────────────┘                    │
│  Files: geo_goldberg_file.h · geo_ggf_ckpt.h · rs_persist.h           │
└────────────────────────────────────────────────────────────────────────┘
          ↕ write once / mmap on demand
┌─ LAYER 6: SERVING ────────────────────────────────────────────────────┐
│  Bake: GGUF → slot region · Pull: slot region → byte-identical        │
│  ┌─────────────┐  ┌───────────┐  ┌───────────┐  ┌──────────────┐     │
│  │ geo_cube_   │  │ geofs_    │  │ kv_rid_   │  │ gguf_        │     │
│  │ serve       │  │ rid       │  │ serve     │  │ roundtrip    │     │
│  │ (weights)   │  │ (files)   │  │ (state)   │  │ (full file)  │     │
│  └─────────────┘  └───────────┘  └───────────┘  └──────────────┘     │
│  Proven: 5 models · 8 languages · logits BITWISE                     │
└────────────────────────────────────────────────────────────────────────┘
          ↕
┌─ LAYER 5: GHOST + GEAR WIRE (scale-change log) ──────────────────────┐
│  ┌──────────────────────────┐  ┌──────────────────────────────────┐   │
│  │ GhostLog (passive)       │  │ Gear Wire (event byte)           │   │
│  │ {block_id, from, to}     │  │ {q:3b|dc:3b|dx:2b} = 1B/event   │   │
│  │ = 5 B/event              │  │ Δ = q×24 + crt(dc,dx)            │   │
│  │                          │  │ seal = 0xFF (unreachable)        │   │
│  │ WIRE-ONLY replay proven  │  │ canonical order: block asc       │   │
│  └──────────────────────────┘  └──────────────────────────────────┘   │
│  Files: geo_ghost_gear_adapter.h · geo_ghost_lift.h · fan24_gear.h    │
│  Adapter: WRITE → lift() + wire append · READ → Δ-chain from wire    │
└────────────────────────────────────────────────────────────────────────┘
          ↕
┌─ LAYER 4: REGISTRY (hyper / identity) ───────────────────────────────┐
│  {id → home address} — 2 B/item, ∝ COUNT not SIZE                    │
│  ┌───────────────┐  ┌──────────────┐  ┌──────────────────────┐       │
│  │ tied_dedup    │  │ residual_    │  │ geo_cap_account      │       │
│  │ {tensor_id →  │  │ space        │  │ budget gate          │       │
│  │  home}        │  │ bond_addressed│  │ leverage ≥ 1 → allow │       │
│  └───────────────┘  └──────────────┘  └──────────────────────┘       │
│  MOVE = reroute link · data never moves (pillar principle)            │
└────────────────────────────────────────────────────────────────────────┘
          ↕
┌─ LAYER 3: PATH (scale log / navigation) ─────────────────────────────┐
│  entry = {from, to} = 2 B/event · delta ∝ SCALE-CHANGE EVENTS        │
│  ┌──────────────┐  ┌────────────────┐  ┌─────────────────────┐       │
│  │ frame_seek   │  │ telescoped     │  │ composed path       │       │
│  │ frame×step   │  │ {w0→w} 1 entry │  │ O(1) per target     │       │
│  │ stride-37    │  │ covers 143     │  │                     │       │
│  │ 1440-cycle   │  │ steps          │  │                     │       │
│  └──────────────┘  └────────────────┘  └─────────────────────┘       │
│  Files: geo_frame_seek.h · geo_scale_wire.h · fibo_walk.h            │
└────────────────────────────────────────────────────────────────────────┘
          ↕
┌─ LAYER 2: VIEWS (6× S₃ permutations + iso bridge) ──────────────────┐
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐       │
│  │ kis_cube_    │  │ iso_rot90    │  │ RID language views   │       │
│  │ views (6)    │  │ tri↔square   │  │ pent/tri/snubL/R/    │       │
│  │ S₃ perms on  │  │ rot90/rot270 │  │ hosoya/zeck/pascal/  │       │
│  │ 12³=1728     │  │ on 144       │  │ hexagram (8 total)   │       │
│  └──────────────┘  └──────────────┘  └──────────────────────┘       │
│  ALL bijections · exhaustive proof · mutual inverses                  │
└────────────────────────────────────────────────────────────────────────┘
          ↕
┌─ LAYER 1: ADDRESSING (modular arithmetic) ───────────────────────────┐
│  physical = (a_w × logical + b_w) % 144                              │
│  a_w ∈ {coprime(144)} = 48 values · b_w = (13×w) % 144              │
│  144 scales = 144 bijective views of SAME data · NO holes            │
│  Files: geo_param_grid.h · tri_hex_tess.h                            │
└────────────────────────────────────────────────────────────────────────┘
          ↕
┌─ LAYER 0: CONTAINER (uniform slots) ─────────────────────────────────┐
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐     ┌──────────────────────┐   │
│  │slot 0│ │slot 1│ │slot 2│ │slot 3│ ... │ slot 20735           │   │
│  │128 B │ │128 B │ │128 B │ │128 B │     │ 128 B                │   │
│  └──────┘ └──────┘ └──────┘ └──────┘     └──────────────────────┘   │
│  20,736 slots = 1 window (= 144²) · data = cargo · NEVER MOVED      │
│  1 tesseract = 8 cubes × 144 slots = 1,152 slots                    │
│  cube 0 = index frame · cubes 1-7 = data                             │
└────────────────────────────────────────────────────────────────────────┘

══════════════════════════════════════════════════════════════════════════
  DATA FLOW: source → bake → slot region → views/ghost/wire → serve
  LOSSLESS GUARANTEE: memcmp every byte at every layer · XOR verify
  IMMOVABILITY: pillar principle — place once, reroute links only
══════════════════════════════════════════════════════════════════════════
```

---

## 2. โครงสร้างหลัก (ไม่ใช่ theory — มี code จริง + test จริง)

### 2.1 Container: ข้อมูลทุกอย่างเท่ากัน

ข้อมูลทุกชิ้นถูกวางใน slot ขนาดคงที่ 128 bytes ระบบรับประกันว่า:

- ทุก slot มีขนาดเท่ากัน — ไม่มี variable-length
- slot ที่ "ว่าง" ก็มีขนาดเท่าเดิม — ไม่ compact
- ข้อมูลถูกวางแล้ว **ไม่เคยถูกย้าย** — ถ้าอยากเปลี่ยนที่ ให้สร้าง slot ใหม่แล้ว reroute pointer แทน

**สิ่งที่ system ไม่ทำ:** ไม่ย้ายข้อมูล ไม่ compact ไม่ merge ไม่ reorder — เพราะทุกครั้งที่ทำอย่างนั้น = ต้องคำนวณใหม่ทุก path → เสี่ยง error

### 2.2 Addressing: พิกัด = ที่อยู่

ทุก slot ถูกนิยามด้วย modular arithmetic:

```
physical_position = (a_w × logical_position + b_w) % 144
```

- `a_w` = ค่า coprime กับ 144 (มี 48 ค่า) — แต่ละค่าให้ bijection คนละตัว
- `b_w` = offset กันซ้ำ (=$((13 \times w) \% 144$))
- **ผล:** 144 "scale" (มุมมอง) ของข้อมูลชุดเดียวกัน — ทุกมุมมองมีครบทุกช่อง ไม่มี hole

**的关键:** ถ้ารู้ `a_w` และ `b_w` → หา physical position ได้ทันที ไม่ต้อง lookup ไม่ต้อง hash

### 2.3 View Layer: 6 มุมมอง S₃

ระบบมี 6 มุมมอง (view) ที่เป็น permutations ของกันและกัน — `core/kis_cube_views.h`:

- แต่ละ view = bijection บน 1,728 slots (12³)
- ทุก view เป็น inverse ของอีก view — หมุนไปมาได้ทุกทิศ
- ทั้ง 6 views ผ่าน exhaustive verification (ทุกค่า 0..1727 ต้อง mapping ถูก)

**加上 iso_rot90:** triangle ↔ square bridge บน 144 slots — rot90/rot270 = inverse pair

### 2.4 Index Frame (Cube 0): หน้าประตู

ทุก tesseract มี cube 0 เป็น index frame:
- 7 cubes × 18 bytes = 126 bytes สำหรับ metadata ของ data cubes
- checksum = `(sum ของค่าทั้ง cube) % 251` → ถ้าย้ายค่า = checksum ผิด → flag ทันที
- **หน้าที่:** ให้ผู้อ่านรู้ว่า cube data อยู่ไหน โดยไม่ต้อง scan ทั้ง field

---

## 3. กลไกที่ทำงานจริง (มี code + test + ตัวเลข)

### 3.1 Bake + Pull: GGUF จริง lossless

**ข้อเท็จจริง:** Qwen2.5-0.5B-Instruct-Q8_0.gguf (675.7 MB, 5,156 parts, 291 tensors)
- bake → slot region → pull กลับ → memcmp กับต้นฉบับ → **0 bad parts**
- 6-view sweep: XOR == zero-padded source → **ทุก view**
- ผ่าน 5 โมเดล: Qwen2.5-0.5B · Qwen3-0.6B · SmolLM2-360M · Kokoro-TTS · smolVLM-text

**ตัวเลขจริง:**
- SEQ read: 8.4–9.7 GB/s (~90% ของ RAM peak)
- Bake 5,305 parts: 0.55–0.7s
- CPU token generation ต้องการ: 5.45 GB/s → window เลี้ยงไหว headroom 1.5×

### 3.2 Scale ≠ 1 ("หายใจ"): weights จริง

ข้อมูลถูก bake ที่ scale=1 → expand ×2 (เกิด holes) → shuffle S₃ → unshuffle → collapse → กลับบ้าน:
- **memcmp ทุก state: 5,305/5,305 ผ่าน**
- holes ไม่ตกค้าง
- carried state 仅 4 events / 32 bytes เท่านั้น

**ข้อเท็จจริง:** v1 แรกเขียน verify `L/R==fid` ซึ่งเป็น tautology (ไม่ได้ verify อะไรเลย) — ถูกจับได้และเขียนใหม่ ข้อเท็จจริงนี้ถูกบันทึกไว้ใน `docs/HONEST-STATUS-2026-08-22.md` §6

### 3.3 RID Slot Storage: 8 ภาษา lossless

RID (Rectangular Icosidodecahedron) slot region ทำให้ข้อมูลชุดเดียวเข้าถึงได้ 8 ภาษา:

| # | ภาษา | หลักการ | Status |
|---|------|---------|--------|
| 0 | pent | dodecahedron face ordering | ✅ lossless |
| 1 | tri | icosahedron (dual) | ✅ lossless |
| 2 | snubL | snub dodeca left (chiral) | ✅ lossless |
| 3 | snubR | snub dodeca right (enantiomorph) | ✅ lossless |
| 4 | hosoya | golden spiral stride F(7)=13 mod 60 | ✅ lossless |
| 5 | zeck | Zeckendorf reversed code | ✅ lossless |
| 6 | pascal | A(n)=Σ(−1)^k C(n−k,k) period-6 | ✅ lossless |
| 7 | hexagram | hex distance rank | ✅ lossless |

- R1 (BAKE+READBACK): bad=0 ทุกภาษา
- R2 (REBUILD): byte-identical ทุกภาษา
- R3 (DAMAGE DRILL): flip 1 byte → localize → re-bake → lossless อีกครั้ง
- R4 (PERSIST): twin file ข้าม destroy ได้

**Colab T4 Integration:**
- bake 675.7 MB + rebuild byte-identical → inference tokens identical
- ผ่าน llama-cpp-python: 129.8 tok/s (direct) · 225.9 tok/s (via RID)

### 3.4 KV Cache: llama.cpp state lossless

 llama state file จริงจาก llama.cpp b9733 (1.29 MB = 10 parts):
- checkpoint mid-generation (@token 100)
- readback byte-identical ผ่าน pent/tri/snub views
- **restore ใน fresh context → logits@restore BITWISE (maxdiff 0, 151,936 dims)**
- 24 post-restore tokens identical

**ข้อเท็จจริง:** llama state files ไม่ได้ prefix-nested (วัดแล้ว 98.8% bytes shift) → delta compression บน state = net loss ไม่ใช่ net gain

### 3.5 GeoFS: persistent slot region as filesystem

GeosVolume ⇄ RID slot region:
- persist blob → parts → DtSlotRegion twin (3 ภาษา)
- reload fresh volume → byte-identical vs original sources
- damage flip → localize → re-bake
- Twin file 7.9 MB ข้าม destroy ได้

### 3.6 Ghost + Gear Wire

ghost lift = log scale-change events แบบ passive:
- entry = {block_id, from_scale, to_scale} = 5 bytes
- gear wire = {q:3b|dc:3b|dx:2b} = 1 byte per event
- canonical wire order: block id ascending → disk layout deterministic ทุกลำดับ lift

**prove แล้ว:** ghost_gear_replay อ่าน Δ-chain จาก WIRE ONLY (ไม่อ่าน entry fields) → lossless

### 3.7 Breathing: sparse backing

 bake GGUF ลง 1 NTFS sparse file:
- logical 2.72 GB, on-disk 0.68 GB (75% holes)
- VERIFY byte-identical
- warm sweep: 11.4–11.6 GB/s ≥ RAM window mode

---

## 4. สิ่งที่พิสูจน์แล้ว (ทุกข้อชี้กลับไป test)

| # | สิ่งที่พิสูจน์ | หลักฐาน | ระดับ |
|---|----------------|---------|------|
| 1 | rot90 bijection ครบ 144 slots | `test_iso_rot90` (sweep + hand-computed values) | exhaustive |
| 2 | fold/unfold inverse ครบ 20,736 จุด | `test_iso_fold` (unfold(fold(g))==g) | exhaustive |
| 3 | 6 views bijection ครบ 1,728 | `test_kis_cube_views` (mutual inverses + S₃ order) | exhaustive |
| 4 | Bake/Pull GGUF จริง lossless | `geo_cube_serve` (memcmp 5,305/5,305) | real data |
| 5 | 6-view sweep XOR == 0 | `geo_cube_serve` | real data |
| 6 | Scale ≠ 1 on weights จริง lossless | `test_breathing_fs` ( memcmp ทุก state) | real data |
| 7 | Multi-model generality (5 models) | `test_gguf_multi_model` (29/29) | real data |
| 8 | KV cache park/resume lossless | `kv_rid_serve` (logits BITWISE) | real data |
| 9 | RID 8 languages lossless | `gguf_roundtrip` (full 675.7 MB) | real data |
| 10 | Ghost + gear wire replay | `test_ghost_gear_adapter` (Δ-chain from wire only) | synthetic |
| 11 | Sparse backing lossless | `test_rs_persist` (holes 75%) | real data |
| 12 | Negative port (model > window) | `test_rs_persist` | real data |
| 13 | Dorca 2-invert compound | `geo_invert_compound_test` | geometry |
| 14 | Snub dodeca chiral pair | `geo_snub_test` (parity solve, exactly 2) | geometry |
| 15 | Index frame checksum | `test_tess_index_frame` (7/7) | synthetic |
| 16 | Scale dedup 112× path | `test_tess_scale_dedup` (13/13) | synthetic |
| 17 | Full cycle stride-37 | `test_tess_full_cycle` (12/12) | synthetic |
| 18 | Torus wrap 144×144 | `test_tess_torus` (16/16) | synthetic |
| 19 | Tetra axis orbits | `test_tess_tetra_axis` (10/10) | synthetic |
| 20 | Sync bridge bijection | `test_geo_sync_bridge` (7/7) | synthetic |
| 21 | Performance: 8.4-9.7 GB/s sweep | `bench` (vs RAM peak 10.72 GB/s) | benchmark |

**ข้อสังเกต:** ข้อ 1–3 เป็น exhaustive proof (ไม่ได้ sampling) · ข้อ 4–12 ใช้ของจริง · ข้อ 13–14 เป็น geometry proof · ข้อ 15–20 เป็น structural proof

---

## 5. สิ่งที่ยังไม่พิสูจน์ / ยังไม่ทำ

| # | รายการ | สถานะจริง | สาเหตุ |
|---|--------|-----------|--------|
| 1 | delta ∝ events บน inference จริง | ❌ NET LOSS | llama state ไม่ prefix-nested (98.8% bytes shift) |
| 2 | llama.cpp integration (wire จริง) | ⚠️ analyzed แล้ว | ต้อง gcc ≥9 + patch llama.cpp source |
| 3 | GPU feed ตรง (ไม่ผ่าน mmap) | ⚠️ VRAM-resident เท่านั้น | RAM bus 52 GB/s < GPU decode |
| 4 | Multi-model serving ทั่วไป | ⚠️ seed proven 2 models | allocator + jump API ยังไม่ build |
| 5 | 18tes (20736 เต็ม) | ❌ ยังไม่ implement | 18 tesseract × 8 cube × 144 — ซับซ้อนเกิน |
| 6 | ภาษาที่ 4–8 ผ่าน RID | ✅ proven | pascal + hexagram เพิ่มล่าสุด (2026-08-24) |
| 7 | KV checkpoint → rollback model state | ❌ ยังไม่ implement | ต้อง wire เข้า generation loop |
| 8 | Goldberg storage (multi-sphere) | ⚠️ proven singles | streaming multi-sphere ยังไม่ integrate |

---

## 6. วิธีการพิสูจน์ (ทำไมอ่านแล้วเชื่อได้)

### 6.1 Oracle อิสระเท่านั้น
- **memcmp vs source file** — เปรียบเทียบ byte กับ byte กับไฟล์ต้นฉบับ
- **exhaustive sweep** — ทดสอบทุกค่า 0..N ไม่ใช่ sampling
- **XOR digest** — ทุก view ต้อง XOR == 0 กับ source
- **hand-computed values** — ค่าที่คำนวณด้วยมือก่อนเขียน code

### 6.2 Anti-tautology
- v1 breathing test verify `L/R==fid` ซึ่งเป็น identity function — ไม่ได้ verify อะไรเลย → ถูกจับได้และเขียนใหม่
- หลัก: test ต้องมี assertion ที่独立จาก implementation — ถ้า assertion = implementation → tautology

### 6.3 Mutation-sensitive
- ทุก bug ระหว่างทาง (relocation หาย, vacated-zero, LIFO violation, off-by-one) ทำให้ test แดงก่อน commit เสมอ
- 例: logits@restore off-by-one → maxdiff ~10 (ไม่ใช่ noise)

### 6.4 Performance claims มี baseline
- ทุก performance number วัดบน hardware เดียวกัน
- raw mmap memcpy = baseline → แล้วเทียบ
- ห้ามคำว่า "competitive" โดยไม่มีตัวเลข

---

## 7. ข้อจำกัดที่รู้แล้ว (frozen/sacred)

1. **Window 20,736 slots × PART_BYTES** — โมเดล > 2.66GB (@128KB) ต้อง multi-window
2. **Event log unwind LIFO** — collapse หลัง shuffle ไม่ matched = invalid (พิสูจน์จาก failure จริง)
3. **DT_HASH_SLOTS = 512** — dramtile twin store จุ named entries ได้จำกัด
4. **Coordinate = address trade-off:** ได้ O(1) access + view derive ฟรี ↔ **ย้ายไม่ได้เลย** — อยากเปลี่ยน = สร้างเสาเข็มใหม่ + reroute link (ไม่แตะของเดิม)
5. **Value axis กำแพง entropy:** Q8_0 คือ whitening → ข้อมูลขาวแล้ว → บีบได้สูงสุด ~1.077× — ใครอ้าง "บีบ Q8 ได้ 10×" = lossy หรือ magic

---

## 8. ตัวเลขศักดิ์สิทธิ์ (ทำไมตัวเลขนี้)

```
20736 = 144² = 12⁴ = 2⁸·3⁴ = 1728×12
     → คำตอบเดียวในช่วง 16-bit ที่ตอบ constraint ทั้งหมด

144   = 16·9 = 4²·3²  → 16:9 ratio bake ใน window
12    = 4×3            → equal-triangle 4-subdivision × Peano 3-adic
576   = edges + faces ของ 6ico compound
1728  = 12³ = FS_PIPES
```

**ข้อเท็จจริง:** ตัวเลขเหล่านี้ไม่ได้ถูกเลือก — มันคือจุดตัดของ constraint ทางคณิตศาสตร์ (Euler characteristic, coprime requirements, modular arithmetic)

---

## 9. สรุปหนึ่งย่อหน้า

ระบบพิสูจน์แล้วว่า **container uniform + address=f(data) + index=cycle + storage=seed/frame/codec** ทำงาน lossless ได้จริงบนข้อมูลจริงทั้ง ratio=1 และ ratio≠1 — single sparse backing (holes 75%) — negative port สำหรับโมเดลเกิน window — 8 ภาษา RID lossless — KV cache state lossless ทุกการ restore — ghost + gear wire replay จาก Δ-chain เท่านั้น — performance 8.4–9.7 GB/s (~90% ของ RAM peak) — bake + rebuild 27.5s บน 675.7 MB — inference tokens + logits bitwise identical ทั้ง CPU และ Colab T4

**สิ่งที่ยังไม่ได้:** KV delta compression บน interface ที่ไม่ได้เป็นเจ้าของ (ต้อง raw K/V hook) — llama.cpp wire-in production — GPU feed ตรง — multi-model serving ทั่วไป — 18tes upgrade — ทั้งหมดอยู่ใน ACTION-PLAN พร้อม root cause ที่วัดได้ทุกข้อ

---

## 10. ไฟล์สำคัญ

| ไฟล์ | บทบาท |
|---|---|
| `core/geo_param_grid.h` | parameterized geometry — start here |
| `core/iso_rot90.h` | triangle ↔ square bijection on 144 |
| `core/kis_cube_views.h` | 6 S₃ views on 12³ |
| `core/geo_ghost_gear_adapter.h` | ghost lift + gear wire adapter |
| `core/gear_wire_bridge.h` | interop bridge (self-contained, no DWGLS deps) |
| `core/geofs_core.h` | GeoFS filesystem layer |
| `core/fibo_walk.h` | walk clock: state = (seed, round, tick) |
| `core/tied_dedup.h` | registry {tensor_id → home} |
| `tests/` (192 files) | test sources (116 governed by Makefile) |
| `docs/TIMELINE_WORKING_MODEL.md` | ฉบับเต็ม 3,332 บรรทัด |
| `docs/TIMELINE_FIRST_FOUNDATION.md` | หลักการราก (174 บรรทัด) |
| `docs/HONEST-STATUS-2026-08-22.md` | สถานะ诚实 (146 บรรทัด) |

---

*เขียน 2026-08-26 · ทุก claim มี test กำกับ · ไม่มีคำว่า "breakthrough" · ไม่มี "revolutionary" · มีแค่ "พิสูจน์แล้ว" กับ "ยังไม่ได้พิสูจน์"*
