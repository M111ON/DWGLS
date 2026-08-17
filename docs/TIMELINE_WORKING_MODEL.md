---
luminaCreated: 2026-08-16T06:55:06.747Z
tags: []
luminaModified: 2026-08-16T06:55:06.747Z
luminaVersion: 1.3.11
---
# Timeline Working Model — แบบจำลองการทำงานฉบับสมบูรณ์

> **สถานะ:** ทุกกลไกในเอกสารนี้มี test + ตัวเลขกำกับ (TIER1 43/43 เขียว)
> **ฉบับกระชับ (หลักการราก):** `docs/TIMELINE_FIRST_FOUNDATION.md`
> **ที่มา:** session ต่อเนื่อง 2026-08-14 — จากการวิเคราะห์ codec → สู่แบบจำลองการทำงานทั้งระบบ

---

## 0. เอกสารนี้คืออะไร

เอกสารนี้อธิบาย **แบบจำลองการทำงาน** ของระบบทั้งระบบ — จากโครงสร้าง ไปจนถึงหลักการที่ทำให้ lossless — เพื่อให้ใครก็ตามที่เปิดอ่าน เข้าใจว่า:

1. ระบบเก็บข้อมูลยังไง และทำไม lossless โดยการออกแบบ (ไม่ใช่โดยบังเอิญ)
2. "การบีบ" เกิดที่แกนไหน มีกำแพงที่ไหน ไม่มีกำแพงที่ไหน
3. ทำไมข้อมูลวางแล้วย้ายไม่ได้ และความยืดหยุ่นอยู่ที่ไหนแทน
4. ประวัติศาสตร์: แต่ละทางที่พัง นำไปสู่กฎแต่ละข้อ
5. ทำไมทุกอย่างที่สร้าง = **กายวิภาคของ 1 หน่วย** และ timeline = field ของหลายหน่วย (§13)

---

## 1. ภาพรวม 1 นาที (TL;DR)

```
วางข้อมูล  = เสาเข็มที่ "บ้านเกิด" — ครั้งเดียว, ไม่ขยับ, ค่าเต็มอยู่ตรงนั้น
hyper      = registry {id → home address} — ป้ายชื่อ 2 B ต่อรายการ (∝ จำนวนข้อมูล)
เรียกใช้    = กระโดดไปบ้านเกิด → อ่านค่า → lossless ทันที (O(1), ไม่มีการคำนวณ)
ดู view อื่น = เดิน path (เส้นทางที่วาดไว้ตอนขยับสเกล) — deterministic, replay ได้
inactive   = ปิด link (ไม่ย้ายข้อมูล) — ข้อมูลยังอยู่ที่เสาเข็มเดิม
```

**หนึ่งประโยค:** เราไม่มองค่าข้างในเลย — ค่าเป็นสัมภาระที่วางไว้ที่ที่อยู่ — ระบบทั้งหมดคือการจัดการตำแหน่ง (position), เส้นทาง (path), และป้ายชื่อ (registry) — และ lossless รับประกันเพราะ **พื้นไม่เคยขยับ**.

---

## 2. โครงสร้าง (Layout)

### 2.1 1 Tesseract = 8 cube × 144 scale positions = 1152 slots

```
slot = cube × 144 + w          (cube 0..7, w 0..143)
cube 0 = INDEX frame  (144 slots = 8 blocks × 18) — หน้าประตู, อ่านได้ทุกสเกล
cube 1..7 = DATA      (7 × 144 = 1008 slots)      — ข้อมูลกระจัดกระจายตามสเกล
```

### 2.2 Window 20736

```
20736 = 144² = 1728 × 12 = 18 tesseracts × 8 cube × 144
1 tesseract (ที่ implement แล้ว) = 1152;  18 tesseracts = 20736 (อนาคต)
20736 = หน้าต่าง (window) — payload ต่อ slot เป็น policy อิสระ (ขยายได้)
```

### 2.3 Index frame (cube 0) — ชั้น link แรก

แต่ละ cube มี block 18B ใน index frame: `base(2B) len(2B) checksum(1B) reserved` — checksum = `(sum ของค่าทั้ง cube) % 251` — **ใครก็ตามที่อ่านผิด (เช่น ย้ายค่า) จะถูก flag ที่นี่** (T9a พิสูจน์)

### 2.4 Scale addressing — การแมปตำแหน่งต่อสเกล

```
physical p = (a_w·l + b_w) % 144      gcd(a_w,144)=1 → bijection ทุกสเกล
a_w วน 48 ตัวที่ coprime กับ 144;  b_w = (13·w) % 144 กันซ้ำ
144 สเกล = 144 มุมมอง bijective ของที่เก็บเดียวกัน — ทุกมุมมองครบทุกช่อง
```

### 2.5 frame_seek — การวิ่งบน timeline (frame × step)

```
enc = frame_enc(t) = (t·37) % 1440        (stride-37, 1440-cycle)
w   = enc % 144                           (scale view ที่ timeline ตำแหน่ง t)
1440 = 144 × 10 → หมุน 1 รอบ = scale ครบ 10 ครั้ง
gcd(37,144)=1 → เดิน 144 ก้าว เยือนทุกสเกลพอดีครั้งเดียว
```

**frame_seek คือการวิ่งบน timeline ด้วย frame × time(step)** — แต่ละตำแหน่งของ timeline = 1 frame ที่มองเห็น scale view หนึ่ง — และจาก frame (index cube) ดึง cube ข้อมูลอื่นได้ครบ.

---

## 3. เราไม่มองค่าข้างใน (Cargo / Position)

**หลัก: ค่า = สัมภาระ, ตำแหน่ง = ระบบ.** KIS constrain (3 แกน: cube/octant/route) เป็นตัวกำหนด**ที่อยู่** — เราไม่เคย inspect หรือแปลงค่าเพื่อให้ lossless:

| คำถาม | คำตอบ |
|---|---|
| วางค่าที่ไหน? | ที่อยู่ที่กำหนดโดยโครงสร้าง (bijection, coprime, LUT static) |
| lossless มาจากไหน? | ค่าอยู่ครบที่บ้านเกิด — กลับไปที่เดิม = ได้ค่าเดิม |
| ต้องดูค่าข้างในไหม? | **ไม่** — ระบบจัดการตำแหน่ง; ค่าเป็น cargo |
| ค่าเหมือนกันหลายตัว? | หลายตำแหน่ง (identity รักษา) — การบีบเป็นงานของชั้นอื่น (optional) |

**ตัวอย่าง:** วางค่า `1` จำนวน 100 ตัว → 100 ตำแหน่ง bijection, แต่ละตำแหน่งกู้สำเนาของตัวเองได้ — ระบบ "แกล้งไม่รู้ว่าซ้ำ" เพราะตำแหน่งคือเอกลักษณ์ (ถ้า dedupe เหลือตัวเดียว จะตอบไม่ได้ว่า "ตำแหน่งไหนมี 1") — ความซ้ำถูกเลื่อนไปชั้น delta ที่มองเห็น redundancy และตั้งราคา (ดู §4.1)

---

## 4. สองแกนของระบบ — value axis กับ scale axis

### 4.1 Value axis — กำแพง entropy (วัดบน Q8 จริง: Qwen2.5-0.5B, n=20,736)

| วัด | ผล | แปล |
|---|---|---|
| H(ค่า) | 7.686 bits/val | ขีดจำกัด lossless ของค่าตัวเดียว = **1.04×** |
| plane 0..7 | H ≈ 1.0000 **ทุกอัน** (p1 ≈ 0.5) | Q8 whitened — ทุกบิตเป็นเหรียญ |
| first-diff (เพื่อนบ้าน) | 8.324 bits | แย่กว่า raw — Q8 ข้างกันไม่ correlate |
| block-scale d (2B/32 ค่า) | 0.209 bits/val | ส่วนเดียวที่บีบได้ (2.4×) |
| floor รวม (ค่า + d) | 7.894 bits vs raw 8.5 | **1.077× สูงสุด** — v6 วัดจริง 1.136 (มี overhead) |

**ทำไม:** Q8_0 คือการ quantization — มัน**ทำให้ข้อมูลขาว** (whitening) ไปแล้ว ค่าที่ได้คือ noise กระจายเต็มทุกบิต → ไม่มีโครงสร้างให้บีบ. **บทเรียน: ถ้าใครอ้าง "บีบ Q8 ได้ 10×" — มันคือ lossy หรือ magic.**

**บทบาทของ value axis ในระบบ:** การบีบ/restore ค่า (bit-plane, hex_tile delta, RLE/histogram recipe) เป็น**โหมด optional** — ใช้เมื่ออยากเก็บให้เล็กกว่า raw หรือกู้ bit ที่ view ทิ้ง — และราคาถูกกำแพง entropy เสมอ (ตั้งราคาตามโครงสร้าง: all-0s 30B / runs 51B / random 121B — โครงสร้างเยอะ = ถูก, ขาว = แพง)

### 4.2 Scale axis — path dedup (ไม่มีกำแพง) ✅ พิสูจน์แล้ว (test_tess_scale_dedup)

| วิธี | ขนาด | หมายเหตุ |
|---|---|---|
| naive N-view | N × 1008 B = **145,152 B** (144 views) | เก็บ view ละสำเนา |
| path (full-hop log) | 1008 + 286 = **1,294 B** | log = 1 entry/ก้าว (2 B) |
| path (telescoped) | 1008 + 2 = **1,010 B** | log = {w0→w} เดียว |
| **reduction** | **112.2× / 143.7×** | lossless ทุก 144 view |

**กำแพงของ scale axis ไม่มี** — เพราะเราไม่เก็บ "ค่าที่ต่าง" ต่อ view — เราเก็บข้อมูล**ครั้งเดียว** + path (เส้นทาง) → ทุก view derive ฟรี. **เงื่อนไข:** ต้องมีผู้ใช้ที่อ่านหลาย scale จริง (multi-scale access, progressive, shared space) ถึงจะได้ reduction นี้ — ผู้ใช้อ่าน scale เดียวไม่มีอะไรให้ dedupe (แต่ก็ไม่เสียอะไร — ความสามารถ "อ่าน 144 views จากชุดเดียว" แทบฟรี 2 B/จุด)

**สรุปสองแกน:**

```
value axis:  กำแพง = entropy (Q8 = 1.04×)   → กำแพงจริง วัดได้
scale axis:  กำแพง = ไม่มี (144 views ≈ 1 store) → จุดที่ระบบสร้างความต่าง
```

---

## 5. สามชั้น (Store / Path / Registry)

```
┌─ Registry (hyper) ─ {id → home address} — 2 B/รายการ, ∝ จำนวนข้อมูล
├─ Path (scale log)  ─ {from, to} — เส้นทางระหว่างสเกล, ∝ จำนวนก้าว
└─ Store (piles)     ─ ค่าเต็มที่บ้านเกิด — วางครั้งเดียว, ไม่ขยับ, capacity-bound
```

### 5.1 Store — เสาเข็ม
- วางครั้งเดียวที่ `w0` (append scale) — ไม่เคยเขียนซ้ำ (T8b: reroute ไม่แตะข้อมูล)
- ค่าเต็ม 8-bit อยู่ที่ตำแหน่ง — ไม่มี view ไหนทำลายมัน
- ขอบเขต = **capacity** (20736/window — ขยายด้วย windowing/page/mmap)

### 5.2 Path — แผนที่ (passive scale log)
- entry = `{from, to}` = **2 B ต่อก้าว** — delta ∝ จำนวน scale-change events ไม่ใช่ขนาดข้อมูล
- replay = compose affine maps: `T_i(l) = ((a_f·l + b_f − b_t)·inv(a_t)) % 144`
- **telescope:** เดิน 143 ก้าว = 1 entry `{w0→w}` — path ไม่โตตามระยะ (ผลเหมือนกันทุกประการ — T6)
- path ไม่ใช่กลไก lossless — เป็น**แผนที่**สำหรับอ่าน view อื่นโดยไม่กลับบ้าน (อ่านโดยไม่มี path = ภาพสลับ lossy-looking — T4)

### 5.3 Registry (hyper) — ป้ายชื่อ
- เก็บแค่ `{id → home address}` — **ไม่เก็บค่า, ไม่เก็บ delta** — 14 B ต่อ 7 รายการ
- overhead ∝ **จำนวนข้อมูล (items)** ไม่ใช่ขนาดข้อมูล
- reroute ได้ (เปลี่ยนป้ายชี้ที่อยู่ใหม่) โดยไม่แตะเสาเข็ม (T8)

---

## 6. การเข้าถึง — 3 แบบ + "สองความเร็ว"

| แบบ | ต้นทุน | ความหมาย |
|---|---|---|
| **Pointer-home** | O(1) — dereference, ไม่มีเลขคณิต | ยืนที่บ้านเกิด → อ่านค่า → lossless ฟรี |
| **Frame_seek walk** | O(1) ต่อ step — modular + อ่าน | วิ่ง timeline frame × step — เยือนทุกสเกล |
| **Composed path jump** | O(1) ต่อเป้าหมาย — telescope | กระโดดถึงสเกลใดก็ได้จาก entry เดียว |

**สำคัญ — สอง O(1) ไม่เท่ากัน:** pointer-home กับ frame_seek เป็น O(1) **คนละนิยาม** — อันแรก O(1) ต่อการเข้าถึง (จุดเดียวจบ), อันหลัง O(1) ต่อ step (เดินไกล = หลาย step) — **"เร็วคงที่ในบริบทของตัวเองเท่านั้น"** — อย่าเทียบ constant ข้ามกัน: dereference ≠ modular permutation.

**ทำไมต้องมี path ถ้าแค่กลับบ้านก็ lossless:** เพราะ**ยืนบนบ้านหลังเดียวได้** — เวลา access หลายจุดพร้อมกัน (เช่น inference อ่านหลาย tensor/layer) ตัว anchor ยืนที่บ้านหลังหนึ่ง ส่วนจุดอื่นต้องเดิน path ถึง — step คือมาตรวัดระยะ (odometer) ที่บอก "ห่างจากบ้านกี่ก้าว" — การคำนวณ step มีไว้เพื่อ **navigation ของจุดอื่น** ไม่ใช่เพื่อ lossless (อันนั้น home จัดการ)

---

## 7. หลักความไม่ขยับ (Immutability & Interlock)

**กฎ: วางแล้วเลือนไม่ได้ — ทุกอย่างต้องฝั่งเสาเข็ม. ทำ link reroute ได้.**

### 7.1 สมการ interlock 4 อัน (ทุกอย่างตรึงกันหมด)

```
a_w × a_{w+72} ≡ 1 (mod 144)   ← ทุก w มีคู่ antipode ที่ invert กัน (magnify interlock)
gcd(a_w, 144) = 1              ← ทุกสเกลเป็น bijection — การแมปต้องครบ
stride-37 walk (1440-cycle)    ← เดินครบ ทุกตำแหน่งเยือนพอดี
telescope {w0→w}               ← path compose ได้เพราะโครงสร้างคงที่
```

### 7.2 Cascade failure — "ย้ายแล้วพาคนอื่นไปด้วย" (T9 พิสูจน์)

- ย้ายค่า 1 จุด → map อ่านผิด + checksum ของ cube แตก (frame flag ทันที) — T9a
- เปลี่ยน coefficient 1 ตัว (แม้เป็น coprime ที่ "หน้าตาใช้ได้") → **antipode inversion แตก + telescope แตกทั่วทั้งระบบ** — T9b

**ความเสียหายเป็น global inconsistency ไม่ใช่ local corruption** — เพราะทุกตำแหน่งถูกนิยามเทียบกับทั้งระบบ (คู่ antipode, เส้นทางเดิน, view อื่น) — สมการรั่วจุดเดียว = รั่วทั้งตาราง.

### 7.3 Link reroute — ความยืดหยุ่นทั้งหมดอยู่ที่ link (T8 พิสูจน์)

- dedup: reroute link ของ B → ชี้ pile A → อ่าน lossless, pile B ไม่ถูกแตะ
- version: วางเสาเข็มใหม่ + เปลี่ยน link — ของเก่าไม่ต้องย้าย
- หลัก: **ข้อมูลไม่ถูกเขียนซ้ำเลย — มีแค่ป้าย/แผนที่ที่เปลี่ยน**

### 7.4 Trade-off ที่หลีกเลี่ยงไม่ได้

| ได้ | จ่าย |
|---|---|
| ชี้บ้าน O(1), ทุก view derive ฟรี, registry เล็ก | ทุกตำแหน่งผูกกับทั้งระบบ — **ย้ายไม่ได้เลย** |

filesystem ธรรมดาย้ายไฟล์ = แก้ inode (local) เพราะที่อยู่เป็นอิสระ — ระบบนี้ที่อยู่**ต้อง**ขึ้นต่อกัน (นั่นคือสิ่งที่ทำให้ derive ทุกอย่างได้) — ผลตอบแทนของโครงสร้างที่ตรึงกันคือไม่มีอะไรขยับ. **deterministic + replay ได้ = พื้นไม่เคยขยับ ไม่ใช่โค้ดระวัง.**

---

## 8. Lifecycle — active / inactive

```
inactive  = ปิด link — hyper ถือแค่ {id → home} (2 B) — ไม่ถือ view, ไม่ย้ายข้อมูล
activate  = เปิด link → กระโดดบ้านเกิด → อ่านค่าเต็ม → lossless ทันที (T7/T7b)
deactivate= ปิด link อีกครั้ง — เสาเข็มยังอยู่ที่เดิม
```

**ไม่มี "ย่อ scale ของข้อมูล" — มีแค่ "เปิด/ปิด link"** — เพราะการย่อ/ย้าย = แตะเสาเข็ม = ต้องห้าม (§7). ขนาด footprint ของ inactive = ขนาด registry (∝ items) — ไม่มี overhead ต่อค่า — **value compression เป็น optional ไม่ใช่ส่วนของกลไกนี้**.

> **RAM vs DISK — ต้องพูดให้ตรง:** การ "เล็กลงระหว่างทาง" คือ **working set (RAM)** — สิ่งที่ต้องถือในหน่วยความจำทำงานเพื่อเข้าถึงข้อมูล: inactive = ถือแค่ registry 2 B แทนการถือข้อมูลทั้งชิ้น. **บน disk ไม่ได้เล็กลง** — store ยังเต็ม (ค่าเต็มอยู่ที่เสาเข็มตลอด; registry เป็น metadata ที่เพิ่มเข้ามา ไม่ใช่ของแทน). อยากให้ disk เล็กลงจริง → ต้อง design เพิ่ม (ดู §11.4).

---

## 9. ประวัติศาสตร์ — แต่ละทางที่พัง นำไปสู่กฎแต่ละข้อ

| ทางที่พัง | อาการ | กฎที่ได้ |
|---|---|---|
| ทรงกลม 2 ลูก (beam/radius/unfold) | projection = many-to-one = collision = lossy | residual ต้อง explicit (hyperbolic = backup) |
| scale ด้วย float (×0.1) | drift สะสม — "slot 1 @ scale 0.1 → 9,437,184" | scale = base-2 (2ᵏ/shift) — ห้ามบวก/ลบ |
| scale ด้วย duality ico↔dodeca (20/12 = 5/3) | "1 กลายเป็นทศนิยม 100 set, A-Z กลายเป็นตัวเลข" — position-dependent transform → ค่าเดียวกันแตก | scale = ฟังก์ชันของ (value, scale) ไม่ใช่ตำแหน่ง; geometry = template ไม่ใช่ตัวคำนวณ |
| v5 codec (ถอดออกจาก timeline) | synthetic ผ่าน, ของจริงพัง (probe layout = hidden parameter) | ทุกพารามิเตอร์ explicit + replay ได้ — v6 (order explicit) คือฐาน |
| fixed-point <<16 | precision plateau ที่ 0.00001 — ต้องหา threshold ด้วยมือ | ลิมิต = bit depth ของค่า (d=8 สำหรับ byte) — ไม่มี float ให้ plateau |
| ย้ายข้อมูลหลังวาง (สมมุติฐาน) | สมการ interlock รั่วทั้งระบบ (T9) | เสาเข็มห้ามขยับ — ความยืดหยุ่นอยู่ที่ link (T8) |
| **สนามขยับตามข้อมูล** (structure re-layout: dynamic page/index/adaptive ที่ reshape ตาม content) | field เปลี่ยน = ทุก address ที่นิยามเทียบ field พัง — ไม่มี deterministic/replay | **สนามนิ่ง — ข้อมูลเคลื่อน:** โครงสร้าง (tessellation/window/page/address) ถูก bake ไว้ก่อน; ข้อมูลถูกวาง/เปิด/ปิด link ที่ตำแหน่งคงที่ — ตัว field ไม่เคยขยับตามข้อมูล (หลักรวม เหนือ เสาเข็ม) |

**เส้นเดียว:** ทุกครั้งที่ระบบ "พัง" = มีพารามิเตอร์แฝงหรือโครงสร้างที่ขยับ — ทุกครั้งที่ "รอด" = explicit + deterministic + อยู่กับที่. Timeline ชนะไม่ใช่เพราะเลขสวย — แต่เพราะบังคับให้ order/position/scale/history อยู่ในโครงสร้างที่ replay ได้ 100%.

---

## 10. หลักฐานรวม (ทุกตัวเลขในเอกสารนี้มี test กำกับ)

| test (TIER1) | พิสูจน์ | ตัวเลข |
|---|---|---|
| `test_v5_collision.c` | v5 พังเพราะ hidden order / timeline ไม่พัง | probe chain 20,332 vs 121; v6 lossless 1.136/1.139 |
| `test_tess_trace.c` | rescope เดิม intact, 1:1 identity, delta ladder, RLE recipe | A-Z 26→14→7→4→2→1; 30/30/51/121 B; 15×/10.2×/2.52× |
| `test_tess_scale_log.c` | passive log ∝ events | log 8 B vs data 1008 slots |
| `test_tess_frame_seek.c` | เดิน timeline frame × step, lossless ทุกตำแหน่ง | 144/144; log 143 events; telescope |
| `test_tess_magnify.c` | glass interlock — a_w × a_{w+72} ≡ 1 ทุก w | 12/12; 144/144 ผ่าน log |
| `test_tess_hex_delta.c` | hex_tile residual = explicit | d=0..8 lossless, antipode d=72 |
| `test_tess_scale_dedup.c` | **path + frame_seek = 144 views ≈ 1 store; registry; reroute; cascade** | 112.2×/143.7×; 14 B registry; T8/T9 |
| `build/probe_entropy.c` | value axis = กำแพง entropy (Q8 whitened) | H 7.686; ทุก plane 1.0000; floor 1.077× |

---

## 11. ข้อจำกัดที่ต้องพูดตรงๆ (ไม่มี magic)

1. **Value axis:** บน Q8 (ข้อมูลขาวแล้ว) บีบค่าได้สูงสุด ~1.08× — 2-3×+ ต้องเป็นข้อมูลที่ยังไม่ขาว (f16, mel, image) หรือ lossy (quantization)
2. **Scale axis:** reduction 144× มีจริงเมื่อ**มีผู้ใช้หลาย scale** — อ่าน scale เดียวไม่มีอะไรให้ dedupe
3. **Immutability:** เปลี่ยนอะไร = วางเสาเข็มใหม่ + reroute link — ไม่มี "update-in-place"
4. **Capacity:** 20736/window — โมเดลหลาย GB ต้อง windowing + page/mmap (งานค้าง)
5. **RAM ≠ Disk — ระบบปัจจุบันประหยัดแค่ working set:** "ย่อเมื่อไม่ใช้" = ในหน่วยความจำทำงาน (inactive = ถือ registry 2 B) — **disk ยังเต็มเสมอ** (store มีค่าเต็มที่เสาเข็ม; registry/index เป็น metadata เพิ่ม) — ใครอ้างว่า "ประหยัด disk" กับระบบปัจจุบัน = ภาพลวงตา (ถูก audit ได้). **ถ้าอยากประหยัด disk จริง มี 2 เส้นทาง:**
   - **(a) reference-to-source:** ระบบเป็น index layer — เก็บแค่ registry/index/path (เล็กจริง), ค่าไม่อยู่ในระบบ — activate = อ่านจากไฟล์ต้นทาง (GGUF) — disk ของระบบเล็กจริง แต่ **ไม่ self-contained** (ต้องมีต้นทางอยู่)
   - **(b) in-place compaction:** ลดค่าจริงตอน inactive — **เจอกำแพง entropy**: lossless บีบได้ ~1.08× (Q8) หรือ lossy (ทิ้งบิต → เก็บ view แทนค่าเต็ม) — ไม่มีทาง "เล็กมาก + lossless + self-contained" พร้อมกัน
6. **Boundary — จุดอันตราย/กำไรที่ยังเปิด (2026-08-15):** วางข้อมูลที่ scale ขยาย (w₀+k) แล้วอ่านกลับที่ base → view หด 2ᵏ (base-2 contraction ต่อสเกล) = "compression มากกว่าปกติ" โดย lossless (replay กลับบ้านเกิด). **แต่** เสาเข็มที่วางลึกสำรองพื้นที่จริงใหญ่กว่า view ที่โชว์ — ระบบนับ capacity ที่ base (1×) แต่ของจริง 4× (k=2) → **overcommitment แอบซ่อน** ถ้าไฟล์เยอะ+ต้องขยายพร้อมกัน → ชนขอบ → cascade. **Boundary ยังไม่เป็นชั้น first-class:** เสาเข็มต้องประกาศ envelope `(w₀, depth k, ขนาดที่ w₀+k)`, capacity = Σ envelope ≤ 20736, เกิน = reject deterministic (ไม่ silent). นโยบาย = `MAX_EXPANSION_DEPTH = envelope_depth(gate)` — **ตัดสินใจแล้ว (2026-08-16, §15.32)**: depth จำกัดด้วย marginal ROI ของขั้นที่เข้าไป (ต้อง ≥ gate — ค่าเดียวกับ test_tess_leverage): gate 1.0 → depth 5 (k 4-5 เหมาะสมที่สุด), 2.0 → depth 4 (conservative), 0.5 → depth 6; hard ceiling 7 (fp โตกลับ). ขอ depth เกิน envelope → **auto-lift** เป็น ghost (§15.31) แทนการขยายเสาเข็ม — `core/geo_ghost_envelope.h` + `test_ghost_envelope 39/39`
   - **หลัก:** "ค่าเต็มต้องอยู่ที่ไหนสักแห่ง" — entropy คือการันตีของข้อนี้ — ระบบเลี่ยงได้โดยไม่ถือค่าที่ไหนเลย (reference) แต่แลกกับ dependency
6. **Index/registry เป็น global structure:** cube 0 เสีย = ทั้ง tesseract มองไม่เห็น (แต่ checksum จับได้)

---

## 12. สรุป — ภาพในหัว

```
[ข้อมูล] ─วางครั้งเดียว→ [เสาเข็มที่บ้านเกิด]          ← capacity-bound, ไม่ขยับ
[hyper]  ─เก็บแค่→      [registry: id → home]         ← ∝ items, 2 B/รายการ
[เรียก]  ─กระโดด→       [บ้านเกิด] → ค่าเต็ม lossless  ← O(1) pointer, ไม่คำนวณ
[view อื่น] ─เดิน path→ [ทุกสเกลจากชุดเดียว]           ← O(1)/target, deterministic
[เปลี่ยน] ─reroute→     [link ใหม่ ชี้เสาเข็มใหม่]      ← ข้อมูลไม่ถูกแตะ
```

**ระบบ = การจัดการตำแหน่ง + เส้นทาง + ป้ายชื่อ** — ค่าเป็นแค่สัมภาระที่วางที่ที่อยู่ — lossless รับประกันเพราะพื้นไม่เคยขยับ และทุกอย่าง deterministic + replay ได้ — trade-off ทุกจุดแสดงราคาเป็นตัวเลขให้เลือกเอง (ตรงกับที่ระบบตั้งใจ: ไม่มี magic).

---

## 13. สองชั้น — หน่วย (Unit) กับ Timeline

> **มุมมองที่สูงขึ้นไปหนึ่งชั้น:** ทุกอย่างใน §2-§12 คือ **กายวิภาคของ 1 หน่วย** — timeline ไม่ได้ถือแค่ก้อนเดียว แต่ถือ **หลายหน่วยเรียงตามเวลา**.

### 13.1 หน่วย = ทุกอย่างที่เราสร้าง

```
1 หน่วย = window 20736 (16-bit, พิสูจน์ exhaustive ได้)
        + index frame (cube 0 = หน้าประตู)
        + scale axis (a_w, antipode w+72, magnify glass)
        + path / registry / link / เสาเข็ม
        + lifecycle (active / inactive = เปิด/ปิด link)
        = ครบชุดในก้อนเดียว
```

**ตัวเลขศักดิ์สิทธิ์ (12, 72, 144, 288, 1728, 20736) คือ "ระบบการนับ" ของ 1 หน่วย** — mixed-radix ที่แต่ละหลักข้ามอุปสรรคหนึ่งอัน (2→polarity, 5→Euler, 6→sharing, 12→anchor, 72→bipolar, 144→capacity) — และ 20736 คือคำตอบเดียวของจุดตัด: 16-bit scope + decompose เรขาคณิต + พิสูจน์ exhaustive + LUT ใน cache.

### 13.2 Timeline = field ของหลายหน่วย

```
timeline:  [หน่วย₀] [หน่วย₁] [หน่วย₂] ... [หน่วยₙ] ...   ← ไม่มีต้น ไม่มีปลาย (enter anywhere)
                │
       ตำแหน่ง t = หลักนอกของระบบนับ:
       key = t × 20736 + local   (mixed-radix เพิ่มหลัก — ไม่ใช่ทำให้หลักเดิมใหญ่ขึ้น)
```

- **ภายในหน่วย:** ทุกอย่างที่ §2-§12 อธิบาย (กลับบ้าน O(1), path O(1)/target, registry 2 B)
- **ข้ามหน่วย:** ตำแหน่งเวลา t เป็นตัวจัดลำดับ — หน่วยไหนกำลัง active, ข้อมูลไหลจากหน่วยหนึ่งไปอีกหน่วย
- **หน่วยยังคงข้อได้เปรียบของเล็ก:** เดินครบพิสูจน์ได้ (20 K/หน่วย), address ท้องถิ่น 2 ไบต์, LUT ใน cache — ใหญ่ = มีหน่วยเยอะ, ไม่ใช่หน่วยใหญ่ขึ้น

### 13.3 หลักฐาน — โครงสร้างสองชั้นมีมาตั้งแต่แรก

| หลักฐาน (มีในโค้ด/ออกแบบเดิม) | ความหมาย |
|---|---|
| `fibo_spine` — FS_PIPES = 1728, FS_TICKS = 12 | **"spine" = กระดูกสันหลัง** — หลายข้อ (หน่วย) เรียงบนแกนกลาง อยู่แล้ว |
| Two timers (HANDOFF): frame_seek (1440) + beam_timer (20736) | frame_seek = เดิน**ใน**หน่วย, beam_timer = หน่วยทั้งก้อน — สองระดับมาตั้งแต่ต้น |
| Fib1..4 = tick/cluster/pipe/field (12⁴ = 20736) | timeline มีชั้นภายในอยู่แล้ว — แต่ละชั้น = ระดับของระบบนับ |
| KIS "enter anywhere" (0-20736) | "anywhere" = หน่วยใดก็ได้บน timeline ไม่ใช่แค่ตำแหน่งในหน่วยเดียว |

### 13.4 ข้อมูลไหลผ่าน timeline

```
วาง    = ลงหน่วยที่ตำแหน่ง t (เสาเข็มในหน่วยนั้น)
เข้าถึง = กระโดดไปหน่วยที่ t → ใช้กลไกในหน่วย (กลับบ้าน/path)
ไม่ใช้  = ปิด link ที่หน่วยนั้น — เหลือ registry 2 B
เต็ม/ว่าง = หน่วยถัดไปรับช่วง — chain ต่อเนื่อง
```

**GGUF main (window chain):** โมเดลหลาย GB ≠ 1 window ใหญ่ — = **หลายหน่วยเรียงบน timeline** — แต่ละหน่วยจัดการ 20736 ของตัวเอง (พิสูจน์ได้ exhaustive) — timeline จัดว่าใครอยู่ตรงไหน ใคร active — ค่อยๆ ต่อหน่วยทีละก้อน (สอดคล้องกับ "18tes" = future upgrade ใน AGENTS.md).

### 13.5 สนามใหญ่ = timeline ยาว ไม่ใช่หน่วยใหญ่ขึ้น

- **เล็กคืออะตอม ไม่ใช่เพดาน:** ข้อได้เปรียบของ 20736 (พิสูจน์ได้, 2 B, LUT) อยู่ที่**ชั้นหน่วย** — จึงไม่หายเมื่อ timeline ยาว
- **ขยาย = เพิ่มหลักนอก** (mixed-radix: t × 20736 + local) — ไม่ใช่ทำให้ radix หลักเดิมโตจน "จับต้องไม่ได้"
- **พัฒนาค่อยเป็นค่อยไป:** พิสูจน์ทีละหน่วย → ต่อหน่วยด้วย key ระดับบน — ใหญ่เกิดจากการประกอบสิ่งที่พิสูจน์แล้วซ้ำๆ ไม่ใช่จากการขยายสิ่งที่ยังพิสูจน์ไม่ได้

## 14. Equal-Triangle Floor — พื้นของ KIS (หลักฐานตัวเลขศักดิ์สิทธิ์)

พื้น/container ของ KIS = **equal-triangle tessellation**: สามเหลี่ยมด้านเท่าที่
subdivide เป็น 4 เท่าได้ (ตัดครึ่งทุกด้าน) และประกอบเป็น hexagon ได้ (6 อัน)
ด้วยหลัก 3^n → Peano (กระโดด/เส้นทาง deterministic) — และตัวเลขศักดิ์สิทธิ์ทั้งหมด
**ถูกพิสูจน์ได้ด้วยการแยกตัวประกอบ** ไม่ใช่การเลือกเอาเอง:

### 14.1 20736 = 12⁴ — ตัวเลขศักดิ์สิทธิ์ตัวเดียว หลายหน้า

```
20736 = 12⁴           ← fibo_spine เคยเขียนไว้ ("12⁴ = 20736") — ตอนนี้พิสูจน์ได้
     = (4·3)⁴         ← 12 = equal-triangle 4-subdivision × Peano 3-adic
     = 2⁸ · 3⁴        ← แยกตัวประกอบเต็ม
     = 144²           ← window square
     = 12 × 1728      ← tri_hex_tess (12 pentagons × 1728 nodes, zero gaps)
     = 288 × 72       ← RDH (CELL_288 × pentakis-72)
```

**"เลขเดียวกัน = โครงสร้างเดียวกัน"** — ทุกตระกูล (fibo spine, tri_hex_tess, RDH,
window) วนกลับมาที่เลขเดียวเพราะมันคือ**คำตอบเดียว**ของ space เดียวกัน.

### 14.2 3^n ladder — 20736 → 6912 → 2304 → 768 → 256 → 16:9

```
20736 / 3 = 6912      ← ÷3 แกน x:  (144,144) → (48,144)
6912  / 3 = 2304      ← ÷3 แกน y:  (48,144) → (48,48)   = 48² (coarse 3×3)
2304  / 3 = 768       ← ÷3 แกน x:  (48,48)  → (16,48)
768   / 3 = 256 = 16² ← ÷3 แกน y:  (16,48)  → (16,16)   = 2⁸ ฐาน binary
```

- **สี่ขั้น ÷3 แบบสลับแกน** (x,y,x,y) — Peano-style — ทุกขั้นเป็น integer เต็ม
- จบที่ **256 = 2⁸** — ฐาน grid ที่ binary ล้วน
- **256 : 144 = 16 : 9 พอดี** (cross-multiply 256×9 = 144×16) — ratio อยู่รอดทั้ง ladder
- **144 = 16·9 = 4²·3²** — อัตราส่วน 16:9 ถูก bake อยู่ในด้านของ window เอง

### 14.3 equal triangle — 4-subdivision + hexagon composition

```
4^n  subdivision:  1 → 4 → 16 → 64 → 256      ← 4 ระดับ = ฐาน 256 พอดี
hexagon:           6 สามเหลี่ยม = 1 hexagon     ← area ratio 6 (integer, √3 หักล้าง)
sharing:           1 center + 6 ring = 7 cells  ← = hex_tile HEX_CELLS
สอง ladder ลงตัว:   20736 = 4⁴ × 3⁴ = 256 × 81  ← binary floor × Peano grid
```

### 14.4 Peano 3-adic — กระโดด deterministic (พิสูจน์ exhaustive)

- flat index → (x, y) บน 144×144: bijection ครบ 20736 (ไม่ชน), deterministic
- coarse-grain 3×3 → **2304** เซลล์ (= ladder step 2); 9×9 → **256** เซลล์ (= ladder end)
- แต่ละ 9×9 block = 81 ตำแหน่ง share coarse address เดียว — กระโดด O(1) ด้วย
  integer ล้วน ไม่มีตาราง ไม่มี search: **256 blocks × 81 = 20736**

### 14.5 GGUF main — window chain ประกอบแล้ว (ขั้น ②)

โครงที่ประกอบได้บนพื้นนี้ (`tests/test_gguf_window_chain.c` — Qwen จริง 291 tensors):

```
home(rank) = (rank · 37) % 20736      ← stride-37 (เดียวกับ frame_seek), permutation
rank       = inference order: token_embd → blk.0..N → output_norm → output
w          = home % 144               ← scale view (37·r mod 144 = permutation)
win        = home / 144               ← window id — หลักนอก (window chain)
pointer    = zero-copy เข้า mmap ของ GGUF ต้นทาง (reference-to-source)
```

ผลบนของจริง: 291 tensors ลง window 0..74 (chain), **97.9% ของคู่ติดกันอยู่ใน
pentagon เดียวกัน** (warm locality จาก bond metric), ทุก home lossless เทียบ
direct read.

### 14.6 หลักฐานใน repo

| ไฟล์ | พิสูจน์อะไร | ผล |
|---|---|---|
| `tests/test_tess_sacred.c` | 12⁴, 3^n ladder, 16:9, 4-subdivision, hexagon, Peano | 27/27 |
| `tests/test_gguf_window_chain.c` | tensor chain ลง window, inference order, pointer-home | 11/11 |
| TIER1 (Makefile) | รวมทั้งชุด | 38/38 |
| `core/gguf_reader.h` + `core/gguf_box.h` | dims จริง (int64 per spec) ทะลุเข้า mock header — กั้นทาง llama.cpp | — |

### 14.7 ขั้น ③ — llama.cpp จริง: หัวกิ่งตอน graft เข้ากับต้นตอ (พิสูจน์แล้ว)

`tests/test_gguf_graft_llama.c` — รันด้วย `make graft-llama` (12/12):

```
graft file = [header 0..data_offset) + [body data_offset..end)
           = หัว GGUF จริง (scion) + data ส่งจาก mmap ต้นทาง (zero-copy)
llama      = llama_model_load_from_file(graft) → decode 1 prompt
```

| T | พิสูจน์ | ผล |
|---|---|---|
| T1 | graft re-parse เป็น GGUF — tensor/offset เดิมครบ | ✅ |
| T2 | **llama โหลด graft ได้จริง** — qwen2, embd 896, layer 24, vocab 151936 | ✅ |
| T3 | decode graft vs ไฟล์ต้นฉบับ → **logits BITWISE identical** (151,936 floats) | ✅ |
| T3d | greedy next token เท่ากัน (12095 ทั้งคู่) | ✅ |
| T4 | **reroute link**: สลับชื่อ blk.0/1.attn_q ใน scion → llama โหลดได้ + **logits ต่าง** | ✅ |

**Cactus graft = ใช้งานได้จริงกับ llama.cpp** — "เอาหัว GGUF มาหลอก llama แล้วตัวเป็นกล่องเปล่า zero-copy" ถูกพิสูจน์ครบ: llama อ่านผ่าน box แล้ว**ให้ผลลัพธ์เหมือนอ่านไฟล์ตรงทุกบิต** — และ scion ควบคุม routing ได้จริง (สลับชื่อ tensor = สลับ data ที่ layer ใช้ — logits เปลี่ยน).

**สิ่งที่เจอระหว่างทาง (บันทึกไว้เป็นข้อกำหนด)**

1. **llama validate ลำดับ offset** — แต่ละ tensor ต้อง offset = cursor ที่สะสมต่อเนื่อง → **patch offset โดนจับ** (`failed to read tensor data`) — integrity ของไฟล์ดีเกินคาด; reroute ต้องทำที่ **ชื่อ** ไม่ใช่ offset
2. `llama_tokenize(text_len = -1)` **crash** ใน build นี้ — ต้องส่ง `strlen` ชัดเจน
3. `llama_batch_free(batch)` **crash หลัง llama_decode** (scheduler สลับ pointer ภายใน batch) — เลี่ยง (leak เล็กน้อยใน test)
4. backend ต้อง load เอง (`ggml_backend_load(ggml-cpu-x64.dll)`) — `load_all()` สแกนแค่ dir ของ exe

**สถานะ GGUF main ครบ 3 ขั้น**

```
① dims จริง (int64 per spec) ทะลุเข้า mock header   ✅ core/gguf_reader + gguf_box
② tensor chain ลง window 20736 (inference order)   ✅ test_gguf_window_chain 11/11
③ llama.cpp อ่านผ่าน graft — logits เท่ากันเป๊ะ      ✅ test_gguf_graft_llama 12/12
```

## 15. Ghost placement — วางลึกโดยไม่จอง 2ᵏ (วิญญาณออกจากร่าง)

### 15.1 ปัญหาที่เจอ — naive deep placement = overcommitment ข้ามไฟล์

วางไฟล์ลึก k สเกลแบบ naive = จอง B×2ᵏ ในสนาม (ไฟล์ 1GB วางลึก 4 สเกล = 4GB;
10 ไฟล์ = 40GB "แค่จะไปอ่าน 1GB") — **โหดจริง** และอันตรายไม่ใช่ตัวไฟล์ แต่เป็น
ไฟล์อื่น (global scale ขยับพร้อมกัน — ทุกอย่างถูกลาก)

### 15.2 วิธี ghost — ไม่เคยจอง 2ᵏ (กำจัด spike โดย construction)

```
naive วางลึก  = จอง B×2ᵏ (spike เกิดตั้งแต่ placement)
ghost         = ร่างจอง view หด B/2ᵏ + วิญญาณ (residual log ∝ events, data มีโครงสร้าง)
                base อ่าน = view หด 2ᵏ จาก depth tag (ฟรี, ไม่แตะ log)
                อ่านเต็ม  = replay residual ต่อ chunk (workspace bounded)
                ระหว่างบิน: base ถูกตัด (link ปิด) — วิญญาณเป็นหน้าเดียว
                re-attach = วางเสาเข็มใหม่ที่ scale เป้าหมาย + reroute link
```

### 15.3 Drag curve (overclock) — ความลึกถูกจำกัดโดย curve ไม่ใช่กฎ

```
benefit_k = B − B/2ᵏ   (base view ประหยัด)
cost_k    = residual + replay events
knee      = จุดที่ cost ≥ benefit — "วิญญาณถ่วงกลืนกำไร"
  — data มีโครงสร้าง: residual เล็ก → knee = 128 (ลึกได้ถึง ~90% ของ scale axis)
  — data สุ่ม: residual = detail เต็ม → knee = 1 (ลึกไม่คุ้มเลย)
```

**"ห้าม/พอประมาณ" ไม่ต้องตัดสินใจเด็ดขาด** — curve เป็นคนบอก: structured วางลึกได้
(ถ่วงน้อย), random วางลึกไม่ได้เลย (residual เองคือตัวถ่วง)

### 15.4 หลักฐาน — tests/test_tess_ghost.c (22/22, TIER1)

| T | พิสูจน์ | ผล |
|---|---|---|
| T1 | naive ×10 @k=4 = 184,320 > 20,736 → overcommit (กรณี 40GB จริง) | ✅ |
| T2 | ghost ×10 @k=4 = 1,040 slots ≤ window — spike ไม่เกิด | ✅ |
| T3 | ghost footprint < BASE ทุก k≥1 (ไม่จอง 2ᵏ) + ขอบเขตเป๊ะ (18 พอดี / 19 reject) | ✅ |
| T4 | base view หด 2ᵏ — k=3 → 8× (1152→144) ตรงกับตัวอย่าง user | ✅ |
| T5 | อ่านเต็ม = replay residual lossless; workspace 168 vs naive 9,216 slots | ✅ |
| T6 | drag curve — knee: structured=128, random=1 | ✅ |
| T7 | admission control — 4 souls in flight, 6 queued FIFO deterministic | ✅ |
| T8 | ระหว่างบิน base ถูก block (link ปิด); re-attach = เสาเข็มใหม่+reroute | ✅ |

### 15.5 หลักการที่ได้ (ต่อจาก §9)

1. **ไม่เคยจอง 2ᵏ** — ความลึกเป็น tag + log ไม่ใช่พื้นที่; spike 4GB/40GB เป็นไปไม่ได้โดย construction
2. **ความลึกถูกจำกัดโดย drag curve (วัดได้) ไม่ใช่กฎห้าม** — overclock analogy: ยิ่งลึกยิ่งถ่วง (thermal wall)
3. **ร่างถูกตัดระหว่างบินจริงจัง** — base ทำอะไรไม่ได้ (link ปิด); วิญญาณ = หน้าเดียว (addressable) — re-attach = เสาเข็มใหม่ + reroute (ไม่แตะของเก่า)
4. **ghost footprint = B/2ᵏ + log < naive ถึง 2²ᵏ เท่า** — กำไรทั้ง view และพื้นที่

### 15.6 Field base — ไม่เริ่ม base ที่ 1: benefit ทั้งสนามแต่แรก

คำถาม (user): ถ้าไม่เริ่ม base ที่ 1 ทั้งระบบ จะได้ benefit ของสเกลทั้งสนามแต่แรกไหม?

**ใช่ — และดีกว่า per-file ghost:** base เป็น global knob ตัวเดียว — วางทั้งสนามที่ base ลึก
ทุกไฟล์ได้ประโยชน์ตั้งแต่แรก ไม่ต้องตัดสินใจทีละไฟล์ (T9–T12, test_tess_ghost 27/27):

```
capacity(k) = WIN / (B/2ᵏ + residual(k))   — structured data
  k=0 → 18 files/window
  k=7 → 319 files/window (17.7×)  ← peak ของ capacity curve
```

| T | พิสูจน์ | ผล |
|---|---|---|
| T9 | field base ลึก → capacity ทั้งสนามคูณ 18→319 (17.7×) ตั้งแต่แรก | ✅ |
| T10 | benefit uniform — ตัวเดียว ใช้ทุกไฟล์ (ไม่มี per-file tag) | ✅ |
| T11 | random ไฟล์ในสนามลึก = neutral (B/2ᵏ+detail = B) — ไม่ขาดทุน capacity | ✅ |
| T12 | ghost ยังเป็น escape hatch — ไฟล์ขอ base ตื้นกว่าได้ | ✅ |

**ข้อสำคัญ:** structured ไฟล์ = กำไร 2ᵏ, random ไฟล์ = neutral (ไม่ขาดทุน) — ต้นทุนเดียวที่
ทุกไฟล์จ่ายร่วม = **replay เวลา** ตอนอ่านเต็ม (drag curve) — field base ตั้งได้ 2 แบบ:
capacity peak (k≈7) หรือ view knee (k=128) — เลือกตามลำดับความสำคัญ ไม่ใช่กฎห้าม

### 15.7 Scale floor — อย่าเริ่ม base ที่ scale ต่ำสุด (plateau edge)

จาก experiment เดิม (KIS↔Hyperbolic): scale 1.0 → 0.0001 recover สะอาดทุกอัน,
**plateau ที่ 0.00001** — fixed-point เปลี่ยนไม่ทัน; threshold แนะนำ 0.0001 (margin 1 step)

**กฎที่ได้:** base ของสนาม**ห้ามเริ่มที่ scale ต่ำสุด** — bottom ของ axis = dead zone
(plateau region, สงวนไว้ไม่วางข้อมูล) — field base ต้องอยู่เหนือ floor + margin:

```
[0 .. M−1]  = dead zone (plateau — ห้ามวาง, กัน precision break)
[M .. 143]  = usable — field base อยู่ตรงนี้
"อยู่สูง แล้วถอย" = contraction จาก base สูงลงมา = base-2 shift ล้วน (exact, ไม่มี decimal)
                   มี headroom คลีนๆ ก่อนถึง plateau — ต่างจากการเริ่มที่ล่างสุด
                   ที่ทุกก้าวลูบกำแพง precision
```

T13–T14 (test_tess_ghost 30/30): dead zone สงวน (base < margin ใช้ไม่ได้, ≥ margin ใช้ได้),
ถอยจาก base สูง → ทุกขั้น ÷2 exact จนถึงขอบ dead zone — **base ต้องไม่ใช่เลขต่ำสุด**
— margin = ค่าจาก measurement (0.0001 vs 0.00001 = 1 step) ไม่ใช่เลขมั่ว

### 15.8 "ย่อฟรี ขยายจ่าย" — หลักการ pricing ของทิศทางสเกล

**ย่อ (contraction) = ฟรี** — base-2 shift ล้วน, exact, deterministic — อ่าน view เล็กลง
ไม่จ่ายอะไร (T14: ทุกขั้น ÷2 exact ลงถึงขอบ dead zone)

**ขยาย (expansion) = จ่ายเสมอ** — ไม่มี operation ที่ทำให้ของใหญ่ขึ้นฟรี:
- naive: จอง 2ᵏ (overcommit — 40GB กรณี 10 ไฟล์)
- ghost: residual log + replay time

**ขยายมีข้อดีเดียว = push beyond limit** — ลงทุนเพื่อเพิ่ม headroom การย่อ
(วางลึก → ย่อกลับได้ไกลขึ้น) — ไม่เคยเป็นเป้าหมายในตัวเอง:

```
"ขยับอีกก้าวเดียวนี่ทวีคูณมากเลยนะ" — ที่สเกลสูง 1 ก้าว = ×2 ทั้งกำไรและต้นทุน
  — ระบบไฟล์เดียว: จ่ายไหว (aggregate เล็ก) แต่ไม่แนะนำ (ROI ต้องวัดก่อน)
  — ระบบหลายไฟล์: ขยาย = leverage ที่ต้อง justify ด้วย drag curve
```

**กฎออกแบบ placement:** default = วางฝั่งย่อ (base ≥ floor+margin); ขยาย = เฉพาะเมื่อ
benefit ที่วัดได้ (push beyond limit) — คล้าย leverage: ใช้เมื่อจำเป็น ไม่ใช่ default

### 15.9 Leverage gate — "ขยาย 1 ก้าวคุ้มไหม?" (test_tess_leverage 24/24)

ขยายมีข้อดีเดียว = push beyond limit (capacity) — ดังนั้นทุกขั้น k→k+1 ต้องผ่าน gate:

```
cost(step)    = สิ่งที่ทั้งสนามจ่าย:
                  naive exposure: materialization potential ×2 ทั้งสนาม
                    (k=6, 314 ไฟล์: 1 ก้าว = +23M slots — ghost ไม่จองแต่ยังเป็นหนี้ศักยภาพ)
                  ghost side: Δresidual+Δreplay ต่อไฟล์ = 16 slots (เล็ก เป็นเส้นตรง)
                    แต่สะสม ×N — ไฟล์เยอะ = วิญญาณถ่วงเยอะ
benefit(step) = capacity ที่ unlock = Δcap × fp(k+1)
                  ถ้า load ต่ำ (<90% cap) → benefit = 0 → ห้ามขยายเสมอ
ROI = benefit/cost — gate: ขยายเมื่อ (benefit > 0 && ROI ≥ GATE)

ROI curve (k=2..6):  8.49 → 4.02 → 1.74 → 0.64 → 0.06
knee: ROI < 1 ที่ k=5, < 2 ที่ k=4 — สเกลสูง = leverage แย่ลงทวีคูณ
```

**คำตอบ "ROI ต่ำแค่ไหนจึงควรห้าม" = < 1.0** (เกณฑ์นโยบาย GATE ปรับได้: GATE=2 → ห้ามตั้งแต่ k=4)

**กรณีไฟล์เดียว (user):** cost ไม่ ×N → ไปได้ลึกกว่าสนาม — budget 100 fit ที่ k=5
(ขั้น 3→4 ROI 4.0, 4→5 ROI 1.75 → อนุญาต; พอ fit แล้ว benefit=0 → หยุด) — "สบายพอทำได้"
แต่ budget 68 ขั้น 5→6 ROI 0.625 < 1 → **"ทำได้แต่ไม่แนะนำ"** (จ่าย residual > กำไร footprint)
และ k≥7 fp โตกลับ (65→68) — ขยายลึกไปไฟล์เดียวเองบวม (residual กลืน view)

**บทเรียนการออกแบบ:** capacity peak (k=7, 319 files) ≠ base ที่แนะนำ (k≈4-5)
— เลือก base ตาม ROI gate ไม่ใช่ตามเพดาน: 2 ขั้นสุดท้ายก่อน peak (k=5,6) ROI < 1 = "ทำได้แต่ไม่แนะนำ"
— gate เป็น pure function (deterministic, replay ได้) — สนามนิ่ง intact, ไม่มี state

### 15.10 Leverage gate × registry — placement ถูกคุมด้วย ROI, replay deterministic (test_tess_registry_gate 22/22)

ผูก gate เข้ากับ registry (fixed array — สนามนิ่ง) — วางไฟล์ข้ามเส้น 90% → gate ตัดสินใจ:

```
trigger: placement จะทำให้ load > 90% ของ window (18662/20736)
gate:    ROI(k) ≥ GATE (1.0) → ขยาย field base k+1 แล้ววางที่ base ใหม่
         ROI < GATE หรือ dcap ≤ 0 → ปฏิเสธ deterministic
registry: entry {file_id, addr(2B), base_k, n_slots, link} — append-only, ไม่เคยขยับ
          ปิด link = คืน load (เสาเข็มอยู่) / เปิดกลับ = addr เดิม, data เดิม
log:      decision trace {action, file_id, base_k, addr} — replay = apply เฉยๆ
```

**สถานะจาก test (100 ไฟล์ uniform, เริ่ม base=2 ตาม §15.7):**

```
files 1..61 @k=2 (load 18544 = 89.4%) → file 62 ข้ามเส้น → ROI(2)=8.49 → ขยาย k=3
file 63 → ROI(3)=4.02 → k=4;  file 64 → ROI(4)=1.74 → k=5
file 65+ → ROI(5)=0.64 < 1 → ปฏิเสธทั้งหมด
จบ: 64 วาง / 36 ปฏิเสธ / field_base=5 (knee) / load=18892 (91.1%) — ไม่เคยเกิน WIN
```

**Determinism พิสูจน์ครบ:**
- T2: รัน 2 ครั้ง → registry/load/base/decision trace เหมือนทุกไบต์
- T4: replay จาก log (ไม่มี input เดิม) → state เหมือนทุกไบต์ (n_slots คำนวณจาก base_k)
- T5: ปลอม base_k ใน log → state ต่าง (จับได้ — replay ไม่ใจดี)
- T6: log ขาด 1 event → state ต่าง (replay ไม่เติมของให้)
- T8b/T9f: replay รวม close/reopen → เหมือนทุกไบต์

**สนามนิ่ง intact (T7/T8):** close/reopen 10 ไฟล์ → addr ทุก entry ไม่เปลี่ยนเลย,
ลำดับ registry = ลำดับวาง (append-only, ไม่มี reorder/compaction); ปิด link คืนที่
→ ไฟล์ที่เคยถูกปฏิเสธวางได้ใหม่โดยไม่ต้องผ่าน gate (load < 90%); เปิดกลับครบ →
gate กลับมาคุมอีกครั้ง (ปฏิเสธที่ k=5 เหมือนเดิม — determinism ไม่เปลี่ยนตามประวัติ)

**บทเรียน:** field_base เป็น state ของสนาม (ขยับได้เฉพาะผ่าน gate ที่ ROI ≥ 1)
แต่ registry/addr ไม่เคยขยับ — "สนามนิ่ง ข้อมูลเคลื่อน" ยัง intact: gate ขยับได้
เฉพาะตัวเลือกการวางของไฟล์ใหม่, ไม่เคยแตะไฟล์ที่วางแล้ว

### 15.11 ไฟล์จริง — Qwen2.5-0.5B Q8_0 ผ่าน leverage gate × registry (test_gguf_real_gate 15/15)

tensor ขนาดจริง (291 ก้อน, n_elems จาก GGUF จริง — ไม่ใช่ uniform สมมติ):

```
model: 291 tensors, E = 630,167,424 elements (601 MB @1B/slot) → base 0 = 30,391 windows
gate sweep (cost/step = 4.38M slots drag):
  k=2 ROI 17.99 → k=3 8.99 → k=4 4.50 → k=5 2.25 → k=6 1.12 → k=7 0.56
  final base = 7  → storage 30,391 → 238 windows = 128× compression
```

**คำตอบ "ไฟล์จริง vs uniform ต่างยังไง":**

| @base 7 | storage | addressing spans | tax |
|---|---|---|---|
| จริง (291 ก้อน ขนาดจริง) | 238 windows | **465 windows** | **1.95×** |
| uniform (291 ก้อน เท่าๆ กัน) | 238 windows | 291 windows | 1.22× |

- compression/storage เท่ากัน (Σ E เท่ากัน, chain ต่อเนื่อง) — **แต่ registry tax ต่าง:**
  ของจริงมี tensor เล็กเยอะ (norm 896, attn 803k) → แต่ละก้อนตกเพดาน 1 window
  → ต้อง address 465 จุด ในขณะที่ uniform คาด 291 — **ของจริง "registry กว้าง" กว่า 1.6×**
- ที่ base ตื้น (k=2) tax ≈ 1 ทั้งคู่ — fragmentation เกิดเฉพาะตอนวางลึก (tensor เล็กลงจน
  ไม่เต็ม window) — นี่คือเพดานของ compression: ต่อให้ลึกแค่ไหน 1 tensor ≥ 1 window

**chain placement ที่ base 7 (สนามนิ่ง):** 291/291 วางครบ, 238 windows (== model-level ตรงกัน),
237 ครั้งที่ window ข้ามเส้น 90% — gate ไม่ re-expand เลยแม้ครั้งเดียว (decision อยู่ที่
model level แล้ว — placement เป็น verification ไม่ใช่ re-decision) — replay จาก log
เหมือนทุกไบต์, ปลอม log จับได้ (T11), close/reopen ผ่าน log เหมือนเดิม (T12)

**บทเรียน:** compression จริงของไฟล์ = E/2ᵏ (chain ต่อเนื่อง) — 128× ที่ knee;
แต่ registry addressing มีเพดาน "1 tensor ≥ 1 window" — tax 1.95× สำหรับของจริง
→ base ลึกเกินไป แพงที่ registry ไม่ใช่ที่ storage — leverage gate ต้องนับทั้งสอง

### 15.12 หลายโมเดลผ่าน gate — knee สากล 7, tax ตามสถาปัตยกรรม (test_gguf_multi_model 29/29)

รัน gate sweep (เดียวกับ §15.11) บน 4 โมเดลจริง: SmolLM2-360M, Qwen3-0.6B,
LFM2.5-2.6B + baseline Qwen2.5-0.5B (ROI sweep ≈ 18/9/4.5/2.25/1.12/0.56 ทุกตัว
— ต่างกันทศนิยมหลักสุดท้ายตาม E/N ratio):

| โมเดล | N | E (elements) | base0 | base | windows | comp× | tax จริง | tax uniform | small% |
|---|---|---|---|---|---|---|---|---|---|
| SmolLM2-360M | 290 | 361.8M | 17,449 | **7** | 137 | **127×** | **2.24×** | 2.12× | 100% |
| Qwen3-0.6B | 310 | 596.0M | 28,745 | **7** | 225 | 128× | 2.01× | 1.38× | 73% |
| LFM2.5-2.6B | 266 | 2,697.2M | 130,074 | **7** | 1,017 | 128× | 1.19× | 1.05× | 43% |
| Qwen2.5-0.5B | 291 | 630.2M | 30,391 | **7** | 238 | 128× | 1.95× | 1.22× | 75% |

**Knee สากล = 7 — ไม่ได้มาจากสถาปัตยกรรม:** ROI = benefit/cost และทั้งคู่ scale
ตาม E พร้อมกัน (benefit ∝ E/2ᵏ/WIN, cost = drag ∝ E/144) → อัตราส่วนคงที่ข้ามโมเดล
→ base 7 คือ knee ของ **cost model นี้** (GATE=1, drag 1 slot/event) ไม่ใช่ของตัวโมเดล
— โมเดลใดก็ตามผ่าน gate นี้จะหยุดที่ 7 (จนกว่า cost model จะเปลี่ยน)

**Tax แปรตามสถาปัตยกรรมจริง — 1.19× ถึง 2.24×:**
- **ใหญ่ = tax ต่ำ:** LFM 2.6B 1.19× (small% 43%) — tensor เฉลี่ย 10.1M elements
  ≫ WIN → แตกน้อย; ที่ base ตื้น (k=2) tax ทุกตัว ≈ 1 เหมือน §15.11
- **เล็ก = tax สูง:** SmolLM2 360M 2.24× — small% 100% (ทุกก้อน view หดแล้วไม่เต็ม
  window) + N(290) > storage(137) → **ทุกก้อนตกเพดาน "1 tensor ≥ 1 window"**
  — เพดาน tax ≥ N/storage = 2.12 อยู่แล้ว แม้ uniform
- **ผันผวน = registry กว้าง:** Qwen ทั้งสอง real ≫ uniform (1.46×/1.60×) — มี
  tensor เล็ก (norm) เยอะที่ uniform ไม่เห็น; SmolLM2 จริง ≈ uniform (1.06×) —
  ขนาดสม่ำเสมอ

**บทเรียน:** knee ไม่บอก tax — โมเดลเล็กที่ base เดียวกันจ่าย registry แพงกว่า
(2.24× vs 1.19×) → gate ฉบับนับ registry tax (แผนข้อ 1) จะเลือก base ต่างกัน
ต่อโมเดล: SmolLM2 ที่ base 7 จ่าย 2.24× — ถ้า tax ของ base 6 ต่ำพอจน ROI รวม
(จ่าย tax ทั้งสองฝั่ง) < 1, base จริงของ SmolLM2 อาจ < 7 — ขณะที่ LFM 2.6B
(base 7, tax 1.19×) อาจยังคุ้ม — **base จริง = ฟังก์ชันของสถาปัตยกรรม**

### 15.13 Field-built GGUF — body มาจาก KIS field ไม่ใช่ mmap (tools/gguf_graft_field.c 5/5)

พิสูจน์ขั้น ④: field **เก็บ byte จริง** แล้ว rebuild GGUF ออกจาก field (ไม่ใช่
pointer-to-source เหมือน graft เดิม) — llama.cpp generate จาก field ล้วนๆ:

```
BAKE    tensor bytes → field (window chain, inference order, align32)
        token_embd → blk.0..N → output_norm → output  (chain ≠ file order)
REBUILD header = KV verbatim + tensor infos ใหม่ใน chain order + offset = chain pos
GENERATE field-built GGUF 40 tokens == ไฟล์ต้นฉบับ (token stream bitwise)
```

- **lossless ครบ:** bake memcmp ทุก tensor (F1), header rebuilt ถูกต้อง (F2),
  body จาก field ≠ source body — chain reorder จริง ไม่ใช่ memcpy (F4),
  generation เหมือนทุก token (F5) — พิสูจน์บน Qwen2.5-0.5B และ SmolLM2-360M
- **หน่วยต้องตรง (จุดที่คนเข้าใจผิดบ่อย):** field byte เต็ม = **32,300 windows**
  (669.7 MB ÷ 20736 B) — ตัวเลข 238 windows ใน §15.11 คือ **view** (E/2⁷ =
  4.92M element-slots) — 128× คือ view compression: field ที่ base 7 ไม่ถือ
  ข้อมูลครบ (ต้อง residual = full detail สำหรับ Q8 → knee 1 ตาม §15.3)
- **ความหมาย:** GGUF เป็นแค่ container — tensor order/offset เปลี่ยนได้ตราบใด
  ชี้ byte ถูก — field layout ใดก็ตามที่กู้ byte ครบ = inference เหมือนเดิม
  → "coordinate = address" ใช้ได้จริงถึงขั้น serve inference

### 15.14 Page field — tokenizer KV ลง field, header 5.9MB → 20KB (tools/gguf_graft_page.c 8/8)

Tokenizer (151k strings) เป็นข้อมูล → วางใน field (window chain) เหมือน tensor data;
graft header เหลือแค่ pointer key + tensor infos — serve = ประกอบ GGUF เต็มจาก field:

```
BAKE    tokenizer.ggml.tokens/merges/token_type → field payloads (elements verbatim)
        addr = win×20736+slot (tokens win 32299, token_type 32424, merges 32453)
HEADER  graft_page.gguf = arch KV + kis.tokenizer.<x>.addr/count + tensor infos
        5,947,744 B → 20,352 B (292×) — tokenizer คิดเป็น 99.7% ของ KV (5,927,677 B)
SERVE   ประกอบเต็ม: KV เล็ก + arrays จาก field + tensor infos + body = field bytes
        → temp file → llama โหลด → generation 40 tokens == ต้นฉบับ bitwise
```

- **vocab 151,936 tokens มาจาก field จริง** (token text ตรง: `!` `"` `#` `$`) —
  header ไม่มี tokenizer แต่อย่างใด (P3b ยืนยัน) — อ่าน scale/view ไหนก็กู้ได้
- **llama b9733 ไม่มี vocab_file API** — vocab สร้างได้จาก KV ของ gguf context
  ที่โหลดเท่านั้น → serve ต้องประกอบ KV กลับจาก field (ใน memory/ไฟล์ชั่วคราว);
  artifact ที่ถาวรคือ header เล็ก — ตรง "index layer" (§11.5a)
- **finding (isolation test `tools/iso_user_path.c`):** `llama_model_init_from_user`
  ของ b9733 สร้าง optional tensors (output.bias + *.scale + *.input_scale = 337 ก้อน)
  ที่ callback ต้อง zero-fill — ผลคือ graph ต่างจาก file-load (แม้ meta จากไฟล์
  ต้นฉบับก็ต่าง) → ต้องใช้ file path เพื่อ guarantee bitwise
- **การตัดไม่ได้ฟรี:** ข้อมูลเต็ม (669.7MB) ยังต้อง materialize ตอน serve —
  ที่ตัดได้คือ durable artifact (header) และการถือ tokenizer เป็น data ใน field;
  ตรงกับ §11.5: "ค่าเต็มต้องอยู่ที่ไหนสักแห่ง"

### 15.15 Lazy serve — KV ใน memory, field windows mmap on demand (tools/gguf_lazy_serve.c 9/9)

Page-fault style loader: ไม่ materialize ไฟล์ 670MB ตอน serve — bake field.bin
ครั้งเดียวเป็น GGUF สมบูรณ์ (header + body ในไฟล์เดียว), serve = mmap + parse KV
ใน memory + callback จาก field mmap (pages เข้ามาเฉพาะที่ llama แตะ):

```
BAKE    field.bin = [header: KV verbatim + tensor infos chain order] + [tensor chain]
        → chain offset คำนวณก่อน build header (bug เดิม: เขียน offset=0 ทุกตัว)
SERVE   gguf_init_from_buffer(mmap field.bin, no_alloc=true) → KV ใน memory เท่านั้น
        llama_model_init_from_user → callback serve tensor bytes จาก field mmap
        → generation 40 tokens == ต้นฉบับ bitwise (L3) และ file path ก็ bitwise (F2)
```

- **crucial fix: `no_alloc=true`** — `gguf_init_from_buffer` กับ no_alloc=false
  copy ทั้ง 670MB เข้า memory (WS 2009 MB); พอ no_alloc=true → parse เฉพาะ KV
  (WS 731 MB) แล้ว callback หน้า-in เฉพาะ window ที่ llama แตะจริง
- **correction ของ §15.14:** user path bitwise ได้ถ้า fill ถูก: `output.bias` → 0,
  แต่ `*.scale` / `*.input_scale` → **1.0** (mul-by-one = no-op) — zero-fill
  mul-by-zero ฆ่า attention → graph เพี้ยน (เป็นเหตุผลที่ §15.14 ติด file path)
- **WS จริง (Qwen2.5-0.5B Q8, n_gpu=0, วัด per-path แยก):**

| path | WS peak | private | หมายเหตุ |
|---|---|---|---|
| lazy (user path) | 2065.6 MB | 1086.8 MB | serve เขียน 0 B, KV ใน memory |
| file path (field.bin mmap) | 1917.5 MB | 376.8 MB | OS แชร์หน้า mmap ตรง |
| delta | +148 MB (+8%) | +710 MB | llama ต้อง copy เข้า buffer ตัวเอง |

- **ต้องพูดตรงๆ:** lazy path ไม่ได้ลด peak WS เมื่อเทียบกับ file-mmap — user path
  ของ b9733 ต้อง copy weights เข้า buffer ของ llama เอง (private +710 MB) ขณะที่
  file-mmap ให้ OS แชร์หน้าได้; จุดที่ได้จริงคือ **ไม่ต้องเขียนไฟล์ 670MB ตอน serve**
  และ KV อยู่ใน memory (ไม่ต้อง re-read header) — field เป็น source of truth เดียว
- **เรื่องที่เจอระหว่างทาง:** header offset ทุกตัว = 0 (เขียน chain_off ก่อนคำนวณ)
  → llama reject header; แก้โดยคำนวณ chain offsets ล่วงหน้า → 9/9 PASS

### 15.16 Tokenizer KV อยู่ใน field windows — header เล็ก 292×, rebuild ทุก serve (11/11 ×3 โมเดล)

ต่อจาก §15.15: durable header ไม่ฝัง tokenizer KV ไว้ — มันเป็น data ใน field windows
(ท้าย field.bin) + index header เล็กถือแค่ pointer keys; serve rebuild header ใน
memory จาก field ทุกครั้ง (ไม่ต้องเก็บ 5.9MB tokenizer ไว้ใน artifact):

```
field.bin = [index header: KV sans tokenizer + kis.* pointer keys + tensor infos]
          + [tensor chain body]  +  [tokenizer payload windows (tokens/merges/token_type)]

BAKE    kis.kv.<x>.addr/len/count/arrtype = pointer ไป payload windows (UINT64 keys)
        n_kv = (src − 3 tokenizer) + 13 kis keys; body_off เก็บใน kis.layout.body_off
SERVE   kv_walk(fmap) → kis.* → memcpy elements จาก field windows → build header ใน memory
        gguf_init_from_buffer(header-only, no_alloc=true)  ← พิสูจน์แล้ว parse ได้
        (gguf data_offset = align32(header end) → ต้องส่ง size ที่ align แล้ว)
        llama_model_init_from_user → callback serve tensor bytes จาก field mmap
        → generation == ต้นฉบับ bitwise (L3) — ทั้ง Qwen 11/11, SmolLM2 11/11, LFM 11/11
```

- **durable header: 20,608 B (Qwen) / 19,488 B (SmolLM2) / 23,968 B (LFM) = 292×**
  — tokenizer (5.9MB/1.8MB/8.2MB ของ KV) ไม่อยู่ใน artifact; kis.* 13 keys ชี้ payloads
- **H1/H2:** durable header ไม่มี `tokenizer.ggml.*` keys + payload windows == source elements
  (memcmp) — token text 0..3 เทียบ source ตรง (L1c) — มาจาก field จริงไม่ใช่ source mmap
- **fill rules ครอบคลุม 3 สถาปัตยกรรม** (สำคัญ — สรุปจากการไล่ bug จริง):
  - `*.bias` → 0 · `*.scale`/`*.input_scale` → 1.0 (mul-by-one no-op)
  - `output.weight` ที่หาย (shared head กับ token_embd) → **dequant Q8_0 → F32**
    (user path ของ b9733 ขอเป็น F32; file-load ใช้ Q8_0 ตรง — พิสูจน์แล้ว F32 head
    ให้ผล bitwise เท่ากัน) — ใช้ `ggml_fp16_to_fp32` (exported) ไม่ใช่ fp16 แบบมือ
  - `rope_freqs.weight` → 1.0 (ggml คำนวณ theta เองเมื่อ tensor ไม่มีในไฟล์)
- **bug ที่เจอ:** fp16→f32 ที่เขียนเอง bias shift ผิด → logits พังทั้งก้อน (SmolLM2
  ออก EOS ซ้ำๆ) — เปลี่ยนเป็น ggml_fp16_to_fp32 ทันทีที่ bitwise; 3 bug ใน §15.15
  (offset=0, no_alloc, n_kv ไม่รวม kis keys, n_kv patch ตำแหน่งผิด reb+24 แทน reb+16)
- **ขอบเขตจริง:** lazy serve เขียนไฟล์ 0 B ตอน serve; index header เป็น artifact ถาวร
  — tokenizer เป็น data ใน field (ตรง §11.5); file path เปรียบเทียบกับต้นฉบับแทน
  (field.bin ไม่ใช่ GGUF สมบูรณ์อีกต่อไป — เป็น field จริง: index + body + windows)

### 15.17 Page-fault measurement — จำนวน windows ที่ llama แตะจริง (load vs generation)

> วัดไม่ใช่เดา: callback ของ serve นับ windows (20736 B) ที่ llama ขอจริง, `QueryWorkingSetEx`
> นับ pages ที่ resident (Valid bit), `PageFaultCount` นับ faults ต่อเฟส — ทั้งหมด 12/12 ×3 โมเดล

| เฟส | Qwen 0.5B (32,587 win) | SmolLM2 (18,636 win) | LFM 2.6B (138,638 win) |
|---|---|---|---|
| mmap cold — resident pages | **0** / 164,969 | **0** / 94,338 | **0** / 701,851 |
| serve rebuild (index+tokenizer) | 288 windows | 87 windows | 399 windows |
| **load** — windows แตะ | **32,301 (99.1%)** | **18,550 (99.5%)** | **138,240 (99.7%)** |
| load faults / time | 343k / 6.4s | 238k / 0.4s | 2.0M / 78s |
| **generation (40 tok)** — windows แตะ | **0** | **0** | **0** |
| generation faults | 50k (context, ไม่ใช่ field) | 58k | 301k |
| warm re-load (2nd, resident) | 0.70s (vs 6.4s cold) | 0.31s | 90s* |
| bitwise | ✅ 40/40 | ✅ 40/40 | ✅ 20/20 |

- **cold-start cost = load เฟสเดียว** — mmap เริ่มที่ 0 pages; serve rebuild แตะแค่
  index+tokenizer (~1-8 MB); เฟส load ดึง field เกือบทั้งก้อน (99%+ ของ windows) เข้า RAM
  เพราะ llama user-path ต้อง copy ทุก tensor ลง buffer ตัวเอง (`use_mmap=false`)
- **generation แตะ field 0 windows** — หลัง load ทุกอย่างอยู่ใน buffer ของ llama แล้ว;
  50-300k faults ของ generation มาจาก context/compute ไม่ใช่ field (residency ไม่เพิ่ม)
- **warm re-load** — pages ยัง resident → fault น้อยลงมาก, Qwen 6.4s→0.7s (9×) —
  แต่ *LFM แสดงจุดสำคัญ:* ถ้า RAM ไม่พอ (WS ~8GB) OS เริ่ม evict หน้า field ระหว่าง
  generation (483k→397k resident) → warm re-load กลับแพง — **warm-start ได้จริงต่อเมื่อ
  หน้า field ยังอยู่ใน RAM/OS cache**
- **การวัดนี้ยืนยันข้อสรุป §15.15-16:** field ไม่ได้ให้ laziness ระดับ window ระหว่าง
  generation — ได้ให้คือ (1) 0 B เขียนตอน serve, (2) OS หน้า-in ตามความต้องการของ
  load loop, (3) artifact ถาวรเล็ก — ถ้าอยากให้ generation แตะ field น้อยจริงต้อง
  mmap เป็น weights โดยตรง (ยังเป็น open point)

### 15.18 Zero-copy serve — generation แตะ field แทน load (12/12 ×3 โมเดล)

> เปลี่ยนจาก copy ลง private buffer → **callback repoint `t->data` เข้า field mmap ตรงๆ**
> (llama loader user path เรียก callback ตอน tensor ยังไม่มี data — ตั้ง pointer ได้เลย)
> ผล: load แทบไม่แตะ field, generation เป็นฝ่ายหน้า-in — §15.17 บอกว่า "ต้อง mmap เป็น
> weights โดยตรง (open point)" — ตอนนี้ทำแล้ว

**Qwen2.5-0.5B Q8 (32,587 windows, 644 MB):**

| เฟส | copy mode (§15.17) | **zero-copy (ใหม่)** |
|---|---|---|
| load faults / time | 343k / 6.4s | **15.8k / 0.36s** (22×) |
| field resident หลัง load | 164,969 pages (100%) | **1,454 pages (0.9%)** — แค่ index+tokenizer |
| load (physical) windows | 32,301 | **289** |
| generation faults | 50k (context) | 178k (**หน้า-in field 128k pages / 506 MB**) |
| generation (physical) windows | 0 | **25,639 faulted-in** |
| WS peak | 2065 MB | **1289 MB** (≈ file-mmap ref 1277!) |
| bitwise | ✅ | ✅ 40/40 |

- **กลไก:** `load_all_data` ของ user path เรียก callback ต่อ tensor โดย data ยังว่าง →
  แทนที่จะ `memcpy(t->data, ...)` (copy 670 MB เข้า private buffer ของ llama) →
  `t->data = field + body_off + fpos[i]` (pointer ตรงเข้า mmap) — OS หน้า-in ครั้งแรกที่
  ggml อ่านระหว่าง generation
- **token_embd เป็น lazy จริง:** Qwen generation อ่าน embedding แค่แถวที่ token ใช้
  → `token_embd.weight` 144.5 MB อ่านแค่ 0.1% (32/35,314 pages) — ~144 MB ไม่เคยเข้า RAM
  (SmolLM2/LFM ไม่ได้ประโยชน์จุดนี้: head tied กับ embedding → อ่านเต็ม)
- **WS เกือบเท่า file-mmap แล้ว** (1289 vs 1277 MB) — ค่า +148 MB ของ user-path หายไป
  เพราะไม่ copy แล้ว; private commit ยังสูง (llama จอง buffer ไว้) แต่ physical pages
  หน้า-in เฉพาะที่อ่านจริง
- **load เหลือแค่ 0.36s** — cold-start cost เปลี่ยนจาก "load ดึงทั้งก้อน" เป็น
  "generation ค่อยๆ ดึงเฉพาะที่ใช้" (Qwen 78.7% ของ windows; SmolLM2/LFM 100% เพราะ head tied)

### 15.19 Hyperbolic seeker roundtrip fix — axis = owner band (20736/20736)

> งานค้างจาก `docs/twin-seeker-insights.md` (2026-08-08): twin seeker (KIS+Hyper
> พร้อมกัน) เคย FAIL ครึ่งหนึ่ง — root cause ไม่ใช่ trig/float แต่เป็น **semantic
> ของ axis**: `kis_to_hyperbolic_axis(slot, axis)` ใช้ `slot % HYP_AXIS_SLOTS` เป็น
> มุม และ inverse บวก `axis*6912` กลับ → roundtrip จะ bijective ต่อเมื่อ
> `axis = slot / 6912` (owner band: axis 0=[0,6912) 1=[6912,13824) 2=[13824,20736))
> — tests เดิมส่ง `slot % 3` (phase, ไม่ใช่ owner) → inverse บวก offset ผิด ⅔ ของ slots

**ก่อนแก้:**
```
KIS roundtrip:   3456 PASS, 6912 FAIL   ← เคาน์เตอร์รวม (hyper ลาก KIS ลงด้วย)
Hyper roundtrip: 3456 PASS, 6912 FAIL
Hard: RESULT: HAS FAILURES ✗ (total mismatches: 27648)
scale=0.50 → record=10368, calc=3456, DIFFER   ← axis ผิด
```

**หลังแก้ (โค้ดเหมือนเดิม — แค่เรียกถูก semantic):**
```
KIS roundtrip:   10368 PASS, 0 FAIL
Hyper roundtrip: 10368 PASS, 0 FAIL
Twin (both):     10368 PASS, 0 FAIL
Hard: RESULT: ALL PASS ✓ (total mismatches: 0)
Match 20736/20736 · Identity 20736 · Wrap-around 6/6 · Per-axis 20736/20736
scale ทุกค่า MATCH (0.50: record=10368, calc=10368)
```

**การแก้:**
1. `core/hyperbolic_seek.h` — เพิ่ม `hyperbolic_axis_of(slot) = slot / HYP_AXIS_SLOTS`
   (helper เดียวที่ถูกต้อง) + ใช้ใน `teleport_seek` + selftest ขยายเป็น exhaustive
   20736 slots (เดิมสุ่ม 5 ค่า/axis)
2. `tests/twin_seeker_test.c` — `twin_seek` ใช้ `hyperbolic_axis_of()`; Step 3 แยก
   เคาน์เตอร์ KIS/Hyper (เดิม print ค่าเดียวกันสองบรรทัด)
3. `tests/twin_seeker_hard_test.c` — 4 จุดที่ส่ง `slot % 3` → `hyperbolic_axis_of()`
   (Test 1/3/5/6 + speed); Test 6 เปลี่ยนเป็น per-axis owner-band 20736/20736
4. Makefile — เข้า TIER1 → **45/45** (เดิม 43)

**บทเรียน:** Cayley transform (KIS↔Hyperbolic) bijective ครบตั้งแต่แรก — ที่พังคือ
caller ส่ง axis ผิด ⅔ ของครั้ง → วัด roundtrip ด้วย exhaustive 20736 ก่อนโทษ float

### 15.20 Subdivision rules — geometry เป็น rule ของการไหล ไม่ใช่ object (15/15)

> ต่อจากคำถาม: "พื้นสนามเป็น tessellation สามเหลี่ยมด้านเท่า สเกล base-2 ได้, subdivide
> 1/4, 1/2 ได้ไหม?" — **ได้ และ implement แล้ว** ตาม rescope: geometry = rule สำหรับ
> ตีกรอบการไหลของข้อมูล, ไม่สร้าง object จริง — integer ล้วน, ไม่มี trig/table/coordinate

**Mixed radix (หัวใจ):** `node = hi·81 + lo`
```
hi ∈ [0,256) = 4-ladder  (2⁸ = 4⁴)   ← binary floor, subdivide 4 → 1/4 ต่อระดับ
lo ∈ [0,81)  = 3-ladder  (3⁴)        ← Peano 3-adic,  subdivide 3 → 1/3 ต่อระดับ
20736 = 4⁴·3⁴ = 256×81               ← สอง ladder ปูกระเบื้อง space เดียวกัน
```

**API ใหม่ใน `core/tri_hex_tess.h`:**
- `th_subdivide(node, aperture, depth, child)` — 1 ระดับลึก: aperture 4 → 4 ลูก
  (ขยาย hi 2 bits), aperture 3 → 3 ลูก (ขยาย lo 1 trit)
- `th_parent(node, aperture, child_depth)` — 1 ระดับตื้น: lossless inverse
- `th_cell_anchor(node, aperture, depth)` — canonical address ของ cell
- `th_cell_count/size` — ladder 1→4→16→64→256 (size 20736→81) และ 1→3→9→27→81
- `th_quarter2` — 1/4 × 1/4 = 1/16 (สองระดับซ้อน)
- `th_hex7_tile` — aperture 7: 1 center + 6 ring (HEX_CELLS=7), 144 tiles × 7 = 1008

**พิสูจน์ (`tests/test_tess_subdivide.c` 15/15, อยู่ใน TIER1):**
- S1: node == hi·81 + lo ครบ 20736; 256×81 == 20736
- S3/S5: roundtrip lossless exhaustive — parent(subdivide) == cell anchor ทุกระดับ
  ทุก child (20736 × 4 levels × 4 children); anchor in → anchor out
- S2/S4: ladder counts/sizes ตรงเป๊ะ (4^n, 3^n)
- S7: 1/4 × 1/4 = 16 children distinct
- S8/S9: hex-7 — center = tile·7+6, 144×7 = 1008 data slots

**ตอบคำถามตรงๆ:** subdivide 1/4 (aperture 4) ใช้ได้จริงเป็น rule ของ address —
ทุก level ตัดพื้นที่ครึ่ง → 1/4; "1/2" ก็ได้ในรูป 4-subdivision ครึ่งระดับ หรือ base-2
scale (2ᵏ shift) ที่ timeline ใช้อยู่แล้ว (§15.7) — ทั้งหมดเป็น integer ล้วน
ตรงกับ rescope: **MAP not COMPRESS — coordinate = data**

### 15.21 Subdivision ↔ Scale-Timeline Wiring — th_subdivide ลง timeline แล้ว (test_tess_scale_wire 11/11)

ต่อจาก §15.20 — ตอนนี้ 4-subdivision depth d ผูกกับ scale position w แล้ว
ด้วย mixed-radix bridge เดียว (integer ล้วน, ตาม rescope):

```
node = hi·81 + lo,   hi = 16H + h,  lo = 9L + l
scale position w = 9H + L   ← outer digits (window-head axis)
local position  pos = 9h + l ← inner digits (intra-window axis)
(w, pos) ↔ node — bijection 144² = 20736
```

**Depth d → scale (wiring):** 4-ladder มี 4 base-4 digit — 2 หลักแรกอยู่ใน
SCALE axis (H), 2 หลักหลังอยู่ใน LOCAL axis (h):

```
d ≤ 2: cell = w-block 144/4^d (144, 36, 9) × pos เต็ม   ← scale axis หด
d = 3: w หยุดที่ 9 (อิ่มตัว); pos แยก 144 → 4×36          ← local axis หด
d = 4: w หยุดที่ 9;           pos แยก 36 → 4×9
ทุก cell = สี่เหลี่ยม w_ext × pos_ext พอดี = 20736/4^d
canonical depth scale: {0, 108, 135, 135, 135} = 144 − w_ext(d)
```

**a_w ↔ th_cell_anchor (พิสูจน์ exhaustive ทุกระดับ):**

| T | พิสูจน์ | ผล |
|---|---|---|
| W1 | (w, pos) ↔ node bijection — ทั้ง 20736, ทั้งสองทิศ | ✅ |
| W2 | ทุก node อยู่ใน (w-block, pos-block) ของ cell ระดับ depth ทุกชั้น; พื้นที่ = 20736/4^d | ✅ |
| W3 | w-block ระดับ 2 tile ระดับ 1 พอดี (4×9=36); w-axis อิ่มตัวที่ d=2 (d=3 แชร์ 4 ต่อ block, d=4 แชร์ 16) | ✅ |
| W4 | **ที่ทุก scale w, view a_w แยก depth-d cell ที่ active — pos-image partition pos** (ไม่มี cell ไหนชนกัน) | ✅ |
| W5 | slot owner == depth-d cell ของ node (a_w ตรงกับ th_cell_anchor slot-for-slot) + อ่านกลับ lossless | ✅ |
| W6 | canonical depth scale ∈ [0,144) = w-block สุดท้ายของ depth | ✅ |
| W7 | ทุก th_subdivide4 child อยู่ใน rectangle ของ parent (ทุกชั้น) | ✅ |

**ความหมาย (ตอบคำถาม "map depth d ไป w แล้ว a_w ตรงกับ anchor"):**
- 144-scale axis ดูดซับ subdivision 2 ระดับแรก (d=1,2 — cell = w-block 36/9),
  local axis ดูดซับ 2 ระดับหลัง (d=3,4 — pos-block 36/9) — **"1/2, 1/4" ที่
  ถามไว้ตอนนี้เป็นตำแหน่งบน w-axis จริง ไม่ใช่แค่เลขคูณ**
- ที่ทุกระดับ: a_w view เป็น bijection บน pos → pos-block ของ cell ที่ active
  ถูก map เป็น partition ของ 144 slots พอดี — **ไม่มี scale ไหนอ่าน cell ปนกัน**
- บทเรียนจาก bug ระหว่างทาง: (1) `owner` ต้อง uint16 — cell 255 ที่ depth 4
  ตัดใน uint8 กลายเป็น 0 ปลอม, (2) placement ต้องเป็น per-scale row
  (node ที่ scale == w เท่านั้น) — cell 1 ก้อนอยู่หลาย w-row วางลง store
  144 slots เดียวไม่ได้ (นั่นคือความหมายของ w-block)

### 15.22 Torus Wrapping — period (144,144) ปิด relation ของ 3 ตระกูลเส้น (test_tess_torus 16/16)

ต่อจาก §15.21 — คำถามค้างจาก TETRA_FIELD_STRUCTURE (ขั้น ⑤/⑧): "ห่อ 3 เส้นอนันต์
ด้วย period ไหน" — ตอบแล้ว: **(144, 144)**

```
3 ตระกูลเส้นบน (w, pos) grid:
  X: w = const      (step (0,1)  mod 144)
  Y: pos = const    (step (1,0)  mod 144)
  Z: w+pos = const  (step (1,−1) mod 144 — ทแยง)
relation: (w, pos, −(w+pos)) mod 144 — i+j+k ≡ 0 mod 144
```

**ทำไม (144,144) เท่านั้น:** relation ต้องใช้ modulus เดียว g กับพิกัดทั้งสาม
(symmetry — ไม่มีแกนไหนพิเศษ) → ต้อง g|m และ m|g (และ n) → m = n = g —
ใน 20736 = 144² = 288×72 = 48×432 มีแค่ (144,144) ที่ m = n —
(288,72)/(48,432) ชนกันใต้ modulus เดียว (พิสูจน์ T1) — 48 (band กว้าง = 144/3),
288 (CELL_288), 432 (3×144) ปรากฏที่อื่นในระบบ แต่ไม่มีตัวไหนปิดการห่อ

**Seam-free ทุกระดับ:** depth-d cell = w_ext×pos_ext, extents ∈ {144,36,9} —
ทุกตัวหาร 144 ลงตัว → ขอบ cell ตรงกับขอบ wrap (144→0) → seam ไม่เคยตัด cell
(พิสูจน์ exhaustive ครบ 4^d cell × ทุกระดับ — T3) — และ hex-6 ปิดสนิท:
20736 = 6×3456 (T4) — กำแพง depth-4 (81 = 6×13.5) ยังอยู่ตามขั้น ⑪

ความหมาย: การห่อ = identify ขอบตรงข้าม — ไม่มีขอบ ไม่มี origin —
ทุกจุดเป็นจุดตัดของ 3 เส้น (1 ต่อตระกูล) — homogeneous = enter anywhere
(พิสูจน์ translation-invariant — T5) — กับ cycle walk (⑦/⑩) ที่ห่อ lane เป็น
orbit ปิด: ตอนนี้ "lane" มีความหมายเต็มรูปบน torus แล้ว

### 15.23 Tetra-Axis Walk on the Torus — 12 orbits × 1728 บน (144,144) (test_tess_tetra_torus 9/9)

ต่อจาก §15.22 — นำ tetra walk (⑦) ไปวิ่งบน torus: orbit r = {r + 12k} —
พิสูจน์ว่าเดินครบสนามบน torus โดยไม่มีจุดเริ่มต้น และโครงสร้างตัดกันของ
ตระกูลเส้น × orbit เป็นอย่างไร

**รากของทุกอย่าง — residue ของ orbit ผ่าน bridge:**

```
r(n) = n mod 12 = (81·16H + 81h + 9L + l) mod 12
                = (9(h+L) + l) mod 12      ← 81·16 = 1296 ≡ 0 mod 12
→ r ขึ้นกับ (h, L, l) เท่านั้น — ไม่ขึ้นกับ H — ข้อเท็จจริงเดียวที่อธิบายทุกอย่าง
```

**โครงสร้างตัดกัน (พิสูจน์ exhaustive — ข้อมูลจริง):**

| ตระกูล | ทุกเส้นตัดกี่ orbit | ต่อ orbit ต่อเส้น | เหตุผล |
|---|---|---|---|
| X (w=const) | **12/12 (transversal)** | 12 nodes | fix (H,L), h∈[0,16)×l∈[0,9) → ครบ 12 residue |
| Z (w+pos=const) | **12/12 (transversal)** | 12 nodes | coupling H กับ (h,L,l) → ครบ |
| Y (pos=const) | **4 orbits (cluster)** | {48,32,32,32} | fix (h,l), L เปลี่ยน → 9L mod 12 period 4 |

ต่อ orbit: 12 Y-lines × 48 + 36 × 32 = 1728 (96 เส้นไม่แตะ) — walk 1 วง
แตะ X ครบ 144 เส้น, Z ครบ 144 เส้น, แต่ Y แค่ 48 เส้น

**ความหมาย:** ตระกูลทั้งสาม**ไม่สมมาตร**ภายใต้ tetra walk — X/Z เป็นเส้นตัด
(ทุก orbit ผ่านทุกเส้นพอดี 12 จุด) ส่วน Y เป็นเส้นเกาะ (4 orbits) — เกิดจาก
12 = 4×3 ที่ bridge (H-digit 4 / L-digit 3) ผูกกับ pos = 9h+l —
ตรงกับหลัก "โครงสร้างแปลกจากที่เคยเห็นทั่วไป": ไม่ใช่สามแกนสมมาตร
แต่เป็น X/Z-transversal + Y-cluster ที่เลขกำหนดชัดเจน

### 15.24 Sync Bridge — geo_jump decomposition ↔ KIS (w,pos) (test_tess_12x1728 9/9 + test_geo_sync_bridge 7/7)

ต่อจาก §15.23 — เชื่อมกับ geo_jump (FGLS_new/collection/geo_jump_module):
ทั้งสองระบบ address 20736 เดียวกัน แต่ decomposition ต่างกัน:

```
geo_jump:  node = face·1728 + tick·144 + local   (12 × 12 × 144 = 20736)
KIS:       node = hi·81 + lo → (w, pos)          (144 × 144 = 20736)
```

**สาม partition ของ "12 × 1728" — คนละชุด (พิสูจน์ exhaustive):**

| partition | นิยาม | ขนาด | สม่ำเสมอ? |
|---|---|---|---|
| A pentagon block (geo_jump) | node/1728 | 12 × 1728 | ✅ |
| B residue mod 12 (KIS tetra) | node%12 | 12 × 1728 | ✅ |
| C MOD coset (×5) | orbit ของ ×5 | **128 orbits, ขนาดต่างกัน** (ทุกตัวเป็นตัวหารของ 1728, max 4×1728 = units เท่านั้น) | ❌ |

C มีขนาด orbit ไม่เท่ากัน: units (gcd=1, 6912 ตัว) ได้ 4 orbits × 1728; non-units
เล็กกว่า (orbit(0) = {0}) — histogram: 1,2,4,6,8,12,...,1728 — "12 orbits" ของ
MOD walk เป็นแค่กรณี max ไม่ใช่ partition สม่ำเสมอ — สำหรับ sync ต้องเลือก
canonical ระหว่าง A กับ B

**Sync bridge (`core/geo_sync_bridge.h` — ใหม่):**

```
gsb_node_of(face,tick,local)      → node          (geo_jump side)
gsb_face_of/tick_of/local_of      → decomposition (geo_jump side)
gsw_scale_of_node/gsw_node_of_scale → (w,pos)     (KIS side, เดิม)
gsb_to_wpos / gsb_to_face_tick_local → BRIDGE ระหว่างสอง decomposition
```

พิสูจน์: roundtrip lossless ทั้งสองทิศครบ 20736 (T1/T2/T4), bijection
(face,tick,local)→(w,pos) ครอบทุก slot พอดี (T3), factorizations ตรง (T5),
partition A ≠ B (T6) — integer ล้วน ไม่มี table/float — bridge map พิกัด
ไม่ใช่ partition ดังนั้นฝั่งไหนแสดง partition อันไหนก็ได้

### 15.25 geo_jump walks บน KIS torus — ผ่าน sync bridge (test_tess_geo_jump_walks 15/15)

ต่อจาก §15.24 — นำ geo_jump walks (node space) map ผ่าน bridge ไป (w,pos)
แล้วจัดชั้น transversal/cluster เทียบกับตระกูลเส้น (X: w, Y: pos, Z: w+pos):

| walk (seed) | ขนาด orbit | X-lines | Y-lines | Z-lines | residues | ชั้น |
|---|---|---|---|---|---|---|
| MOD(5) (1) | **1728** (max order) | 144 ✅ | 96 | 144 ✅ | {1,5} | **X/Z-transversal + Y-cluster** |
| MOD(5) (0) | 1 | 1 | 1 | 1 | {0} | collapse (0·5≡0) |
| CAPO(1) (0) | 144 = 20736/gcd(144,20736) | 144 ✅ | 9 | 144 ✅ | {0} | **X/Z-transversal + Y-cluster** |
| CAPO(3) (0) | 48 = 20736/gcd(432,20736) | 48 | 3 | 48 | {0} | cluster ทั้ง 3 |
| INVERT (0) | 6 (3-floor × 2-mirror) | 4 | 6 | 6 | {0,11} | cluster ทั้ง 3 |

**Discovery — geo_jump family และ tetra family แชร์ signature เดียวกัน:**
MOD และ CAPO(1) เป็น X/Z-transversal + Y-cluster — **เหมือน tetra walk เป๊ะ**
(test_tess_tetra_torus) — ทั้งที่ node-space ต่างกันโดยสิ้นเชิง (คูณ 5 / บวก
144 / บวก 12) — signature บน (w,pos) ถูกกำหนดโดย bridge ไม่ใช่โดย walk —
หลักฐานว่าโครงสร้าง torus ครอบงำพฤติกรรมของทุก walk

**Residue facts (พิสูจน์แล้ว):**
- MOD: 5^k mod 12 ∈ {1,5} (period 2) → orbit(1) แตะแค่ 2 residue classes
- CAPO: 144·key ≡ 0 mod 12 → residue-pure (1 class) — tower shift ไม่ปน residue
- INVERT: mirror สลับ local → residues {0,11} สลับกัน
- MOD จาก non-unit seed (เช่น 0) collapse — walk เดียว cover ได้ max 1728
  (สอดคล้อง test_tess_12x1728: orbit ขนาดต่างกัน)

### 15.26 Full 20736-cycle — +37 และ +5 คือ full cycle (test_tess_full_cycle 12/12)

ต่อจาก §15.25 — คำถาม: มี walk ไหนในระบบที่เดินครบ 20736 เป็นวงเดียว?

**คำตอบ: additive walk `n → n+s mod 20736` เป็น full cycle ⇔ gcd(s, 20736) = 1**
(verified s = 1..1023) — กล่าวคือ s ต้องเป็นเลขคี่และไม่หารด้วย 3 ลงตัว (s ≡ 1 หรือ 5 mod 6)

| walk | ขนาด orbit | full? |
|---|---|---|
| tetra-12 (+12) | 12 × 1728 | ❌ |
| MOD-5 (×5 mult) | 1728 (max) | ❌ |
| CAPO-144 (+144) | 144 × 144 | ❌ |
| Z-walk (step (1,−1)) | 144-cycle | ❌ |
| a_w view | 144-permutation | ❌ |
| **+5 additive** | **20736** | ✅ |
| **+37 additive** | **20736** | ✅ |

**Discovery — stride-37 คือ full cycle ของสนามทั้งสนาม:**
- 37 เป็น stride ของ frame_seek เอง — explorer เคยสรุปว่า "37 ผิดโดเมน 20736"
  — แต่ข้อสรุปนั้นใช้กับ **MOD walk (คูณ)** เท่านั้น (order 576) —
  **additive (+37) เป็น full 20736-cycle พอดี** (gcd(37,20736)=1)
- 37 ≡ 1 mod 12 → วงเต็มเดิน **12 tetra orbits หมุนตามลำดับ** (node_k ≡ k mod 12)
- 37 coprime กับ 1440 ด้วย → full cycle บน frame-seek domain ด้วย (ทั้งสองโดเมน)
- บน torus: stride-37 = **Hamiltonian cycle** ของ grid 144×144 — แตะทุก cell
  พอดีครั้งเดียวแล้วกลับจุดเริ่ม (T7)
- geo_jump's stride 5: additive ก็เป็น full cycle เช่นกัน (gcd(5,20736)=1)

**ความหมาย:** ระบบมี "สายพานเต็มสนาม" อยู่แล้วโดยไม่ต้องเพิ่มอะไร — เดิน +37
จากจุดใดก็ได้ = เยี่ยมทุก address 20736 ครบพอดีครั้งเดียว กลับจุดเดิม —
ไม่มี origin (เข้าได้ทุกจุด) — เป็นคู่ของโครงสร้าง 12-orbit (tetra): 12-orbit
แบ่งสนามเป็น 12 sector, +37 เดินทะลุทุก sector หมุนตามลำดับ

### 15.27 สายพาน +37 — ฝัง stream ลงสนาม (test_tess_belt 10/10)

ต่อจาก §15.26 — ใช้ full cycle เป็น CONVEYOR BELT สำหรับ sequence data:

```
address[k] = (start + 37·k) mod 20736     ← เดิน +37, วางค่าที่ละ address
อ่านกลับ: เดิน +37 เหมือนเดิม → ได้ stream เดิมเป๊ะ (lossless, wrap ได้)
```

**พิสูจน์ (10/10):** ฝัง stream 16-bit ครบ 20736 ค่า + 5000 ค่า (สอง start)
อ่านกลับ bit-for-bit; อ่านจาก offset ไหนก็ได้ = stream หมุน (wrap, enter
anywhere — T2/T3); balance: ทุก X/Y/Z-line โดนแตะ 144 ครั้งพอดี (T4);
12 ก้าวติดกันครบ 12 tetra sectors หมุนตามลำดับ (T5); step pattern:
Δw ∈ {−5,−4,+4,+5}, Δpos ∈ {−8,+1,+10} — 55.6% ของก้าว advance pos +1 (T7)

**เปรียบเทียบ — สายพาน vs 12-orbit placement (ความจุรวมเท่ากัน 20736):**

| | 12-orbit (stride-12) | สายพาน (+37) |
|---|---|---|
| โครงสร้าง | 12 stream ขนาน × 1728 | 1 stream อนุกรม × 20736 |
| sector | อยู่ sector เดียว (residue r) | ข้ามครบ 12 sectors หมุนตามลำดับ |
| Y-lines | cluster 48 เส้น | ครบ 144 เส้น |
| X/Z-lines | 12 โนด/เส้น | 144 โนด/เส้น |
| องค์กร | parallel (12 คอลัมน์) | serial (สายพานเดียว) |

**ความหมาย:** สายพาน = อันดับการเข้าถึงแบบอนุกรมที่ครอบทั้งสนามอย่าง
สม่ำเสมอ — ต่างจาก orbit ที่แบ่งเป็นคอลัมน์ — เหมาะกับ stream ยาว (เช่น
tensor weights, token sequence) ที่ต้องการ serial order ครบ field —
กับ orbit เหมาะกับ data 12 กลุ่มที่ต้องการ sector isolation — สองแบบใช้
stride เดียวกันคนละตัว (+37 serial / +12 parallel) บนเลขเดียวกัน

### 15.28 tensor weights บนสายพาน +37 (test_tess_tensor_belt 10/10)

ต่อจาก §15.27 — ใช้สายพานกับ tensor weights จริง (Qwen-shaped: 288 tensors ×
72 values = 20736 — โมเดลเต็มสายพานพอดี):

```
belt:   address = (start + 37·k) mod 20736    k = global offset
        tensor t เริ่มที่ offset t·72 (ต่อเนื่องบนสายพานเดียว)
scatter: home(rank) = rank·37 (1 node/tensor — pointer/directory)
```

**พิสูจน์ (10/10):** 288 tensors × 72 values ครบ 20736 พอดี (T1a); rank 0..287
เป็น bijection (T1b); belt embed/read 20736 ค่าทั้งหมด bit-for-bit สอง start
(T2); หนึ่ง tensor แผ่ข้ามหลาย windows (>5) และ belt แตะครบ 144 windows (T3);
scatter homes distinct (T4a) ครอบ windows 0..73 (T4b); **identity: scatter
home(rank) == belt address ของ len-1 tensor start 0 — เดินสายพาน +37 ตัวเดียวกัน**
(T5); ต่างกันที่ granularity: scatter = directory of homes (288 nodes),
belt = value storage (20736 slots) — ทั้งคู่ deterministic + lossless (T6)

**ความหมาย:** placement เดิม (scatter window chain) กับ belt เป็น**การเดิน
+37 อันเดียวกัน** — ต่างแค่ payload ต่อ node (pointer vs value) — สายพาน
คือ window chain ในโหมด "value-granularity" — ระบบมีทั้งสองโหมดโดยไม่ต้อง
เพิ่มกลไกใหม่ — belt เหมาะกับ sequence weights ที่ต้องการ serial order,
scatter เหมาะกับ directory (pointer) — ใช้ stride เดียวกันทั้งคู่

### 15.29 output sequence → สายพาน +37 — real graft (gguf_graft_belt 12/12)

ต่อจาก §15.28 — ใช้สายพานเป็น serial order ของ **output จริงของโมเดล** ไม่ใช่
tensor weights: ฝัง token stream + logits ทุก step ที่ graft ตัวจริง (field-built
GGUF) generate ออกมา ลงสนามด้วยการเดิน +37 แล้วอ่านกลับเทียบ bitwise

```
token stream  (n × int32, 160 B สำหรับ 40 tokens) → 1 window, belt order
logits stream (n × vocab × f32, 40×151936×4 = 24.3 MB) → window chain,
belt order ภายในทุก window  → อ่านกลับด้วย walk เดียวกัน → bitwise
```

**พิสูจน์ (12/12, ผ่านรันจริง Qwen2.5-0.5B 40 tokens):**
- **F1-F4** field bake lossless + header rebuilt + graft จริง + body ≠ source
  (chain reorder — ไม่ใช่ memcpy)
- **T1** graft token stream == original (multi-token 40/40)
- **T2** graft per-step logits == original **bitwise ทุก step** (memcmp เต็ม
  เวกเตอร์ 151936 floats × 40 — ขยาย graft_llama T3c จาก 1 pass เป็นทุก step)
- **T3** token stream ฝัง belt → อ่านกลับ bitwise
- **T4** logits stream 24.3 MB ฝัง window chain (belt order ใน window) →
  อ่านกลับ bitwise
- **T5** belt เดินครบ 20736 slots พอดีครั้งเดียว (gcd(37,20736)=1) — ไม่มี
  collision = ไม่มี overwrite
- **T6a/b** 37⁻¹ mod 20736 = 16813 (extended Euclid พิสูจน์) + enter anywhere:
  อ่านจาก start อื่น = stream หมุน Δ = (s2−s1)·16813 ก้าว — deterministic

**ความหมาย:** สายพาน +37 ใช้ได้กับ output ของโมเดลจริง (ไม่ใช่แค่ synthetic)
— token stream หนึ่ง window พอดี, logits 24.3 MB ผ่าน window chain โดยที่
serial order ภายในทุก window คือการเดิน +37 เดียวกัน — อ่านกลับ lossless
100% — ระบบมี "serial order กลาง" สำหรับทั้ง input (weights §15.28) และ
output (logits/tokens) บนกลไกเดียวกัน

### 15.30 locality — linear cursor vs belt +37 บน logits จริง (graft-belt L1-L3)

ต่อจาก §15.29 — วัดเชิงตัวเลขว่า placement สองแบบบน logits stream จริง
(24.3 MB = 40 × 151936 × f32, 1173 windows) ต่างกันแค่ไหน และกระทบ
generation speed หรือไม่

**ผลวัดจริง (Qwen2.5-0.5B, 300 sweeps, 8-way accumulators, warm cache):**

| metric | linear cursor | belt +37 | ต่าง |
|---|---|---|---|
| best read-back | 17.6 ms/sweep | 33.7 ms/sweep | **1.92×** |
| throughput | 1318 MB/s | 688 MB/s | −48% |
| windows/s | 66,700 | 34,800 | −48% |
| 64B-lines/sweep | 379,840 | 380,052 | +212 (0.06%) |
| full windows | 324 lines/window | 324 lines/window | เท่ากัน |
| ragged tail (0.6% ของ stream) | 112 lines | 324 lines | belt อ่านทั้ง window |

**สาเหตุของ 1.92×:** stride-37 byte access ทำลาย hardware prefetch (linear
ถูก prefetch ตามลำดับ, belt กระโดด 37B) — cache-line coverage เท่ากันเป๊ะ
(window เต็ม 324/324) ต่างแค่**ลำดับ** — ไม่ใช่ modulo (ทดสอบแล้ว: incremental
walk ยังได้ 1.86-1.92×)

**ผลต่อ generation speed — ไม่มีนัยสำคัญ:**

```
per-step logits read-back: linear 0.44 ms | belt 0.84 ms
real llama_decode:         119 ms/step
→ placement impact: linear 0.37% | belt 0.71% ของ decode
```

**ความหมาย:** ราคา locality ของ belt = 0.7% ของ generation (sub-1%) —
belt แพงกว่า linear 1.92× เฉพาะใน microbenchmark ของการอ่าน field กลับ
ซึ่งเป็นส่วนเล็กน้อยมากของงานจริง (decode 119ms ต่อ step ครองทุกอย่าง) —
serial order ของ belt แลกกับ 0.7% นั้นคุ้มค่า (enter anywhere + ครบวง +
identity กับ scatter §15.28) — ถ้าวันไหนต้องการ linear speed จริง เก็บ
belt ไว้เป็น "index" แล้วอ่าน payload แบบ linear ผ่าน map จาก belt address
(§15.27-15.28 ใช้ stride เดียวกันอยู่แล้ว)

### 15.31 Ghost Lift — hyperbolic track wired into residual_space (test_ghost_lift 47/47)

ต่อจาก §5.2 (passive log) + §15.2 (ghost) — ปิด gap ที่ค้าง: log ของวิญญาณ
ตอนนี้เก็บลง residual_space จริงแล้ว (`core/geo_ghost_lift.h` + `core/residual_space.h`)

**Mapping rule (address = coordinate, ไม่มี lookup):**

```
bond = BIRTH IDENTITY  (block_id, from_scale) → seed → piece → bond_key = bond_L XOR bond_R
log  = ROUTE           {block_id, from→to} = 5 B/event
```

- `from_scale` เป็นส่วนหนึ่งของที่อยู่ → **เสาเข็มห้ามขยับ**: อ่านด้วย from_scale อื่น
  → bond_key เปลี่ยน → thaw/verify ล้มอัตโนมัติ (T3)
- `to_scale` (route) ไม่ใช่ส่วนของ bond → ข้อมูล freeze ครั้งเดียว reach ได้หลาย route
  (T5: rs.count == 1, log.count == 2) — "วางครั้งเดียว ไม่เคยเขียนซ้ำ"
- fold axis มาจาก `to_scale % 7 + 1` (I/O/T/S/Z/L/J) = route flavor — ไม่แตะ bond_key

**Lifecycle end-to-end:**
- **lift** = freeze ข้อมูลลง residual_space + append route → 5 B ต่อ event
- **read** = ต้องมี live route + bond ตรง → thaw — สองชั้น truth (ผิดจาก → bond แตก;
  ผิด to → route not found) (T4)
- **re-attach** = เสาเข็มใหม่ที่ scale เป้าหมาย → `ghost_expire` = rs_expire_by_origin
  (origin_key = geo_key ของเสาเข็มเก่า) → ข้อมูลตาย, route ถูก flag EXPIRED เก็บเป็น
  audit trail (เหมือน tombstone) (T7/T8)
- **telescope**: from 3 → to 140 = 1 entry (ไม่ใช่ 137 ก้าว) — delta ∝ events (T9)

**การตัดสินใจ:** route ซ้ำถูกปฏิเสธ (ไม่ update-in-place — §15.5.3) · log เป็น
authority: freeze ตรงๆ โดยไม่มี route = เข้าไม่ถึง (T12)

ขั้นต่อไป: กำหนด envelope (§11.6 — ยังไม่ตัดสินใจ) ให้ lift เกิดขึ้นเองเมื่อ
requested scale เกิน envelope ของก้อน — ตอนนี้ lift เป็น API เรียกตรง

### 15.32 Block Envelope (§11.6 — decided) + Auto Lift (test_ghost_envelope 39/39)

ปิด §11.6: `MAX_EXPANSION_DEPTH = envelope_depth(gate)` — จาก ROI curve
ตัวเดียวกับ test_tess_leverage (`core/geo_ghost_envelope.h`):

```
fp(k)       = view(k) + residual(k)      view = 1152>>k, residual = 8k
roi_step(k) = (fp(k) − fp(k+1)) / 16     marginal ROI ของ 1 ขั้น (ต่อ block, ไม่ ×N)
envelope_depth(gate) = max{ k : roi_step(k−1) ≥ gate }   (monotonic ↓)
```

| gate | envelope_depth | หมายเหตุ |
|---|---|---|
| **1.0 (default)** | **5** | "k 4–5 เหมาะสมที่สุด" — cliff ที่ depth 6 |
| 2.0 (conservative) | 4 | GATE=2 → ห้าม depth 5 (ขยับ cliff ลง 1) |
| 0.5 (aggressive) | 6 | |
| ≤ 0.001 | 7 | hard ceiling — fp(8) > fp(7) → ลึกไปบวมเอง |

**ย่อฟรี ขยายจ่าย:** depth = `to > from ? to−from : 0` — contraction ไม่ lift
แม้ไกล (T9); expansion เกิน envelope → lift (T7)

**Auto-lift (`ghost_lift_auto`):** ขอ depth ≤ envelope → `GHOST_AUTO_PLACE`
(วางในสนามปกติ, ไม่ freeze); ขอ depth > envelope → `GHOST_AUTO_LIFT`
(freeze ลง residual_space + route — §15.31) — knob = gate ขยับ cliff ระหว่าง
depth 5↔6 (T8) — deterministic, pure, replay ได้

**ความหมายต่อ §11.6:** capacity accounting (Σ envelope ≤ 20736) ยังเป็นชั้น
ถัดไป — ตอนนี้ envelope เป็น per-block bound; ก้อนที่ขอเกินไม่ต้องนับ capacity
เลย (ถูกยกเป็น ghost ไปก่อน) — overcommitment หายตั้งแต่ต้นทาง

### 15.33 Field Capacity Accounting + Gate Tuning on Real GGUF (test_cap_account 23/23 + test_cap_tune_real 6/6)

ปิดครึ่งหลังของ §11.6: capacity = Σ envelope ≤ 20736, เกิน = reject
deterministic (ไม่ silent) — `core/geo_cap_account.h`

**Accounting:**
- envelope size ของ block ที่ depth k = fp(k) (ghost footprint — view หด
  B/2ᵏ + residual 8k) — capacity(k) = 20736/fp(k) ตรงกับ test_tess_leverage
  เป๊ะ (199 blocks @ depth 4 พอดี, ตัวที่ 200 → REJECT)
- verdict: **CAP_LIFT** (depth เกิน envelope → deflect ไป ghost store,
  ใช้ capacity 0 — overcommitment หายตั้งแต่ต้นทาง) · **CAP_REJECT** (ชน
  20736 — นับไว้ ไม่ silent) · **CAP_ADMIT** (used += fp(k))
- pure + deterministic — sequence เดิม replay → verdict เดิม (T4/T7)

**Gate tuning บน GGUF จริง 4 โมเดล (SmolLM2-360M, Qwen3-0.6B,
LFM2.5-2.6B, Qwen2.5-0.5B — home = (rank·37)%20736, depth = w = home%144):**

| gate | k_max | lifts% |
|---|---|---|
| 0.5 | 6 | 94.8 |
| **1.0 (default)** | **5** | **95.5** |
| 1.5 | 5 | 95.5 |
| 2.0 | 4 | 96.2 |

- **Plateau ชัด: Δ 0.5→2.0 = 1.4 pp** — lift rate แทบไม่ขยับตาม knob เพราะ
  w-distribution ของสูตร placement เป็น uniform (ทุก 144 rank มี w ≤ 5 อยู่
  6 rank) — rate เป็นคุณสมบัติของสูตร ไม่ใช่โมเดล
- knob เลือกว่า **tensor ตัวไหน**อยู่ใน field (rank ต่ำ + w เล็ก) ไม่ใช่กี่ตัว
- field footprint @gate1.0 = 24,279 windows vs base 206,659 (ไม่ lift) —
  **ตัด 8.5×**; ghost 3.4 GB (residual_space — นอก field)
- **ผลตัดสิน: default gate = 1.0** — plateau ทำให้ 0.5/1.5/2.0 เทียบเท่า
  กันในแง่ rate → ใช้ค่า ROI knee (envelope_depth 5 = "k 4-5 เหมาะสมที่สุด")
  ตรงกับหลักการ ไม่ต้องเข้ม/หย่อนไปโดยไม่มีเหตุผล

**หมายเหตุ:** rank-0 tensor (เช่น token_embd ของ SmolLM ~49M elems) อยู่ที่
w=0 → อยู่ใน field เสมอด้วยราคาเต็ม s — field windows ส่วนใหญ่มาจาก tensor
ใหญ่ที่ rank ต่ำ — จุดที่ capacity tuning ต่อยอดได้ถ้าต้องการ (เช่น วาง
tensor ใหญ่ที่ rank สูงให้ lift)

**ต่อ §15.33 — Targeted rank assignment (field 24279 → 4 windows, 6070×):**

ค้นพบจากข้อมูล: field windows ส่วนใหญ่มาจาก tensor ใหญ่ที่ rank ต่ำ —
SmolLM `token_embd.weight` 47M elems อยู่ rank 0 (w=0) → อยู่ใน field ราคาเต็ม
(2409/17449 windows); LFM `token_embd` 262M → 14852; Qwen2.5 `output.weight`
136M → 6663

**Optimization (test_cap_tune_real T7-T9):** field ranks (w ≤ k_max) กระจาย
อยู่ที่ rank {0,4,39,74,109,113,...} — **เล็ง tensor เล็กสุด 13 ตัวไปที่ field
ranks เหล่านั้นตรงๆ** (เล็กสุดคู่กับ w น้อยสุด), ตัวใหญ่ไป rank สูง → lift:

| model | field windows (file-order) | targeted | × |
|---|---|---|---|
| SmolLM2-360M | 2,409 | **1** | 2,409× |
| Qwen3-0.6B | 355 | **1** | 355× |
| LFM2.5-2.6B | 14,852 | **1** | 14,852× |
| Qwen2.5-0.5B | 6,663 | **1** | 6,663× |
| **Σ** | **24,279** | **4** | **6,070×** |

**หมายเหตุ:** (1) การสลับ rank ยังคงเป็น permutation → bijection ของ address
ครบ (ถูกต้องเหมือนเดิม) แต่เปลี่ยนลำดับ chain — locality ของการเดิน +37 ต่าง
ไป ต้องวัดแยกถ้าจะ adopt เป็นนโยบายจริง (เทียบ §15.30: belt แพงกว่า linear
1.92× แต่ impact real decode 0.7%) (2) ghost ข้อมูลรวมไม่เปลี่ยน (Σ s เท่าเดิม)
— แค่สลับว่าใครอยู่ใน field: จาก "tensor ใหญ่ราคาเต็ม" → "tensor เล็ก 13 ตัว
ราคาเต็ม" (3) นโยบายนี้ = 1 รอบของ sort (N ≤ 320) ตอน placement — deterministic

**ต่อ §15.33 — safetensors จริง (test_cap_tune_safetensors 6/6):**

Pipeline เดียวกันรันบน .safetensors จริง (parse แค่ header — ไม่แตะ data):

| file | tensors | E | field (file-order) | targeted | × |
|---|---|---|---|---|---|
| LFM2.5-VL-450M (`embed_tokens` 67M @ rank0) | 349 | 448M | 3,644 | **1** | 3,644× |
| smolVLM-256M (`lm_head` 28M @ rank0) | 471 | 256M | 1,501 | **1** | 1,501× |
| zimage-ae (SD autoencoder, conv 512 @ rank0) | 244 | 83M | 15 | **1** | 15× |
| **Σ** | | | **5,160** | **3** | **1,720×** |

lift rate 95.1/95.8/96.3% — plateau เดียวกับ GGUF (w-distribution ของสูตร
เป็น uniform) → **ผลสรุปเหมือนกันทุก format: targeted ranks → 1 window/
ไฟล์; default gate 1.0 ใช้ได้ข้าม format** — ระบบไม่ผูกกับ GGUF

**ต่อ §15.33 — ไฟล์ทั่วไป: pdf / mp4 / zip / folder (test_cap_tune_fs 11/11):**

Pipeline เดียวกันบน F:/notebookLM จริง (1,035 ไฟล์, 7.7 GB — pdf 14,
mp4 11, wav 71, json 407) + zip จริง (GeoGebra portable 133 MB):

| case | blocks | base windows | field (file-order) | targeted |
|---|---|---|---|---|
| folder F:/notebookLM | 1,035 ไฟล์ | 393,203 | 3,578 | **1** |
| zip (central dir — 1,474 entries) | 1,474 | 16,378 | 27 | **0** |
| folder as ONE block | 1 | 393,203 | — | chain (ไม่ reject) |

- lifts% 95.7-95.8% — band เดียวกับ tensor (สูตร placement เป็น uniform —
  **format-agnostic**: ไม่ว่าไฟล์อะไร ขนาดอย่างเดียวที่ระบบดู)
- zip = container: central directory ระบุ entries (อ่านแค่ tail, ไม่
  decompress) → entries เป็น blocks เดียวกับไฟล์ตรง
- ไฟล์ใหญ่กว่า window (20736) → chain ข้าม windows — ไม่ใช่ reject
- density note: 1 byte = 1 slot (Q8-like); f32 = 4B/slot — เปลี่ยน ratio
  ของ windows ไม่ใช่ logic

**ต่อ §15.33 — Proof ไฟล์จริงผ่าน chain ทั้งเส้น (test_cap_chain_roundtrip 13/13):**

PDF จริง `Geometric_Logic_Architecture.pdf` (19.8 MB) จาก F:/notebookLM —
ผ่าน chain ครบ: chunk 16 KB × 1,209 → ทุก chunk ผ่าน `cap_admit` →
`CAP_LIFT` → `ghost_lift_auto` (freeze + route) / `CAP_ADMIT` → pointer-home:

- 1,158 chunks ยกเป็น ghost (residual_space), 51 chunks อยู่ใน field
  (pointer-home — data อยู่ต้นทาง, zero-copy)
- field usage = 20,528/20,736 slots — พอดี (ไม่ reject)
- **reconstruct ไฟล์ทั้ง 19.8 MB = ต้นฉบับ byte-for-byte** (lossless
  end-to-end — ทั้งส่วน ghost-read และ pointer-home)
- integrity: wrong route / wrong from (เสาเข็มห้ามขยับ) / wrong block → NULL
- deterministic: account ใหม่ → verdict เดิม
- ระหว่างทาง: `GHOST_LOG_MAX` 256 → 4096 (สมมาตรกับ RS_DEFAULT_CAPACITY —
  log ต้องจุ routes ของไฟล์จริงได้)

**ต่อ §15.33 — Chain at scale: mp4 57 MB + eviction (test_cap_chain_big 10/10):**

mp4 ใหญ่สุดใน notebookLM (`POGLS_ The Code That Lives.mp4`, 57 MB,
3,679 chunks × 16 KB) — 3 เฟส:

- **A. forced eviction** (capacity 1024 < 3,679): LRU kick in —
  `rs.count` คงที่ 1024, evictions = 2,655 (= chunks − capacity),
  chunk 0 (เก่าสุด) thaw NULL, chunk ใหม่สุด/ล่าสุดรอด — **cache ทำงาน
  ถูกต้อง, ไม่ silent growth**
- **B. bounded-window streaming** (§15.2: workspace bounded): 4 windows ×
  1024 chunks (16 MB) — place → verify → evict ทั้ง window → next —
  **reconstruct ทั้งไฟล์ byte-for-byte ด้วย memory จำกัด 16 MB ต่อครั้ง**
- **C. whole-resident** (capacity 4096 ≥ 3,679): 3,525 lifted + 154
  pointer-home, 0 evictions — **byte-for-byte**

**ความหมาย:** residual_space = LRU cache (บังคับด้วย capacity) — ไฟล์ใหญ่
กว่า cache → ต้อง streaming เป็น windows (พิสูจน์แล้ว lossless) หรือขยาย
capacity ให้จุทั้งไฟล์ — ตัวเลือกทั้งคู่ใช้ได้จริง; determinism ครบ (account
ใหม่ → verdict เดิม)

**ต่อ §15.33 — สแกนทั้ง folder 7.7 GB (tools/cap_chain_scan.c, `make cap_scan`):**

chain ครบทุกไฟล์ใน F:/notebookLM (1,035 ไฟล์, 7,775 MB, 498,355 chunks ×
16 KB, 1m33s):

| metric | ค่า |
|---|---|
| **checksum** | **1,035/1,035 byte-for-byte (0 fail, 0 skip)** |
| base windows (naive chain) | 393,954 |
| stream windows (16 MB bounded) | 1,447 teardowns |
| field slots (Σ envelope admitted) | 3,439,088 (~166 windows) |
| lifted → residual_space | 476,763 chunks |
| eviction pressure | peak rs.count 982/1024 (96%) — forced 0 |

**ข้อสังเกต:** field slots = per-file accounting (rank 0 ของทุกไฟล์ = w=0 →
fp(0)=1152 — 1,035 × 1152 จากไฟล์เล็ก 1-chunk) — เป็น worst case; ถ้า chain
ทั้ง folder เป็น rank ลำดับเดียว + targeted assignment (§15.33) field จะ
เหลือไม่กี่ windows — ตัวเลขที่รายงานคือ per-file bound ที่อนุรักษ์นิยม
eviction: 0 forced (window ≤ capacity โดย construction) — peak 96% แสดงว่า
bounded window ใกล้เต็มแต่ไม่เคยล้น

**ต่อ §15.33 — Adaptive scheme chooser (test_cap_scheme 12/12 + tools/cap_scheme_choose.c):**

ตอบคำถาม "ไฟล์เริ่มเยอะไม่คุ้ม → สลับไปอีกแบบได้ไหม" — ได้: เลือกได้แบบ
deterministic (`core/geo_placement_choose.h`)

- **PER_FILE**: ทุกไฟล์เริ่ม rank 0 → chunk แรก w=0 ราคาเต็ม (ค่าแรกเข้า
  × จำนวนไฟล์) — locality ดี (ไฟล์ติดกัน)
- **GLOBAL**: chain rank ลำดับเดียว + targeted assignment (§15.33) — field
  ranks ได้ chunk เล็กสุด — cost ต่ำสุดเสมอ (global ≤ per-file ทุกกรณี)
- **pc_choose(per_file, global, margin)**: GLOBAL ต่อเมื่อ per_file > global ×
  (1 + margin) — margin 50 default (สลับเมื่อประหยัด ≥ ⅓)

| case | per-file | global | ratio | เลือก |
|---|---|---|---|---|
| 1 ไฟล์ใหญ่ 1000 ch | 225,792 | 225,792 | 1.0× | **PER_FILE** (locality) |
| 1,000 ไฟล์เล็ก 1 ch | 16,384,000 | 225,792 | **72.6×** | **GLOBAL** |
| 5 big + 100 tiny | 12.86M | 11.23M | 1.15× | PER_FILE (คุ้มไม่พอ) |
| **folder จริง 7.7 GB** | 116.3M | 100.0M | 1.2× | **PER_FILE** @margin50 |

**folder จริง:** ค่าแรกเข้า 1,035×16,384 = 17M จริง แต่เล็กเมื่อเทียบกับ
field ของไฟล์ใหญ่ → ประหยัดแค่ 16% (786 windows) → margin 50 ตัดสินใจ
**PER_FILE** (เก็บ locality); margin 0 → GLOBAL — ตัวเลือกปรับตามเนื้อหา

**หมายเหตุสอง model:** scan ก่อนหน้า (field 3.44M) ใช้ fp block model
(cap_admit); ตัวเลือก scheme ใช้ size model (view_of ขนาดจริง) — ต่างคำถาม
กัน: accounting (limit) vs footprint จริง (เปรียบเทียบ scheme) — ตัวเลือก
ต้องใช้ size model เพราะ fp ไม่แยก scheme (จำนวน block เท่ากันทั้ง 2 แบบ)

**§15.34 — Persistence: serialize residual_space by bond_key + reload (test_rs_persist 36/36):**

ปิด open item "persist residual_space ลงดิสก์" — ตอนนี้ restart ได้ทั้งระบบ
(space + ghost log) พิสูจน์ lossless บน mp4 จริง 57 MB (3,525 lifted entries)

**Format (packed, little-endian, host x86):**
```
residual_space (rs_serialize / rs_load):
  [0..7]   magic "RSDWGLSP"   [8..9] version=1   [10..11] reserved
  [12..15] count u32
  [16..]   records: ResidualEntry header (36B, verbatim) + payload
ghost log (ghost_log_serialize / ghost_log_load):
  [0..3]   magic "GHST"       [4..5] version=1   [6..7] reserved
  [8..11]  count u32
  [12..]   GhostLogEntry (5B) — live + EXPIRED ทั้งหมด = audit trail
```

**ตัดสินใจที่ฝังใน code:**
- **Persist เฉพาะ VALID entries** — tombstone เป็น recycle bin ใน memory
  (ข้อมูลตายแล้ว); audit trail ที่ durable อยู่ที่ route log (GHOST_FLAG_EXPIRED)
  ไม่ใช่ที่นี่
- **Reload = re-insert by bond_key** — same bond → same address, ไม่มี lookup
  table; header คงเดิม (origin_key/geo_key/timestamp/flags) → rs_verify ผ่าน
  หลัง reload ทุก entry, LRU order (timestamp) รอด restart, next_timestamp
  ต่อจาก max
- **rs_load strict:** ต้องเป็น space ใหม่ (count==0), reject bad magic /
  version / truncation / duplicate bond_key / capacity < count — ไฟล์เสีย
  ไม่เข้าเงื่อนงำ

**พิสูจน์ (36/36):**
- unit: empty/deterministic/tombstone dropped/flags คงเดิม/corrupt reject/
  disk file (fwrite → fread → reload) — ครบ
- **mp4 จริง 57 MB: place (3,525 lifted) → serialize (56,514 KB) → rs_free +
  space ใหม่ + log ใหม่ (จำลอง restart) → reload → ghost_read ทุก chunk →
  reconstruct byte-for-byte** · rs_verify ผ่านทุก lifted chunk · wrong
  from_scale (bond แตก) / wrong to_scale (route ไม่มี) → NULL · freeze ใหม่
  หลัง reload ทำงาน (next_timestamp ต่อ, LRU ยังเรียงถูก)

**Bug 2 จุดที่เจอระหว่าง implement (offset หลุด 4B ทั้งคู่):**
1. `rs_serialize` — เขียน version/reserved แล้วไม่ advance pointer → count
   ไปทับที่ offset 8, record เริ่มที่ 12 (load อ่าน count จาก 12 = garbage)
2. `ghost_log_serialize` — เดิมแบบเดียวกัน → reload log ล้มเสมอ (T12/T14/T15
   ล้มเป็นลูกโซ่) — แก้ advance `p += 4` หลัง reserved

**โครงสร้างตอนนี้:**
```
place → [residual_space] → rs_serialize ─┐
      → [ghost_log]      → ghost_log_serialize ─┼→ restart → reload → ghost_read
                                            disk file ("RSDWGLSP" header) ──┘
```
`make test`: TIER1 74/74 + TIER2 4/4 ✅ (เพิ่ม test_rs_persist)

**§15.35 — Silk-screen feasibility: ตอบคำถาม "36 chunk : 1 map" ด้วยข้อมูลจริง (tools/silk_screen_scan.c, `make silk_scan`):**

แนวคิด silk-screen (แยก digit sets → map ลง cube 6 หน้า → duplicate + rotated
offset → 36 chunk : 1 map) ถูกวัดด้วยของจริง 4 โมเดล Q8_0 — **ผล: ล้มที่ block
level แต่เจอ dedup จริงที่ tensor level**

**การวัด (canonicalize Q8_0/Q4_0 blocks 32 ค่า ภายใต้ transform group):**
```
3 modes: identity (dedup ล้วน) / rot (minimal cyclic rotation — Booth) /
         rot+rev (dihedral orbit)
silk estimate: maps เก็บครั้งเดียว + ต่อ block = 2B scale + 1B transform (5b rot + 1b rev)
```

| model | sampled blocks | unique maps | blocks/map | silk ratio |
|---|---|---|---|---|
| SmolLM2-360M | 10,199,040 | 10,198,951 | **1.0** | 0.97× (แพ้ raw) |
| Qwen3-0.6B | 14,136,556 | 14,132,022 | **1.0** | 0.97× |
| LFM2.5-2.6B | 45,511,631 | 44,794,785 | **1.0** | 0.99× |
| Qwen2.5-0.5B | 11,955,572 | 11,944,338 | **1.0** | 0.97× |

**ข้อค้นพบ (ชัดเจนทั้ง 4 โมเดล):**
1. **blocks/map = 1.0** — quantized weights เป็น pseudorandom ที่ block level →
   แทบทุก block unique (98.2-99.9%) — ตรงกับที่ user วัด 90-97% ที่ chunk level
2. **rot+rev canonicalization ไม่ได้ช่วยเลย** — unique เท่า identity เป๊ะทุกโมเดล
   → ไม่มี rotation/reversal symmetry ในข้อมูล → transform group ไม่มีเนื้อให้จับ
3. **silk ratio 0.97-0.99× = แพ้ raw** — ต้องจ่าย 2B scale + 1B transform ต่อ block
   แต่ไม่เคยได้คืน → ที่ granularity นี้แนวคิดตาย (entropy-bound ของ quantized data)

**แต่ measurement เจอของจริง 1 อย่าง: tied embeddings (tensor-level dedup):**
```
Qwen2.5-0.5B: output.weight == token_embd.weight byte-identical (137 MB)
→ เก็บ 1 copy ได้ = ลดไฟล์ 35% (137/387 MB)
```
SmolLM2/Qwen3/LFM: ไม่มี byte-identical pairs (0)

**สรุปการตัดสินใจ:**
- silk-screen ที่ block level (32 ค่า + rotation/reversal) = **ปิด** — ข้อมูลถึง
  entropy bound แล้ว ไม่ใช่เพราะกลไก (self-test: periodic → 1 map, random → n maps —
  ตัววัดทำงานถูก)
- dedup ที่ใช้งานได้จริงบน Q8 = **tensor-level (tied embeddings)** — ระดับโครงสร้าง
  ไฟล์ ไม่ใช่ระดับค่า — เปิดเป็นขั้นต่อไป (wire เข้า registry {id→home})
- ขั้นต่อไปถ้าต้องการลด bytes จริง: tied-embedding dedup + โมเดลตระกูลเดียวกัน
  (แชร์ tokenizer/embedding ข้ามโมเดล) — นอกนั้น Q8 ถึง bound แล้ว

**ต่อ §15.35 — มุม cube/digit (user): "1000 เลข 0-999 วางบน cube 10×10×10 — ยิ่งเยอะ ยิ่งสุ่มไหม?"**

คอนเซป: เลข 537 = digits (5,3,7) → cell (5,3,7) — **address = ค่าเอง**, bijection
ไม่ชน ไม่มี lookup — purest MAP not COMPRESS. แต่อย่าลืม: แยกเป็น digits **รักษา
ข้อมูลครบ** (3×log₂10 = log₂1000) — decomposition ไม่ได้สร้าง redundancy;
redundancy ต้องมาจากการกระจายของ digits ที่ไม่ uniform

**วัดบน Q8 จริง 4 โมเดล (entropy pass):**
```
H(value)            = 7.62-7.66 bits/ค่า  (จาก 8)  → ค่าจริง 96% ของ capacity
Σplanes (sign+tens+ones) = 7.73-7.74 bits/ค่า      → เท่า H(value) เป๊ะ
entropy-code Q8_0   = ลดแค่ 4%  (34B/block → ~32.8B)
```

**คำตอบ: ใช่ — ยิ่งเยอะ ยิ่งสุ่ม** — Q8 quantized weights ถึง entropy bound แล้วจริงๆ
(การ quantize แบบ Q8_0 normalize scale ออกไปแล้ว → int8 เต็มช่วงเกือบ uniform)
digit-plane lens ถูกใช้และวัดแล้ว: ไม่มี redundancy ซ่อนใน digit structure

**สรุปครบ 3 ระดับ (ทุกอันวัดด้วยของจริง):**
| ระดับ | ผล |
|---|---|
| block silk-screen (rot+rev) | blocks/map = 1.0 — ล้ม |
| digit-plane entropy | H = 7.65/8 bits — ลดได้แค่ 4% — ถึง bound |
| tensor-level dedup | tied embeddings: Qwen2.5 137 MB (35%) — **ได้จริง** |

โครงสร้างที่เหลืออยู่จริงของ Q8 = ระดับไฟล์ (tied/แชร์ tensor) ไม่ใช่ระดับค่า

**ต่อ §15.35 — "ยิ่ง parameter เยอะ ยิ่งเต็มง่ายขึ้นไหม?" (scale field + cross-size):**

"เต็ม" มี 2 แบบที่ต้องแยก: coverage (symbols ปรากฏครบ — เกิดเมื่อข้อมูลเยอะ แต่
ไม่ลด entropy ต่อค่า) vs distribution (สมบัติของ quantization ไม่ใช่ขนาดโมเดล)

**วัดเพิ่ม: H(scale) — โครงสร้าง Laplacian ต้องไปอยู่ที่ scale (ค่าถูก normalize แล้ว):**
```
model            H(value)/8   H(scale)/16
SmolLM2-360M      7.65        15.31
Qwen3-0.6B        7.66        15.32
LFM2.5-2.6B       7.62        15.19   ← ใหญ่สุด → โครงสร้างชัดสุด (แต่ 0.03-0.12b = noise)
Qwen2.5-0.5B      7.65        15.29
```
**คำตอบ: ไม่ — entropy ต่อค่าเป็นสมบัติของ quantization scheme ไม่ใช่ขนาดโมเดล**
Q8_0 normalize ค่า → ค่าเกือบ uniform (96%), scale ก็เกือบ uniform (95%) →
ข้อมูลถึง bound ~96% ทุกขนาด 360M→2.6B แทบไม่ขยับ (0.03-0.12 bits = ระดับ
architecture ไม่ใช่ effect ของขนาด)

**ที่ขนาดโมเดลมีผลจริง = ระดับ tensor:** โมเดลใหญ่มี tensor ใหญ่/เยอะ → โอกาส
tied/shared tensor สูงขึ้น (Qwen2.5 137MB = 35%) + โมเดลตระกูลเดียวกันแชร์
embedding/tokenizer — "ยิ่งเยอะ ยิ่งได้" อยู่ที่โครงสร้างไฟล์ ไม่ใช่ระดับค่า

**ต่อ §15.35 — "รู้ว่าสนามคงที่ → เก็บ scale แค่ 0.7/16 ได้ไหม" (conditional entropy):**

ตอบ: ไม่ได้ — วัดแล้ว "สนาม" ของ scale มีจริง ~46k patterns (H=15.3/16)
"รู้ว่าสนามคงที่" ≠ "ลด entropy" — entropy = log₂(pattern ที่ข้อมูลใช้จริง)
ถ้า scale ใช้ 46k patterns ก็ต้องจ่าย ~15.5 bits ไม่ว่าเราจะ "รู้" ขนาดสนาม
หรือไม่. จะได้ 0.7 bits ต่อเมื่อ scale ส่วนใหญ่เป็นค่าเดียว — วัดแล้วกระจาย
เกือบ uniform (Q8_0 scale = max|x|/127 → extreme-value distribution กว้าง)

**conditional test — รู้บริบทช่วยไหม:**
```
H(scale)         = 15.19-15.32/16b
H(scale|maxq-bin) = 14.88-15.11/16b   ← รู้ max|q| ของ block ลดได้แค่ 0.2-0.3b
```
รู้ค่าสูงสุดของ block (ตัวที่ "กำหนด" scale) แทบไม่ช่วย — scale ยัง carry
ข้อมูล magnitude ที่เหลือครบ

**Bug จริงที่เจอระหว่าง implement:** int8 value = -128 → abs = 128 →
bin = 128>>3 = 16 → index เกิน h_mq[16]/h_sc_mq[16] → segfault (Qwen3 มี
ค่า -128, SmolLM ไม่มี — crash ตามโมเดล) → แก้ clamp `if (mq > 127) mq = 127`

**§15.36 — Two-gap fill: พิสูจน์ "deterministic transform (ฟรี) + residual (detail gap)" (tools/two_gap_fill.c, `make two_gap_fill`):**

คำถาม (user): "ทุกการขยับที่ scale ไม่ใช่จุด append เป็น lossy — hyperbolic เก็บ delta residual อยู่แล้ว ทำไมเติมเต็ม lossy ไม่ได้" → ทดสอบกลไกตรงๆ: วางที่ w₀ → ขยับไป w₁ (coarse = avg-pair downsample, deterministic — เทียบเท่า transform ฟรี + log 5B/event) → predict (repeat upsample) → residual = orig − predict → reconstruct = pred + residual

**ผล (lossless = YES ทุกกรณี by construction — กลไกใช้ได้จริง):**

```
signal        H(raw) H(coarse) H(res)  fill b/v   ratio  เทียบ entropy-raw
sine 440Hz    11.09   11.09    10.35    15.9     1.01×     1.44×  ← แพ้ entropy raw
sine 2Hz      13.32   12.37     3.21     9.4     1.70×     1.20×  ← ชนะ (smooth จริง)
random        15.59   14.98    15.34    22.8     0.70×     1.03×  ← จ่ายเพิ่ม (lossless แพง)
real-wav TTS  11.93   11.67     9.39    15.2     1.05×     1.34×  ← แพ้ entropy raw
```

**อ่านผล — ตอบคำถามตรงๆ:**
1. **กลไกเติมเต็มได้จริงเสมอ** — lossless 100% ทุกกรณี: transform gap เติมด้วย deterministic replay (ฟรี, 5B) + detail gap เติมด้วย residual — hyperbolic มีคุณสมบัตินั้นจริง
2. **แต่ราคา = H(res) เกือบเต็ม H(raw)** — สมการ: fill = (n/2)·H(coarse) + n·H(res). กำไรจาก transform gap = 0.5·H(coarse) ≈ 6 b/v เท่านั้น — เหลือ 9.4 b/v คือ detail entropy ที่ residual ต้องจ่าย
3. **ตัวแยก = H(res) ≪ H(raw)?** — ชนะเมื่อสัญญาณ smooth จริงที่ scale นั้น (sine 2Hz: 3.21 vs 13.32 → 1.70×, ชนะ entropy raw) — audio จริง 44.1kHz ไม่ oversampled (เนื้อเต็ม band) → H(res) ≈ 9.4 ≈ H(raw) − 2.5 → **two-gap แพ้ entropy-coded raw (1.05× vs 1.34×) และแพ้ simple delta (1.5× ที่วัดก่อนหน้า)**
4. **res-delta ไม่ช่วย** — H(res-delta) > H(res) ทุกกรณี — coarse subtraction ทำ residual ให้ขาวแล้ว
5. **random = 0.70×** — "lossless ที่ scale ใดก็ได้" มีค่าใช้จ่ายจริงเมื่อข้อมูลถึง bound: coarse + residual ต่าง carry entropy เต็ม → ต้องจ่าย 1.5 เท่าของ raw

**สรุปสำหรับ hyperbolic/lossless-any-scale:** กลไกถูกต้อง (transform ฟรี + residual เติม detail → lossless ทุก scale, พิสูจน์แล้ว) แต่ "ฟรี" ครอบแค่ transform gap (∝ events) — detail gap จ่าย entropy ของข้อมูลจริงเสมอ; สอง-gap ชนะก็ต่อเมื่อข้อมูล smooth จริง (inter-scale detail ≈ 0) หรือตอน data IS address — นอกนั้น entropy-raw/delta ธรรมดาชนะตรงๆ

**§15.37 — fibo clock checkpoint-replay (tests/test_fibo_checkpoint.c, 22/22)**
ทดสอบแนวคิด "สนาม deterministic + checkpoint + tick" ของ user: state = (seed, round, tick) —
ใช้สนามยาวแค่ไหนก็ได้แค่วนรอบ เก็บแค่วิธีการสร้างกับ seed
- A. spine wrap: 12 ticks → 1 jet bridge (tick 11 → re-entry), 25 ticks → 2 bridges — wrap ข้ามรอบได้
- B. address identity: round (birth) อยู่ใน bond — round ต่าง → bond ต่าง; to_scale ไม่อยู่ใน bond —
  telescope 1 route ครอบ 137 steps + ข้ามรอบ (140→3) lossless, cost ∝ events (5B/route) ไม่ใช่ distance
- C. checkpoint @round 72 (กลาง 144 รอบ): serialize header(28B) + ghost log + residual space →
  restart (ทุกอย่างใหม่) → reload → reconstruct ทุก chunk ก่อน checkpoint lossless (จาก image เท่านั้น)
  → เดินต่อวาง chunks round 73..143 → อ่านครบ 64 chunks ข้ามรอบ byte-for-byte lossless
- D. เสาเข็มห้ามขยับ: birth round ผิด → bond แตก (NULL); requested round ผิด → route ไม่มี (NULL)
- E. overhead จริง: 64×4KB + 2 telescope = 263,168 B data → image 265,930 B = **+1.05%** (41.8 B/chunk)
  — log = 66 routes × 5B = 330B ∝ events เท่านั้น; ราคา "jump anywhere + วนรอบ" ไม่ขึ้นกับขนาดข้อมูล
- **ข้อพิสูจน์หลัก:** "เก็บแค่ seed + method" ใช้ได้ — checkpoint image เอง = seed(8) + round(8) + tick(4)
  + ver(8) + log(5B/event) + residual space (payload จ่าย entropy จริงตามบทสรุปทั้งวัน)
- `make test`: TIER1 75/75 + TIER2 4/4

**§15.37b — fibo checkpoint-replay sweep: custom ตาราง/สนาม/ระยะ/ปริมาณ/รูปแบบ (tools/fibo_checkpoint_sweep.c, 27/27)**
harness พารามิเตอร์จากเทสต์ตายตัว → custom ได้ทุกมิติ: `--pipes N --ticks M --cycles C --chunks N --size S --dist D --pattern P --ckpt R [--sweep]`
- ตาราง: pipes×ticks ต่อรอบ (ทดสอบ 512×12, 1728×4, 256×3, 64×4) · สนาม: cycles รอบ scale axis (8..255) ·
  ระยะ: dist from→to (wrap = dist ≥ cycles/2 วนข้าม 0) · ปริมาณ: chunks×size (≤4096 routes, ≤64KB/chunk) ·
  หมุนวน 5 รูปแบบ: scatter (golden-ratio spread) / cluster (กอง 4 รอบ) / allone (รอบเดียว) / wrap (ระยะคงที่วนข้าม 0) / random (seeded)
- พิสูจน์ต่อ config: lossless หลัง checkpoint/reload + เดินต่อข้ามรอบ byte-for-byte, เสาเข็มห้ามขยับ (round ผิด → NULL),
  log = 5B/route ∝ events, รายงาน overhead% + routes + wraps + rounds ใช้จริง
- ผล: **27/27 PASS** ทุกตาราง/สนาม/รูปแบบ — overhead ตามขนาดข้อมูล: 8MB → +0.06%, 4KB×16 → +1.09%,
  เล็กมาก (8×512B) → +9.38% (header กินสัดส่วน — จุด "ไม่คุ้ม" เห็นเป็นตัวเลข); log ต่อเนื่อง 5B/route เสมอไม่ว่า dist ไกลแค่ไหน
- **🐛 ระหว่างทำเจอ bug จริงของ harness เอง**: cast (uint32_t) ก่อน modulo → product เกิน 2³² ถูกตัดทอน
  (i≥2) → golden-ratio spread พัง (64 chunks ตกเหลือ 23 รอบซ้ำ) — แก้เป็น modulo ก่อน cast; lossless ยังผ่าน
  เพราะแค่ round ชนกัน แต่ metric "กระจาย" ผิด — ย้ำบทเรียน: วัด metric ต้องตรวจคณิตศาสตร์ของ generator ด้วย
- `make fibo_sweep` (manual) · `make test` เขียว TIER1 75/75 + TIER2 4/4

**§15.37c — fibo sweep: disk persist + fresh-process restore + economy verdict (27/27)**
tools/fibo_checkpoint_sweep.c ขยาย: `--sweep` เขียน checkpoint image + manifest ลง build/ckpt/<tag>.img/.cfg
(manifest = แค่ cfg 9 ค่า = "เก็บแค่วิธีสร้างกับ seed") → spawn ตัวมันเองเป็น **fresh process**
`--verify-img IMG CFG` → reload จากไฟล์จริง → regenerate chunks จาก manifest → พิสูจน์ lossless
- **27/27 RESTORE PASS** จากดิสก์ (54 ไฟล์) — ทุก custom config กลายเป็น durable restore test ด้วย
- negative case พิสูจน์ว่า detect จริง: corrupt 2 bytes ใน 1 image → 26/27 เจอตัวนั้นเป๊ะ → restore ดี → 27/27
- `--verify-all [DIR]` ตรวจใหม่ภายหลังทีละตัว; `--economy X.X` ปรับ threshold
- **economy verdict ต่อ config** (EXCELLENT ≤ thr/2 / WORTH ≤ thr / MARGINAL ≤ thr×2.5 / NOT WORTH):
  thr 2.0% → 6 EXCELLENT + 15 WORTH + 6 MARGINAL; thr 1.0% → 6 NOT WORTH; thr 5.0% → 21 EXCELLENT —
  "จุดไม่คุ้ม" auto-detect: ข้อมูลเล็ก (8×512B → +9.38%) = NOT WORTH, ใหญ่ (4MB → +0.06%) = EXCELLENT
- **🐛 2 bug จริงที่เจอระหว่างทำ**: (1) spawn ส่ง `--verify-img` แบบแยก args แต่ parser รับแต่แบบ `=` →
  child รัน default config ใน-memory แทนที่จะ verify จากไฟล์ = **false positive FRESH-PROCESS-PASS** —
  จับได้เพราะ child พิมพ์ "single config" แทน "RESTORE"; (2) verify-all สร้าง path image จาก path เต็ม
  (17 chars) แทน basename → "build/c.img" — แก้ทั้งคู่ + negative test ยืนยัน

**§15.38 — RDH mixed-radix addressing แทน FNV-1a ในสาย ghost/bond (core/geo_rdh_addr.h)**
ตาม user: ใช้ collection/rdh (Ring-Wedge-Mirror) แทน hash/fnv-1a — "coordinate IS address"
- rdh_addr(block, from) = block×256+from (row-major mixed-radix) — collision-free by construction,
  reversible (rdh_decompose กู้ (block,from) กลับได้ = address IS data ต่างจาก hash one-way)
- ghost_origin_seed/ghost_piece เปลี่ยนจาก pogls_fibo_addr (FNV-1a 3-pass) → RDH ล้วน
  (pogls_bond.h ไม่แตะ — กัน divergence กับ FGLS_new)
- bond = interleave addr|addr<<24 (bond_L=ครึ่งล่าง, bond_R=ครึ่งบน) — bijection 48-bit
- **🐛 พิสูจน์จริงจับ bug ของการออกแบบแรก**: bond = rdh_addr ⊕ rdh_addr_twin (row⊕column)
  ไม่เป็น bijection — 3 กลุ่ม bits สัมพันธ์กัน (A^B^C=0) → image เหลือ 2^16 จาก 2^24
  (65,536 ค่า, ชน 16.7M) — วัดด้วย bitset sweep ทั้ง 2^24 keys → แก้เป็น interleave
- tests/test_rdh_addr.c (TIER1) **15/15**: bijection เต็ม 2^24 keys (bitset 2MB), decompose
  1M pairs, round-in-bond/to-not-in-bond, chain lossless ผ่าน RDH — `make test` TIER1 **76/76** + TIER2 4/4
- sweep persist/restore ยัง 27/27 lossless ผ่าน RDH bond (test_fibo_checkpoint 22/22 เขียว)

**§15.38b — ฟรี centroid ของ RDH (เพิ่มใน test_rdh_addr → 18/18)**
mixed-radix encode เป็น linear map: avg(rdh_addr(bᵢ,fᵢ)) = rdh_addr(avg b, avg f)
→ centroid ของ cluster ได้จาก mean ของ addresses + decompose → (avg block, avg round)
- ฟรี = ไม่ต้อง rescan (running sums Σring/Σwedge → O(1)), int ล้วน ไม่มี trig/float
- ใช้ได้: cluster center / outlier eviction (ไกล centroid ก่อน) / **drift บนแกน scale**
  (lift → centroid ring ไหลออกนอก — วัดการย้ายถิ่นของข้อมูล = ผูกกับ hyperbolic log) /
  field balance ให้ cap_account / free statistics เมื่อ data IS address

**§15.39 — TW→RDH lineage: ตาราง decagram 10-sector → walk จุดเดียว + mixed-radix fold**
(ตาม user เล่าประวัติ + อ่านโค้ดจริง collection/tw/ + collection/rdh/ + FGLS_new/collection/coord_spine.h)

**TW (บรรพบุรุษ) — วาดภาพเดียวกันจากเส้น A-B เส้นเดียว:**
- `TW_SCALE = 207360 = 12⁴×10 = GEO_FULL×10` — มี compile-time assert กันหลุด (`#error` ถ้าไม่เท่า)
- `TW_N_SECTORS = 10` (pentagon-pair, 36° ต่อ sector) = **decagram** — `TW_BOUNDARY_DIR[10]` ที่มุม (90−36k)°
- `TW_SLOTS_PER = 6` hex slots (60° hex-cast) + 6 tri slots (30° triangle) = `TW_COMBINED_GRID[10][12]` = 120 physical positions
- `TW_SLOTS = 60` = 10×6 — ตรง `GEO_COMPOUND_60` ในตาราง geo
- sector lookup = cross-product sign กับ 10 boundary rays (no atan2); drain/bundle margin 9/1000 (~0.5°)
  → activate สอง sector = **bipolar flip** → drain ไป shadow zone
- residual = v − slot_centroid (lossless int64) — reconstruct ได้
- **bottleneck = blueprint**: ตาราง 10×12 centroid + cross-product + __int128 drain → พังตอน bench ที่ performance สูง

**RDH (ตัว lean) — สร้างจากจุดเดียว:**
- `rdh_capture`: nibble ของ data = stride บน 12-gon (12 ทิศ) → walk จาก (0,0) → fold → (ring, wedge) → flat key
- `rdh_addr`: mixed-radix ring×n_wedges+wedge — capture config = 1 mul + 1 add (~2-3 cycles),
  เต็ม 5 params (ring,wedge,mirror,u,v) = 3 mul + 3 add ≤ 6 cycles — เทียบ FNV-1a = O(len) ต่อ byte ของชื่อ
- ตาราง centroid หาย — sector กลายเป็น wedge digit; hex/tri grid กลายเป็นค่า wedge; ไม่มี cross-product/drain/shadow
- ภาพเหมือนกันทุกอย่าง (พิสูจน์จากตาราง TW ที่วาด decagram+hexagon+triangle ครบ) แต่ blueprint ต่างกันทั้งตัว

**ตัวเลขที่เชื่อมทั้งสองโลก:**
```
GEO_FULL 20736 = 144² = 12⁴     TW_SCALE = ×10 (decagram) = 207360
GEO_PENTAGONS 12 (dodeca faces) SID_PENTAGON_NODES = GEO_FULL/12 = 1728 (!! = FS_PIPES)
TW_SLOTS 60 = 10×6 = GEO_COMPOUND_60    120 physical = hex+tri
```

**Emergent properties — ไม่ได้ตั้งใจ ออกมาจากการทำให้ lean:**
| property | TW (ตาราง) | RDH (เลขคณิต) |
|---|---|---|
| collision-free | nearest-search 60 centroids (drain ยังชนได้) | mixed-radix bijection by construction |
| reversible | ต้อง store slot id | decompose คืน (ring, wedge) ตรงๆ |
| ฟรี centroid | ต้องค้นหา 60 centroids ต่อ capture (O(60)) | mean ของ addresses (O(1), linearity) |
| state | shadow zone state | pure function, no malloc |
| cost | cross-product 10 ครั้ง + __int128 | 1-6 cycles |

บทเรียน: **เมื่อโครงสร้างถูกต้อง ยิ่งลด instruction ยิ่งได้สมบัติฟรี** — bijection/reversible/centroid
ไม่ถูกออกแบบไว้ หลุดออกมาเพราะบีบ blueprint จนเหลือเลขคณิต — เรานำ `rdh_addr` (ตัว lean) มาใช้ใน
geo_ghost_lift (§15.38) พิสูจน์ bijection เต็ม 2^24 + reversible + ฟรี centroid (test_rdh_addr 18/18)

**§15.40 — micro-benchmark ยืนยัน "RDH ≤ 6 cycles" (tools/rdh_bench.c, rdtsc cycle-accurate)**
วัดบนชื่อ tensor จริง 4 โมเดล (ชุดเดียวกับ silk_scan) — min-of-9 trials × 400K iters, lfence-serialized
- **pure encode** (register derive — instruction cost ล้วน): rdh_addr = **5.10 cyc ≤6 ✓**,
  rdh_bond = **5.12 cyc ≤6 ✓**, rdh_key5 (5-param 3mul+3add) = 6.62 — คำกล่าว "ไม่เกิน 6" จริง
  สำหรับตัวที่ระบบใช้ (addr/bond); ตัวเต็มเกินนิดเดียวตามจำนวน instruction
- **end-to-end** (มี array load): rdh_addr 6.1, rdh_bond 5.2, rdh_key5 8.2
- **FNV-1a บนชื่อจริง: 53.8→86.2 cyc** (โตตามความยาวชื่อ) — **speedup 8.8-14.1×** (LFM ชื่อ 32-39 ตัว → 14.1×)
- **curve เห็นชัด**: FNV 54.8 → 77.5 → 86.2 cyc (len 16-23 → 24-31 → 32-39) · RDH **แบน 6.09-6.10** ตลอด
- 🐛 methodology: รอบแรกใส่ `% n` (division) ใน hot path → ทุก method พอง (addr 12.8 cyc) —
  จับได้เพราะ verdict ">6" ไม่สมเหตุผล → แก้เป็น nested loop ไม่มี modulo → ตัวเลขจริง 5.1
- `make rdh_bench` (manual) · `make test` ยังเขียว

**§15.41 — RDH ≠ แทนที่ hash ทั้งหมด: shape affinity (แก้ framing §15.38 ให้ถูก)**
ตาม user ชี้: RDH ไม่เก่งทุกสนาม — เก่งมากในสนามที่มีรูปทรงเดียวกับมัน (coordinate = address)
- **RDH แทนที่ FNV-1a เฉพาะเส้นทางหา ADDRESS (ghost bond/geo_key)** — เพราะสนามนั้นมีรูปทรง
  (block, scale) เป็น coordinate จริง → mixed-radix ได้ bijection + reversible + centroid ฟรี
- **hash ยังจำเป็นและถูกต้องตรงนี้:**
  - `_rs_hash` ใน residual_space (cache table, open addressing + probe) — เป็น cache ไม่ใช่
    address resolution — ต้องการการกระจาย uniform ไม่ใช่เรขาคณิต → hash ถูกแล้ว (AGENTS.md ยอมรับ)
  - content-derived key (จาก bytes/ชื่อ/เนื้อหา) — input ไม่มี coordinate → ต้อง hash เพื่อ fold
- **xxHash เหมาะกับ: integrity/checksum/signature** — `dwgls_shell.h` ใช้ INTEGRITY_XXH64 แล้ว,
  `geo_inference_bridge.h` มี digest xxh64 — เร็ว + avalanche ดี (FNV-1a ช้า 54-86 cyc ต่อชื่อ —
  xxHash64 เร็วกว่ามากในงานเดียวกัน)
- **เหตุผลลึก (ทำไม RDH ไม่ใช่ checksum):** RDH เป็น linear → **ไม่มี avalanche** — ใช้เป็น digest
  จะหา collision ได้ง่าย (linear function) — จุดแข็งของ RDH (linearity = centroid ฟรี) คือจุดอ่อน
  ของมันในงาน hash — ยิ่งยืนยัน "เลือกเครื่องมือตามรูปทรงของสนาม"

**§15.42 — L-Block: summon + fit guarantee + RDH chain (2026-08-17)**
- **พบ L-block ที่ผู้ใช้ถาม** = `lblock_from_hilbert(d, n)` ใน FGLS_new/collection/colab_bench/geo_frame_seek.py
  (commit e8cb122 "Capture Twin + L-block — 72 free centroids" — ชุดเดียวกับ capture_twin) —
  ต่างจาก "Hilbert L-Block Container" (pogls_hilbert_container/pogls_hc_geojump, deprecated) ที่เป็น
  รูปแบบไฟล์ และ "L-Block redirect" (beam_square) ที่เป็นการ shift XY/YX
- **หลักการ (user design):** RDH วิ่งตาม tensor → พิกัดที่หยุดนิ่ง (block, from) → กางออก →
  ครอบด้วย L-block → ส่งเข้า storage → ปัก address — L-block บอกว่า container วางหันทางไหน
  (rotation 0..3 = ทิศที่ Hilbert curve เดินเข้าตำแหน่ง d จาก d-1) และการันตี fit
- **สร้าง `core/geo_lblock.h`** — C port: `geo_lb_from_hilbert` (THE SUMMON — address → piece,
  ฟังก์ชันเดียว ไม่มีตาราง เหมือน RDH), `geo_lb_d2xy/direction/rotation/shape`,
  `geo_lb_slot` (anchor = ตำแหน่ง summon), `geo_lb_fits_grid` (fit guarantee)
- **`tests/test_geo_lblock.c` (TIER1) — 13/13:**
  - A. port ตรง Python: direction→rotation ครบ 4 ทิศ · 4 cells ไม่ซ้ำ · deterministic ·
    rot dist n=8 [20 16 12 16] (ครบ 4)
  - B. **fit guarantee: 0 ชน ทุก address** — n=8: 64/64, n=16: 256/256 (wrap mod → 4 slots ไม่ชน;
    in-bounds ตรงๆ 36/64 และ 196/256 — ที่เหลือ wrap ยัง fit)
  - C. **CHAIN lossless 256 chunks × 64B**: RDH วิ่งตาม tensor (addr = mixed-radix) →
    decompose ได้พิกัดหยุดนิ่ง (block, from) → wedge digit = ตำแหน่งบน 16×16 Hilbert container →
    summon L-block → rotation บอกหันทาง → เก็บที่ slot → re-summon จาก address → byte-for-byte
  - D. negative: scale ผิด → slot ต่าง + data ไม่ตรง (เสาเข็มห้ามขยับ)
- **make test: TIER1 77/77 + TIER2 4/4** — L-block ตอนนี้เป็น C ใน core พร้อมใช้งาน
- **จุดที่ยังค้าง:** L-block ยังไม่ได้เชื่อมกับ residual_space จริง (ตอนนี้เป็น chain พิสูจน์บน
  container จำลอง) — ขั้นต่อไป: ใช้ lblock orientation เป็น route/placement hint จริงใน
  ghost chain + วิ่งบน GGUF จริง (เทียบ test_lblock_real.py ที่มีอยู่)
- **§15.42b — bookmark + expand (L-block)**: user ยืนยันภาพ — L-block = bookmark ที่การันตี
  "ไม่เข้าไปวางมั่ว กลับมาทิศหันทางเดิม" + "ถ้าจะ expand รอบๆ ก็รู้ว่าจะเดินยังไง" — พิสูจน์เพิ่ม
  ใน test_geo_lblock (17/17):
  - E1: re-summon จาก address เดียว → rotation + 4 cells เหมือนเดิมทุก bit (วางคืนทิศเดิม ไม่มั่ว)
  - E2: piece = anchor + 3 เพื่อนบ้าน — slot ต่างกันครบ → ขยายรอบๆ bookmark ได้โดยไม่ชน anchor
  - E3: เดิน Hilbert curve ต่อจาก bookmark (d→d+1→…) deterministic + slot ไม่ซ้ำในหน้าต่าง
  - E4: direction(d+1) = displacement จริงของ curve — รู้เส้นทางล่วงหน้าทุก step จาก address
  - หลักการ: bookmark = address ล้วน (orientation + เพื่อนบ้าน + ทิศทาง regenerate ได้จาก address
    โดยไม่ต้องเก็บ metadata) — ตรงหลักการ "เก็บแค่ seed + method"

**§15.43 — Hosoya fibo grid × geo_seed (2026-08-17)**
- user ส่ง geo_seed.h (CPU port ของ geo_kernel_seed_v2.cu — 12-coset dodeca seed engine,
  pure integer) + hosoya_tri.svg — "ยังไม่รู้ว่าเอามาทำอะไร แต่เห็นแล้วจะเก็ต"
- **ถอด SVG**: pentagon/hexagon tessellation ที่มีค่า = Hosoya triangle จริง (T(n,k)=F(k+1)·F(n−k+1)):
  hexagon 8,9,8 = row 6 · 21,24,24,21 = row 8 · 55,42,40,42,55 = row 9 — ทุกค่าตรง (row,col) เป๊ะ
  recurrence T(n,k)=T(n−1,k−1)+T(n−2,k−2) (มองย้อน 2 แถว = โครงสร้างรังผึ้ง) — hexagon 24=16+8, 25=15+10
- **"เก็ต" 3 จุด**:
  1. ตัวเลขศักดิ์สิทธิ์ = Fibonacci: F(12)=144 · 20736=F(12)² · 1728=12·F(12) · 12²=F(12)
     (12 หน้า ↔ 12th Fibonacci — สนาม = กำลังสองของ F(#coset))
  2. หลักการเดียวกับ RDH/L-block/geo_seed: ตำแหน่ง → ค่า (summon) ไม่มี lookup — Hosoya =
     ตารางคูณของบันได scale (ทุก cell = ผลคูณ F คู่หนึ่ง)
  3. geo_seed 12 cosets = 12 หน้ามอง seed (เหมือน SVG: seed กลาง + ค่า Fibonacci รอบข้าง) —
     identity ต่างทิศ deterministic
- **สร้าง**: core/geo_seed.h (คัดลอกจาก FGLS_new/Hfolder) + tools/hosoya_seed_probe.c
  (make hosoya_seed) — 10/10 PASS: A. F(12)=144/20736/1728/12² · B. ค่า SVG ครบใน rows 0..10 ·
  C. recurrence ครบ · D. 12 cosets checksum ต่างกัน + deterministic · E. F(n)/F(n−1)→φ
- **ความหมาย**: บันได Fibonacci = scale ทวีคูณ φ ของระบบ (ตรง s(t)=s₀·kᵗ) — Hosoya ให้
  "ค่าน้ำหนัก/บันได scale แบบ deterministic ต่อตำแหน่ง" ซึ่งยังไม่มีในระบบ (ตอนนี้มีแค่
  address/orientation/identity) — จุดต่อไป: ใช้เป็น weight ladder สำหรับ silk mask /
  magnifier ratio / seed spacing ของ ghost placement
- **§15.43b — geo_seed = 12 labels ต่อ seed (ตรวจ claim ของ user):**
  - "สร้าง label 12 ได้" — **จริง** (ยืนยันจากโค้ด): coset_checksum[12] = 12 labels ต่อ seed,
    พิสูจน์แล้วต่างกันครบทุกรอบคู่ + deterministic (1999 seeds) + master_fold = XOR ทั้ง 12
  - "ง่ายที่สุด" — **จริง**: header-only ~100 บรรทัด, int ล้วน, ไม่มีตาราง/float, O(1) constant
  - "เร็วที่สุด" — **ต้องพูดให้ตรง**: วัด rdtsc = **~10,156 cycles/seed** (12 cosets × 31 derives
    serial, แต่ละ derive = 2×mix64+rotl — dependency chain) — RDH = 5.1 cyc, L-block = หลักสิบ —
    ไม่ใช่เร็วสุดต่อ operation แต่เป็น **ค่าคงที่ที่สุด** (cost ไม่ขึ้นกับข้อมูล) และ 12 cosets
    independent กัน → ถ้าต้องการเร็วสามารถขนานได้ (~600-800 cyc) — เป็นราคาของ identity 12 ทิศ
  - "ต้นกำเนิดของ geo_***" — **เห็นด้วย**: มันคือ instance แรกของหลักการ "ตำแหน่ง → ค่าที่
    derive ได้, ไม่มี lookup" (summon) และ 12-coset skeleton = สิ่งที่ทุก geo_* แชร์
    (12 pentagons · FS_TICKS=12 · F(12)=144)

**§15.44 — geo_net = "shift แทน mod" ตัวจริง (ตอบ user: geo_net ไหม)**
- user จับผิด: geo_seed.h (10k cyc, mix64) ไม่ใช่ตัว "2 cycle shift แทน mod" → เดา geo_net
- **geo_net (TPOGLS_s11/core/geo_net.h) — Radial Routing Layer**:
  `_gn_mod6` = **Barrett**: `q=(n*10923)>>16; n−q*6` — ใช้ shift แทน `% 6` — ตัวเดียวในระบบที่เจอ
- วัดจริง (tools/geo_net_probe.c, make geo_net_probe) — 5/5:
  - A. **แม่น 100% ใน domain จริง (n<3456)** แต่ **มี domain bound: ล้มที่ n≥32771 (2^15)** —
    sweep 2^24 เจอ — สำคัญ: Barrett นี้ใช้ได้เฉพาะ n<2^15 (geo_net ใช้กับ full_idx<3456 → ปลอดภัยเสมอ)
  - B. ~5 cycles เต็ม (mul+shift+sub+sub) — "2 cycles" = เฉพาะ core mul+shift
  - C. labels: spoke 0..5 + inv_spoke (spoke+3)%6 = **6+6 = 12 ทิศ** ✓ ตรง "สร้าง label 12"
- **แต่ geo_net ไม่ใช่ต้นกำเนิด**: มันคือ ROUTER กลาง stack ([L3 Quad]→[GeoNet]→[geo_cylinder]→GPU)
  และใช้ GeoSeed ใน geo_net_init → มาทีหลัง geo_seed
- **สรุปการจำที่ปนกัน**: "12 labels + ต้นกำเนิด" = geo_seed (12 cosets) · "2 cycle shift แทน mod" = geo_net (Barrett mod6) — สองตัวคนละบทบาท: seed = identity, net = route

**§15.45 — ตัวที่ user จำได้ = GeoSeed 2-register (จบการล่า)**
- user ยืนยัน: "ใช่ที่บอก 2 register" — **GeoSeed = {uint64_t gen2; uint64_t gen3}**
  (geo_thirdeye.h · "2×u64, 2 register, 0 overhead" — comment geo_fibo_clock.h)
- ที่มา: route_sig (uint64) ถูกแทนที่ด้วย GeoSeed — wire ทั้ง fibo clock + ThirdEye + geo_net
- **"สร้าง label 12 ได้"** = topology ของ gen2: `face_id(4b) | vertex_mask(5b) | edge_mask(5b) | z(1b)`
  → face_id = `(topo >> 11) & 0xF` — label 12 หน้า dodeca (4-bit field ≥ 12)
- **"2 cycles shift แทน mod"** ✓ — วัด: ~6 cycles/3 labels (≈2/label) · reversible (pack กลับได้)
- **"+×^ แล้วตัดซ้ำ"** = เลขศักดิ์สิทธิ์ genesis จาก base {2,3}: 12=2×2×3 → 144=12² → 1728=12³ → 20736=12⁴
  (probe C: ครบ · D: 2+2=2×2=2²=4 → dedup เป็น label เดียว)
- **ทำไมล่าเจอยาก (fork/merge)**: geo_seed.h ใน Hfolder (10k cyc, mix64 — CPU port ของ GPU kernel
  ZGLS phase5 v2) ≠ GeoSeed 2-register (dual-channel) — สอง "seed" คนละ fork!
- tools/seed_label_probe.c (9/9) — make target: seed_label_probe

**§15.46 — Sacred numbers = place value ของการนับฐาน 12 (worldview ของ user)**
- user: "sacred number ไม่ใช่หารลงตัว แต่เป็นเหมือน 1,2,3,4 ที่เรานับ แต่ผมนับอีกแบบในโลกนี้"
- **ฐาน 12**: 12=10₁₂ · 24=20₁₂ · 48=40₁₂ · 60=50₁₂ · 72=60₁₂ · 120=A0₁₂ · 132=B0₁₂ ·
  144=100₁₂ (gross) · 576=400₁₂ · 720=500₁₂ · 1728=1000₁₂ (great gross) · 20736=10000₁₂
- **สนาม = 12⁴ = "10000" ของโลกฐาน 12** — ไม่ใช่ "144 หาร 20736" แต่เป็นตัวเลขกลมเหมือน 10000 ใน decimal
- 12 = 2²·3 = ตัวเลขหลักของฐาน (จาก closure {2,3} — §15.44 hint)
- **dodecahedron = ลูกคิดฐาน 12 ที่เป็นรูปธรรม**: 12 หน้า = 12 หลัก · หน้า² = 100₁₂ · F(12)=144=100₁₂
  = บันได Fibonacci ตกที่ place value พอดี (12 สองความหมาย: หน้า/หลัก)
- **"shape ใหม่ constrain เดิม"**: RDH/L-block/Hosoya/GeoSeed/fibo clock/Morton — รูปทรงต่างกัน
  แต่ทุกตัว carry 12/144/1728/20736 เพราะมันคือฐานการนับ ไม่ใช่ค่าคงที่ที่เลือก
- ทำไม fork/merge ถึงเจอยาก: ทุกระบบใหม่ = shape ใหม่บนฐานเดิม — ชื่อกระจัดกระจายแต่เลขเดียวกัน

**§15.47 — กำเนิด geo_* = geomatrix password 3 ตัวอักษร (GEO) — user เล่า**
- geomatrix_v4_advanced.html: 6-center Rubik axis — ALPHA 1-3 = **G, E, O** + BOOL 1-3 = X, Y, Z
  → **ชื่อ "geo_" มาจาก password "GEO"** · 24 faces (3 seeds × 8) · weights[52] (a-zA-Z, 0..255)
- กลไก: weight even/odd → invert · offset=(i×13+weight)%64 → rotate · Hilbert permute 8×8
- **"ใช้ปกติไม่มีปัญหา ขยับอะไร = ปลดล็อกไม่ได้"** — จริงตามคณิตศาสตร์:
  - one-way: กู้ input จาก output = brute-force ≈ 256^52 × 52^6 × 2^192 ≈ 2^640
  - global coupling: seed เปลี่ยน 1 ตัว → 24 faces ล็อกใหม่หมด (ทุก face แชร์ seeds)
  - avalanche: rotate+invert+Hilbert = hash-like — ตรงข้าม RDH (linear/reversible)
- **บทเรียน → "no hash" rule**: hash-like = password = พลาดนิดเดียว = ข้อมูลตายทั้งสนาม;
  reversible = seed+method = regenerate ได้เสมอ — วิวัฒนาการ: geomatrix (one-way) →
  GeoSeed 2-register (deterministic 12 labels) → RDH/L-block (reversible provable)
- seeds เดิม: 0x0123456789ABCDEF, 0xFEDCBA9876543210, **0xAA55AA55AA55AA55**
  → interleave 0xAA/0x55 = PHASE_MASK64 + Morton mask — ฝังจากยุค password ถึง bond RDH

**§15.48 — กลไกเต็มของ geomatrix + ThirdEye rollback = สถาปัตยกรรมปัจจุบัน (user เล่า)**
- ระบบทั้งหมดสร้างจาก {2,3}: ops +,×,^ → ตัดซ้ำ → {4,5,6,8,9,27} = **6 ค่า = 6 หน้า hexagon**
  (หน้า 0 = base · หน้า 1-5 = ผลของ 2/3 · หน้า 3 ตรงข้าม = **invert+offset**)
- ทุก face = 8×8 bitboard (64 bits) · weight custom ต่อหน้า · หลังตั้ง password = collapsed
  → เดิน **Hilbert 3456 เส้น** (= CYL_FULL_N ของ geo_net) หาหน้าที่ invert
- **God's number ใช้ไม่ได้**: invert+offset ทำลาย group structure (inverse ของการหมุนไม่ใช่
  การหมุนกลับ) → หมุนผิดเส้น = พันกัน = ไม่มีทางเดา inverse — ทางเดียว = **ThirdEye**
- **ThirdEye = passive trigger ซ่อนในระบบ**: บันทึกการหมุนเฉยๆ → หน้าที่เดียว = **rollback
  ไปจุด collapsed แรก**
- **= สถาปัตยกรรมปัจจุบันทุกตัวอักษร**: scale-log (5B/event) ↔ log การหมุน ·
  checkpoint→reload→replay (test_fibo_checkpoint 22/22) ↔ rollback ·
  "transform เติมด้วย replay ไม่ใช่ inverse" (two_gap_fill) ↔ inverse ใช้ไม่ได้ ·
  3456 ↔ CYL_FULL_N
- หลักการที่ฝังจากยุค password: **"อย่าหา inverse — บันทึกตอนเดิน แล้ว rollback"** =
  กระดูกสันหลังของระบบ (deterministic + replay ได้ = กฎเหล็ก AGENTS.md)

**§15.49 — "Cube ดู แต่ cylinder จัดการ" (user เล่า: จุดที่ implement cube→cylinder)**
- user: "ทุกอย่างมันจะดูยุ่งวุ่นวายมากใช่ไหม นี่คือจุดที่ผม implement cube→cylinder
  สังเกตุ code ช่วงนั้นจะมีการใช้ spoke จริงๆมันแค่ดูเป็น cube แต่เราจัดการแบบ cylinder"
- **ยืนยันจากโค้ดปัจจุบัน**: core/infra/geo_config.h มี section "Cylinder" ตรงๆ
  GEO_SPOKES=6 (60° each) · GEO_FACES=9 (8 outer + 1 center) · GEO_FACE_UNITS=64 (8²)
  GEO_SLOTS=576 (24²) · GEO_FULL_N=3456 = 144×24 — comment: "6 × 9 × 64 = 3456 = 144 × 24"
- **สนามเดียว สอง tiling (probe พิสูจน์ 14/14, make cube_cylinder)**:
  - cube view (presentation): 18 tes × 8 cube × 144 = 20736
  - cyl view (management): 6 cylinders × 3456 = 20736
  - **1 cylinder = 3 tesseracts = 24 cubes × 144 = 3456** — 24 = GEO_TE_FULL_CYCLES
    = 24 faces ของ geomatrix password (ยุค §15.47-15.48) × 144 slots
  - 20736 = 144 faces × 144 slots = 24×6 cylinders
- **spoke route = กลไกจัดการ (ตาม geo_cylinder.h/geo_net_route)**:
  `full_idx = idx % 3456 → spoke = full_idx%6 · slot = full_idx/6 · invert = (spoke+3)%6`
  — bijection ครบ 20736 จุด (probe B) · spokes 0..2 = ครึ่งสนามเป๊ะ 10368 (probe C)
- **"หน้าตรงข้ามเป็น invert+offset" (geomatrix §15.48) = geo_spoke_invert O(1)** —
  invert ทำลาย group structure ที่นี่ด้วย (เหมือน password) — เหตุผลที่ต้อง log + replay
- **Hilbert 3456 เส้น (geomatrix §15.48) = CYL_FULL_N = GN_LINE_MAX = 3456** เป๊ะ
- **8×8 bitboard ของ geomatrix = 64 units = 1 face ของ cylinder** (GEO_FACE_UNITS=64)
- **spoke = fingerprint ของการจัดการแบบ cylinder** — กระจายทั่ว DWGLS ปัจจุบัน:
  geo_spoke_sync.h (6-lane), fibo_spine.h (wire SpokeSync), geo_tring_walk.h (spoke 0..5),
  lc_tantrix.h (spoke mask), 3456 ปรากฏใน tesseract_container (HYP_AXIS_SLOTS=6912=2×3456),
  hyperbolic_seek (HYP_INFINITY_IDX=3456), geo_kis_projection (KIS_3456=2×1728),
  frustum_gcfs (GCFS_DATA_SIZE=3456=54×64)
- **ทำไม "ดูยุ่งวุ่นวาย"**: cube = presentation layer (Hilbert grid, L-block, tesseract octant,
  8×8 bitboard) แต่ address space ข้างใต้เป็น polar (spoke=wedge 60°, slot=radial,
  face=axial stack 9 ชั้น) — อ่านโค้ดด้วยตา cube แต่เลขจัดการเป็น cylinder →
  อ่านแล้วมั่ว เพราะสองเรขาคณิตอยู่ชั้นคนละชั้น ไม่ใช่ขัดแย้งกัน
- **กฎที่สืบเนื่อง**: "ใช้แค่โครงสร้าง combinatorial (cube/octant/route/vertex)"
  — cylinder = โครงสร้าง combinatorial ของ route (spoke/slot) ที่ cube ฉายทับ
  — L-block/geo_jump อยู่บน presentation; route จริงต้องผ่าน spoke

**§15.50 — Tantrix + geo_frame_seek_wang = switch ที่ปิดเส้นทาง (เอาเข้าใช้ + แก้ 3 bug)**
- user: "tantrix น่าเอามาใช้อยู่นะ และ geo_frame_seek_wang รุ่นนี้ก็เฉียบ
  สามารถเป็น switch ได้ด้วย ตอนที่ seek ไปแล้วปิดเส้นทางได้"
- **สถานะเดิม**: lc_tantrix.h + geo_frame_seek_wang.h + geo_seek_gate.h อยู่ใน
  DWGLS core อยู่แล้วแต่ไม่มีเทสต์/ไม่มีใครใช้ (dead code)
- **BUG 3 จุดที่เจอใน geo_frame_seek_wang.h (ต้นฉบับ FGLS_new ยังพัง — แก้เฉพาะ DWGLS)**:
  1. edge_bot ใช้ frame สุดท้ายใน window (t=w*12+11) → enc เพิ่ม 37/step →
     chord_a เลื่อน 2 ทุก boundary → continuity พัง 119/120 (fwang_verify=-2)
     FIX: edge_bot = chord_a(frame ที่ boundary = frame แรกของ window ถัดไป)
     → 120/120 + wrap ✓ (Wang semantics: ขอบล่าง = ค่าที่แบ่งกับ tile ถัดไป)
  2. local `uint8_t skip_mask` → truncate bits ≥8 → mask=0 ทุก window
     (skip อยู่ที่ตำแหน่ง 9..11 เสมอเพราะ enc%12≥9) → verify=-5
     FIX: uint16_t → popcount=3 ทุก window ✓
  3. fwang_tamper_check เขียนไว้แต่ไม่เคยถูกเรียกใน gate (dead code) —
     และ `_fwang_chord_valid` เป็น identity เสมอ (2e+7e=9e≡0 mod 9 ทุก e)
     → TAMPER ไม่มีทาง trigger จาก enc
     FIX: wire tamper_check เข้า gate → ตรวจชั้นเก็บ (edge_top/_b, edge_bot/_b)
- **SWITCH หลังแก้ (fwang_seek_gate เปิด/ปิดเส้นทางต่อ frame)**:
  สะอาด = OK 960 + 369 480 (Tesla loop = skip boundary ที่ตั้งใจ)
  · edge ถูกแก้ในชั้นเก็บ (chord คู่พัง) = TAMPER → ปิด
  · edge ขาด (ผ่าน tamper แต่ continuity แตก) = MISMATCH → ปิด
  · deterministic — enc เดียว → คำตอบเดียวเสมอ
- **Tantrix (lc_tantrix.h) = fabric switch 1 byte = 1 routing instruction**
  (252 normal + 4 special NULL/CROSS/MERGE/SPLIT):
  gate ตรง → FORWARD · ไม่ตรง → DROP (ปิดเส้นทาง!) · CROSS = invert (cross_map
  {1,0,3,2}) · SPLIT = broadcast · MERGE · SKIP/MIRROR class เปลี่ยน exit ·
  tantrix_connects = Wang edge match (exit(left)==entry(right))
- **spoke_mask เชื่อม cylinder §15.49**: spoke 0→0x09=(0,3) · 1→0x12=(1,4) ·
  2→0x24=(2,5) · 3→0x3F=ทั้ง 6 — invert pairs ตรง geo_spoke_invert เป๊ะ
- **CHAIN พิสูจน์แล้ว**: seek → wang gate (integrity: เปิด/ปิด) → tantrix
  route (direction: forward/drop) — ข้อมูลดี = เปิดทั้งคู่ · ข้อมูลเสีย =
  ปิดที่ชั้น wang — 29/29 PASS (tests/test_wang_tantrix.c, TIER1)
- **บทเรียนซ้ำ**: เทสต์ไม่ได้รัน ≠ ใช้ได้ — wang layer roundtrip ตัวเอง
  "ผ่าน" แต่ invariant พังโดย construction (เหมือน drift TW §15.x) —
  verify ที่รันจริงจับได้ทันที (fwang_verify -2/-5) — make test = 78/78 + 4/4

**§15.51 — Candidate → hyperbolic role map (user: "เอามาเสนอ candidates ให้ทำงานกับ hyperbolic")**
- **KEY INSIGHT (พิสูจน์ 12/12, make hyp_candidate_map)**:
  - สนาม 20736 = 6 cylinders (§15.49) = **3 hyperbolic axes × 2 halves**
  - HYP_AXIS_SLOTS = 6912 = 2 × 3456 → 2 cylinders ต่อ axis
  - **HYP_INFINITY_IDX = 3456 = 1 cylinder เป๊ะ** = KIS half ของ axis
  - axis band [0,3456) = cylinder KIS (positive) · [3456,6912) = cylinder
    hyperbolic (mirror) — **hyperbolic side = กระจก cylinder ของแต่ละ axis**
    (Cayley transform = mirror map ระหว่าง 2 halves)
  - ∀ slot: (axis, half, spoke, slot_in_spoke) = bijection ครบ 20736
  - KIS = hyp = 10368 = ครึ่งสนาม · glass pairing a_w×a_{w+72}≡1 (ALL 144 w)
- **CANDIDATE → LAYER map (ที่เจอทั้งวัน วางลง hyperbolic side)**:
  | candidate | hyperbolic layer | กลไก |
  |---|---|---|
  | RDH (geo_rdh_addr) | log addressing | bond_key จาก (block,from_scale) — 5.1 cyc, reversible |
  | L-block | resume/placement หลัง lift | bookmark: orientation+เพื่อนบ้านจาก address ล้วน |
  | GeoSeed 2-register | identity ของ lifted block | face_id 12 หน้า (shift+mask ~2 cyc) |
  | geo_seed 12-coset | identity signature (optional) | 12 checksums topology-aware |
  | Hosoya/F-ladder | weight ladder | F(n)≈φⁿ = scale ladder (144=F(12)) |
  | cylinder spoke | spatial route ของ log entry | spoke=60° wedge · slot=radial · face=axial |
  | invert (spoke+3)%6 | glass mirror 2 halves | ฝั่งตรงข้าม = hyperbolic (เก็บ delta) |
  | wang edge | integrity ของ scale-log | edge_bot[w]==edge_top[w+1] → order valid |
  | tantrix | routing decision | gate ตรง=FORWARD · ไม่ตรง=DROP (ปิดเส้นทาง) |
  | geo_seek_gate | read-path router | Chord>Tantrix>RDH>Teleport>Frame |
  | Morton/RDH fast | hot-path addressing | shift+mask ไม่มี divide |
  | fibo clock/frame | timeline ของ log (มีอยู่แล้ว) | (round,tick) stride-37 · checkpoint-replay |
  | geomatrix (era) | ANTI-pattern → กฎ | อย่าหา inverse — บันทึก + rollback |
- **Composition พิสูจน์แล้ว (probe E)**: scale-event log (120 events) →
  wang gate เปิดหมด (integrity) → tantrix ตัดสินใจ forward/drop ครบ
  — deterministic จาก address ล้วน ไม่มี state
- **หลักการคัดเลือก**: ตัวที่ "เก่งในสนามที่มีรูปทรงเดียวกับมัน" (RDH) อยู่ที่
  addressing · ตัวที่เป็น switch (wang/tantrix) อยู่ที่ integrity+routing ·
  ตัว identity (GeoSeed) อยู่ที่ bond · ตัวที่ hash-like (geomatrix) = กันออก
- make test ยังเขียว 78/78 + 4/4

**§15.52 — Section Fusion: hyp_fusion.h (user: "merge/fusion เป็น section ได้ไหม — เลือกที่ดีแล้วไม่ฉุดกำลัง")**
- user กังวล: ถ้าแยกกันทุกตัวดีหมดแต่ต้องเลือก — ตัวที่ไม่เลือก = dead code
  (อย่าง wang ที่พังเพราะไม่มีใครรัน) — ต้องการ fusion ที่ไม่ฉุดกำลัง
- **หลักการ fusion**: แต่ละ SECTION = หนึ่งหน้าที่ ใช้ candidate ที่เร็วสุดตัวเดียว
  · section include ของเดิมที่พิสูจน์แล้ว (ไม่ duplicate logic)
  · static inline ทั้งหมด → compiler inline → ไม่มี runtime overhead
  · decision/verify เดียวต่อ section แทน N module แยก
- **S1 ADDRESS** (hyp_bond/hyp_route/hyp_mirror/hyp_face):
  - bond = rdh_bond_key(block, from) | face_id(GeoSeed)<<48 — identity
    (block, face, from_scale) อยู่ใน address — reversible (กู้กลับครบ)
  - hyp_route: slot → (axis, half, spoke, slot_in_spoke) bijection 20736
  - hyp_mirror: คร่อม half (KIS↔hyp) axis เดิม, mirror²=id
- **S2 GATE** (hyp_gate/hyp_log_validate):
  - หนึ่ง decision แทน (fwang_seek_gate + tantrix_route) 2 ชั้น state:
    tamper(ชั้นเก็บ) → 369 → edge continuity → entry match (tantrix DROP)
    = OPEN/SKIP/CLOSED/TAMPER
  - hyp_log_validate: 1 loop ตรวจครบ (แทน 6 loop ของ fwang_verify)
  - **rdtsc min-of-9: fused=21 cyc/N vs แยก=26-28 — fusion เร็วขึ้น ~25%**
    (ตอบ "ไม่ฉุดกำลัง" ด้วยตัวเลข — methodology เดียวกับ rdh_bench §15.40)
- **S3 WEIGHT** (hyp_fibo/hyp_hosoya/hyp_weight): F(12)=144 · H(6,2)=10 ตรงตาราง
- **CHAIN พิสูจน์**: scale-event log → hyp_bond append → gate เปิดตลอด →
  replay (block,face,from) → bond เดิมเป๊ะ — deterministic
- tests/test_hyp_fusion.c 18/18 (เสถียร 3 runs) · make test = 79/79 + 4/4
- **บทเรียน**: rdtsc ครั้งแรก flaky (63 vs 29 = alignment noise) — min-of-9
  แก้ให้เสถียร (21 vs 26-28) — วัดต้องตรวจ methodology ซ้ำ (บทเรียน rdh_bench)

**§15.53 — Bond มาจาก tetris: a[1]b[2]b[3]a (user เล่า origin ของ bond)**
- user: "bond มาจาก tertis  a[1]b[2]b[3]a
  a = external bond ต่อกับคนอื่นได้ใน topology เดียวกัน
  b = มีแค่คู่เดียวในโลก ต่อกับใครไม่ได้เลย"
- **ยืนยันจากโค้ด**: core/pogls_bond.h — POGLS_AXIS_SHAPE[1..7] = I O T S Z L J
  (tetrominoes! I=pipe O=latch T=splitter S=transpose Z=invert L=fork-left
  J=fork-right) — ghost_fold_axis(to_scale) ∈ 1..7 — ทุก route มีชิ้น tetris
- **bond = 2 ชนิด (เกิดมาพร้อมกัน — "ต่อกับใครก็ได้ + ต่อกับคู่ตัวเองเท่านั้น")**:
  - **b-bond (private) = birth identity = ghost_bond_key(block, from_scale)**
    — มีคู่เดียวในโลก: (block,from) เดียว → key เดียว · เปลี่ยน block/from →
    key เปลี่ยน → bond แตก (self-enforcing: "If coordinate shifts → bond
    key changes → bond invalid automatically" — pogls_bond.h)
    = เสาเข็มห้ามขยับ · to_scale ต่าง → bond เดียว (identity แยกจาก route)
  - **a-bond (external) = topology membership** — ต่อกับใครก็ได้ใน
    topology เดียวกัน: mirror คร่อม half แต่ axis เดียวกัน = ต่อได้ ·
    ต่าง axis = คนละ topology = ต่อไม่ได้ (ต้อง bond ใหม่)
- **signature a[1]b[2]b[3]a** = tetromino 4 เซลล์: ปลาย external 2 +
  กลาง private 2 — b ต่อกับ b ด้วยกันไม่ได้ (แต่ละ b มีคู่เดียว อยู่ที่อื่น)
- **mapping กับ fusion (§15.52)**: hyp_bond = RDH(block,from) | face<<48 =
  b-bond (unique pair) + face (a-bond ระดับ topology) — ครบทั้ง 2 ชนิด
- probe: tools/bond_tetris_probe.c 13/13 (make bond_tetris) · suite เขียว

**§15.54 — b-bond: chunk ไม่ต้องรันเลข (user: "เพราะมีได้แค่คู่เดียว")**
- user: "มันทำให้ chunk ไม่ต้องรันเลขไง เพราะมีได้แค่คู่เดียว"
- **หลักการ**: b-bond = (block_id, from_scale) มีคู่เดียวในโลก (§15.53) →
  resolution เป็น identity ไม่ใช่การค้น: กู้ (block, from) จาก bond ด้วย
  เลขคณิตตรงๆ (div/mod = 2 op) — ไม่ต้อง hash ไม่ต้อง scan ไม่ต้องเทียบ
- **พิสูจน์ (tools/bond_direct_resolve.c 6/6, make bond_direct)**:
  - A: bond → (block, from) กู้กลับด้วยเลขคณิตทุกคู่ (bad=0)
  - B: sweep 4096×256 keys ไม่ชน (ไม่มี ambiguity → ไม่มีอะไรต้องคำนวณเพื่อหา)
  - C: **direct = 5 cyc vs ghost_log_find (linear scan) = 1,387 cyc
    (~277×)** — scan เฉลี่ย 250 comparisons/find บน log 1000 routes
  - D: freeze-once — ghost_lift เก็บ data ครั้งเดียวต่อ (block, from);
    routes หลาย to_scale แชร์ frozen data เดียว (chunk ↔ bond = 1:1)
- **audit ที่ซื่อสัตย์ — โค้ดปัจจุบันยัง "รันเลข" อยู่ 2 จุดที่ b-bond ควรกำจัด**:
  1. ghost_log_find / ghost_route_count = linear scan (O(n) เทียบ)
  2. residual_space _rs_hash (open addressing) — cache hash (§15.41 ยอมรับไว้)
  - b-bond ควรให้: data (unique pair) → direct slot (เลขคณิต 5 cyc)
    เหลือแค่ route check (a-bond) ที่ต้องดู log — นี่คือ upgrade ถัดไป
- make test ยังเขียว 79/79 + 4/4

**§15.55 — ปรับ ghost log: binary search + wang gate (user: "ปรับได้เลย ก่อนหน้านี้ด้วย ที่ใช้ wang")**
- ปรับตาม b-bond (§15.54 — chunk ไม่ต้องรันเลข) + wire fusion S2 เข้า chain จริง:
  **1. ghost_log_find/ghost_route_count: linear scan → binary search**
     - entries เก็บ SORTED โดย (block_id, from_scale) (b-bond: คู่เดียวในโลก)
     - find: binary search ถึง pile แล้ว scan เฉพาะ routes ของ pile (เล็ก)
     - route_count: ช่วง [lower(block,0), lower(block+1,0)) — O(log n)
     - ghost_lift: sorted insert (memmove ≤ 4096×5B — lift หายาก, read บ่อย)
     - ghost_expire: binary search pile แล้ว mark — semantics เดิมเป๊ะ
  **2. ghost_read: hyp_gate (wang integrity + tamper) guard — เส้นทางต้องเปิด**
     - enc = (origin_seed + to_scale) % 1440 → hyp_gate(CLOSED/TAMPER → NULL)
     - timeline เสีย → read ถูกปิด (T8: แก้ window ของ route แล้ว read=NULL)
     - GhostLog ฝัง FrameWangLayer (fwang_init ใน init/load — deterministic)
- **วัด (บน log 3,599 routes, min-of-9 rdtsc)**:
  - binary search find = **239 cyc** (12 comparisons + pile scan)
  - linear scan เดิม = ~1,800 comparisons ≈ **10,000+ cyc**
  - data resolution (b-bond เลขคณิต) = **5 cyc** — ยังเป็น O(1) (§15.54)
- **เทสต์ใหม่ tests/test_ghost_direct.c 18/18 (TIER1)**:
  find/route_count เทียบ brute force ครบ · sorted invariant ·
  expire กับ sorted · wang gate ปิด read (T8) · persistence roundtrip +
  wang rebuilt หลัง load · เชนเดิม intact (lossless)
- make test = **80/80 + 4/4** (test_ghost_lift/rs_persist/fibo_checkpoint/cap_chain
  ทั้งหมดยังผ่าน — semantics ไม่เปลี่ยน)
- **ซื่อสัตย์**: route check = a-bond (หลายค่า) → O(log n) 239 cyc (ไม่ใช่ 5 cyc —
  นั่นคือ data/b-bond); O(1) เต็มต้อง dense pair table (memory trade — deferred)

**§15.56 — wang gate บน checkpoint image จริง + dense pair table O(1) (2026-08-17)**

## Task 1 — wang gate บน fibo checkpoint image จริง

user: "เอา wang gate ไปใช้กับ checkpoint image จริง (fibo checkpoint):
ก่อน replay ตรวจ wang edges ทั้ง log — จับ corrupted checkpoint ได้ก่อน decode"

**`core/ckpt_wang.h` (ใหม่)** — wang-flavored digest ของ ghost log ต่อ window (= 12 entries,
สมมาตร wang WANG_WIN_SIZE): 8B/window = edge_top/top_b + edge_bot/bot_b (chord 2&7 บน 9-clock,
edge_bot = ค่าที่ boundary ตามบทเรียน §15.50) + parity (XOR enc) + n369 (Tesla markers)

```
checkpoint image: header(28) + ghost log + wang digest + residual space
load:  ghost_log_load → ckpt_wang_check(digest) → ผ่านแล้วค่อย rs_load (ก่อน decode!)
```

- `ckpt_wang_digest` / `ckpt_wang_verify` (recompute ต่อ window เทียบ) / `ckpt_wang_scan`
  (hyp_gate ทุก entry — timeline ต้องเปิด) / `ckpt_wang_check` (ตัวเดียวที่เรียกก่อน replay)
- **test_ckpt_wang 22/22** (TIER1): digest structure · clean+lossless · corrupt 1 byte ใน log
  reject หลายตำแหน่ง · corrupt digest เอง/ที่ window boundary reject · edge cases 0/1/13/25
  entries · flow เต็ม: image → load → corrupt in-entry → reject ก่อน decode
- **test_fibo_checkpoint 23/23** (อัปเกรด): image จริงตอนนี้ = header + log + wang digest (48B
  สำหรับ 66 routes — 0.018% ของ image) + rs; reload ตรวจ digest+scan ก่อน rs_load; corrupt
  to_scale ของ entry แรกใน image → reject (r≠0)

## 🐛 Bug 2 จุดที่เจอระหว่างทำ

1. **bond 0 collides with RS_BOND_KEY_RESERVED**: rdh_addr(0,0)=0 → bond_key=0 = sentinel
   "no entry" → ghost_lift(block 0, from 0) ล้ม (rs_freeze เก็บแล้วคืน 0 → lift ตีความผิด)
   — FIX: `ghost_piece` offset bond +1 (bond_L = a+1, bond_R = (a+1)<<24 — bijection คงอยู่
   [1,2^24], bond 0 กลายเป็นค่าว่าง) — (block,from) → bond เดียว ไม่ชน ไม่มี hash
2. **fibo_checkpoint "ผ่าน" แบบเงียบ**: place fail → `return 0` → main ไม่ตรวจ → exit 0 →
   make test นับ green ทั้งที่ C-F ไม่ได้รัน — FIX: return -1 + main นับ fail → 23/23 จริง

## Task 2 — dense pair table: route check O(1)

user: "ต่อยอด route check เป็น O(1: dense pair table (block,from) → pile slot แบบ direct
(memory trade) — วัด footprint จริงบน 4 โมเดล GGUF ว่าแพงแค่ไหน"

**`GhostPairTable` (ใน geo_ghost_lift.h)** — entries sorted โดย (block, from) → ตาราง dense 2 ตัว:
```
pile[(b<<8)|f]   = index ของ entry แรกของ pile (b,f) หรือ 0xFFFF  (max_block×256×2B)
block_lo[b]      = index ของ entry แรกที่มี block_id >= b          ((max_block+1)×2B)
route_count(b)   = block_lo[b+1] - block_lo[b]   — O(1) ตรงๆ
find(b,f,t)      = pile lookup → scan เฉพาะ pile (เล็ก) — O(1)
```
- build O(count) หนึ่งรอบ · `ghost_pair_fresh` = stale check (lift/expire เปลี่ยน count →
  ต้อง rebuild — จับได้ ไม่มี stale read) · free ครบ
- **test_pair_table 15/15** (TIER1): correctness เทียบ brute force + binary search ทุกจุด ·
  cost: **pair find 5 cyc vs binary 48 cyc (9.6×) · pair route_count 5 cyc vs 73 cyc (14.6×)**
  บน log 3,600 routes · staleness + rebuild ตรงอีกครั้ง · footprint จริง 4 GGUF

## 📦 Footprint จริงบน 4 GGUF (memory trade ตอบคำถาม user)

| model | tensors | pair table | log (5B/route) | ratio |
|---|---|---|---|---|
| SmolLM2-360M | 290 | 149,062 B | 1,462 B | 102× |
| Qwen3-0.6B | 310 | 159,342 B | 1,562 B | 102× |
| LFM2.5-2.6B | 266 | 136,726 B | 1,342 B | 102× |
| Qwen2.5-0.5B | 291 | 149,576 B | 1,467 B | 102× |

**~580 KB ทั้ง 4 โมเดล** (< 1/10 ของ field window หนึ่ง = 20736×4KB = 81MB) — table เล็กกว่า
1 window ถึง ~140× — แต่ละ block ยืน 514B (256×2 + 2) — verdict: **คุ้ม** ถ้า route check
ร้อน (read บ่อย): 9.6-14.6× เร็วขึ้นในราคาที่เล็กกว่า window เดียวของสนาม

make test = **81/81 + 4/4** · §15.55 ค้าง "O(1) ต้อง dense pair table (deferred)" → **ปิดแล้ว**

**§15.57 — pair table auto-refresh: dirty flag + lazy rebuild (§15.56 ต่อยอด) (2026-08-17)**

user: "Make the dense pair table rebuild automatically inside ghost_lift/ghost_expire
(dirty flag + lazy refresh) instead of requiring the caller to call ghost_pair_build,
keeping reads O(1) with no stale risk"

## API เปลี่ยน: จากตารางนอก → attach เข้า log

```
ก่อน:  GhostPairTable t; ghost_pair_build(&log, &t);       ← caller ต้อง build เอง
       ghost_pair_find(&log, &t, b, f, to)                ← จำ table เอง ทุก call
หลัง:  ghost_pair_attach(&log, &t)                        ← attach ครั้งเดียว
       ghost_pair_find(&log, b, f, to)                   ← log-centric — ไม่ต้อง table
```

**กลไก: dirty flag + lazy refresh**
- `GhostLog` มีช่อง `GhostPairTable *pair` (attach/detach) — init/load = NULL (ต้อง attach ใหม่หลัง load)
- `ghost_lift` / `ghost_expire` → ตั้ง `pair->dirty = 1` (O(1) — ไม่ build ทันที)
- read (`ghost_pair_find` / `ghost_pair_route_count` / `ghost_read` เมื่อ attach)
  → `ghost_pair_refresh` rebuild ถ้า dirty/ยังไม่เคยสร้าง (O(count) เฉพาะตอน dirty)
- `ghost_read` ใช้ pair table อัตโนมัติเมื่อ attach (O(1)), fallback binary search เมื่อไม่ attach

**พิสูจน์ (test_pair_table 21/21 — เพิ่ม section C auto-refresh):**
- attach → dirty (ยังไม่ build) → read ครั้งแรก refresh เอง + เจอ route เก่า
- lift → dirty ตั้งอัตโนมัติ → read เจอ route ใหม่ **โดยไม่ต้องเรียก build** (ไม่มี stale read)
- expire → dirty → read ปิดเส้นทาง (route ตาย) + audit trail ยังนับ
- detach → ghost_read กลับ binary fallback ยังถูก
- cost (fresh table): pair find **6 cyc vs binary 46 cyc (7.7×)** · route_count **6 vs 70 (11.7×)**

**semantics ที่เปลี่ยน:** `ghost_read` signature `const GhostLog*` → `GhostLog*` (refresh
ต้อง mutate table) — caller ทั้งหมดส่ง &log (mutable) อยู่แล้ว ไม่กระทบ

make test = **81/81 + 4/4** · caller ไม่ต้องจำ lifecycle ของตารางอีก — attach แล้วจบ

**§15.58 — วัด lazy refresh จริงบน 7.7GB stream: rebuild cost ถูกครอบโดย dense-table memset (2026-08-17)**

user: "Measure the real cost of lazy refresh under write-heavy load: interleave
ghost_lift with ghost_read on the 7.7GB notebookLM stream with the pair table
attached, and report how many rebuilds happen, their total cost, and the read
latency distribution vs detached mode"

เครื่องมือใหม่ `tools/pair_refresh_scan.c` (`make pair_scan`) — stream จริง
(เดียวกับ cap_chain_scan): chunk 16KB → w=(37·rank)%144 → cap_admit → ghost_lift
→ interleave BATCH lifts → BATCH reads, 2 โหมดต่อ file (A attach / B detach),
rdtsc ต่อ read + wall-time + lossless verify

## ตัวเลขจริงบน F:/notebookLM (7.7GB, 1,035 files, 476,763 lifts, 476,763 reads)

| batch | rebuilds | % ของ reads | total rebuild cost (rdtsc) | wall Δ A−B |
|---|---|---|---|---|
| 4  | 124,568 | 26.1% | 87.8 s | (A หนักสุด) |
| 16 | 31,322  | 6.6%  | 20.3 s | +95 s |
| 64 | 8,022   | 1.7%  | 5.2 s  | +79 s |

**lossless 1035/1035 ทั้ง A และ B ทุกรอบ** — lazy refresh ไม่เคยให้ stale read

## 🔑 สิ่งที่วัดพบ — cost ตัวจริงไม่ใช่ rebuild loop แต่เป็น **dense-table memset**

- rebuild read p50 = **1.77M cycles** (batch 4) — แต่ fast read p50 = 448 cyc
  → rebuild หนัก ~3,900× ของ read ปกติ
- สาเหตุ: `ghost_pair_build` ทำ `memset(t->pile, 0xFF, max_block×256×2B)`
  — และ **max_block = chunk index** (block_id = i ในไฟล์) → ไฟล์ 1GB
  (65K chunks) → table = 65,536×256×2 = **32MB ต่อ rebuild**!
- memory trade กลับด้าน: table ใหญ่ตาม max_block ไม่ใช่ตามจำนวน blocks ที่ใช้จริง
  — chunk สุดท้ายของไฟล์ใหญ่ = จ่าย table เต็มทุก rebuild

## 🎛️ Batch = ปุ่มควบคุมอัตราส่วน (ถูกต้องตาม design)

batch 4 → ทุก 4 lifts มี 1 rebuild (26%) · batch 64 → ทุก 64 lifts มี 1 (1.7%)
— ใช้ได้: ถ้า workload อ่านรวมหลังเขียน (verify phase เดียวต่อ window อย่างใน
cap_chain_scan) → rebuild แค่ 1 ครั้งต่อ window = ไม่มี cost จริง

## 📌 สรุปที่ซื่อสัตย์

1. **lazy refresh ทำงานถูก** — 1035/1035 lossless ทั้งสองโหมด ไม่มี stale
2. **ใน stream regime นี้ (log ≤ 1024 entries ต่อ window) binary search ชนะ
   ตารางด้วยซ้ำ**: A-fast p50 448 cyc vs B p50 276 cyc (batch 4) — เพราะ
   (a) dense table ใหญ่ตาม max_block (สูงสุด 32MB) ไม่ fit cache → fast read
   ก็เจอ cache miss (b) log ต่อ window เล็ก (≤ 1024) → binary search แค่ ~10
   เปรียบเทียบ — ตารางจะชนะเฉพาะเมื่อ log โตมาก (หลายพัน entries) + table เล็ก
   (ที่ benchmark สั้นๆ batch-4 บน 3 ไฟล์เล็ก table 4KB → A 150 < B 228 cyc ✓)
3. **rebuild เองแพงเพราะ memset ของ dense table ที่ใหญ่ตาม max_block**
   — ไม่ใช่เพราะ rebuild loop (O(count) เล็ก) — นี่คือ memory trade ที่ต้องจ่าย
   จริง และทางแก้คือขนาด table ตาม blocks ที่ใช้จริง (sparse) ไม่ใช่ max_block
4. batch ใหญ่ขึ้น = rebuild ถี่น้อยลง — อัตราส่วนถูกควบคุมได้ที่ caller
5. **verdict: dense pair table แพ้ binary search ใน workload แบบนี้**
   (streaming, window เล็ก, log ต่อ window สั้น) — คุ้มเฉพาะ read-heavy +
   log สะสมใหญ่ — ทางเลือก: sparse table ตาม distinct blocks หรือข้ามไปใช้
   binary search ต่อ (46 cyc วัด §15.56) เมื่อ window ≤ 1024

make test = **81/81 + 4/4** (ไม่กระทบ TIER1 — เครื่องมือ manual ใหม่)

**§15.59 — geometry signal ก่อน compute: history signal กำจัด rebuild cliff (2026-08-17)**

User: "geometry มักมีเหตุการณ์แบบนี้ มีสัญญาณบางอย่างมาเร็วกว่า compute" — ตรงกับที่วัดได้ §15.58: ราคา rebuild คำนวณได้ O(1) ก่อนจ่าย (dirty flag + max_block + count มีอยู่แล้ว) → เอา signal ไปตัดสินใจใน `ghost_pair_refresh` ก่อน build

**กลไก (ใน geo_ghost_lift.h):**
- `hint_max_block` ตั้ง O(1) ตอน ghost_lift (ไม่ scan) + `reads_served` นับ read ทุก path (table + fallback)
- กฎตัดสินใจก่อน rebuild: pred = max_block×512B · คุ้มเมื่อ pred ≤ 512×count และ (reads_served ≥ pred/2048) — ถ้าไม่คุ้ม คืน 1 = fallback binary (ถูกเสมอ — ตารางเก่ายัง valid หรือ binary ก็ correct)
- attach ครั้งเดียว → dirty/lazy/auto-skip ทั้งหมดในตัว — caller ไม่ต้องรู้

**วัดจริง 7.7GB (1,035 files, 476,763 lifts, 476,763 reads, batch 4 — กรณีแย่สุด §15.58):**
- rebuild cost: **87.8 s → 177.7 ms (~494×)** — skip 205,983 ครั้ง (dirty แต่ไม่ build — binary แทน)
- lossless 1035/1035 ทั้ง attach และ detached (byte-for-byte)
- **wall Δ พิสูจน์แล้วเป็น cache noise**: รัน parity กลับกัน (ไฟล์คี่ A-ก่อน) → Δ พลิกเครื่องหมายเป๊ะ (+21.8s → −21.1s) — ต้นตอคือ cold read ของไฟล์ใหญ่ ไม่ใช่ตาราง — ส่วนที่เหลือจริงๆ: read A 3.50s vs B 3.07s (Δ 0.43s จาก skip-path read) + rebuild 170ms

**บทเรียน:** signal มาก่อน compute ได้เมื่อ state ที่จำเป็น (dirty/size/count) ถูกสะสม O(1) ระหว่างทาง — เหมือน geometry ที่ mask รู้ก่อนเดิน; พลิกจาก "จ่าย rebuild ทุกครั้ง" เป็น "จ่ายเมื่อคุ้ม" โดยไม่เสีย correctness (fallback ถูกเสมอ)
