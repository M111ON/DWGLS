# DWGLS — ภาพรวมทั้งระบบ (หนึ่งหน้า)

> **MAP not COMPRESS** — เรขาคณิตคือ address space; coordinate = data.
> ไม่มี hash, ไม่มี lookup, ไม่มี 0 — ทุกอย่าง deterministic + replay ได้

## หลักการ (กฎทั้งหมด — รายละเอียดใน TIMELINE_WORKING_MODEL.md)

| # | กฎ | สาระ |
|---|---|---|
| 1 | **สนามนิ่ง ข้อมูลเคลื่อน** (§9) | field bake ล่วงหน้า, ข้อมูลวางครั้งเดียว; มีแค่ เปิด/ปิด link — ไม่มีอะไรถูกย้าย |
| 2 | **เสาเข็ม** (§5/§7) | วางแล้วไม่ขยับ — link reroute ได้, data ไม่ได้ |
| 3 | **ย่อฟรี ขยายจ่าย** (§15.8) | contraction = base-2 shift exact; expansion = leverage ต้องผ่าน gate |
| 4 | **Leverage gate** (§15.9) | ขยายเมื่อ `benefit > 0 && ROI ≥ 1` — knee = 5 (tesseract scale) |
| 5 | **อย่าเริ่ม base ต่ำสุด** (§15.7) | bottom = dead zone (plateau) — field base ≥ margin |
| 6 | **Ghost ไม่จอง 2ᵏ** (§15) | วางลึก = view B/2ᵏ + residual log — spike 4GB/40GB ไม่เกิด |
| 7 | **1 tensor ≥ 1 window** (§15.11) | registry addressing มีเพดาน — tax จริง 1.95× vs uniform 1.22× |
| 8 | **Lossless = decode เปรียบทุกค่า** | ratio < 1 ต้องพิสูจน์ด้วย decode — `geo_codec_verify()` = binary truth |

## ตัวเลขศักดิ์สิทธิ์ (จุดตัดของข้อจำกัด — ไม่ใช่เลขเลือก)

```
20736 = 12⁴ = 2⁸·3⁴ = 144² = 1728×12        ← คำตอบเดียวในช่วง 16-bit (พิสูจน์แล้ว)
12    = 4×3 = equal-triangle 4-subdivision × Peano 3-adic
144   = 16·9 = 4²·3²  (16:9 bake ใน window)  576 = edges+faces 6ico
ladder 20736→6912→2304→768→256  (÷3 สลับแกน)  กลับ 16:9 พอดี
```

## สถานะ test — TIER1 47/47 เขียว

```
make tier1        # ทั้งหมด 47 ตัว (self-contained)
make graft-llama  # ขั้น ③ ต้องมี I:/llama + Qwen จริง

หลักฐานแกน:   test_tess_sacred 27/27 · index_frame 7/7 · scale_log 10/10
              frame_seek 8/8 · magnify 12/12 · hex_delta 10/10
Subdivide:    test_tess_subdivide 15/15 (aperture 3/4/7 rule-only: 4-ladder
              1→4→16→64→256, 3-ladder 1→3→9→27→81, roundtrip lossless)
Scale wire:   test_tess_scale_wire 11/11 (depth d → scale w: 4-ladder 2 หลัก
              แรกใน w-axis, 2 หลังใน pos; a_w แยก depth-d cell ทุกระดับ)
Hyper seeker: twin_seeker_test 10368/10368 (KIS+Hyper) · twin_seeker_hard_test
              ALL PASS (20736/20736 roundtrip — แก้ axis semantic แล้ว)
Ghost/gate:   test_tess_ghost 30/30 · leverage 24/24 · registry_gate 22/22
GGUF main:    test_gguf_box 16/16 · window_chain 11/11 · real_gate 15/15
              multi_model 29/29 (4 โมเดลจริง: knee=7, tax 1.19-2.24×)
              graft_llama 12/12 (logits == ไฟล์ต้นฉบับ bitwise)
```

## GGUF main — ครบ 3 ขั้นแล้ว

```
① dims จริง (int64 per spec) เข้า mock header        ✅ test_gguf_box 16/16
② tensor chain ลง window 20736 (inference order)     ✅ window_chain 11/11
③ llama.cpp อ่านผ่าน graft — logits bitwise identical ✅ graft_llama 12/12
   + generation 40 tokens ผ่าน graft == ไฟล์ต้นฉบับ (tools/gguf_graft_generate.c)
   + body จาก KIS field (bake → rebuild) → generation == เดิม (graft-field 5/5)
   + tokenizer KV ลง field → header 5.9MB → 20KB (292×) (graft-page 8/8)
   + lazy serve: KV ใน memory + field mmap on demand, ไม่ materialize 670MB
     (lazy-serve 12/12 ×3 โมเดล — WS 2065 vs ref 1917 MB, +8% เพราะ user path)
   + tokenizer KV อยู่ใน field windows — durable header 20KB (292×), rebuild
     ทุก serve (index header + payload windows; fill: bias=0/scale=1.0/head=embd)
   + zero-copy: callback repoint t->data เข้า field mmap → load 0.36s/0.9% resident,
     generation หน้า-in เฉพาะที่ใช้ (Qwen 25,639/32,587 win, token_embd อ่าน 0.1%)
     WS peak 1289 MB ≈ file-mmap 1277 (copy mode เคย +148 MB) — §15.18
```

**ผลลัพธ์ไฟล์จริง (Qwen2.5-0.5B Q8_0, 291 tensors, 630M elements):**
gate ให้ base 7 → storage 30,391 → **238 windows = 128×** — registry tax จริง 1.95×
(465 spans vs uniform 291) — วางครบ 291/291, replay deterministic ครบ

> **หน่วยต้องตรง:** 238 windows = **view** (E/2⁷ element-slots) — field เก็บ
> byte เต็ม (Q8: 669.7 MB) ต้อง **32,300 windows** — 128× คือ view compression
> (§11.5: เล็ก+lossless+self-contained เลือกได้สองอย่าง)

## ไฟล์สำคัญ

| ไฟล์ | บทบาท |
|---|---|
| `core/geo_param_grid.h` | parameterized geometry — start here |
| `core/gguf_box.h` / `gguf_reader.h` | GGUF box (dims fix `hdr_size=24` อยู่ที่นี่) |
| `core/tri_hex_tess.h` | equal-triangle floor (20736) |
| `core/geofs_core.h`, `core/geo_kis_container.h` | GeoFS + container |
| `tests/test_tess_*.c` | หลักฐานตัวเลขศักดิ์สิทธิ์ + ghost + gate |
| `tests/test_gguf_*.c` | GGUF main (box/window chain/real gate/graft) |
| `docs/TIMELINE_WORKING_MODEL.md` | รายละเอียดครบ 15 บท (661 บรรทัด) |

## แผนต่อ (เปิดไว้)

1. **gate ฉบับนับ registry tax** — ROI รวม storage + per-tensor spans → base จริงต่อโมเดล (SmolLM2 2.24× ที่ base 7 → อาจ < 7)
2. ~~**หลายโมเดลผ่าน gate**~~ ✅ เสร็จแล้ว — §15.12: knee สากล 7, tax 1.19× (LFM 2.6B) ถึง 2.24× (SmolLM2 360M)
3. ~~**ขั้น ③ เต็ม — lazy serve**~~ ✅ §15.15-18: KV ใน memory + tokenizer KV ใน field windows (header 20KB) + zero-copy serve (generation แตะ field, load 0.36s) — เหลือ link toggle (lifecycle จริง)
4. **18tes (20736 เต็ม)** — 18 tesseract × 8 cube × 144 — ยังไม่ implement

## ⚠️ ข้อควรระวัง

- **`master` (I:/DWGLS) ยังมี `core/gguf_box.h` ตัวเก่า `hdr_size=16` (heap overflow)** —
  native-fs มีตัว fix `hdr_size=24` แล้ว — merge ระวังตัวเก่าทับตัวใหม่
- งาน geometry อยู่บน branch มีงาน (merge branch / feat/geo-native-fs) — worktree โดน isolate
  ไปสาย audio (`feature/geo-audio-codec`) ไม่มีไฟล์งาน — ตรวจ `git worktree list` ก่อนเริ่ม
- Worktree เป็น Freebuff-managed — ไฟล์ที่สร้าง (test/docs) ยังไม่ commit

## เริ่ม session ใหม่ยังไง

```bash
make tier1                  # ตรวจฐาน 42/42
./build/test-test_gguf_real_gate  # หรือรัน test ที่สนใจ
# อ่าน docs/TIMELINE_WORKING_MODEL.md §14-15 (หลักฐานล่าสุด) ก่อนแก้โค้ด
```
