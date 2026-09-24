# DWGLS × TPU — mapping + ผลวัดจริง (Colab TPU v5e)

สถานะ: **รันแล้ว** — `bench/tpu_jax_bench.py` บน Colab TPU v5 lite (v5e), jax 0.7.2, 2026-09-24 → 4 PASS / 0 FAIL / 1 SKIP (ดู §ผลวัดจริง ล่างสุด)

## ต่างจาก CUDA ตรงไหน (กรอบก่อน)

- TPU ไม่มี CUDA/Driver model — เข้าถึงผ่าน **PJRT/XLA** (JAX, PyTorch/XLA, TF)
- kernel-level เขียนด้วย **Mosaic/Pallas** (≈ CUDA ของ TPU) — เขียน HBM addressing/verification ได้จริง
- memory layout จัดการโดย **XLA compiler** ไม่ใช่โค้ดเรา — ไม่มี "mmap + pointer routing" แบบ GGUFBox
- hardware: systolic array + HBM (ไม่ใช่ VRAM/GDDR), Colab ฟรี = TPU v2-8/v3-8, Kaggle ~30 ชม./สัปดาห์

## Mapping: DWGLS asset → TPU

| DWGLS | บน TPU | ระดับ |
|---|---|---|
| **int-only O(1) addressing** (stride-37, DRamTile) | XLA `iota` + modular arith ตรงตัว — ทดสอบได้ว่า address คำนวณเร็ว/ช้าแค่ไหน vs memory-bound | ✅ ทำได้เลย |
| **JetBridge small-batch coalescing** (ตัวคำถามเดิม) | คู่เทียบ = XLA fusion + persistent compilation cache; sweep N เหมือน `gpu_batch_break_even` | ✅ core experiment |
| **GPU bandwidth/launch/overhead ชุดวัด** | Pallas microbench: HBM bw, dispatch/compile overhead, host↔device | ✅ เก็บ data เทียบ substrate |
| **memory tiering concept** (#970 Saturn-ring — substrate-independent) | fixed zones + sync-back ลองบน HBM: host RAM ↔ HBM tiering = โครงเดียวกับ GPU RAM↔VRAM | ✅ concept test ถูกที่สุด |
| **integrity layer** (CRC-64, multi-view XOR consensus, planet watch) | substrate-independent — host-side หรือ Pallas check kernel เท่ากัน | ✅ ย้ายได้ |
| **.tesspack น้ำหนัก/transport** | เก็บ weight shards แล้ว assemble เป็น TPU checkpoint (ไม่ใช่ GGUF) — ได้ integrity + windowed read เหมือนเดิม | ⚠️ ต้องเขียน adapter checkpoint format |
| **MoE expert addressing** (`moe_expert_addr`) | deterministic expert→HBM placement มีที่ยืน (SparseCore = sparse gather โดยกำเนิด) | ⚠️ speculative |
| **breathing scale/RELABEL** | หลักการ (interpretation function over static field) ไม่ผูก GPU — ใช้ได้ แต่ no practical driver ตอนนี้ | ⚠️ theory only |
| **KV dead-slot injection** (#231-234) | ❌ ไม่ได้ — XLA ครอบ KV layout เหมือน llama.cpp เองก็เป็น | ❌ |
| **GGUFBox zero-copy / Vulkan / llama.cpp** | ❌ stack คนละโลก | ❌ |

## รายการวัด (mirror GPU benches ชุดเดิม) — รันครบแล้ว 2026-09-24

1. **HBM bandwidth**: write/read/copy (คู่เทียบ 101/36/60 GB/s ของ 1050 Ti)
2. **host↔device bandwidth + latency**: `jax.device_put` size sweep (คู่เทียบ H2D 0.76 GB/s ×1)
3. **dispatch + compile overhead**: first-call (compile) vs steady dispatch (คู่เทียบ 13.5 µs launch)
4. **small-batch sweep**: N=1..65536 เหมือน `gpu_batch_break_even` — fixed cost ของ TPU อยู่ที่ compile ไม่ใช่ launch ต่างกันแค่ไหน
5. **persistent compilation cache on/off**: คู่เทียบ "init ห้ามทุก call" ของ GPU

สคริปต์ JAX ~50-100 บรรทัดพอสำหรับทั้ง 5 ข้อ

## บทสรุป mapping (ก่อนรัน — ยังคงสภาพ)

- **ทำได้จริงตั้งแต่แรก:** addressing-as-computation, overhead/bandwidth measurement, tiering concept, integrity layer
- **ไม่ได้:** pointer-routing/zero-copy, KV inject, โค้ด CUDA ทุกตัว
- **คุณค่าหลักตอนนี้ = เก็บ baseline ตัวเลข** — substrate ที่ 2 ทำให้เห็นว่า fixed-cost amortization เป็น universal หรือเป็นของ CUDA เฉยๆ (หลักการเดียวกับที่ #970 ทำ VRAM แล้ว map substrate อื่น)

คู่อ้างอิงผล GPU: `docs/GPU-SMALL-BATCH-OVERHEAD-2026-09-23.md`

## ผลวัดจริง — Colab TPU v5 lite (v5e), jax 0.7.2, 2026-09-24

`bench/tpu_jax_bench.py` → **4 PASS / 0 FAIL / 1 SKIP** (T5 cache = SKIP สาเหตุ libtpu single-holder, แก้ให้ B5 รันก่อน grab device แล้ว)

| วัด | TPU v5e | GTX 1050 Ti | TPU:GPU |
|---|---|---|---|
| device r+w best | **482 GB/s** | 101.5 GB/s | 4.8× |
| d2d copy | 473 GB/s | 48.3 GB/s | 9.8× |
| host→device best | 7.5 GB/s | 0.76 GB/s (PCIe ×1) | 10× |
| host→device small latency | **170–240 µs** | 8.9 µs (64 B) | 20–25× แย่กว่า |
| batch=1 synced completion | **153 µs** | 16.5 µs | 9.3× แย่กว่า |
| enqueue async | 48 µs | 13.5 µs launch | 3.6× |
| fixed cost/call (steady) | ~200–240 µs flat N=1..65536 | ~16.5 µs | 12–15× |
| fixed-cost ตั้งอยู่ที่ | **compile 23.9 ms** (one-time) | launch (ทุก call) | คนละจุด |
| per-chunk floor | **0.3 ns** @1M elems | ~100 ns | compute ดีกว่ามากเมื่อ amortize |
| per-chunk ≤2× floor | N ≥ 1M | N ≥ 512 | floor คนละ nature (TPU=compute, GPU=mem) |

ข้อสังเกต:
1. **สมมติฐานยืนยันบน substrate ที่ 2** (#6144): fixed-cost amortization universal — TPU ลงโทษ small batch หนักกว่า GPU ~10× (153 vs 16.5 µs ต่อ call) แปลว่า coalescing/persistent-cache มีค่า**ยิ่งกว่า**บน TPU ไม่ใช่น้อยลง
2. TPU รวย throughput แต่ต้องรอ batch ใหญ่: HBM 482 GB/s + 0.3 ns/chunk หลัง amortize — คู่ขนานกับ "ไม่มี bridge ต้องรอ 512–1024" ของ GPU (แต่ floor ต่างกัน: TPU ชน compute, GPU ชน H2D×1)
3. B2 device_get วัดได้ 1552 GB/s flat 10 µs = **artifact สงสัย** (เกิน phys ได้) — อย่าอ้างตัวเลขนี้
4. compile% ≤10% ไม่เจอใน sweep เพราะเทียบ compile one-time กับ call เดียว — ความจริง compile ค่าเดียว 23.9 ms แล้ว steady 204 µs/call ต้องนับ amortized หลาย call
