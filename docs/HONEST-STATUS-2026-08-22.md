# HONEST STATUS — ระบบทำงานยังไง และพิสูจน์ไปถึงไหนแล้ว
## วันที่ 2026-08-22 · branch feature/geo-native-fs · TIER1 112/112 + TIER2 4/4

> เอกสารนี้เขียนด้วยกฎเดียว: **ทุก claim ต้องมีหลักฐานชี้กลับ และทุกสิ่งที่ยังไม่พิสูจน์ต้องบอกว่ายังไม่พิสูจน์**
> ห้าม wording สวยกว่าตัวเลข (กฎ #179)

---

## 1. ระบบนี้คืออะไร (หนึ่งย่อหน้า)

ระบบจัดเก็บที่ทำงานบน **โครงสร้างคงที่ (deterministic container)** — slot ทุกใบ
เท่ากันหมด (128KB), cell = 144 slots, tesseract = 1152, window = 20,736 slots
(= 128×162 = 144²) ข้อมูลจอดตามพิกัดที่คำนวณจาก content/seed ล้วน —
**ไม่มี hash, ไม่มี lookup table, ไม่มี directory** ระบบไม่รู้และไม่สนใจว่า
ข้างใน slot คืออะไร การันตีข้อเดียว: *พับ/ย่อขยาย/หมุน view แล้วเนื้อข้อมูล
เหมือนเดิม 100%* — พิสูจน์ด้วย memcmp/XOR กับต้นทางทุกครั้ง

---

## 2. กลไก (ทั้งหมด pure int, header-only)

| ชั้น | ไฟล์ | ทำอะไร |
|---|---|---|
| tri↔square | `core/iso_rot90.h` | bijection (4×4)×(3×3)=12×12 บน 144 slots; rot90/rot270 = inverse pair |
| fold | `core/iso_fold.h` | 1 tesseract (1152) = 9 anchors (9×128); inverse Hilbert d2xy |
| cube views | `core/kis_cube_views.h` | 6 มุมมอง = S₃ permutations บน 12³ = FS_PIPES |
| hardware addr | `core/geo_dram_tile.h` | dram_addr = anchor×128 + hilbert_8x8(x,y,layer) |
| serving | `tools/geo_cube_serve.c` | bake GGUF → window, pull ผ่าน 6 views |

---

## 3. สิ่งที่พิสูจน์แล้ว ✅ (พร้อม oracle)

### 3.1 Bijection ทุกชั้น (TIER1, sweep exhaustively)
- `test_iso_rot90`: rot90 bijection ครบ 144 slots · rot270 mutual inverse ·
  hand-computed values (slot 89↔81 ฯลฯ) · fixed points {0,11}
- `test_iso_fold`: unfold(fold(g)) == g ครบ **20,736** จุด · tes k อยู่ anchors [9k, 9k+8] เป๊ะ ·
  inverse Hilbert ตรง forward function 64/64 จุด
- `test_kis_cube_views`: 6 views bijection ครบ 1,728 · mutual inverses ·
  order structure ตรง S₃ (swaps=2, cycles=3)

**Oracle:** counting/bitset sweep ครบ domain + ค่า hand-computed — ไม่มี expected จาก implementation

### 3.2 Bake/Pull GGUF จริง lossless (ratio=1 path)
`geo_cube_serve` บน Qwen2.5-0.5B (5305 parts, 669.8MB):
- VERIFY byte-identical 0 bad parts (memcmp vs source mmap)
- 6-view sweep: XOR == zero-padded source digest **ทุก view**

### 3.3 Scale≠1 ("หายใจ") บน weights จริง
`geo_breathing_test v2`: bake @scale1 → expand×2 (holes) → shuffle S₃ →
unshuffle → collapse → home:
- memcmp 5305/5305 **ทุก state** · holes clean · bijection unique ·
  whole-window XOR MATCH · DIAG 0/20736 slots differ
- carried state = **4 events / 32 bytes** เท่านั้น
- ⚠️ transform ผสมจริง: expand ย้าย bytes + เกิด holes, shuffle เป็น S₃ permutation —
  v1 แรกเป็น tautology (L/R==fid) โดนจับได้และเขียนใหม่

### 3.4 Multi-model generality
code เดิมไม่แก้ ผ่าน 5 โมเดล: Qwen2.5-0.5B · Qwen3-0.6B · SmolLM2-360M ·
Kokoro-TTS · smolVLM-text — lossless + 6-view ทั้งหมด

### 3.5 KV cache park/resume lossless
`kv_real_multiturn_bench` บน state file จริงจาก llama.cpp b9733:
- resume ผ่าน DRamTile twin store → memcmp full buffer **ALL OK**
- skeleton compression บน real KV = **4.09×**

### 3.6 ประสิทธิภาพ (hardware นี้: GTX 1050 Ti era desktop)
| งาน | ค่า | % ของเพดาน |
|---|---|---|
| RAM read peak (bw_probe) | 10.72 GB/s @8thr | = เพดานเครื่อง |
| 6-view sweep | 8.4–9.7 GB/s | ~90% ของ peak |
| raw mmap memcpy baseline | 8.65 GB/s | — |
| bake 5305 parts (parallel) | 0.55–0.7s | — |
| CPU tg requirement | 5.45 GB/s @ 8.08 tok/s | window เลี้ยงไหว headroom 1.5× |

### 3.7 Read–Write Identity (สมการ)
WRITE@p → SCALE ×k (bytes นิ่ง) → READ ×(scale_now/scale_then):
ratio=1 → อ่านจุดเดิม lossless ทันที · ratio≠1 → replay log → lossless —
**ทั้งสอง path ผ่านการทดลองบนข้อมูลจริงแล้ว** (3.2 + 3.3)

---

## 4. สิ่งที่ยังไม่พิสูจน์ / ยังไม่ทำ ❌⚠️ (โปร่งใสเต็มที่)

| รายการ | สถานะจริง |
|---|---|
| **delta ∝ events บน inference จริง** | ❌ NET LOSS — byte-delta บน llama serialized state = 100–107% (state format ไม่ prefix-nested, วัดยืนยัน 98.8% bytes เปลี่ยน) · resume ช้ากว่า memcpy floor 20–45× · ต้อง hook raw K/V buffers ซึ่ง public API ไม่เปิด |
| **multi-model universe (20 โมเดลชี้ข้าม)** | ❌ ยังไม่มี model-slot allocator — เป็น vision ที่ประกอบจากชิ้นที่พิสูจน์แล้ว แต่ยังไม่ได้ build |
| **negative port (window overflow)** | ❌ LFM2.5-2.6B (22,014 parts > 20,736 slots) **FAIL ตรงๆ** — overflow handler ยังไม่มี |
| **sparse backing file** | ⚠️ ตัดสินใจแล้ว (แทน 569 volume files) แต่ยังไม่ implement — วันนี้ใช้ VirtualAlloc RAM window |
| **llama.cpp integration** | ⚠️ วิเคราะห์ bandwidth แล้ว (CPU tg ไม่ตก) แต่ pull API ยังไม่ได้ wire เข้า llama.cpp จริง; build ติด gcc ≥9 |
| **GPU path** | ⚠️ VRAM-resident เท่านั้น — geometry เป็น source tier ต้นทาง ไม่สามารถ feed GPU สดระหว่าง generate (52 GB/s เกิน RAM bus) |

---

## 5. ข้อจำกัดที่รู้แล้ว (sacred/frozen)

- window 20,736 slots × PART_BYTES — โมเดล > 2.66GB (@128KB) ต้องขยาย chunk หรือ multi-window
- event log unwind **LIFO** — collapse หลัง shuffle ไม่ matched = invalid (พิสูจน์จาก failure จริง)
- `DT_HASH_SLOTS` = 512 — dramtile twin store จุ named entries ได้จำกัด
- vacated set = origins ∖ destinations — ย้ายแล้วต้องเช็คจุดว่างที่แท้จริง
- local `hyper` variable ชน Windows headers → ต้องตั้ง `is_hyper`

---

## 6. วิธีการพิสูจน์ (ทำไมอ่านแล้วเชื่อได้)

1. **Oracle อิสระเท่านั้น**: memcmp vs source file/golden buffer · exhaustive
   bitset sweeps · hand-computed values · order-invariant XOR
2. **Anti-tautology**: verify ต้องมี transform ที่มีผลจริง — v1 breathing
   ที่ verify `L/R==fid` โดนจับและเขียนใหม่ (failure ถูกเก็บไว้ใน doc)
3. **Mutation-sensitive**: ทุก bug ระหว่างทาง (relocation หาย, vacated-zero,
   LIFO violation) ทำให้เทสแดงก่อน commit เสมอ
4. **Perf claims ต้องมี baseline วัดเครื่องเดียวกัน** (phase 5 mmap baseline;
   bw_probe ceiling) — ห้ามคำว่า "competitive" ลอยๆ

---

## 7. ประโยคสรุป

> ระบบพิสูจน์แล้วว่า: **container uniform + address=f(data) + index=cycle +
> storage=seed/frame/codec** ทำงาน lossless ได้จริงบนข้อมูลจริงทั้ง ratio=1 และ
> ratio≠1 โดยแบก state ข้าม scale เพียง 32 bytes
>
> ระบบยังไม่พิสูจน์: cross-model universe, overflow port, และ KV delta
> บน interface ที่เราไม่ได้เป็นเจ้าของ — ทั้งหมดอยู่ใน next steps
> พร้อม root cause ที่วัดได้ทุกข้อ
