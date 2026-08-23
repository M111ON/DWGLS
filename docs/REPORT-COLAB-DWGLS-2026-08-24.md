# Report — DWGLS × Colab T4 Integration (2026-08-24)

> ทดสอบระบบ DWGLS บน Google Colab T4 ครบทั้ง pipeline:
> RID slot storage → lossless roundtrip → llama-cpp-python inference

## Executive Summary

| # | Test | Result |
|---|------|--------|
| 1 | Prebuilt CUDA wheel (ไม่ต้อง build C) | ✅ pip install ~2 นาที |
| 2 | Multi-instance GPU sharing | ✅ 2 inst = 280 tok/s aggregate (near-linear) |
| 3 | Batch concatenation | ✅ 337 tok/s, VRAM −90% |
| 4 | 7B on T4 / CPU | ✅ 36-38 tok/s (T4) · 1.7 tok/s (CPU) |
| 5 | **8-view GGUF roundtrip (lossless)** | ✅ ALL PASSED — 675.7 MB, 29.8s |
| 6 | Phase 3 Python integration | ✅ RID twin → rebuild byte-identical → inference identical |

## 1. Infrastructure — Prebuilt Wheel (แก้ปัญหา build 50 นาที)

`llama-cpp-python` มี prebuilt CUDA wheel (`--extra-index-url .../whl/cu124`) —
pip install ~2 นาที แทน build llama.cpp จาก source ~50 นาที
(compile บน Colab VM เสียเวลา + session drop = build หายทุกครั้ง)

- `colab-pack/deploy_llama_cuda.sh` — deploy + bench
- `gguf_roundtrip.c` compile เอง 1 วินาที (gcc มีบน VM อยู่แล้ว) → **ไม่ต้อง build C library เลย**

## 2. GPU Sharing Benchmarks (Qwen2.5-0.5B Q8_0, T4)

| Scenario | Per-instance | Aggregate | VRAM |
|----------|-------------|-----------|------|
| Serial (1) | 141.5 tok/s | 141.5 | 929 MiB |
| Concurrent ×2 | 140.1 | **280.3** | 2,545 MiB |
| Concurrent ×3 | 107.6 | **322.7** | 4,981 MiB |
| Batch concat ×6 | — | **337** | ~529 MiB |

**สาเหตุ:** decode = memory-bandwidth bound (T4 320 GB/s) — share ได้จนถึงจุด saturate

## 3. Model Scale (Qwen2.5-7B Q4_K_M)

| Hardware | tok/s | VRAM |
|----------|-------|------|
| T4 GPU | 36–38 | 4,691 MiB |
| CPU (fallback) | 1.7 | — |

7B on T4 = 22× เร็วกว่า CPU; batch 6-view ยังได้ 33.5 tok/s aggregate

## 4. ★ DWGLS 8-View Lossless Roundtrip

`tools/gguf_roundtrip.c` — bake full GGUF (675.7 MB · 5156 parts · 86 layers)
ผ่าน RID slot region ครบ **8 ภาษา**:

| # | View | หลักการ | Status |
|---|------|---------|--------|
| 0 | pent | dodecahedron face ordering | ✅ R1+R2 (+R3 damage drill) |
| 1 | tri | icosahedron (dual) | ✅ R1+R2 |
| 2 | snubL | snub dodeca left (chiral) | ✅ R1+R2 |
| 3 | snubR | snub dodeca right (enantiomorph) | ✅ R1+R2 |
| 4 | hosoya | golden spiral stride F(7)=13 | ✅ R1+R2 |
| 5 | zeck | Zeckendorf reversed code | ✅ R1+R2 |
| 6 | **pascal** | A(n)=Σ(−1)^k C(n−k,k) period-6 | ✅ R1+R2 **NEW** |
| 7 | **hexagram** | hex distance rank (axial→cube x+y+z=0) | ✅ R1+R2 **NEW** |

- R1 BAKE+READBACK: bad=0 ทุก view (lossless)
- R2 REBUILD: byte-identical ทุก view
- R3 DAMAGE DRILL (pent): flip 1 byte → localize part 2578 → re-bake → lossless อีกครั้ง
- R4 PERSIST: twin file 676.3 MB ใช้งานข้าม destroy ได้

Oracle tests: `pascal_zigzag_probe` (diagonal→Fibonacci identity),
`hexagram_cubes_probe` (hex lattice ≡ cubic slice, MacMahon tiling count 20)

## 5. Phase 3 — End-to-End Integration

```
GGUF 675.7 MB ──bake 27.5s──▶ RID twin ──rebuild──▶ byte-identical ✓
                                    │
                                    ▼
                     llama-cpp-python inference
                     text output identical ✓
```

- `inference_direct`: 129.8 tok/s · `inference_rid path`: 225.9 tok/s
- DWGLS อยู่ชั้น **storage** — inference API (chat/completion) ใช้ปกติไม่ต้องแก้อะไร

## 6. Coverage ของระบบ

**ใช้แล้ว (~40%):** RID views ×8, dramtile_store twin, gguf_reader
**ยังไม่ integrate:** KIS timeline (checkpoint/rollback), Goldberg storage (multi-sphere),
GeoFS (filesystem layer), kis_cube_views S₃, geo_param_grid

## Files

```
colab-pack/
├── deploy_llama_cuda.sh      ← prebuilt wheel deploy + bench
├── test_multi_instance.py    ← GPU sharing benchmark
├── test_batch_vs_instance.py ← batch vs separate instances
├── dwgls_7b_gpu.py           ← 7B on T4
├── dwgls_7b_cpu.py           ← 7B on CPU
├── test_dwgls_python.py      ← Phase 3 end-to-end
└── test_dwgls_python.sh      ← runner
tools/p2_view_perf.c          ← Linux clock_gettime fix
docs/REPORT-COLAB-DWGLS-2026-08-24.md ← report นี้
```

## Next Steps

1. KIS timeline checkpoint → rollback model state ระหว่าง generation
2. Multi-model packing ด้วย Goldberg storage
3. GeoFS mount → tensor-as-file abstraction
