---
luminaCreated: 2026-08-16T06:55:06.498Z
tags: []
luminaModified: 2026-08-16T06:55:06.498Z
luminaVersion: 1.3.11
---
# Tetra Field Structure — รวบรวมข้อมูล (เชื่อมทีละอย่าง)

> รวบรวมจากการสนทนา 2026-08-16: โครงสร้างของสนาม 20736 ที่ดู "แปลกจากที่เคยเห็นทั่วไป"
> พิสูจน์แล้ว: `test_tess_tetra_axis.c` 10/10 (TIER1 48/48) — ข้อมูลทุกชิ้นมีเลขกำกับ

---

## สายโซ่ — เชื่อมทีละอย่าง

### ① สมการราก: 128 × 162 = 144 × 144 = 20736

```c
// core/infra/gear_lock.h — เขียนไว้ตั้งแต่แรก
GEAR_CPU_WORLD = 128u   // compute  (2⁷)
GEAR_GPU_WORLD = 162u   // geometry (2·3⁴ = icosphere 2× base-3)
GEAR_GEO_FULL  = 128u * 162u   // = 20736
```

**compute × geometry = natural square grid** — ตัวเลขนี้อยู่ใน repo (`geo_bfs_hub.h`:
"Sacred: 20736 = 128×162 (gear) = 1728×12 (spine) = 144×144 (BMP blocks)")

### ② สอง ladder — สองโลกขนานในเลขตัวเดียว

```
20736 = 2⁸ × 3⁴ = 256 × 81
node  = hi·81 + lo     hi ∈ [0,256) = 2-power (binary floor)
                       lo ∈ [0,81)  = 3-power (Peano grid)
```

- โลก A (2-power): scale ×2/÷2, 4-subdivision — อาศัยบน hi-axis
- โลก B (3-power): Peano 3-adic, 3-subdivision — อาศัยบน lo-axis
- สองโลกขนานกัน ไม่ผสม: scale ไม่แตะกริด 3, กระโดด 3 ไม่แตะ scale 2
- บรรจบที่จุดเดียว: 20736 = 12⁴ (จุดตัดของข้อจำกัด ไม่ใช่เลขเลือก)

### ③ 16:9 = (4:3)² — ratio ถูก square โดย window 2 มิติ

```
4:3  = หนึ่ง 4-subdivision : หนึ่ง 3-subdivision  (ต่อ level-pair)
16:9 = (4:3)² = 4²:3²     ← window ยกกำลังสอง
144  = 12² = (4·3)²       ← ด้าน window
256:144 = 16:9 ✓          ← ต้องเป็น 2⁸ ถึงจะได้ square ของ 4:3 (2⁷ ให้ 8:9 ✗)
```

### ④ 3456 — เลขหน้าเดียว บทบาทหลายอย่าง

```
3456 = 2⁷·3³
     = 20736/6            ← hexagon count (hex-6: center = 6/6 share)
     = HYP_INFINITY_IDX   ← จุด infinity กลางแกน (hyperbolic_seek.h)
     = 6912/2             ← ครึ่งของ band ต่อแกน
3456 × 2 = 6912 = หนึ่งแกนของ KIS  ← "×2 ทั้งระบบ" rule
```

### ⑤ 3 แกน = 3 เส้นอนันต์ (ด้านของสามเหลี่ยม)

```
triangle grid → lattice มี 3 ทิศทางของเส้น (0°, 60°, 120°) — หนึ่งทิศต่อด้าน
แต่ละ edge = เส้นยาวอนันต์ — ไม่มีปลาย ไม่มี 0 ระหว่างทาง
เรา (ระบบ) = สามเหลี่ยม = จุดตัดของเส้น 3 เส้น (จุดยอด = ตัดทีละคู่: L1∩L2, L2∩L3, L3∩L1)
3 แกน = 3 ตระกูลเส้น — จำนวนแกน = จำนวนด้านของ cell (= 3 เพราะ cell เป็นสามเหลี่ยม)
```

### ⑥ สามเหลี่ยม = 1 หน้าของ regular tetrahedron

```
tetrahedron = จุดตัดของ 4 ระนาบ (3 แกน + 1 ด้านตรงข้าม)
4 ระนาบ → ตัดทีละคู่ C(4,2) = 6 ขอบ → ตัดทีละ 3 C(4,3) = 4 จุดยอด
V=4, E=6, F=4 — Euler V−E+F = 2 — ทุกจำนวน integer
12 = 4×3 = 4 หน้า × 3 ขอบ/หน้า = 6 ขอบ × 2 ทิศ = directed edges
12⁴ = 20736 — เลข 12 ใน 12⁴ คือโครงสร้างของ tetrahedron เอง
```

### ⑦ Tetra-axis walk — วิ่งบนสนามตาม 12 directed edges

```
orbit r = { r + 12k : k ∈ [0,1728) }   (stride-12, mod 20736)
12 orbits × 1728 = 20736 — partition พอดี
1728 = 12³ = TH_PENTAGON_NODES — แต่ละ orbit = 1 pentagon
cycle ปิด: จาก node ใดก็ได้ 1728 ก้าวกลับจุดเดิม — ไม่มี start ไม่มี 0
orbit r ↔ (vertex r/3, edge r%3) — 4 จุดยอด × 3 ขอบ
```

### ⑧ กฎที่ได้จากโครงสร้าง (ไม่ใช่กติกาที่ตั้งเอง)

```
- ไม่มีจุดเริ่มต้น ไม่มี 0 → ต้องเป็น cycle/torus (enter anywhere)
- depth ของ subdivision สูงสุด 2 (window-aligned) — ต่ำกว่านั้นเจอทศนิยม (13.5 = 81/6)
  → 13.5 ไม่ใช่ความผิดของสมการ แต่เป็นสัญญาณว่า "ไปไกลกว่าจุดเล็กสุด" (tetrahedron)
- จุดเล็กสุด integer = tetrahedron (4/6/4/12) — ลงต่ำกว่านี้ polytope ยัง integer
  แต่ tiling (hex) เริ่มไม่ลงตัว
```

### ⑨ ตาราง 12ᵏ — ทุกอย่าง collapse เป็นเลขเดียว

```
12⁰ = 1        ← cell หน่วย (จุดเล็กสุด)
12¹ = 12       ← tetrahedron: 4 หน้า × 3 ขอบ/หน้า = directed edges
12² = 144      ← ด้าน window (หนึ่งมิติของสนาม) = 4²·3² = 16·9
12³ = 1728     ← orbit ขนาด = 1 pentagon = FS_PIPES (fibo_spine) = 20736/12
12⁴ = 20736    ← สนามเต็ม = (12²)² = window²
```

**ทุก factorization ต่างกันของจำนวนเดียวกัน** — เหมือนเหรียญสองหน้า: square grid
(address) กับ tetrahedron (geometry) เป็นหน้าคนละด้านของ 20736:

```
128×162      = 20736     ← gear: compute × geometry  (gear_lock.h)
144×144      = 20736     ← natural square grid (w, pos) — address
12⁴ = (4·3)⁴ = 20736     ← tetrahedron (12 = 4×3) — geometry
12 × 12³     = 20736     ← tetra-axis walk: 12 orbits × 1728
2⁸·3⁴        = 20736     ← สอง ladder (hi·lo mixed radix)
3 × 6912     = 20736     ← 3 แกน × band (hyperbolic_seek)
6 × 3456     = 20736     ← hex-6 × hexagon count (= HYP_INFINITY_IDX)
```

ที่ "ตีตาราง" แล้วเห็น = เห็นว่า factor ทุกตัวลงตัวพอดีไม่มีเศษ (ที่ depth ≤ 2 ตามกฎ
⑧) — ไม่ใช่ "เลขหลายตัวที่บังเอิญเข้ากัน" แต่เป็น **จำนวนเดียวกันมองหลายมุม** —
สลับหน้าได้อิสระ เพราะ address กับ geometry เป็น factorization ของเลขเดียว

---

### ⑩ cycle walk = ไม่มี distance function — หลบ non-metric โดย construction (Nagy 2003)

> อ้างอิง: B. Nagy, "Shortest Paths in Triangular Grids with Neighbourhood Sequences",
> CIT 11 (2003) 111-122 — คณิตศาสตร์มาตรฐานของ triangular grid: 3 พิกัด + ผลรวม 0
> (ยืนยันขั้น ⑤), lane = เส้นคงที่ 1 พิกัด, hex = dual ของ triangle (ยืนยัน hex-6)

**คำเตือนของเอกสาร:** ระยะทางบน triangular grid (นิยามผ่าน neighbourhood sequence)
ไม่จำเป็นต้องเป็น metric — ตัวอย่าง 3.4.1: `d(A,B) = 2` แต่ `d(B,A) = 3` (ไม่ symmetric)
— ตัวอย่าง 3.4.2: ไม่ผ่าน triangle inequality — สาเหตุคือ **ลำดับก้าวผสม**
`B = (b₁ b₂ b₃ …)` ที่ประเภทก้าวเปลี่ยนทุกก้าว (เดินกลับต้อง reverse sequence → ระยะไม่เท่า)

**แต่ sequence คงที่ปลอดภัยเสมอ:** `B = (k, k, k, …)` — m-neighbour เป็นความสัมพันธ์
symmetric (นิยาม |p(i)−q(i)| ≤ 1, Σ ≤ m สมมาตรใน p,q) → reverse path ความยาวเท่าเดิม

| walk ของเรา | ภาษาของ Nagy | symmetric? |
|---|---|---|
| stride-12 tetra orbits (⑦) | constant seq B=(12) | ✅ โดย construction |
| stride-37 frame-seek/scatter | constant seq B=(37) | ✅ โดย construction |
| stride-3 Peano/trit (lo-world, ②) | constant seq B=(3) | ✅ โดย construction |
| 3-phase hyperbolic (step%3, X→Y→Z) | periodic seq — หน้าตาคล้าย B=(1 3 2) | ⚠️ แต่**ไม่ใช่ distance** |

**3-phase ปลอดภัยเพราะมันไม่ใช่ระยะทาง:** `phase = step % 3` ใน `hyperbolic_seek.h`
เป็น load-balance rule (60/25/15 ต่อ phase) ไม่ใช่ d(p,q) — และ twin_seeker roundtrip
KIS→Hyper→KIS เป็น bijection 20736/20736 (พิสูจน์แล้ว) — รูป periodic เหมือนกับตัวอย่าง
3.4.1 ของ Nagy แต่ไม่มี metric ให้พัง

**หลักฐานหัวใจ — ระบบเราไม่มี distance function เลย:**

```
เราใช้แค่:  permutation (bijection + cycle)  ← ต้องการแค่ f: สนาม→สนาม, ปิดวง
ไม่ใช้:     distance (ต้อง d(p,q)=d(q,p), triangle inequality)  ← ปัญหาของ Nagy
→ ปัญหา non-metric เกิดได้กับ "ระบบที่คำนวณระยะ" เท่านั้น — เราไม่มีให้เกิด
```

cycle walk ยังเป็นคำตอบของคำถามการห่อ (ขั้น ⑤/⑧ ที่ยังเปิด): เส้นอนันต์ (lane) →
ห่อเป็น orbit ปิด 12 วง × 1728 — ไม่ต้องมี origin เพราะวงปิดไม่มีจุดแรก (enter anywhere)

**ถ้าวันหน้าต้องการ "ระยะทาง" จริง:** ทางรอดมี 2 แบบที่พิสูจน์แล้ว — (ก) constant
stride (symmetric เสมอ), หรือ (ข) hexagonal distance `max(|Δa₁|,|Δa₂|,|Δa₃|)`
(Luczak–Rosenfeld, ref [9] ของ Nagy — metric จริง) — อย่าใช้ sequence ผสม

---

### ⑪ สองพื้น: window floor (d ≤ 2) vs hexagon floor (d ≤ 3) — ทำไม depth-4 แตก (13.5)

hexagon count ต่อ depth (hex-6, 6 nodes ต่อ tile):

```
depth 0:  20736/6  = 3456  ✓ integer
 depth 1:  5184/6   = 864   ✓
 depth 2:  1296/6   = 216   ✓
 depth 3:  324/6    = 54    ✓   ← ยังลงตัว!
 depth 4:  81/6     = 13.5  ✗   ← แตกตรงนี้จุดเดียว
```

**ทำไม depth-4 ถึงแตก (สมการกำแพง 3-world):**

```
cell(d) = 20736/4^d = 4^(4−d)·3⁴ = 2^(8−2d)·3⁴
6 = 2·3 ต้องการ factor 2 กับ 3 ทั้งคู่
 d ≤ 3:  2^(8−2d) เหลืออยู่        → 6 หารลงตัว  ✓
 d = 4:  4⁰ = 1 — 2-world หมดเกลี้ยง → cell = 3⁴ = 81 ล้วน
         81 มีแต่ตัวประกอบ 3 — ไม่มี 2 → 81/6 = 13.5 ✗
```

13.5 = ระบบเตือนว่า **"2-world หมดแล้ว"** — 4-ladder ถูกใช้ครบทั้ง 4 ระดับที่
depth-4 — เหลือแต่กำแพง 3⁴ = 81 ของ lo-world — hexagon (6 = 2·3) ต้องการ
2-world มาแทรก แต่ไม่มีแล้ว → ทศนิยม

**สองพื้น — กฎคนละชั้น (ต้องแยกให้ชัด):**

| พื้น | ขีดจำกัด | เหตุผล | สัญญาณ |
|---|---|---|---|
| **Window floor** (scale axis, §15.21) | depth ≤ 2 | w-axis ดูดซับได้ 2 ระดับ (144→36→9) แล้วอิ่มตัว | ลง d=3-4 = อยู่ใต้ window |
| **Hexagon floor** (tiling) | depth ≤ 3 | 6 หาร 324 ลงตัว (54) แต่หาร 81 ไม่ลง | **13.5** เกิดที่ d=4 |

13.5 เป็นสัญญาณของ **hexagon floor** และมันคือชั้นลึกสุดของทั้งระบบ (d=4 =
ระดับสุดท้ายของ 4-ladder) — ส่วนกฎ "depth ≤ 2" เป็นกฎอนุรักษ์นิยมกว่าที่ผูกกับ
scale axis — ทั้งสองชี้จุดเดียวกัน: **จุดเล็กสุดของระบบอยู่ที่ natural square
grid (window) — subdivide ลึกกว่านั้น = ลงใต้ความละเอียดธรรมชาติ → เลขไม่ลงตัว**

---

### ⑫ geo_jump ↔ KIS — 7 jump types, 3 discoveries, zones (จากโค้ดจริง)

> อ่านจาก `I:/FGLS_new/collection/geo_jump_module/include/geo_jump.h` (342 บรรทัด)
> + พิสูจน์ด้วย probe จริง (MOD order, bijectivity, partition ต่าง)

**โครงสร้าง geo_jump = ตัวเลขเดียวกับเราเป๊ะ:**

```
GEO_TOWER=144  GEO_BLOCK=48  FLOORS=3  CELLS=16(4²)
GEO_PENTAGONS=12  GEO_SHELL_TICK=12  GEO_FIBO_CLOCK=1440
GEO_MOD_PRIME=162 (2·3⁴)  GEO_MOD_STRIDE=5  GEO_FULL=20736 (144²)
ZONES: INNER 24 (=144/6 — hexagon!)  OUTER 144  FAR 432 (=3×144)
```

**zones 24/144/432 + block 48 = ตัวเลข period candidates ของคำถามห่อ torus ครบ**
(24 = 144/6, 48 = 144/3 band, 144 = tower, 432 = 3×144) — อยู่ใน geo_jump แล้ว

**ตาราง mapping — 7 jump types (enum มี 7 ไม่ใช่ 6 ตาม explorer):**

| JUMP | กลไกจริง (จากโค้ด) | bijective? | ของเรา (KIS) |
|---|---|---|---|
| HILBERT | project ไป tower เดิม + offset = hilbert(col,row) + floor·16 | ❌ fix params → 1 offset/tower | a_w view (ของเรา bijective ครบ 144) |
| PEANO | เหมือน HILBERT แต่ cell = peano snake | ❌ | 3-ladder Peano walk (lo-world) |
| PENTAGON | face = node/1728 (12 blocks) + layer·144 | ❌ | 12×1728 — block ≠ residue partition |
| MOD | node × 5 mod 20736 | ✅ order 1728 | additive stride-12 — 12×1728 แต่ partition คนละชุด |
| INVERT | 3-floor cycle + 48-mirror → project tower 0 (period 6) | ❌ collision 20592 | 3-phase (step%3) + antipode mirror |
| GROUND | project ไป (tower, fixed slot) | ❌ | per-scale placement / cell anchor |
| CAPO | node + key×144 (tower shift) | ✅ | **scale-axis translation — stride-144 / window shift** |
| default | node + 1 | ✅ | — |

**Discovery ① — "12 × 1728" มี 3 partition คนละชุด (พิสูจน์ test_tess_12x1728 9/9):**

```
A pentagon block (geo_jump):  node/1728     → 12 × 1728 สม่ำเสมอ ✅
B residue mod 12 (KIS tetra): node%12       → 12 × 1728 สม่ำเสมอ ✅
C MOD coset (×5):             orbit ของ ×5  → 128 orbits ขนาดต่างกัน ❌
   (sizes = ตัวหารของ 1728 ทั้งหมด: 1,2,4,6,8,...,1728 — max 4×1728 = units เท่านั้น)
→ "12 orbits" ของ MOD walk เป็นแค่กรณี max — sync ต้องเลือก canonical ระหว่าง A กับ B
```

**Discovery ② — "bijective map" ใช้ไม่ได้ทุก jump (พิสูจน์จริง):**
MOD + CAPO bijective เท่านั้น — INVERT มี collision 20592 (project ลง tower 0!)
— HILBERT/PEANO/GROUND/PENTAGON เป็น projection — กลไก navigation ไม่ใช่ permutation

**Discovery ③ — CAPO = +key×144 = การเลื่อน tower = scale-axis translation**
ตรงกับ stride-144 บน w-axis ของเรา — จุดเชื่อม sync ที่ชัดเจนที่สุด

**ภาพรวม — สอง decomposition ของ 20736 เดียวกัน (bridge: geo_sync_bridge.h):**

```
geo_jump:  node = face·1728 + tick·144 + local   (12·12·144 = 20736)
KIS:       node = hi·81 + lo → (w, pos)          (144² = 20736)
```

---

## สิ่งที่ "แปลกจากที่เคยเห็นทั่วไป"

| เรื่องปกติ | โครงสร้างนี้ |
|---|---|
| วาดรูปสามเหลี่ยมก่อน แล้วค่อยวางแกน | **เส้นมาก่อน** — สามเหลี่ยมเกิดจากการตัดกันของเส้น 3 เส้น |
| มี origin (0,0) เป็นจุดอ้างอิง | ไม่มี origin — 3 แกน + relation (i+j+k ≡ 0) ทำให้ทุกจุดเท่ากัน |
| เลขศักดิ์สิทธิ์ถูก "เลือก" | 12⁴ = 20736 คือ**จุดตัดเดียว**ที่ constraint ทุกตัวปิดพร้อมกัน |
| 4-subdivision = แค่เลข 4 | 4 = หน้าของ tetrahedron — 12 = 4×3 อยู่ในโครงสร้าง cell เอง |
| 1728 = แค่จำนวน | 1728 = 12³ = orbit ขนาด = 1 pentagon = 20736/12 |
| scale มี 0 | multiplicative s(t) = s₀·kᵗ — kᵗ ≠ 0 เสมอ + dead zone ก้น axis |

---

## สถานะ

| ไฟล์ | สถานะ |
|---|---|
| `tests/test_tess_tetra_axis.c` | 10/10 (พิสูจน์ ①-⑧ ฝั่ง tetra walk) — TIER1 52/52 |
| ⑨ ตาราง 12ᵏ | derived จากตัวเลขที่พิสูจน์แล้ว — ไม่มีค่าทศนิยมที่ depth ≤ 2 |
| ⑩ cycle walk vs non-metric | ยืนยันโดย Nagy 2003 — constant stride = symmetric; ระบบไม่มี distance function → หลบโดย construction |
| ⑪ สองพื้น (window/hexagon) | 13.5 = กำแพง 3⁴ — 2^(8−2d)·3⁴: d=4 หมด 2-world; hexagon floor d ≤ 3, window floor d ≤ 2 |
| ⑫ geo_jump ↔ KIS | จากโค้ดจริง — 7 jump types mapping; 3 discoveries (3 partitions, bijectivity จริง, CAPO = scale shift); zones 24/48/144/432 |
| สมการราก 128×162 | มีอยู่แล้วใน `core/infra/gear_lock.h` |
| 3 แกน X/Y/Z (Hilbert/Peano/Metatron) | มีอยู่แล้วใน `core/hyperbolic_seek.h` (6912/band) |
| tetra-axis walk | **ใหม่** — พิสูจน์แล้ว 10/10 |

จุดที่ยังเปิด (ยังไม่ได้เชื่อม): tetra tessellation เต็มรูปแบบ (flat: tetra+octa / hyperbolic: tetra ล้วน) — และการนำ geo_jump jump types ไปวิ่งบน KIS torus ผ่าน sync bridge (ขั้นถัดไปจาก ⑫)

ปิดแล้วในระหว่างรวบรวม: การห่อ torus — period **(144,144)** พิสูจน์แล้ว (test_tess_torus 16/16, §15.22) — tetra walk บน torus (test_tess_tetra_torus 9/9, §15.23) — 3 partitions ของ 12×1728 + sync bridge (test_tess_12x1728 9/9 + test_geo_sync_bridge 7/7, §15.24)
