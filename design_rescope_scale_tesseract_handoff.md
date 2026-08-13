# Rescope Handoff — Scale Timeline + 1 Tesseract (Frame-as-Index)

> **สถานะ:** Conceptual → **พิสูจน์แล้วโดย test 5 ตัว** (ทุกตัวผ่าน, อยู่ใน Makefile TIER1)
> **วันที่:** 2026-08-14
> **ส่งต่อจาก:** session rescope (DWGLS worktree) → implement ต่อ (container/codec/GGUF)
> **หลักการเดียว:** เราไม่ได้สร้าง geometry — ใช้โครงสร้าง combinatorial (cube/octant/route) เป็น template ในการ map ข้อมูลเท่านั้น

---

## 1. หลักการเดียว (1 บรรทัด)

**ระบบ = scale timeline multiplicative + passive scale-change log บน hyperbolic side + 1 tesseract เป็น frame-as-index**

- scale = **อัตราการขยายคงที่** (`s(t) = s₀·kᵗ`) — ไม่ใช่ geometry; ico/dodeca sealed/spike เป็นแค่พาหะ
- timeline ไม่มี 0 ไม่มีที่สิ้นสุด — เราเลือกหน้าต่าง `(0, 20736)` มาทำงาน
- **ทุกอย่างขยับพร้อมกันหมด (global scale เดียว)** → append ไม่ต้อง tag scale
- ฝั่ง hyperbolic เก็บ **log ของ scale-change events** (เล็ก, deterministic) → replay → lossless
- ดูเผินๆ เหมือน lossy — **lossless เมื่ออ่านที่ scale เดียวกับตอน append**; scale ไม่ตรง → replay log/delta → lossless อีกครั้ง

## 2. ตัวเลขศักดิ์สิทธิ์ (ยึดให้ตรง)

| ค่า | ความหมาย |
|---|---|
| 20736 | window ทั้งหมด = 144² = 1728×12 = 18 tes × 8 cube × 144 |
| 5184 | 20736÷4 (quadrant) = 36 scales × 144 vertices |
| 144 | scale positions (W) ต่อ cube/vertex |
| 1152 | 1 tesseract = 8 cube × 144 |
| 1440 | frame_seek cycle (stride-37, 12 faces × 120) |
| 8 | cubes ต่อ tesseract (1 index + 7 data) |
| 18 | tesseracts ใน 6ico compound — **FUTURE, ยังไม่ implement** |

## 3. Rescope 3 ชั้น (เรียงจากล่างขึ้นบน)

### 3.1 Scale Timeline + Passive Log (พิสูจน์: `test_tess_scale_log.c` 10/10)

```
slot = cube×144 + w          w = scale position ∈ [0,144)
view at w:  p = (a_w·l + b_w) % 144     (gcd(a_w,144)=1 → bijection)
```

- append ที่ scale w0 → data เก็บครั้งเดียว, **ไม่ tag scale**
- scale เปลี่ยน → passive log entry 1 อัน (`{from, to}` 2 bytes) — **delta ∝ จำนวน events ไม่ใช่ขนาดข้อมูล**
- อ่านที่ w0 (log ว่าง) → lossless ตรงๆ
- อ่านที่ w ≠ w0 → replay log (deterministic) → lossless
- replay telescope: log ยาวเท่าไรก็ collapse เป็น entry เดียว `{w0 → w}`

### 3.2 Seeker เดิน Timeline (`test_tess_frame_seek.c` 8/8)

```
t (timeline) → enc = frame_enc(t) = (t·37) % 1440     (geo_frame_seek.h)
w (scale view) = enc % 144                            (1440 = 144×10)
```

- เดิน 144 step → ครบทุกระดับ scale เป๊ะ (gcd(37,144)=1); 1 รอบ = 10 รอบ scale
- ทุกตำแหน่ง: **เห็น index frame (cube 0) เสมอ** → retrieve ครบ 8 cube → lossless ทุกตำแหน่ง (145,152 checks ผ่าน)

### 3.3 Magnify Glass (`test_tess_magnify.c` 12/12)

```
glass = กลาง window [36+δ, 108+δ)   center = 72+δ   (δ = offset นิดหน่อย = 5)
อัตรา invert กับฝั่งตรงข้าม: a_w × a_{w+72} ≡ 1 (mod 144)  ครบทุก w
  glass: 5, 7  ↔  ฝั่งตรงข้าม: 29 (=inv 5), 103 (=inv 7)
ฝั่งตรงข้าม = hyperbolic side (compressed, เก็บ delta)
```

- 20736÷4 = 5184 = 36 scales × 144 vertices ✓
- ตรงกับ antipodal binding ของ `sim_kis_hyperbolic.py` (peak A = trough B, ผลรวมคงที่)

### 3.4 Delta จริง = hex_tile residual (`test_tess_hex_delta.c` 10/10)

```
view at dist d (จาก glass center = จุด append, "enter anywhere"):
    view_d = v >> d << d        (บิตล่างหาย — ดู lossy)
scale เปลี่ยน 1 hop → hyperbolic side เก็บ 1 residual layer:
    layer_k = bit-plane k ของค่าทั้งหมด, hex_tile-encoded (hex_tile.h, 144 tiles × 7 cells)
replay: v = view_d | Σ plane_k << k      (hex_tile_decode) → lossless
antipode (d=72, view=0) → delta เก็บครบ — ฝั่งตรงข้าม = hyperbolic side ตามตัวอักษร
```

- structured (Q8-like staircase) delta 5,097B vs random 10,361B — FLAT tiles ชนะชัด
- per-event delta (~637B) < re-store data (1008B)

## 4. 1 Tesseract = Frame-as-Index (ฐานของทุกชั้น)

```
slot = cube×144 + local          cube 0..7, local 0..143 → 1152 slots
cube 0 = INDEX frame (144 slots = 8 blocks × 18):
         base(2) + len(2) + stride/checksum(1) ของทุก cube
cube 1..7 = DATA (1008 slots)
มอง 1 frame → retrieve ครบ 8 cube — lossless, deterministic   (test_tess_index_frame.c 7/7)
```

- **index frame อ่านได้ตรงๆ ทุก scale** (ประตูหน้าบ้านเปิดตลอด — ไม่ต้อง replay)
- data เก็บครั้งเดียว; delta/log อยู่ฝั่ง hyperbolic แยกจากกัน

## 5. Files ที่ทำแล้ว (ทุกตัว PASS + อยู่ใน Makefile TIER1)

| File | Test | ผล |
|---|---|---|
| `tests/test_tess_index_frame.c` | 1 tess = 8 cube, index LUT | 7/7 |
| `tests/test_tess_scale_log.c` | scale timeline + passive log | 10/10 |
| `tests/test_tess_frame_seek.c` | frame_seek เดิน timeline | 8/8 |
| `tests/test_tess_magnify.c` | magnify glass 20736÷4 + offset | 12/12 |
| `tests/test_tess_hex_delta.c` | hex_tile residual delta | 10/10 |

แก้ไขด้วย: `AGENTS.md` (section "🧭 Rescope — Scale Timeline + 1 Tesseract"), `Makefile` (TIER1 +5 ตัว)

```
make test-test_tess_index_frame   # 7/7
make test-test_tess_scale_log     # 10/10
make test-test_tess_frame_seek    # 8/8
make test-test_tess_magnify       # 12/12
make test-test_tess_hex_delta     # 10/10
```

## 6. สิ่งที่ implement ต่อ (integration map)

1. **เอาเข้า container จริง** — ชนกับ `core/tesseract_container.h` (8 octants, Cayley) / `core/geo_tess_container.h` (20736 slots) / `core/dwgls_tesseract_codec.h`:
   - index frame (cube 0) ต่อกับ `TessHeader` / `TESS_Header` — ใส่ base/len/checksum ลง block
   - scale log (passive) ต่อกับ `hyper_delta.h` / `residual_space.h` / Jet Bridge (`fibo_spine.h`)
2. **GGUF จริง** — เอา weights จริง (quantized Q8-like → staircase) วิ่งผ่าน chain:
   `frame_seek → magnify glass → index frame → hex_tile delta` → พิสูจน์ lossless กับ tensor จริง + วัดอัตราส่วน delta จริง (ดู `gguf_reader.h`, `geo_frame_seek.h`)
3. **Seeker tools** — geo_jump / dramtile / RDH (อยู่ repo อื่น) — เอามาขับ frame_seek/magnify
4. **18tes (6ico compound) = FUTURE** — 18 tes × 8 cube × 144 = 20736 — ยังไม่แตะ, ซับซ้อนเกินไปตอนนี้

## 7. Working Rules (ห้ามละเมิด)

- **No Geometry Construction**: ห้าม compute vertex/face/projection/coordinate — ใช้แค่โครงสร้าง combinatorial เป็น template map ข้อมูล: int ล้วน, LUT static, modular arithmetic
- scale/hyperbolic/4D = ฟังก์ชัน map บน address ไม่ใช่ space ที่ต้องสร้าง
- Lossless = decode → เปรียบเทียบค่าทุกตำแหน่ง (ห้ามเชื่อ encode-only)
- 20736 = window ที่เลือกใช้ ไม่ใช่ขอบเขตของระบบ

---

## ป้าย / state
- สถานะ rescope: **พิสูจน์ครบ (47/47 checks ใน 5 tests)** — พร้อม implement ต่อ
- สิ่งที่ยังไม่ทำ: container integration, GGUF จริง, seeker tools เชื่อม, 18tes (future)
