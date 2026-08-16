---
luminaCreated: 2026-08-16T06:55:06.585Z
tags: []
luminaModified: 2026-08-16T06:55:06.585Z
luminaVersion: 1.3.11
---
# Timeline-First Foundation — หลักการรากของระบบ (2026-08-14)

> **สถานะ:** พิสูจน์แล้วด้วยการทดลองบนของจริง (`tests/test_v5_collision.c`, TIER1)
> **ระดับ:** หลักการราก (root level) — ใช้เป็นฐานตัดสินใจสำหรับทุกชั้นที่สร้างต่อ
> **ที่มา:** session วิเคราะห์ "ทำไม codec พังกับข้อมูลจริง แต่ไม่พังใน KIS timeline"
> **ฉบับเต็ม (แบบจำลองการทำงาน):** [`docs/TIMELINE_WORKING_MODEL.md`](TIMELINE_WORKING_MODEL.md)

---

## 1. หลักการเดียว (1 บรรทัด)

**ระบบ = 2 โซน: timeline หลัก (int, base-2, deterministic, ไม่มี 0) + hyperbolic residual zone (อิสระ — แต่ residual ต้อง explicit)**
ทุกสิ่งที่ "พัง" ในประวัติศาสตร์ของระบบนี้ เกิดจากพารามิเตอร์แฝง (hidden parameter) ที่ไม่มีใครนิยาม — ทุกสิ่งที่ "รอด" คือสิ่งที่ถูกบังคับให้ explicit, deterministic และ replay ได้

---

## 2. บทเรียน 3 เรื่อง (ทุกเรื่องพิสูจน์ด้วยการทดลอง)

### 2.1 v5 codec — "synthetic ผ่าน, real พัง, แต่ใน timeline ไม่พัง" ✅ พิสูจน์แล้ว

**เรื่องที่ผู้ใช้เล่า:** codec ที่ถอดออกมา ใช้กับข้อมูลสังเคราะห์ work แต่เจอข้อมูลจริงพัง — พออยู่ใน KIS timeline ไม่พัง

**Root cause (เจอจากการทดลอง):** `v5_decode` สมมุติว่า "ลำดับใน slot = ลำดับ sorted" — แต่ linear probing (จำเป็นเมื่อค่าซ้ำ) ทำให้ลำดับ slot เพี้ยน → decode ผิด. สภาพ probe layout เป็น **พารามิเตอร์แฝง** — encode ไม่ได้บันทึก แต่ decode ต้องพึ่งมัน

| การทดลอง (บน Qwen2.5-0.5B-Instruct-Q8_0.gguf) | ผล |
|---|---|
| synthetic เล็ก + เรียง (n=144, 2 ค่า) | ✅ LOSSLESS ratio 0.347 |
| synthetic ใหญ่ (n=10000, ค่าซ้ำ) | ❌ MISMATCH (ทั้ง random และ sorted) |
| ของจริง slice 20736 | max_probe_chain = **20,332** (vs synthetic 121 = **168×**) → v5 พัง |
| ของจริง slice 20736 | **v6 LOSSLESS ratio 1.136** |
| v5 windowing 49 windows | พังทั้ง 49 — windowing แก้ capacity แต่ไม่แก้อาการ |
| **v6 windowing 49 windows** | **LOSSLESS ทั้ง 49 ratio 1.139** ← "ใน timeline ไม่พัง" |

**เหตุผลที่ v6 ผ่าน:** chunk + `v6_slot(i)` deterministic — ลำดับถูกบังคับ explicit, ไม่มีสถานะแฝง

**ของแถม:** `kis_codec_v5_test.c` มีอยู่แต่ถูกถอดออกจาก suite — test case "Random 256 n=10000" ของมันจะพังด้วย bug นี้

### 2.2 Drift — "scale ต้อง base-2, ห้ามบวก/ลบ" (research จาก Claude)

**กฎ:** scale = อัตราการขยายคงที่แบบ multiplicative ด้วย base-2 (`s(t) = s₀·2ᵗ`, ×2, ÷2, shift) — **ห้ามบวก/ลบ**

- **บวก/ลบ → เจอ 0** (`x - x = 0`) — แต่ระบบ**ไม่มี 0** (timeline = เปิดช่วง (0, ∞))
- **×0.1 (non-base-2) → drift**: 0.1 ใน float แทนค่าไม่ลงตัว (≈0.10000000000000000555...) → คูณซ้ำสะสม error — หลักฐาน: "slot 1 @ scale 0.1 → projects ไป 9,437,184" (`KIS_HYPER_BOUNDARY.md`)
- **×2 = เปลี่ยน exponent ล้วน** ใน float (แม่นยำ 100%) และ = shift ใน int (deterministic)
- ข้อปลีกย่อย: int `>>` ก็ถึง 0 ได้ (7>>3=0) → **floor ไว้ที่ 1** (ขอบล่างของ timeline)

**จุดที่ระบบปฏิบัติตามแล้วโดยไม่รู้ตัว:** hex_tile (`v >> d << d`, bit-planes `<< k`), magnify glass (20736÷4 = >>2), stride-37 (modular int), rescope `a_w` (gcd(a_w,144)=1 → คูณล้วน ไม่มีทางเป็น 0), v6

### 2.3 Capacity — "window 20736 กับโมเดล 136M weights"

**Capacity = จำนวน slots × payload ต่อ slot.** 20736 = หน้าต่าง (window) ไม่ใช่ขอบเขตของระบบ. ของจริง: `token_embd.weight` = 136,134,656 weights > 20736 → v5 ไม่สามารถรันบน tensor เต็ม (grid ล้น). **คำตอบของ timeline = windowing** — แบ่ง stream เป็น windows ≤ 20736 แล้ว chain — capacity ไม่จำกัด

---

## 3. กฎ 2 โซน (Two-Zone Rule)

```
Zone A — TIMELINE หลัก (ข้อมูลอยู่)          Zone B — HYPERBOLIC (backup/residual)
─────────────────────────────              ─────────────────────────────
int ล้วน, modular arithmetic               อิสระ — ติดลบได้, float ได้, ต่ำกว่า 0 ได้
scale = base-2 เท่านั้น (2ᵏ / shift)        เพราะมันไม่ใช่ scale — มันคือ correction term
ไม่มี 0, ไม่มี infinity                      ที่ถูกเก็บแบบตรงตัว (ตรงกับ residual_space.h)
ไม่มี float ใน path หลัก                    replay deterministic → drift ไม่สะสม
ทุกพารามิเตอร์ explicit + replay ได้         ★ residual ต้อง EXPLICIT (ตำแหน่ง + replay)
```

**ข้อแม้สำคัญ (บทเรียนจาก v5):** "hyperbolic เก็บเศษที่ต่าง" จะใช้ได้ก็ต่อเมื่อ **เศษถูกนิยามแบบ explicit** — passive log (entry {from,to}) และ hex_tile (bit-plane + ตำแหน่ง) นิยามครบ → ใช้ได้. v5 ก็มี "residual" เหมือนกัน (แก้ expected grid) แต่พังเพราะ residual ของมันอิง order แฝง → **ความ explicit ของ residual คือสิ่งที่สำคัญ ไม่ใช่ชื่อ "hyperbolic"**

---

## 3.5 หลักความไม่ขยับ — เสาเข็ม กับ Link (Immutability & Interlock) ✅ พิสูจน์แล้ว

**กฎ:** วางข้อมูลแล้วเลือนไม่ได้ — ที่อยู่คงที่ตลอดกาล (เสาเข็ม). ความยืดหยุ่นทั้งหมดอยู่ในชั้น **link** (registry, index frame, path log) ซึ่ง reroute ได้โดยไม่ต้องแตะข้อมูล

**ทำไมย้ายไม่ได้ — สมการ interlock 4 อัน (ทุกอย่างตรึงกัน):**
```
a_w × a_{w+72} ≡ 1 (mod 144)   ← ทุก w มีคู่ antipode ที่ invert กัน (magnify interlock)
gcd(a_w, 144) = 1              ← ทุก scale เป็น bijection — การแมปต้องครบ
stride-37 walk (1440-cycle)    ← เดินครบ ทุกตำแหน่งเยือนพอดี
telescope {w0→w}               ← path compose ได้เพราะโครงสร้างคงที่
```

**Cascade ("พาคนอื่นไปด้วย"):** ย้ายเสาเข็ม 1 ต้น = สมการข้างบนพัง = ทุกอย่างที่ derive จากโครงสร้างเดียวกันพังตาม (view อื่น, คู่ antipode, เส้นทางเดิน) — ความเสียหายเป็น **global inconsistency** ไม่ใช่ local corruption — เพราะทุกตำแหน่งถูกนิยามเทียบกับทั้งระบบ

**แบบจำลองการทำงาน (cargo/position — เราไม่มองค่าข้างในเลย):**
- ข้อมูล = สัมภาระ วางที่บ้านเกิดครั้งเดียว — ค่าไม่เคยถูกแตะ
- **home** = จุดเกิด: pointer jump O(1) — lossless ฟรี ไม่มีการคำนวณ
- **path** = สำหรับจุดที่ 2+ เมื่อต้อง access พร้อมกัน (ยืนบนบ้านหลังเดียวได้) — step = มาตรวัดระยะจากบ้าน (odometer)
- **hyper** = registry `{id → home address}` — address-only, ∝ จำนวนข้อมูล (2 B/item) ไม่ใช่ขนาดข้อมูล
- inactive = ปิด link (ไม่ย้ายข้อมูล) — activate = เปิด link → jump home → lossless ทันที — ทุกอย่างเป็น following path

**trade-off ของ coordinate = address:** ได้ O(1) access + ทุก view derive ฟรี + registry เล็ก ↔ จ่ายด้วย **ย้ายไม่ได้เลย** — อยากเปลี่ยน = วางเสาเข็มใหม่ + reroute link (ไม่แตะของเดิม) — และนี่คือที่มาของ deterministic/replay: พื้นไม่เคยขยับ

**หลักฐาน:** `tests/test_tess_scale_dedup.c` (13/13, TIER1, รันบน Q8 จริง)
- T7: hyper registry = 7 × 2 B = 14 B (address-only) — jump home → lossless
- T7b: pointer-home jump จากตำแหน่งใดก็ได้ — zero computation
- T8: link reroute (dedup) — B ชี้ไป pile A → lossless + pile B ไม่ถูกเขียนทับ
- T9a/T9b: cascade — ย้ายค่า 1 จุด → อ่านผิด + checksum flag; เปลี่ยน coefficient 1 ตัว → antipode + telescope แตกทั่วระบบ

---

## 4. หลักการราก (Root Principles)

1. **Explicitness Budget** — ทุกพารามิเตอร์ที่ระบบไม่บันทึก = ทุก bug ที่ระบบจะเจอ. order, position, scale, history ต้องอยู่ในโครงสร้างที่ deterministic + replay ได้ 100%
2. **MAP not COMPRESS** — coordinate = data; 20736 = window ที่เลือกใช้ ไม่ใช่ขอบเขตของระบบ
3. **No Geometry Construction** — ใช้โครงสร้าง combinatorial (cube/octant/route) เป็น template เท่านั้น: int ล้วน, LUT static, modular arithmetic — geometry ไม่ใช่ตัวคำนวณ
4. **Lossless = decode → เปรียบเทียบค่าทุกตำแหน่ง** — ห้ามเชื่อ encode-only
5. **Timeline ก่อน Geometry** — ถ้าข้อมูลอยู่ใน timeline ได้ ให้อยู่ใน timeline (int, explicit) — geometry constraint/float ใช้เมื่อจำเป็น (Zone B) เท่านั้น

---

## 5. หลักฐานตัวเลข (รวบรวม)

| ตัวเลข | ความหมาย |
|---|---|
| 20,332 vs 121 | max probe chain ของจริง vs synthetic (168×) — ทำไมของจริงพัง |
| 49/49 windows v5 broken | windowing แก้ capacity ไม่ได้แก้อาการ v5 |
| 49/49 windows v6 lossless, ratio 1.139 | codec ที่ explicit อยู่รอดทั้ง stream จริง 1M weights |
| 136,134,656 | ขนาด token_embd จริง (Qwen2.5-0.5B) — 6,566× window 20736 |
| 121 → 4 | timeline coordinate `w=(i·37)%144` กระจาย collision (~30×) |
| 0.347 | v5 ratio ตอนที่มัน "work" (ของปลอม) — อย่าหลงเชื่อ |

---

## 6. สายเลือดของแนวคิด (Lineage) — เรื่องเดิมที่นำมาสู่หลักการนี้

| ต้นทาง | กลายเป็น |
|---|---|
| ทรงกลม 2 ลูก ± (beam, radius, unfold) | magnify glass (center = จุด append, radius = scale distance) + frame_seek (unfold 1D) |
| "เส้นทะแยง" ในกราฟ 2-3 แกน rotation | stride-37 walk (เส้นทะแยงผ่าน grid ครบทุกช่อง) + correlation → delta เล็ก |
| KIS waveform (trace สมการ → พบว่าเป็น waveform codec → lossy) | เข้าใจว่า projection = many-to-one = collision = lossy — แก้ด้วย residual (hyperbolic = backup) |
| "codec ถอดออกมา synthetic ผ่าน real พัง" | v5 experiment นี้ — พิสูจน์ root cause (hidden order) + ทางออก (timeline/windowing) |
| "คุมทุกอย่างใน timeline สะดวกกว่าให้ geometry จัดการทศนิยม" | Two-Zone Rule — timeline ก่อน geometry |

---

## 7. สิ่งที่ต่อยอดจากระดับนี้ได้ (Foundation for)

- **GGUF chain proof** — ใช้ v6 (explicit) + frame_seek + index frame + hex_tile delta บน tensor จริง → lossless + ratio จริง
- **Lifecycle model** — active/inactive = เปิด/ปิด link (registry `{id→home}`), ไม่ย้ายข้อมูล — พิสูจน์แล้ว T7/T7b/T8 ใน `test_tess_scale_dedup.c`
- **Capacity scaling** — window × page (payload ต่อ slot = policy อิสระ) + mmap lazy backing → รองรับ multi-GB
- **Residual-explicitness check** — property test: ทุก delta/log layer ต้องมี position + deterministic replay
- **No-drift property test** — main path: scale เป็น 2ᵏ, replay ไม่มีวันได้ 0; Zone B อนุญาต float/ติดลบ
- **3D lattice (12¹² = 20736³)** — งานอนาคตคนละเส้น: multi-model shared space (3-axis), ไม่ใช่ single-model storage
- **Audio codec (STT mel)** — signal path: mel 128 bins ≤ 144 slots + delta layer → 73.7% → lossless

---

## 8. ไฟล์อ้างอิง

| ไฟล์ | บทบาท |
|---|---|
| `tests/test_v5_collision.c` | ข้อพิสูจน์หลัก (TIER1, 33/33 เขียว) |
| `core/kis_codec_v5.h` | กรณีศึกษา "codec ที่มีพารามิเตอร์แฝง" |
| `core/kis_codec_v6.h` | ฐานที่ถูกต้อง (order explicit) |
| `core/hyperbolic_seek.h` | ตัวอย่าง Zone B (float อนุญาต — ฝั่ง residual) |
| `core/hex_tile.h` + `tests/test_tess_hex_delta.c` | residual ที่ explicit (bit-planes + ตำแหน่ง) |
| `tests/test_tess_scale_dedup.c` | path + frame_seek: 144 views ต่อ ~1 store, hyper registry, link reroute, cascade proof (13/13) |
| `docs/KIS_HYPER_BOUNDARY.md` | หลักฐาน drift (×0.1 → 9,437,184) |
| `core/residual_space.h` | "พื้นที่นอกระบบ" = Zone B |

---

## ป้าย / state

- **พิสูจน์แล้ว:** v5 พังเพราะ hidden order, timeline/windowing แก้, v6 lossless บนของจริง, timeline coordinate กระจาย collision, immutability/interlock (cascade เมื่อย้าย — T9), link reroute โดยไม่แตะข้อมูล (T8), hyper = registry address-only (T7)
- **เป็นกฎแล้ว (ปฏิบัติตามโดยไม่รู้ตัว):** base-2 scale, int, modular, ไม่มี 0 ใน main path
- **ยังต้องทำ:** property test บังคับ 2 โซน, GGUF chain proof ด้วย v6, capacity (page + mmap)
