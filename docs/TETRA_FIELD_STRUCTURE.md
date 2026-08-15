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
| `tests/test_tess_tetra_axis.c` | 10/10 (พิสูจน์ ①-⑧ ฝั่ง tetra walk) — TIER1 48/48 |
| ⑨ ตาราง 12ᵏ | derived จากตัวเลขที่พิสูจน์แล้ว — ไม่มีค่าทศนิยมที่ depth ≤ 2 |
| สมการราก 128×162 | มีอยู่แล้วใน `core/infra/gear_lock.h` |
| 3 แกน X/Y/Z (Hilbert/Peano/Metatron) | มีอยู่แล้วใน `core/hyperbolic_seek.h` (6912/band) |
| tetra-axis walk | **ใหม่** — พิสูจน์แล้ว 10/10 |

จุดที่ยังเปิด (ยังไม่ได้เชื่อม): การห่อ 3 เส้นอนันต์เป็น torus ด้วย period (144/288/48/432) — relation ของ lattice ที่ทำให้ arrangement ปิด — และ 4 ระนาบ → tetra tessellation เต็มรูปแบบ (flat: tetra+octa / hyperbolic: tetra ล้วน)
