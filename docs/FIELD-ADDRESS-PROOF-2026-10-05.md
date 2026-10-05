# FIELD-ADDRESS-PROOF — 2026-10-05

**คำถามที่ปิด:** "field บอก address ของตัวเองได้จริงไหม และ address นั้นชี้ bytes ถูกต้องไหม"
**คำตอบ:** ได้ และถูก — พิสูจน์ด้วย memcmp กับ GGUF ต้นทางทุก tensor
**Receipts:** `fieldA.bin` 291/291 PASS (638.7 MB) · `fieldB.bin` 310/310 PASS (604.1 MB)
**Commit:** `e2427e4`

---

## 1. ปัญหาที่ตั้งต้น

`tools/geo_field_query.c` ทำ "tensor name → chain position → field bytes" ได้ แต่มีข้อจำกัด 3 ข้อ:

1. print แค่ 8 tensor แรก (`shown < 8`, บรรทัด 141)
2. ต้องมี baked field file
3. **ต้องมี source GGUF ด้วย** เพราะมัน memcmp verify — ถ้าไม่มี source มันเหลือแค่ count check

ผู้ใช้ถามว่า "ทำไมดู address ไม่ได้" ⇒ คำตอบจริงตอนนั้นคือ **ไม่มี source GGUF ที่ตรง** (`fieldA.bin` 310 tensors, GGUF บนเครื่องมี 291/290/273/775) ⇒ ไม่ใช่โค้ดขาด แต่ **field เก่า source หายไปแล้ว** — ซึ่งคือจุดขายของ field เอง (bake แล้วลบ source ได้)

## 2. ข้อค้นพบหลัก: field บอก address ของตัวเอง

ตรวจว่า `fieldA.bin` เป็น GGUF ที่สมบูรณ์ไหม ⇒ **เป็น** (header ครบ) ⇒ ∴ metadata อยู่ในตัว field เอง ⇒ ตรวจ KV ทั้งหมด:

```
kis.format.version   = 2
kis.layout.body_off  = 31680          <- จุดเริ่ม body
kis.layout.fpos[310] = 390749184, 0, 127626240, ...   <- chain position ของทุก tensor
```

**∴ address ของทุก tensor ถูกเก็บ *ในไฟล์ field เอง*** ⇒ อ่านได้โดยไม่ต้องมี source GGUF
⇒ นี่คือช่องที่ต้องปิดด้วยเครื่องมือ: `tools/field_address.c`

```
address(t) = body_off + fpos[file_idx(t)]
```

## 3. บั๊ก 2 ตัวที่เจอระหว่างทาง (และเป็นเหตุที่ต้อง verify)

**บั๊ก 1 — `arr_ptr` เป็น pointer ที่ค้างชี้**
`kv_walk` เก็บ `arr_ptr` เป็น pointer เข้า `infos[]` array ที่ถูกใช้ซ้ำ ⇒ พอ KV ตัวถัดไปเขียนทับ ⇒ `fpos` อ่านได้ 0 ทุกตัว
⇒ **แก้:** เก็บเป็น **offset จาก base** แล้วบวก `base` ตอนอ่าน

**บั๊ก 2 — `%llu` msys-gcc ไม่รับ**
printf พิมพ์ค่าผิดเงียบ ๆ ⇒ build ผ่านแต่ output เพี้ยน
⇒ **แก้:** `%I64u` ทั้งไฟล์

**บทเรียน:** ทั้ง 2 บั๊กนี้ *ผ่าน build* — เจอได้เฉพาะตอนรันกับไฟล์จริง (doctrine #7267: verify binary ทุกครั้ง)

## 4. Oracle: how the proof works

`tools/verify_field.c` — เทียบ 2 ทางที่ไม่พึ่งกัน:

| ขา | ที่มา | วิธี |
|---|---|---|
| **field** | `kis.layout.fpos[]` ในตัว field | `body_off + fpos[i]` → อ่าน bytes |
| **source** | GGUF tensor table | หา tensor ด้วย **ชื่อ** → อ่าน bytes |

⇒ **memcmp ทุก tensor** ⇒ ถ้าตรงทุกไบต์ ⇒ address ที่เก็บใน field *ถูกต้อง*

## 5. ผลการรันจริง (end-to-end)

รัน `build/dual_lazy_serve.exe` กับ GGUF จริง 2 ตัว ⇒ **23/23 PASS** (co-serve + evict isolation + recovery)
⇒ ในระหว่างนั้น **bake fieldA/fieldB ใหม่สด ๆ** จาก GGUF จริง (ไม่ใช่ field เก่า) ⇒ ∴ verify ด้วย field สด = end-to-end จริง

```
$ build/verify_field.exe build/fieldA.bin I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf
  field : build\fieldA.bin  v2  tensors=291  body_off=31680
  source: Qwen2.5-0.5B-Instruct-Q8_0.gguf  tensors=291
  verify_field: 291 PASS, 0 FAIL, 0 MISSING
    bytes proven at stored addresses: 669763072 (638.7 MB)
    RESULT: every field address resolves to the correct source bytes

$ build/verify_field.exe build/fieldB.bin I:/model/Qwen3-0.6B-Q8_0.gguf
  field : build\fieldB.bin  v2  tensors=310  body_off=35776
  source: Qwen3-0.6B-Q8_0.gguf  tensors=310
  verify_field: 310 PASS, 0 FAIL, 0 MISSING
    bytes proven at stored addresses: 633495552 (604.1 MB)
    RESULT: every field address resolves to the correct source bytes
```

**⇒ ∴ พิสูจน์ได้ว่า:** address ใน field = raw bytes ของ GGUF ต้นทาง **ไม่ใช่ delta encoding** — วางตรง ๆ ตาม address ⇒ ชี้แล้วได้ของจริง

**หมายเหตุ:** ก่อนหน้านี้ผมจับคู่ source ผิด (`fieldA.bin` เก่า 408MB = Qwen3-0.6B bake เก่า) ⇒ ได้ 0 PASS, 218 FAIL ⇒ **FAIL นั้นถูก** — ไม่ใช่บั๊ก field แต่คือหลักฐานว่า oracle ทำงาน (จับคู่ผิด → จับได้จริง)

## 6. ข้อจำกัดจริงที่เจอ

- **field v0 (เช่น `field.bin` 2.4GB, Qwen3-4B) ไม่มี `fpos[]`** ⇒ address ตรวจไม่ได้ ⇒ tool print `fpos[]=absent` ⇒ **field v2 เท่านั้น**
- **`kis.layout.fpos` index = file-index order** ไม่ใช่ chain-order ⇒ ต้อง map ให้ถูก

## 7. สถานะงาน (เรียงตามลำดับ)

| ข้อ | งาน | สถานะ |
|---|---|---|
| **1** | **verify address กับ GGUF จริง (field self-describing ถูกจริง)** | ✅ **เสร็จ 601/601 PASS** |
| 1b | rescope probe เป็น frustum composite 6 direction | ✅ เสร็จ (24/24, 21/21) — synthetic |
| **2** | **fog/pattern/frustum บน tensor จริง** | ✅ **เสร็จ 6/6 ทั้ง 2 โมเดล (864 face bytes, 0 differ)** |
| **3** | **SIFT forage loop บน 1M vectors จริง** | ❌ ยังไม่ทำ (`F.bin/off.bin/mem.bin` หาย ต้อง generate จาก `.npy`) |

## 8. เครื่องมือที่เพิ่ม

- `tools/field_address.c` — ดู address จาก field ลำพัง (ไม่มี source)
- `tools/verify_field.c` — พิสูจน์ address ↔ source bytes
- `tools/frustum_real.c` — fog/pattern walk บน tensor จริง
- Make targets: `make field-address`, `make verify-field`, `make frustum-real`

## 9. ข้อ 2 — fog/pattern บน tensor จริง (2026-10-05)

`tools/frustum_real.c` รันโมเดล fog/built บน field ที่ bake จริง แทนค่าคำนวณมือ:

- **anchor** = tensor จริงที่ address จริง (จาก `fpos[]`)
- **6 faces** = 6 ตำแหน่งใน tensor (deterministic, ดูแต่ position ไม่ดูค่า)
- **fog** = เดิน face ไหน = ปิด face นั้น (ทิ้งร่องรอย)
- **pattern** = 6-bit mask, **tombstone** = entry+exit

```
fieldA.bin (Qwen2.5-0.5B-Q8_0, 64 anchors)  6/6 PASS  384 face bytes, 0 differ
fieldB.bin (Qwen3-0.6B-Q8_0, 80 anchors)    6/6 PASS  480 face bytes, 0 differ
```

| ข้อ | พิสูจน์ | ผล |
|---|---|---|
| R1 | 6 faces เป็นตำแหน่งต่างกัน | PASS (real sizes) |
| **R2** | **bytes ที่ face == GGUF ต้นทาง ตำแหน่งเดียวกัน** | **864 compared, 0 differ** |
| R3 | 2 walker คนละเส้นทาง → fog ต่างกัน | ffn `0x39` vs attn `0x3f` |
| R4 | เดินซ้ำไม่เปิดอะไรใหม่ | 3 new → 0 new |
| R5 | tombstone entry+exit ระบุทางผ่าน | `0x24` (เข้า 2 ออก 5) |
| R0 | forward ไม่เคยถูกบล็อก | mask `0x3f` |

**⇒ ∴ R2 คือข้อสำคัญ: walk อ่าน bytes จริงจากโมเดล ไม่ใช่สำเนา**

**⇒ ∴ ของจริงที่เจอบน data: `output.weight` กับ `token_embd.weight` มี bytes *เหมือนกันเป๊ะ* (tied weights) ⇒ แต่เป็น *anchor คนละตัว* ที่ fog แยกกันได้** — ตรงกับ doctrine ที่ว่า pattern มาจาก *ร่องรอยของ walker* ไม่ใช่จากค่า

## 10. บั๊กที่ 3 — `field_address` print address ผิด (แก้แล้ว `70ec861`)

`fpos[]` ถูก index ด้วย **file tensor index ตรง ๆ** ไม่ใช่ chain order ⇒ การ sort ตาม chain ทำให้ print address ของ tensor ผิด (token_embd โชว์ `390786048` แทนที่จะเป็น `body+0 = 31680`)

`verify_field.c` (291/291) กับ `frustum_real.c` (384/384) ใช้ file-index ตรงมาตลอด ⇒ ∴ **field ถูกมาตลอด บั๊กอยู่ที่ `field_address` เอง**

