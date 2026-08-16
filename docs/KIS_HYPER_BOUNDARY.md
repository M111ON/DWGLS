---
luminaCreated: 2026-08-16T06:55:01.743Z
tags: []
luminaModified: 2026-08-16T06:55:01.743Z
luminaVersion: 1.3.11
---
# KIS ↔ Hyperbolic Dual Boundary — Documentation

## วันที่: 7 สิงหาคม 2026

## สรุปการค้นพบ

### 1. Drift เกิดขึ้นจริงเมื่อ Scale เปลี่ยน

**การพิสูจน์:**
```
slot 0 at scale 1.0 → projects to 0
slot 1 at scale 0.1 → projects to 9,437,184 (!!!)
slot 2 at scale 0.1 → projects to 19,922,944 (!!!)

→ Position เปลี่ยนหมด = drift จริง
→ อ่านจาก slot เดิม = ได้ข้อมูลผิด
→ ต้องอ่านจาก creation point = ถึงจะถูก
```

**กฏ:** ต้องอ่านจากจุดที่สร้างถึงจะครบ

### 2. Hyperbolic Formula: x × f(time)

**สูตร:**
```c
hyper_resolve_address(creation_point, current_scale)
- creation_point = (slot, scale, hyper_re, hyper_im)
- formula: x × scale_ratio → new address
- Result: O(1) address computation
```

**ความเร็ว:**
- Slow (with atan2): 182 ns/op
- Fast (no atan2): 10 ns/op (18x faster)
- Frame seek: 5 ns/op

### 3. Compression เกิดขึ้นจริง

**ผลลัพธ์จาก Real GGUF (Qwen2.5 Q8_0):**
```
Scale 1.0: 20736 → 20736 unique = 1.00x (baseline)
Scale 0.5: 20736 → 16248 unique = 1.28x
Scale 0.1: 20736 →  8088 unique = 2.56x
```

**ทำไม compression เกิดขึ้น:**
- KIS projection = geometric mapping (ไม่ใช่ storage)
- ย่อ scale → positions merge → หลาย slots → address เดียวกัน
- = natural compression

### 4. 3-Axis Auto Selection

**Auto axis ตาม slot number:**
- AXIS_X: 0-6911
- AXIS_Y: 6912-13823
- AXIS_Z: 13824-20735

**Roundtrip:** 20736/20736 PASS (ทุก slot บนทุกแกน)

### 5. Architecture ที่ถูกต้อง

```
เดิม: เก็บ data + delta = ไม่ลดพื้นที่
ใหม่: เก็บ data + address formula = ลดได้จริง!

KIS: เก็บ original data (ไม่เพิ่มพื้นที่)
Hyper: เก็บ formula (เล็กมาก)
     → คำนวณ address ตอน runtime

Compression = side effect ของการเปลี่ยนมุมมอง
```

## Test Files

| File | ทดสอบอะไร |
|------|-----------|
| test_kis_hyper_handoff.c | Threshold finding |
| test_kis_hyper_handoff_v2.c | Single axis test |
| test_kis_hyper_delta.c | Delta capture |
| test_kis_hyper_real_gguf.c | Real GGUF test |
| test_kis_hyper_threshold.c | Scale sweep |
| test_kis_hyper_correct.c | Creation point rule |
| test_kis_hyper_formula.c | Formula proof |
| test_kis_hyper_pipeline.c | Full pipeline |
| test_kis_hyper_3axis.c | 3-axis auto selection |
| test_kis_hyper_speed.c | Speed benchmark |
| test_kis_hyper_fast.c | Optimized (no atan2) |

## สรุป

1. ✅ Drift เกิดขึ้นจริงเมื่อ scale เปลี่ยน
2. ✅ Formula คำนวณ address ได้ O(1)
3. ✅ Compression เกิดขึ้นจริง (2.56x ที่ scale 0.1)
4. ✅ 3-axis auto selection ทำงาน
5. ✅ Fast version: 10 ns/op (พร้อม GPU acceleration)

## GPU Acceleration

**ยังไม่ได้ใช้ GPU:**
- CPU: 10 ns/op = 100M ops/sec
- GPU T4: 7.23 GB/s
- GPU HBM: 18.23 GB/s
- GPU:CPU ratio = 173:1

**Potential:** 100M × 173 = 17.3 BILLION ops/sec

## Board Posts

| # | หัวข้อ |
|---|--------|
| #14 | KIS ↔ Hyperbolic Dual Boundary |
| #15 | System Status: What Works vs What's Missing |
| #16 | Container = Protection for Scale/Offset Changes |
| #17 | KIS + Hyper: x × f(time) = compute on fly |
| #18 | DRIFT PROVEN: Scale change = position shift |
| #19 | HYPERBOLIC FORMULA PROVEN: x × f(time) works |

## Commit

```
2d27f04 feat: KIS ↔ Hyperbolic dual boundary experiments
```
