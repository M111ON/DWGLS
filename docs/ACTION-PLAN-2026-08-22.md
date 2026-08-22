# ACTION PLAN — แผนดำเนินการต่อ (ตั้งแต่ 2026-08-22)
## ฐานปัจจุบัน: TIER1 112/112 · breathing proven · serving ~90% peak · honest status = docs/HONEST-STATUS-2026-08-22.md

> กฎของแผนนี้: **ทุก phase ต้องมี acceptance gate ที่วัดด้วย oracle**
> ผ่าน gate ก่อนไป phase ถัดไป — failure เก็บเป็นบทเรียนใน doc เสมอ

---

## Phase 0 — Toolchain (เงื่อนไขของ Phase 3)
**งาน:** ติดตั้ง winlibs GCC 13+ portable → `I:\tools\gcc13`
**เพราะ:** mingw64 gcc 8.1 เก่าเกินสำหรับ llama.cpp source (std::filesystem bug)
**Gate:** compile `tools/server` จาก I:\llama.cpp สำเร็จ + TIER1 เดิม 112/112 ยังเขียว (regression check ว่า gcc ใหม่ไม่ทำ DWGLS พัง)
**ขนาด:** S

---

## Phase 1 — Single Sparse Backing (ฐานของ storage)
**งาน:** geo_cube_serve variant ที่ bake ลง **1 sparse file** (FSCTL_SET_SPARSE,
offset = dram_addr × CHUNK) แทน RAM VirtualAlloc window
**เพราะ:** ตัด naming weight (569 files → 1), free space ในไฟล์ = negative space จริง,
เป็นฐานของ multi-model
**Oracle/Gate:**
- VERIFY byte-identical เท่าเดิม · 6-view XOR match เท่าเดิม
- Explorer เห็น 1 ไฟล์ · ขนาด on-disk ≈ payload (holes ไม่กิน)
- perf ไม่ต่ำกว่า RAM window เกิน 10% (mmap-backed)
**ขนาด:** M (~100–150 บรรทัด บน twin store pattern)

---

## Phase 2 — Negative Port (overflow → residual)
**งาน:** เมื่อ parts > window slots: ส่วนเกินไปจอด **residual registry**
(bond = {model_id, part_id → disk offset}) แทน FAIL; read path replay bond
ดึงจาก disk spill ได้ lossless
**Test case จริง:** **LFM2.5-2.6B** — 22,014 parts > 20,736 slots (ตอนนี้ FAIL)
**Oracle/Gate:**
- LFM2.5 bake สำเร็จ: in-field parts + residual parts
- pull ทุก tensor byte-identical ครบโมเดล (in-field memcmp + residual memcmp vs source)
- carried cost: bond entries ∝ overflow parts เท่านั้น
- LIFO unwind rule ยังคงผ่าน (breathing test รันซ้ำหลังมี port)
**ขนาด:** M–L

---

## Phase 3 — Raw K/V Hook (ปิดโจทย์ delta ∝ events)
**งาน:** patch llama.cpp (ต้องมี Phase 0): expose pointer ต่อ layer ของ K/V cache
→ dump real KV **ที่ level ของ cache buffer** (ไม่ใช่ serialized state) →
rerun kv_real_multiturn_bench
**เป้าหมายวัด:** delta/KV% ต้องร่วงจาก 100–107% มาที่ ~new-tokens% (append-only จริง)
**Oracle/Gate:**
- resume memcmp full-cache == live ✓ (lossless ไม่เปลี่ยน)
- delta size ∝ tokens เพิ่ม ไม่ ∝ context เต็ม — ถ้ายังไม่ได้ = บันทึก negative result
- resume latency ≤ 2× memcpy floor (จาก 20–45×)
**ขนาด:** L (patch C++ + rebuild)

---

## Phase 4 — Multi-Model Universe (ภาพ 20 โมเดล)
**งาน:** model-slot allocator (partition window/residual ต่อโมเดล) +
jump API `point(model, tensor)` → resolve O(1)
**ต่อเมื่อ:** Phase 1+2 เสร็จ (ต้องมี single backing + residual registry ก่อน)
**Oracle/Gate:**
- bake ≥3 โมเดลรวมกันใน universe เดียว (Qwen2.5 + SmolLM + Kokoro)
- cross-model pull ทุกคู่ byte-identical
- coordinate collision = 0 (bitset sweep ทั้ง universe)
**ขนาด:** L

---

## Phase 5 — Adaptive Serving (เรียนจาก colibri)
**งาน:** (ก) heat counter ต่อ slot → hot/cold promotion บน twin dual;
(ข) prefetch next-part ใน pull path; (ค) mirror striping ต่อ disk (deterministic fold parity แทน hash)
**Oracle/Gate:** RAND pull latency ลงจาก 1.35ms; sweep ≥95% peak คงเดิม;
**ห้าม** non-deterministic output (replay ต้องได้ bytes เดิมทุก policy state)
**ขนาด:** M ต่อรายการ

---

## Ongoing — Cross-machine Validation
- `geo-views-package.zip` พร้อมอยู่แล้ว — รันบนเครื่องอื่น: tests ALL PASS +
  bw_probe ceiling ใหม่ + cube_serve GB/s เทียบสัดส่วน peak
- คาดหวัง: correctness invariant ข้ามเครื่อง · throughput scale ตาม hardware

---

## Dependency Graph

```
P0 toolchain ──┬──▶ P3 raw K/V hook
               └──▶ (llama.cpp wire-in, later)
P1 sparse backing ──▶ P4 multi-model universe
P2 negative port ──┘
P5 adaptive (anytime after P1)
```

## ลำดับแนะนำ
**P0 (S) → P1 (M) → P2 (M-L)** ให้ได้ก่อน: storage สะอาด + overflow ไม่ FAIL
— จากนั้น P3 ตอบโจทย์ entropy ที่เหลือ แล้ว P4 คือภาพ 20 โมเดลที่คุณวาดไว้
