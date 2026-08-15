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
6. **Boundary — จุดอันตราย/กำไรที่ยังเปิด (2026-08-15):** วางข้อมูลที่ scale ขยาย (w₀+k) แล้วอ่านกลับที่ base → view หด 2ᵏ (base-2 contraction ต่อสเกล) = "compression มากกว่าปกติ" โดย lossless (replay กลับบ้านเกิด). **แต่** เสาเข็มที่วางลึกสำรองพื้นที่จริงใหญ่กว่า view ที่โชว์ — ระบบนับ capacity ที่ base (1×) แต่ของจริง 4× (k=2) → **overcommitment แอบซ่อน** ถ้าไฟล์เยอะ+ต้องขยายพร้อมกัน → ชนขอบ → cascade. **Boundary ยังไม่เป็นชั้น first-class:** เสาเข็มต้องประกาศ envelope `(w₀, depth k, ขนาดที่ w₀+k)`, capacity = Σ envelope ≤ 20736, เกิน = reject deterministic (ไม่ silent). นโยบาย = ค่าคงที่ `MAX_EXPANSION_DEPTH` (0 = ห้าม, k = ทำได้พอประมาณ) — **ยังไม่ตัดสินใจ**
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
