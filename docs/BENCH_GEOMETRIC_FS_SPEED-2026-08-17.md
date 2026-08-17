# GEO_SPEED_BENCH — 3-way Geometric FS Speed Comparison

**Date:** 2026-08-17 · **Tool:** `tools/geo_speed_bench.c` · **Target:** `make geo_bench`

เปรียบเทียบ 3 ทางบน Windows (NTFS, gcc mingw64, mmap แบบเดียวกับ dramtile_store):

| Path | หลักการ |
|---|---|
| **MAP** | `dram_addr()` จริงจาก `geo_dram_tile.h` → `offset = addr × CHUNK_SZ` (coordinate = address, no hash) ผ่าน mmap |
| **CLASSIC** | contiguous file + index array (traditional FS floor) |
| **RAM** | heap memcpy เท่านั้น (physical ceiling) |

เงื่อนไข: MAP storage overhead ≤ 50% ของ logical bytes · ทุก read pass verify ด้วย memcmp (lossless = PASS)

---

## Workload 1 — Chunky tensors (GGUF-like)

500 tensors · 256KB-512KB each · logical **235.93 MB** · MAP window 268.44 MB (**13.8% overhead**)

| Operation | MAP | CLASSIC | RAM | สรุป |
|---|---|---|---|---|
| write cache | 2.06–2.62 GB/s | 2.03–2.12 GB/s | 2.86 GB/s | ≈ เท่ากัน |
| write flush (sync จริง) | 0.03–0.09 GB/s | 0.05 GB/s | — | MAP แพ้ 1.56× (window ใหญ่กว่า) |
| read seq cold | 1.75–1.84 GB/s | 3.21–3.44 GB/s | — | **CLASSIC ชนะ ~1.9×** |
| read seq warm | 3.41–3.52 GB/s | 3.38–3.50 GB/s | — | ≈ เท่ากัน |
| read rand cold | 1.52–1.89 GB/s | 3.35–3.46 GB/s | — | **CLASSIC ชนะ ~1.7×** |
| read rand warm | 2.97–3.28 GB/s | 3.12–3.21 GB/s | 3.44 GB/s | ≈ เท่ากัน |

> **Verdict: MAP ไม่ชนะ speed บน chunk tensor** — disk อ่าน sequential ชนะ stride/Hilbert spread และ CLASSIC ได้ 0% overhead ฟรีๆ

## Workload 2 — Tiny random-access objects (KV-cache-like) ★ MAP ชนะ

15000 objects · 96–160 B (avg 128 B) · logical **1.92 MB** · MAP window 2.42 MB (**26% overhead** ภายใน budget)

| Operation | MAP | CLASSIC | ต่าง |
|---|---|---|---|
| write | **81 ns/op** (1.59 GB/s) | 209 ns/op (0.61 GB/s) | **2.6×** |
| read random (copy+verify) | **102 ns/op** | 2087 ns/op | **20×** |
| read random (zero-copy, pointer ตรง) | 138 ns/op | — | syscall-free |

> **Verdict: MAP ชนะ 20× ที่ tiny random access** — fseek+fread จ่าย syscall ต่อ op (~2000 ns) ในขณะที่ geometric addressing = pointer ตรง (~100 ns) **syscall overhead (ไม่ใช่ copy overhead) คือตัวที่ฆ่า CLASSIC**

## Workload 3 — KV-cache-like (mixed sizes) ★ MAP ชนะทั้ง speed และ overhead

100,000 objects · 64B..4KB mixed (30% 64B … 2% 4KB) · logical **41.99 MB** · **size-classed slots** (แต่ละ object ใส่ class ที่พอดีกับขนาด)

| Operation | MAP | CLASSIC | ต่าง |
|---|---|---|---|
| write | **181 ns/op** (2.32 GB/s) | 511 ns/op (0.82 GB/s) | **2.8×** |
| read random (copy+verify) | **283 ns/op** | 5090 ns/op | **18×** |
| read random (zero-copy) | 337 ns/op | — | syscall-free |
| overhead | **0%** (window = logical) | 0% | เสมอ (MAP ไม่เสีย padding) |

> **Verdict: size-classed slots ชนะ 2 ทาง** — ได้ 18× speed (syscall-free) **และ** 0% overhead เพราะ slot class พอดีกับขนาด ไม่เหมือน fixed-slot (งาน 2 ที่เสีย 13.8–26%)

## Workload 4 — REAL KV-cache จาก GGUF จริง (Qwen2.5-0.5B) ★ ยืนยันบนโมเดลจริง

อ่าน metadata จริงจาก `I:\DWGLS\build\qwen05-direct.gguf` ผ่าน `gguf_reader.h`:
24 layers · **7 n_kv_head** · head_dim 128 · n_ctx 2048 → KV block = **256 B** (K/V 1 (layer,pos)) · logical 25.17 MB

| Operation | MAP | CLASSIC | ต่าง |
|---|---|---|---|
| PREFILL write | **123 ns/block** (2.09 GB/s) | 322 ns/block (0.79 GB/s) | **2.6×** |
| DECODE read (attention, zero-copy verify) | **38 ns/block** (6.74 GB/s) | 2041 ns/block (0.13 GB/s) | **54×** |
| overhead | **0%** (window = logical) | 0% | เสมอ |

> **Verdict: ยืนยันบนโมเดลจริง** — block 256B ของ KV-cache จริง = แถวที่ MAP ชนะสุด (54× read, 2.6× write) ที่ 0% overhead

## Wire เข้า engine: DtSlotRegion (hybrid, 13/13 test PASS)

เพิ่ม `DtSlotRegion` ใน `core/infra/dramtile_store.h` — direct geometric addressing (offset = addr × slot_sz, collision-free, O(1), zero-copy) แยกจาก hash path:

```
ช่องว่างที่เจอ: dt_put_kv/dt_put ใช้ hash 512 slots (addr % 512)
              → KV จริง 24×2048 = 49152 blocks → collide แน่นอน (พิสูจน์ใน test B1)
ทางแก้:      DtSlotRegion — dense geometric address ตรงๆ, ไม่มี hash, ไม่มี directory
```

`tests/test_hybrid_kv.c` (13/13 PASS, ใน TIER1):
- **A** slot put/get ครบ 49152 blocks → lossless
- **B** hash-based put → collision เกิดจริง (ต้องใช้ slot)
- **C** hybrid พร้อมกัน: weights (DRamTileStore contiguous) + KV (slots)
- **D** speed จริง: MAP **60 ns/blk** vs CLASSIC **2139 ns/blk** → **35.8×** (lossless)
- **E** twin disk mmap → reopen → reload lossless

```
ตำแหน่งทางออก: chunky/weights → DRamTileStore (contiguous, directory)
               tiny/KV      → DtSlotRegion  (direct address, syscall-free)
```

## Workload 5 — GGUF จริง (Qwen3-4B-Instruct, bimodal) ★ RAM hot path: MAP แพ้เล็กน้อย

model จริง `F:\model\zimage\Qwen3-4B-Instruct-2507-Q4_K_M.gguf` (398 tensors, 2.49 GB) — distribution **bimodal**:
- **145 tiny** (<64K, รวม 784 KB): norms 512B×72 + q/v/k norms <64K
- **253 big/huge** (1.4 MB–319 MB): attn/ffn + token_embd 319MB

```
วิธีวัด (honest): ใหญ่ใช้ contiguous เหมือนกันทั้ง 2 layout → ตัดออก (cancels out)
                benchmark เฉพาะ 145 tiny ที่เป็นตัวต่างจริง
                (RAM hot path — พอดีกับ RAM 7.9GB ของเครื่อง, ไม่มี swap)
```

| Operation | HYBRID (size-classed slot) | CONTIG (dense array) | ต่าง |
|---|---|---|---|
| write | 2.54 GB/s | 2.92 GB/s | MAP แพ้ ~1.15× |
| read seq | 5.73 GB/s | 7.31 GB/s | MAP แพ้ ~1.28× |
| read rand | 7.65 GB/s | 8.94 GB/s | MAP แพ้ ~1.17× |
| lossless | 4/4 PASS | 4/4 PASS | — |
| overhead | **57.2%** (size-class round ขึ้น) | 0% | MAP แพ้ |

> **Verdict: ใน RAM (hot path ทุกอย่างเป็น pointer แล้ว) hybrid slot แพ้ dense array เล็กน้อย (1.2–1.3×)** — ต่างกันแค่ addressing (class_base+pos = 2 add) vs dense (1 add) + cache locality แย่ลงเมื่อ class แยก region นี่คือ**ราคาจริงของ geometric layout ใน RAM** และตรงข้ามกับ KV-cache ที่ชนะ 54× (เพราะ CLASSIC จ่าย syscall ต่อ op)

> **ตีความรวมกับ Workload 2-4**: MAP ชนะเมื่อจ่าย syscall (disk/cold path: fseek+fread ~2000ns/op) — **ไม่ชนะเมื่อทุกอย่างเป็น pointer แล้ว** (RAM hot: แพ้ 1.2–1.3×). สรุป: **geometric layout ให้ค่าเฉพาะตอน data ต้องข้าม kernel boundary (disk/KV spill) — ใน RAM, dense array ชนะ**

---

## ข้อสรุปเชิงออกแบบ (honest)

1. **Speed ของ MAP ชนะเฉพาะโหลด tiny/random-access** (KV cache, small tensors, index tables) — 18–54× ที่ 0% overhead (size-classed / fixed-slot พอดี)
2. **Chunky/large tensors: ต้องใช้ contiguous layout** — ไม่จ่าย window padding ที่ 13.8% เพื่อได้ speed เท่ากัน
3. **ใน RAM hot path: dense array ชนะ slot เล็กน้อย (1.2–1.3×)** — geometric addressing จ่าย addressing overhead + cache locality ที่แย่ลง
4. **เสาหลักที่เหลือของ MAP** = deterministic O(1) pointer access (ไม่มี syscall), zero-copy handoff, และ multi-scale views (capability) — ไม่ใช่ raw throughput ของเทนเซอร์ใหญ่
5. **overhead ที่แท้จริงของ MAP = fixed-slot padding** — แก้ได้ด้วย **size-classed slots** (KV workload = 0% ส่วนเกิน) ตัวเลือก slot design ตัดสินใจที่ workload

## หมายเหตุวิธีวัด (สำหรับ replay)

- mmap pattern ตาม `dramtile_store.h` (`dt_store_init_twin`)
- cold = fresh mapping/open (OS page cache ยังมีข้อมูล — ไม่ใช่ power-off cold จริง)
- write flush เทียบกันตรง: MAP = `FlushViewOfFile`, CLASSIC = `fflush+FlushFileBuffers`
- ทุก read ผ่าน `memcmp` กับ source → PASS หมายถึง lossless จริงทุก row

**ไฟล์:** `tools/geo_speed_bench.c` (synthetic 3 workloads) · `tools/geo_kv_real_bench.c` (KV-cache จาก GGUF จริง) · `tools/gguf_hybrid_bench.c` (GGUF จริง bimodal) · **Build:** `make geo_bench` · **Run:** `./build/geo_speed_bench`, `./build/geo_kv_real_bench <model.gguf>`, `./build/gguf_hybrid_bench <model.gguf>`