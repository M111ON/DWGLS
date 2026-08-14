# Timeline Working Model — แบบจำลองการทำงานฉบับสมบูรณ์

> **สถานะ:** ทุกกลไกในเอกสารนี้มี test + ตัวเลขกำกับ (TIER1 35/35 เขียว)
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
