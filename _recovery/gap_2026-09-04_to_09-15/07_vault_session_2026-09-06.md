# Session: 2026-09-06 — DWGLS LFM 8B Tesspack Pipeline

## What Was Done
- Built + ran all DWGLS wire/GPU/bench tools on LFM 8B A1B Q4_K_M.gguf (4.80 GB, 256 tensors, MoE 12/64)
- Full tesspack pipeline: tess-bake → tess-pack → tess-assemble → lossless verify
- 12 new benchmark executables built and run

## Key Results
| Metric | Result |
|--------|--------|
| Tesspack overhead | 5.4% (budget 50%) |
| KV decode MAP vs CLASSIC | **89.4x faster** (6.69 vs 0.07 GB/s) |
| Tiny random-access MAP | **24x faster** (zero-copy) |
| RDH vs FNV-1a | **11.6x faster** (5.11 vs 70.7 cycles) |
| Lossless verify | MD5 match: F57DEF02E4E034D4F16FFA125977C45A |

## Files
- F:\model\lfm8b.tesspack (5.06 GB)
- docs/REPORT-lfm8b-tesspack-2026-09-06.md

## Next Steps
1. Bridge layer: .tesspack → ggml tensor on-demand decode
2. mmap loader (header only, decode on access)
3. GPU scatter kernel (CUDA/Vulkan)
4. llama-server integration

## State
Model files at F:\model\, pipeline at I:\DWGLS-native-fs\, all bench tools in build/

## Session ended 2026-09-06 15:09:20
**Project:** DWGLS-native-fs

DWGLS: Multi-format tesspack lossless (4/4 models). Q1_0 type 41 added to gguf_reader.h + 4 tools. tess_scatter_bench_v2.cu written DRamTile+GearLock+sig32, Colab GPU 503. Next: general tesspack-to-GGUF assembler for inference.
