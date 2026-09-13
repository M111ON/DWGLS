# DWGLS Walkthrough — ประกอบใหม่ทีละชั้น + recheck สด (2026-09-13)

> วิธีอ่าน: เริ่มจากฐาน [1] ขึ้นไป [7] ทุกชั้นมี Claim → หลักฐาน (รันสดวันนี้ ไม่ใช่อ้างอิง) →
> Direction check (✅ ถูกทิศ / ⚠️ ระวัง / ❌ ทิ้ง) → Verdict
> บันทึกความจำ: ctx_memory #801/#802/#803 (+ #795 roadmap, #798 field-vs-adapter, #799 anti-cubic)
> handoff: cloud `shf/dwglsnativef/2026-09-13` + obsidian `[[Memory/Sessions/2026-09-13_dwgls-native-fs]]`

---

## [1] FIELD — ฐาน

**Claim:** ระบบคือ field ไม่ใช่ data manager — geometry เป็น address space,
coordinate = address, ไม่ hash/lookup, int-only.

**Recheck (เลขคณิต ไม่ต้องเชื่อใคร):**
- `20736 = 144² = 12⁴ = 2⁸×3⁴` → 256×81 = 20736 ✓
- stride-37 เป็น prime, `20736 mod 37 = 16 ≠ 0` → coprime จริง → bijection ไม่ชน
- `core/geo_box_axes.h:4-12` — field ใหญ่ **ไม่ใช่** cubic volume แต่เป็น
  6 deterministic paths ทับบนกล่องใบเดียว (address = axis, position, local)

**Direction:** ✅ ฐานเลขลงตัว / ⚠️ หลุมที่ห้ามตก: ขาย field ว่าเป็น compressor
(แพ้ zstd), วาดเป็น 3D volume (code ปฏิเสธแล้ว) / ❌ ทิ้ง: hyperbolic float
Cayley เก่า (`hyperbolic_seek.h` deprecated), KIS v5 (value-as-address ชนกัน)

**Verdict: ฐานแน่น**

โครงสนามที่ยืนยันด้วยภาพ (rhombus tiling + 3×3×3 cube): ภาพ 2D (3 rhombi ชนกัน
= 1 cube projection, 3 ทิศ = 3 Worlds) คือ view function ของ field
(`core/isometric_map.h` มี rhombus addressing จริง) ภาพ 3D คือการอ่านแบบ
stacking — แต่ constraint ที่ภาพไม่บอก: zero-sum (`i+j+k ∈ {0,1}`) ใช้จริงแค่
4 จาก 8 octants อีกครึ่ง derive ผ่าน antipodal (ที่มาของ bipolar 1/2)

---

## [2] KIS — bijection #1 (stride-37)

**Claim:** `slot[i] = (i×37) % 20736` เป็น bijection — แก้บั๊ก v5 ด้วยการแยก
VALUE ออกจาก ADDRESS (`core/kis_codec_v6.h:25-27`)

**Recheck — รันสด `tests/kis_codec_v6_standalone_test.c` → 9/9 PASS:**
- T0: bijection บน [0,20736) ครบทุก slot — **0 collision** (brute-force)
- T1–T7: roundtrip lossless หมด (`mm=0`): same42, alternating ±1, random 10k,
  edge values, full grid 20736, over-chunk 41473, 1M weights
- T10: production API encode/decode/verify ผ่าน

**ซื่อสัตย์เรื่อง ratio:** `ratio=1.1–3.1x` คือ **overhead (ใหญ่ขึ้น)** ไม่ใช่
compression — KIS แลกพื้นที่เพิ่มนิดหน่อยเพื่อ O(1) addressing + verify ได้

**Direction:** ✅ แยก address↔value (บทเรียน v5), inverse ผ่าน modular arithmetic
(`37 × 16813 ≡ 1 mod 20736`, O(1) ไม่ต้อง walk-back)
⚠️ `core/dwgls_codec_kisv6.h` เป็น **dead stub** ใน vtable (ไม่ใช้ stride-37) —
ของจริงคือ `core/kis_codec_v6.h` เท่านั้น อย่าหยิบผิดตัว

**Verdict: bijection พิสูจน์สด**

---

## [3a] HYPERBOLIC — bijection #2 (ขาคู่ของ KIS)

**Claim:** hyperbolic ใหม่ = deterministic centroid walk (route + key frame +
f(step)) ไม่ใช่ float Cayley — strides `{1,9,81}` (mixed-radix `20736=2⁸·3⁴`)
คี่หมด → parity flip 100%, stride-1 ครอบทั้ง field = 1 keyframe

**Recheck — รันสด `tests/test_geo_hyperbolic.c` → ALL PASS:**
- T0: stride ตรง spec `{1,9,81,27}` ครบ 4 แกน · T1: stride-1 เดินครบ 20736
  ไม่ซ้ำ กลับบ้านพอดีรอบเดียว · T2/T3: reversible ทุก node × ทุกแกน + parity
  flip ทุกก้าว · T4: store ตรง reference `th_cell_anchor` อิสระ (ไม่ตรวจตัวเอง)
- T5/T6: enter-anywhere + centroid idempotent

**โครงสร้างข้างใน (frame seek):** hyper เป็น deterministic container หั่น
cell เท่าๆ กัน (`geo_hyperbolic_store.h:64-69`: cell k คลุม
`[k·cell_size,(k+1)·cell_size)`, centroid = anchor) reconstruct ด้วย stride-1
walk `f(step)` — ส่วน frame seek (`geo_frame_seek.h:56-59`) ใช้ DNA เดียวกัน:
`frame_enc(t) = (t×37) % 1440`, face = enc/120 (timeframe), phase = (enc/12)%12
(round) — ที่อยู่ = รอบที่เท่าไร × ตำแหน่งในรอบ

**Direction:** ✅ ลำดับถูก: KIS (address/int) กับ hyper (route/geometry) เป็นแฝด
คนละโครง — ต้องมีทั้งคู่ก่อน fan24 ถึงมีความหมาย / ❌ ทิ้ง: tetra-roll
(orbit แค่ 2–6, state ใหญ่กว่าโดยไม่ได้อะไร)

**Verdict: hyper พร้อมเป็นขาคู่ของ KIS**

---

## [4] BREATHING — กลไกหายใจ (demo cell scope)

**Claim:** anchor เป็น constraint เคลื่อนที่ → delta bounded ถาวร → compact ฟรี
ข้างทาง (side channel ขนานกับ main path, payload ไม่ถูกแตะ)

**ทำไมเกิดตอนนี้ได้โดยไม่รอ fan24:** breathing ใช้แค่ seeker+scale+anchor
(มีครบจาก field/hyper) — fan24 เป็นแค่รูปแบบสายส่งของ event ที่ breathing
ผลิต (encoder มาทีหลังได้ ของที่ encode ต้องเกิดก่อน)

**Recheck — รันสด `tests/test_bfs_breath.c` → 22/22 PASS:**
- T1/T2: 1000 breaths + ลง hyperbolic ลึก — delta ไม่หลุด bound, anchor ตามจริง
- T3: int8 encode/decode exact + clamp 2 ฝั่ง · T4 (สำคัญสุด): main path
  lossless ตลอด 500 interleaved breaths · T5: delta layer เล็กกว่า 4×
- T6: 5000 breaths ยัง bounded + lossless (reanchor 26 ครั้ง)

**Scope ที่ต้องซื่อสัตย์:** ทั้งหมดเกิดบน **BFS demo cell**
(`breathing_fs.h:115`: `block_data[144][144]` = 20,736 bytes) — กลไกจริงแต่
สนามเล็ก ของ production (GB) ใช้**ลูกของมัน** (MEM_RESERVE: จองอากาศ
3.7–5.2GB, commit เฉพาะ active region → RSS 48MB เปิดไฟล์ 3.5GB) ไม่ใช่ code
ตัวเดียวกัน — เอา 22/22 ไปอ้างว่า production พร้อมตรงๆ ไม่ได้

**ของที่เล็กลงอยู่ฝั่ง hyper:** `home_pos` (KIS/anchor) คงที่ ส่วน `delta =
home×(scale−1)` + int8 layer + re-anchor events (hyper/log) คือตัวที่เล็กลง 4× —
scale ต่ำกว่า 1 เข้า `[HYPERBOLIC]` (window > space) ทุก block พร้อมกัน

**ห้ามเรียก compression ของข้อมูล:** ตามรอย 1 byte — payload วางที่ home อ่านที่
home เท่าเดิมเสมอ (T4) ส่วนที่ "เล็กลง 4×" คือสมุดบันทึกการขยับ (16B→4B)
BIMG persist overhead จริง ≈ **1.215× (ใหญ่ขึ้น)** — ยอมจ่ายแลก O(1)+verify

**Verdict: กลไกพิสูจน์สด — เหลือแค่ห่อ event ลงสาย**

---

## [5] FAN24 — สายส่ง + bijection #4 (CRT)

**Claim:** ring-24 = เฟืองร่วมของแฝด (KIS 8 ฟัน × hyper 3 ฟัน, gcd(8,3)=1 →
CRT bijection) — log เก็บแค่ Δ (8 บิต/event, RIM 3 บิต) ไม่มี absolute
position แม้แต่ seed (`core/fan24_gear.h`)

**Recheck — รันสด `tests/test_tess_scale_log_gear.c` → 19/19 PASS:**
- O1/O2: CRT ตรง brute-force ครบ 24 cells + encode→decode exact ครบ 20736 pairs
- O4: Δ=0 ไม่ปล่อย event (home ไม่พูด) · T5/T6: อ่านผิด scale ไม่ replay →
  เพี้ยน 1008/1008 / replay → lossless 1008 · T7b/T7c: backward walk หา seed
  เอง + late joiner เข้ากลางทาง lossless (enter-anywhere จริง)
- T8: RIM @1000 events = 387B vs baseline 2000B (19%) · O6: split ผิด (12,2)
  ชนกันจริง (`0~12`) — fence ห้ามใช้ addressing

**CRT แบบช้าๆ:** วง KIS อ่านได้แค่ `s mod 8`, วง hyper อ่านได้แค่ `s mod 3` —
แต่เพราะ gcd(8,3)=1 คู่เศษไม่ซ้ำกันเลยใน 24 ช่อง (เช่น s=5,13,21 วง KIS อ่าน 5
เหมือนกันหมด แต่วง hyper อ่าน 2/1/0 ต่างกัน) ย้าย 5→61: Δ=56=24×2+8 →
`{q=2,dc=0,dx=2}` แต่ละฝั่งเดินวงตัวเอง ไม่ต้องมีนาฬิกากลาง
CRT = Chinese Remainder Theorem — **ค้นพบอิสระจาก constraint
(สองฝั่ง sync โดยไม่มีนาฬิกากลาง + ต้องไม่ชน) ไม่ได้ลอกตำรา** เจอ fence
(12,2) ด้วยการทดลองก่อนรู้ชื่อทฤษฎี = proof-by-construction

**ทฤษฎีเดียวค้ำสองขา:** CRT โผล่ 2 ที่ — (1) fan24 (mod 8, mod 3) → ring-24,
(2) stride-37 inverse (`37×16813≡1 mod 20736`) → O(1) reverse ของ v6_slot

**Direction:** ✅ ถูกทิศ + ถูกที่ (เกิดหลัง KIS+hyper ครบ ตามสั่ง) — เป็นสายส่ง
ของ breathing ไม่ใช่ชั้นใหม่ / ⚠️ RIM ใช้ได้เฉพาะ log ที่ Δ≡0 mod 24 ทั้งหมด

**Verdict: สายส่งพร้อม**

---

## Seeker / Magnifier — อยู่ชั้น [4]

- **seeker** (`breathing_fs.h`): scale, space, window, current_pos, home_pos,
  is_hyperbolic = "นิ้วชี้" — ขยับแล้วทุก block derive `current = home×s` ตาม
  = MVCC (ตำแหน่งคือ version, scale คือเวลา)
- **magnifier** (`bfs_magnify.h`): "เลนส์บนนิ้ว" — ครึ่งกลางของ 144 cells เป็น
  กระจก (อัตราขยายใน/นอกต่างกัน) + กฎ antipodal `a_w × a_{w+72} ≡ 1 (mod 144)`
- field นิ่งสนิท นิ้วเท่านั้นที่ขยับ — trace จะเห็นแค่ scale byte + seeker +
  metadata O(144) + gear events วิ่ง ส่วน payload นิ่ง ("ทั้งสนามวิ่ง" คือภาพ
  ที่คำนวณตอนอ่าน ไม่ใช่ของเคลื่อนที่ — เหมือนดาวหมุนเพราะโลกหมุน)
- sync 3 ฝั่งผ่านเส้นเดียว: ไทม์ไลน์ร่วม (`scale_bridge.h`: 1 ฟัน = 1 semitone
  = ×2^(1/12), `s(W) = 2^(−W/12)`) + สาย fan24 + scale byte ใน header ของ image
  ที่ map อยู่ (จุดร่วม CPU/GPU/BFS)

---

## [6] TESSPACK — ท่อส่งของจริง (GB)

**Claim:** bake → pack → verify → assemble = ไฟล์ไป-กลับ byte-identical
(capo = 1 tesseract = 1152 slots, stride-37 scatter + CRC-64)

**Recheck 3 บันได:** ① สเกลเล็ก (รันสด `tests/test_tesspack.c` → 3/3:
20736+10000+500 cells verify จาก pack ตรงหมด) ② สเกลจริง (ledger: real-pack
**44,319 capos 0 fail** + multi-format 4 models) ③ ปลายทาง (assemble → GGUF →
llama.cpp **logits BITWISE identical 12/12**)

**Overhead ซื่อสัตย์ (0–36.5% ตาม quant):** Kokoro 0% · Qwen3.5-2B Q8_0 +3.1% ·
LFM +5.4% · Qwen3-VL +24.5% · Bonsai Q1_0 +36.5% — ส่วนเกินคือค่าธรรมเนียม
(cube-0 index ทุก capo + CRC + codebook) ของ O(1) + เปิดทีละ capo + verify
**คุ้มเมื่อ serve (จ่ายดิสก์ถูกครั้งเดียว ~$20/300GB ประหยัด VRAM แพงทุกเดือน +
transfer 93.8%) ไม่คุ้มเมื่อเก็บเฉยๆ** (archival ใช้ zstd) — overhead โหด
เฉพาะ quant เบา (cell เล็กแบก index เท่าเดิม) ลดได้ด้วย capo ใหญ่ขึ้น +
บีบ metadata (มี structure ต่างจาก weight)

**Direction:** ✅ verify แบบ decode→compare ทุกค่าทุกตำแหน่ง / ⚠️ warnings ตอน
build (`strict-aliasing` fp16, implicit ggml decl — หนี้ก่อน v1)

**Verdict: ท่อปิด**

---

## [7] SERVE + ADAPTERS — โครงครบ รอต่อ (ชั้นเดียวที่ยังไม่ปิด)

**Recheck:** ✅ `tools/bake_gcube.c` compile ผ่าน (adapter GGUF + safetensors)
✅ `tools/tesspack_server.c` มีอยู่แต่ `#include "llama.h"` (external backend)
✅ MoE route / KV park / breathe_view ไฟล์ครบ (audit ตั้งแต่ consensus R2)

**ระบบไม่ใช่ของ GGUF:** field ไม่รู้ว่า byte คืออะไร (5 data types ผ่าน code
เดียวกัน lossless หมด) — GGUF (`gguf_reader.h`) คือ adapter ใหญ่สุด + รถพิสูจน์,
safetensors/ONNX (Kokoro 0%) คือ adapter อื่น, LLM inference (llama.cpp) คือ
**flagship demo ไม่ใช่ตัวตน** — stack ที่ถูก: bytes(any) → adapter → field →
serve → ผู้บริโภคใดก็ได้ v1 เลือก GGUF+llama เพราะ proof ครบสุดเท่านั้น

**งาน v1 ที่เหลือ (M/L — ไม่ใช่งานวิจัย):** server loop + MoE router join +
KV park path + MEM_RESERVE threaded lifetime + mmap↔GPU coherency

---

## View layer — bijection #3 (D4) + voronoi/multi-pointer

- **D4 orientations (mark เพิ่ม, รันสด `build/d4_bij.exe`):** 144 points × 8 ops,
  `op∘inverse = identity` ครบทุกจุด **fails=0** — inverses:
  rot90↔rot270, mirrors/diagonals self-inverse (`core/geo_fractal_addr.h:222-243`)
  ช่วย 3 เรื่อง (ไม่มีบีบค่า): สลับมุมมอง Hardware↔Natural↔Flat ด้วย triality
  permute O(1) ไม่ย้าย byte (ฐาน GPU zero-copy) · ให้เลข 144 มีที่มา
  (`192−48=144`, `36×4=144`) · หมุน cell ฟรี 8 ท่า (ญาติ antipodal)
- **voronoi mask + multi-pointer:** มีจริง (`geo_voronoi_mask.h`,
  `geo_fs_voronoi.h`, `geo_multi_pointer.h` — 6 axis views ของกล่องใบเดียว,
  `vm_verify: PASS`) mask = ผู้คุมรัศมีแรงขยาย (กันสเกลระเบิดตอน field ใหญ่),
  multi-pointer = นิ้ว 6 นิ้วพร้อมกัน
- 🚩 **regression:** `tests/test_voronoi_mask.c` เก่ากว่า header
  (`vm_masked_seek` ต้องการ `Spotlight*` เพิ่ม) → compile ไม่ผ่าน ต้องซ่อม

---

## ทะเบียน 4 bijections (พิสูจน์สดทั้งหมด 2026-09-13)

1. **stride-37** `v6_slot` บน 20736 — 0 collision + inverse 16813 (O(1) reverse)
2. **hyperbolic walk** stride-1 orbit เต็ม + centroid idempotent
3. **D4** 8 orientations (144×8, 0 fail) + triality 3-cycle HW↔NAT↔FLAT
4. **CRT** `s ↦ (s%8, s%3)` บน Z24 (24/24 unique, fence (12,2) ชนจริง)

**กฎเหล็กข้อมูล:** permutation ลด entropy ไม่ได้ — geometry จัดระเบียบที่อยู่
ไม่เคยลดความสุ่มของค่า (zstd บน sequential vs scatter-37 ได้ขนาดเท่ากัน)
ของเล็กลงจริงมีแค่: antipodal 1/2 (compression จริง) + MoE/sig32 (transfer-less)
+ keyframe (store-less) + int8 log (log-compact) — **payload ไม่เคยหดสักบิต**

---

## ชิ้นที่เก็บตก — residual / version control / transport components

ที่อยู่ของแต่ละชิ้นใน stack (ไม่มั่วชั้น):

**Transport (ระหว่าง hub → ผู้รับ — ใต้ Step 5/6):**
- **fibo_spine** (`core/infra/fibo_spine.h`) — พิธีขนส่ง: flat id → (pipe 1728,
  tick 12) → ลาก tick ถึง 11 → `jet_bridge_hop` ใช้ร่วมกันทั้ง BFS hub และ rail hub
- **jet_bridge** — ประตู tick-11 (ทางออกวงรอบ) กระโดดผ่าน residual_space
- **residual_space** (`core/residual_space.h`) — ช่องทางกายภาพที่ jet bridge
  กระโดดผ่าน (shared memory CPU↔GPU)

**Memory + sync substrate (ใต้ Step 4–7):**
- **drantile/DRamTile** (`core/infra/geo_dram_tile.h`) — สระหน่วยความจำรวม
  GPU-CPU (anchor×128 + Hilbert 8×8, page-pinned, mmap-backed) — ตัวทำ 47 GB/s
- **gear_lock** (`core/infra/gear_lock.h`) — sync ฝั่ง CPU (world counters
  128×162 = 20736) ทั้งสอง hub มาตอกบัตรที่นี่
- **geo_rail_hub** (`core/geo_rail_hub.h`) — tensor hook รุ่นก่อน tess-serve:
  `.gcube` mmap → caller (`geo_rail_pull` คืน pointer ตรง, bench 131M pulls/s)

**Version control (ตัดขวาง — หลักอยู่ Step 4):**
- **seeker MVCC**: ตำแหน่ง = version, scale = เวลา (`bfs_mvcc_snapshot/restore`,
  ring 8 slots) · BIMG persist MVCC ≤8 versions (scale change = timeline version
  ใหม่) · ghost log = audit trail ถาวร (EXPIRED ไม่ลบ เก็บเป็นร่องรอย)

**KV state machine (Step 7/v3):**
- **kv_remap_rail** (`core/kv_remap_rail.h`) — PARK → SCAN → PATCH/REBUILD →
  PARK (+FREEZE) ตาม threshold 15/40/85% — นี่คือเครื่องจักรของ KV park/resume

**Alternate addressing (ข้าง Step 2/3):**
- **geo_jump** — อยู่ **นอก repo** (`collection/geo_jump_module/`, ต่อผ่าน
  `core/geo_sync_bridge.h`): สูตร `node = face·1728 + tick·144 + local`
  (12·12·144 = 20736 เลขเดียวกัน แยกองค์ประกอบคนละแบบ)
- **rail_sync / rail_ring / phase_rail** (`core/infra/geo_rail_*`) —
  เครื่องจักร dual-rail เก่า (PARK/OPEN/REWIND gates ผ่าน peer XOR) ปัจจุบัน
  **parked** (รายละเอียดหลุดจาก context ปัจจุบัน เหลือแค่ใน memory #135) —
  อย่าสับสนกับ `geo_rail_hub` (คนละตัว: นั่นของใหม่ใช้งานจริง)

---

## คิวต่อไป — wiring (ตกลง 2026-09-13, บันทึก #804)

ชุดหลัง (A2×A2/D4-triality/fractal/quadtree/param_grid) + L-block + geo_jump
bridge + quadtree = **proven islands**: wire กันเองภายใน + test/bench/probe
ผ่าน แต่ `tools/` สาย product ไม่มีใคร include เลย
1. ซ่อม `test_voronoi_mask.c` (Spotlight arg) — กู้ความเชื่อ CI
2. เอา D4 triality 3-views เข้าท่อ serve/GPU zero-copy
3. L-block + geo_jump bridge เป็น placement adapters
4. quadtree accel ก็ต่อเมื่อ seek วัดได้ว่าช้า
5. rail_* parked ต่อ — **กฎ: ไม่สร้าง geometry ใหม่จนกว่าเกาะจะโดน wire หมด
   (พักวิจัย เข้าเฟส integration)**

---

## สัญญา + สิ่งที่ห้าม (ล็อกถาวรจนกว่าจะมีหลักฐานใหม่)

- **Anchor contract:** อ่านที่ home = lossless ฟรี · ผิดที่+replay = lossless แบบจ่าย
  · ผิดที่+ไม่สน anchor = เพี้ยนอย่างซื่อสัตย์ (T5: 1008/1008)
- **ห้ามขาย compression** · **ห้าม claim "23% smaller"** (ledger มีแต่ overhead)
  · companion = demo shell ไม่แข่ง Ollama · sig32 = sidecar แถม
- **ทิ้งถาวร:** float Cayley, KIS v5, cubic-volume field, BFS-cubic re-arch (144³ =
  2.99M slots = 144× — อยู่นอก v1), `dwgls_codec_kisv6.h` stub
- **Roadmap (#795):** v1 tess-store+serve (Linux x86_64 NVMe, llama backend, demo
  Qwen3-4B/8GB) → v2 tess-moe (เงิน: 10–15% ของที่ประหยัด, เริ่ม llama ก่อน) →
  v3 perf pack (KV park + GPU zero-copy) · gate: ลด $/M-token ≥30% หรือ latency
  ≤1.2× ไม่งั้นไม่เป็น product หลัก · ต้องมี batch 1/8/32, RSS/tok-s/p99/soak 1–4h
  ใต้โหลด, corrupted/fuzz/audit ก่อนขาย
