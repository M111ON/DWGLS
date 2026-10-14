# CLEANUP PLAN 2026-10-14 — ล้าง build + ไฟล์ซ้ำ ให้โครงสร้างลีน

**สถานะ: ✅ ทำแล้ว (2026-10-14)** · **เจ้าของงาน:** user
**บริบท:** ดิสก์ `I:` เต็ม **99%** (เหลือ ~1.5 GB จาก 112 GB) — ann-sync ถูกบล็อกไม่ให้ rebuild index
**ขอบเขต:** เฉพาะไฟล์ที่ **สร้างใหม่ได้** (gitignored / derived) ไม่แตะ source, ไม่แตะ git

---

## ✅ ผลจริง (วัดหลังทำ)

| | ก่อน | หลัง |
|---|---|---|
| ดิสก์ I: ว่าง | 1.5 GB (99%) | **6.2 GB (95%)** |
| build/ | 7,237 MB | ~2,440 MB |
| index vs store | 59,756 ≠ 59,967 | **59,967 = 59,967 = 59,967 (IN SYNC)** |
| live :8096 entries | 59,756 | **59,967** |

**ลบจริง 4,901.7 MB** (9 ไฟล์ สร้างใหม่ได้) · **ย้ายไปที่จอด 684 MB** (ยังไม่ลบ)

### ⚠️ บทเรียนสำคัญ: "ย้าย" ไม่คืนพื้นที่ถ้าอยู่ไดรฟ์เดียวกัน
การ `mv` ไฟล์ภายใน `I:` → `I:` **ไม่ได้คืนไบต์เลย** (แค่เปลี่ยนชื่อ path)
ยอดที่คืนจริง 4.8 GB มาจาก **การลบเท่านั้น** — ส่วนที่จอด (`zip/`) ยังกินที่เท่าเดิม
ถ้าต้องการคืนพื้นที่จริงจาก Tier 2/3 ต้องลบ หรือย้ายข้ามไดรฟ์

---

## 🐞 บั๊กที่เจอระหว่างทำ (แก้แล้ว — ใหญ่กว่าที่คิด)

**`ann_sync.py` เขียน index ผิดชื่อ** → index **แช่แข็ง** ไม่ตรง store มา 4 วัน

```
ann_sync: REFUSING regrow — server serves ...ann_index.jsonl,
                                we built ...kv_index.jsonl
```

สาเหตุ: rename `kv_*` → `ann_*` (ann `5b20435`, 11 ต.ค.) แก้ที่ `build_index.py`
และ server แต่ **ลืม `ann_sync.py`** → ทุกรอบมันสร้าง `kv_index.jsonl` แล้วถูก
ปฏิเสธ regrow → sync รายงาน "in sync" (signature ตรง) แต่ไฟล์ที่เสิร์ฟไม่ขยับ

**แก้:** `INDEX_OUT` default → `ann_index.jsonl` (ตรงกับ `build_index.py`)
**พิสูจน์:** `ann_sync.py --force` → built 59967 (43.7s) → `regrow ok (nent=59967 K=8)`
→ live เห็น 59,967 ค้นหาปกติ

> หมายเหตุ: `ann_sync.py` อยู่ใต้ `I:/tools/cloud_ws/` ซึ่ง **ไม่ได้อยู่ใน git** —
> แก้บนดิสก์แล้ว ทำงานจริง แต่ commit ไม่ได้

---

## รายละเอียดที่ทำไป

### Tier 1 — ลบจริง (4,901.7 MB) ✅
9 ไฟล์ สร้างใหม่ได้ทั้งหมด (ต้นฉบับ `I:/model/*.gguf` ยังครบ + `tools/tess_gguf_pack.c` ยังอยู่)

### Tier 2/3 — ย้ายไปที่จอด `zip/` (684 MB) ⏸ ยังไม่ลบ
- `zip/sift.tar.gz` + `siftsmall.tar.gz` (173.6 MB) — ดาวน์โหลดมา สร้างใหม่ไม่ได้
- `zip/ann-kv-legacy/` (247.3 MB) — `kv_index.jsonl` **เหมือน `ann_index.jsonl` เป๊ะ** (md5 ตรง) เป็นชื่อ fallback เก่า
- `zip/memcore-legacy/` (263.4 MB) — `fact_store.sqlite3` (v1) + `fact_store.twin`; **โค้ดหลายไฟล์ยังอ้างชื่อ `fact_store.sqlite3`** → ไม่กล้าลบ

### Bonus — tmp ค้าง
`data/memcore/ann_index.jsonl.tmp` (225 MB) — artifact ที่ regrow ทิ้งไว้ → ลบแล้ว

---

## หลักการ (ยึดตาม file policy ที่ user ตั้งไว้)

1. ของเก่าไม่ลบถ้าไม่แน่ใจ — ใช้ **ย้ายไปที่จอด** ก่อน แล้วค่อยลบเมื่อยืนยัน
2. ไฟล์ที่ **สร้างใหม่ได้ + ใหญ่ + งานจบแล้ว** → ลบ เหลือแค่ตัวสร้าง (generator)
3. ไฟล์ที่ **สร้างใหม่ไม่ได้** (ดาวน์โหลดมา) → **ห้ามลบ** ย้ายไปที่จอด
4. ทุกอย่างอยู่ใต้ `build/` ซึ่ง gitignored → **ลบแล้ว git ไม่กระทบ**
5. ไม่มี process ไหนถือไฟล์เหล่านี้อยู่ (ตรวจแล้ว: ว่าง)
6. **การย้ายภายในไดรฟ์เดียวกันไม่คืนพื้นที่** — ต้องลบจริงหรือย้ายข้ามไดรฟ์

---

## Tier 1 — ลบได้เลย (สร้างใหม่ได้ทั้งหมด) → คืน **4.9 GB**

| ไฟล์ | ขนาด | สร้างใหม่จาก | เหตุผล |
|---|---|---|---|
| `build/qwen25.tesspack` | 718.8 MB | `tess_gguf_pack` + `I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf` (มีอยู่) | เทสเก่า 26 ก.ย. |
| `build/qwen25.tesspack.gz` | 676.4 MB | กดจาก .tesspack ข้างบน | สำเนาซ้อน |
| `build/qwen3fresh.tesspack` | 672.4 MB | `tess_gguf_pack` + `I:/model/Qwen3-0.6B-Q8_0.gguf` (มีอยู่) | เทสเก่า 26 ก.ย. |
| `build/graft_full.gguf` | 650.1 MB | `gguf_graft_page` จาก gguf ต้นฉบับ | artifact เทส graft |
| `build/field.bin` | 644.4 MB | `gguf_lazy_serve` สร้างใหม่ (Qwen2.5 675.7 MB) | field window เก่า 8 ต.ค. |
| `build/fieldA.bin` | 644.4 MB | เดียวกัน | สำเนา 5 ต.ค. |
| `build/fieldB.bin` | 609.8 MB | เดียวกัน (Qwen3 639.5 MB) | สำเนา 5 ต.ค. |
| `build/test_rail.gcube` | 280.3 MB | `bake_gcube` จาก gguf | artifact เทส rail |
| `build/tesspack_hdr.tmp` | 5.1 MB | tmp ค้าง | ขยะ |
| **รวม** | **4,901.8 MB** | | |

**หมายเหตุ field\*.bin:** ทั้ง 3 ไฟล์คือ field window ของ GGUF (ขนาดตรงกับต้นฉบับ 675/675/639 MB)
สร้างใหม่ได้จาก `gguf_lazy_serve` / `frustum_real` — เก็บไว้แค่ตัวที่ต้องใช้รันรอบถัดไป ไม่ต้องเก็บ 3 ก๊อป

---

## Tier 2 — ย้ายไปที่จอด (สร้างใหม่ไม่ได้) → คืน **160.5 MB**

| ไฟล์ | ขนาด | เหตุผล |
|---|---|---|
| `build/sift.tar.gz` | 160.5 MB | **ดาวน์โหลดมา** — สร้างใหม่ไม่ได้ ต้องโหลดใหม่ |
| `build/siftsmall.tar.gz` | 5.1 MB | เดียวกัน |

**ที่จอด:** `I:/DWGLS-native-fs/zip/` (ตาม policy: zip → zip/) — ถ้าโหลดใหม่ได้ตลอด ก็ลบได้ แต่แนะนำย้ายก่อน

---

## Tier 3 — ไฟล์ซ้ำในระบบอื่น (คนละเรื่องกับ build) → คืน **~487 MB**

| ไฟล์ | ขนาด | สถานะ | ข้อควรระวัง |
|---|---|---|---|
| `/i/tools/ann/data/memcore/kv_index.jsonl` | 235.6 MB | index ชื่อเก่า (rename เป็น `ann_*` แล้ว) | **ต้องยืนยัน**ว่าไม่มีอะไรอ่านชื่อเก่า |
| `/i/tools/cloud_ws/memcore/fact_store.sqlite3` | 153.7 MB | legacy store (v1, nemotron 2048-dim) | **ต้องยืนยัน**ว่าไม่มีอะไรใช้ |
| `/i/tools/cloud_ws/memcore/fact_store.twin` | 97.5 MB | ไม่รู้จัก ต้องถามเจ้าของ | **ต้องยืนยัน** |

> Tier 3 ผมยังไม่แนะนำให้แตะในรอบนี้ — มันคนละระบบกับที่คุณสั่ง (build) และต้องเช็ค consumer ก่อน

---

## Tier 4 — ทางเลือก (ยังไม่ทำ) → คืน **~69 MB + index**

| เรื่อง | คืนได้ | หมายเหตุ |
|---|---|---|
| dedup `v1-migrate` (38.4 MB ซ้ำกับ `brainstorm` 100%) | 38 MB | ต้อง user ยืนยันว่าลบได้ |
| dedup แถว `compartment_examples` (920 แถว → 346 แบบ) | ~60 MB | ต้องแก้ daemon ไม่ให้ดัมป์ซ้ำ |
| index vec เก็บเป็น binary แทน text | ~190 MB | ต้องแก้ `build_index.py` + server (งานใหญ่) |

---

## ลำดับที่แนะนำ (ทำตามได้เลย)

```
ขั้น 1  ย้าย sift*.tar.gz → zip/          (กันพลาดก่อน)
ขั้น 2  ลบ Tier 1 ทั้ง 9 ไฟล์             → คืน 4.9 GB
ขั้น 3  รัน ann_sync.py                   → index ตาม store ทัน (211 แถวใหม่)
ขั้น 4  (ตัวเลือก) Tier 3 หลังยืนยัน consumer
```

**หลังขั้น 2:** ดิสก์เหลือ ~6.4 GB (จาก 1.5 GB) → พอให้ ann-sync rebuild (ต้องการ ~600 MB + margin)
**หลังขั้น 3:** ระบบครบวงจรอีกครั้ง (index ตรง store)

---

## สิ่งที่ **ไม่** แตะในแผนนี้

- source code ทั้งหมด (`.c`, `.h`, `.py`, `Makefile`)
- อะไรก็ตามที่ git track (ตรวจแล้ว: ไม่มีไฟล์ >20 MB ใน git)
- `I:/model/*.gguf` — ต้นฉบับ ห้ามแตะ
- `.exe` 467 ไฟล์ (51 MB รวม) — ไม่คุ้มเสี่ยง ลบทิ้งได้ทีหลังถ้าต้องการ
- `memory-daemon` task (Disabled) — user สั่งไม่ให้เปิด
