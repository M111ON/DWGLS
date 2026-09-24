# Small-Batch GPU Overhead Report — DRamTile + GearLock + JetBridge vs Normal Path

**Date:** 2026-09-23
**Device:** NVIDIA GeForce GTX 1050 Ti (sm_61), CUDA Toolkit 12.6 (`I:\cuda_temp`)
**Question (owner):** ทดสอบ DRamTile + GearLock + JetBridge ลด overhead GPU จน small batch ใช้งานได้ — และถ้าไม่มีระบบนี้ ต้องรอ batch เท่าไหร่ถึงคุ้ม overhead?
**Status:** GTX 1050 Ti ทั้ง 3 bench ผ่านครบ — 31/31 + 5/5 + 5/5 PASS · **Colab T4 cross-machine รันแล้ว** (§7, 2026-09-24, 3/3 ALL_BENCH_OK) · **Colab TPU v5e รันแล้ว** (`bench/tpu_jax_bench.py`, ดู `docs/DWGLS-TPU-MAPPING-2026-09-24.md`)

---

## 1. TL;DR

| ระบบ | batch=1 ได้ไหม | ต้องรอ batch เท่าไถึงคุ้ม (normal path) |
|---|---|---|
| **มี** DRamTile+GearLock+JetBridge | **ได้ ทันที** — CPU build 24 ns, 12 wants → 1 dispatch = 3.05 µs/want | ไม่ต้องรอ |
| **ไม่มี** (normal path) | ไม่ได้ — จ่าย 27–35 µs/chunk = 270–342× floor | **≥ 512 chunks (32 KB)** ถึงเริ่มคุ้ม, **≥ 1024 chunks (64 KB)** ถึงเกือบเต็มประสิทธิภาพ |

คุณค่าของระบบไม่ใช่แค่เลข 10:1 บน paper แต่คือ **eliminate การ stall**: normal ต้องเก็บ wants 512–1024 ชิ้นก่อนยิง (delay สูง), ระบบบีบให้ยิงได้เลยที่ batch=1 ด้วยต้นทุนเทียบเท่า normal batch ≈ 12–16

> **Design intent (owner, 2026-09-24):** GPU coalescing ที่วัดได้ทั้งหมดนี้เป็น**ผลพลอยได้** ไม่ใช่จุดประสงค์ของ JetBridge ระบบที่แท้จริงคือ rail design — pipeline เป็นรางรถไฟ/ท่อน้ำ อะไรที่อยู่นอกรางจะหยุดเดินเมื่อ timeline หลักหยุด (rail_sync); `fibo_spine + ribcage` (ก้างปลา) แตกกิ่งออกจาก timeline เปิดทุก 12 tick (หลัง tick 11 จบ ก่อนเข้า tick 13) เพื่อ**รอข้อมูลที่มาช้าแต่คาดการได้** (มีตัวตนในระบบแล้ว — เบรครอประกอบ); `jet_puller` คือฟองอากาศวิ่งบนราง mod-12 (~12× เร็วกว่าปกติ) ทะยานไปดักรอ sync กลับที่ mod11=จบ/mod12=filled แล้วกลับเข้าเส้นทางหลักที่ mod1 ของรอบใหม่ — wants ที่กองในกิ่งรอจึงออกมาเป็น dispatch เดียวพอดี (memory #6205)

---

## 2. Benches ที่รัน

### 2.1 CPU path — `tests/test_gpu_small_batch.c` → 31/31 PASS

อยู่ใน TIER1 (Makefile:163) หลัง `test_gpu_pipeline`

| ตัววัด | งบ (spec) | ผล |
|---|---|---|
| T1–T2 correctness: batch=1 + sweep {1..256}×base{0,100} — addr, offset=addr×64, XOR checksum, unique | ทุก entry ตรง | PASS |
| T3 warm ctx — build ไม่แตะ spine/gear state, epoch +1 ต่อ build | invariant | PASS (first bridge ที่ tick 11, ไม่ใช่ 12 — restart-at-1) |
| T4 GearLock world boundary — 128×162=20736 (sacred แยกจาก implementation) | oracle จากสเปค | PASS (40×4+2 → world 1 ตรง floor(162/162)) |
| T5 JetBridge coalesce — schedule อ้างอิง oracle ที่เขียนใหม่จาก spec ไม่ใช่จากโค้ด | first bridge=11, count=tick−1 | PASS: 120 wants → 11 dispatches = **≥10:1** |
| T6 DRamTile address compute | < 500 ns/call | **6.4 ns/call** |
| T7 batch=1 per-call (warm persistent ctx) | < 5 µs | **24 ns/call** |
| T7 cold (init ใหม่ทุก call) | — | 18.4 µs/call → **ratio 778×** |
| T8 sweep ns/chunk | — | **~24.5 ns คงที่** จาก 1→1024 chunks |

**Fixed cost เดียวที่เหลือ:** `geo_pipeline_init` memset ~535 KB GeoPipelineCtx → **ห้าม init ต่อ call, ปล่อย ctx ค้าง**

Mutations ที่เคย fail ได้: bridge condition 12→13 (11 fails, count fails), GearLock 162→163 (crossing fails), ตัด epoch increment (A3 fail)

### 2.2 GPU launch latency — `bench/gpu_launch_bench.cu` → 5/5 PASS

| ตัววัด | งบ | ผล |
|---|---|---|
| L1 empty kernel launch (min-of-500) | < 20 µs | **13.5 µs** |
| L2 H2D 64 B | < 20 µs | **8.9 µs** |
| L3 batch=1 เต็ม (H2D + tiny kernel) | < 50 µs | **16.5 µs** |
| L4 naive 12-dispatch (normal) | — | 203.8 µs |
| L4 bridge 1-dispatch (12 wants) | ≥3× ถูกกว่า | 17.0 µs → **ratio 11.99×** |

ตรงกับ结构性 12→1 launch reduction จาก spec

### 2.3 Batch break-even — `bench/gpu_batch_break_even.cu` → 5/5 PASS (รันซ้ำ 2 รอบ ตรงกัน)

นิยาม (ตั้งก่อนรัน, independent ของโค้ด under test):
- `full(N)` = H2D(N×64 B) + kernel + sync, min-of-R
- `floor` = ns/chunk ดีสุดใน sweep (throughput-bound tail)
- `pct(N)` = launch_floor / full(N) — launch share (H2D เป็น data ไม่ใช่ overhead; launch+H2D latency+GPU kernel floor เป็น fixed ที่ batching มีเพื่อ amortize)

| N | full µs | ns/chunk | launch % |
|---:|---:|---:|---:|
| 1 | 27.0–35.0 | 27,000–35,000 | 34–40% |
| 16 | ~36 | ~2,270 | ~30% |
| 256 | ~64–72 | ~251–284 | ~17% |
| 512 | ~86–95 | ~168–185 | ~13% |
| 1024 | ~129–138 | ~126–135 | ~8.5% |
| 4096 | ~419–440 | ~102–108 | ~2.6% |
| 65536 | ~8,400 | ~128 | 0.1% |

**Break-even (คำตอบของคำถาม "ต้องรอ batch เท่าไถึงคุ้ม"):**

| เกณฑ์ | Normal path ต้องรอ |
|---|---|
| per-chunk ≤ **2× floor** (เริ่มพอใช้) | **512 chunks (32 KB)** |
| per-chunk ≤ **1.5× floor** (เกือบเต็ม) | **1024 chunks (64 KB)** |
| launch share ≤ 10% | 1024 chunks |
| batch=1 | 270–342× floor — ไม่คุ้ม |
| floor (best) | **100–102 ns/chunk** |

หมายเหตุ: launch share ที่ N=1 แค่ 34–40% (ไม่ใช่ >50%) — ที่เหลือคือ H2D latency + GPU kernel floor ~15 µs ซึ่งเป็น fixed cost จริงเช่นกัน; N50 (launch ≤50%) จึง trivially satisfied ตั้งแต่ N=1 ไม่ใช่เกณฑ์ที่มีความหมาย — เกณฑ์จริงคือ per-chunk vs floor

**JetBridge line:** 12 wants → 1 dispatch = 36.6 µs = **3.05 µs/want = 8.8–11.5× ถูกกว่า normal N=1** (ตรงกับ 11.99× จาก bench 2.2)

---

## 3. สรุป stack ที่พิสูจน์แล้ว

```
small batch want
  → geo_pipeline_build_index (warm ctx)      24 ns      [test_gpu_small_batch T7]
  → JetBridge บีบ 12 wants → 1 dispatch      ≥10:1      [T5: 120→11]
  → GPU: H2D 64 B + kernel                    16.5 µs    [gpu_launch_bench L3]
  → bridge dispatch รวม 12 wants              3.05 µs/want [break_even]
```

ทั้ง CPU-side และ GPU-side ไม่มีเพดาน fixed-cost ค้างที่ batch=1 — **small batch ใช้งานได้จริง**

## 4. เงื่อนไข / บทเรียน

1. **ห้าม `geo_pipeline_init` ต่อ call** — memset ~535 KB = 18.4 µs = fixed cost ทั้งหมด (778× เท่า warm path)
2. nvcc 12.6 ต้องผ่าน `vcvars64.bat` ของ VS 2022 BuildTools ก่อน (`cl.exe` ไม่อยู่ใน PATH):
   ```
   cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat\" >nul 2>&1 && I:\cuda_temp\bin\nvcc.exe -O2 -arch=sm_61 -Icore -Icore\infra -o build\<name>.exe bench\<name>.cu"
   ```
3. Test expectations ทั้งหมดมาจาก spec/math ที่เขียนใหม่ (128×162, bridge count=tick−1, pct/floor นิยามก่อนรัน) — ไม่อ่านจาก function ภายใต้การทดสอบ (rule #105)
4. ยังไม่ได้วัด: PCIe copy bandwidth แท้จริงของเครื่องนี้ (tail ns/chunk ที่ N>16384 ขึ้นเล็กน้อยจาก wrap contention บน field 20736 — ไม่ใช่ launch overhead)

## 5. GPU Bandwidth (วัดเพิ่มเติม — `bench/gpu_bandwidth_bench.cu`)

| Path | Best GB/s | หมายเหตุ |
|---|---|---|
| H2D pinned | **0.76** | latency-bound ≤64 KB (0.26–0.68), plateau จาก 4 MB |
| H2D pageable | 0.74 | |
| D2H pinned | 0.83 | |
| D2D (device→device) | **48.3** | theoretical VRAM 112.1 (128-bit × 3504 MHz DDR) |
| device write | **101.5** | ~90% theoretical |
| device read | 36.8 | test artifact — kernel มี atomicAdd, ไม่ใช่ pure read ceiling |
| in-kernel r+w copy | 60.2 | 2× traffic นับแล้ว |

**ข้อค้นพบสำคัญ — PCIe link รัน x1:**
```
nvidia-smi: gen current=3 max=3, width current=1  max=16  ← x1 เท่านั้น!
gen3 ×1 ≈ 0.99 GB/s theoretical → วัดได้ 0.76-0.83 = ~80% ของ x1 = ปกติของ x1
```
GPU รองรับ gen3 ×16 (≈12 GB/s) แต่ link ปัจจุบันคือ **×1** — 可能是: การ์ดไม่ได้ seat ในช่อง x16, riser/สล็อตจำกัด, หรือ BIOS lane config. ถ้าแก้เป็น ×16 จริง H2D จะขึ้น ~16× (0.76 → ~12 GB/s) และ break-even batch ของ normal path จะลดลงมาก เพราะ floor ~100 ns/chunk × 64 B ≈ 0.64 GB/s ≈ H2D ตอนนี้พอดี — **throughput floor ปัจจุบันคือ PCIe x1 path ไม่ใช่ VRAM**

สรุป stack ฝั่ง VRAM: write 101 GB/s / D2D 48 GB/s แรงพอ; คอขวดจริงของ pipeline ตอนนี้คือ H2D ข้างนอก

## 6. Files

- `tests/test_gpu_small_batch.c` — CPU-path overhead + coalesce proof (TIER1)
- `bench/gpu_launch_bench.cu` — CUDA launch latency + 12:1 on real GPU
- `bench/gpu_batch_break_even.cu` — normal-path break-even sweep vs JetBridge
- Memories: #6127 (CPU stack), #6130 (launch latency), #6132 (break-even)

## 7. T4 cross-machine (Colab Tesla T4 sm_75, 2026-09-24)

รันผ่าน `scripts/run_gpu_benches.sh sm_75` — **ALL_BENCH_OK** (3/3 benches), ทุก check PASS

| ตัววัด | GTX 1050 Ti (gen3 ×1) | Tesla T4 (Colab) | T4/1050Ti |
|---|---|---|---|
| L1 empty launch | 13.5 µs | **7.30 µs** | 1.85× เร็ว |
| L2 H2D 64 B | 8.9 µs | **6.70 µs** | 1.33× |
| L3 batch=1 full | 16.5 µs | **10.97 µs** | 1.50× |
| L4 naive 12-dispatch | 203.8 µs | 119.0 µs | 1.71× |
| L4 bridge 1-dispatch | 17.0 µs (11.99×) | **11.17 µs (10.66×)** | 1.52× |
| break-even ≤2× floor | N ≥ 512 | **N ≥ 4096** | floor ดีกว่า 11× (9 vs 100 ns) |
| break-even ≤1.5× floor | N ≥ 1024 | N ≥ 8192 | |
| launch share ≤10% | N ≥ 1024 | N ≥ 8192 | |
| per-chunk floor | ~100 ns | **9.0 ns** | 11× |
| H2D pinned best | 0.76 GB/s | **12.37 GB/s** | 16.3× (= gen3 ×16 เต็ม) |
| D2D | 48.3 GB/s | 120 GB/s | 2.5× |
| device write | 101.5 GB/s | 347 GB/s | 3.4× |

ข้อสังเกต:
- **JetBridge coalesce ratio คงเดิม ~11× ข้าม arch** (11.99× sm_61 / 10.66× sm_75) — เป็น property ของ schedule ไม่ใช่ hardware
- **T4 ต้องรอ batch ใหญ่กว่า** (4096 vs 512) ทั้งที่แรงกว่า — เพราะ floor ดีกว่า 11× (9 vs 100 ns/chunk) เกณฑ์ 2× floor จึงเข้มขึ้น; H2D ×16 เต็มช่วยให้ full path ถูกลงแต่ fixed cost launch ~7 µs ยังคงเป็นตัวกำหนดเกณฑ์
- break-even บน T4 ยืนยันการวิเคราะห์เดิม: **ไม่มี bridge = ต้องรอ batch ใหญ่; มี bridge = ยิงได้เลย** เหมือนกันทุกเครื่อง
- `scripts/run_gpu_benches.sh [arch]` — build fatbin (native SASS + PTX) + รันครบ 3 bench; default sm_75 (T4), ใส่ sm_61/sm_70/sm_80/sm_89 ตามเครื่อง
