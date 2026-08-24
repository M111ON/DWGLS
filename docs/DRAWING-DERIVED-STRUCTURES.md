# Drawing-Derived Structures — ภาพวาดผู้ใช้เป็น Proof Sketch อิสระ
## 2026-08-24 · branch feat/geo-native-fs

> **หลักการของเอกสารนี้:** ภาพวาดของผู้ใช้ไม่ใช่ inspiration — เป็น **derivation path อิสระ**
> ที่บรรจบกับ code โดยไม่เคยเห็นกัน (เทียบเท่า oracle test สองทาง: sketch ↔ implementation
> ถึงผลลัพธ์เลขเดียวกัน = น่าเชื่อถือกว่าทางเดียว)
>
> ทุกตัวเลขในเอกสารนี้ decode จากไฟล์จริง (GeoGebra XML / SVG paths) หรือ verify ด้วย script
> — ไม่มี expected แบบ circular

---

## สาย 1 — Zigzag ⋀⋁ → Pascal (ภาษา 7)

**ต้นทาง:** สเก็ตช์ 8 ส.ค. — `⋀⋁⋀⋁` บนราง `_ _ _` (endless 2-state parity flip)

**ปลายทาง:** `tools/pascal_zigzag_probe.c` — zig-zag diagonal ของสามเหลี่ยม Pascal
อ่านเป็น stream → identity `A(n) = Σ(−1)^k C(n−k,k)` **period-6**

- bijection ครบ + mutation red + roundtrip lossless บน GGUF 675.7MB (`76aee0c`)
- ชื่อ probe ใน repo มาจากภาพสเก็ตช์ตรงๆ (zigzag)

---

## สาย 2 — Hexagram Rhombus Grammar (ภาษา 8)

**Grammar canonical (ผู้ใช้กำหนด):** หน่วยนับ = **rhombus (ρ)**

| หน่วย | tri | ρ | ความหมาย |
|---|---|---|---|
| 1 hexagon | 6 | **3** | = cube projection (3 หน้าเห็น) |
| 1 hexagram | 12 | **6** | "rhombus 6 อัน ประกอบกลับเป็น hexagon" |
| hexagram + 6ρ | 24 | 12 | → hexagon side-2 (**19 cells**) |

**Fold bijection (verify int-only แล้ว):**
- hexagram 13 cells → tips (dist-2 alternate) fold เข้า notches (rotational pairing)
- ทุก tip–notch pair ระยะ axial เท่ากัน 6/6 → refold solution เดียว (เหมือน snub parity)
- folded = 13 cells; complement หมุน 30° ให้ hexagon-19; holes หลัง fold = ยอดเดิม
- candidate runtime toggle ต่อ sector (bit ต่อ ρ) — คู่ fold/unfold แบบ enantiomorph

**Metatron linkage:** 13 cells = Metatron topology (1+6+6) · solid-many-in-one-carrier
= RID window ที่ซ่อน pent/tri/snub/hosoya/zeck/pascal · 13 = F(7) = stride hosoya (ภาษา 5↔8 แชร์เลข)
· Z-axis ใน `hyperbolic_seek.h` ตั้งชื่อ METATRON — ตอนนี้มีเหตุผลรองรับครั้งแรก

---

## สาย 3 — Seven Construction = Index Gate

**ต้นทาง:** GeoGebra `hextess.html` — วาด hexagon 7 อัน (side=4 เท่ากันหมด)

**Decode จาก XML (30 จุด + 1 dependent = 31 unique):**

| ค่า | ผลวัด |
|---|---|
| hexagons | 7 · area 41.569 (= 6 unit-tri) ต่ออัน |
| unique vertices | **31 = 2⁵−1** (Mersenne โผล่จาก construction ล้วน) |
| shared vertices | 8 · มี triple-points **3 จุด (C,D,G)** ที่ 3 hexagons มาเจอกัน |
| shared edges | 1 (D–E) — ที่เหลือทับซ้อนผ่านกัน |
| centers | เรียง 2–3–2 บน hex lattice (spacing = side ⇒ forced overlap) |

**บทบาท 3 ระดับ (ผู้ใช้อธิบาย):**

| บทบาท | จำนวน | บนกระดาษ | ความหมาย |
|---|---|---|---|
| โครงจริง | 6 | ✅ | cells ที่ projection แยกได้ |
| โดนทับ (occluded) | 1 | ❌ | จอง address ไว้ มุมมองมองไม่เห็น |
| เติมเส้น (syntax) | 1 | ✅ | ปิดรอยตัด = completeness lock |
| **รวม** | **8** | วาด 7 | = tesseract cell |

**Isomorphism กับ `core/geo_tess_wiring.h`:**

| | ภาพวาด | code |
|---|---|---|
| gate/lock | hexagon กลาง (syntax) | **cube 0 = index frame** |
| ที่ gate ถือ | เส้นทางไปหาทุก cell | base[8]/len[8]/stride[8] slot 0..23 |
| route ออก | 7 ตำแหน่งจริง | cubes 1..7 (1008 slots) |
| เดินครบ | เส้นตัดผ่านครบ | stride-37 coprime 144 |
| dual-status element | นับใน 8 แต่เป็นกุญแจของ 7 | cube 0 นับใน 8 cubes, payload = metadata |

Timeline: สเก็ตช์ 8 ส.ค. → rescope index-frame 14 ส.ค. → proven `test_tess_index_frame` 7/7
— **ภาพวาดนำโค้ด ~6 วัน**

---

## สาย 4 — Ring-24 Geometric Gearbox

**ต้นทาง:** SVG (24-gon + chords) + คำอธิบาย Gemini (แก้ bug แล้ว)

**Decode จาก SVG paths:**

| ชิ้น | ค่าวัดจริง |
|---|---|
| 24-gon | center (715.6, 448.7) r=417.4 · vertex step **15.0°** เป๊ะ |
| chords รวม | **216 = 6³** (step-1:168 · step-6:24 · step-8:24) |
| squares | step-6 → mod-6 classes = **6 อัน** ✓ |
| triangles | step-8 → mod-8 classes = **8 อัน** ✓ |
| hexagons | **วาดจาก segment ไม่ใช่ chord**: unit segments unique = **144**, candidate inward hexagons 24, fully-drawn **24/24** (ครบ 6 ด้านทุกอัน, shared sides = 0) |

### Double-144 (นับสองแบบ ได้เลขเดียว)

| นับ | การคำนวณ | ผล |
|---|---|---|
| strokes | 24 hex × 6 segments (no sharing) | **144** |
| area | 24 hex × 3ρ × 2tri | **144** |

144 = TESS_CELLS = 12² = 18 tess × 8 cube cells — **construction segment ล้วน ไม่มีตัวเลขในไฟล์
แล้วจบที่เลขหลักของระบบเอง**

### Gearbox properties (corrected)

1. **Divisor law:** gear = divisor d ∣ 24 · stride s → shape มี 24/s ด้าน · **count ≡ stride (self-dual)**:
   stride-8 → 8 triangles · stride-6 → 6 squares · stride-4 → 4 hexagons
   *(⚠️ correction ตาราง Gemini ล่าง: octagon=stride-3 ไม่ใช่ 8, hexagon=stride-4 ไม่ใช่ 6)*
2. **Prime basis force:** 24 = 2³×3 → divisor lattice {1,2,3,4,6,8,12,24} ⊂ {2ᵃ3ᵇ} ล้วน
   20-gon (มี 5) หรือ 28-gon (มี 7) พังทันที — **24 ถูก force โดย prime basis {2,3} ของบ้าน**
   · ratios 8×/6×/4×/2× ล้วน smooth = เข้า scale ladder ของ breathing
3. **มุมฟรีจาก concyclicity:** 105°+75°=180° = cyclic quadrilateral theorem (จุดบนวงใดๆ
   มุมตรงข้ามรวม 180°) · 90° = chord ครึ่งวง (12 steps) — **ไม่ต้อง config วัดไม่ได้ = วาดผิด**
   = free oracle
4. **Gear shift = scale change โดย bytes นิ่ง** = zero-copy relabel breathing (proven Aug 22,
   `exp/alt-scale-semantics`) — concept นี้มี implementation รองรับแล้ว
5. Caustic/envelope rings = intersection nodes ของ chord family (step-8: 137 nodes · step-6: 64)
   — anchor candidates, ต้อง probe int-cleanliness ก่อนใช้

### สถานะ honest

| claim | สถานะ |
|---|---|
| 24 hexagons fully drawn from segments, 144 strokes | ✅ measured จาก SVG |
| triangles 8 / squares 6 / chords 216 | ✅ measured |
| double-144 | ✅ counted two ways |
| divisor lattice ⊂ {2,3} | ✅ number theory |
| angles 105/75/90 concyclic-forced | ✅ theorem |
| caustic anchors เป็น address/routing | ⚠️ concept — ยังไม่ probe |
| load-balancing ผ่าน 105°/75° bias | ❌ ยังไม่มี mechanism |
| multi-model per gear | ❌ ทิศทาง Phase 4 — ยังไม่ proven |

---

## Grammar รวม (สาย 1–4)

```
segment ─2─▶ triangle ─2─▶ rhombus ─3─▶ hexagon(=cube proj.)
                                            │ ×24 ring (2³×3 carrier)
                                            ▼
              144 strokes = 144 tri = TESS_CELLS = 12²
                                            │
        20736 = 144²  ◀═══ สูตรโครงสร้างหลัก ════┘

zigzag ⋀⋁ (parity) ──▶ pascal stream     [ภาษา 7]
rhombus fold/unfold    ──▶ hexagram view  [ภาษา 8]
gate-1 : payload-7 : cells-8 ──▶ index frame (cube 0)
divisor gears on 24    ──▶ scale ladder (breathing-compatible)
```

**หลักร่วมทุกสาย:** projection อาจข้าม/ทับ cell — address space ไม่ข้าม
ภาพให้ syntax, address ให้ truth, ทุกจำนวน derive ได้ ไม่ต้องเก็บ

---

## Next probes (candidate queue)

1. `polygon24_probe.c` — 26 structures บน carrier เดียว (24 hex + 8 tri + 6 sq):
   closure + count≡stride + มุม integer degrees เป็น oracle · map เข้า 20736?
2. hexagram fold toggle — bit-per-rhombus runtime switch (คู่ fold/unfold)
3. caustic nodes int-cleanliness sweep (rational coordinate families?)
4. interop bridge — ฝัง construction protocol (GeoGebra/SVG grammar) เป็น metadata
   ให้ external reader decode ได้

## Sources

- `attachments/hextess.html` (GeoGebra construction protocol, 7 hexagons, ggbBase64 decoded)
- `composer_2026-08-24_12-58-26-142_2aaa4c.svg` (24-gon, 216 chords, 666 segments)
- สเก็ตช์ composer_2026-08-24_10-18-41 (`x2 · 1 cube : 8 mini cube = tesseract`)
- Session archive: hm_20260808_212331_d7af87 (zigzag/KIS{x,y,z}/tesseract discussion)
