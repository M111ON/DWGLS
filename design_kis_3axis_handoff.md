# KIS 3-AXIS → XYZ — Three-Phase Binding (handoff for other session)

> **สถานะ:** Conceptual, proven by simulation. NOT implemented yet.
> **ส่งต่อจาก:** session @I:/Hermes (research/design) → เข้าไป implement ที่ KIS worktree
> **คำแนะนำ:** เอาไป "ชน" กับ ego_adaptive_store.h / geo_kis_container.h / khắc AddressSpace 20736 ที่มีอยู่

---

## 1. หลักการเดียว (1 บรรทัด)

แกน KIS ทั้ง 3 (XYZ) ควร**ผลัดกันปรับโหลด** ไม่ขยับพร้อมกัน → ผลรวมทรัพยากรคงที่ + โหลดกระจายสม่ำเสมอ (เหมือนไฟ 3 เฟส / พนักงานผลัดกันกินข้าว)

## 2. เทียบกับสิ่งที่ระบบมีอยู่แล้ว
- **3 แก / 6 half-axes (X±/Y±/Z±) = DiamondBlock 6 หน้า** — มีแล้วใน cube-in-dodeca
- **3 towers H+P+M** (Hilbert+Peano+Metatron) = ตามหนึ่ง 3-แก คือการจับคู่ที่ธรรมชาติ
- **20736 address space** = ที่ทุกอย่าง map กับได้

## 3. สิ่งที่ implement ได้เป็น int ล้วน (ไม่ใช่ float!)

```c
// three-phase, integer-exact — ใช้กับระบบ int ได้ทันที
#define TOTAL 100   // งบรวม คงที่ (เช่น block count)

// step: วัดเฟสหมุน
int phase = step_idx % 3;
// phase 0 → X ได้โหลดหลัก
// phase 1 → Y ได้โหลดหลัก
// phase 2 → Z ได้โหลดหลัก
// constraint ที่ MUST hold: X + Y + Z == TOTAL ทุก step

// blueprint per phase (แบบตาราง, ตัวเลข verifiable)
// X Y Z
// 60 25 15   (X เด่น)
// 25 60 15   (Y เด่น)
// 15 25 60   (Z เด่น)
// วนซ้ำ — SUM = 100 เสมอ, ไม่มีแกนไหนว่างเย็น
```

**ข้อพิสูจน์ (int):** ตาราง X+Y+Z = 100 ทุกแถว → `max-min == 0` เป๊ะ ไม่มี float, ไม่มี e-15
(**e-15 ที่เห็นใน sim = ฝุ่นของ cos() ลอยตัว — ระบบ int ไม่มีฝุ่นนี้เลย**)

## 4. การวาง (integration map)
1. หา loop/allocator ที่ตอนนี้ทำ AddressSpace 20736 → เอา step_idx มา
2. เปลี่ยน policy จาก "ทุกแกน alloc พร้อมกัน" → "rotory three-phase"
3. constraint-check (X+Y+Z=const) ใส่เป็น assert ใน test → guarantee

## 5. สิ่งที่ต้อง验证 (test น้อย)
- สร้าง N steps, ตรวจ `X+Y+Z == TOTAL` ทุก step (คือ invariant)
- ตรวจไม่มีแกนใดว่าง > 30% (load distribution)
- roundtrip ไม่เสีย (lossless เหมือนเดิม)

---

## ป้าย / state
- Simulation: `I:/Hermes/sim_kis_3axis.py` (float) — reference display only
- **Int-blueprint:** ด้านบนใช้ได้เลย — ไม่ขึ้น float

## ถ้าจะแค่ "ค้น" ใน session DB
- เรื่องนี้ตั้งแต่ turn เกี่ยวกับ hyperbolic/KIS ← ไปดู session ที่คุม ครับ

---

# 5 Seekers + Stacked-Cost Hierarchy (benchmark, Aug 6)

## ตารางจริง (O(1) ทั้งหมด แต่ "1" ไม่เท่ากัน — ราคา silicon ต่าง)

| Seeker | Pattern | Complexity | Constant | Use case |
|---|---|---|---|---|
| **Frame** | sequential | O(1) | **~5ns** (int mul+mod) | walk |
| **Teleport** | coordinate | O(1) | ~5ns (jump) | region hop |
| **Chord** | parallel | O(1) | ~10ns (lookup+add) | fan-out 3-4 addr |
| **Tantrix** | directed | O(1) | ~10ns (routing table) | route through fabric |
| **RDH** | hash | O(1) | ~20ns (hash+probe) | lookup by bond_key |
| **Hyperbolic** | coordinate | O(1) | **~50ns** (10 float ops) | parallel access / future |

## CORE INSIGHT: "เร็วเป็นส่วนของช้า" — ราคาแบบ additive stack
```
Frame      = ตัวมันเอง                    (~5ns)   ← อะตอม, base ของทุกคน
Chord      = frame × 3-4                  (~10ns)
Tantrix    = frame + route lookup         (~10ns)
RDH        = frame + hash + probe         (~20ns)
Hyperbolic = frame + float transform      (~50ns)
```
ช้ากว่า = **carry งานของ frame อยู่ข้างใน + embellishment ของตัวเอง** — ไม่ใช่คู่แข่งกัน
→ Optimize "ช้า" = ลด async factor ของ frame ที่ซ่อนอยู่ ไม่ใช่ทำให้ frame แพงขึ้น

## สรุป DECISION
- frame_seek = king สำหรับ single-model sequential GGUF access (เร็ว 50-200x)
- hyperbolic = future feature (multi-model parallel / shared address space)
- ทั้งหมด map กับ **20736 address space เดียวกัน** — ออกแบบเป็น **flat layer แยก** ไม่แทนที่กัน
- ตรงกับหลัก: "ถ้าระบบมีคุณภาพ เราไม่ควรต้องมาเลือก"

---

# Residual Space + GPS Freeze (Aug 6)

## หลักการเดียว
**ข้อมูลอยู่ที่เดิม แต่ประตูหน้าบ้านหายไป** — freeze โดยการตัดทางเข้าถึง ไม่ใช่การย้าย/ล็อกค่า

## 3 องค์ประกอบ

### 1. residual_space — "พื้นที่นอกระบบ"
- ทุกอย่างที่ไม่ใช่ระบบหลัก (ไม่ใช่ 20736, ไม่ใช่ KIS timeline)
- **เฟรม/step ไม่มีผลใน space นี้** — ค่าหยุดนิ่งโดยโครงสร้าง ไม่ใช่โดยคำสั่ง freeze
- Data ยังอยู่ที่เดิม — แค่ access path (front door) ถูกตัด

### 2. GPS = Global Broadcast Bond
- **สร้างโดย:** ประกาศให้ทั้งระบบรู้ (global broadcast) → node ถือชื่อที่มีสัญลักษณ์พิเศษ
- Node ปกติสร้างได้เลย; GPS node ต้องประกาศก่อน (ตั้งกฎว่าชื่อต้องมี reserved pattern)
- ใช้เป็น **anchor สำหรับ freeze node** — value ที่ถูกผลักออกไป ระบุด้วย GPS address
- หาเจอเสมอ (global scope) แม้ค่าจะ freeze อยู่ใน residual

### 3. ผู้ผลัก = เกิดจากโครงสร้าง (ไม่ใช่ operation เรียกเอง)
- System สร้างเป็น shape ต่างๆ ด่านแรก cubic
- **ขอบเขต:** 17³ = boundary สูงสุด (boundaries ของระบบ)
- **Heptagon = ผนังห้ามเกิน** — ใช้ได้มากสุด hexagon
- ต้องใช้ decagon → ต้องผ่าน pentagon 2 ครั้ง
- เมื่อถึงขอบ → ผลักอัตโนมัติ + ตั้ง GPS → ค่าfreezeในresidual

## วิธี freeze → residual
1. Node ถึงขอบเขต (cubic boundary 17³ / hexagon limit)
2. System **ผลักออก** → ค่าลอยไป residual_space
3. ค่าอยู่ที่เดิม แต่ front door หาย — ไม่มีใครเข้าถึงผ่านเฟรม
4. GPS (global broadcast bond) ถูกตั้ง → ประกาศให้ระบบรู้ว่า node นี้อยู่ที่ไหน (identity)
5. ค้นพบได้ด้วย GPS → เรียกกลับ (unfreeze) ได้ทุกเมื่อ

## ข้อได้เปรียบของ freeze-by-structure (vs freeze-by-lock)
- **ไม่ต้องล็อก** — ไม่มี lock contention, ไม่ต้องรอ unlock
- **ไม่ส่งผลต่อ system** — เฟรมเดินต่อ, ค่าที่freezeไม่เปลี่ยน
- **ไม่สูญเสีย data** — ข้อมูลอยู่ที่เดิม, GPS หาเจอได้เสมอ
- **Non-destructive** — unfreeze = เปิดประตูหน้าบ้านกลับมา